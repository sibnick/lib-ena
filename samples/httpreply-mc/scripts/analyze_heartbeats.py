#!/usr/bin/env python3
"""
Turn httpreply-mc heartbeat lines into per-core rates.

The heartbeat prints cumulative counters. This script differences two
consecutive heartbeats for the same core and divides by the heartbeat
interval. Read it from the NETLOG ring capture (GET /__log, which
run_ec2_lowconc_probe.py saves with --stats) or from a serial console
capture. Records are matched anywhere in the input text, because the
console and the ring can join several records onto one line.

The interval is not in the log. The worker loop in main.c uses a
fixed 2 s interval, so the default here is 2.0 s. Pass --interval if
the build uses another value.

What to look for:

- polls/s is the worker loop rate. An idle core that reaches the
  dormant backoff regime polls a few hundred times per second. A core
  that holds connections busy-polls, and the value goes up by three
  orders of magnitude. A core with connections and a low polls/s
  means the sleep path is active.
- active is the count of open TCP connections on that core. Compare
  it across cores to see how the client connections split.
- rx and tx are per-queue packet counts. Their deltas follow the
  active split.
- pbuf, pcb and seg are free object counts, not counters. The
  per-core pools shrink as the core count grows.

The NETLOG ring returns only its tail, so the capture covers the last
part of a run, not the whole run.

Usage:
  python3 scripts/analyze_heartbeats.py FILE [FILE ...]
      [--interval 2.0] [--cores N] [--summary-only]
"""

import argparse
import re
import sys

# One record, matched anywhere in the input text.
RECORD = re.compile(
    r"core\s+(?P<core>\d+)\s+heartbeat\s+\("
    r"polls=(?P<polls>\d+),\s*rx=(?P<rx>\d+),\s*tx=(?P<tx>\d+),"
    r"\s*active=(?P<active>\d+),\s*rxpost=(?P<rxpost>\d+)"
    r"\s*refill=(?P<refill>\d+)\s*rxdrop=(?P<rxdrop>\d+),"
    r"\s*aen=(?P<aen>\d+)\s*rxdrop=(?P<aen_rx>\d+)\s*txdrop=(?P<aen_tx>\d+),"
    r"\s*free:\s*pbuf=(?P<pbuf>\d+)\s*pcb=(?P<pcb>\d+)\s*seg=(?P<seg>\d+)"
)

FIELDS = (
    "polls",
    "rx",
    "tx",
    "active",
    "rxpost",
    "refill",
    "rxdrop",
    "aen",
    "aen_rx",
    "aen_tx",
    "pbuf",
    "pcb",
    "seg",
)

# These are free object counts and open connection counts, not
# counters. Report the current value, not the difference.
GAUGES = ("active", "pbuf", "pcb", "seg", "rxpost")


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="Difference heartbeat counters.")
    p.add_argument("files", nargs="+", help="netlog or console capture files")
    p.add_argument(
        "--interval",
        type=float,
        default=2.0,
        help="heartbeat interval in seconds (default: 2.0)",
    )
    p.add_argument(
        "--cores",
        type=int,
        default=0,
        help="warn if fewer than this many cores appear",
    )
    p.add_argument(
        "--summary-only",
        action="store_true",
        help="print only the per-core summary",
    )
    return p.parse_args(argv)


def read_records(path):
    """Return the heartbeat records in file order."""
    try:
        text = open(path, "rb").read().decode("utf-8", "replace")
    except OSError as exc:
        print(f"[WARN] cannot read {path}: {exc}", file=sys.stderr)
        return []
    out = []
    for m in RECORD.finditer(text):
        rec = {k: int(m.group(k)) for k in FIELDS}
        rec["core"] = int(m.group("core"))
        out.append(rec)
    return out


def deltas(series, interval):
    """Difference consecutive records in one core series."""
    rows = []
    for prev, cur in zip(series, series[1:]):
        row = {}
        skip = False
        for f in FIELDS:
            d = cur[f] - prev[f]
            # A negative delta means the counters restarted. Skip it.
            if d < 0:
                skip = True
                break
            row[f] = d
        if skip:
            continue
        row["polls_s"] = row["polls"] / interval
        row["rx_s"] = row["rx"] / interval
        row["tx_s"] = row["tx"] / interval
        for g in GAUGES:
            row[g] = cur[g]
        rows.append(row)
    return rows


def print_intervals(core, rows):
    print(f"core {core}: {len(rows)} intervals")
    if not rows:
        print("  no pair of heartbeats to difference")
        return
    print("   idx   polls/s      rx/s      tx/s  rxdrop   active  pbuf pcb   seg")
    for i, r in enumerate(rows):
        print(
            f"  {i:4d} {r['polls_s']:10.0f} {r['rx_s']:10.0f} "
            f"{r['tx_s']:10.0f} {r['rxdrop']:7d} {r['active']:7d} "
            f"{r['pbuf']:5d} {r['pcb']:4d} {r['seg']:5d}"
        )


def print_summary(by_core):
    print("\nsummary per core")
    print(
        " core  intervals  polls/s min/max    rx/s min/max   "
        "active max   rxdrop  pbuf min"
    )
    for core in sorted(by_core):
        rows = by_core[core]
        if not rows:
            print(f" {core:4d} {0:9d}   no data")
            continue
        pmin = min(r["polls_s"] for r in rows)
        pmax = max(r["polls_s"] for r in rows)
        rmin = min(r["rx_s"] for r in rows)
        rmax = max(r["rx_s"] for r in rows)
        amax = max(r["active"] for r in rows)
        rxdrop = sum(r["rxdrop"] for r in rows)
        pbuf_min = min(r["pbuf"] for r in rows)
        print(
            f" {core:4d} {len(rows):9d}   {pmin:8.0f}/{pmax:<8.0f}   "
            f"{rmin:8.0f}/{rmax:<8.0f}  {amax:9d}  {rxdrop:7d}  "
            f"{pbuf_min:8d}"
        )


def main():
    args = parse_args()
    all_records = []
    for path in args.files:
        recs = read_records(path)
        print(f"[read] {path}: {len(recs)} heartbeat records")
        all_records.extend(recs)
    if not all_records:
        print("no heartbeat records found")
        return 1

    by_core = {}
    for core in sorted({r["core"] for r in all_records}):
        series = [r for r in all_records if r["core"] == core]
        by_core[core] = deltas(series, args.interval)

    if args.cores and len(by_core) < args.cores:
        print(f"[WARN] only {len(by_core)} cores seen, expected {args.cores}")

    if not args.summary_only:
        print()
        for core in sorted(by_core):
            print_intervals(core, by_core[core])
            print()

    print_summary(by_core)
    return 0


if __name__ == "__main__":
    sys.exit(main())
