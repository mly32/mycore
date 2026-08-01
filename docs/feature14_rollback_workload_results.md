# Feature 14 Rollback Workload Results

## Decision

Feature 14 keeps atomic same-frame replay as its only production rollback path. The measured
target 200 ms workload remained below the 4 ms p99 research threshold at every entity count, and
rollback work alone exceeded a 30 Hz frame budget in 0% of measured reconciliations. The
conditional `spike/multi-frame-resimulation` is therefore not activated.

This is a decision about the current Dots workload and rollback model, not a permanent claim that
all future games fit in one frame. Re-run the workload after material checkpoint, closure, or
simulation-cost changes.

## Deterministic workload

The `dots_rollback_workload` executable builds a complete `FullReplicated` Dots checkpoint,
predicts a suffix containing one split, advances authority through the first command, and times
the atomic reconciliation/replay of the retained suffix. Each measured reconciliation starts
from an identical prepared timeline copy. The executable verifies:

- the requested replay-frame count;
- the final predicted checkpoint digest;
- one retained split topology event; and
- the percentage of timed reconciliations exceeding `33.333 ms`.

The entity matrix is 10, 100, 500, and 1,000 replicated entities. RTT-equivalent cases use 3, 6,
and 12 replay ticks for 100, 200, and 400 ms respectively. Approximate checkpoint storage is
reported from value objects and their element storage; it is diagnostic rather than an allocator
measurement.

Measurements below were recorded on 2026-07-29 from commit `da50f3f`, using the
`macos-clang-release` preset on Darwin arm64. Each row contains 1,000 measured reconciliations
after three warmups. Times are milliseconds.

| Entities | RTT | Replay ticks | Checkpoint bytes | Topology events | p50 | p95 | p99 | Max | Over 33.333 ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10 | 100 ms | 3 | 368 | 1 | 0.014 | 0.038 | 0.061 | 0.291 | 0% |
| 10 | 200 ms | 6 | 368 | 1 | 0.022 | 0.029 | 0.064 | 0.150 | 0% |
| 10 | 400 ms | 12 | 368 | 1 | 0.036 | 0.044 | 0.044 | 0.047 | 0% |
| 100 | 100 ms | 3 | 1,448 | 1 | 0.060 | 0.063 | 0.066 | 0.098 | 0% |
| 100 | 200 ms | 6 | 1,448 | 1 | 0.083 | 0.127 | 0.191 | 0.237 | 0% |
| 100 | 400 ms | 12 | 1,448 | 1 | 0.139 | 0.334 | 0.566 | 0.940 | 0% |
| 500 | 100 ms | 3 | 6,248 | 1 | 0.265 | 0.311 | 0.404 | 0.740 | 0% |
| 500 | 200 ms | 6 | 6,248 | 1 | 0.367 | 0.403 | 0.421 | 0.884 | 0% |
| 500 | 400 ms | 12 | 6,248 | 1 | 0.538 | 0.583 | 0.601 | 0.632 | 0% |
| 1,000 | 100 ms | 3 | 12,248 | 1 | 0.511 | 0.554 | 0.594 | 0.972 | 0% |
| 1,000 | 200 ms | 6 | 12,248 | 1 | 0.681 | 0.720 | 0.738 | 0.785 | 0% |
| 1,000 | 400 ms | 12 | 12,248 | 1 | 1.030 | 1.134 | 1.250 | 1.588 | 0% |

Reproduce the matrix with:

```bash
cmake --preset macos-clang-release
cmake --build --preset macos-clang-release --target dots_rollback_workload
./build/macos-clang-release/bin/dots_rollback_workload --iterations 1000
```

The ordinary test suite runs one measured iteration per row as a deterministic correctness and
schema check. It deliberately does not assert wall-clock thresholds.

## Native impairment soaks

The session launcher now accepts zero graphical clients when at least one bot is present and
supports `--duration-seconds`. A bounded run succeeds only if the server and all participants
remain healthy through the deadline; any earlier server exit or nonzero client/bot exit still
fails immediately.

The following Debug-preset, five-bot native sessions passed on 2026-07-29:

| Nominal RTT | Loss per outgoing endpoint | Duration | Result |
|---:|---:|---:|---|
| 10 ms | 10% | 30 s | Passed |
| 200 ms | 5% | 20 s | Passed |
| 400 ms | 5% | 20 s | Passed |

Nominal RTT is twice `--fake-lag-ms` because the launcher applies the same one-way outgoing lag
to server and clients. The runs exercised real cross-process GameNetworkingSockets transport,
joins, unreliable input/snapshot traffic, reconciliation, player absorption, and spectating.
They produced no timeline failure, invalid acknowledgement, pending-input queue overflow, or
unexpected process exit. Debug-build 2 ms replay warnings remain budget observations rather than
correctness failures.

Use this target 200 ms soak command after rollback/network changes:

```bash
python3 games/dots/tools/dots_session.py \
    --build-dir build/macos-clang-debug \
    --clients 0 \
    --bots 5 \
    --fake-lag-ms 100 \
    --fake-loss-percent 5 \
    --duration-seconds 30
```

These bot-only soaks establish native transport and rollback stability. The deterministic
workload's `over_33ms_percent` column is the isolated rollback-attributable frame-budget measure;
it does not include rendering or unrelated application work.
