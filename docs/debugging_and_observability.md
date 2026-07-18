# Debugging and Observability Guide

This guide is the canonical reference for Dots runtime debugging output: what each value means,
where it comes from, how often it changes, and which conclusions it can and cannot support.

Keep this guide synchronized whenever an overlay label, metric definition, log category, debug
visual, fault control, or warning threshold changes. Feature plans may describe intended output,
but this guide must clearly distinguish implemented behavior from planned behavior.

## Status Labels

This document uses three status labels:

- **Current:** implemented on `feature/11-prediction-reconciliation` before Feature 11 runtime
  work begins.
- **Feature 11 planned:** specified in
  [`plans/11-prediction-reconciliation.md`](plans/11-prediction-reconciliation.md), but not yet
  implemented.
- **Feature 12 planned:** specified in
  [`plans/12-remote-interpolation.md`](plans/12-remote-interpolation.md), but not yet
  implemented.

When a feature phase is approved, change its entries to **Current** as part of that phase's
documentation update.

## State Vocabulary

Networking debug output is useful only when the state being measured is named precisely.

| Term | Meaning | Owner |
|---|---|---|
| Authoritative world | The complete gameplay `simulation::World` stepped by the server. | Server only |
| Authoritative sample | State copied from the server into a snapshot. It is historical when the client receives it. | Protocol/replication |
| Replicated world | The newest validated authoritative sample installed by a client. | Client runtime |
| Predicted state | Controlled-player state advanced locally from owned input. | Feature 11 client runtime |
| Pre-correction state | Prediction immediately before a nonzero reconciliation correction. | Feature 11 debug history |
| Presentation state | The transient positions and geometry submitted for drawing. | Dots presentation/client app |
| Interpolated remote state | Presentation sampled between two known remote snapshot states. | Feature 12 presentation |

A native client cannot observe the server's live current position. A client debug ghost labeled
**authoritative** must mean **latest received authoritative sample** and show its snapshot age.

## Current Dots Overlay

The Dots-owned Dear ImGui panel is anchored in the lower-right corner. It can collapse and its
content can scroll. `MyCore::DebugUI` owns ImGui integration, but the fields and their meanings
remain Dots-owned.

### World and presentation fields — Current

| Label | Source and units | Meaning |
|---|---|---|
| `Input` | Client configuration | Active mouse, keyboard, or hybrid input mapping mode. |
| `Presentation` | Client mode | Offline presentation mode, or `NETWORKED FIXED` for the current uncompensated network path. |
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

## Current Logs and Profiling

Important log categories include:

| Category | Meaning |
|---|---|
| `dots.client` | Client startup and general runtime information. |
| `dots.client.session` | Client transport, handshake, assigned identity, and disconnect lifecycle. |
| `dots.client.simulation` | Client fixed-step overload warnings, escalation, and recovery. |
| `dots.server` | Headless server startup, listen address, tick lifetime, and shutdown. |
| `dots.server.session` | Connection acceptance, assigned players, rejected packets, and cleanup. |

Feature 7 added Tracy zones for the client frame, fixed-step work, presentation extraction, and
render submission. Tracy is on-demand; dormant instrumentation does not mean the zones were
removed.

## Prediction and Reconciliation Output — Feature 11 Planned

Feature 11 will add a dedicated **Session** section with:

- Runtime/connection state and protocol version.
- Server-assigned client ID.
- Controlled entity ID and transport connection handle.
- Latest snapshot ID/server tick and local input tick.

The server and client tick values will be shown separately. Until a future tick-synchronization
feature defines their mapping, subtracting them does not produce a meaningful latency value.

The planned **Prediction** section will include:

| Field | Meaning |
|---|---|
| Last sent input | Newest input sequence recorded by the client. |
| Last acknowledged input | Newest sequence the server says was included in authoritative stepping. |
| Command lead | Count of sent inputs newer than the latest ACK. |
| History use/high-water | Current and maximum occupancy of the fixed 256-entry replay ring. |
| Server pending input | Per-client authoritative input queue depth reported in the latest snapshot. |
| Rollback base | Snapshot ID, server tick, and ACK used to start the latest reconciliation. |
| Replay count | Inputs replayed after installing that authoritative base. |
| Replay duration | CPU duration for scratch replay and atomic commit. |
| Reconciliation count | Newer accepted snapshots processed while prediction was ready. |
| Correction distance | Difference between prediction before reconciliation and after full replay. |
| Corrections/min | Rolling frequency of corrections above the configured numerical tolerance. |
| Smoothing offset | Remaining presentation-only displacement from corrected prediction. |
| Hard resync | History-capacity recovery that snaps to replicated authority and clears replay state. |

### Planned Feature 11 world-space legend

| Visual | Meaning |
|---|---|
| Filled player | Actual presented position and camera target. |
| White outline | Corrected predicted simulation position. |
| Orange outline | Latest received authoritative position. |
| Magenta outline | Prediction immediately before the most recent nonzero correction. |
| Purple markers | Results of replayed unacknowledged inputs after the rollback base. |

Correction-specific magenta/purple visuals remain for two seconds. Slight radius offsets keep
coincident outlines visible.

### Planned Feature 11 fault controls

- Inject `+1` world unit of client-only X prediction error.
- Drop the next three input packets while continuing local prediction.
- Show/hide state layers and replay markers.
- Clear retained correction visuals.

Injected drops are application debug events and will have their own counter. They must not be
added to transport packet-loss measurements.

## Remote Interpolation Output — Feature 12 Planned

Feature 12 will add a 32-sample presentation buffer and a remote render cursor delayed by six
server ticks, currently 200 ms.

Planned metrics include:

| Field | Meaning |
|---|---|
| Buffer fill | Samples stored out of the fixed capacity. |
| Coverage | Difference between oldest and newest buffered server ticks, shown in ticks and milliseconds. |
| Target delay | Intentional six-tick separation between newest authority and remote presentation. |
| Current delay | Actual newest-tick minus presentation-cursor distance. |
| Presentation tick | Fractional server-tick coordinate used to draw remotes. |
| Cursor rate | Current 0.95–1.05 presentation-clock adjustment. This does not change local input rate. |
| Brackets | Older/newer snapshot IDs and server ticks enclosing the cursor. |
| Alpha | Fraction between the selected bracket ticks. |
| Jitter | Latest and EWMA deviation between observed and server-implied snapshot spacing. |
| Late snapshot | Snapshot arriving at or behind the current presentation cursor. |
| Hold/underrun | Continuous period with no newer bracket; remotes hold rather than extrapolate. |
| Hard rebase | Presentation cursor reset after recoverable bracketing is lost. |

### Planned Feature 12 world-space legend

For one selected remote entity:

- Filled circle: interpolated presentation.
- Cyan outline: older known authoritative endpoint.
- Blue outline: newer known authoritative endpoint.

An ImGui toggle will optionally draw endpoint brackets for every remote entity. It defaults off
to avoid clutter and unnecessary debug draw cost at scale.

Feature 12 presentation-clock correction and future local input-clock synchronization solve
different problems. Feature 12 keeps a delayed remote cursor centered in known snapshots. A
future tick-synchronization feature would map local input ticks to estimated server ticks and
adjust command slack.

## Troubleshooting Patterns

### High RTT but fresh snapshots

The connection has network travel time, but snapshot delivery remains regular. Feature 11 local
prediction should hide input response latency; Feature 12 will intentionally keep remotes behind
the newest snapshot.

### Snapshot age rises while transport remains connected

The application is not accepting newer snapshots. Check loss, snapshot send cadence, protocol
decode/rejection logs, and server health. Transport state alone does not prove replication flow.

### Command lead and server input queue grow together — Feature 11 planned

The client is producing input faster than the server consumes it, the server is overloaded, or
clock drift is accumulating. Feature 11 observes this trend but does not speed or slow local
simulation.

### Frequent corrections with low loss — Feature 11 planned

Check shared movement operations, tick application order, ACK semantics, collision/mass events
that are intentionally not predicted, and floating-point divergence. Use the orange, magenta,
white, and replay markers to identify where states first disagree.

### Smoothing offset never settles — Feature 11 planned

Corrections are arriving more frequently than the 100 ms residual can decay. Inspect correction
distance/frequency, replay count, server input depth, and fault-injection state. Do not lengthen
smoothing until the underlying divergence is understood.

### History use exceeds 75% — Feature 11 planned

Snapshot acknowledgements have stalled for several seconds. Inspect snapshot age, loss,
connection state, and server health. At full capacity the client performs a visible hard resync
rather than retaining an unreplayable prediction.

### Remote cursor repeatedly holds — Feature 12 planned

The buffer lacks a newer endpoint. Compare buffer coverage, current delay, jitter, late samples,
and packet loss. Holds are safer than inventing remote movement.

### Remote cursor rate remains at 0.95 or 1.05 — Feature 12 planned

The newest-snapshot distance is persistently outside its target. This may indicate sustained
clock drift, bursty delivery, or insufficient fixed interpolation delay. Use measured data before
adding adaptive delay.

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

