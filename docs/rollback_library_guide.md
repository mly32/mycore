# MyCore Rollback Library Guide

`MyCore::Rollback` is a synchronous, game-neutral checkpoint/replay timeline. A game supplies
deterministic state operations and typed values; the library owns bounded history, transactional
reconciliation, event lifecycle comparison, and optional consequence deduplication.

Use this guide when integrating a new game. The
[rollback prediction design](rollback_prediction_design.md) records the broader Dots design and
rationale; it is not the onboarding guide for the engine library.

## What the library does

The library provides:

- A typed `Timeline<Model>` with initialization, predicted advance, newer-tick reconciliation,
  same-tick authority refresh, scope rebase, and hard resync.
- Bounded history of sampled commands, explicit per-frame assumptions, checkpoints, digests,
  and event journals.
- Scratch restore/replay followed by one atomic commit.
- Typed state differences and diagnostic digests supplied by the game model.
- Stable event transitions: first predicted, revised, retracted, confirmed, and
  authority-only.
- Movable `EventBatch<Model>` output for publishing only post-commit event transitions, including
  an authority-only resolver for clients without an active predicted timeline.
- `StaticConsequenceRouter` policies for predicted one-shots, cancelable effects, and
  confirmed-only effects.

The library does not provide:

- A game-state schema, serializer, network protocol, input sampler, physics system, or
  prediction-set policy.
- Determinism automatically. The game still owns complete checkpoints and fixed-step rules.
- Interpolation, correction smoothing, render objects, audio objects, or presentation state.
- A background or multi-frame replay scheduler. Calls are synchronous and caller-owned.
- Durable exactly-once delivery across a process restart.

The normal data flow is:

```text
validated authority checkpoint ──> Timeline::initialize/reconcile
                                           │
sampled command + assumptions ────> Timeline::advance
                                           │
                                           v
                                   committed game State
                                           │
                           Commit/EventBatch + consequence routing
                                           │
                                           v
                                      presentation
```

Prediction changes responsiveness, not authority. Only validated authoritative data enters an
`AuthorityFrame`. Authority does not have to arrive over a network: an offline composition or
test can promote a local checkpoint to the authoritative baseline while using the same timeline
contract.

## Add the dependency

Link the game-owned prediction adapter, not the simulation itself, to the engine target:

```cmake
target_link_libraries(
    your_game_prediction
    PUBLIC
        YourGame::Simulation
        MyCore::Rollback
)
```

Include the umbrella header:

```cpp
#include "mycore/rollback/rollback.hpp"
```

Keep the model adapter in game code. `engine/rollback` must not know game entities, mechanics,
protocol fields, or presentation types.

## Runtime ownership and call discipline

`Timeline`, its model value, and `StaticConsequenceRouter` are caller-owned synchronous objects.
They do not create worker threads and do not provide internal synchronization. A composition root
must give each instance one mutation owner, normally the fixed-step simulation thread. Concurrent
reads during a mutation are a data race unless the caller supplies an external synchronization
boundary.

Treat one timeline call as an indivisible transaction:

- Do not re-enter that timeline from `Model::restore`, `step`, `capture`, `digest`, `diff`,
  `event_key`, or a stimulus-refresh callback.
- Model and refresh operations must not call presentation, audio, analytics, transport, input
  devices, wall clocks, or other externally visible services.
- Consequence handlers run synchronously inside `StaticConsequenceRouter::consume`. Do not
  recursively consume another batch through the same router or mutate the timeline from a
  handler.
- Borrowed timeline pointers/references and router references/spans are available only from
  lvalues. Do not retain them across a non-const owner operation, owner move, or transfer to
  another thread. Copy the game-defined value needed by an asynchronous consumer.
- `Commit` and `EventBatch` are owning value outputs. Move those values across composition
  boundaries instead of sharing references into the timeline.

The timeline makes its own replay atomic. It cannot make a larger transaction involving a
protocol inbox, replicated-world cache, outer command ring, presentation queue, or session state
atomic. The composition root owns that boundary, as shown in
[Compose an authoritative update transaction](#compose-an-authoritative-update-transaction).

## Implement a rollback model

A model is a copyable policy value with the following associated types:

| Type | Contract |
|---|---|
| `State` | Mutable deterministic simulation state. Must be movable. |
| `Checkpoint` | Complete owning replay state with exact equality. Must be copyable. |
| `Stimulus` | Causes for one predicted tick. Sampled command fields are immutable; explicitly identified authority-derived assumption fields may be transactionally refreshed. Must be copyable. |
| `Scope` | Game-defined prediction membership/policy for one scope epoch. Must be copyable and equality-comparable. |
| `Event` | Deterministic simulation output. Use a `std::variant` when using the consequence router. |
| `EventKey` | Stable semantic occurrence identity with equality. Must be copyable. |
| `EventKeyHash` | Default-constructible hash callable for `EventKey`. |
| `StateDiff` | Game-defined difference from the previously committed state to the new state. |
| `StateDigest` | Copyable, equality-comparable diagnostic checkpoint digest. |
| `Error` | Typed restore/step failure detail. |

A game with no simulation events can use a simple equality-comparable placeholder `Event`,
always return an empty event vector, and omit `StaticConsequenceRouter`. The timeline does not
require the event type to be a `std::variant`; only the static router does.

The model supplies:

```cpp
class GameRollbackModel {
public:
    using State = game::World;
    using Checkpoint = game::WorldCheckpoint;
    using Stimulus = game::TickStimulus;
    using Scope = game::PredictionScope;
    using Event = game::SimulationEvent;
    using EventKey = game::SimulationEventKey;
    using EventKeyHash = game::SimulationEventKeyHash;
    using StateDiff = game::WorldDifference;
    using StateDigest = game::CheckpointDigest;
    using Error = game::PredictionError;

    [[nodiscard]] std::variant<State, Error>
    restore(const Checkpoint& checkpoint, const Scope& scope) const;

    [[nodiscard]] Checkpoint capture(const State& state, const Scope& scope) const;

    [[nodiscard]] std::variant<std::vector<Event>, Error>
    step(State& state, const Stimulus& stimulus, const Scope& scope) const;

    [[nodiscard]] StateDigest
    digest(const Checkpoint& checkpoint, const Scope& scope) const;

    [[nodiscard]] StateDiff
    diff(const State& previous, const State& current, const Scope& scope) const;

    [[nodiscard]] EventKey event_key(const Event& event) const;
};

static_assert(mycore::rollback::RollbackModel<GameRollbackModel>);
```

The operations have stronger semantic requirements than the C++ concept can express:

- `restore` validates the checkpoint and scope, rebuilds derived state, and publishes no
  partially restored state.
- `capture(restore(checkpoint))` returns the same canonical checkpoint.
- `step` advances exactly one fixed simulation tick and uses only `state`, `stimulus`, `scope`,
  and immutable rules reachable from them.
- `step` returns deterministic events but performs no audio, rendering, logging, analytics,
  networking, or other external side effects.
- `Stimulus` stores causes such as sampled commands and explicit remote assumptions. Sampled
  commands never change. An assumption derived from external authority may change only through
  the timeline's explicit replay-refresh transaction. A stimulus never stores callbacks,
  input-device references, wall-clock queries, or regenerated events.
- `Scope` is causally closed for every enabled predicted mechanic. Missing state must disable the
  mechanic, select a safe fallback scope, or reject the frame.
- Scope equality means exact membership and prediction policy equality. Reusing an epoch with an
  unequal scope is invalid even when an operation would otherwise discard history.
- If prediction can create entities, the game defines how those entities remain inside the
  scope before an authoritative entity ID exists. Use a unique causal spawn key, include the
  new entity's conservative interaction reach when building the scope, and reject unkeyed,
  duplicate, or unrelated additions.
- `digest` is diagnostic. Exact typed validation and state comparison remain the correctness
  mechanism.
- An `EventKey` identifies one semantic occurrence across replay. Do not include the replay
  attempt, and do not include a tick if the same occurrence may move to another tick.

The engine cannot inspect an opaque checkpoint. The game adapter must validate that an
`AuthorityFrame::tick` agrees with the checkpoint's logical tick, that rules/schema are
compatible, and that every required entity and external cause is present.

## Describe mechanics and prove causal closure

Prediction membership is a gameplay proof, not a distance setting. Before implementing a scope
builder, write down what each predicted mechanic reads, writes, receives from outside the
checkpoint, and can create or remove. A useful worksheet is:

| Mechanic | Reads | Writes | External causes | Required prediction closure | Events |
|---|---|---|---|---|---|
| Movement | Owner command, position, movement rules | Position, last movement | Sampled local command or recorded remote assumption | Controlled owner and every moved piece | Usually none |
| Consumption | Positions, radii, mass, consumable existence | Mass and entity existence | Recorded remote movement assumptions | Every entity and consumable that can interact during the replay horizon | Consumed/absorbed |
| Split | Mass, cooldown, piece count, allocator | Topology, mass, launch velocity, cooldown | Sampled split edge | Complete owner-local state plus every predicted child and its conservative reach | Split |
| Merge | Piece topology, positions, mass, merge deadlines | Topology, position, mass | None beyond retained movement causes | All pieces of the owner and entities that can affect them before merge | Merged |
| Global timer or rule | Rule version, phase, deadline | Enabled mechanics or outcome state | Validated authority fact when not derivable locally | Required global/non-spatial domain, even when no nearby entity represents it | Game-defined |

For each enabled mechanic:

1. Seed the scope from locally controlled state.
2. Add every entity, owner-local field, allocator, rule, timer, and authority fact the mechanic
   may read over the replay horizon.
3. Add state whose writes can affect any of those reads, including collision chains and
   topology changes.
4. Add predicted children by stable causal spawn identity and include their conservative future
   reach.
5. Repeat until membership reaches a fixed point.
6. If required state is unavailable, disable the affected mechanic, choose a documented safe
   fallback profile, or reject the authority frame.

Event subscription is separate from state closure. A simulation may need an entity in the
predicted state to preserve deterministic gameplay without publishing that entity's presentation
events to this client. Conversely, subscribing to an event does not provide the checkpoint state
required to predict its mechanic.

Test the worksheet as executable invariants. Removing any declared dependency should make scope
construction fail or select the safe fallback, while a full-replicated oracle should produce the
same scoped result for interactions wholly inside the proven closure.

## Determinism and portability

`MyCore::Rollback` guarantees transactional replay mechanics; it does not make arbitrary game
code deterministic. Define the compatibility envelope in which a checkpoint and stimulus stream
must replay identically. At minimum:

- Use one fixed tick duration and one documented mechanic order.
- Give unordered command input, spatial candidates, collisions, and simultaneous events stable
  sorting and tie-breaking before they mutate state.
- Store gameplay RNG state and allocation cursors in the checkpoint before randomness or entity
  identity depends on them. Never draw gameplay randomness from a process-global generator.
- Keep wall time, frame delta, input-device state, network arrival order, pointer values, and
  container hash order out of `step`.
- Version checkpoint schema, immutable rules, and any algorithm change that makes old authority
  incompatible. Reject an incompatible frame; a hard resync cannot make two different rulesets
  deterministic.
- Define canonical digest encoding explicitly: field order, integer width and byte order,
  optional/variant tags, floating-point representation, and sorted collection order. Never hash
  object padding or native container layout.

IEEE floating-point types alone do not guarantee bit-identical results across every compiler,
optimization mode, instruction set, or math-library implementation. If cross-platform
bit-identical replay is required, constrain the supported toolchain and operations, quantize
authoritative state, or use an appropriate fixed-point representation. Otherwise, treat normal
authority correction as the convergence mechanism and use replay digests to measure how often
platform differences occur. A digest is diagnostic evidence, not proof of semantic equality or
a security boundary.

Keep recorded replay fixtures that run in Debug and optimized builds on every supported platform.
They should compare canonical checkpoints, ordered event journals, and typed differences—not only
the final digest. A new platform or compiler is supported for prediction only after those
fixtures pass.

## Drive the timeline

Construct a timeline with a history capacity chosen from the maximum replay window and measured
checkpoint cost:

```cpp
using Timeline = mycore::rollback::Timeline<GameRollbackModel>;
using Commit = mycore::rollback::Commit<GameRollbackModel>;
using Failure = mycore::rollback::TimelineFailure<GameRollbackModel>;

Timeline timeline{GameRollbackModel{}, {.capacity = 64}};
```

Initialize it from the first validated authoritative checkpoint:

```cpp
mycore::rollback::AuthorityFrame<GameRollbackModel> authority{
    .tick = checkpoint.tick,
    .acknowledged_through = std::nullopt,
    .scope_epoch = mycore::rollback::ScopeEpoch{1},
    .checkpoint = checkpoint,
    .events = {},
};

auto result = timeline.initialize(authority, scope);
if (const auto* failure = std::get_if<Failure>(&result)) {
    handle_failure(*failure);
    return;
}
observe_commit(std::get<Commit>(result), *timeline.state());
```

For each locally predicted tick, sample all mutable inputs first and submit one explicit
stimulus with a strictly increasing command sequence:

```cpp
auto result = timeline.advance(
    mycore::rollback::CommandSequence{next_sequence},
    game::TickStimulus{
        .local_command = sampled_command,
        .remote_assumptions = recorded_assumptions,
    });

if (const auto* failure = std::get_if<Failure>(&result)) {
    handle_failure(*failure);
    return;
}
observe_commit(std::get<Commit>(result), *timeline.state());
```

Sequence zero is valid. `StrongId::invalid()` is not. A command acknowledgement is cumulative:
`acknowledged_through = N` means every retained stimulus with sequence `<= N` can be removed
before replay. An absent acknowledgement means none of the retained stimuli are acknowledged.
Acknowledgements cannot regress, disappear after one has been accepted, or exceed the last
submitted sequence.

When newer authority arrives:

```cpp
auto result = timeline.reconcile(validated_authority_frame);
```

On success, the timeline:

1. Restores authority into scratch state.
2. Removes acknowledged stimuli.
3. Replays the remaining stimuli through the old prediction head.
4. Regenerates event journals and calculates transitions.
5. Atomically replaces state and history.

No scratch state is observable. A failed call leaves the committed state and history unchanged;
only failure statistics advance.

Normal `reconcile` replays each retained `Stimulus` exactly as stored. Use
`reconcile_with_stimulus_refresh` when a stimulus combines an immutable local command with a
derived assumption that newer authority supersedes:

```cpp
auto result = timeline.reconcile_with_stimulus_refresh(
    validated_authority_frame,
    [&latest_authority](mycore::rollback::CommandSequence sequence,
                        const game::TickStimulus& previous,
                        const game::World& scratch_before_step,
                        const game::PredictionScope& scope)
        -> std::variant<game::TickStimulus, game::PredictionError> {
        auto refreshed = previous; // preserves sampled local commands and edge actions
        refreshed.remote_assumptions =
            derive_remote_assumptions(latest_authority, scratch_before_step, scope);
        return refreshed;
    });
```

The callback runs in sequence order after authority restore and before each retained step. It
receives read-only scratch state as it existed before that step and returns the complete
stimulus that should be replayed and retained. It must be deterministic and side-effect free.
Do not use it to resample input or rewrite player intent. If it returns a model error, the call
fails with `StimulusRefreshFailed` and publishes none of the scratch state or refreshed history.
`rebase_scope_with_stimulus_refresh` provides the same contract when installing a higher scope
epoch.

The [Feature 14 prediction-stutter postmortem](feature14_prediction_stutter_postmortem.md)
documents the production failure that motivated this API, including why exact replay remains
correct for sampled local commands but not for a superseded authority-derived assumption.

## Choose the correct timeline operation

| Operation | Authority requirement | History behavior | Use |
|---|---|---|---|
| `initialize(frame, scope)` | First valid frame and valid scope epoch | Starts empty | Begin a timeline. |
| `advance(sequence, stimulus)` | None | Appends one predicted frame | Predict exactly one tick. |
| `reconcile(frame)` | Authority tick strictly newer than the last authoritative tick; same scope epoch | Drops acknowledged frames and replays the suffix | Normal server correction. |
| `reconcile_with_stimulus_refresh(frame, refresh)` | Same as `reconcile` | Drops acknowledged frames, refreshes each retained stimulus in scratch, and replays it | New authority revises derived external assumptions but not sampled player commands. |
| `refresh_authority(frame)` | Authority tick and scope epoch equal the current authoritative base | Replaces the validated same-tick base and replays retained history | Publish late authority events or a later validated view of the same tick without discarding future prediction. |
| `refresh_authority_with_stimulus_refresh(frame, refresh)` | Same as `refresh_authority` | Refreshes and replays retained stimuli from the replacement base | Same-tick authority also refines derived assumptions. |
| `rebase_scope(frame, scope)` | Scope epoch strictly increases; authority tick is the same or newer | Replays retained stimuli under the new scope | Membership, horizon, or mechanic-policy change. |
| `rebase_scope_with_stimulus_refresh(frame, scope, refresh)` | Same as `rebase_scope` | Refreshes and replays retained stimuli under the new scope | A new scope changes derived per-frame assumptions. |
| `hard_resync(frame, scope)` | Authority tick is the same or newer; scope epoch is newer or the scope equals the current scope at the same epoch | Clears retained history | Recover when replay cannot safely continue. |

A same-tick authority refresh or scope rebase may replace the base checkpoint because a
caller-validated later snapshot can refine the state projected into that scope. The caller must
first prove that the frame is newer in its own transport/snapshot ordering; the generic timeline
does not know that ordering. `hard_resync` may also replace the checkpoint, but an unequal scope
requires a strictly newer epoch. Scope epochs describe incompatible prediction membership; they
are not simulation ticks.

Normal reconcile/refresh/rebase calls reject an acknowledgement beyond the timeline's last
submitted command. `hard_resync` is the deliberate exception: after the caller independently
validates a coherent authority frame, it may advance the acknowledgement beyond timeline history
because the operation discards that history. This supports commands that an outer client buffered
while its predicted model temporarily could not advance, such as input retained after speculative
local elimination.

If a game has such an outer buffer, compare `frame.acknowledged_through` with
`timeline.last_submitted_sequence()` before selecting the operation. Use `hard_resync` only when
the game's session/protocol validation has already accepted the frame and the acknowledged
command is provably covered by that outer buffer. Then discard the acknowledged outer prefix and
submit the newer suffix normally. `timeline.acknowledged_through()` reports the authority already
installed in the timeline; it is not the same frontier as local submission.

After any successful call, read the immutable committed state through `timeline.state()`.
Do not cache its pointer across later timeline mutations. `Commit::state_diff` describes the
change from the previously committed predicted state to the new committed state.

Useful commit fields include:

- `kind`, authoritative/predicted ticks, acknowledgement, and replayed-frame count.
- The game-defined typed state difference.
- Authoritative, predicted, and prior-at-authority diagnostic digests.
- Event changes for consequence/presentation observers.

Move event output across composition boundaries with
`event_batch_from_commit(std::move(commit))`. Empty batches have no observable event lifecycle
and need not be queued. If authority events arrive before initialization or while no predicted
state exists, validate and convert them with `resolve_authority_only_events(model, events)`.
Both paths produce the same `EventBatch<Model>` contract consumed by
`StaticConsequenceRouter`.

## Compose an authoritative update transaction

The timeline validates and commits its own state atomically, but a networked client normally has
several other fallible objects that must agree with the same packet. Use candidate copies or
equivalent scratch builders so a late failure cannot install only half of an authoritative
update.

The complete ordering is:

```text
decode packet
  -> validate framing, schema, IDs, rules, snapshot order, ACK, and receipt syntax
  -> hydrate a candidate authoritative checkpoint/world
  -> apply receipts to a candidate receipt inbox
  -> build and validate the candidate prediction scope
  -> choose reconcile/refresh/rebase/hard-resync
  -> prove every bounded queue has capacity and advance candidate publication frontiers
  -> run the timeline's atomic replay after every outer failure point is cleared
  -> install world + inbox + durable session fields through non-failing moves
  -> enqueue the committed EventBatch
  -> route consequences and update presentation from committed state only
```

A simplified composition-root shape is:

```cpp
auto decoded = decode_and_validate(packet);
if (!decoded) {
    reject_packet(decoded.error());
    return;
}

auto candidate_world = replicated_world;
auto candidate_receipts = receipt_inbox;
auto candidate_batches = pending_event_batches;

auto authority = hydrate_authority(*decoded, candidate_world, candidate_receipts);
if (!authority) {
    reject_packet(authority.error());
    return;
}

auto scope = build_prediction_scope(authority->checkpoint, retained_commands);
if (!scope) {
    reject_packet(scope.error());
    return;
}

if (!candidate_receipts.mark_ready_for_publication(authority->events) ||
    !candidate_batches.can_push(1)) {
    reject_packet(capacity_or_receipt_error());
    return;
}

// This is the first live mutation. Everything after a successful replay is a
// prevalidated, non-failing move/install or bounded push.
auto result = apply_authority(timeline, *authority, *scope);
auto* commit = std::get_if<Commit>(&result);
if (commit == nullptr) {
    reject_packet(timeline_error(result));
    return;
}

candidate_batches.push_prevalidated(event_batch_from_commit(std::move(*commit)));
replicated_world = std::move(candidate_world);
receipt_inbox = std::move(candidate_receipts);
pending_event_batches = std::move(candidate_batches);
```

The names above are illustrative game/protocol adapters, not additional rollback-library APIs.
The important property is that all fallible outer validation and capacity work finishes before
the timeline call, the timeline itself commits atomically, and all remaining related installs
are non-failing. A game that cannot provide non-failing moves/installs must instead construct an
equivalent candidate timeline and swap the complete aggregate; that requires a copyable model or
a game-owned reconstruction path beyond the minimum `RollbackModel` concept.

After installation, every successful timeline mutation has exactly one event-publication path.
An empty batch may be discarded deliberately; a nonempty batch must not be silently lost. A
consequence-handler failure is reported after gameplay commit and is not a reason to rewind the
accepted authoritative state or retry an irreversible effect.

Local predicted ticks need a corresponding transaction boundary. Sample device input once,
assign its stable command sequence, retain the immutable command in the outer recovery buffer,
send it according to the game's transport policy, and call `advance` only under a documented
send-failure policy. Never resample the device during replay. The outer buffer, timeline
submission frontier, server acknowledgement, receipt publication frontier, and presentation
consumption frontier are distinct coordinates even when their numeric values happen to match.

## Simulation events and presentation consequences

Simulation events are rewindable outputs. External effects are not. Keep these paths separate:

- Rebuild transforms, geometry, mass displays, and other continuous presentation from the newly
  committed `State` and `StateDiff`.
- Send event transitions to a consequence router only after the timeline call commits.
- Never invoke presentation effects from `Model::step`; replay may call it many times.

Event transitions mean:

| Transition | Meaning |
|---|---|
| `FirstPredicted` | A stable key appeared in predicted history for the first time. |
| `Revised` | The same key survived replay with different event data. |
| `Retracted` | The occurrence disappeared or authority rejected it. |
| `Confirmed` | Authority confirmed an occurrence already present in predicted history. |
| `AuthorityOnly` | A confirmed occurrence was first observed through authority. |

Every event key must be unique across retained history. Reusing a key for two events is a model
correctness failure. A confirmed `AuthorityEvent` includes an event whose derived key matches its
declared key; a rejected authority event includes only the key. Identical repeated receipts are
safe, while conflicting receipts fail atomically.

### Consequence policies

One event type may have several handlers with different policies:

| Policy | Required handler API | Intended use |
|---|---|---|
| `PredictOnce` | `bool on_first(EventKey, const Event&)` | Immediate one-shot that cannot truly be undone, such as a short sound or flash. |
| `PredictCancelable` | `std::optional<Token> on_predict(...)` plus `bool on_revise(Token&, ...)`, `bool on_cancel(Token&, ...)`, and `bool on_confirm(Token&, ...)` | Long-lived or reversible particle, trail, animation, or voice. |
| `ConfirmOnce` | `bool on_confirmed(EventKey, const Event&)` | Durable result, banner, stinger, analytics, or other authority-only consequence. |

Each handler declares:

```cpp
using Event = game::SomeEvent;
static constexpr auto policy = mycore::rollback::ConsequencePolicy::PredictOnce;
```

The complete cancelable shape is:

```cpp
struct CancelableParticleHandler {
    using Event = game::SomeEvent;
    using Token = game::ParticleHandle;
    static constexpr auto policy =
        mycore::rollback::ConsequencePolicy::PredictCancelable;

    [[nodiscard]] std::optional<Token>
    on_predict(const GameRollbackModel::EventKey& key, const Event& event);
    [[nodiscard]] bool
    on_revise(Token& token, const GameRollbackModel::EventKey& key, const Event& event);
    [[nodiscard]] bool
    on_cancel(Token& token, const GameRollbackModel::EventKey& key, const Event& event);
    [[nodiscard]] bool
    on_confirm(Token& token, const GameRollbackModel::EventKey& key, const Event& event);
};
```

Create and consume a static router:

```cpp
mycore::rollback::StaticConsequenceRouter<
    GameRollbackModel,
    ImmediateFlashHandler,
    CancelableParticleHandler,
    ConfirmedBannerHandler>
    router{flash_handler, particle_handler, banner_handler};

const auto report = router.consume(commit);
```

Handler calls return success. The router reports failures but deliberately does not retry the
same occurrence: retrying an external side effect could duplicate it. Router ledger state is
non-rewindable. `retired_event_keys` proves that retained replay can no longer reproduce an
occurrence, but it does not prove that an external receipt cannot redeliver it. Add independently
derived keys to `EventBatch::externally_retired_keys`; the router prunes a key only after both
proofs arrive, in either order, and after any cancelable token is inactive. A canceled
`PredictCancelable` entry is erased during cancellation. Stable keys must not be reused after
external retirement because a later occurrence with that key violates the proof contract.

The router validates the complete owning batch before any handler, ledger, or statistic changes.
`FirstPredicted` and `AuthorityOnly` carry only `current`; `Revised` and `Confirmed` carry both
values with the same event variant alternative; `Retracted` carries only `previous`.
`ConsequenceDispatchReport::contract_failures` reports the change index and typed shape,
alternative, or valueless-event error. A contract failure is distinct from an ordinary handler
failure: no handler is called, and a production consumer should preserve committed gameplay for
diagnostics and terminate only the integration/session whose trusted batch was malformed.

`ConsequenceDispatchReport::handlers` gives the per-batch delta for every statically registered
handler, including its tuple index and declared policy. `router.handler_statistics()` exposes
the corresponding session-cumulative totals. Use these values to identify a specific handler;
aggregate `report.statistics` alone cannot distinguish two handlers that subscribe to the same
event type with different policies. `router.ledger_statistics()` reports both outstanding proof
sets, per-policy retained-key counts, live/inactive cancelable counts, and cumulative pruning.
Dots' production adapter in
`games/dots/presentation/` is the concrete example: state-derived movement/merge visuals remain
outside the router, while split, food, absorption, and confirmed HUD feedback use the three
policies.

## Failure and recovery

Inspect both `TimelineFailure::code` and its optional game-defined `model_error`.

| Failure | Typical interpretation and response |
|---|---|
| `NotInitialized`, `AlreadyInitialized` | Caller lifecycle bug. Do not continue prediction until composition is corrected. |
| `InvalidCommandSequence`, `NonMonotonicCommandSequence` | Input/history integration bug. Preserve committed state and repair the sequence source. |
| `HistoryExhausted` | No room for another prediction. Use the newest validated authority for a hard resync; do not silently discard an unacknowledged frame. |
| `StaleAuthority` | Ignore the old frame. |
| `InvalidAcknowledgement` | Protocol/session inconsistency. Reject the frame; hard-resync only from a separately validated coherent frame. |
| `IncompatibleScope` | Use a valid higher-epoch scope rebase or hard resync. |
| `IncompatibleAuthority` | Reject malformed frame metadata or an invalid same-tick rebase. |
| `DuplicateEventKey` | Game event-identity correctness bug. Do not expose consequences from the failed call. |
| `ConflictingAuthorityEvent` | Conflicting or malformed authority receipt; reject it as a protocol/identity error. |
| `ModelRestoreFailed` | Invalid checkpoint or scope according to the game adapter. |
| `StimulusRefreshFailed` | A derived-assumption refresh could not produce a complete valid retained stimulus. Preserve committed state and use the game-defined rebase or hard-resync fallback. |
| `ModelStepFailed` | Invalid retained stimulus or deterministic simulation failure. Preserve committed state and apply the game-defined fallback/recovery policy. |

Do not choose recovery based only on replay duration. The initial library completes replay
synchronously and atomically. If measured replay later requires time slicing, committed state
must remain visible until scratch catches the moving prediction head.

## Onboarding checklist

Before a new game enables prediction:

1. One thread or externally synchronized executor owns timeline and consequence-router mutation;
   callbacks are non-reentrant and borrowed state does not escape its lifetime.
2. Checkpoint round-trip is exact and rebuilds all derived indexes.
3. Replaying the same checkpoint and stimuli produces identical checkpoints and event journals
   in Debug and optimized builds on every supported prediction platform.
4. Every external cause is checkpoint state or a stimulus field; sampled player intent is
   immutable and every refreshable authority-derived field is explicitly identified and tested.
5. Every enabled mechanic has a causally closed scope or an explicit safe fallback. Before
   retaining an older superset scope, prove that the newest authority checkpoint still projects
   completely into it; owner membership can change even after that owner leaves the newly
   required closure.
6. Authority hydration validates schema, tick, rules, IDs, scope, acknowledgement, and receipts
   before calling the timeline, and the outer composition transaction cannot partially install
   related live objects.
7. Receipt streams distinguish semantic acceptance, event-batch publication, consequence
   delivery, network acknowledgement, and server-confirmed retirement.
8. Event keys survive rollback and never identify two semantic occurrences.
9. Continuous presentation comes from committed state; one-shot consequences use an explicit
   policy.
10. Every failure path preserves the last committed state and chooses a documented recovery.
11. History capacity covers the intended replay window at an acceptable checkpoint memory cost.
12. Tests cover multi-frame correction, structural restoration, event revision/retraction,
    acknowledgement trimming, scope rebase, capacity exhaustion, and hard resync.
13. Canonical checkpoint and event fixtures cover ordering, RNG/allocator state, schema/rule
    compatibility, and the intended cross-platform floating-point contract.
14. Deterministic state-machine tests check commit/frontier/history invariants and failure
    atomicity; a structured sanitizer-backed fuzzer repeats the same assertions over generated
    operation traces.

The complete minimal model and consequence examples are in
[`tests/mycore_rollback_tests.cpp`](../tests/mycore_rollback_tests.cpp). The first production
adapter is
[`games/dots/prediction`](../games/dots/prediction/), with its game-specific design in the
[rollback prediction design](rollback_prediction_design.md). Its first network composition is
[`games/dots/client_runtime`](../games/dots/client_runtime/): it demonstrates verified
checkpoint hydration, interaction-closure selection, retained input/remote assumptions, atomic
authority reconciliation and same-tick refresh, authority-derived remote-assumption refresh,
identity remapping, a bounded accepted/published/retired authority-receipt inbox, and a separate
confirmed session lifecycle.
