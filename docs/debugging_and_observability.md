# Debugging and Observability Guide

Use the state and clock names defined in
[`networked_prediction_reference.md`](networked_prediction_reference.md). In particular, latest
replicated authority, owned prediction, remote presentation, and composed presentation are
separate sources and must never share an ambiguous "current world" label.

This guide is the canonical reference for Dots runtime debugging output: what each value means,
where it comes from, how often it changes, and which conclusions it can and cannot support.

Keep this guide synchronized whenever an overlay label, metric definition, log category, debug
visual, fault control, or warning threshold changes. Feature plans may describe intended output,
but this guide must clearly distinguish implemented behavior from planned behavior.

## Status Labels

This document uses three status labels:

- **Current:** Feature 11 owned prediction/reconciliation, Feature 12 remote
  interpolation-and-hold, and Feature 13's authoritative absorption/session lifecycle are
  implemented. Feature 13's follow/free spectator presentation and authoritative Gameplay output
  are also implemented.
- **Feature 14 planned:** game-neutral rollback timeline, interaction-closed Dots prediction, and
  Rollback output specified in
  [`plans/14-selectable-world-rollback.md`](plans/14-selectable-world-rollback.md) and
  [`rollback_prediction_design.md`](rollback_prediction_design.md).

When a feature phase is approved, change its entries to **Current** as part of that phase's
documentation update.

## State Vocabulary

Networking debug output is useful only when the state being measured is named precisely.

| Term | Meaning | Owner |
|---|---|---|
| Authoritative world | The complete gameplay `simulation::World` stepped by the server. | Server only |
| Authoritative sample | State copied from the server into a snapshot. It is historical when the client receives it. | Protocol/replication |
| Latest replicated snapshot | The newest validated authoritative sample installed by a client. | Client runtime |
| Predicted state | Feature 11 controlled-player state advanced locally from owned input. | Client runtime |
| Predicted World | Feature 14 complete gameplay state restored from a checkpoint and replayed through an interaction closure and recorded assumptions. | Planned `MyCore::Rollback` timeline with Dots model |
| Pre-correction state | Prediction immediately before a nonzero reconciliation correction. | Client runtime debug history |
| Presentation state | The transient positions and geometry submitted for drawing. | Dots presentation/client app |
| Interpolated remote state | Presentation sampled between two known remote snapshot states. | Feature 12 presentation |
| Extrapolated remote state | Feature 14 bounded visual-only advancement outside the prediction closure; never gameplay input. | Planned Dots presentation |
| Confirmed consequence | A durable session/gameplay result shown only after authority reports it, even if related reversible World state was predicted. | Server decision/client display |

A native client cannot observe the server's live current position. A client debug ghost labeled
**authoritative** must mean **latest received authoritative sample** and show its snapshot age.

## Clock and Timeline Vocabulary

The full clock model and state ownership table live in the
[networked prediction reference](networked_prediction_reference.md). Use these labels
consistently in overlays, logs, and non-debug UI:

| Label | Meaning | Comparable between clients? |
|---|---|---|
| `World simulation time` | Latest authoritative server tick divided by 30 Hz. On a client this must be qualified as latest-known unless it is explicitly estimated. | Yes, when referring to the same server tick. |
| `Client session time` | Local steady-clock duration since this client became ready. | No; clients join at different times and own different clocks. |
| `Latest-known world time` | Time encoded by the newest snapshot this client has accepted. Exact but historical. | Server ticks are comparable; freshness is per-client. |
| `Estimated live server time` | Latest server-tick anchor advanced by local elapsed time, optionally with a filtered one-way-delay estimate. | Approximate; clients can disagree. |
| `Remote presentation time` | Feature 12 fractional server-tick cursor used to render remotes about 200 ms behind newest known authority. | Comparable as a server-tick coordinate, but independently buffered per client. |
| `Predicted World tick` | Feature 14 authoritative checkpoint tick plus the count of replayed unacknowledged commands. It labels speculative simulation state, not server “now.” | Comparable only with its stated authoritative base and replay range. |
| `Snapshot ID` | Per-client snapshot ordering ID. | No; never interpret it as elapsed time. |
| `Owned prediction extent` | Rollback snapshot/server tick and ACK plus the replayed unacknowledged input range. | Not a single time value. |

Avoid the ambiguous label `estimated world time`; use `estimated live server time` when estimating
server “now,” or `remote presentation time` when describing the delayed scene. A non-debug timer
aligned with the scene should use remote presentation time. A match timer may display an estimated
server deadline locally, but only the server decides whether the deadline has passed.

Reconciliation never rewinds client session time or the server timeline. It rebuilds only owned
predicted state from a newer authoritative base and then smooths the visible correction. Feature
12 presentation-cursor recovery is likewise not authoritative gameplay rollback.

### Compensation clock status

Feature 13's Gameplay tab calculates one narrow deadline estimate from the latest replicated
server tick plus local steady-clock time since that snapshot arrived. It is an unfiltered
presentation countdown and does not add a general live-server clock. It never drives respawn
eligibility, prediction, reconciliation, or simulation; the server's current tick remains the
only eligibility decision.

Feature 11 does not estimate live server time. Owned prediction uses local input steps and server
ACKs; reconciliation replays the retained input suffix; correction smoothing decays a spatial
offset over a fixed 100 ms of local steady time. Network conditions can change correction
frequency and magnitude, but not that duration.

Feature 12 also does not estimate or render server “now.” Its remote presentation cursor
uses server ticks and targets `newest received tick - 6`. It advances at 100% speed within a
`±0.25`-tick deadband and otherwise uses
`clamp(1 + 0.02 * tick_error, 0.95, 1.05)`. The target remains 200 ms; RTT and jitter metrics are
observations, not inputs to an adaptive delay policy in Feature 12. When no newer bracket exists,
the cursor freezes at rate `0.0` until a newer endpoint arrives.

Feature 14's planned adaptive command buffer is also not a live-server-time estimator. It targets
two queued server commands, filters reported queue depth with EWMA `alpha = 1/8`, leaves a
`1.5..2.5` deadband, and applies only this bounded client cadence correction:

```text
rate scale = clamp(1 + 0.025 * (2 - smoothed depth), 0.95, 1.05)
```

The overlay must keep queue depth, cadence scale, predicted tick, estimated live server time, and
remote presentation time as separate values. None changes the authoritative 30 Hz server tick.

Do not label either mechanism `lookahead smoothing`: owned prediction advances known local intent,
while remote interpolation intentionally renders older known authority. The networking guide's
[compensation section](server_authoritative_networking_guide.md#which-compensation-uses-which-clock)
contains the formulas, network-change behavior, and the explicitly deferred live-clock estimator.

## Current Dots Overlay

The Dots-owned Dear ImGui UI has a compact **Dots game state** panel fixed at the top left, a
left lower **Dots session** pane with **Runtime**, **Network**, and **Gameplay** tabs, and a right
lower **Dots diagnostics** pane with **Prediction**, **Interpolation**, and **Tools** tabs.
`[debug].enabled` defaults to `true`; set it to `false` to hide these panes, suppress world-space
diagnostic layers, and prevent the UI from receiving input. Disabling debug does not change
simulation or gameplay presentation. Fault injection and visual-layer controls live under Tools
rather than extending the Prediction metrics view. `MyCore::DebugUI` owns ImGui integration, but
the fields and their meanings remain Dots-owned. Subdued explanatory descriptions and visual-layer
legends wrap to their pane's available width; disabled or unavailable values remain distinct
interaction/state output.

### World and presentation fields — Current

The top-left game-state panel shows **Players** and **Authoritative player**. The latter is the
controlled player's latest position in the offline world or latest accepted replicated snapshot;
it is not the locally smoothed presentation position.

| Label | Source and units | Meaning |
|---|---|---|
| `Input` | Client configuration | Active mouse, keyboard, or hybrid input mapping mode. |
| `Presentation` | Client mode | Offline presentation mode, `NETWORKED PREDICTED` while playing, or `NETWORKED SPECTATOR` after a confirmed spectator transition. Remote entities use Feature 12's delayed known-authority presentation frame. |
| `Tick` | Offline world tick or latest replicated server tick | In offline play this is the local world tick. In networked play it is the tick stored in the latest accepted server snapshot. |
| `Players` | Current offline or replicated entity collection | Number of player entities visible to this client state. It is not the server's total connected-client count. |
| `Food` | Current offline or replicated entity collection | Number of food entities visible to this client state. |
| `Grid cells` | Offline authoritative/local world | Occupied spatial-grid cells. This is absent in networked presentation because replicated state does not own the server grid. |
| `Snapshot` | Replicated world | Latest accepted per-client snapshot sequence ID. This is absent offline. |

### Replication fields — Current

| Label | Source and units | Meaning |
|---|---|---|
| `Snapshot age` | Client steady clock, milliseconds | Time since the newest accepted snapshot arrived. This includes neither its time on the server nor an estimate of one-way latency. |
| `Receive rate` | Accepted snapshots in the last second | Application-level accepted snapshot rate. Stale or invalid packets do not count. Target is currently 15 snapshots/s. |

Snapshot age describes freshness of the client's replicated view. It must not be presented as
RTT, interpolation delay, or server tick health.

### Input scheduling telemetry — Current

Protocol-v2 full snapshots carry `pending_input_count`, the number of distinct samples left in
this client's bounded authoritative input queue after the snapshot tick. `ReplicatedWorld`
stores the newest value, and the **Prediction** overlay shows its current and runtime high-water
values. This value is not transport queue depth, RTT, or total input across all clients.

The queue capacity is 64 samples. The server consumes at most one sample per client before each
authoritative tick and continues the last installed movement when the queue is empty. With
`[network].input_redundancy = true` (the default), each outgoing packet includes the current
sample and up to two prior unacknowledged samples. Setting it to `false` sends only the current
sample. Overlapping samples are deduplicated by sequence ID.

### Transport fields — Current

Transport values come from `MyCore::NetTransport`. Native values are mapped from
GameNetworkingSockets. In-memory endpoints report connection state but intentionally leave
network-only measurements unavailable.

| Label | Source and units | Meaning |
|---|---|---|
| `State` | Transport connection state | Connecting, connected, closing, disconnected, or failed. |
| `RTT` | Native transport, milliseconds | Transport round-trip estimate. Do not halve it and label the result one-way latency. |
| `Packet loss` | Native transport, percent | Transport-level observed loss. It is distinct from application-rejected or debug-injected packets. |
| `Bytes/s in / out` | Native transport rolling rates | Encoded traffic including all carried Dots messages visible to the backend. |
| `Packets/s in / out` | Native transport rolling rates | Transport packet rates, not protocol message-kind counts. |
| `Queued reliable / unreliable` | Native transport, bytes | Payload currently pending in transport send queues. This is not the server's Feature 11 input queue. |
| `Reliable sent unacked` | Native transport, bytes | Reliable bytes sent but not yet acknowledged by the transport. Ordinary input and snapshots are unreliable. |
| `Queue delay` | Native transport, milliseconds | Estimated delay already introduced by the outbound transport queue. |

An unavailable measurement is shown as `unavailable`, never fabricated as zero.

### Frame and fixed-step fields — Current

| Label | Source and units | Meaning |
|---|---|---|
| `Frame` | Latest client render-loop duration, milliseconds | CPU-side wall time between client frames. It is not GPU presentation latency. |
| `Average` | Bounded rolling client frame average | Recent mean frame duration. |
| `FPS` | Rolling client frame rate | Derived from client frame samples. |
| `Simulation health` | Latest fixed-step overload flags | `OVERLOAD` when the latest frame hit its step cap, missed its deadline, or discarded time. |
| `Tick rate` | Rolling completed fixed steps / target | Offline simulation steps, or networked client input-send steps. In native mode this is not the server's measured tick rate. |
| `Steps` | Current frame | Fixed steps executed in this render frame. |
| `Excess` | Fixed-step accumulator | Whole steps left pending when the per-frame step limit was reached. |
| `Simulation` | Current frame, milliseconds | Time spent executing the client fixed-step work. In current networked mode this primarily covers input send/poll work. |
| `Backlog` | Fixed-step accumulator, milliseconds | Fractional and retained fixed-step time awaiting processing. |
| `Catch-up / cap hits` | Lifetime client counters | Frames with multiple steps and frames that reached the configured maximum. |
| `Deadline misses` | Lifetime client counter | Fixed-step work that exceeded its step duration. |
| `Discarded time` | Lifetime client duration | Time deliberately excluded from the client accumulator after frame clamping or backlog caps. |

The standalone server owns its own 30 Hz heartbeat. The graphical client's current fixed-step
panel cannot be used to prove that a remote server is maintaining 30 Hz.

## Current Offline Presentation Debugging

`[debug].presentation_mode` currently supports:

- `interpolated`: draw the local player and camera from the same interpolated fixed-step sample.
- `fixed`: draw the latest completed simulation position.
- `comparison`: draw the interpolated player plus a transparent white outline at the latest
  fixed-step position.

These modes change only presentation. Collision, food consumption, and world state always use
the fixed-step simulation position.

## Current Spectator Presentation

The graphical network client enters spectator presentation only after replicated session mode is
`Spectating`. It follows the confirmed killer from the same delayed remote sample used to draw
that player. Free-camera position and zoom are local presentation state. Missing follow geometry
switches the camera to free mode at its last valid position; it never selects an unconfirmed
replacement.

While spectating, the **Network** and **Interpolation** tabs remain live. The controlled entity is
shown as `none`, and **Prediction** reports that local prediction is unavailable instead of
displaying camera state as predicted gameplay. **Tools** shows only its active remote-presentation
section, endpoint-outline toggle, and applicable legend; it omits local prediction faults,
prediction/replay layers, and correction controls. The **Gameplay** tab reports the confirmed
lifecycle state; free-camera position, zoom, and camera mode remain presentation state and are not
reported as authoritative gameplay.

## Current Logs and Profiling

Important log categories include:

| Category | Meaning |
|---|---|
| `dots.client` | Client startup and general runtime information. |
| `dots.client.session` | Client transport, handshake, assigned identity, disconnect lifecycle, and newly confirmed absorption, session-mode, follow-target-loss, and respawn-result transitions. |
| `dots.client.simulation` | Client fixed-step overload warnings, escalation, and recovery. |
| `dots.client.prediction` | Prediction history pressure/recovery, hard resyncs, replay-budget warnings, and explicit debug fault injection. |
| `dots.server` | Headless server startup, listen address, tick lifetime, and shutdown. |
| `dots.server.session` | Connection acceptance, assigned players, authoritative defeat/respawn decisions, follow-target loss, rejected packets, liveness timeouts, and cleanup. |

Feature 7 added Tracy zones for the client frame, fixed-step work, presentation extraction, and
render submission. Phase 11.2 adds `Dots prediction reconciliation`, covering bounded scratch
history replay and its atomic state commit. Tracy is on-demand; dormant instrumentation does not
mean the zones were removed.

## Prediction and Reconciliation Output — Current

`Dots::ClientRuntime` currently predicts controlled-player movement immediately after each
successful input send. Every newer snapshot is validated in scratch state, the acknowledged
history prefix is discarded, and at most 256 remaining inputs are replayed in the same client
frame before replicated and predicted state commit together. The graphical client draws the
controlled player and follows it with the camera from one presentation position: corrected
prediction plus the current visual-only smoothing offset. Remote entities still draw from the
delayed Feature 12 remote presentation frame.

The runtime exposes `predicted_position()`, `pre_correction_position()`,
`latest_replay_path()`, `latest_correction_replay_path()`, and `prediction_statistics()`.
`pre_correction_position()` and the correction-specific replay path update only after a nonzero
correction. Presentation copies them for two seconds of visual retention. A history-capacity hard
resync clears prediction history, smoothing, and retained correction visuals.

The **Network** tab's Session section shows:

- Runtime/connection state and protocol version.
- Server-assigned client ID.
- Controlled entity ID and transport connection handle.
- Latest snapshot ID/server tick and local input tick.

The server and client tick values are shown separately. Until a future tick-synchronization
feature defines their mapping, subtracting them does not produce a meaningful latency value.

The **Prediction** tab rows are:

| Field | Current meaning and lifetime |
|---|---|
| Redundancy | Whether outgoing packets repeat up to two retained unacknowledged samples. |
| Last sent input | Newest successfully sent and recorded input sequence, or invalid before the first send. |
| Last acknowledged input | Newest sequence the latest accepted snapshot says the server processed, or invalid before the first ACK. |
| Command lead | Count of successfully sent inputs newer than the latest ACK. It can exceed retained history after a deliberate hard resync. |
| History use/high-water | Current and runtime-maximum occupancy of the fixed 256-entry replay ring. Capacity is a correctness bound, not an adaptive target. |
| Server pending input | Current and runtime-high-water depth of this client's authoritative 64-entry server input queue, as reported by snapshots. |
| Rollback base | Snapshot ID, server tick, and ACK used for the latest successful reconciliation. |
| Replay count | Latest, lifetime-total, and runtime-maximum numbers of inputs replayed after installing an authoritative base. |
| Replay duration | Latest, last-120-reconciliation average, and runtime maximum scratch-replay/commit CPU duration in milliseconds. |
| Reconciliation count | Newer accepted snapshots processed after prediction became ready. |
| Correction count | Reconciliations whose final prediction moved by more than `0.0001` world units. |
| Correction distance | Latest and runtime-maximum distance between prediction before reconciliation and the fully replayed result. |
| Corrections/min | Count of nonzero corrections in the trailing 60 seconds of the client steady clock. |
| Replay over budget | Lifetime count of reconciliations exceeding 2 ms; warnings are rate-limited to once per five seconds. |
| Hard resync | Lifetime count of full-ring recoveries that snap prediction to the newest replicated controlled-player sample and clear history/debug replay state. |
| Smoothing offset | Current presentation-only displacement vector and magnitude. It decays linearly to zero over 100 ms without modifying predicted state. |
| Injected faults | Pending/total client-only packet drops and the number of explicit prediction-error injections. These do not alter transport loss metrics. |

History-pressure warnings begin above 75% occupancy and are rate-limited to once per five
seconds while pressure persists. Recovery is logged once occupancy returns to 75% or below. All
counts and high-water values reset with a new `Dots::ClientRuntime` instance. The overlay colors
history utilization green below 50%, yellow at 50%, orange at 75%, and red at 90%.

### Current prediction world-space legend

| Visual | Meaning |
|---|---|
| Filled player | Actual presented position and camera target. |
| White outline | Corrected predicted simulation position. |
| Orange outline | Latest received authoritative sample. It is historical, not the server's live position. |
| Magenta outline | Prediction immediately before the most recent nonzero correction. |
| Purple markers | Results of replayed unacknowledged inputs after the rollback base. |

Correction-specific magenta/purple visuals remain for two seconds. Slight radius offsets keep
coincident outlines visible.

### Current prediction fault controls

The **Tools** tab in the right-hand **Diagnostics** pane owns these controls:

- Inject `+1` world unit of client-only X or Y prediction error.
- Force `+1` world unit of client-only position drift along the last non-zero movement direction.
  This directly corrupts predicted position; it does not submit an extra movement input. The
  overlay shows the direction, and the control is disabled until the client has sent non-zero
  movement. Retaining the last direction makes the control usable while ImGui temporarily
  captures mouse steering.
- Drop the next three input packets while continuing local prediction.
- Show/hide state layers and replay markers.
- Clear retained correction visuals.

The layer and replay toggles default on. During an injected-drop burst, the **Prediction** tab in
the right-hand **Diagnostics** pane shows completed and remaining drop counts and disables
starting an overlapping burst. Completion is
retained as a green receipt for two seconds, even when catch-up consumes all three drops between
rendered frames. Suppressed sends still record and predict their input exactly as deliberate
network loss would. Injected drops have a separate counter and are never added to transport
packet-loss measurements.

## Remote Interpolation Output — Current

Feature 12 uses a 32-sample presentation buffer and a remote render cursor delayed by six server
ticks, currently 200 ms. The client receives every accepted snapshot from a runtime poll, then
adapts it into the renderer-free remote history.

The **Interpolation** tab in the right-hand **Diagnostics** pane reports:

| Field | Meaning |
|---|---|
| Buffer fill | Samples stored out of the fixed capacity. |
| Coverage | Difference between oldest and newest buffered server ticks, shown in ticks and milliseconds. |
| Target delay | Intentional six-tick separation between newest authority and remote presentation. |
| Current delay | Actual newest-tick minus presentation-cursor distance. It can be temporarily negative when an underrun freezes a cursor already beyond the newest known tick. |
| Presentation tick | Fractional server-tick coordinate used to draw remotes. |
| Cursor rate | Current 0.95–1.05 presentation-clock adjustment, or `0.0` while holding. This does not change local input rate. |
| Brackets | Older/newer snapshot IDs and server ticks enclosing the cursor, plus alpha. |
| Jitter | Latest and EWMA deviation between observed and server-implied snapshot spacing. |
| Late snapshot | Snapshot arriving at or behind the current presentation cursor. |
| Hold/underrun | Continuous period with no newer bracket; remotes hold rather than extrapolate. The overlay reports current state, episode/recovery counts, and current/last/maximum/total duration. |
| Hard rebase | Forward-only presentation cursor reset after recoverable bracketing is lost or the cursor falls more than six ticks behind. |
| Delayed creates/removes | Remote entity lifecycle transitions actually exposed by sampled presentation frames. |

### Feature 12 world-space legend

For every remote player:

- Filled circle: interpolated presentation.
- Cyan outline: older known authoritative endpoint.
- Blue outline: newer known authoritative endpoint.
- Cyan-to-blue dots: short connector from the older endpoint toward the newer endpoint.

**Show remote endpoint outlines** controls the cyan/blue outlines and their cyan-to-blue
connectors for every remote player without changing remote interpolation or gameplay. Cyan is the
older bracket endpoint; blue is the newer endpoint that the delayed cursor is approaching. The
**Interpolation** tab lists endpoint values for the lowest-ID sampled remote player as a
representative example. Endpoint circles are debug-only and never feed presentation or gameplay
state.

Feature 12 presentation-clock correction and future local input-clock synchronization solve
different problems. Feature 12 keeps a delayed remote cursor centered in known snapshots. A
future tick-synchronization feature would map local input ticks to estimated server ticks and
adjust command slack.

## Authoritative Gameplay Output — Current

Feature 13 adds a separate **Gameplay** tab. It reports server-confirmed lifecycle rather than
extending movement prediction metrics:

| Field | Meaning |
|---|---|
| Client/session | Server-assigned client ID and confirmed `Playing` or `Spectating` mode. |
| Owned pieces | Confirmed owned-piece count plus primary entity ID, if any. |
| Killer/follow | Confirmed killer or current follow entity; absence must be explicit. |
| Defeat tick | Server tick at which the last authoritative piece was absorbed. |
| Respawn available | Server deadline tick and `Estimated countdown`. The countdown is latest snapshot tick plus snapshot receipt age, clamped at zero; it never decides eligibility. |
| Latest absorption | Authoritative absorber, victim, transferred mass, and tick for an event involving this session. |
| Respawn request | Latest request input sequence and confirmed accepted/rejected result. An input ACK alone is not success. |

Spectator camera mode, free-camera position, zoom, and follow-target availability are presentation
state. They do not imply control of an authoritative entity.

Lifecycle fields are repeated in full snapshots and remain visible after their transition packet.
Absent entity IDs, ticks, events, or requests display `none`; a countdown displays `unavailable`
until both a deadline and snapshot-age anchor exist. The tab labels the estimate as
presentation-only because a local `eligible` display can precede the server receiving or accepting
a request.

## Complete Rollback Output — Feature 14 Planned

Feature 14 adds a separate **Rollback** tab while retaining Feature 11 Prediction and Feature 12
interpolation diagnostics for comparison.

| Field | Meaning |
|---|---|
| Prediction profile | `InteractionClosure`, `FullReplicated`, or `OwnedMovement`, plus predicted/interpolated/extrapolated/confirmed entity counts. |
| Prediction scope | Scope epoch, included mechanics, entity/owner/global state domains, causal subscriptions, closure seed/count, replay horizon, and `IncompleteClosure` fallback reason. |
| Replay coordinates | Authoritative snapshot/tick, predicted tick, input ACK, and exact replay sequence range. |
| Prediction lead | Predicted tick minus its stated authoritative base tick. This replay extent is not RTT, snapshot age, or estimated live server time. |
| History | Occupied frames out of 64, replay tick count, checkpoint bytes, and hard-resync reason. |
| State digest | Authority checkpoint schema/digest and corresponding predicted diagnostic digest. Typed differences, not hash equality, remain the correctness source. |
| Replay duration | Latest, p50, p95, p99, and maximum same-frame replay time; 2 ms is a warning, not a partial-replay cutoff. |
| Continuous divergence | Position, velocity, mass, radius, and deadline corrections that preserve topology. |
| Structural divergence | Entity create/remove, ownership, component-set, split, merge, and elimination corrections. |
| Predicted spawns | Pending, matched, rejected, authority-only, and ambiguous prediction-key counts. Ambiguity causes hard resync. |
| Event lifecycle | `FirstPredicted`, `Revised`, `Retracted`, `Confirmed`, and `AuthorityOnly` counts, with the selected stable event key. |
| Consequence delivery | Per-policy delivered, suppressed, revised, canceled, confirmed, and authority-only counts for `PredictOnce`, `PredictCancelable`, and `ConfirmOnce`. |
| Authority receipts | Latest received/acknowledged sequences, server/client pending depth, duplicate count, and conflict/overflow failures. |
| Command buffer | Target/latest/EWMA server queue depth, cadence scale, low/high events, and accumulated phase correction. |
| Remote assumption | Source snapshot and tick range over which last-known remote movement was held; remote edge actions remain zero. |
| Outside-closure presentation | Latest-authority age, visual extrapolation age/cap, hold count, closure-entry transition, and interpolation fallback. No gameplay is executed for this layer. |

The selected-entity world overlay draws independently labeled latest-known authoritative,
predicted, Feature 12 interpolated, bounded extrapolated, pre-correction, and smoothed
presentation layers. Structural markers identify speculative spawns, removals, matches, and
rejections. Event markers show stable key and transition. A layer is hidden or marked
unavailable when its state source does not exist; zero is not used as a placeholder.

Fault tools cover position or mass divergence, forced split rejection, spawn-classification
mismatch, action-packet suppression, remote held-input divergence, repeated rollback of one event
key, and duplicate/conflicting authority receipts. Each fault has a durable
armed/triggered/completed receipt and remains separate from measured transport loss.

Same-frame replay is the planned baseline. Branch 14h records entity count, replay ticks,
checkpoint bytes, topology changes, RTT/jitter/loss grouping, client-frame impact, and replay
duration. Multi-frame resimulation is not shown as an available mode unless a separate reviewed
implementation exists; the decision thresholds and atomic-commit invariants live in
[`rollback_prediction_design.md`](rollback_prediction_design.md#same-frame-replay-and-deferred-multi-frame-work).

## Troubleshooting Patterns

### High RTT but fresh snapshots

The connection has network travel time, but snapshot delivery remains regular. Feature 11 local
prediction should hide input response latency; Feature 12 intentionally keeps remotes behind the
newest snapshot.

### Snapshot age rises while transport remains connected

The application is not accepting newer snapshots. Check loss, snapshot send cadence, protocol
decode/rejection logs, and server health. Transport state alone does not prove replication flow.

### Command lead and server input queue grow together

The client is producing input faster than the server consumes it, the server is overloaded, or
clock drift is accumulating. Feature 11 observes this trend but does not speed or slow local
simulation.

### Frequent corrections with low loss

Check shared movement operations, tick application order, ACK semantics, collision/mass events
that are intentionally not predicted, and floating-point divergence. Use the orange, magenta,
white, and replay markers to identify where states first disagree.

### Smoothing offset never settles

Corrections are arriving more frequently than the 100 ms residual can decay. Inspect correction
distance/frequency, replay count, server input depth, and fault-injection state. Do not lengthen
smoothing until the underlying divergence is understood.

### History use exceeds 75%

Snapshot acknowledgements have stalled for several seconds. Inspect snapshot age, loss,
connection state, and server health. At full capacity the client performs a visible hard resync
rather than retaining an unreplayable prediction.

### Remote cursor repeatedly holds

The buffer lacks a newer endpoint. Compare buffer coverage, current delay, jitter, late samples,
and packet loss. The cursor rate is `0.0` during the hold and resumes only after a newer endpoint
arrives. Holds are safer than inventing remote movement.

### Remote cursor rate remains at 0.95 or 1.05

The newest-snapshot distance is persistently outside its target. This may indicate sustained
clock drift, bursty delivery, or insufficient fixed interpolation delay. Use measured data before
adding adaptive delay.

### Structural corrections repeat — Feature 14 planned

Compare prediction profile/closure, predicted-spawn classification, held remote assumptions,
checkpoint configuration, and the first topology tick that differs. Position smoothing cannot
repair an entity create/remove, ownership, split, merge, or deadline mismatch.

### A one-shot consequence repeats — Feature 14 planned

Inspect its stable event key, transition history, handler policy, and suppression count.
`PredictOnce` and `ConfirmOnce` are keyed per handler, so replaying or confirming the same key
must not invoke that handler twice. A changing key indicates incorrect game identity; a stable
key with repeated delivery indicates an occurrence-ledger defect.

### A predicted effect remains after rejection — Feature 14 planned

Confirm the handler uses `PredictCancelable`, produced a stored lifecycle token, and received a
`Retracted` transition. `PredictOnce` deliberately cannot erase a cue already perceived; use it
only when one brief false positive is acceptable.

### Hard resyncs rise — Feature 14 planned

Group reasons by missing history, capacity exhaustion, incompatible checkpoint, and ambiguous
prediction key. Then compare snapshot age, ACK progress, history occupancy, and configuration
validation. A hard resync is correct recovery, but a repeated reason is a defect or an undersized
bound that needs evidence.

### Command cadence stays clamped — Feature 14 planned

Compare latest and EWMA server queue depth with packet loss, input ACK progress, and empty/high
events. A persistent clamp may reveal clock drift or delivery pressure; it does not mean client
session time or the server tick rate changed.

### Same-frame replay exceeds its budget — Feature 14 planned

Correlate replay duration with replay ticks, predicted entity count, checkpoint bytes, and
structural changes. A 2 ms warning does not permit partial state. Use the documented p99 and
frame-overrun research thresholds before proposing a multi-frame scheduler.

## Impairment Testing

Native clients and the session launcher accept outgoing fake lag and loss. Lag is one-way per
process; applying 50 ms at the server and client produces approximately 100 ms transport RTT.

Use impairment to answer a specific question:

- Prediction responsiveness: add RTT, move the local player, and compare prediction with the
  latest authoritative sample.
- Reconciliation: use Feature 11 injected error or packet drops and inspect replay/correction
  output.
- Redundancy: compare the same drop schedule with redundancy enabled and disabled.
- Remote interpolation: vary loss/jitter schedules and inspect known endpoints, buffer coverage,
  cursor rate, and holds.
- Complete rollback: after Feature 14 lands, compare prediction profiles and inspect replay,
  structural divergence, command-buffer, and state-layer output under identical impairment.

Random transport loss is useful for play testing. Deterministic tests should use controlled
arrival schedules so correction and buffer metrics have exact expected values.

## Maintaining This Guide

When changing observability:

1. Identify the owner and source of the value.
2. Define units, update cadence, lifetime/reset behavior, and unavailable behavior.
3. State what the value must not be confused with.
4. Add deterministic metric tests independent of ImGui where practical.
5. Update this guide, README when the workflow changes, and the owning feature plan status.

Do not add an unlabeled zero for unavailable transport data, call a delayed sample live server
state, mix presentation delay with RTT, or present a client-side timing counter as server health.
