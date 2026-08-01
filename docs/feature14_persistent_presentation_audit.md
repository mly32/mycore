# Feature 14 Persistent Presentation Audit

Status: corrected on `feature/14` after the step 7 presentation integration.

## Trigger

With prediction debug layers enabled, the filled local-player circle lagged behind the white
predicted-position outline even when no reconciliation correction was active. With the layers
hidden, the filled circle appeared to stutter. Remote players could also receive additional
smoothing when a delayed interpolation bracket advanced.

The filled primary and white outline are intentionally different during a real reconciliation
residual: simulation corrects immediately while presentation eases the old visual pose to the
corrected pose over 100 ms. They should otherwise overlap. The persistent presentation adapter
was violating that rule by smoothing a pose which had already been smoothed by
`LocalPredictionPresentation`.

## Presentation Pipeline

The graphical client has several state and presentation stages:

1. The replicated World contains the latest accepted server authority.
2. The rollback World restores authority and replays retained commands for the predicted
   interaction closure.
3. `LocalPredictionPresentation` applies the one allowed 100 ms visual residual to a corrected
   primary pose.
4. The Dots frame extractor selects a pose source for each entity.
5. `PersistentWorldPresentation` preserves semantic identity, interpolates predicted fixed ticks,
   smooths real discontinuities, fades removals, and records the primary motion trail.
6. Diagnostic outlines are appended separately and do not feed simulation or normal
   presentation.

Step 5 must not infer that every new source revision is a discontinuity. A revision identifies a
new input sample, but its meaning depends on the source:

| Source | Pose supplied to the adapter | Revision means | Persistent policy |
|---|---|---|---|
| `State` | Already-presented local primary, or current offline pose | Local predicted tick or no revision | Pass through; do not add smoothing |
| `Predicted` | Complete rollback-World pose in the interaction closure | Predicted fixed tick | Interpolate ordinary tick advance; smooth a changed pose at the same tick |
| `Extrapolated` | Newest snapshot advanced by its known movement and launch | Authoritative snapshot ID | Follow continuously within one revision; smooth replacement by a newer authority sample |
| `Interpolated` | Delayed sample between two known authority snapshots | Newer bracket snapshot ID | Pass through; the interpolation buffer already supplies continuous motion |

Source handoffs, predicted-child entity-ID remaps, and entity removal remain actual presentation
transitions. Handoffs and remaps preserve the old visual pose and decay the residual over 100 ms;
removals fade over 100 ms.

## Root Cause

The adapter classified this condition as a correction:

```text
source is not Predicted AND source revision changed
```

That combined two different facts:

- a new sample or bracket became available; and
- the selected pose discontinuously replaced a prior guess.

For the controlled player, the extractor labels the already-smoothed
`LocalPredictionPresentation` pose as `State` and advances its revision with the rollback World
tick. The persistent adapter therefore created another 100 ms residual on nearly every 30 Hz
tick. The camera and white predicted outline bypassed that second residual, exposing the filled
circle behind them.

For delayed interpolation, a bracket revision is normal cursor progress between known samples,
not a correction. Applying another residual at each bracket change could add avoidable lag.

New extrapolation authority is different: the client has advanced the previous snapshot using a
held remote input, and the newer snapshot can invalidate that guess. Preserving the previous
visual and decaying only that replacement residual remains intentional.

## Correction

Revision-triggered smoothing now applies only to `Extrapolated` source replacement. `State` and
`Interpolated` revisions pass through. The existing policies for predicted fixed-tick
interpolation, same-head predicted correction, source handoff, identity remap, and removal remain
unchanged.

Focused tests establish the source contract:

- already-presented `State` revisions pass through without a correction;
- delayed interpolation follows both within-bracket motion and bracket changes directly;
- extrapolation advances directly within one snapshot revision and smooths a newer snapshot
  replacement;
- predicted fixed ticks interpolate;
- same-head predicted changes, source handoffs, and predicted-key ID remaps smooth;
- structural fades and motion trails stay bounded.

## Remote Motion: Prediction, Extrapolation, and Interpolation

There is no single remote-player mode:

| Client situation | Remote state path | Runs gameplay rules? | Time behavior |
|---|---|---|---|
| Playing, inside the owned interaction closure | Complete rollback-World prediction using newest authoritative held remote movement; unknown edge actions are neutral | Yes: movement, launch, cohesion, collision, consumption, absorption, split/merge state already in the closure, and replay | Advances with the local prediction head and reconciles |
| Playing, outside the closure, default `extrapolated` mode | `RemoteExtrapolationBuffer` using shared player movement/launch kinematics | No | Advances the newest accepted snapshot for at most six ticks/200 ms, then holds |
| Playing, outside the closure, `interpolated` mode | Feature 12 snapshot interpolation buffer | No | Renders six ticks behind newest authority and holds on underrun |
| Playing, `comparison` mode | Extrapolated filled pose plus delayed interpolation outline | No outside the closure | Shows both policies |
| Spectating, default `live` mode | Feature 12 interpolation, then `RemoteExtrapolationBuffer` and persistent semantic tracks only during underrun | No | Interpolates six ticks behind authority; after exhausting the newest endpoint, advances only that uncovered tail for at most another six ticks/200 ms, then holds |
| Spectating, `delayed` mode | Feature 12 delayed interpolation through persistent semantic tracks | No | Interpolates six ticks behind authority and holds immediately on underrun |

The extrapolation layer is presentation-only. It does not make collision, eating, splitting,
merging, closure, or checkpoint decisions. Remote gameplay that can causally affect an owned
piece belongs in the predicted interaction closure instead.

## Why Square-Turning Bots Correct

The client cannot know a remote direction change before authority carrying that change arrives.
Until then, closure prediction and outside-closure extrapolation both continue the last known
level movement. When a square-pattern bot turns, the client may therefore be ahead along the old
edge while the new snapshot is already along the new edge. The replacement must converge to
server truth.

This correction is inherent to guessing unknown remote input, but the particular visual
tradeoff is selectable:

- extrapolation reduces apparent motion latency and can guess wrong at turns;
- delayed interpolation avoids predicting the turn by rendering known history, at the cost of
  six ticks/200 ms of visual delay;
- loss can make extrapolation reach its 200 ms cap and hold, so the next accepted authority may
  cause a larger correction;
- the persistent adapter hides the discontinuity over 100 ms but does not alter gameplay state.

For physics-heavy games, this does not imply that every remote object should run full future
gameplay. A local vehicle and its immediate interaction closure may be predicted, while distant
vehicles remain interpolated or briefly extrapolated. A locally fired projectile is commonly
created immediately; a remote projectile cannot be predicted before its fire action is known and
may instead be created on receipt and advanced to the appropriate presentation time. Server
rewind or hit validation is a separate authority policy. Any low-latency guess about unknown
remote inputs can require correction.

## Prevention Rules

- Treat source revision as sample identity, not a universal discontinuity signal.
- Give every pose producer an explicit sampling contract: fixed-tick interpolation, continuous
  pass-through, or authority-replacement correction.
- Smooth a corrected simulation pose once. Downstream compositors must not low-pass a pose that
  is already presentation-smoothed.
- Keep debug state outside the normal compositor so diagnostic outlines expose, rather than
  inherit, presentation residuals.
- Test same-revision motion and revision changes separately for every presentation source.
- Preserve the rule that presentation residuals never feed rollback, collision, authority,
  checkpoint, or event decisions.

The later correction-history hardening closed the remaining cross-tick limitation. Reconciliation
now supplies an explicit per-entity correction generation and displacement to presentation, so a
predicted-closure correction does not have to be inferred from `source_revision`. The revision
continues to identify ordinary fixed-tick source progress; only the explicit correction signal
creates a residual. Source/tick heuristics must not be reintroduced to guess one.
