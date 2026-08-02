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

- **Current:** Feature 14 complete-World prediction/reconciliation, persistent rollback-aware
  presentation, consequence handlers, bounded outside-closure extrapolation, adaptive command
  timing, bounded consequence retirement, Rollback diagnostics, explicit correction generations,
  typed receipts for interactive position/loss faults, deterministic scale workloads, bounded
  native impairment soaks, receive-window input backpressure, direct spectator stress roles,
  server overload timing, and the measured same-frame replay decision are implemented. Feature
  12 delayed interpolation remains the fallback/comparison and selectable delayed-spectator
  path, while live spectators use bounded Dots kinematic extrapolation only for its uncovered
  underrun tail. Feature 13's authoritative lifecycle and spectator presentation remain
  implemented.

When a feature phase is approved, change its entries to **Current** as part of that phase's
documentation update.

## State Vocabulary

Networking debug output is useful only when the state being measured is named precisely.

| Term | Meaning | Owner |
|---|---|---|
| Authoritative world | The complete gameplay `simulation::World` stepped by the server. | Server only |
| Authoritative sample | State copied from the server into a snapshot. It is historical when the client receives it. | Protocol/replication |
| Latest replicated snapshot | The newest validated authoritative sample installed by a client. | Client runtime |
| Predicted state | Owned projection of the complete predicted World advanced locally from owned input. | Client runtime |
| Predicted World | Complete gameplay state restored from a verified checkpoint and replayed through an interaction closure and recorded assumptions. | Current `MyCore::Rollback` timeline with Dots model |
| Pre-correction state | Prediction immediately before a nonzero reconciliation correction. | Client runtime debug history |
| Presentation state | The transient positions and geometry submitted for drawing. | Dots presentation/client app |
| Interpolated remote state | Presentation sampled between two known remote snapshot states. | Feature 12 presentation |
| Extrapolated remote state | Feature 14 bounded visual-only advancement outside the prediction closure; never gameplay input. | Current Dots presentation |
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

Reconciliation never rewinds client session time or the server timeline. It rebuilds only the
scoped predicted World from a newer authoritative base and then smooths the visible
primary-position correction. Feature
12 presentation-cursor recovery is likewise not authoritative gameplay rollback.

### Compensation clock status

Feature 13's Gameplay tab calculates one narrow deadline estimate from the latest replicated
server tick plus local steady-clock time since that snapshot arrived. It is an unfiltered
presentation countdown and does not add a general live-server clock. It never drives respawn
eligibility, prediction, reconciliation, or simulation; the server's current tick remains the
only eligibility decision.

Feature 14 prediction does not estimate live server time. The timeline uses local input steps,
explicit remote movement assumptions, and server ACKs. Reconciliation refreshes those derived
remote assumptions from newest authority and replays the retained input suffix; sampled local
commands remain unchanged. Correction smoothing decays a spatial offset over a fixed 100 ms of
local steady time. Network conditions can change correction frequency and magnitude, but not
that duration.

Feature 12 also does not estimate or render server “now.” Its remote presentation cursor
uses server ticks and targets `newest received tick - 6`. It advances at 100% speed within a
`±0.25`-tick deadband and otherwise uses
`clamp(1 + 0.02 * tick_error, 0.95, 1.05)`. The target remains 200 ms; RTT and jitter metrics are
observations, not inputs to an adaptive delay policy in Feature 12. When no newer bracket exists,
the cursor freezes at rate `0.0` until a newer endpoint arrives.

Feature 14's adaptive command buffer is also not a live-server-time estimator. It targets
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
lower **Dots diagnostics** pane with **Rollback**, **Interpolation**, and **Tools** tabs.
`[debug].enabled` defaults to `true`; set it to `false` to hide these panes, suppress world-space
diagnostic layers, and prevent the UI from receiving input. Disabling debug does not change
simulation or gameplay presentation. A confirmed kill/defeat banner is non-debug gameplay UI:
it still uses the shared ImGui rendering context when debug panes are disabled, never captures
input, and fades after 1.5 seconds. Fault injection and visual-layer controls live under Tools
rather than extending the Rollback metrics view. `MyCore::DebugUI` owns ImGui integration, but
the fields and their meanings remain Dots-owned. Subdued explanatory descriptions and
visual-layer legends wrap to their pane's available width; disabled or unavailable values remain
distinct interaction/state output.

### World and presentation fields — Current

The top-left game-state panel shows **Players** and **Authoritative player**. The latter is the
controlled player's latest position in the offline world or latest accepted replicated snapshot;
it is not the locally smoothed presentation position. While a confirmed respawn deadline exists,
the panel also shows the authoritative deadline tick and the same presentation-only countdown as
the Gameplay tab. It labels a zero estimate `eligible`; the server still decides eligibility.

| Label | Source and units | Meaning |
|---|---|---|
| `Input` | Client configuration | Active mouse, keyboard, or hybrid input mapping mode. |
| `Presentation` | Client mode | Offline presentation mode, `NETWORKED PREDICTED` while playing, or `NETWORKED SPECTATOR` after a confirmed spectator transition. Playing remotes use the configured extrapolated/interpolated source. Both spectator modes normally interpolate buffered authority; live alone extrapolates a bounded underrun tail. |
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

Protocol-v5 full snapshots carry `pending_input_count`, the number of distinct samples left in
this client's bounded authoritative input queue after the snapshot tick, and
`input_receive_through`, the highest fresh input sequence authority currently accepts.
`ReplicatedWorld` stores both. These values are not transport queue depth, RTT, or total input
across all clients.

The advertised receive window is 32 samples and the defensive queue capacity is 64. A conforming
client cannot reach the latter. It keeps grant-blocked commands in a fixed unsent outbox, pauses
production at eight unsent entries, and resumes at two. The server consumes at most one sample
per client before each authoritative tick and continues the last installed movement when the
queue is empty. With
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
| `Tick rate` | Rolling completed fixed steps / target | Offline simulation steps, or networked client scheduling steps. Paused and spectator steps need not produce input. In native mode this is not the server's measured tick rate. |
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
`Spectating`. `[spectator].presentation_mode` defaults to `live`. Both modes normally use the
six-tick authoritative interpolation buffer. When that cursor exhausts its newest endpoint,
`live` extrapolates only the uncovered movement/launch tail for at most another six ticks/200 ms;
`delayed` holds immediately. Neither mode runs spectator rollback or gameplay mechanics. A
defeated Player follows the confirmed killer from the same selected, persistently composed sample
used to draw that player. A direct Spectator joins with no killer/deadline and begins in free
camera. Free-camera position and zoom are local presentation state. Missing follow geometry
switches the camera to free mode at its last valid position; it never selects an unconfirmed
replacement.

While spectating, the **Network** and **Interpolation** tabs remain live. The controlled entity is
shown as `none`, and **Rollback** reports that local prediction is unavailable instead of
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
| `dots.client.session` | Client transport, initial and repeated handshake attempts, idempotent repeated welcomes, assigned identity, disconnect lifecycle, and newly confirmed absorption, session-mode, follow-target-loss, and respawn-result transitions. |
| `dots.client.simulation` | Client fixed-step overload warnings, escalation, and recovery. |
| `dots.client.presentation` | Rejection of a noncanonical/stale remote extrapolation sample, including candidate and prior snapshot coordinates. Such rejection fails the session rather than drawing unvalidated state. |
| `dots.client.consequence` | Non-retried Dots consequence-handler failures after an otherwise successful rollback commit, plus typed malformed-batch contract failures. A contract failure includes batch kind, change index, error code, and count before terminating only that client session. |
| `dots.client.prediction` | Prediction history pressure/recovery, hard resyncs, replay-budget warnings, explicit debug fault injection, and fatal scope/timeline operation failures. Timeline failures name the operation, engine rollback error, and any nested Dots model/checkpoint/tick error; scope failures name the failed build or projection and scope epoch/counts. |
| `dots.client.prediction.frontier` | Entry into deliberately deferred prediction, recovery by ordinary replay or validated ACK catch-up, and fatal authority failures with authoritative ACK, timeline ACK/submission, retained input range, deferred count, and last-sent input. Transition logs require `debug.prediction_log_level` `info` or `debug`; failure context is unconditional. |
| `dots.client.prediction.scope` | Successful scope-epoch changes with replay depth, horizon, and before/after causal-owner, event-owner, player, and food membership counts when `debug.prediction_log_level` is `info` or `debug`. |
| `dots.client.prediction.reconciliation` | At `debug.prediction_log_level = "debug"`, each nonzero largest common remote-player displacement across an installed predicted head, including whether the before/after heads represent the same tick. Zero-displacement installs are omitted. |
| `dots.server` | Headless server startup, listen address, tick lifetime, and shutdown. |
| `dots.server.session` | Connection acceptance, assigned players, authoritative defeat/respawn decisions, follow-target loss, rejected packets, liveness timeouts, and cleanup. |

Feature 7 added Tracy zones for the client frame, fixed-step work, presentation extraction, and
render submission. Phase 11.2 adds `Dots prediction reconciliation`, covering bounded scratch
history replay and its atomic state commit. Tracy is on-demand; dormant instrumentation does not
mean the zones were removed.

## Prediction and Reconciliation Output — Current

`Dots::ClientRuntime` currently predicts a complete interaction-closed World immediately after
each successful input send. Every newer snapshot ID is validated and digest-checked, the
acknowledged history prefix is discarded, and at most 256 remaining inputs are replayed with
remote assumptions refreshed from newest authority in the same client frame before replicated
and predicted state commit together. A later snapshot at the same server tick uses an explicit
authority refresh instead of reinitializing or discarding future history. Their sampled local
commands remain unchanged. The
interaction closure uses the greater of the actual retained suffix length and a five-tick
operating floor as its horizon; the 256-entry ring capacity is only a storage and hard-resync
bound. The client verifies the closure before each predicted step, retains a safe existing
superset when ACKs make the new requirement smaller, and rebuilds from newest authority under a
new epoch only when new causal membership is required, because old stimuli contain no
assumptions for newly admitted entities.

The graphical client draws all predicted food and player topology inside that interaction island;
duplicates are removed from the selected outside-closure frame. The controlled primary and
camera use corrected prediction plus the current visual-only smoothing offset. Remote entities
outside the island default to latest-snapshot movement/launch extrapolation capped at six ticks
and then hold. Delayed interpolation remains selectable for playing clients. Both spectator modes
use it normally; live adds only a bounded underrun-tail fallback.

The runtime exposes `predicted_world()`, `predicted_primary_entity_id()`,
`predicted_owned_entity_ids()`, `predicted_scope_entity_ids()`,
`latest_prediction_identity_remaps()`,
`predicted_position()`, `pre_correction_position()`, `latest_replay_path()`,
`latest_correction_replay_path()`, `take_prediction_event_batches()`, and
`prediction_statistics()`.
`pre_correction_position()` and the correction-specific replay path update only after a nonzero
correction. Presentation copies them for two seconds of visual retention. A history-capacity hard
resync clears prediction history and rebuilds the timeline from the newest verified checkpoint;
presentation smoothing and retained correction visuals reset.

The **Network** tab's Session section shows:

- Runtime/connection state and protocol version.
- Server-assigned client ID.
- Controlled entity ID and transport connection handle.
- Latest snapshot ID/server tick and local input tick.

The server and client tick values are shown separately. `Local input tick` is the contiguous
client fixed-step sampling ordinal placed in `InputSample::client_tick`; the current server
schedules by per-session input sequence and arrival, not by that value. Until a future
tick-synchronization feature defines a validated mapping, subtracting local input tick from
server tick does not produce a meaningful latency or queue-delay value.

The current overlay also does not claim an exact sequence-to-server-tick mapping. A snapshot ACK
proves only that the named cumulative input frontier was processed no later than that snapshot's
server tick. Per-input receive tick, authoritative application tick, and queue wait are planned
as bounded server provenance diagnostics rather than inferred from unrelated clocks.

The **Rollback** tab rows are:

| Field | Current meaning and lifetime |
|---|---|
| Join role | Requested and server-accepted `Player` or `Spectator` role. |
| Input ACK / receive grant / transmitted through | Latest authority-processed command, highest fresh command authority permits, and newest command actually handed to transport. A direct spectator shows no grant or transmit frontier. |
| Unsent / high-water | Commands accepted into local prediction but not yet transmitted, plus their runtime maximum. |
| Input production | `RUNNING` or hysteresis-controlled `PAUSED`, with lifetime pause count, accumulated pause time, and sent status-heartbeat count. |
| Authority receipts | Highest contiguous sequence semantically accepted, published into a queued post-commit event batch, and echoed as retired by the server; retained/pending-publication payload counts; queued observable event-batch count; replay/external consequence-retirement evidence; per-policy retained-key counts; and cumulative pruning. These fields remain available while Spectating. |
| Redundancy | Whether outgoing packets repeat up to two retained unacknowledged samples. |
| Last accepted input | Newest locally accepted and predicted input sequence, whether already transmitted or still in the unsent outbox. |
| Last acknowledged input | Newest sequence the latest accepted snapshot says the server processed, or invalid before the first ACK. |
| Timeline input submitted | Newest command sequence submitted to predicted simulation. This can lag last-sent input while prediction is deliberately deferred after speculative local elimination. |
| Command lead | Count of successfully sent inputs newer than the latest ACK. It can exceed retained history after a deliberate hard resync. |
| Deferred outside timeline | Retained outer input count newer than the timeline-submitted frontier. These inputs are still sent and are replayed when an authoritative checkpoint restores a viable predicted owner. |
| History use/high-water | Current and runtime-maximum occupancy of the fixed 256-entry replay ring. Capacity is a correctness bound, not an adaptive target. |
| Scope | Current epoch, certified replay horizon, causal owner/player/food counts, separately subscribed event-owner count, and lifetime rebase count. A smaller newly required closure may retain a safe existing superset to prevent presentation ownership from oscillating as ACK depth changes. |
| Server pending input | Current and runtime-high-water depth of this client's authoritative input queue, as reported by snapshots. The normal receive window is 32; 64 is only the defensive capacity. |
| Command buffer | Fixed target, latest and EWMA server depth, bounded cadence scale, accumulated phase correction, low/high observations, two-input prefill count, and discarded producer-overrun count. |
| Rollback base | Snapshot ID, server tick, and ACK used for the latest successful reconciliation. |
| Replay count | Latest, lifetime-total, and runtime-maximum numbers of inputs replayed after installing an authoritative base. |
| Replay duration | Latest, last-120-reconciliation average, and runtime maximum scratch-replay/commit CPU duration in milliseconds. |
| Reconciliation count | Newer accepted checkpoints reconciled after the complete timeline became ready. |
| Correction count | Reconciliations whose final prediction moved by more than `0.0001` world units. |
| Remote entity corrections | Latest and lifetime counts of common remote players whose rebuilt predicted positions moved by more than `0.0001` world units at a reconstructable common tick. Same-head comparisons use the final predicted checkpoint; cross-head comparisons use a retained replay checkpoint at the prior head. Ordinary forward progress with no common checkpoint is excluded. |
| Correction distance | Latest and runtime-maximum distance between prediction before reconciliation and the fully replayed result. |
| Remote correction distance | Latest and runtime-maximum comparable common-tick correction distance across remote predicted players. Each retained correction carries an explicit generation and displacement into presentation; source revision alone never creates a predicted correction residual. |
| Corrections/min | Count of nonzero corrections in the trailing 60 seconds of the client steady clock. |
| Correction ghosts | Active bounded history count/capacity, split into local and remote entries. |
| Replay over budget | Lifetime count of reconciliations exceeding 2 ms; warnings are rate-limited to once per five seconds. |
| Hard resync / ACK catch-up | Lifetime total hard-resync count and the subset selected because validated authority acknowledged a command retained outside the temporarily deferred timeline. Full-ring recovery discards retained input; ACK catch-up discards only the acknowledged prefix and rolls the newer suffix forward. |
| Smoothing offset | Current presentation-only displacement vector and magnitude. It decays linearly to zero over 100 ms without modifying predicted state. |
| Injected faults | Pending/total client-only packet drops and the number of explicit prediction-error injections. These do not alter transport loss metrics. |
| Rollback consequences | Consumed batch count, currently visible cue count, monotonic stinger sequence, all five event-transition totals, and per-handler declared policy with delivered/suppressed/revised/canceled/confirmed/failure totals. |
| Persistent presentation | Active semantic tracks, structural fades, retained motion-trail samples, and cumulative source handoffs, smoothed corrections, and prediction-key identity remaps. |

History-pressure warnings begin above 75% occupancy and are rate-limited to once per five
seconds while pressure persists. Recovery is logged once occupancy returns to 75% or below. All
counts and high-water values reset with a new `Dots::ClientRuntime` instance. The overlay colors
history utilization green below 50%, yellow at 50%, orange at 75%, and red at 90%.

`debug.prediction_log_level` defaults to `off`. Set it to `info` for scope and command-frontier
transition summaries or `debug` to add nonzero remote reconciliation detail. The latter is meant
for short reproduction sessions and does not change prediction or presentation state.

`debug.correction_history_count` selects the combined local/remote world-space correction-history
capacity from 1 through 64 and defaults to 8. This changes diagnostics only.

Input-flow warnings use `dots.client.input` when the unsent outbox pauses at eight and log
recovery at two. `dots.server.input` reports queue pressure at 24, recovery at eight, and
out-of-window rejection with processed/grant/queue/packet frontiers.
`dots.server.simulation` reports standalone-server overload, five-tick catch-up exhaustion,
discarded wall-time debt, and timing recovery.

### Current prediction world-space legend

| Visual | Meaning |
|---|---|
| Filled player/food | Predicted interaction-island topology combined with the configured outside-closure source; the primary uses its smoothed presentation position. |
| White outline | Corrected predicted simulation position. |
| Orange outline | Latest received authoritative sample for each confirmed owned piece. A newly predicted split child has no orange outline until authority confirms it. These are historical samples, not live server positions. |
| Magenta outline | An entity position immediately before a recent nonzero correction. This includes the local primary and comparable remote players. New entries are opaque and older entries fade without changing hue. |
| Purple markers | Results of replayed unacknowledged inputs after the rollback base. |

The filled primary normally overlaps the white outline. Separation is expected only while a real
100 ms local reconciliation residual is active. The persistent World adapter does not smooth the
already-presented local `State` pose a second time.

Each magenta entry remains for at most two seconds and the combined history retains only the
configured last N entries. Remote entries are recorded only when authority rebuilds the same
predicted head tick; movement from an older head tick to a newer one is forward simulation, not a
misprediction. Purple markers still describe the latest local replay and remain for two seconds.
Slight radius offsets keep coincident outlines visible. Stable magenta preserves layer meaning;
opacity communicates age, avoiding rotating hues that could be confused with authority,
prediction, or interpolation layers.

### Current prediction fault controls

The **Tools** tab in the right-hand **Diagnostics** pane owns these controls:

- Inject `+1` world unit of client-only X or Y prediction error.
- Force `+1` world unit of client-only position drift along the last non-zero movement direction.
  This directly corrupts predicted position; it does not submit an extra movement input. The
  overlay shows the direction, and the control is disabled until the client has sent non-zero
  movement. Retaining the last direction makes the control usable while ImGui temporarily
  captures mouse steering.
- Drop the next three input packets while continuing local prediction.
- Show/hide state layers. **Show corrected roll-forward path** defaults off and draws purple
  primary-position samples from the last nonzero correction's replayed input suffix for two
  seconds. It does not show remote paths, topology, or replayed events.
- Clear retained correction visuals.

The layer and replay toggles default on. During an injected-drop burst, the **Rollback** tab in
the right-hand **Diagnostics** pane shows completed and remaining drop counts and disables
starting an overlapping burst. Completion is
retained as a green receipt for two seconds, even when catch-up consumes all three drops between
rendered frames. The runtime also retains a bounded 32-transition ledger of typed `Armed`,
`Triggered`, and `Completed` receipts for position-divergence and input-loss faults; Tools shows
the newest six. Suppressed sends still record and predict their input exactly as deliberate
network loss would. Injected drops have a separate counter and are never added to transport
packet-loss measurements.

## Remote Presentation Output — Current

Feature 12 uses a 32-sample presentation buffer and a remote render cursor delayed by six server
ticks, currently 200 ms. The client receives every accepted snapshot from a runtime poll, then
adapts it into the renderer-free remote history.

The **Interpolation** tab in the right-hand **Diagnostics** pane reports:

| Field | Meaning |
|---|---|
| Playing mode | `EXTRAPOLATED`, `INTERPOLATED`, or `COMPARISON` from `debug.remote_presentation_mode`. |
| Spectator mode | `LIVE` or `DELAYED` from `spectator.presentation_mode`; live is the default. |
| Live underrun fallback | `inactive` while live spectators remain bracketed; otherwise the 0–6 tick movement/launch tail beyond the exhausted authoritative endpoint, including `HELD AT CAP` after 200 ms. Delayed mode never activates it. |
| Extrapolation age/ticks | Local steady-clock age of the newest accepted authoritative kinematic snapshot and the clamped 0–6 tick advancement applied to movement/launch. |
| Extrapolating/held/static | Outside-closure players still within the six-tick horizon, players held at the horizon, and non-player entities kept at their authoritative positions. |
| Extrapolation accepted/rejected | Samples accepted by the monotonic finite/canonical kinematic buffer or rejected by its validation. |
| Buffer fill | Samples stored out of the fixed capacity. |
| Coverage | Difference between oldest and newest buffered server ticks, shown in ticks and milliseconds. |
| Target delay | Intentional six-tick separation between newest authority and remote presentation. |
| Current delay | Actual newest-tick minus presentation-cursor distance. It can be temporarily negative when an underrun freezes a cursor already beyond the newest known tick. |
| Presentation tick | Fractional server-tick coordinate used to draw remotes. |
| Cursor rate | Current 0.95–1.05 presentation-clock adjustment, or `0.0` while holding. This does not change local input rate. |
| Brackets | Older/newer snapshot IDs and server ticks enclosing the cursor, plus alpha. |
| Jitter | Latest and EWMA deviation between observed and server-implied snapshot spacing. |
| Late snapshot | Snapshot arriving at or behind the current presentation cursor. |
| Hold/underrun | Continuous period in which the authoritative interpolation cursor has no newer bracket and therefore holds. The overlay reports current state, episode/recovery counts, and current/last/maximum/total duration. A live spectator may cover the displayed tail with the separate bounded fallback above. |
| Observed/underrun share | Milliseconds since the interpolation buffer first became ready and the percentage of that interval spent underrun. This is the broad network-delivery regression signal; it includes the part a live spectator masks with bounded extrapolation. |
| Live displayed pause | Whether an underrun has exceeded the live six-tick extrapolation cap. Episode/recovery counts, current/last/maximum/total duration, and percentage of observed time measure actual live-spectator pauses rather than masked underruns. |
| Hard rebase | Forward-only presentation cursor reset after recoverable bracketing is lost or the cursor falls more than six ticks behind. |
| Delayed creates/removes | Remote entity lifecycle transitions actually exposed by sampled presentation frames. |
| Example remote entity/endpoints | Representative entity ID and its older/newer interpolation samples. All three rows remain present with `unavailable` placeholders during startup, holds, and entity turnover so the diagnostics pane does not resize every time a bracket changes. |

### Feature 12 world-space legend

For every remote player:

- Filled circle: interpolated presentation.
- Cyan outline: older known authoritative endpoint.
- Blue outline: newer known authoritative endpoint.
- Cyan-to-blue dots: short connector from the older endpoint toward the newer endpoint.

**Show remote endpoint outlines** controls the cyan/blue outlines and their cyan-to-blue
connectors for every remote player without changing remote interpolation or gameplay. They
remain visible when a remote is inside the predicted interaction scope so that its predicted
position can be compared with both interpolation endpoints. Cyan is the older bracket endpoint;
blue is the newer endpoint that the delayed cursor is approaching. The **Interpolation** tab
lists endpoint values for the lowest-ID sampled remote player as a representative example.
Endpoint circles are debug-only and never feed presentation or gameplay state.

For Playing clients, `extrapolated` is the default. It advances the newest accepted owner
movement plus each player's launch velocity with the shared Dots kinematic helper, including
launch decay, for at most six ticks/200 ms. Food is static and cohesion, collision, consumption,
absorption, split, merge, and closure logic never run in this layer. `interpolated` selects the
Feature 12 delayed frame. `comparison` draws extrapolation normally and adds an outline at the
delayed interpolated position. Both spectator modes select the Feature 12 frame while it has a
newer bracket. Only after an underrun does spectator `live` use the bounded kinematic helper for
the uncovered tail; spectator `delayed` holds the endpoint. Food stays at authority and no
spectator presentation mode runs cohesion, collision, consumption, absorption, split, merge, or
lifecycle. A delayed interpolation sample passes through the persistent adapter because its
cursor is already continuous. New extrapolation authority,
prediction-closure entry, and predicted-child entity-ID remaps hand off through persistent
semantic tracks; actual visible pose/radius residuals decay over 100 ms. Movement trails retain
at most eight samples for 300 ms, and removed gameplay circles use a 100 ms structural fade.
The [persistent presentation audit](feature14_persistent_presentation_audit.md) records the
per-source policy.

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

## Complete Rollback Output — Feature 14 Step 8

The **Rollback** tab reports the profile and closure contract, authority/prediction coordinates,
exact retained replay range, checkpoint schema/digests and approximate in-memory checkpoint
storage, last-120 replay p50/p95/p99, typed state-difference summaries, predicted-spawn
lifecycle, command-buffer health, consequence-ledger pruning, receipt rejection categories, and
held-remote-assumption provenance.

| Field | Meaning |
|---|---|
| Prediction profile | Requested and active `InteractionClosure`, `FullReplicated`, or `OwnedGameplay`, plus `IncompleteClosure` fallback reason. |
| Prediction scope | Scope epoch, mechanic/domain/causal-channel masks, causal owner/player/food counts, event-owner subscription count, and replay horizon. |
| Replay coordinates | Authoritative snapshot/tick, predicted tick, input ACK, and exact replay sequence range. |
| Prediction lead | Predicted tick minus its stated authoritative base tick. This replay extent is not RTT, snapshot age, or estimated live server time. |
| History | Occupied frames out of the 256-entry recovery bound, replay tick count, approximate checkpoint storage, and hard-resync/ACK-catch-up counts. |
| State digest | Authority checkpoint schema/digest and corresponding predicted diagnostic digest. Typed differences, not hash equality, remain the correctness source. |
| Replay duration | Latest, p50, p95, p99, and maximum same-frame replay time; 2 ms is a warning, not a partial-replay cutoff. |
| Continuous divergence | Maximum position and mass deltas plus owner/player/food difference counts for the latest authority installation. |
| Structural divergence | Rules/allocator/structural flags and entity create/remove counts. |
| Predicted spawns | Pending, matched, rejected, authority-only, and ambiguous prediction-key counts. Ambiguity causes hard resync. |
| Event lifecycle detail | Aggregate transition and per-handler consequence counters plus bounded ledger proof/pruning state. A selected-key history browser is not implemented. |
| Authority receipts | Accepted/published/server-retired frontiers, depth counters, consequence-ledger proof/pruning counters, and duplicate, conflict, invalid-retirement, sequence-gap, queue-overflow, and receipt-capacity failure counts. |
| Command buffer | Target/latest/EWMA server queue depth, cadence scale, low/high events, and accumulated phase correction. |
| Remote assumption | Count, source-tick range, and replay-frame tick range over which last-known remote movement was held; remote edge actions remain zero. |
| Outside-closure presentation detail | The Interpolation tab retains current extrapolated/held/static entity counts, authority age/cap, delayed-buffer coverage, and cursor holds. |

The world overlay draws independently labeled latest-known authoritative,
predicted, Feature 12 interpolated, bounded extrapolated, pre-correction, and smoothed
presentation layers. A layer is hidden or marked unavailable when its state source does not
exist; zero is not used as a placeholder. Structural/event detail remains aggregate rather than
an interactive selected-entity/key browser.

Interactive fault tools cover position divergence and action-packet suppression. Each has bounded
typed armed/triggered/completed receipts and remains separate from measured transport loss.
Mass/split/identity/remote-assumption/event-key and hostile receipt cases remain deterministic
test fixtures; invalid receipt candidates are applied to scratch inbox/world copies and leave the
last accepted live state unchanged.

Same-frame replay is the atomic production path. The deterministic workload records entity count,
replay ticks, checkpoint bytes, topology events, replay distributions, and rollback-only 30 Hz
frame overruns. Bounded native soaks group RTT and loss separately. The optimized target 200 ms,
1,000-entity case measured 0.738 ms p99 and 0% rollback-only frame overruns, so Feature 14 did not
activate the conditional multi-frame spike. The complete matrix and method are in the
[Feature 14 workload results](feature14_rollback_workload_results.md); the decision thresholds
and atomic-commit invariants live in
[`rollback_prediction_design.md`](rollback_prediction_design.md#same-frame-replay-and-deferred-multi-frame-work).

## Troubleshooting Patterns

### High RTT but fresh snapshots

The connection has network travel time, but snapshot delivery remains regular. Complete local
prediction should hide input response latency; Feature 12 intentionally keeps outside-closure
remotes behind the newest snapshot.

### Snapshot age rises while transport remains connected

The application is not accepting newer snapshots. Check loss, snapshot send cadence, protocol
decode/rejection logs, and server health. Transport state alone does not prove replication flow.

### Command lead and server input queue grow together

The client is producing input faster than the server consumes it, the server is overloaded, or
clock drift is accumulating. First compare **Input ACK**, **Receive grant**, **Transmitted
through**, and **Unsent**. The adaptive controller moves command production by at most five
percent around the fixed gameplay rate. If that is insufficient, production pauses at eight
unsent commands and resumes at two while networking and rendering continue. Persistent pauses or
server timing warnings mean the underlying authority/replication cost still needs investigation.
Headless bots additionally discard missed producer deadlines after a replay or polling overrun.
The server rejects a fresh sequence beyond the advertised receive grant as a peer violation; an
honest current client should never reach the 64-entry defensive overflow guard.

### Frequent corrections with low loss

Check shared tick order, ACK semantics, prediction-scope membership, remote-assumption authority
provenance, and floating-point divergence. Use common-tick reconciliation logs plus the orange,
magenta, white, and replay markers to identify where states first disagree. Repeated comparable
remote corrections after one direction change match the failure pattern in the
[Feature 14 prediction-stutter postmortem](feature14_prediction_stutter_postmortem.md).

### Remote corrects when changing direction

First check the prediction profile and configured remote presentation mode. A remote inside the
interaction closure runs complete rollback-World prediction from its newest authoritative held
movement. A remote outside the closure defaults to movement/launch-only extrapolation for at most
six ticks/200 ms. Neither path can know a remote direction edge before authority arrives, so a
turn can correct away from the held direction. Loss can enlarge the correction before the
extrapolation cap holds. Select `interpolated` to compare against the six-tick delayed,
known-endpoint tradeoff. The
[persistent presentation audit](feature14_persistent_presentation_audit.md) distinguishes this
unavoidable unknown-input correction from the corrected local double-smoothing bug.

### Smoothing offset never settles

Corrections are arriving more frequently than the 100 ms residual can decay. Inspect correction
distance/frequency, replay count, server input depth, and fault-injection state. Do not lengthen
smoothing until the underlying divergence is understood.

### History use exceeds 75%

Snapshot acknowledgements have stalled for several seconds. Inspect snapshot age, loss,
connection state, and server health. At full capacity the client performs a visible hard resync
rather than retaining an unreplayable prediction.

### Prediction timeline or scope failure

Read the preceding `dots.client.prediction` error before the application-level runtime failure.
For a timeline failure it identifies the failed operation and typed rollback/model error. For a
scope failure it identifies whether scope construction or authority projection failed and
includes the authority tick and scope epoch. A split or merge immediately before
`checkpoint outside scope` usually means an older retained scope admitted a remote owner but not
that owner's new topology; the runtime should rebase to a completely projectable scope, never
install partial owner state. On confirmed defeat there is no longer a live owned root from which
to rebuild the normal interaction closure, so the terminal transaction instead expands only the
topology of already-admitted owners, resolves the event lifecycle, and discards prediction. A
terminal projection failure after that expansion is still a defect.

A `dots.client.consequence` batch-contract error is also a defect rather than network loss or a
recoverable stale frame. The router rejects the complete batch without calling handlers or
changing its ledger. Preserve the preceding rollback/prediction logs and the named change index;
the affected client session terminates so corrupted consequence integration state is not reused.

A valid retained owned command whose owner was removed by an earlier replayed interaction is an
expected conditional no-op and must not produce `model_step_failed: ... invalid command`. That
message after an absorption indicates a replay-applicability regression rather than corrupt
network input.

### Remote cursor repeatedly holds

The buffer lacks a newer endpoint. Compare buffer coverage, current delay, jitter, late samples,
and packet loss. The cursor rate is `0.0` during the hold and resumes only after a newer endpoint
arrives. Holds are safer than inventing remote movement.

### Remote cursor rate remains at 0.95 or 1.05

The newest-snapshot distance is persistently outside its target. This may indicate sustained
clock drift, bursty delivery, or insufficient fixed interpolation delay. Use measured data before
adding adaptive delay.

### Structural corrections repeat

Compare prediction profile/closure, predicted-spawn classification, held remote assumptions,
checkpoint configuration, and the first topology tick that differs. Position smoothing cannot
repair an entity create/remove, ownership, split, merge, or deadline mismatch.

### A one-shot consequence repeats

Inspect its stable event key, transition history, handler policy, and suppression count.
`PredictOnce` and `ConfirmOnce` are keyed per handler, so replaying or confirming the same key
must not invoke that handler twice. A changing key indicates incorrect game identity; a stable
key with repeated delivery indicates an occurrence-ledger defect.

### A predicted effect remains after rejection

Confirm the handler uses `PredictCancelable`, produced a stored lifecycle token, and received a
`Retracted` transition. `PredictOnce` deliberately cannot erase a cue already perceived; use it
only when one brief false positive is acceptable.

### Hard resyncs rise

Group reasons by missing history, capacity exhaustion, incompatible checkpoint, and ambiguous
prediction key. Then compare snapshot age, ACK progress, history occupancy, and configuration
validation. A hard resync is correct recovery, but a repeated reason is a defect or an undersized
bound that needs evidence.

### Command cadence stays clamped

Compare latest and EWMA server queue depth with packet loss, input ACK progress, and empty/high
events. A persistent clamp may reveal clock drift or delivery pressure; it does not mean client
session time or the server tick rate changed.

### Same-frame replay exceeds its measured budget

Correlate replay duration with replay ticks, predicted entity count, checkpoint bytes, and
structural changes. A 2 ms warning does not permit partial state. Re-run the release workload and
compare it with the recorded p99 and frame-overrun research thresholds before proposing a
multi-frame scheduler.

## Impairment Testing

Native clients and the session launcher accept outgoing fake lag and loss. Lag is one-way per
process; applying 50 ms at the server and client produces approximately 100 ms transport RTT.
Local prediction already advances once per accepted command while that command stream awaits
authority and its returning ACK, so replay depth normally grows with this complete loop. Do not
add the RTT again to `Prediction lead`: that field is the exact retained replay extent, not a
clock-synchronization target.

Use impairment to answer a specific question:

- Prediction responsiveness: add RTT, move the local player, and compare prediction with the
  latest authoritative sample.
- Reconciliation: use the injected error or packet drops and inspect replay/correction output.
- Redundancy: compare the same drop schedule with redundancy enabled and disabled.
- Remote interpolation: vary loss/jitter schedules and inspect known endpoints, buffer coverage,
  cursor rate, and holds.
- Complete rollback: exercise split and contested interactions and inspect replay/correction
  output under identical impairment. Use a bot-only `--duration-seconds` session when the pass
  condition is process health through a fixed deadline.

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
