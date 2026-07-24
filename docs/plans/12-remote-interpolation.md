# Feature 12: Remote Interpolation

## Goal

Render non-owned entities smoothly from known authoritative snapshots under latency, jitter,
loss, and independent client/server clocks. Keep the owned player on Feature 11 prediction and
never guess remote input.

Feature 12 owns the historical presentation buffer and its delayed render timeline. It does not
change server authority, local prediction, the local input-send clock, or replicated gameplay
state.

Feature 14 may later predict remote entities from explicitly recorded held-input assumptions.
This feature still establishes the known-endpoint interpolation fallback, comparison view, and
correctness baseline described by
[`../rollback_prediction_design.md`](../rollback_prediction_design.md); it never silently guesses
remote actions.

The canonical state, time, compensation, and scale vocabulary is in
[`../networked_prediction_reference.md`](../networked_prediction_reference.md). Feature 12 uses
interpolation-and-hold only: raw remote extrapolation is deliberately unscheduled and is not a
fallback for an underrun.

## Ownership Boundary

Keep source state separate instead of maintaining one ambiguous client "current world":

- `Dots::ClientRuntime` owns the latest replicated snapshot and owned-player input prediction.
- A renderer-free `Dots::RemotePresentation` target owns accepted remote snapshot history, its
  delayed cursor, and immutable remote presentation frames.
- `Dots::Presentation` owns local visual smoothing and composes local and remote frames for
  rendering/debugging.
- `Dots::Prediction` remains deferred to Feature 14 for complete speculative World replay.

`Dots::RemotePresentation` accepts semantic snapshots (snapshot ID, server tick, sorted entity
states, arrival time), not transport messages or `ClientRuntime`. The Feature 12 client app
adapts accepted full snapshots into that value. Feature 16 must supply the same coherent semantic
form after delta reconstruction rather than route raw delta records into presentation history.

## Feature 11 Boundary

Feature 11 provides:

- The latest validated replicated world.
- The controlled entity ID and server-assigned client ID.
- Owned-player predicted and presentation positions.
- Snapshot arrival-rate/age metrics.

Feature 12 adds:

- A history of accepted snapshots for presentation.
- A fractional remote presentation cursor.
- Remote entity interpolation and delayed lifecycle.
- Jitter, buffer, hold, rate-correction, and hard-rebase metrics.
- Known-endpoint versus interpolated world-space debugging.

Local correction smoothing remains Feature 11. Active local clock synchronization remains
deferred. Feature 12 adjusts only its delayed remote presentation cursor.

## Accepted Snapshot Stream

`Dots::ClientRuntime` must expose every accepted snapshot from a poll so the client cannot lose
intermediate presentation samples when multiple packets arrive in one frame.

Implemented result types:

```text
AcceptedSnapshot
    validated FullSnapshot value
    local steady-clock arrival time

ProcessEventsResult
    optional RuntimeError
    accepted snapshots in delivery order
```

All current call sites continue to process the runtime error. The graphical client additionally
forwards accepted snapshots into the presentation buffer. Only snapshots already accepted by
replication may enter presentation history.

## Presentation Snapshot Buffer

Add a Dots-owned `presentation::RemoteSnapshotBuffer` with a fixed capacity of 32 samples,
covering roughly 2.13 seconds at the current 15 Hz snapshot rate.

Each stored sample contains:

- Snapshot ID and authoritative server tick.
- Local arrival time.
- Complete entity states sorted by protocol entity ID.

Reject stale samples before insertion. Keep the newest 32 samples; if an arrival would exceed
capacity, discard the oldest history entry and let the normal bounded rebase recover a cursor
whose lower bracket was evicted. Keep capacity, ordering, and memory independent of transport
behavior.

The buffer excludes no state when storing a snapshot, but sampling treats the controlled player
separately: its drawn position always comes from Feature 11 presentation.

## Remote Presentation Timeline

The server advances at 30 Hz and currently emits a full snapshot every two server ticks. Use a
fixed target delay of three snapshot intervals:

```text
target delay = 6 server ticks = 200 ms
```

Use server ticks, not arrival timestamps, as interpolation coordinates.

Name this coordinate **remote presentation tick/time** in code, metrics, and documentation. It is
neither estimated live server time nor a general world clock: it intentionally describes the
delayed remote scene on one client. Snapshot ID remains an ordering value and must never be used
as elapsed time.

Startup behavior:

1. Store the first sample and hold its remote presentation.
2. Continue buffering until the stored tick span reaches six ticks.
3. Initialize the fractional presentation cursor to `newest_server_tick - 6`.
4. Begin normal advancement without rewinding already advanced presentation.

During normal rendering, advance the fractional cursor by local elapsed time expressed in 30 Hz
server ticks. The desired cursor is always `newest_server_tick - 6`.

If an advance exhausts the newer bracket, present the newest known state and enter a hold. Freeze
the numeric cursor on subsequent frames until an accepted snapshot provides an endpoint newer than
the cursor. A snapshot at or behind the frozen cursor is late: retain it as the newest known older
endpoint, update the held presentation, and continue waiting for a newer bracket. This keeps the
cursor and visible lifecycle monotonic without allowing a long outage to run the cursor arbitrarily
far ahead of received authority.

Apply bounded presentation-clock discipline:

- While holding, set the effective cursor rate to `0.0`.
- Deadband: `±0.25` server tick.
- Outside the deadband:

  ```text
  rate = clamp(1 + 0.02 * tick_error, 0.95, 1.05)
  ```

- Advance at exactly `1.0` inside the deadband.
- Hard-rebase forward to the desired cursor only if no valid lower bracket can be recovered or the
  cursor is more than six ticks behind the desired cursor.
- Never hard-rebase backward. When the cursor is ahead of the desired delay after a hold, rebuild
  the target delay through the bounded slow rate.

This rate adjustment prevents long-term presentation-buffer drift without changing client input
or prediction cadence.

## Sampling and Entity Lifecycle

For every non-owned entity, find the older and newer snapshots bracketing the cursor.

- Present in both: linearly interpolate position and mass; derive radius from interpolated mass.
- Present only in the older sample: hold the older state through the interval and remove it when
  the cursor reaches the newer tick.
- Present only in the newer sample: create it when the cursor reaches the newer tick.
- Removed and later recreated with the same protocol ID follows the full-snapshot endpoint
  sequence and never interpolates across absence.
- Food uses the same delayed lifecycle; its position normally remains fixed.

Interpolate across wider server-tick gaps when packet loss removes intermediate snapshots and
valid endpoints still exist. If no newer bracket exists, hold the newest known state. Never
extrapolate remote movement or run guessed remote input.

Count one hold/underrun event per continuous episode, not once per rendered frame. Record episode
duration and recovery.

## Jitter and Buffer Metrics

For consecutive accepted samples, compare observed local arrival spacing with the interval
implied by their server-tick difference. Feed the absolute deviation into an exponentially
weighted moving average using weight `1/16`.

Expose:

- Samples stored and capacity.
- Tick and millisecond coverage.
- Target and current presentation delay.
- Fractional presentation server tick.
- Current cursor rate scale and tick error.
- Older/newer bracket snapshot IDs and ticks.
- Interpolation alpha.
- Latest and EWMA arrival jitter.
- Late-snapshot count.
- Hold/underrun episode count, current state, and duration.
- Rate-correction and hard-rebase counts.
- Delayed entity create/remove counts.

A late snapshot is one whose authoritative tick is at or behind the presentation cursor when it
arrives. It may still be retained only when it can form a required bracket; it must never move
the cursor backward.

## Post-merge review remediation

A review of the merged MC-20 implementation found that the first underrun frame held the newest
entity sample but continued advancing the numeric cursor. On the following frame, the symmetric
large-error rule rebased the cursor backward, visibly replaying old remote movement. The same
review found that startup returned an empty remote frame instead of holding the first sample and
that lifecycle/recovery metrics and tests did not fully match the completed checklist.

The corrective implementation:

- Presents the first accepted sample while the normal six-tick buffer warms up.
- Freezes the cursor after it exhausts a newer bracket and resumes only from a newer endpoint.
- Restricts hard rebases to forward recovery and tests cursor monotonicity across multiple hold
  frames and late-snapshot recovery.
- Records current, last, maximum, and total hold duration plus recovery count.
- Counts remote entity create/remove transitions from frames actually exposed to presentation.
- Covers packet-loss gaps, delayed creation/removal, and same-ID removal/recreation in
  deterministic tests.

## Remote Debug Visualization

For every remote player:

- Filled circle: actual interpolated presentation.
- Cyan outline: older known authoritative sample.
- Blue outline: newer known authoritative sample.
- Cyan-to-blue dots: short connector from older toward newer endpoint.

The Interpolation tab reports cursor tick and alpha plus endpoint snapshot IDs, ticks, and values
for the lowest-ID sampled remote player as a representative example. It does not provide a
per-entity selector.

**Show remote endpoint outlines**, default on, hides every remote player's endpoint colors and
connector without changing the interpolated remote fill, buffered state, or simulation.

Keep Feature 12 colors distinct from Feature 11's local orange/white/magenta prediction layers.
Do not describe interpolated positions as replicated or authoritative state.

## Fixed Delay and Deferred Adaptation

Feature 12 uses a fixed 200 ms target delay. Measured jitter and hold behavior determine whether
a later change should adapt the target between two and four snapshot intervals.

RTT and jitter do not feed the target-delay formula in this feature. Network changes affect which
samples are available, the cursor's error relative to `newest_server_tick - 6`, and whether the
sampler holds or hard-rebases. During active sampling the bounded 95–105% cursor rate corrects
buffer position; a hold freezes at `0.0`. Neither mode is an estimated-live-server clock or looks
ahead of received authority.

Deferred work includes:

- Adaptive target-delay policy.
- Remote prediction or extrapolation.
- Active local input-clock discipline and target command slack.
- Server-side rewind.
- A persistent client render world unless a later game's presentation requirements justify it.

## Documentation

- Update `docs/debugging_and_observability.md` with implemented buffer and interpolation fields,
  visual styles, and diagnoses.
- Update README to describe delayed remote presentation separately from local prediction.
- Update the networking guide's current timeline and Feature 12 status.
- Preserve the distinction between transport RTT, snapshot arrival jitter, intentional
  interpolation delay, and current presentation-buffer depth.

## Phased Checklist

### Phase 12.1: Accepted snapshot stream and presentation buffer

- [x] Expose every accepted snapshot and arrival time from client event processing.
- [x] Add the fixed 32-sample ordered buffer.
- [x] Add capacity, ordering, coverage, and jitter tests.
- [x] Checkpoint implemented: accepted stream and bounded buffer tests pass.

### Phase 12.2: Remote interpolation and presentation clock

- [x] Add the six-tick delayed cursor and startup buffering.
- [x] Add bounded 95–105% cursor-rate correction and hard rebase.
- [x] Add interpolation, lifecycle, loss-gap, hold, and recovery behavior.
- [x] Keep the controlled player exclusively on Feature 11 presentation.
- [x] Add deterministic timeline and lifecycle tests.
- [x] Checkpoint implemented: delayed interpolation and lifecycle tests pass.

### Phase 12.3: Remote interpolation debugging

- [x] Add selected-entity endpoint and interpolation visualization.
- [x] Add the all-remote bracket toggle.
- [x] Add buffer, jitter, cursor, hold, lifecycle, and rebase overlay fields.
- [x] Update the debugging guide from planned to implemented.
- [x] Checkpoint implemented: diagnostics and endpoint views are available.

### Phase 12.4: Integration and exit validation

- [x] Run deterministic jitter, capacity, hold, and hard-rebase schedules.
- [ ] Manually validate native remote smoothness and debug views.
- [x] Run the full configure, build, and CTest suite.
- [x] Finalize README, networking, and debugging documentation.
- [ ] Feature 12 completion approved.

## Exit Criteria

- Non-owned players render smoothly between known snapshots with a 200 ms target delay.
- Independent client/server clock drift is absorbed by bounded presentation-rate correction.
- Loss and jitter produce explainable interpolation, holds, or rebases without state explosions.
- Entity create/remove behavior is deterministic on the delayed timeline.
- The debugger exposes both known endpoints and the actual interpolated representation.
- The controlled player remains responsive through Feature 11 prediction and is never routed
  through the remote buffer.
