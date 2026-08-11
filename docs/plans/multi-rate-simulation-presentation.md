# Multi-rate Simulation and Presentation Boundary Audit

## Status and Ordering

This is a planned research and boundary-hardening pass. The presentation contract is recorded
now. Run the measured cadence comparison after `feature/17-bot-load-harness` establishes the
current 30 Hz cost baseline and before `feature/15-interest-management` changes that workload.
The outcome may retain the current cadence; 240 Hz is a candidate, not a predetermined result.

Use `research/multi-rate-simulation-cadence` for the comparison. If it recommends a production
change, write a separately reviewed feature plan before changing Dots simulation semantics,
rollback checkpoints, or protocol behavior.

## Problem

Dots intentionally separates its fixed 30 Hz simulation from variable-rate rendering, but the
separation has not always been enforced at the data boundary. A render loop can run at 144 Hz
while repeatedly reading the newest 30 Hz simulation pose. When the playing camera did that,
smoothly sampled remote entities were viewed through a stepping camera and appeared to flicker.

Dots also currently performs movement integration, spatial-grid updates, collision detection,
absorption, consumption, splitting, and merging in one 30 Hz `World::advance()` operation. A
higher fixed physics cadence could improve fast-contact accuracy for a future mechanic, but it
would not by itself improve rendering smoothness. Presentation interpolation already supplies
the meaningful intermediate samples for a 60, 144, 240, or variable-refresh display.

Use precise terms throughout this work:

- **Gameplay tick:** the authoritative 30 Hz command, rule, checkpoint, and publication cadence.
- **Physics substep:** a possible fixed integer subdivision of one gameplay tick, such as eight
  240 Hz substeps. It is not a network tick or render frame.
- **Network cadence:** currently 30 Hz client input and 15 Hz authoritative snapshots.
- **Render frame:** variable-rate client presentation and GPU submission.
- **Display refresh:** the monitor-controlled rate at which submitted images become visible.

## Goals

- Make it difficult to render any moving object or camera directly from a discrete simulation,
  replication, or prediction pose.
- Produce one immutable, game-owned presentation snapshot per render frame, including the camera
  and the controlled entity sampled from the same presentation timeline.
- Keep rendering variable-rate and independent from gameplay, physics, and networking cadences.
- Measure whether 60, 120, or 240 Hz internal physics substeps materially improve collision or
  movement correctness over the 30 Hz baseline.
- If substeps are justified, keep 30 Hz command consumption, gameplay publication, rollback
  checkpoints, and network input cadence unless the evidence identifies a separate reason to
  change one of them.
- Preserve deterministic replay, server authority, headless operation, and explicit thread
  ownership.

## Non-goals

- Do not make rendering fixed at 240 Hz. Rendering remains display-driven and variable.
- Do not use a 240 Hz physics loop as a substitute for presentation interpolation.
- Do not introduce a general physics engine, generic scene graph, or universal engine world for
  Dots.
- Do not put authoritative physics on an unsynchronized background thread.
- Do not send inputs or snapshots at 240 Hz merely because internal substeps use that cadence.
- Do not activate `MyCore::Tasks` without Feature 21's measured entry criteria.

## Chosen Presentation Boundary

The target client flow is:

```text
authoritative / replicated / predicted state
                  |
                  v
Dots-owned presentation sampling
  - previous/current source samples
  - render accumulator or presentation cursor
  - correction residuals
  - camera derived from the same sampled pose
                  |
                  v
immutable PresentationSnapshot
  - camera
  - presentation-space entity transforms
  - visual lifecycle and debug primitives
                  |
                  v
Render2D/Render3D preparation -> ordered GPU submission
```

The renderer and camera must consume presentation poses, never raw simulation poses. A
composition root may inspect simulation state to produce gameplay input or diagnostics, but it
must not bypass the presentation sampler when constructing a normal rendered frame. Fixed-state
debug comparison remains allowed only as an explicitly labeled diagnostic layer.

The snapshot is immutable after publication. This is useful immediately as an ownership rule and
later as the handoff to bounded render-preparation tasks. It does not require a render thread or
task scheduler now.

### Presentation validation

- Sample controlled motion at synthetic 60, 144, and 240 Hz render intervals while simulation
  advances at 30 Hz; screen-space motion must remain continuous and monotonic between ticks.
- Verify the controlled entity and following camera use the exact same sampled position.
- Verify remote presentation remains smooth in screen space while the local camera moves.
- Verify correction smoothing is applied once, after fixed-tick interpolation, and never feeds
  simulation or rollback state.
- Add a dependency/API test ensuring render submission accepts the immutable presentation output
  without requiring access to a Dots `World`.

## Physics Cadence Comparison

Compare these configurations with identical commands, initial checkpoints, and recorded seeds:

1. 30 Hz gameplay with one 30 Hz simulation step: the baseline.
2. 30 Hz gameplay with two 60 Hz physics substeps.
3. 30 Hz gameplay with four 120 Hz physics substeps.
4. 30 Hz gameplay with eight 240 Hz physics substeps.

Use an integer number of fixed substeps per gameplay tick. One accepted 30 Hz movement input is
held across all of that tick's substeps; edge actions such as split are consumed exactly once.
The owner thread completes every substep before it publishes the next authoritative gameplay
state. Network polling and GPU submission never observe a partially stepped world.

Dots collisions are gameplay, not a separable generic rigid-body layer. The research must
compare two explicit semantics rather than silently choosing one:

- resolve contact-driven topology changes immediately at the deterministic substep where they
  occur, then aggregate the resulting journal into the enclosing gameplay tick; or
- collect contact facts during substeps and apply game rules once at the gameplay-tick boundary.

The first is physically responsive but may require a substep ordinal in deterministic event
ordering. The second preserves a strict 30 Hz business-rule boundary but can change contested
collision outcomes. If neither contract is simpler and demonstrably better than the current
single step, keep Dots at 30 Hz.

### Rollback and protocol constraints

- Rollback continues to accept one command per gameplay tick and must deterministically replay
  the same number and order of internal substeps.
- Checkpoints are published at gameplay-tick boundaries. Any integrator remainder or other
  substep state needed for exact replay belongs in the checkpoint.
- A structural edge action is never repeated once per substep.
- Multiple events inside one gameplay tick must retain stable, collision-free identities and a
  canonical order through replay, authority receipts, and consequence delivery.
- Snapshot cadence remains 15 Hz for the experiment. A cadence change requires independent
  bandwidth and latency evidence.
- Protocol versioning is required only if a selected design changes serialized state, event
  identity, or client interpretation; an internal implementation detail is not put on the wire.

### Measurements

Use Feature 17's recorded 10, 100, 500, and 1,000-client/entity workloads where applicable, plus
deterministic collision sweeps designed to expose tunneling and contested-contact ordering.
Record for every candidate:

- authoritative tick mean, p95, p99, and deadline misses;
- client prediction and reconciliation replay mean, p95, p99, and budget warnings;
- collision misses or materially different outcomes against a high-resolution oracle;
- snapshot bytes and event counts, which should remain cadence-independent unless semantics
  genuinely change;
- deterministic checkpoint, journal, and consequence equality across repeated runs;
- implementation complexity and checkpoint/protocol expansion.

Do not select 240 Hz solely because the machine can run the small case. Select the lowest cadence
that fixes a demonstrated correctness or gameplay problem while retaining the measured server
and prediction budgets. If interpolation already solves the visible problem and the collision
suite finds no material improvement, retain 30 Hz.

## Considered Approaches

### Keep 30 Hz simulation and strengthen presentation interpolation

This is the baseline and likely remains sufficient for current Dots motion. It has the lowest
server and replay cost and keeps event timing simple.

### Fixed physics substeps inside a 30 Hz gameplay tick

This is the only production candidate in this pass. It preserves one authoritative owner and
integer tick boundaries while allowing more accurate integration/contact sampling.

### Independent 240 Hz physics thread

Rejected for this pass. It introduces partially visible state, cross-thread command timing,
checkpoint synchronization, and nondeterministic event-order risks without a measured need.

### Render-time physics extrapolation

Not authoritative physics. Bounded presentation extrapolation can reduce apparent latency but
must remain replaceable visual state and may require correction. It cannot decide collisions,
absorption, or other gameplay outcomes.

## Exit Criteria

The research branch is complete when:

- the immutable presentation boundary and camera/entity sampling invariants have automated
  coverage;
- all four cadence configurations run the same deterministic correctness and workload corpus;
- results distinguish visual smoothness from physics correctness and CPU cost;
- rollback, event identity, checkpoint, networking, and ownership consequences are documented;
- the outcome explicitly retains 30 Hz or recommends one measured substep cadence; and
- any recommendation that changes production behavior has a new feature plan with migration and
  protocol decisions before implementation.

## Open Questions

- Which future Dots mechanic, if any, demonstrates a collision error at 30 Hz that interpolation
  cannot solve?
- Must contact-driven absorption occur at a substep or only at the enclosing gameplay boundary?
- Would a selected higher cadence apply to authoritative and predicted Dots worlds identically,
  or should the experiment reject asymmetric stepping outright?
- Does the selected model need substep ordinals in event journals, debug traces, or protocol
  receipts?
- Is a transient immutable snapshot sufficient, or does measured render preparation justify a
  persistent client-only render world later?
