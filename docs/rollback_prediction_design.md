# Rollback Prediction Design

This document is the canonical design contract for the game-neutral rollback mechanism and its
first Dots integration. Feature plans define delivery order, and
`debugging_and_observability.md` defines operator-facing output.

## Status and Scope

- **Current:** Feature 11 predicts only the owned player's position by replaying unacknowledged
  movement.
- **Remote presentation baseline:** Feature 12 renders remote entities from delayed known
  snapshots and holds at the newest endpoint.
- **Authoritative lifecycle baseline:** Feature 13 implements deterministic absorption and
  confirmed Playing/Spectating, defeat, follow, and respawn state.
- **Committed Feature 14 direction:** add a statically typed `MyCore::Rollback` kernel and use
  Dots movement, food, absorption, split, and merge as its first complete vertical slice.
- **Deferred research:** multi-frame resimulation is considered only if measured same-frame
  replay crosses the thresholds in this document.

Feature 14 extracts only the game-neutral timeline, transaction, event-lifecycle, and consequence
delivery mechanisms. Dots continues to own its World, checkpoint schema, commands, mechanics,
prediction-set closure, event meanings, protocol, and presentation behavior.

## State and Authority Model

Keep these views separate:

| State | Meaning | Owner |
|---|---|---|
| Authoritative World | Complete gameplay truth after one server tick. | Server |
| Latest authoritative view | Newest validated server checkpoint known to the client; historical by receipt time. | Client copy |
| Predicted World | An authoritative checkpoint advanced through retained replay stimuli inside a closed prediction set. | Client |
| Interpolated comparison | Feature 12 delayed sampling between known authoritative snapshots. | Client presentation |
| Presentation World | State actually drawn after correction smoothing and optional bounded remote extrapolation. | Client presentation |
| Consequence ledger | Non-rewindable record of which external consequences were exposed, canceled, or confirmed. | Client presentation/runtime |

Prediction never grants authority. Simulation corrects immediately to the replay result;
presentation may smooth the visible correction but never feeds smoothed or extrapolated values
back into simulation.

## Ownership and Dependencies

Feature 14 adds:

- `MyCore::Rollback` under `engine/rollback`, depending only on `MyCore::Core` and
  `MyCore::Time`. It contains no Dots, protocol, transport, rendering, audio, or SDL types.
- `Dots::Prediction` under `games/dots/prediction`, depending on the engine kernel and Dots
  simulation value types. It defines the Dots rollback model, scope closure, event identities,
  diffs, and checkpoint adapter.
- Complete checkpoint and atomic-tick support in `Dots::Simulation`, still independent of
  networking and presentation.
- Composition in the Dots client runtime and presentation targets. The server remains headless
  and never runs client rollback.

The engine API is template/concept based. Do not introduce a virtual mechanic registry, a
cross-game event enumeration, or a serialized engine checkpoint format.

## Engine Rollback Contracts

The public engine vocabulary is:

```cpp
namespace mycore::rollback {

struct HistorySettings {
    std::size_t capacity{64};
};

enum class EventTransition {
    FirstPredicted,
    Revised,
    Retracted,
    Confirmed,
    AuthorityOnly,
};

enum class ConsequencePolicy {
    PredictOnce,
    PredictCancelable,
    ConfirmOnce,
};

template <class Model>
class Timeline;

template <class... Handlers>
class StaticConsequenceRouter;

} // namespace mycore::rollback
```

A `RollbackModel` supplies these game-owned types and operations:

- `State`, `Checkpoint`, `Stimulus`, `Event`, `EventKey`, `StateDiff`, and `StateDigest`.
- Validate and restore a checkpoint without consulting mutable runtime state.
- Capture a checkpoint from a committed state.
- Advance exactly one fixed tick from an immutable stimulus and return a deterministic event
  journal.
- Calculate a canonical diagnostic digest and a typed state difference.
- Return a stable key for each typed event and validate that one key never describes conflicting
  semantics.

The engine exposes these behavioral values:

- `AuthorityFrame<Model>`: validated checkpoint, authoritative tick, acknowledged command
  sequence, scope epoch, and authoritative event receipts.
- `FrameRecord<Model>`: predicted tick, immutable replay stimulus, checkpoint, digest, event
  journal, and scope epoch.
- `Commit<Model>`: immutable committed state, typed diff, replay coordinates, event transitions,
  correction data, and statistics.
- `ReconcileResult<Model>`: accepted commit or a typed rejection/recovery reason.

`Timeline<Model>` initializes from authority, advances one predicted tick, reconciles
transactionally, rebases a prediction scope, hard-resyncs, and exposes immutable committed and
debug views. Callers cannot access scratch state.

## Replay Stimuli and Rollforward

The kernel stores immutable **causes**, not callbacks or device state. Dots defines a per-tick
stimulus containing:

- The sampled local command and its sequence.
- Level-triggered movement assumed for relevant remote entities.
- Zero for unknown remote edge actions.
- Any explicit deterministic external fact required by that tick.

Keyboard, controller, transport, and wall-clock state are sampled before the stimulus enters the
timeline. Replay never polls them again.

Simulation events are outputs. They are regenerated by replay and must not be reloaded as though
they were input. A fact that causes simulation from outside the World must instead be explicit
checkpoint state or an explicit stimulus.

On an accepted authoritative frame at tick `T`, reconciliation:

1. Validates ordering, immutable rules, checkpoint completeness, command ACK, receipt sequences,
   scope epoch, and event identities.
2. Locates the old predicted record corresponding to `T` for diagnostics.
3. Restores the authoritative checkpoint into a scratch state.
4. Removes acknowledged stimuli and replays the retained suffix through the existing prediction
   head.
5. Regenerates scratch event journals and compares them with the previously committed journals.
6. Computes typed state and structural differences.
7. Stages history, event transitions, identity remaps, and metrics.
8. Atomically publishes the new committed state and a `Commit<Model>`.

“Current” means the latest prediction tick represented by retained stimuli, not estimated server
wall time. The initial implementation completes this transaction in the client frame that
accepts the snapshot. A future multi-frame implementation may extend the replay target with newly
sampled stimuli but may publish only after scratch catches that target.

## Checkpoint and Digest Contract

A Dots checkpoint contains every value required to restore and replay:

- Simulation tick and immutable `WorldRules`.
- Entity allocator state and stable/predicted identity associations.
- Sorted owners, players, and food with kind, ownership, position, mass, applied movement,
  launch velocity, and relevant input sequence.
- Split cooldown and per-piece merge eligibility ticks.
- Deterministic ordering state and any future gameplay RNG state before it is used.

Radius and spatial-grid contents are derived and rebuilt during restore. Confirmed network
session state remains outside the speculative World unless a specific field is a deterministic
input to predicted gameplay.

The implemented
[authoritative spawn search](plans/authoritative-spawn-search.md) derives its starting point from
current World state and adds no cursor. If it later gains a cursor, free list, or random stream,
that state enters the checkpoint before the revised search is used by replay. Join and respawn
placement remain server-only.

A checkpoint never contains renderer objects, particle handles, audio voices, UI state, logs,
sockets, wall-clock timestamps, or the consequence ledger.

Protocol version 4 carries a checkpoint schema identifier and a 64-bit FNV-1a digest over a
documented canonical sequence of primitive field bytes. The client hydrates and validates the
typed checkpoint, recomputes the digest, and rejects an incompatible frame before mutation.
Digest equality is diagnostic; typed validation and differences remain the correctness gate.

Feature 16 delta snapshots must first materialize the same coherent checkpoint contract. A
partial delta record is not a rollback checkpoint.

## Prediction Mechanics and Interaction Closure

Dots uses statically declared `PredictionMechanic` contracts. Each mechanic declares:

- State domains read and written.
- Mechanic dependencies and deterministic tick order.
- How it expands entity membership over a replay horizon.
- Typed simulation events and stable event-key rules.
- State-to-presentation projection requirements.

The initial mechanics are movement, food consumption, player absorption, and split/merge.

Prediction profiles are:

| Profile | Behavior | Use |
|---|---|---|
| `InteractionClosure` | Owned pieces plus the fixed-point closure of every entity and mechanic that can affect them during the replay horizon. | Default |
| `FullReplicated` | Every entity in the reconstructed replicated view. | Correctness oracle and workload benchmark |
| `OwnedMovement` | Owned movement only; contested mechanics remain authoritative. | Safe incomplete-state fallback |

Closure starts from owned pieces and expands through conservative swept bounds. Growth from food,
split launch reach, and recursively reachable player interactions enlarge the set. Before each
predicted step, an increased replay horizon may require a scope rebase from latest authority.

Every participant in a predicted interaction must share the same timeline. Excluded entities do
not collide with or otherwise affect the predicted island. Missing required replicated state
causes an atomic fallback to `OwnedMovement` with an `IncompleteClosure` reason; the client never
guesses the missing interaction.

Feature 15 AOI must replicate the conservative interaction margin required to construct this
closure.

## Dots Atomic Tick

Server, offline mode, and prediction execute the same owner-command tick:

1. Install level-triggered movement and consume edge actions once.
2. Resolve split.
3. Advance movement, launch decay, and post-deadline cohesion.
4. Resolve enemy absorption.
5. Resolve eligible same-owner merges.
6. Resolve food consumption.
7. Commit topology/resources and publish the deterministic tick journal.

No simulation callback performs presentation, logging, networking, analytics, or audio work.

## Split, Launch, and Merge Rules

Split is an edge action keyed by its input sequence. Immutable server defaults are:

- Split recast: 15 ticks.
- Merge delay: 150 ticks.
- Maximum pieces per owner: 8.
- Minimum eligible parent mass: 16.
- Child launch speed: 18 world units/second.
- Linear launch-speed decay: 18 world units/second squared.
- Post-deadline cohesion speed: 3 world units/second.

Eligible owned pieces are processed by stable identity. Each divides its mass evenly, creates a
child with `PredictionKey{owner, input sequence, ordinal}`, selects current movement, last
non-zero movement, then positive X as launch direction, and applies cooldown/deadline state. A
rejected split still consumes and ACKs the command.

After merge eligibility, pieces cohere toward their mass-weighted centroid. Eligible touching
same-owner pieces merge in stable order, conserve mass, and use mass-weighted position and
velocity. Enemy absorption retains Feature 13 ordering. Only authority confirms that the final
piece was lost and changes the network session to Spectating.

## Event Journals and Stable Identity

Dots uses a typed event variant for `SplitOccurred`, `FoodConsumed`, `PlayerAbsorbed`, and
`PiecesMerged`. Stable keys are explicit value variants:

- Split: owner, input sequence, and child ordinal.
- Food consumption: the stable food entity identity.
- Player absorption: the stable victim entity identity.
- Merge: the stable identities of both consumed pieces in sorted order.

Keys identify semantic occurrences across repeated replay and should not use the replay attempt
or corrected tick when the same occurrence can move between ticks. A key collision with
conflicting event type or causal identity is a correctness failure and triggers hard resync.

Predicted entity handles are never server authority. A matching `PredictionKey` remaps the
presentation association without creating a duplicate cue. Rejection removes the predicted
entity and retracts its event; an authority-only spawn creates normally; an ambiguous match hard
resyncs.

## Consequence Delivery

Event journals remain rewindable simulation output. A `StaticConsequenceRouter` consumes only
successful commits and owns a non-rewindable ledger keyed by `(handler, EventKey)`.

Policy is declared by each typed handler, not by the event type. One event may feed multiple
handlers with different policies:

- `PredictOnce`: invoke `on_first` for the first predicted or authority-only occurrence. The
  ledger suppresses that handler for the same key during later replay, confirmation, retraction,
  or reappearance. Rejected predictions may leave one brief false-positive cue.
- `PredictCancelable`: invoke `on_predict`, then `on_revise`, `on_cancel`, or `on_confirm` for a
  keyed handler token. Activation is idempotent while active; a real reactivation after
  cancellation is a new lifecycle transition, so one-shot sounds must not use this policy.
- `ConfirmOnce`: invoke `on_confirmed` only after an authoritative receipt. Repeated snapshots
  and prior prediction cannot invoke it twice.

State-derived presentation is not a consequence policy. A commit observer projects transforms,
sizes, loops, and other persistent visuals from committed state every frame.

The router stages its own ledger changes from a successful commit before invoking handlers.
Handler failure is reported but is not automatically retried, preserving at-most-once rather
than pretending to provide external exactly-once delivery.

The timeline reports when an event key is no longer reachable from retained replay stimuli.
Routers may then prune inactive predicted entries; stable game keys must never be reused.
Confirmed delivery is additionally guarded by the monotonic authority-receipt watermark. This
bounds bookkeeping while preserving at-most-once delivery across every legal replay in the
connected session.

The router does not claim persistent exactly-once behavior across a process crash, and it cannot
erase audio, haptics, or pixels the player already perceived. Cancelable handlers stop or fade
their remaining presentation.

### Dots consequence examples

| Mechanic | Immediate predicted result | Consequence example | Delivery |
|---|---|---|---|
| Movement | Position and movement | Avatar transform and motion trail | State-derived commit projection |
| Split | Topology, mass, launch, cooldown | Short one-shot split flash; future sound uses the same handler shape | `PredictOnce` |
| Split | Same predicted split | Child launch ring/trail | `PredictCancelable` |
| Food consumption | Food removal and mass gain | Keyed food-pop particles | `PredictCancelable` |
| Player absorption | Victim removal and mass transfer | Immediate consume flash; future crunch sound uses the same path | `PredictOnce` |
| Player absorption | Same predicted absorption | Victim collapse/consume pulse | `PredictCancelable` |
| Confirmed absorption/defeat | Server-confirmed durable result | Kill/defeat banner and stinger hook | `ConfirmOnce` |
| Merge | Topology and combined mass | Blob geometry and motion | State-derived commit projection |

Dots currently has no audio backend. Feature 14 demonstrates policies through lightweight
Dots-owned visual cues and deterministic test handlers; it does not introduce a speculative
engine audio subsystem.

## Authoritative Event Receipts

World state remains the source of gameplay truth, but transient confirmed consequences require a
loss-tolerant receipt:

- The server assigns a monotonically increasing per-session `AuthorityReceiptSequence`.
- Each receipt contains server tick, typed event key, and the minimum confirmed payload needed by
  a consequence handler.
- Full snapshots repeat bounded batches of unacknowledged receipts.
- Input packets acknowledge the highest contiguous receipt sequence.
- The server retains up to 256 receipts and sends at most 16 per snapshot. Reaching the bound is
  an explicit session error; receipts are never silently dropped.
- Identical duplicates are ignored, conflicting duplicates reject the frame, and a receipt is
  delivered to the consequence router only with an accepted authority commit at or beyond its
  server tick.

Repeated Feature 13 session fields remain authoritative state. Receipts control one-time
consequence delivery and do not replace those state fields.

## Local Elimination and Durable State

Predicted absorption can immediately remove the local final piece from the predicted World.
Presentation retains a pending-elimination camera/control proxy and continues capturing input.
Rollback restores the piece and cancels the proxy. Only confirmed authority enters Spectating,
selects a follow target, enables respawn eligibility, or exposes other durable session results.

Respawn placement remains server-only. Scoring, achievements, and a general kill-feed system are
outside Feature 14, but future handlers for them must use `ConfirmOnce`.

## Remote Prediction and Presentation

Inside the interaction closure, Dots steps complete shared mechanics. Unknown remote movement
holds its newest replicated level vector; unknown edge actions are zero. The source snapshot and
held range are stored with each frame.

Outside the closure, presentation may advance known movement and launch vectors without
collision or gameplay logic for at most six ticks/200 ms, then holds. This visual extrapolation:

- Never enters checkpoints, closure construction, collision, or future replay.
- Smooths when newer authority or closure entry replaces it.
- Coexists with Feature 12 delayed interpolation as spectator mode, fallback, and A/B comparison.

Extrapolation makes presentation look newer; it is not more authoritative.

## Adaptive Command Buffer

The server remains fixed at 30 Hz and consumes at most one ordered command per client each tick.
Feature 14 targets two queued commands and prefills two neutral commands when prediction becomes
ready.

Accepted snapshots update a queue-depth EWMA with `alpha = 1/8`. Depth 1.5 through 2.5 is a
deadband. Outside it:

```text
rate scale = clamp(1 + 0.025 * (2 - smoothed depth), 0.95, 1.05)
```

Only client command/prediction cadence changes. Server rate, gameplay deadlines, and session wall
time do not. Empty queues hold level movement but never repeat edge actions.

## Same-Frame Replay and Deferred Multi-Frame Work

History retains 64 ticks, approximately 2.13 seconds at 30 Hz. Missing history, capacity
exhaustion, incompatible checkpoint/scope, or ambiguous identity causes a hard resync to newest
validated authority.

Replay has a 2 ms warning, not a cutoff. Duration alone never publishes incorrect partial state.
First evaluate simulation optimization, closure size, and reconciliation frequency.

Record replay ticks, predicted entities, checkpoint bytes, structural changes, p50/p95/p99/max
duration, client-frame impact, correction/event transitions, and impairment grouping at 10, 100,
500, and 1,000 entities and 100, 200, and 400 ms RTT.

Create the separate `spike/multi-frame-resimulation` plan only if replay exceeds 4 ms at p99 or
causes rollback-attributable frame overruns in more than 1% of reconciliations in the target
200 ms scenario after cheaper mitigations. A future scheduler must preserve an immutable
baseline, accept new commands, supersede work for newer authority, atomically swap only after
catch-up, and hard-resync before history exhaustion.

## Debug and Metrics Contract

Feature 14 exposes:

- Prediction profile, scope epoch, included mechanics/state domains, closure size, and incomplete
  closure fallback.
- Authoritative checkpoint/tick/digest, corresponding predicted digest, predicted head, command
  ACK, and replay range.
- History occupancy, checkpoint bytes, replay duration distribution, and hard-resync reason.
- Typed continuous and structural differences.
- Predicted spawn pending/matched/rejected/authority-only/ambiguous counts.
- Event keys and `FirstPredicted`, `Revised`, `Retracted`, `Confirmed`, and `AuthorityOnly`
  transition counts.
- Per-policy delivered, suppressed, updated, canceled, and confirmed consequence counts.
- Receipt queue/ACK depth and duplicate/conflict counts.
- Authoritative, predicted, interpolated/extrapolated, pre-correction, and presentation layers.

The canonical labels and troubleshooting rules remain in `debugging_and_observability.md`.

## Research Basis

- [Psyonix: Rocket League physics and networking](https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf)
  describes client prediction history and whole-physics rewind/catch-up.
- [Unity: prediction details](https://docs.unity.cn/Packages/com.unity.netcode%401.5/manual/prediction-details.html)
  documents the incorrect interactions caused by rolling back one participant while freezing
  another.
- [Unreal: networked physics](https://dev.epicgames.com/documentation/unreal-engine/networked-physics-overview?lang=en-US)
  separates state history/resimulation from render interpolation and smoothing.
- [GGPO](https://github.com/pond3r/ggpo) demonstrates a game-supplied save/load/advance contract
  behind a reusable rollback mechanism.
