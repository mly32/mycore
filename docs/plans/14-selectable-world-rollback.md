# Feature 14: Selectable World Rollback

## Purpose

Replace Feature 11's position-only predictor with a Dots-owned rollback kernel capable of
restoring and resimulating complete gameplay state. Use split/merge, cooldowns, predicted spawns,
food, and player absorption as the proving vertical slice.

The durable contracts live in `../rollback_prediction_design.md`. This plan sequences their
implementation and must not redefine them independently.

## Scope

Feature 14 includes:

- Complete Dots World checkpoints and fixed-tick history.
- Selectable prediction sets, defaulting to all replicated entities.
- Same-frame atomic rollback and hard-resync recovery.
- Predicted remote movement using recorded assumptions.
- Predicted split/launch/remerge and cooldown state.
- Predicted entity lifecycle and authoritative spawn classification.
- Guarded session/durable consequences and resimulation-aware cues.
- Adaptive command-buffer timing.
- Predicted-world presentation, Feature 12 fallback/comparison, debugging, and load metrics.

Feature 14 does not add AOI, delta snapshots, server-side rewind, client authority, scoring,
achievements, a general physics engine, a game-neutral rollback library, or multi-frame replay.

## Module and World Boundaries

Add `Dots::Prediction` under `games/dots/prediction`. Its public include root is
`dots/prediction/`; it depends on Dots simulation value types and contains no SDL, rendering,
transport, or application code.

Dots simulation gains complete checkpoint capture/restore and all state required to replay its
fixed tick. Client runtime owns network validation, command transmission, authoritative
checkpoint hydration, and kernel lifetime. Presentation reads immutable committed/debug views.

The authoritative server continues to step one World once per 30 Hz tick. It never runs client
rollback.

## Protocol Version 4 and Complete State

Bump the protocol to version 4 with no dual-version negotiation.

`ServerWelcome` carries immutable server gameplay/prediction configuration required to run the
same rules. Entity/snapshot state adds:

- Owner ID and applied movement.
- Split launch velocity.
- Per-piece merge eligibility tick.
- Per-session split cooldown deadline.
- Predicted-spawn key when an authoritative entity originated from a predicted action.
- All session fields retained from Feature 13.

The client validates configuration before prediction becomes ready. It cannot override the
server values; local configuration may choose presentation/debug modes only.

Remote movement is level state and may be held in prediction. Remote action bits are never held.
Feature 16 must materialize delta state into the same coherent checkpoint contract.

## Rollback Kernel

Implement the public contracts from the design document:

- `PredictionKey`
- `PredictionSetMode`
- `CuePolicy`
- `WorldCheckpoint`
- `RollbackFrameRecord`
- `ReconcileReport`
- `RollbackKernel`

The initial ring stores 64 fixed ticks. Every accepted authoritative update builds scratch state,
discards acknowledged commands, replays the suffix, resolves cues, and commits atomically in the
same client frame.

The existing 2 ms replay budget becomes a warning and metric. It does not interrupt replay.
History exhaustion, incompatible checkpoints, or ambiguous predicted identity hard-resync to the
newest validated authority and clear incompatible speculative state.

## Prediction Set and Remote Presentation

Modes are:

- `AllReplicated`: predict the entire full snapshot; default for Feature 14.
- `OwnerAndInteractionClosure`: predict owned pieces and all possible replay-window interactors.
- `OwnerOnly`: predict owned pieces and keep remotes on Feature 12 interpolation.

Mode changes occur only at an atomic boundary and rebase incompatible history.

The normal Feature 14 presentation renders the predicted World. Feature 12 interpolation remains
available as a runtime fallback and A/B comparison. Debug views can draw authoritative-known,
predicted, interpolated, pre-correction, and smoothed presentation layers simultaneously.

Unknown remote input holds the replicated movement vector. Its source snapshot and held tick
range are recorded in rollback history and metrics.

## Split, Launch, and Merge Rules

Split is an edge-triggered action associated with its input sequence.

Defaults, all server-owned and immutable for a session:

- Split recast: 15 ticks/0.5 seconds.
- Merge delay: 150 ticks/5 seconds.
- Maximum pieces per owner: 8.
- Minimum eligible parent mass: 16.
- Child launch speed: 18 world units/second.
- Linear launch-speed decay: 18 world units/second squared.
- Post-deadline cohesion speed: 3 world units/second.

On a valid split:

1. Iterate owned pieces by ascending authoritative/predicted stable identity.
2. Split every eligible piece until the owner reaches the cap.
3. Divide parent mass equally and update both radii.
4. Use current non-zero movement direction, then last non-zero direction, then positive X.
5. Create each child with `PredictionKey{client, input sequence, ordinal}`.
6. Apply launch velocity to the child and assign the merge deadline to both results.
7. Set the session split-cooldown deadline.

A rejected split still consumes and ACKs its input sequence; authoritative topology/deadlines
cause rollback to remove the prediction.

After merge eligibility, add cohesion toward the owner's mass-weighted centroid. Same-owner
pieces never absorb one another. Eligible touching/overlapping pieces merge automatically in
stable-ID order, preserve mass, and use mass-weighted position and velocity. A session enters
confirmed defeat only when authority removes its last piece.

Enemy absorption consumes individual pieces through Feature 13's ordering. Player mass and
piece count remain conserved across split and merge apart from food/enemy transfer.

## Predicted Spawn Classification

Replay recreates the same temporary child for the same prediction key. Authority includes the
key on the permanent entity.

- Match: remap the temporary presentation/entity association without replaying cues.
- Reject: remove the temporary entity during reconciliation and cancel eligible cues.
- Authority-only spawn: create normally.
- Ambiguous key: hard-resync and report a correctness failure.

Client temporary handles are never accepted by the server as entity authority.

## Guarded Consequences and Cues

Predict reversible World state, including movement, food, mass, absorption, split/merge, and
entity topology.

Keep these confirmed-only:

- Playing/spectating transition.
- Respawn success and placement.
- Score, kill feed, and achievements.

When the predicted World removes the local player's last piece before authority confirms defeat,
presentation retains a pending-elimination control/camera proxy and client runtime continues
capturing input. Confirmation transitions to spectator mode; rollback restores the predicted
piece and cancels the proxy.

Every speculative effect declares `Resimulated`, `Deduplicated`, `Cancelable`, or
`ConfirmedOnly`. Reconciliation never blindly fires all step events again.

## Adaptive Command Timing

The server remains fixed at 30 Hz and consumes at most one ordered command per session each tick.

Feature 14 targets a pending server depth of two commands. On prediction readiness, submit two
neutral commands to seed the lead without moving the player.

For each accepted snapshot:

```text
smoothed depth = EWMA(latest server pending depth, alpha = 1/8)
```

Depth 1.5 through 2.5 is a deadband. Outside it:

```text
rate scale = clamp(1 + 0.025 * (2 - smoothed depth), 0.95, 1.05)
```

Apply the scale only to client prediction/input cadence. Server tick rate, cooldown deadlines,
and client session wall time do not change. An empty queue holds movement but clears edge
actions.

Expose target/latest/smoothed depth, cadence scale, empty/high events, and accumulated phase
correction. Validate convergence under step changes in latency and modest clock drift.

## Same-Frame Replay and Deferred Time-Slicing

Replay completes and atomically commits in the snapshot's client frame. Do not implement a
second time-sliced scheduler in this feature.

Phase 14.6 collects the metrics specified by the design document across 10, 100, 500, and 1,000
entities and 100, 200, and 400 ms RTT. If the documented p99/overrun thresholds are crossed after
cheaper mitigations are evaluated, stop and write a separate multi-frame resimulation plan.

## Debugging and Fault Injection

Add a **Rollback** tab rather than overloading Feature 11's Prediction tab. Display:

- Prediction-set mode and predicted/interpolated/confirmed entity counts.
- Authoritative snapshot/tick, predicted tick, prediction lead, ACK, and replayed input range.
- History occupancy, replay ticks, duration distribution, and hard-resync reason.
- Continuous corrections and structural create/remove/ownership corrections.
- Predicted-spawn pending/matched/rejected/ambiguous counts.
- Cue replay/deduplication/cancellation/confirmation counts.
- Command-buffer target/latest/EWMA depth, cadence scale, and drift corrections.

World-space tools show the selected entity's authoritative-known, predicted, interpolated,
pre-correction, and smoothed state. Structural replay markers label predicted spawns, removals,
and classifications.

Add deliberate client-only faults for position/mass divergence, predicted split rejection,
spawn-classification mismatch, action packet loss, and remote held-input divergence. Fault state
must have a durable receipt and remain distinct from transport metrics.

## Implementation Checkpoints

Do not start a checkpoint until the preceding checkpoint is reviewed and approved.

### Phase 14.1: Complete checkpoint schema

- [ ] Add protocol version 4 and immutable server simulation configuration.
- [ ] Capture, validate, hydrate, and restore complete Dots World state.
- [ ] Prove checkpoint round trips and deterministic ticks without networking.
- [ ] Phase 14.1 approved.

### Phase 14.2: Atomic rollback kernel

- [ ] Add `Dots::Prediction` and its public contracts.
- [ ] Add the 64-frame ring, selectable set, transactional reconciliation, and hard recovery.
- [ ] Replace the position-only scratch replay without exposing partial state.
- [ ] Phase 14.2 approved.

### Phase 14.3: All-replicated prediction

- [ ] Predict remote held movement and every full-snapshot entity.
- [ ] Make predicted World presentation the default.
- [ ] Preserve Feature 12 interpolation as fallback and comparison.
- [ ] Phase 14.3 approved.

### Phase 14.4: Structural gameplay and cues

- [ ] Add split/launch/remerge and server-owned cooldown validation.
- [ ] Add predicted spawn identity/classification and structural replay.
- [ ] Add guarded session consequences and resimulation-aware cue policies.
- [ ] Phase 14.4 approved.

### Phase 14.5: Adaptive timing

- [ ] Add neutral prefill, queue EWMA/deadband, and bounded cadence control.
- [ ] Add dynamic latency/drift tests and timing metrics.
- [ ] Document predicted tick, session time, and server-time relationships.
- [ ] Phase 14.5 approved.

### Phase 14.6: Observability and exit validation

- [ ] Add Rollback tab, state/topology overlays, and deliberate faults.
- [ ] Run two-client impairment scenarios and entity-scale workloads.
- [ ] Record the same-frame versus deferred multi-frame research decision from metrics.
- [ ] Update Feature 15's AOI entry/exit and collision-margin obligations.
- [ ] Feature 14 completion approved before Feature 15 implementation.

## Test Plan

Kernel tests cover matching and mismatching complete replay, stale authority, ACK monotonicity,
mode changes, no partial commit, history capacity, checkpoint incompatibility, and hard resync.

Gameplay tests cover deterministic split order, cap/minimum/cooldown rejection, launch/decay,
merge eligibility/cohesion, mass conservation, same-owner immunity, enemy piece absorption, and
last-piece defeat.

Lifecycle tests cover accepted/rejected/authority-only/ambiguous spawns, stable replay identity,
and cue replay/deduplication/cancellation/confirmation.

Timing tests cover neutral prefill, EWMA/deadband math, 0.95/1.05 clamps, empty queues, edge-action
non-repetition, latency step changes, loss/reordering, and clock drift.

Integration scenarios run two clients at 100-200 ms artificial latency with jitter/loss and
verify eventual convergence to identical authority. Performance workloads record the design
document's metrics at 10, 100, 500, and 1,000 predicted entities without making wall-clock timing
a flaky unit-test assertion.

## Exit Criteria

- The client restores and resimulates complete Dots gameplay state from validated authority.
- All-replicated prediction is the normal view and Feature 12 remains a working fallback.
- Split/merge, cooldowns, absorption, and predicted lifecycle converge after rejection or remote
  disagreement.
- Durable session consequences never occur from speculation alone.
- Every rollback is atomic, bounded, measurable, and recoverable.
- Dynamic command timing remains bounded and does not alter server authority or session clocks.
- Metrics produce an explicit evidence-based decision on whether multi-frame replay warrants a
  separate future plan.
