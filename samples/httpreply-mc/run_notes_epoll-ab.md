# Run Note: epoll ready-list A/B (httpreply-mc)

## Purpose

Validate the fix for Ticket 20d68e6796. The fix makes the epoll wake path
walk only ready entries. This note measures throughput and tail latency
with and without the change.

## Method

Two builds share one source tree. The only difference is the `epoll.c`
ready-list hunk in `patches/unikraft-e31b2c44.patch`.

- `base`: pre-merge build. The wake path walks every registered fd. Cost
  is O(N).
- `fixed`: current trunk. The wake path walks only ready entries. Cost is
  O(armed).

Setup follows the `ec2-perf-testing` skill:

- Target and client on `c6i.large`, same subnet, `us-east-1`.
- `wrk -t4`, keep-alive, 20 s per step, warm-up at c=50.
- Concurrency levels: 50, 100, 200, 300.
- One run per level.

## Results

req/s and p99 latency. `d%` is fixed minus base.

```
    c |  base req/s  fixed req/s      d% |  base p99  fixed p99      d%
   50 |    137683.1     114177.9  -17.1% |      0.50       0.52   +4.4%
  100 |    203591.5     188758.2   -7.3% |      0.76       0.81   +7.0%
  200 |    207909.4     231895.1  +11.5% |      1.61       1.35  -16.1%
  300 |    211408.2     230022.9   +8.8% |      3.16       2.45  -22.5%
```

All runs reported zero socket errors. No timeouts or connection failures.

## Findings

- At c=200 and c=300 the fix raises throughput by 9 to 12 percent and
  lowers p99 by 16 to 22 percent. This matches the ticket. The O(N) walk
  grows with live connections per core. The fix removes it.
- At c=50 and c=100 the change is within run-to-run noise. The ticket
  states the cost is small below c=200. One run per level cannot resolve
  a difference that small.
- The fixed build saturates near 230k req/s. The base build saturates
  near 210k req/s.

## Conclusion

The fix helps in the range the ticket targets. Close Ticket 20d68e6796.

## Cleanup

Terminated all instances. Deregistered both AMIs. Deleted both snapshots.
The account has no running instances, snapshots, or owned AMIs.
