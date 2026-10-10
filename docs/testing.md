# Host Test Suite

The host test suite runs the driver on a normal machine. It needs no Unikraft
runtime and no ENA hardware. A mock device answers admin commands and fills
completion rings, so the control path and the data path run for real.

Every test file carries the BSD-3-Clause header that
[docs/conventions.md](conventions.md) section 3 requires. `make license-check`
enforces it.

---

## 1. Running the suite

```bash
make clean && make test    # build and run all 14 binaries
make sanitize              # the same tests under ASan and UBSan at -O0
./build/test_admin         # one binary on its own
```

The tests compile with `-std=c99 -Wall -Wextra -Werror -pedantic`. They include
`-Iinclude -Ireference -Itests`. The build does not need `-Ireference`; the
driver's own headers in `include/` are used.

The host job in `.github/workflows/ci.yaml` runs four gates in this order:
`make format-check`, `make license-check`, `make test`, `make sanitize`. A pull
request must pass all four.

---

## 2. What each binary covers

`make test` builds and runs these programs. The Makefile defines each one at
lines 8 to 21 and builds it with the rules at lines 75 to 135.

| Binary | Source file | Covers |
| :-- | :-- | :-- |
| `test_runner` | `tests/test_pci_scaffold.c` | Phase 1: PCI id matching, BAR sizing, device status and reset polling |
| `test_admin` | `tests/test_admin_queue.c` | Phase 2: admin command round trip, command id wrap, timeout and reset recovery, AENQ dispatch, fatal-error recovery, admin lock behaviour |
| `test_init` | `tests/test_init.c` | Phase 3: device attributes, MTU set, host info, AENQ group configuration, LLQ negotiation |
| `test_datapath` | `tests/test_datapath_rings.c` | Phase 4: ring allocation, hardware create and destroy, index and phase handling |
| `test_tx` | `tests/test_tx_datapath.c` | Phase 5: transmit submit, doorbell, completion processing, request-id pool |
| `test_rx` | `tests/test_rx_datapath.c` | Phase 6: receive refill, poll, multi-descriptor chains |
| `test_netdev` | `tests/test_netdev.c` | Phase 7: netdev operations, queue lifecycle, feature gating |
| `test_intr` | `tests/test_intr.c` | Phase 8: MSI-X table setup, mask and unmask registers, coalescing, poll step engine |
| `test_llq` | `tests/test_llq.c` | Phase 9: low-latency queue push path and capacity checks |
| `test_validation` | `tests/test_validation.c` | End-to-end control and data path checks over a full bring-up |
| `test_spsc` | `tests/test_spsc.c` | The lock-free SPSC ring used by `samples/httpreply-mc` |
| `test_idlebackoff` | `tests/test_idlebackoff.c` | The idle backoff policy used by the same sample |
| `test_rss_skew` | `tests/test_rss_skew.c` | Simulation of an RSS queue split, from ticket `ca72834ec7` |
| `test_tx_guard` | `tests/test_tx_guard.c` | Micro-benchmark of the cost of the TX cross-CPU guard |

Most binaries print a banner per case and a summary line such as
`ALL PHASE 7 NETDEV TESTS PASSED (25/25)`. Some older binaries print only
`[TEST]` and `[PASS]` lines. A run is good when every binary exits 0 and the
`test` target stops with no error.

---

## 3. The mock device

`tests/mock_pci.h` and `tests/mock_pci.c` emulate an ENA device: a 4 KiB BAR0
register bank, the device side of the AQ, ACQ and AENQ rings, queue creation,
and feature negotiation.

Start every test that talks to the device like this:

```c
struct mock_ena_hw hw;
mock_ena_hw_init(&hw);
ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

/* Only for paths that wait for a device reset: */
ena_device_set_reset_poll_hook(mock_ena_hw_reset_poll_hook, &hw);
```

### Fault and behaviour controls

| Call | Effect |
| :-- | :-- |
| `mock_ena_hw_hang_admin` / `mock_ena_hw_clear_admin_hang` | Stop or resume completion of admin commands. A finished reset also clears the hang. |
| `mock_ena_hw_set_admin_status` | Return a device error status for the next command. |
| `mock_ena_hw_inject_bad_cmd_id` / `..._clear_bad_cmd_id` | Complete with a command id that does not match the request. |
| `mock_ena_hw_inject_bad_db_offset` / `..._clear_bad_db_offset` | Return an out-of-range doorbell offset from a create command. |
| `mock_ena_hw_inject_fake_req_id` / `..._clear_fake_req_id` | Complete with a request id the driver never posted. |
| `mock_ena_hw_inject_aenq`, `mock_ena_hw_inject_aenq_payload` | Post an asynchronous event, for example a fatal error or a link change. |
| `mock_ena_hw_require_attrs_first` | Reject feature sets until the driver reads device attributes. |
| `hw.reset_polls_to_finish = N` | Finish a device reset after N polls of the reset status register. A value of 0 never finishes it. |
| `mock_pci_inject_fault`, `mock_pci_clear_faults` | Apply the fault enum in `enum mock_pci_fault_type` in one call. |

### Records you assert on

The struct keeps counters that let a test check what the driver actually did:
`sq_created_count`, `cq_created_count`, `sq_destroyed_count`,
`cq_destroyed_count`, `attrs_read`, `negotiated_mtu`, `aenq_set_count`,
`rss_set_key_count`, `rss_set_ind_count`, `reset_polls`, and `last_opcode`.

### Host-only hooks in the driver

These exist so a test can observe driver behaviour without real hardware. They
are compiled only for host builds:

| Hook | Purpose |
| :-- | :-- |
| `ena_admin_set_db_hook` | See every admin doorbell index the driver writes. |
| `ena_device_set_reset_poll_hook` | Count or control the polls of the reset status register. |
| `ena_plat_set_mock_msix_vectors` | Report a given MSI-X vector count to the driver. |
| `ena_plat_set_mock_cpu_id` | Pin the reported CPU id, to test per-core ownership. |
| `ena_plat_set_mock_dma_alloc_fail` | Make the Nth DMA allocation fail. |

---

## 4. Choosing a link set

Each binary links only the driver files it exercises. The Makefile defines the
sets as `ENA_SRCS`, `ENA_SRCS_P2` through `ENA_SRCS_P9`, and `ENA_SRCS_ALL` at
lines 23 to 36.

Two rules:

1. Reuse the set of the nearest existing test. Do not link every source file
   into a test that needs three.
2. A set that takes `src/ena_admin.c` must also take the ring create and RX
   refill code, because reset recovery re-creates IO queues. A set that takes
   `src/ena_init.c` must also take `src/ena_rss.c`, because feature re-apply
   restores the RSS table.

---

## 5. Adding a test

1. Create `tests/test_<area>.c`. Start with the BSD-3-Clause header from
   `docs/conventions.md` section 3.
2. Add a `TESTn = $(BUILD)/test_<area>` line near Makefile lines 8 to 21.
3. Add a build rule that compiles your file, `tests/mock_pci.c`, and the chosen
   link set. Add `-pthread` if the test uses threads.
4. Add the new variable to the prerequisites and the recipe of the `test`
   target. Put it in phase order.
5. Write one function per case. Print `[TEST] Running <name>...` before the
   case and `[PASS] <name> passed` after it. Call them from `main()` in the
   order they must run, then print a summary line.
6. Use `<assert.h>` for checks. `tests/test_framework.h` adds setup and
   teardown hooks and allocation tracking through `test_track_alloc` and
   `test_free`. Only `test_netdev.c` and `test_validation.c` use it today.
   Use it when a test must prove that nothing leaks.
7. Keep host-only code, such as threads and POSIX headers, inside an
   `#ifndef __Unikraft__` guard.
8. Name the defect you cover in a comment above the case, in the form
   `[Ticket <10-char-UUID>]`, and add the same reference to your commit
   message.
9. Run `make format`, `make license-check`, and `make test` before you commit.

### Make a stuck path fail fast

A test that waits on a driver bug can hang forever, and CI then times out with
no useful report. Run the call in a worker thread and wait a bounded time
instead. `test_admin_timeout_recovers_after_reset` in
`tests/test_admin_queue.c` shows the pattern: create a thread, poll a `done`
flag for a fixed period, assert the flag is set, then join.

---

## 6. What the host suite does not cover

The host suite never touches real ENA hardware. Device behaviour that only
appears on EC2, such as LLQ push after idle or a real AENQ keep-alive stream,
needs an instance. Use the scripts under `samples/httpreply-mc/scripts/` and
the steps in [docs/ec2_deployment.md](ec2_deployment.md). For a quick check of
bring-up after a driver change, build the sample image and run
`samples/httpreply-mc/scripts/run_ec2_verification.py --smoke`. It boots the
target, fetches three pages, saves the console output, and tears everything
down. Store the console logs and CSV results with `fossil uv add`, not in a
commit.
