# Rollback Prediction Design

This document is the canonical design contract for Dots rollback, selectable prediction, and
speculative gameplay consequences. It defines durable invariants and interfaces; feature plans
define implementation order, and `debugging_and_observability.md` defines the operator-facing
output.

## Status and Scope

- **Current:** Feature 11 predicts only the owned player's position by replaying unacknowledged
  movement.
- **Remote presentation baseline:** Feature 12 uses delayed interpolation for remote presentation
  before complete rollback changes the normal predicted view.
- **Authoritative lifecycle baseline:** Feature 13 now carries entity ownership and one atomic,
  recipient-specific Playing/Spectating block in every full snapshot. Defeat and respawn remain
  confirmed server results. Confirmed spectating selects follow-killer/free-camera presentation;
  those camera modes never predict a lifecycle transition or authoritative entity.
- **Committed direction:** Feature 14 replaces position-only replay with a complete Dots World
  rollback kernel and makes the prediction set selectable.
- **Deferred research:** time-sliced resimulation may be reconsidered only from measured Feature
  14 workloads. Same-frame atomic replay remains the baseline.

The first kernel is deliberately Dots-owned. Its contracts should be reusable in shape, but no
`engine/` library or cross-game ABI is justified until a second game demonstrates the same needs.

## State Model and Authority

Keep these views separate. The complete cross-feature ownership and clock vocabulary is in
[`networked_prediction_reference.md`](networked_prediction_reference.md).

| State | Meaning | Owner |
|---|---|---|
| Authoritative World | Complete gameplay truth at a server tick. | Server only |
| Latest replicated snapshot | Newest validated authoritative data received by one client. It is historical by receipt time. | Client copy of server truth |
| Predicted World | Replicated checkpoint advanced speculatively through retained commands and remote assumptions. | Client only |
| Remote presentation frame | Delayed presentation sampled between known snapshots. | Client presentation |
| Presentation state | Positions, radii, camera, UI, audio, and effects actually shown. | Client presentation |
| Confirmed consequence | Session or durable event that is exposed only after authority reports it. | Server decision, client display |

Prediction never grants authority. A client may temporarily display speculative gameplay, but a
newer valid server state always wins reconciliation.

## Planned Dots Prediction Contracts

Feature 14 should add a `Dots::Prediction` target under `games/dots/`. It depends on Dots
simulation value types and must not depend on SDL, rendering, transport, or an executable
composition root.

The public vocabulary is:

```cpp
struct PredictionKey {
    ClientId origin_client_id;
    InputSequenceId input_sequence_id;
    std::uint16_t spawn_ordinal;
};

enum class PredictionSetMode {
    AllReplicated,
    OwnerAndInteractionClosure,
    OwnerOnly,
};

enum class CuePolicy {
    Resimulated,
    Deduplicated,
    Cancelable,
    ConfirmedOnly,
};
```

The exact storage may remain private, but the following behavioral types are required:

- `WorldCheckpoint`: complete value state needed to restore a Dots World at one fixed tick.
- `RollbackFrameRecord`: predicted tick, checkpoint, applied commands, remote assumptions, and
  cue journal for one retained frame.
- `ReconcileReport`: authoritative base, input acknowledgement, replay range, continuous and
  structural divergence, cue changes, duration, and recovery result.
- `RollbackKernel`: initializes from authority, advances one fixed tick, reconciles
  transactionally, hard-resyncs, changes prediction mode at an atomic boundary, and exposes
  immutable committed/debug views.

These names define the intended contract. They do not require virtual interfaces: the initial
implementation should use ordinary Dots value types and functions.

## Checkpoint Contract

A checkpoint must be sufficient to reproduce later predicted state without consulting mutable
presentation or transport state. It contains:

- Simulation tick and deterministic gameplay configuration.
- Recipient session mode, owned and primary entities, confirmed follow target, defeat/respawn
  ticks, and authoritative action results received with the same snapshot.
- Entity IDs, kinds, ownership, positions, movement, launch velocity, mass, and radius-derived
  state.
- Cooldown and merge deadline ticks.
- Entity allocator state and predicted-spawn associations required by replay.
- Any future gameplay RNG state and ordering state before a predicted system may use them.

A checkpoint never contains pointers, renderer objects, widgets, wall-clock timestamps, log
state, sockets, or other irreversible side effects.

Feature 16 delta snapshots must reconstruct a coherent authoritative view before passing it to
rollback. A partial network record is not itself a rollback checkpoint.

## Command and Tick Contract

- One retained command advances one predicted fixed simulation tick.
- Commands are immutable and identified by a strictly increasing input sequence.
- Movement is level-triggered and may be held when a tick has no newer sample.
- Split, respawn, and future actions are edge-triggered. They execute at most once for their
  input sequence and are never repeated by held-input behavior.
- Input redundancy may retransmit an identical command. A conflicting duplicate is invalid.
- An ACK means the server consumed the command; it does not mean every requested action passed
  gameplay validation.

For the initial sequence-driven timeline:

```text
predicted tick = authoritative snapshot tick + replayed unacknowledged command count
```

The rollback tick is not client session time, wall time, or Feature 12's delayed remote
presentation cursor.

## Selectable Prediction Set

The kernel supports these policies:

| Mode | Predicted entities | Intended use |
|---|---|---|
| `AllReplicated` | Every entity in the reconstructed client view. | Feature 14 default and Rocket League-style experiment |
| `OwnerAndInteractionClosure` | Owned state plus every entity that can affect it during the replay interval. | Cost-controlled competitive fallback |
| `OwnerOnly` | Locally owned pieces only. | Feature 11-like fallback and debugging comparison |

A prediction set must be closed over predicted interactions. If A can collide with B during a
replay tick, either both participate in that predicted island or their interaction remains
authority-confirmed. Changing modes occurs only at an atomic frame boundary and clears or
rebases history that is incompatible with the new set.

Before Feature 15, `AllReplicated` means the complete full snapshot. Feature 15 must adapt it to
AOI membership plus a collision safety margin, with explicit entry/exit initialization and
transition smoothing.

## Remote Assumptions

The client does not know another player's future input. A predicted remote entity therefore
holds its newest replicated movement until newer authority arrives. Unknown remote action bits
are zero: edge actions cannot be invented or held.

Remote assumptions are recorded with each rollback frame so a correction can be attributed to
missing information rather than mistaken for client authority or clock drift.

## Transactional Reconciliation

Reconciliation follows one indivisible transaction:

1. Validate snapshot ordering, IDs, immutable configuration, ACK monotonicity, and checkpoint
   completeness.
2. Build a scratch World from the authoritative checkpoint.
3. Discard acknowledged command history.
4. Recreate speculative spawns deterministically from prediction keys.
5. Replay the unacknowledged suffix through shared Dots simulation rules.
6. Compare continuous state, topology, ownership, deadline, and gameplay-event results.
7. Resolve speculative cue lifecycle changes.
8. Atomically commit World, history, spawn mappings, metrics, and presentation correction data.

Stale or invalid snapshots do not mutate committed prediction. No caller can observe a partially
replayed World.

The initial history bound is 64 ticks, approximately 2.13 seconds at 30 Hz. Missing history,
capacity exhaustion, checkpoint incompatibility, or prediction-key inconsistency causes a hard
resync to the newest validated authority and clears incompatible speculative state.

## Predicted Entity Lifecycle

Spawns use `PredictionKey{client, input, ordinal}` as their stable speculative identity. Local
temporary entity handles are never sent as authority.

When authority arrives:

- A matching server entity classifies and replaces the temporary identity without replaying the
  spawn cue twice.
- A rejected or absent spawn disappears during reconciliation and cancelable presentation is
  cleaned up.
- A server spawn without a local prediction is created normally.
- A prediction-key collision or ambiguous classification is a correctness failure and triggers
  hard recovery rather than an arbitrary match.

Entity removal, ownership changes, and component changes are structural state. They must be
restored and replayed, not approximated as position corrections.

## Gameplay and Consequence Policy

| Mechanic or state | Rollback simulation | Normal consequence |
|---|---|---|
| Local movement and owned pieces | Predicted | Immediate |
| Replicated remote movement | Predicted by default | Immediate with presentation correction |
| Split and merge topology | Predicted | Immediate with predicted-spawn classification |
| Cooldown and owned resource cost | Predicted | Immediate countdown corrected to server deadline |
| Food removal and mass gain | Predicted | Immediate and reversible |
| Player absorption and mass transfer | Predicted | Immediate World result and reversible |
| Local predicted elimination | Predicted in World | Retain a control/camera proxy pending confirmation |
| Spectator transition | Not speculative | Confirmed session state only |
| Respawn placement | Not speculative | Pending request until server spawn |
| Score, kill feed, and achievements | Not speculative | Confirmed and deduplicated |
| Sound and particles | Per-cue policy | Speculative only when safely cancelable or deduplicated |

The local pending-elimination proxy keeps collecting input and preserves a camera anchor without
pretending that the session entered spectator mode. Confirmation replaces it with spectator
presentation; rollback cancels it and restores predicted control.

## Cue and Presentation Contract

Simulation state corrects immediately. Presentation may smooth the visible change but never
feeds smoothed values back into replay.

Cue policies mean:

- `Resimulated`: undo and produce again from replayed simulation.
- `Deduplicated`: a predicted cue and its confirmed counterpart produce one visible occurrence.
- `Cancelable`: run immediately but provide explicit rejection cleanup.
- `ConfirmedOnly`: do not expose until authority reports the durable result.

Feature 12 interpolation remains available as a comparison and fallback. Feature 14 normally
renders the predicted World, with distinct debug layers for authoritative-known, predicted,
interpolated, pre-correction, and smoothed presentation state.

## Adaptive Command Buffer

The authoritative server remains fixed at 30 Hz and consumes at most one ordered command per
client each tick. It never changes the World tick rate to repair one connection.

Feature 14 targets two queued server commands and prefills two neutral commands when prediction
becomes ready. Accepted snapshots update a queue-depth EWMA with `alpha = 1/8`. A depth from 1.5
through 2.5 is a deadband. Outside it, adjust only the client prediction/input cadence:

```text
rate scale = clamp(1 + 0.025 * (2 - smoothed depth), 0.95, 1.05)
```

An empty server queue holds the last movement but never repeats edge actions. The controller
reports target, latest and smoothed depths, rate scale, low/high events, and accumulated phase
corrections. It does not redefine client session time or estimated server time.

## Same-Frame Replay and Recovery

The baseline completes every bounded replay in the client frame that accepts the authoritative
snapshot. Scratch state commits atomically. Replay duration is measured against a 2 ms warning
budget, but a wall-clock overrun never causes a partial commit.

If same-frame cost is unacceptable, first evaluate simulation optimization, prediction-set
reduction, and snapshot-triggered replay frequency. Time-sliced resimulation remains a measured
research direction, not a second scheduler implemented preemptively.

## Deferred Multi-Frame Resimulation Research

Feature 14 records:

- Replay tick count and predicted entity count.
- Checkpoint bytes and structural changes.
- p50, p95, p99, and maximum replay duration.
- Replay duration grouped by entity count, RTT, jitter, loss, and topology changes.
- Client frame-time impact and over-budget rate.
- Hard-resync, correction, spawn-classification, and cue-cancellation counts.
- Input-to-presentation latency and command-buffer health.

Measure 10, 100, 500, and 1,000 entities at 100, 200, and 400 ms RTT with representative jitter
and loss. Reconsider time-slicing only if same-frame replay exceeds 4 ms at p99 or causes a
rollback-attributable frame overrun in more than 1% of reconciliations at the target 200 ms
scenario, and cheaper mitigations materially harm interaction quality.

Crossing that threshold pauses implementation and requires a separate reviewed plan. A future
multi-frame design must keep an immutable baseline, preserve atomic final commit, handle newer
snapshots superseding active work, keep accepting commands, bound scratch lag, and hard-resync if
it cannot catch up. A partially caught-up World is never exposed.

## Debug and Metrics Contract

The Gameplay and Rollback views must expose enough data to explain a correction without inferring
it from motion:

- Prediction mode and predicted/interpolated/confirmed entity counts.
- Authoritative base snapshot/tick, predicted tick, ACK, and replay range.
- Prediction lead in ticks, defined only as predicted tick minus that authoritative base tick;
  it is not RTT, snapshot age, or an estimate of live server time.
- History occupancy, replay ticks/duration, and hard-resync reason.
- Continuous versus structural divergence.
- Spawn classifications, rejections, and ambiguous-key failures.
- Cue replay, suppression, cancellation, and confirmation counts.
- Command-buffer target/latest/EWMA depth, cadence scale, and drift corrections.
- State-layer overlays for authoritative, predicted, interpolated, pre-correction, and
  presentation state.

The canonical labels, colors, availability rules, and troubleshooting steps live in
`debugging_and_observability.md` and must remain synchronized with implementation.

## Research Basis

- [Psyonix: Rocket League physics and networking](https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf)
  describes input buffering, predicting all physics actors, whole-physics rewind, and expensive
  same-frame catch-up.
- [Valve: Source Multiplayer Networking](https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking)
  describes local prediction, remote interpolation, and server-side rewind for discrete shots.
- [Unity: prediction switching](https://docs.unity.cn/Packages/com.unity.netcode%401.5/manual/prediction-switching.html)
  makes prediction selectable by ownership and interaction needs.
- [Unreal: networked physics](https://dev.epicgames.com/documentation/en-us/unreal-engine/networked-physics-overview)
  separates resimulation from cheaper predictive interpolation and records the CPU/memory
  tradeoff.
