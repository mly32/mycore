# MyCore Rollback Library Guide

`MyCore::Rollback` is a synchronous, game-neutral checkpoint/replay timeline. A game supplies
deterministic state operations and typed values; the library owns bounded history, transactional
reconciliation, event lifecycle comparison, and optional consequence deduplication.

Use this guide when integrating a new game. The
[rollback prediction design](rollback_prediction_design.md) records the broader Dots design and
rationale; it is not the onboarding guide for the engine library.

## What the library does

The library provides:

- A typed `Timeline<Model>` with initialization, predicted advance, authoritative
  reconciliation, scope rebase, and hard resync.
- Bounded history of sampled commands, explicit per-frame assumptions, checkpoints, digests,
  and event journals.
- Scratch restore/replay followed by one atomic commit.
- Typed state differences and diagnostic digests supplied by the game model.
- Stable event transitions: first predicted, revised, retracted, confirmed, and
  authority-only.
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
                             Commit + consequence routing
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

## Implement a rollback model

A model is a copyable policy value with the following associated types:

| Type | Contract |
|---|---|
| `State` | Mutable deterministic simulation state. Must be movable. |
| `Checkpoint` | Complete owning replay state with exact equality. Must be copyable. |
| `Stimulus` | Causes for one predicted tick. Sampled command fields are immutable; explicitly identified authority-derived assumption fields may be transactionally refreshed. Must be copyable. |
| `Scope` | Game-defined prediction membership/policy for one scope epoch. Must be copyable. |
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

## Choose the correct timeline operation

| Operation | Authority requirement | History behavior | Use |
|---|---|---|---|
| `initialize(frame, scope)` | First valid frame and valid scope epoch | Starts empty | Begin a timeline. |
| `advance(sequence, stimulus)` | None | Appends one predicted frame | Predict exactly one tick. |
| `reconcile(frame)` | Authority tick strictly newer than the last authoritative tick; same scope epoch | Drops acknowledged frames and replays the suffix | Normal server correction. |
| `reconcile_with_stimulus_refresh(frame, refresh)` | Same as `reconcile` | Drops acknowledged frames, refreshes each retained stimulus in scratch, and replays it | New authority revises derived external assumptions but not sampled player commands. |
| `rebase_scope(frame, scope)` | Scope epoch strictly increases; authority tick is the same or newer | Replays retained stimuli under the new scope | Membership, horizon, or mechanic-policy change. |
| `rebase_scope_with_stimulus_refresh(frame, scope, refresh)` | Same as `rebase_scope` | Refreshes and replays retained stimuli under the new scope | A new scope changes derived per-frame assumptions. |
| `hard_resync(frame, scope)` | Authority tick is the same or newer; scope epoch cannot regress | Clears retained history | Recover when replay cannot safely continue. |

A same-tick `rebase_scope` must use the exact current authoritative base checkpoint.
`hard_resync` may replace that checkpoint. Scope epochs describe incompatible prediction
membership; they are not simulation ticks.

After any successful call, read the immutable committed state through `timeline.state()`.
Do not cache its pointer across later timeline mutations. `Commit::state_diff` describes the
change from the previously committed predicted state to the new committed state.

Useful commit fields include:

- `kind`, authoritative/predicted ticks, acknowledgement, and replayed-frame count.
- The game-defined typed state difference.
- Authoritative, predicted, and prior-at-authority diagnostic digests.
- Event changes for consequence/presentation observers.

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
non-rewindable. Current `PredictOnce` and `ConfirmOnce` tombstones live for the router session;
`retired_event_keys` alone does not prove they are safe to erase. A canceled
`PredictCancelable` entry is erased and may be activated again if that semantic key legitimately
returns.

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

1. Checkpoint round-trip is exact and rebuilds all derived indexes.
2. Replaying the same checkpoint and stimuli produces identical checkpoints and event journals.
3. Every external cause is checkpoint state or a stimulus field; sampled player intent is
   immutable and every refreshable authority-derived field is explicitly identified and tested.
4. Every enabled mechanic has a causally closed scope or an explicit safe fallback.
5. Authority hydration validates schema, tick, rules, IDs, scope, acknowledgement, and receipts
   before calling the timeline.
6. Event keys survive rollback and never identify two semantic occurrences.
7. Continuous presentation comes from committed state; one-shot consequences use an explicit
   policy.
8. Every failure path preserves the last committed state and chooses a documented recovery.
9. History capacity covers the intended replay window at an acceptable checkpoint memory cost.
10. Tests cover multi-frame correction, structural restoration, event revision/retraction,
    acknowledgement trimming, scope rebase, capacity exhaustion, and hard resync.

The complete minimal model and consequence examples are in
[`tests/mycore_rollback_tests.cpp`](../tests/mycore_rollback_tests.cpp). The first production
adapter is
[`games/dots/prediction`](../games/dots/prediction/), with its game-specific design in the
[rollback prediction design](rollback_prediction_design.md). Its first network composition is
[`games/dots/client_runtime`](../games/dots/client_runtime/): it demonstrates verified
checkpoint hydration, interaction-closure selection, retained input/remote assumptions, atomic
authority reconciliation, authority-derived remote-assumption refresh, identity remapping, and
a separate confirmed session lifecycle.
