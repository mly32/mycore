# Feature 14 Remote Prediction Stutter Postmortem

## Status

Resolved on `feature/14`.

| Role | Commit |
|---|---|
| Regression introduced | `8135f1c Install complete client rollback timeline` |
| Initial prediction-scope correction | `8e599f2 Fix client prediction scope horizon` |
| Replay-provenance correction and final fix | `f249973 Stabilize remote rollback prediction` |

This document is a case study for prediction-system design and review. The canonical current
contracts remain in the [rollback library guide](rollback_library_guide.md), the
[rollback prediction design](rollback_prediction_design.md), and the
[server-authoritative networking guide](server_authoritative_networking_guide.md).

## Summary

After the complete Dots World was connected to the engine rollback timeline, remote bots visibly
jumped backward and forward. Remote interpolation endpoint diagnostics also disappeared for
many bots. The server simulation was not rolling bots backward. The client had admitted them to
its predicted interaction island and repeatedly rebuilt their predicted positions from a stale
remote-movement assumption.

Two independent mistakes combined:

1. The fixed 256-input history **capacity** was used as the prediction scope's causal
   **horizon**. This made an ordinary local session select a much larger interaction closure than
   its real unacknowledged replay suffix required.
2. A future stimulus sampled a remote owner's movement from the current **predicted** World.
   After rollback, that World still contained the historically replayed remote guess. The guess
   was therefore mislabeled as newest authority and propagated into later prediction.

The first mistake changed which system presented a bot. The second made the predicted system
reintroduce an obsolete direction after every newer snapshot. Together they looked like
continuous network misprediction.

## User-visible impact

- Bots jumped back toward authority and then moved forward along the obsolete predicted path.
- The problem began immediately or at the first remote direction change, depending on session
  timing.
- Remote endpoint/interpolation ghosts disappeared for bots admitted to prediction because the
  presentation composer correctly removes interpolated duplicates of predicted entities.
- Local movement could remain responsive, making the failure look remote-only.
- A one-bot reproduction was unreliable because the local player could spawn beside and be
  absorbed by that bot.

The missing endpoint ghosts did not mean remote interpolation had stopped globally. They were a
useful clue that presentation ownership had moved from the interpolated view to the predicted
view.

## Deterministic reproduction

Use two bots so the local spawn is not immediately contested, and disable hybrid mouse steering
so ambient cursor position cannot change local input:

```toml
[input]
mode = "keyboard"

[debug]
prediction_log_level = "debug"
```

```bash
python3 games/dots/tools/dots_session.py \
    --build-dir build/macos-clang-debug \
    --clients 1 \
    --bots 2 \
    --client-config <keyboard-debug-config.toml>
```

The bots cycle right, down, left, and up every four seconds. That makes a remote level-vector
change repeatable without adding a synthetic gameplay fault.

The most useful diagnostic distinction is whether reconciliation compares the same predicted
head tick:

- Different before/after head ticks can simply mean authority advanced while the client had no
  retained inputs.
- A nonzero displacement at the same head tick is an actual correction.
- Repeated same-head corrections after one remote direction edge indicate that an old assumption
  is being reintroduced.

The debug log now reports only nonzero remote displacements. Logging every reconciliation in a
debug build created enough output to perturb fixed-step timing, so short, filtered diagnostic
sessions are preferable.

## The model that should have been preserved

Several numeric values coexist but have different meanings:

| Value | Meaning | May determine |
|---|---|---|
| History capacity (`256`) | Maximum retained commands before recovery is required | Memory and hard-resync bound |
| Retained unacknowledged suffix | Commands that must be replayed after newest authority | Actual replay work |
| Scope horizon | Conservative causal reach needed for that replay suffix | Prediction membership |
| Scope operating floor (`5` ticks currently) | Minimum causal horizon used despite a shorter immediate suffix | Scope stability and small scheduling variation |
| Server input-hold window (`5` ticks by default) | How long authority retains level movement without a new input | When authority changes held movement to zero |
| Remote interpolation delay (`6` ticks) | Historical presentation cursor offset outside prediction | Presentation only |

These values may happen to be numerically close in a configuration. They are not interchangeable.
The current scope floor and default input-hold window are both five ticks, but one does not
implicitly configure the other. In particular, neither storage capacity, server input hold, nor
presentation delay is itself a valid causal horizon.

State also has distinct provenance:

| State | What it means |
|---|---|
| Latest authoritative checkpoint | Newest server fact accepted by this client |
| Retained local command | Immutable user intent sampled for one sequence |
| Retained remote assumption | Authority-derived level fact used to speculate through a tick |
| Predicted checkpoint | Authority plus replayed local commands and assumptions |
| Presentation state | Predicted or historical state plus visual smoothing |

A value found in a predicted checkpoint does not become authoritative merely because it has the
same type as a value in an authoritative checkpoint.

## Root cause 1: a storage bound became a causal horizon

The initial integration declared the interaction-closure horizon from
`kPredictionHistoryCapacity`. That asked the scope builder to include every entity that could
possibly interact over 256 ticks, even when the client normally retained about five inputs.
Nearby bots consequently entered the prediction island instead of remaining on delayed
interpolation.

The initial code also treated exact horizon and owner membership as scope compatibility. The
first correction made the horizon dynamic, but exact owner membership still let ACK depth and
authority replace a safe larger scope with a smaller one. That partial fix allowed presentation
ownership to oscillate between prediction and interpolation.

The correction was to:

- Derive the required horizon from the actual retained suffix, with a five-tick operating floor.
- Treat the 256-entry ring only as a storage and recovery bound.
- Retain an existing scope when its mechanic policy, causal channels, and entity membership are
  a safe superset of the newly required closure.
- Rebase only when new causal membership or a changed subscription is required.

This restored endpoint ghosts for outside-closure bots and stopped ACK-driven presentation
ownership changes.

## Root cause 2: predicted state laundered an old guess into new authority

Each Dots stimulus contains:

- Immutable sampled local commands and edge actions.
- Derived held-movement assumptions for relevant remote owners.

Ordinary rollback correctly replayed the retained stimuli exactly as originally recorded. That
is correct for local commands, but it left every retained remote assumption at its older
authority source. More importantly, construction of the *next* stimulus read remote movement
from the predicted head. After replay, that head contained the older remote guess installed by
`AssumeMovement`. The client then recorded the guess again with the timeline's newer authority
tick.

The source tick changed, but the information had not come from that source. This is a provenance
error, not floating-point nondeterminism.

When a bot turned, the failure loop was:

```text
new authority reports DOWN
        |
restore authority, then replay old RIGHT assumption
        |
predicted head contains RIGHT
        |
next future stimulus samples RIGHT from predicted head
        |
next authority again corrects RIGHT to DOWN
```

The first scope fix made this loop easier to see but could not remove it.

## Resolution

### Preserve local commands; refresh derived assumptions

The engine timeline now offers:

- `reconcile_with_stimulus_refresh`
- `rebase_scope_with_stimulus_refresh`

The game callback receives the command sequence, previous retained stimulus, read-only scratch
state before the step, and active scope. It returns the complete replacement stimulus or a typed
model error.

Dots copies each retained stimulus, leaves its local command and edge actions unchanged, and
rebuilds only its remote-movement assumptions from the newest authoritative checkpoint. The
replacement history and replayed World publish only if the entire transaction succeeds. A
refresh failure returns `StimulusRefreshFailed` without changing committed state or history.

Future stimuli use that same separately retained authoritative checkpoint as their movement
source. They never infer newest remote intent from predicted or presentation state.

### Make presentation ownership stable

The scope horizon follows the retained suffix with its operating floor. An existing safe causal
superset remains selected when ACKs shrink the immediate requirement. A newly required entity or
causal channel still forces an atomic scope rebase from newest authority because old stimuli did
not contain assumptions for newly admitted state.

### Add targeted observability

The Prediction tab now exposes:

- Scope epoch and certified horizon.
- Owner, player, and food membership counts.
- Lifetime scope-rebase count.

`debug.prediction_log_level` provides:

- `info`: scope-change summaries.
- `debug`: scope summaries plus nonzero comparable remote-player displacement, before/after head
  ticks, same-head classification, and retained input count.

After the fix, the deterministic two-bot session showed one expected correction when each
unknown remote direction edge became authoritative, followed by stable reconciliation rather
than repeated backward/forward motion.

## Contributing design conditions

The direct bugs were local, but several design conditions made them easy to introduce:

- `HistorySettings::capacity` and `PredictionRequest::replay_horizon` were strongly typed as
  different concepts only at their outer structures; code could still explicitly convert the
  capacity count into the horizon's `TickDelta` without declaring why.
- The initial engine API treated a `Stimulus` as uniformly immutable. It could not express that
  one field was sampled player intent while another was a revisable authority-derived level
  assumption.
- Dots did not retain a separate newest authoritative checkpoint for constructing future
  assumptions, so the convenient predicted World became an accidental data source.
- Correction metrics focused on the controlled player. Remote presentation could oscillate
  without the primary correction counter explaining what the user saw.
- The integration changed rollback, causal membership, and presentation ownership together.
  Unit coverage for each component did not exercise their transition boundaries.

## Why existing tests did not catch it

The individual components were tested, but the failing composition was not:

- Scope tests proved closure construction, but did not distinguish a storage bound from the
  runtime causal horizon.
- Timeline tests proved exact retained-stimulus replay, which was the old API's intended
  behavior.
- Remote-assumption tests used a stable remote movement vector.
- Presentation tests correctly removed interpolated duplicates for predicted entities.
- No test combined a retained local suffix, newer authority changing a remote level vector, a
  future input after replay, and stable presentation ownership.

The regression tests now cover:

- Closure growth from retained replay depth rather than ring capacity.
- Retaining a safe scope superset after ACK shrink.
- Refreshing derived stimuli atomically during reconcile and scope rebase.
- Refresh failure preserving committed engine state and history.
- A remote direction change replacing every retained assumption and feeding the next future
  stimulus from newest authority.

## How to prevent this class of bug

Use these review rules whenever a predicted mechanic is added:

1. **Name every bound by purpose.** Storage capacity, replay depth, causal horizon, authority
   hold, interpolation delay, and smoothing time must not share an unqualified “window” or
   “history” variable.
2. **Track provenance, not just value types.** For every stimulus field, state whether it was
   sampled locally, copied from authority, derived from predicted state, or created only for
   presentation.
3. **Separate immutable causes from revisable assumptions.** Local user intent and edge actions
   remain immutable. An external level assumption may be refreshed only through an explicit,
   atomic engine operation.
4. **Never seed simulation from presentation.** Interpolated, extrapolated, and smoothed values
   cannot enter checkpoints, stimuli, closure construction, or authority comparisons.
5. **Do not promote predicted state to authority.** A predicted checkpoint can provide scratch
   state for deterministic stepping, but it cannot certify the source of an external fact.
6. **Make scope changes monotonic where safe.** If an existing scope is a causally valid superset,
   retain it rather than flapping presentation ownership as ACK depth changes.
7. **Test changing facts, not only steady state.** Every level-triggered external assumption needs
   a test where authority changes it while multiple frames are retained.
8. **Test the next frame after reconciliation.** A replay can end correctly while the next
   predicted stimulus immediately reintroduces stale data.
9. **Classify correction coordinates.** Diagnostics must distinguish authority advancing to a
   different head tick from disagreement at the same head tick.
10. **Keep debug instrumentation non-perturbing.** Prefer transition/nonzero logs and counters to
    per-tick output, especially in fixed-step debug sessions.

## Current neighboring-mechanic audit

A targeted audit after the fix found no second active instance of the same provenance violation
in the currently implemented Dots mechanics:

| Mechanic or layer | Current source of replay truth | Audit result |
|---|---|---|
| Local movement and split edge | Retained `InputSample` converted once into the local `TickCommand` | Refresh copies the prior stimulus and replaces only `remote_movement_assumptions` |
| Remote held movement and authoritative stop | Separately retained newest authoritative checkpoint | Both nonzero direction changes and zero movement replace retained assumptions |
| Split topology, launch, merge, and cooldowns | World checkpoint: allocator, prediction keys, launch velocity, owner state, and merge/split deadlines | Restore/replay tests cover structural correction and stable predicted identity |
| Food consumption and player absorption | Checkpoint membership plus interaction-closed scope | Closure and event-retraction tests cover participants entering deterministic resolution |
| Entity creation/removal | Authority checkpoint, scope epoch, and owned `PredictionKey` lifecycle | New causal membership rebuilds from authority; old incomplete stimuli are not reused |
| Random or scheduled simulation facts | No mutable RNG or wall-clock service is polled by the current World tick | Allocator/tick state is checkpointed; adding randomness would require checkpointed RNG state or an explicit fact |
| Presentation | Post-commit predicted state or historical replicated samples | No presentation value is read by `Dots::Prediction` or `Dots::ClientRuntime` to construct a stimulus |
| Audio/particles/UI consequences | Stable-keyed post-commit event transitions | Consequence router policy, not World replay, owns external exposure |
| Score and other global aggregates | Not implemented | The causally closed aggregate or confirmed-base-plus-speculative-delta rule remains mandatory when added |
| Multi-frame replay | Not implemented; reconciliation is synchronous and atomic | No partially refreshed scratch state can currently become presentation input |

This was a focused provenance and boundary audit, not a proof that every future mechanic is
correct. The regression tests and review rules above are the continuing guardrails.

## Larger bug class and audit matrix

This incident belongs to a broader class: **semantic collapse across bounds, provenance, and
state layers**. The code can be memory-safe and deterministic while consistently replaying the
wrong meaning.

| Area to audit | Similar failure | Required invariant |
|---|---|---|
| Local movement and actions | Resampling a key or mouse during replay; repeating a split edge | Commands are captured once by sequence and never refreshed |
| Remote movement and AI intent | Predicted velocity is labeled as newest server intent | Level assumptions carry authority provenance and refresh only from authority |
| Server input hold/stop | Client continues movement after authority's hold expires | Authoritative zero movement refreshes retained assumptions like any other level change |
| Split, merge, launch, and cooldowns | Derived topology or timers are reconstructed from presentation or incomplete state | Structural state and deadlines are checkpointed; unknown remote edges are not invented |
| Food and player interactions | A participant enters causal reach but is absent from the replay island | Scope is closed over the full replay horizon before stepping |
| Entity lifecycle | A scope admits a new entity but old frames have no cause for it | Rebase from newest authority and regenerate complete retained stimuli |
| Score, resources, or other non-spatial state | A local delta is treated as complete global truth | Predict only a causally closed aggregate or keep confirmed base plus explicitly speculative delta |
| RNG and scheduled facts | Replay polls a mutable generator, clock, or service | Seed/state is checkpointed or the fact is an explicit stimulus |
| Consequences | Replayed simulation repeats sound, particles, analytics, or UI | Simulation emits stable keyed events; post-commit consequence policy owns exposure |
| Presentation smoothing/extrapolation | A visually smooth value feeds the next simulation step | Presentation is a terminal consumer and never a simulation source |
| Multi-frame replay | Partially refreshed scratch state becomes visible | Scratch state, replacement stimuli, events, and scope publish atomically at one caught-up head |

This audit applies beyond spatial mechanics. Score, cooldowns, inventory, ability state,
scheduled spawns, AI decisions, and global objectives can suffer the same provenance error if a
predicted value is later treated as an authoritative observation.

## Validation

The final correction was validated with:

- Focused engine rollback, Dots prediction, and client-config tests.
- A native two-bot keyboard-only session across multiple programmed direction changes.
- Full tracked-source `clang-format`.
- `run-clang-tidy` across all 58 configured translation units with warnings as errors.
- Full debug build and all 215 tests.
- Verified `dots_client_package`.
