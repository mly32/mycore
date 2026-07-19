# Feature 11: Prediction and Reconciliation

## Goal

Make the controlled player respond immediately to local movement input while preserving the
headless server as the only gameplay authority. Reconcile prediction from validated server
snapshots, replay unacknowledged input atomically, and smooth only the visible correction.

Feature 11 also adds bounded input redundancy, correction/replay observability, and deliberate
fault injection so prediction behavior can be explained instead of inferred from motion alone.
Remote-entity interpolation remains Feature 12 work.

## State and Ownership Model

Keep these client-side values distinct:

1. **Replicated authority sample:** the newest validated server snapshot received by the client.
   This is historical by the time it arrives and must not be labeled as the server's live state.
2. **Predicted state:** the controlled player's authoritative sample plus replayed unacknowledged
   inputs.
3. **Pre-correction state:** the predicted position immediately before the latest nonzero
   reconciliation correction, retained temporarily for debugging.
4. **Presentation state:** predicted position plus a decaying visual correction offset.

Only the server owns and steps `simulation::World`. The client predictor owns position only; it
does not predict mass, food consumption, spawning, death, or interactions with other players.
Replicated state is installed immediately, prediction is corrected immediately, and presentation
is the only layer allowed to converge over time.

## Protocol Version 2 and Input Redundancy

Bump the Dots protocol to version 2. Feature 10 binaries are rejected through the existing
version check; dual-version negotiation is out of scope.

Replace the version-1 single-command input payload with a bounded packet while retaining input
message-kind value `3`:

```text
InputPacket
    sample_count                  u8, 1..3
    last_received_snapshot_id     u32
    samples                       sample_count × InputSample

InputSample
    sequence_id                   u32
    client_tick                   u32
    movement_x                    f32 bits
    movement_y                    f32 bits
    action_bits                   u16
```

Samples are consecutive and ordered oldest to newest. The decoder validates the count, exact
payload size, sequence and client-tick ordering, IDs, finite bounded movement, and action bits.
The maximum encoded input packet is 71 bytes including the 12-byte packet header.

Add `[network].input_redundancy = true` to the built-in configuration, sample TOML, and JSON
schema. Enabled packets contain the current input and up to two prior unacknowledged samples.
Disabled packets contain only the current sample. The server always accepts either valid form.

Add the current bounded server pending-input count to each client-specific full snapshot as a
`u8` after `last_processed_input_id` and before the entity count. Valid values are `0..64`. This
is input-scheduling telemetry, not transport queue depth or a measurement of network RTT.

## Server Input Scheduling

Give each ready server session a bounded ordered queue of 64 distinct input samples.

- Ignore samples already acknowledged as processed.
- Deduplicate overlapping redundant packets.
- Insert fresh reordered samples by sequence.
- Apply at most the oldest queued sample for each client before one authoritative world tick.
- Advance `last_processed_input` only after that sample is included in a successful world step.
- Continue the player's previously installed desired movement when no new sample is available.
- Disconnect only the offending session if its valid-looking input stream overflows the queue.

This prevents multiple commands received in one poll from overwriting one another before a world
step and gives redundant samples real recovery semantics. It keeps server work bounded and adds
no server rollback or resimulation. At 1,000 clients and 30 input packets per second, the maximum
three-sample application payload is approximately 2.13 MB/s before transport overhead.

## Shared Movement and Client Prediction

Extract Dots-owned movement normalization and one-fixed-tick position advancement functions from
`simulation::World`. The world and client predictor must execute the same operations and
constants. Do not move Dots movement policy into an engine library.

`Dots::ClientRuntime` owns a fixed 256-entry input/result ring. Each entry contains the exact
protocol input sample and its resulting predicted position. At 30 Hz the ring covers roughly
8.53 seconds.

For a normal input send:

1. Construct and validate the current sample and its redundancy packet.
2. Send the packet unreliably.
3. Only after a successful send, append the sample to history and advance prediction one tick.
4. Increment sequence and client tick without wrapping.

Explicit debug-simulated packet loss is the sole exception: it suppresses transport send while
recording and predicting the sample as though the network dropped it.

## Transactional Reconciliation

For every newer accepted snapshot after the session is ready:

1. Validate the controlled entity and its player kind.
2. Validate the input acknowledgement before mutating replicated or predicted state.
3. Reject an ACK that exceeds the last sent input, regresses, or becomes invalid after a valid
   ACK.
4. Start scratch prediction at the snapshot's authoritative controlled-player position.
5. Discard history entries through the acknowledged sequence.
6. Replay every remaining sample in sequence using shared movement code.
7. Record the replay path and correction metrics.
8. Atomically commit the validated replicated world and completed predicted result.

Never expose partially replayed state. Stale snapshots remain ignored and cannot roll any client
layer backward.

Replay finishes in the snapshot's client frame. Movement-only replay is bounded to 256 vector
steps, so multi-frame replay would add complexity without a demonstrated need. Instrument the
work with a Tracy zone and record latest, rolling-average, and maximum duration. Replays over 2
ms increment an over-budget counter and produce a rate-limited warning, but still complete
atomically.

If the input ring is full before a new sample is recorded, hard-resync to the newest replicated
controlled-player position, clear input history and presentation smoothing, increment and log a
hard resync, then continue with the new input. Do not silently discard only the oldest entry or
dynamically resize the bound.

## Correction Smoothing

A correction is the distance between the predicted result immediately before reconciliation and
the fully replayed result. Distances at or below `0.0001` world units are treated as a match.

For a nonzero correction:

1. Correct predicted state immediately.
2. Evaluate the currently remaining presentation offset.
3. Add `old_predicted - corrected_predicted` to that residual so the rendered position is
   continuous at the correction instant.
4. Decay the residual linearly to zero over 100 ms.

The rendered controlled-player circle and following camera use the identical presentation
position. Replicated mass and radius remain authoritative. A hard resync clears smoothing and
snaps presentation deliberately.

## Prediction Debugging and Metrics

Extend the ImGui overlay with a **Session** section:

- Client runtime and connection state.
- Protocol version.
- Server-assigned client ID and controlled entity ID.
- Connection handle.
- Latest snapshot ID and server tick.
- Local input tick. It is not directly comparable to the server tick without synchronization.

Add a **Prediction** section:

- Redundancy enabled/disabled.
- Last input sent and acknowledged.
- Unacknowledged command lead.
- Input-history count, capacity, percentage, and high-water mark.
- Latest server pending-input depth and observed high-water mark.
- Rollback snapshot, server tick, and input ACK.
- Last, total, and maximum replay counts.
- Latest, average, and maximum replay duration.
- Reconciliation and nonzero-correction counts.
- Last and maximum correction distance and rolling corrections per minute.
- Remaining smoothing-offset vector and magnitude.
- Replay-over-budget, hard-resync, and injected-fault counts.

Color history utilization at 50%, 75%, and 90%. Rate-limit warnings above 75% and report
recovery.

Use existing outlined-circle rendering for world-space inspection:

- Filled player circle: actual presentation position.
- White outline: corrected predicted position.
- Orange outline: latest received authoritative position.
- Magenta outline: predicted position immediately before the latest correction.
- Small purple circles: latest correction's replay path.

Slightly offset outline radii keep coincident layers visible. Retain correction-specific ghosts
and replay markers for two seconds. The orange ghost must be labeled **latest authoritative
sample**, never **live server position**.

Add interactive debug controls:

- Show/hide prediction layers, enabled by default.
- Show/hide the latest replay path, enabled by default.
- Inject a client-only `+1` world-unit X prediction error.
- Drop the next three input packets while continuing prediction. Three packets exceed the
  two-command redundancy window.
- Clear retained correction visualization.

Show an explicit warning while fault injection is armed and keep injected drops separate from
transport-reported loss.

## Clock Drift and Buffer Sizing

Feature 11 observes but does not actively discipline the local input clock.

- Track sent-minus-ACK command lead, server pending-input depth, high-water marks, and rolling
  trends.
- Small oscillator differences are absorbed by the bounded server queue and corrected through
  authority.
- Sustained server-queue growth indicates clock drift, server overload, or excessive client
  input production.

Actively speeding or slowing local simulation requires target server ticks, command slack, and a
server-clock estimate. That is deferred tick-synchronization work, not Feature 12 remote
interpolation.

Keep the prediction ring fixed at 256. Tune the constant only from impairment data using the
target RTT, snapshot delay, jitter allowance, input rate, redundancy depth, and a safety margin.
Expose utilization rather than adding a runtime configuration that changes correctness bounds.

## Documentation

- Add and maintain `docs/debugging_and_observability.md`.
- Update README with the implemented responsive-client behavior and debugging entry points.
- Update the networking guide's current-state descriptions and protocol-v2 timeline.
- Add the debugging guide to `AGENTS.md` routing for prediction, interpolation, metrics, and
  timing work.
- Preserve the distinction between transport, replication, prediction, and presentation
  measurements.

## Phased Checklist

### Phase 11.1: Protocol and server input foundation

- [x] Protocol version 2 and bounded input packet.
- [x] Redundancy configuration and schema.
- [x] Server input queue and snapshot queue-depth telemetry.
- [x] Protocol, queue, recovery, and overflow-isolation tests.
- [x] Checkpoint approved before continuing.

### Phase 11.2: Prediction and atomic reconciliation

- [x] Shared movement functions and 256-entry history.
- [x] Immediate prediction and transactional ACK validation.
- [x] One-frame scratch replay, hard resync, metrics, and Tracy zone.
- [x] Matching, mismatch, stale, delayed, invalid-ACK, and capacity tests.
- [x] Checkpoint approved before continuing.

### Phase 11.3: Local presentation and visual debugging

- [x] 100 ms correction smoothing.
- [x] Authority, prediction, pre-correction, presentation, and replay visuals.
- [x] Client/session identifiers and prediction overlay.
- [x] Interactive fault injection and debug tests.
- [x] Debugging guide updated from planned to implemented.
- [ ] Checkpoint approved before continuing.

### Phase 11.4: Integration and exit validation

- [ ] Deterministic and native 100–200 ms impairment validation.
- [ ] Redundancy enabled/disabled playability checks.
- [ ] Full configure, build, and CTest run.
- [ ] README and networking documentation finalized.
- [ ] Feature 11 completion approved before Feature 12 implementation.

## Exit Criteria

- Local movement responds on the client input tick without waiting for a server snapshot.
- Reconciliation always accepts server truth and atomically replays bounded unacknowledged input.
- Correct prediction produces no correction event; mismatch is visible and explainable through
  metrics and world-space ghosts.
- Presentation correction converges in 100 ms without altering simulation state.
- The server remains authoritative, headless, bounded per client, and isolated from offender
  failures.
