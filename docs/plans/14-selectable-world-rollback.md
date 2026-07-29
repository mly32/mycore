# Feature 14: Engine Rollback Programming Model and Predicted Dots World

## Problem

Before Feature 14, Feature 11 predicted only the controlled position. The server alone stepped
the complete Dots World, while remote presentation used Feature 12 delayed interpolation. That model cannot
speculate structural mechanics such as food consumption, player absorption, split, or merge, and
its position-specific history cannot become a clean shared mechanism by adding more special
cases.

Feature 14 must establish a reusable programming model for:

- Capturing and restoring complete game-defined checkpoints.
- Retaining immutable sampled input plus explicit per-tick authority-derived assumptions.
- Comparing authority with the corresponding predicted history.
- Replaying to the current prediction head and committing atomically.
- Distinguishing authoritative, predicted, interpolated/extrapolated, and presentation state.
- Regenerating deterministic simulation events without repeating irreversible consequences.

The durable contracts live in `../rollback_prediction_design.md`. This plan sequences them and
must stay aligned with that document.

## Goals

- Add a game-neutral, statically typed rollback timeline under `engine/`.
- Make Dots the first demanding production example rather than embedding Dots policy in the
  engine API.
- Run shared Dots movement, food, absorption, split, and merge logic in a closed predicted island.
- Roll forward retained local commands and recorded assumptions after authoritative correction.
- Refresh superseded authority-derived assumptions without resampling or rewriting local
  commands.
- Demonstrate `PredictOnce`, `PredictCancelable`, and `ConfirmOnce` consequence delivery.
- Retain same-frame atomic replay while leaving a clean seam for separately justified
  multi-frame resimulation.
- Produce clear authority, prediction, and presentation debugging and workload evidence.

## Non-Goals

Feature 14 does not add AOI, delta snapshots, server rewind, client authority, scoring,
achievements, a general physics/ECS system, an audio backend, persistent exactly-once effects,
or a production multi-frame replay scheduler.

Feature 12 interpolation remains supported. Feature 14 adds bounded visual extrapolation outside
the predicted closure, but that extrapolation never runs collision or gameplay rules.

## Chosen Design and Tradeoffs

### Engine mechanism, game-owned model

Earlier planning kept rollback entirely Dots-owned until a second game existed. Feature 11 and
the complete Dots requirements now expose a clear game-neutral contract: typed state,
checkpoint, stimulus, deterministic step, history, transactional replay, and post-commit event
lifecycle. Extract those mechanisms as `MyCore::Rollback`; keep mechanics, data schema, closure,
event semantics, and presentation in Dots.

A C++20 concept/template API is chosen over virtual callbacks or a runtime mechanic registry.
This preserves strong types, makes the engine independent of a universal event/schema model, and
lets tests instantiate small non-Dots models.

### Interaction closure by default

Predicting every replicated entity is valuable as a correctness oracle but is not the
1,000-player steady-state. The normal profile is the fixed-point interaction closure seeded from
owned pieces. `FullReplicated` remains a benchmark/oracle, and `OwnedGameplay` is the safe
fallback when required closure state is missing.

### Event journal before side effects

Simulation produces deterministic typed event journals. It never invokes audio, particles, UI,
logs, networking, or analytics during a step or replay. A generic post-commit router compares
stable keys and applies a policy per typed handler. The occurrence ledger is non-rewindable, so
restoring the World cannot cause a sound or one-shot effect to happen repeatedly.

### Hybrid remote presentation

Entities inside the closure run shared Dots simulation. Entities outside it may use six
ticks/200 ms of presentation-only movement/launch extrapolation and then hold. Delayed
interpolation remains the spectator/fallback/comparison path. Neither smoothed nor extrapolated
state can seed prediction.

### Diagnostic digest, typed correctness

Protocol version 4 includes a canonical checkpoint digest for wire validation and debug
comparison. Full typed validation and differences decide reconciliation; the hash is not treated
as proof of equality.

### Same-frame first

Reconciliation builds scratch state and publishes once after replay reaches the current
prediction head. A 2 ms budget is a warning. Only measured p99/frame-overrun thresholds may
create the separate `spike/multi-frame-resimulation` follow-up.

## Ownership and Data Flow

- `MyCore::Rollback` depends only on Core and Time and owns the generic timeline, frame history,
  atomic replay transaction, event transitions, occurrence ledger, and static consequence router.
- `Dots::Simulation` owns complete value checkpoints, owner-command ticking, structural rules,
  deterministic ordering, and tick journals.
- `Dots::Prediction` adapts World to the engine model and owns mechanic contracts, closure,
  stable Dots event keys, typed diffs, and scope rebasing.
- Dots protocol/replication hydrates validated authoritative checkpoints and repeated
  authoritative consequence receipts.
- Dots client runtime owns networking, command sampling, timeline lifetime, and post-commit
  fanout.
- Dots presentation owns smoothing, visual cue handler instances, entity remapping,
  interpolation/extrapolation, and debug drawing.
- The authoritative server steps one World at 30 Hz and never runs client rollback.

Normal client flow:

```text
sample devices/network
        |
        v
immutable Dots TickStimulus ---> Timeline::advance
                                      |
                                      v
                            committed predicted World
                                      |
                     Commit observers / consequence router
                                      |
                                      v
                              presentation World

validated snapshot ---> AuthorityFrame ---> scratch restore/replay ---> atomic Commit
```

## Public Interfaces

`MyCore::Rollback` exposes:

- `Timeline<Model>` with `initialize`, `advance`, `reconcile`, `rebase_scope`, `hard_resync`, and
  immutable history/state/statistics access.
- `HistorySettings`, initially fixed to 64 ticks for Dots.
- `AuthorityFrame<Model>`, `FrameRecord<Model>`, `Commit<Model>`, and
  `CommitResult<Model>`.
- `CommitKind::{Initialize, Advance, Reconcile, ScopeRebase, HardResync}`; initialization is a
  commit so first-frame authoritative events are observable.
- `EventTransition::{FirstPredicted, Revised, Retracted, Confirmed, AuthorityOnly}`.
- `ConsequencePolicy::{PredictOnce, PredictCancelable, ConfirmOnce}`.
- `StaticConsequenceRouter<Model, Handlers...>` with a separate ledger for each typed handler.
- Commit retirement information that lets routers prune inactive keys only after retained replay
  can no longer regenerate them and the authority-receipt watermark proves duplicates cannot
  return. The first kernel increment retains session tombstones until receipt integration can
  provide both proofs.

A rollback model supplies `State`, `Checkpoint`, `Stimulus`, opaque game-defined `Scope`, an
event variant, stable event-key variant, typed diff/digest, checkpoint restore/capture, one atomic
fixed step, exact checkpoint equality for same-tick scope rebases, and event identity validation.

The engine never defines a Dots entity, component, command, protocol field, or cue.

## Complete Dots Checkpoint and Tick

Add value types for immutable `WorldRules`, complete `WorldCheckpoint`, owner state,
`PredictionKey`, `TickCommand`, typed `SimulationEvent`, and `TickJournal`.

Checkpoint state includes:

- Tick and allocator state.
- Sorted owners, players, and food.
- Entity identity/kind/owner, position, mass, applied movement, launch velocity, and relevant
  command identity.
- Owner-local split cooldown, owned-piece membership/count, last non-zero movement, merge
  eligibility, and predicted-spawn association; immutable piece-cap policy lives in
  `WorldRules`.
- Any deterministic cursor/RNG/order state before it is introduced.

Radius and spatial-grid storage are rebuilt. Confirmed Playing/Spectating and respawn results
remain session authority outside speculative World state.

The implemented [authoritative spawn search](authoritative-spawn-search.md) remains derived from
World state and adds no cursor. If it later introduces a cursor, free list, or random stream,
that value must enter the checkpoint before replay uses it. Join and respawn placement remain
server-only.

Replace separate input application and stepping composition with one shared tick:

1. Install level movement and consume edge actions once.
2. Split.
3. Movement, launch decay, and cohesion.
4. Enemy absorption.
5. Same-owner merge.
6. Food consumption.
7. Topology/resource commit and tick journal.

The server, offline client, and predictor call this same operation.

## Prediction Set

Each Dots `PredictionMechanic` statically declares state read/write domains, dependencies,
closure expansion, event identity, and presentation projection.

Initial mechanics:

- Movement.
- Food consumption.
- Player absorption.
- Split/merge.

`InteractionClosure` is a causal state closure, not only a spatial radius. Its Dots scope records
enabled mechanics, included entities/owners, required global state domains, causal authority
subscriptions, replay horizon, and scope epoch.

The fixed-point builder expands through:

- Ownership dependencies such as split cooldown, owned-piece membership/count, last movement,
  and owned aggregates.
- Conservative swept spatial bounds for movement, growth, split reach, food, and recursive
  player interaction.
- Mechanic dependencies and every state domain they read.
- Non-spatial causal/global dependencies that can change a predicted result.

Before advancing, the client recomputes the closure using the greater of a five-tick operating
floor and the next retained unacknowledged-input depth, not the 256-entry history capacity. The
current scope can continue while its selected causal membership and subscriptions contain that
fresh result; a smaller result retains the safe existing superset instead of oscillating
presentation ownership as ACK depth changes. Newly required membership or a changed causal
subscription forces an atomic scope rebase from latest authority and replay. Excluded entities
cannot interact with the predicted island. Missing required entity, owner, global, or causal
state falls back to `OwnedGameplay` and reports `IncompleteClosure`.

State policy is:

| State class | Feature 14 behavior |
|---|---|
| Owner-local deterministic state | Predict from retained commands; split cooldown and owned-piece count are the Dots proof. |
| Global deterministic baseline | Replicate immutable rules and checkpoint/advance the World tick; reject incompatible rules. |
| Mutable global aggregate | Predict only with every causal contribution; otherwise retain authority plus an explicitly speculative local delta. |
| Durable session/economy/match result | Authority-confirmed only. |

Score remains a non-goal. If added later, presentation may compose
`confirmed score + speculative local delta` and reconcile that delta by event key. Winning,
unlocking an ability, or any other score-driven gameplay consequence remains confirmed unless
the scope contains all score-affecting causes.

Feature 15 must provide a conservative AOI margin sufficient to build the same closure.

## Replay, Rollforward, and Recovery

Each retained frame records the exact Dots `TickStimulus`, checkpoint, scope epoch, diagnostic
digest, and generated event journal. It does not retain device callbacks. Sampled local fields
remain exact; the explicit engine refresh transaction may replace authority-derived assumption
fields before retained replay.

For authority at tick `T`:

1. Validate checkpoint/schema/digest, ACK, scope, identities, and receipts.
2. Compare with the predicted record at `T` for diagnostics.
3. Restore authority to scratch.
4. Drop acknowledged commands.
5. Refresh superseded authority-derived assumptions and replay the retained suffix through the
   previous prediction head.
6. Regenerate events, calculate typed differences, and resolve predicted identities.
7. Atomically publish World/history/event transitions/metrics/presentation correction data.

Stale or invalid authority does not mutate committed state. Missing history, capacity exhaustion,
incompatible checkpoint/scope, or ambiguous identity hard-resyncs to newest validated authority.
The explicit hard-resync recovery may accept a validated ACK beyond timeline history because it
discards that history; normal replay transactions may not. Duration alone never chooses an
incorrect partial state.

When speculative local elimination leaves no owner to step, Dots continues retaining and sending
input outside the temporarily deferred timeline. A session-validated ACK through that retained
range selects hard resync, then the client replays the remaining unacknowledged suffix. The
operation is permitted only when the exact ACK is still covered by the outer ring.

The [Feature 14 prediction-stutter postmortem](../feature14_prediction_stutter_postmortem.md)
records why the storage bound, causal horizon, immutable input, and refreshable assumption must
remain separate concepts.

## Consequence Demonstration Matrix

The physical mechanic is always replayed independently of how a presentation consequence is
delivered.

| Mechanic | Predicted gameplay | Dots demonstration | Mode | Rollback/confirmation behavior |
|---|---|---|---|---|
| Movement | Position and movement vectors | Avatar transform and motion trail | State-derived commit observer | Correct immediately; smooth only the rendered correction |
| Split | Mass division, child topology, launch, cooldown | Short split flash | `PredictOnce` | Appears immediately once; resimulation and later confirmation cannot repeat it |
| Split | Same split | Child launch ring/trail | `PredictCancelable` | Keyed to the predicted child; cancel on rejection and remap on confirmation |
| Food consumption | Food removal and mass gain | Food-pop particle group | `PredictCancelable` | Remove/fade if rollback restores the food |
| Player absorption | Victim removal and mass transfer | Immediate consume flash | `PredictOnce` | Makes the eat responsive; a rejected prediction may leave one brief false positive |
| Player absorption | Same absorption | Victim collapse/consume pulse | `PredictCancelable` | Cancel and restore presentation when the victim returns |
| Authoritative absorption/defeat | Confirmed durable result | Kill/defeat banner and stinger hook | `ConfirmOnce` | Deliver once from an authority receipt; prediction alone never announces it |
| Merge | Combined topology and mass | Blob geometry and motion | State-derived commit observer | Rebuild from committed pieces and smooth structural correction |

Dots currently has no audio backend. The feature uses Dots-owned render cues and fake test
handlers to prove the same API an audio handler will consume later.

## Consequence and Receipt Semantics

Policy is per handler, not per event type:

- `PredictOnce` records exposure before later replay can revisit the key. The same key never
  starts that handler twice in the session.
- `PredictCancelable` owns a typed token and receives predict/revise/cancel/confirm lifecycle
  calls. A one-shot sound must use `PredictOnce`, not this mode.
- `ConfirmOnce` receives only a published authoritative receipt batch and deduplicates repeated
  snapshots.

The router stages ledger changes before handler invocation. Handler errors are surfaced and not
automatically retried. A timeline retirement hint does not by itself prune an at-most-once
tombstone. Pruning becomes safe only when no retained stimulus can replay the key and the
authority-receipt watermark proves it cannot be repeated; game event keys are never reused.

Dots stable keys are explicit variants:

- Split: owner, input sequence, child ordinal.
- Food consume: stable food identity.
- Absorption: stable victim identity.
- Merge: sorted consumed-piece identities.

Protocol version 4 adds per-session monotonic authority-receipt sequences. Snapshots repeat up to
16 unacknowledged receipts, input packets ACK the highest contiguous published sequence, and the
server retains up to 256. Snapshots echo server retirement so the bounded client inbox can prune
payloads and live-key records. Overflow is an explicit session failure rather than silent cue
loss. Session state remains repeated snapshot state; receipts exist only to deliver transient
confirmed consequences once.

## Split and Merge Rules

Immutable server defaults:

- Split recast: 15 ticks/0.5 seconds.
- Merge delay: 150 ticks/5 seconds.
- Maximum pieces per owner: 8.
- Minimum eligible parent mass: 16.
- Child launch speed: 18 world units/second.
- Linear launch-speed decay: 18 world units/second squared.
- Post-deadline cohesion speed: 3 world units/second.

On a valid split, process pieces by stable identity, divide mass evenly, select current
movement/last movement/positive X for launch direction, create
`PredictionKey{owner, input sequence, ordinal}`, apply launch/deadlines, and set owner cooldown.
A rejected split still consumes and acknowledges its command.

After eligibility, pieces cohere toward the mass-weighted owner centroid and touching pieces
merge in stable order with mass-weighted position/velocity. Same-owner pieces never absorb each
other. Feature 13 enemy-absorption ordering remains unchanged. Authority alone confirms
last-piece defeat and Spectating.

## Protocol Version 4

Make one protocol bump after checkpoint, split, and event schemas are stable; do not implement
dual-version negotiation.

`ServerWelcome` carries immutable `WorldRules`. Snapshots add:

- Checkpoint schema ID and canonical 64-bit FNV-1a digest.
- Next authoritative entity ID and complete owner state.
- Applied movement, launch velocity, cooldown/merge deadlines, and optional `PredictionKey`.
- Bounded repeated authoritative receipt batches.

Input packets add the authority-receipt ACK. The client validates all fields and reconstructs a
typed checkpoint before timeline mutation.

## Presentation

The normal playing view composes:

- Predicted World entities inside the interaction closure.
- Fixed-tick render interpolation and 100 ms correction smoothing for committed predicted state.
- Bounded movement/launch extrapolation outside the closure for six ticks/200 ms, then hold.
- Feature 12 delayed interpolation for spectators, fallback, and comparison.

Extrapolated state cannot collide, consume, split, merge, seed closure, or become a checkpoint.
Entering the closure restores from authority and replays; presentation smooths from the prior
visual pose.

Predicted identity matching preserves a presentation association. Rejection removes/cancels it,
authority-only state creates normally, and ambiguity hard-resyncs.

## Adaptive Timing

The server stays at 30 Hz and consumes at most one ordered command per session tick. Prediction
targets two queued commands and begins with two neutral samples.

```text
smoothed depth = EWMA(latest server pending depth, alpha = 1/8)
rate scale = clamp(1 + 0.025 * (2 - smoothed depth), 0.95, 1.05)
```

Depth 1.5 through 2.5 is a deadband. Apply the scale only to client command/prediction cadence.
An empty server queue holds movement and clears edge actions.

## Debugging and Faults

Add a **Rollback** view with:

- Prediction profile, scope epoch, included mechanics/domains, closure count, and fallback.
- Authority/prediction ticks and digests, command ACK/replay range, history occupancy, and
  hard-resync reason.
- Continuous and structural differences.
- Event transition and per-consequence-policy counts.
- Authority receipt pending/ACK/duplicate/conflict counts.
- Replay p50/p95/p99/max and command-buffer health.
- Independently toggled authoritative, predicted, interpolated/extrapolated, pre-correction, and
  presentation layers.

Faults cover position/mass divergence, split rejection, predicted identity mismatch, action loss,
remote assumption divergence, repeated rollback of one event key, receipt duplication, and
receipt conflict. Fault receipts remain separate from transport statistics.

## Implementation Sequence

Keep commits focused and reviewable, but do not add approval gates between these steps:

1. Add `MyCore::Rollback`, including the timeline, scratch transaction, event transitions,
   consequence router, and non-Dots toy-model tests.
2. Refactor Dots World around complete checkpoints, owner state, atomic tick commands, stable
   event journals, and prediction identity while preserving current behavior.
3. Add `Dots::Prediction`, static mechanic contracts, causal interaction closure, prediction
   profiles, typed differences/digests, and offline rollback for movement, food, and absorption.
4. Add split/launch/cooldown/cohesion/merge rules, predicted identity, and offline consequence
   demonstrations.
5. Add protocol version 4 checkpoint fields, immutable rules, digest, prediction keys, sequenced
   authority receipts, and hostile validation.
6. Replace the client position ring with the complete Dots timeline and integrate
   interaction-closed prediction, split input, identity mapping, and confirmed session guarding.
6.5. Close the audit findings documented in
   [`../feature14_rollback_prediction_audit.md`](../feature14_rollback_prediction_audit.md):
   replace the incomplete fallback with owned gameplay, separate event subscriptions from state
   closure, publish every client timeline commit, use the kernel scope-rebase transaction, and
   install a bounded accepted/published/retired authority-receipt lifecycle.
7. Add persistent presentation composition, consequence handlers, correction smoothing, bounded
   extrapolation, interpolation fallback, and every consequence-matrix example.
8. Add adaptive command timing, complete Rollback diagnostics/faults, impairment scenarios,
   entity-scale workloads, documentation updates, and the measured same-frame/multi-frame
   decision.

Steps 1 through 7 are implemented on `feature/14`; step 8 is next.
`MyCore::Rollback` now provides the generic
timeline and consequence machinery. Dots Simulation now provides immutable `WorldRules`, sorted
complete checkpoints, atomic restore, one owner-scoped command batch per tick, typed food and
absorption, split, and merge journals, stable keys, and predicted identity storage. Its scratch
tick implements stable split ordering/cap/cooldown, child launch and decay, post-deadline
cohesion, stable mass-weighted merge, and the existing absorption/food ordering. Radius and
spatial indexes are rebuilt on restore. `Dots::Prediction` now provides static mechanic
contracts, the three prediction profiles, conservative causal/spatial closure, safe
incomplete-state fallback, predicted-child admission by unique owned `PredictionKey`, scope
projection, exact retained local/remote movement causes, canonical digests, typed differences,
and the complete-World adapter to `MyCore::Rollback`. Offline tests cover structural correction
and exercise `PredictOnce`, `PredictCancelable`, and `ConfirmOnce` with Dots events.

Protocol v4 now carries immutable rules, the schema-1 canonical checkpoint and digest, allocator
and complete owner/entity state, optional prediction keys, the split action bit, and monotonic
typed authority receipts. Replication builds and exactly rehydrates `WorldCheckpoint`, rejecting
restore or digest failures. The server retains at most 256 relevant receipts per session, repeats
at most 16 from the unacknowledged frontier, accepts only issued ACKs, and fails only a session
that exhausts retention. Protocol, replication, and in-memory session tests cover malformed
checkpoint state, all receipt event variants, duplicate/conflicting/gapped receipts, batching,
ACK retirement, split identity, and overflow.

The production network client now hydrates each validated protocol checkpoint into a complete
Dots World, selects `InteractionClosure`, and advances that scope through the engine timeline
with retained local commands and explicit held remote-movement assumptions. Authority is installed
and the unacknowledged suffix replayed in scratch state before the replicated view, timeline, and
presentation projection commit together. Retained local commands remain immutable, while the
engine's transactional stimulus-refresh hook replaces held remote-movement assumptions with the
newest authoritative values before replay; refreshed history commits only if the entire replay
succeeds. The closure horizon follows the actual retained suffix with a five-tick operating
floor; the fixed 256-entry ring is only a recovery bound. Before each input, a fresh closure check
rebases only when the existing causal membership/subscriptions do not cover the new requirement.
Safe supersets are retained across smaller ACK-driven horizons. A changed closure rebuilds from
the newest authority under a new scope epoch because older frames did not record causes for newly
admitted entities.

Graphical input now submits an edge-triggered split on Space. Predicted topology, mass, launch,
food, absorption, merge, and owner state are rendered from the predicted interaction island.
Outside it, Playing clients advance only newest-authority movement and launch for at most six
ticks/200 ms and then hold; Spectating clients retain Feature 12 interpolation-and-hold.
Predicted children retain their `PredictionKey` across replay and through an authoritative
entity-ID remap. Predicted removal of the final local piece does not enter Spectating or stop
input capture; only the confirmed replicated session can do that. The client converts new
authority receipts into confirmed timeline events and ACKs their contiguous published sequence.
Remote interpolation endpoint ghosts remain visible for in-scope players when enabled so the
predicted and authoritative presentation layers can still be compared. Local and comparable
same-head remote corrections emit entity-specific runtime records into a configurable bounded
presentation history. The history keeps the magenta pre-correction hue stable, fades opacity
over two seconds, and excludes ordinary movement between different predicted head ticks.

The step 6.5 audit remediation makes the fallback transition-closed as `OwnedGameplay`, separates
causal state membership from owner-participant event subscriptions, and routes every observable
initialize/advance/reconcile/authority-refresh/scope-rebase/hard-resync result through a bounded
post-commit `EventBatch`. Replicated World state no longer accumulates receipts. A separate
bounded inbox tracks accepted, published, and server-retired frontiers, rejects sequence gaps,
conflicting retransmissions, and live stable-key reuse, and ACKs only after a batch is queued.
Pre-welcome receipts remain pending; terminal Spectating receipts publish before prediction is
cleared. Same-tick authority refinement and explicit hard resync preserve their distinct history
semantics.

A follow-up command-frontier audit closed the predicted-elimination ACK gap. Dots now tracks and
displays sent, authoritative-ACK, timeline-submitted, and deferred-outer-input frontiers
separately. If coherent authority acknowledges a command that was sent and retained while the
predicted owner was absent, the client selects the engine's explicit hard-resync exception and
rolls any newer retained commands forward. Fatal timeline operation failures log all frontiers,
and the regression test covers authority acknowledging inside a multi-input deferred range with
an unacknowledged suffix.

### Step 7 Decision Record

Step 7 installs one Dots-owned rollback-aware presentation adapter in both offline and networked
composition roots. It does not add game presentation policy to `MyCore::Rollback`. The adapter
consumes the engine's post-commit `EventBatch`, owns a session-lifetime
`StaticConsequenceRouter`, and projects persistent state and transient cues into Dots
presentation data.

The concrete policy demonstration is:

| Mechanic | Dots presentation | Delivery |
|---|---|---|
| Movement | Fixed-tick avatar interpolation and an eight-sample/300 ms motion trail | State-derived |
| Split | 180 ms expanding flash | `PredictOnce` |
| Split | Child launch ring/trail for up to one second | `PredictCancelable` |
| Food consumption | 250 ms food-pop group | `PredictCancelable` |
| Player absorption | 150 ms immediate consume flash | `PredictOnce` |
| Player absorption | 300 ms victim-collapse pulse | `PredictCancelable` |
| Confirmed absorption | 1.5 second kill/defeat HUD banner and monotonic stinger hook | `ConfirmOnce` |
| Merge | Smoothed survivor geometry and a 100 ms consumed-piece fade | State-derived |

Cancelable split and absorption cues fade for 100 ms on rejection; a canceled food pop fades for
80 ms. Confirmation updates the active token and any predicted-child entity-ID association
without restarting it. Handler failure is counted and logged but is not retried and does not fail
the authoritative session. The router survives respawn and lives until the client session ends.
Hard resync clears presentation residuals and trails but not consequence tombstones.

Simulation events carry the occurrence geometry required after topology has changed:
`FoodConsumed` carries the food position, `PlayerAbsorbed` carries absorber and victim positions,
and `PlayerSplit` carries its origin and initial launch velocity. Stable keys do not change.
Protocol v4 receipt encoding includes this geometry without another version bump.

Network presentation owns persistent semantic tracks keyed by `PredictionKey` when present and
by entity ID otherwise. The selected source order while Playing is predicted closure, latest
snapshot extrapolation, then delayed interpolation fallback. Predicted fixed-tick samples use the
client accumulator alpha. Same-tick correction, predicted/remote source handoff, and structural
replacement preserve the prior visual pose and decay only their presentation offset over 100 ms.

Outside the prediction closure, presentation advances only known owner movement and per-entity
launch velocity from the newest accepted snapshot. It executes the same Dots kinematic step for
at most six ticks/200 ms and then holds. It never runs cohesion, collision, food consumption,
absorption, split, merge, closure construction, or checkpoint logic. Spectators continue to use
Feature 12 delayed interpolation. The debug-selectable remote modes are `Extrapolated` (default),
`Interpolated`, and `Comparison`; comparison renders extrapolation normally and overlays the
delayed interpolated position.

The confirmed-only cue uses a noninteractive Dear ImGui HUD banner because Dots has no production
font or audio backend. The ImGui frame may therefore render the HUD even when debug panes are
disabled, but the banner never captures input. Its monotonic stinger sequence is the future audio
hook; Feature 14 does not add audio or a general text renderer.

Step 7 also adds game-neutral per-handler/policy dispatch statistics to the consequence report.
The production Dots adapter exercises every row above in both offline and networked composition
roots, including a noninteractive confirmed-only HUD banner and a monotonic future-audio hook.
Adaptive command timing, complete fault controls, workload measurement, router tombstone pruning,
and any multi-frame replay scheduler remain step 8.

## Test Plan

Engine tests with a small deterministic model cover:

- Matching/mismatching authority, long replay suffixes, ACK trimming, stale authority, scope
  rebase, history exhaustion, hard resync, refreshed derived stimuli, refresh failure, and no
  partial commit.
- Repeated replay of one key invokes `PredictOnce` once.
- Retraction cancels `PredictCancelable` once; revision updates its existing token; confirmation
  does not duplicate it.
- `ConfirmOnce` ignores prediction/retraction and fires once across repeated receipts.
- Multiple handlers with different policies subscribe to the same event type.
- Conflicting event identities fail without invoking handlers.
- Ledger retirement bounds storage without allowing an old retained event to fire again.

Dots tests cover:

- Checkpoint round-trip and deterministic replay for movement, food, absorption, split, and merge.
- Closure expansion through ownership/cooldown, mechanic/global dependencies, movement, growth,
  split reach, and recursive player interaction.
- Owner-local cooldown correction without spatial expansion, immutable-rule incompatibility,
  missing causal-state fallback, and full-world oracle agreement.
- Split order/cap/mass/cooldown, launch/decay, merge eligibility/cohesion, mass conservation, and
  last-piece defeat.
- Predicted spawn match/reject/authority-only/ambiguity and presentation remapping.
- Every consequence-matrix row under repeated rollback and rejection.
- Receipt ACK, loss, duplication, reordering, conflict, and capacity behavior.

Integration scenarios use two clients at 100--200 ms latency with jitter/loss and verify eventual
structural convergence, responsive predicted eat/split, no duplicate consequence delivery,
confirmed session lifecycle, and coherent debug layers.

Performance workloads record 10, 100, 500, and 1,000 entities at 100, 200, and 400 ms RTT without
making wall-clock thresholds flaky unit-test assertions.

## Exit Criteria

- The client runs shared Dots gameplay inside a provably closed predicted island.
- Authoritative correction restores complete state and rolls retained stimuli forward to the
  previous prediction head atomically.
- Movement, food, absorption, split, and merge converge structurally.
- Dots exercises every generic consequence policy and repeated replay does not duplicate
  one-shot feedback.
- Outside-closure extrapolation remains presentation-only and bounded.
- Durable session state and confirmed consequences never derive from speculation alone.
- `MyCore::Rollback` contains no Dots, protocol, transport, rendering, or audio policy.
- Debugging clearly distinguishes authoritative, predicted, interpolated/extrapolated, and
  presentation state.
- Metrics make the conditional multi-frame spike decision explicit.
