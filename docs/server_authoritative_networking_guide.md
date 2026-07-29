# Protocol, Transport, and Server-Authoritative Networking

This guide assumes you understand the basic idea of a client and server: the client collects
input and draws the game, while the server runs shared gameplay. It explains the boundaries
between Dots protocol messages, the game-neutral transport, authoritative simulation, replicated
client state, the complete rollback predictor, and remote presentation
interpolation/extrapolation.

## The short version

The current networked path is deliberately simple:

```text
client input -> interaction-closed predicted World -> smoothed local presentation
    |
    | InputPacket bytes
    v
transport -> authoritative server simulation -> FullSnapshot -> replicated client world
                                                       |                 |
                                                       v                 v
                                             rollback/replay         bounded remote rendering
```

The server owns the only authoritative gameplay `World`. The client sends input requests and
speculatively steps a complete Dots World inside the fixed-point interaction closure around its
owned pieces. A verified server checkpoint replaces the rollback base and the client atomically
replays unacknowledged input through the previous prediction head. Gameplay state corrects
immediately; only presentation residuals decay over 100 ms. While Playing, remote players outside
the closure default to bounded movement/launch extrapolation from newest authority and then hold.
Spectators use the Feature 12 delayed interpolation buffer.

`dots_client --in-memory` exercises this complete path without opening a socket. Its client and
server live in one process and communicate through the same abstract transport interface used by
the native backend. `dots_client --connect 127.0.0.1:27020` instead connects to a separate
`dots_server` process through GameNetworkingSockets. The in-memory backend remains FIFO and
lossless, while the native backend supports realistic outgoing latency and loss simulation.

The runtime has **complete interaction-closed prediction and reconciliation** through
`MyCore::Rollback` and `Dots::Prediction`. The overlay says `NETWORKED PREDICTED` and separately
displays the latest authoritative primary sample, corrected prediction, and smoothed
presentation. Predicted topology is drawn for the entire interaction island. Other players
default to at most six ticks/200 ms of presentation-only movement/launch extrapolation and then
hold; delayed interpolation remains selectable, supplies comparison endpoints, and is mandatory
while Spectating.

## Three different responsibilities

Protocol, transport, and authority answer different questions:

| Layer | Question it answers | Current owner |
|---|---|---|
| Protocol | What do these bytes mean? | `Dots::Protocol` |
| Transport | How do byte payloads move between connected endpoints? | `MyCore::NetTransport` |
| Server runtime and simulation | Which requests are accepted, and what is true now? | `Dots::Server` and `Dots::Simulation` |
| Replication | Which authoritative state is sent and installed? | `Dots::Replication` |
| Client runtime | What is this client's confirmed session, replicated view, and interaction-closed predicted World? | `Dots::ClientRuntime` |
| Presentation | How does received state become visible circles and a camera? | `Dots::Presentation` |

Keeping these separate lets protocol tests run without sockets, transport tests run without Dots,
and server tests run without a window or GPU.

## Protocol: the meaning of the bytes

The Dots protocol is a game-owned contract. It defines four messages today:

| Message | Direction | Delivery | Purpose |
|---|---|---|---|
| `ClientHello` | Client to server | Reliable | Request a session using the supported protocol version. |
| `ServerWelcome` | Server to client | Reliable | Assign the client ID and immutable match/session rules. |
| `InputPacket` | Client to server | Unreliable | Submit one to three sequenced movement/action samples and acknowledge the latest snapshot and contiguous authority receipt. |
| `FullSnapshot` | Server to client | Unreliable | Carry a complete schema-tagged checkpoint, recipient lifecycle state, repeated authority receipts, and the server-retired receipt frontier. |

Every encoded message starts with a 12-byte header containing:

```text
magic "DOTS" + protocol version + message kind + flags + payload length
```

The current protocol version is 4. Version 1, used by Features 8--10, carried one input sample
and no pending-input depth. Version 2 added one-to-three-sample redundancy and per-client queue
depth. Version 3 adds player owner IDs, respawn actions/configuration, and a recipient-specific
session block containing mode, owned/primary/follow entities, defeat/deadline ticks, latest
absorption, and explicit respawn result. Version 4 adds immutable `WorldRules`, complete
rollback-checkpoint fields, prediction keys, a canonical schema/digest, the split action bit,
and sequenced typed authority receipts. There is no dual-version negotiation: older binaries
receive `UnsupportedVersion` rather than having their payload interpreted as version 4.
Food, absorption, and split receipts include occurrence geometry so presentation does not depend
on an entity that the same event already removed or transformed.

Integers and floating-point bit patterns have defined widths and use big-endian network byte
order. Fields are encoded individually; the implementation never copies a C++ struct directly
onto the wire. This keeps the format independent of compiler padding, machine byte order, and
the server's in-memory component layout.

The decoder treats every packet as hostile input. It checks framing, lengths, version, enums,
IDs, finite numbers, movement ranges, known action bits, canonical entity/owner ordering,
checkpoint closure and prediction-key invariants, receipt order/identity, lifecycle combinations,
deadline ordering, and size limits before accepting a message. Replication separately restores a
typed scratch `WorldCheckpoint` and verifies its schema-1 FNV-1a digest. A malformed packet
produces a typed error. At the server boundary, malformed or wrong-direction messages disconnect
only the offending peer.

### Why reliable and unreliable are both useful

Handshake messages are reliable because they are infrequent state transitions that must arrive
in order. Losing a welcome message cannot be fixed by receiving a newer welcome message.

Reliable delivery means that the transport retransmits and preserves ordering while the
connection remains usable. It does not mean that the application retries forever or that a
connection is guaranteed to form. Dots submits each `ClientHello` and `ServerWelcome` once and
lets GameNetworkingSockets perform any required retransmission. The transport can still declare
the connection failed, and the client gives the complete connection-plus-handshake sequence a
10-second startup deadline. Heavy loss can therefore make a reliable handshake fail to complete.

Reliable and unreliable are message delivery modes on the same GameNetworkingSockets connection,
not separate TCP and UDP application paths. Unreliable means no retransmission guarantee; it does
not bypass connection security or Dots protocol decoding and validation.

Retransmission also trades timeliness for delivery. Under loss, a reliable message can arrive
much later and reliable data can wait behind earlier missing data. This is appropriate for the
small ordered handshake, but not for a continuous stream of replaceable gameplay state.

Input and snapshots are time-sensitive. A delayed old movement sample or snapshot is usually
less useful than the newest one, so they are sent unreliably and carry application sequence IDs.
The receiver can ignore stale data without waiting for retransmission. Protocol-v4 input packets
contain the current sample and, by default, up to two prior unacknowledged samples. The server
deduplicates overlapping packets before its bounded scheduling queue. The current server also
sends another full snapshot at 15 Hz, so a lost snapshot is replaced by a newer complete view.
Later snapshot deltas will improve loss handling without turning the real-time stream into a
reliable queue of obsolete state.

The in-memory implementation is lossless for both delivery modes. It records the same intended
semantics as the native implementation so tests and runtimes do not need a different contract.

### Packet size policy

The codec distinguishes a preferred transport payload from a hostile-input safety limit. Normal
messages should eventually fit within about 1,200 bytes so the application does not routinely
depend on network fragmentation. The decoder's hard limit is 64 KiB so oversized input cannot
cause unbounded allocation or parsing work.

Current full-world snapshots are allowed to exceed the 1,200-byte target while remaining under
the hard limit. The in-memory transport does not model a network MTU. Interest management,
delta encoding, quantization, prioritization, and per-client byte budgets in Features 15–16 will
make ordinary native-network snapshots fit the preferred budget.

### IDs belong to domains

Several ID types may have the same integer representation but different meanings:

```text
ConnectionHandle        transport connection domain
protocol::ClientId      Dots wire client domain
protocol::EntityId      Dots wire entity domain
simulation::EntityId    authoritative World storage domain
```

Defining `EntityId` in both protocol and simulation is intentional. Strong types prevent an ID
from one domain being passed accidentally into another. `Dots::Replication` owns the explicit
mapping at the network/simulation boundary. The shared numeric representation is a current
mapping policy, not permission to treat the types as interchangeable.

## Transport: moving opaque payloads

`MyCore::NetTransport` knows about:

- Connection handles.
- Connected and disconnected events.
- Received byte payloads.
- Reliable or unreliable delivery intent.
- Sending, polling, and disconnecting.

It does not know about players, input commands, snapshots, entity IDs, Dots, or serialization.
To the transport, an encoded `FullSnapshot` is only a copied byte vector associated with a
connection.

The current `InMemoryNetwork` provides one server endpoint and one or more client endpoints. It
queues copied payloads in FIFO order, delivers them when the receiving endpoint polls, isolates
clients, and reports disconnects exactly once to each side. This makes multiplayer session tests
deterministic without pretending to be a real network.

Feature 10 puts a GameNetworkingSockets implementation behind the same endpoint contract:

```text
                 MyCore::NetTransport::Endpoint
                         /              \
                        /                \
        deterministic tests          native network
          InMemoryNetwork        GameNetworkingSockets
```

The protocol and Dots runtimes do not care which implementation carries their bytes. The
native backend adds cross-process connections, encryption, congestion behavior, and realistic
latency/loss simulation; it does not add gameplay replication or prediction by itself.

A normal client exit explicitly disconnects its transport connection. The server receives the
disconnect event, removes the corresponding session and authoritative player, and logs the
reason. Abrupt process loss follows the same cleanup path after the transport detects failure,
but cannot provide a graceful local-request event.

Before a native graphical client tears down its networking instance, it requests a lingering
transport close and drains endpoint callbacks for a bounded interval. The interval is the
configured outgoing fake lag plus 50 ms, clamped from 50 ms to two seconds. This lets a delayed
close notification leave the process; it does not wait indefinitely for an application-level ACK.
If either Dots runtime cannot enqueue an application message, it closes that connection before
removing or abandoning its session state; a peer is never intentionally left attached to an
orphaned session.

After a listening endpoint delivers a peer's disconnect event, it releases the server-side
transport record for that connection. Server code must capture any final statistics before or
while handling that event rather than treating closed handles as permanent history. A client
endpoint retains its own single terminal record for post-close diagnostics.

The Dots server also has a gameplay-level liveness fallback. Ready clients normally submit valid
input packets at 30 Hz; after 90 server ticks (three seconds) without one, the server logs a
warning, closes the transport session, and removes the player. This prevents a lost connection
from retaining its last movement forever while still tolerating ordinary packet loss. The timeout
uses server ticks, not wall-clock sleeps, so it is deterministic in tests.

A transport connection that does not complete `ClientHello` within 300 server ticks (10 seconds)
is also closed. Accepting the hello starts the ready-session activity window; time spent waiting
to begin the application handshake is not charged against the three-second input-liveness
window.

Separately, Dots holds a player's last applied movement for at most five server ticks when its
per-tick input queue runs dry. On the next missing-input tick it neutralizes movement but retains
the connection until normal input resumes or the liveness timeout expires. This gameplay policy
avoids indefinite movement during packet gaps without treating one missing packet as a disconnect.

## Native session startup and lifecycle

There are three related but separate layers during startup:

1. The client process initializes its window, renderer, and other local systems.
2. GameNetworkingSockets establishes a transport connection between processes.
3. Dots performs its application handshake and installs the initial replicated world.

This produces the following normal sequence:

```text
server process                         client process
--------------                         --------------
open listen socket                     initialize window and renderer
print DOTS_SERVER_READY                begin transport connection
receive Connected event       <------> receive Connected event
create pending session                 send reliable ClientHello
validate ClientHello
spawn authoritative player
assign client and entity IDs
send reliable ServerWelcome    ------>
send unreliable FullSnapshot   ------>
log "Client ... joined"                install identity and snapshot
                                        log "Session ready ..."
```

`DOTS_SERVER_READY` only means that the server is listening. A renderer log before a client
transport log is also normal because local presentation is initialized first. The server can log
that a client joined before the client logs that its session is ready: the server has accepted the
hello at that point, while the client still needs both the welcome and a snapshot containing a
valid recipient session block. Those two messages may arrive in either order. If the first unreliable
snapshot is lost, a later 15 Hz full snapshot can still complete the client handshake.

The client starts one 10-second deadline when its networked runtime is created. That deadline
includes transport connection establishment, the reliable hello/welcome exchange, and receipt
of a usable snapshot. Reaching it is a failed startup even if the transport was still retrying.
Dots currently does not reconnect or begin a second handshake automatically; the client exits
with `Could not establish the authoritative session`.

After startup, network polling and 30 Hz fixed input production do not depend on a drawable
surface. If a graphical window is minimized or temporarily has zero drawable size, rendering
pauses but the authoritative session remains active. Mouse steering is neutral until a viewport
is available again; keyboard input can still produce movement.

Connection handles, client IDs, and entity IDs belong to different domains. It is normal for a
client to report transport connection `0`, Dots client `0`, and a much larger controlled entity
ID. The server creates food entities before joining players, so the first player is not generally
entity `0`.

After startup, lifecycle events are handled as follows:

| Event | Client behavior | Server behavior |
| --- | --- | --- |
| Escape or window close | Requests a graceful transport disconnect, logs it, and exits normally. | Receives the disconnect event and removes only that client's session and player. |
| Server closes | Logs the transport close and stops the networked client. | The server process exits. |
| Connection fails | Logs the transport failure and stops the networked client. | Removes the session and player when it observes the failure. |
| Malformed or wrong-direction packet | The offending client is disconnected. | Rejects and removes only the offending session; the server and other clients continue. |
| Last owned player is absorbed | Installs repeated `Spectating` state without treating the missing primary as a protocol failure, then follows the confirmed killer or uses a local free camera. | Keeps the connection alive and repeats killer, defeat, deadline, and action-result state. |
| Respawn action | Sends one edge-triggered action and continues spectating until a snapshot reports the authoritative result. An input ACK alone is not success. | Rejects early/non-spectator requests or safely spawns a new owned player after the deadline. |
| Client process is killed or loses connectivity | Cannot send a graceful request. | Detects the transport failure later, then removes the session and player. |

The local `dots_session.py` launcher treats an unexpected nonzero client exit or any server exit
as failure of the development session and terminates the remaining child processes. That cleanup
may be abrupt, so terminating the launcher is not a good test of every client's graceful-leave
log path. Close one client window while leaving the launcher running to observe that path.

## Reasoning about simulated lag and packet loss

The native impairment options configure outgoing traffic in each process, and they are active
before transport connection establishment. They therefore affect connection negotiation,
application handshake messages, and gameplay traffic rather than only the post-join session.

The launcher passes the same `--fake-lag-ms` and `--fake-loss-percent` values to the server and
every client. Lag is a one-way delay at each sender. With 50 ms configured by the launcher, a
request pays about 50 ms on the client-to-server path and its response pays about 50 ms on the
server-to-client path, producing roughly 100 ms of transport round-trip time before tick,
snapshot, rendering, or retransmission delays. To test only one direction, run the server and
clients manually and put the impairment option only on the desired sender.

Packet loss is random per outgoing low-level packet, not a promise to discard exactly that
percentage of application messages in a short run. Loss at each process is independent. With
50% loss applied at both endpoints, one simple request-and-response attempt has only a
`0.5 * 0.5 = 25%` chance that both packets survive that attempt. Reliable transport retries can
improve the eventual odds while the connection remains alive, but every retry is exposed to loss
again and may arrive only after transport backoff. This is why two clients started with identical
settings can have different outcomes, and why one can join while another exceeds 10 seconds.

The current messages behave under impairment like this:

| Message or phase | Delivery | Expected behavior under loss |
| --- | --- | --- |
| Transport connection establishment | Transport-managed | May retry, take much longer, or fail before the Dots handshake starts. |
| `ClientHello` and `ServerWelcome` | Reliable | Transport retransmits while viable; delay increases, but Dots does not repeatedly call `send`. |
| `InputPacket` at 30 Hz | Unreliable | The next one or two packets can recover a lost current sample while it remains inside the default two-sample redundancy window. Three consecutive losses, or any loss with redundancy disabled, can skip a sample. |
| `FullSnapshot` at 15 Hz | Unreliable | Entity and lifecycle state hold until a newer snapshot. Durable session fields repeat, so a lost defeat/respawn transition is recovered by a later snapshot. Owned movement keeps predicting only while Playing. |
| Disconnect | Transport lifecycle | Graceful requests are reported promptly when delivered; abrupt loss is reported after failure detection. |

Owned movement responds locally despite lag, but authority and its ACK arrive later, increasing
history use and replay work. Inputs lost beyond the configured redundancy window can leave the
server using an older desired direction and produce a later correction. Lost snapshots make
remote entities hold and delay owned-player reconciliation. This is expected under impairment,
not evidence that the authoritative simulation itself stopped.

Use modest impairment to study steady-state behavior and high impairment to study failure paths:

- Start with no impairment to establish a baseline.
- Use 50--100 ms of lag to make latency visible while keeping startup predictable.
- Use roughly 5--20% loss to observe recovery during a joined session.
- Treat 50% loss at both endpoints as a destructive test where startup failure is expected.

The transport debug data and lifecycle logs answer different questions. Transport RTT, loss, and
queue statistics describe network health; snapshot rate and age describe what replication is
actually reaching the client. A loss statistic may be unavailable early in a connection while
the transport gathers enough samples. For startup diagnosis, follow the logs in order: server
ready, transport open on both peers, server join, then client ready. The missing transition
identifies which layer did not complete.

## What server authoritative means

Server authoritative means that the server's simulation result is the final gameplay truth.
Clients submit intent, not outcomes.

For example, the client may say:

```text
input sequence 42: move in direction (0.8, -0.6)
```

It may not say:

```text
my player is now at (900, 400), has mass 500, and consumed entity 17
```

The server associates each connection with the player it spawned for that session. An
`InputPacket` samples do not choose an entity to control. The server validates and queues them,
maps at most the oldest queued sequence to a simulation input ID before each tick, applies it
only to the session's owned player, and advances the fixed-step world. Movement, collision,
eating, mass, spawning, and removal remain server decisions.

Authority is an ownership rule, not a statement about where code can execute. A future client
may run the same movement code speculatively for responsiveness, but the server result still
wins whenever the two disagree.

## One current in-memory session

The handshake establishes both identity and initial state:

```text
client runtime             transport              server runtime
      |                        |                         |
      |<------ Connected -----|------ Connected ------>|
      |--- ClientHello ------>|------------------------>|
      |                        |          validate and spawn player
      |<-- ServerWelcome -----|<------------------------|
      |<-- FullSnapshot ------|<------------------------|
      |                        |                         |
      | ready after welcome + snapshot                  |
```

The welcome is reliable. The immediate full snapshot is unreliable. The client tolerates either
arrival order and becomes ready only after it has an assigned identity and a snapshot containing
its controlled player.

After the handshake, each current in-memory fixed step is composed in this order:

1. The client maps keyboard or mouse state to a normalized movement vector.
2. It creates the next sequenced input sample and a bounded `InputPacket`, including its latest
   snapshot ID, highest contiguous authority-receipt sequence, and, when enabled, up to two prior
   unacknowledged samples.
3. The protocol encoder produces bytes and the client sends them unreliably.
4. The server polls, decodes, validates, deduplicates, and orders fresh samples in that
   connection's bounded queue.
5. The server applies at most the oldest queued sample for each client, then advances its
   authoritative `World` by one 30 Hz tick.
6. Every two ticks, the server builds and sends a 15 Hz full snapshot for each ready client.
7. The client polls and atomically installs a newer snapshot.
8. While Playing, presentation composes the persistent predicted interaction island with bounded
   outside-closure extrapolation by default and centers the camera on the smoothed primary. While
   Spectating, it draws the complete delayed remote sample and uses confirmed-killer follow or a
   presentation-only free camera.

Each full snapshot currently contains every player and food entity in canonical identity order,
plus:

- Its snapshot sequence ID.
- The authoritative server tick.
- The last input sequence processed for that client.
- That client's current pending-input queue depth after the tick.
- Checkpoint schema `1`, the canonical 64-bit diagnostic digest, and the next entity ID.
- Complete owner movement, last-movement/input, split-cooldown, per-player launch/merge state,
  and optional predicted-child identity.
- The recipient's repeated lifecycle state.
- The highest authority-receipt sequence already retired by the server, or `none`.
- Up to 16 relevant unacknowledged typed authority receipts.

`ReplicatedWorld` rejects invalid snapshots, ignores stale snapshots, and fully replaces its
entity collection when it accepts a newer one. Authority receipts live in a separate bounded
`AuthorityReceiptInbox`, not in replicated World state. The inbox accepts only identical
retransmissions and a gap-free extension, rejects reuse of a live stable event key, and tracks
accepted, event-batch-published, and server-retired frontiers. `Dots::Replication` also exposes
exact typed checkpoint hydration and digest verification for the complete rollback timeline.
`ReplicatedWorld` remains a client view, not a second authoritative `simulation::World`.

Authority receipts deliver transient confirmed food, absorption, split, and merge events without
turning snapshots into reliable transport. Each session has its own monotonic sequence and
receives events relevant to its owner. The server retains up to 256 unacknowledged receipts and
repeats the first 16 in each snapshot. The client ACKs only the highest contiguous receipt whose
successful rollback transaction has queued an observable event batch; input packets retire the
server copy through that published ACK, and later snapshots echo the retirement frontier. A gap,
conflicting duplicate, live semantic-key reuse, ACK beyond the issued frontier, invalid
retirement echo, or retention overflow is an explicit session failure. Repeated
Playing/Spectating and respawn fields remain ordinary authoritative state rather than
receipt-driven one-shots.

`client_runtime::Runtime` retains at most 512 post-commit prediction event batches for its
presentation/consequence consumer. Every composition root must regularly take that stream.
Headless clients such as `dots_bot` intentionally discard the returned batches because they have
no side effects to present; failing to drain them is a consumer-liveness error and eventually
returns `PredictionEventQueueFull`. This bound prevents a missing consumer from leaking memory.

## The baseline no-compensation mental model

Without prediction, a networked client would render only the last authoritative sample received:

```text
server truth at ticks:       0 -- 1 -- 2 -- 3 -- 4 -- 5 -- 6
snapshots sent:              S0      S1      S2      S3
client renders:              S0------S1------S2------S3
```

There is no known state between `S1` and `S2` for remote presentation to estimate. Feature 12
instead keeps a short history of those remote samples and samples a cursor six server ticks behind
its newest known snapshot. If that cursor runs out of a newer sample, it holds rather than
extrapolating. The owned interaction island separately predicts from local input and recorded
remote assumptions.

On a real network without compensation, the same model would also put round-trip and scheduling
delay between pressing a key and seeing the returned authoritative result:

```text
press key -> input travels to server -> later server tick -> snapshot travels back -> draw
```

The in-memory mode validates the architecture but largely hides that responsiveness problem
because it has no simulated network delay. Native sessions make the distinction visible and
allow it to be amplified with fake lag and loss: owned movement responds immediately, while
authoritative samples and outside-closure remote entities remain delayed. Feature 12 makes those
remotes smooth by presenting historical known samples rather than guessing remote input or
velocity. Inside the prediction closure, the client instead runs shared physics and gameplay
from explicitly retained last-known movement assumptions.

Input samples have sequence IDs, protocol-v4 packets retain bounded redundancy, the server
schedules at most one queued sample per client per tick, snapshots acknowledge
`last_processed_input`, and inputs report the latest received snapshot and highest contiguous
authority receipt. The client runtime retains a fixed 256-entry input history and engine
timeline. That capacity is a recovery bound, not the interaction-closure horizon: the horizon is
the greater of a five-tick operating floor and the actual retained unacknowledged suffix, checked
before each predicted advance. A safe existing causal superset remains selected when ACKs shrink
the immediate requirement. Each accepted snapshot is exactly hydrated into a checkpoint,
projected to the current interaction scope, and used as the base for atomic replay of the suffix.
Every replay stimulus records its immutable local command and explicit held remote-movement
assumptions. When newer authority changes a remote owner's held movement, reconciliation
transactionally refreshes those derived assumptions across the retained suffix before stepping
it; subsequent future inputs also sample that newest authority. This produces one correction for
an unknowable remote direction edge instead of propagating the stale guess through later
snapshots. The server does not yet use snapshot acknowledgements for delta baselines.

## There is no single “game frame”

The word *frame* is overloaded. Rendering, simulation, replication, transport polling, and the
physical display do not have to advance together.

### Clock and timeline vocabulary

The canonical ownership and time definitions, including why remote presentation is intentionally
historical, are in [Networked prediction and time reference](networked_prediction_reference.md).
This section applies those definitions to the current Dots protocol and diagnostics.

Use one name per concept. In particular, do not use **estimated world time** as a second name for
**estimated live server time**; the current server owns one authoritative world, so those phrases
would describe the same estimate and invite accidental comparison with presentation time.

| Term | Definition | Scope and authority |
|---|---|---|
| Authoritative server tick | Count of successfully completed `World::step()` calls. It starts at zero with the current server world and advances at the nominal 30 Hz simulation rate. | Shared truth for every client connected to that server run. |
| World simulation time | `authoritative_server_tick / 30 Hz`. This measures simulated world progression, not necessarily server-process wall uptime when the server stalls or catches up. | Authoritative, discrete, and shared. |
| Snapshot ID | Per-client sequence number for snapshots sent to that session. It starts when the client joins and exists to reject stale or reordered snapshots. | Not a clock and not comparable between clients. |
| Latest-known world time | `latest_received_snapshot.server_tick / 30 Hz`. This is exact for the received sample but historical by the time it is drawn. | Per-client knowledge of shared authority. |
| Client session time | Local steady-clock duration since that client became ready. It advances smoothly regardless of packet delivery. | Per-client and never authoritative gameplay time. |
| Estimated live server time | A client's estimate anchored by a received server tick and advanced with local steady-clock elapsed time. Optional one-way-delay estimation can move the estimate closer to server “now.” | Per-client estimate; clients can disagree because their latency and samples differ. |
| Remote presentation time | Feature 12's fractional server-tick cursor used to sample remote entities. It intentionally trails the newest known tick by about six ticks/200 ms. | Per-client presentation state, not live authority. |
| Owned prediction extent | The latest authoritative controlled-player base plus a particular sequence of locally replayed, unacknowledged inputs. | Describe with snapshot/server tick, ACK, and replayed input range; it is not one trustworthy server tick. |
| Correction smoothing age | Local wall time within the 100 ms visual correction decay. | Presentation-only; it does not advance or rewind simulation. |

Client session time is deliberately independent of packet arrival. Tying it directly to snapshots
would make it pause during loss and jump during jitter. Instead, local steady time makes a server
clock estimate continuous between authoritative anchors:

```text
anchor_server_time = received_server_tick / 30 Hz
anchor_local_time  = local steady-clock time when that snapshot arrived

estimated_live_server_time(now) =
    anchor_server_time + (now - anchor_local_time)
```

This basic estimate remains behind the server by the snapshot's production, transit, and polling
delay. Adding roughly half the RTT is a simple symmetric-path approximation, not proof of actual
one-way delay. Gameplay deadlines must therefore remain server decisions expressed as ticks; a
client estimate is suitable only for presentation.

#### Which compensation uses which clock

Dots does not maintain a filtered general-purpose estimated-live-server clock. Feature 13's
Gameplay tab does calculate a narrow, unfiltered deadline projection from the latest replicated
server tick plus local steady time since that snapshot arrived. It must not be mistaken for a
hidden input-prediction or smoothing clock, and it never decides respawn eligibility.

| Mechanism | Status | Timeline/input it actually uses | Uses estimated live server time? |
|---|---|---|---|
| Interaction-closed World prediction | Current, Feature 14 step 7 | Local 30 Hz commands plus latest-authority remote assumptions applied to a complete scoped Dots World. | No. |
| Reconciliation | Current, Feature 14 step 7 | Verified authoritative checkpoint plus replay of the unacknowledged input suffix. Server tick labels the rollback base but does not choose how far to replay. | No. |
| Local correction smoothing | Current, Feature 11 | Local steady-clock age of a fixed 100 ms visual offset. | No. |
| Remote presentation | Current, Feature 12 | Fractional cursor in historical server-tick coordinates, targeting six ticks behind the newest known snapshot; holds on underrun. | No. |
| Outside-closure extrapolation | Current, Feature 14 step 7 | Local steady-clock age of the newest snapshot converted to at most six shared movement/launch ticks; holds after the cap. | No. |
| Interaction-closed World rollback | Current, Feature 14 step 7 | Authoritative checkpoint plus retained immutable local commands and transactionally refreshed remote assumptions, replayed through the previous prediction head. | No. |
| Adaptive command buffer | Planned, Feature 14 | Reported server input-queue depth controls a bounded client command cadence. | No. |
| Feature 13 respawn countdown | Current | Latest replicated server tick plus local steady time since snapshot receipt; unfiltered and presentation-only. | It is a limited estimate, never authority. |
| Filtered smooth world-time UI | Deferred | A filtered mapping from local steady time to estimated server tick. | Potentially. |
| Server-side rewind/lag compensation | Deferred | Server history plus a validated, bounded mapping of client action time into server ticks. | It would need a clock-mapping policy, but never trust the client's claim directly. |

Calling prediction “lookahead” can obscure this distinction. Owned prediction advances known
local input beyond the latest acknowledged authority; it does not ask where the server clock is
and extrapolate the whole world. Remote interpolation does the opposite of lookahead: it
intentionally samples older known server states.

Current local correction smoothing is spatial and fixed-duration:

```text
correction = prediction_before_reconcile - prediction_after_replay
new_offset = residual_offset_from_any_prior_correction + correction
presentation_position = corrected_prediction + decaying_offset

decaying_offset(age) = new_offset * clamp(1 - age / 100 ms, 0, 1)
```

Changing RTT or jitter can change how often corrections arrive and how large they are, but it does
not change the 100 ms decay. Overlapping corrections add to the residual that has not finished
decaying.

Feature 12 instead disciplines a deliberately delayed remote cursor:

```text
desired_cursor = newest_received_server_tick - 6
tick_error      = desired_cursor - remote_presentation_cursor

cursor_rate = 1.0                                      inside ±0.25 tick
cursor_rate = clamp(1 + 0.02 * tick_error, 0.95, 1.05) outside the deadband
cursor_rate = 0.0                                      while holding without a newer bracket
```

A newer snapshot moves `newest_received_server_tick` and therefore the target. Jitter changes
arrival spacing but not the server-tick coordinates stored in the buffer. During loss, the cursor
can exhaust its newer bracket and freeze. Late snapshots update the held known state without
moving the cursor backward; normal advancement resumes once a newer endpoint exists. A hard
rebase is forward-only when the cursor falls more than six ticks behind or its lower bracket has
been evicted. The six-tick target remains fixed in Feature 12—RTT and measured jitter do not
automatically enlarge or shrink it.

#### If estimated live server time is implemented later

Treat each accepted snapshot as a noisy clock observation, not a command to reset the clock. For
snapshot `i`:

```text
t_i = local steady-clock arrival time
s_i = authoritative server tick carried by the snapshot
d_i = estimated snapshot production-to-arrival delay

observed_server_tick_at_arrival = s_i + (30 Hz * d_i)
phase_error = observed_server_tick_at_arrival - estimate(t_i)
```

Using `d_i = 0` produces the conservative historical anchor already described. Using
`d_i ≈ RTT/2` estimates server “now” under a symmetric path, but includes error from asymmetric
routing, snapshot cadence, and polling. A robust estimator would:

1. Reject stale samples before clock processing.
2. Filter delay/phase observations and reject large transient outliers.
3. Slew phase gradually and adjust rate only for sustained clock drift.
4. Keep the exposed estimate monotonic; do not jump a visible clock backward for ordinary jitter.
5. Increase uncertainty while snapshots are missing and correct when accepted samples resume.
6. Hard-rebase only at startup or after a separately specified large-error/recovery threshold.

Higher stable RTT would move a delay-compensated estimate forward gradually. Jitter would widen
observation noise rather than directly shake the displayed clock. Packet loss would leave the
estimate advancing from local steady time with growing uncertainty. No concrete filter, delay
sample selection rule, slew limit, or rebase threshold is implemented or approved yet, so this
model must not be presented as current behavior.

If a UI should match the delayed remote scene rather than estimate server “now,” derive its time
from the Feature 12 remote presentation cursor instead. The owned predicted player and delayed
remote entities intentionally occupy different presentation timelines, so a rendered frame does
not have one exact authoritative tick.

#### Two clients observing one server

This illustrative instant assumes the live server has completed tick 300:

| Value | Server | Client 1 | Client 2 |
|---|---:|---:|---:|
| Shared authoritative tick/world time | `300` / `10.000 s` | Same server truth, not directly visible live | Same server truth, not directly visible live |
| Client joined near server tick | — | `240` | `285` |
| Client session time | — | About `2.000 s` | About `0.500 s` |
| Latest received snapshot | — | ID `29`, server tick `298` | ID `7`, server tick `296` |
| Latest-known world time | — | `9.933 s` | `9.867 s` |
| Time since that snapshot arrived | — | `40 ms` | `20 ms` |
| Basic estimated live server time | — | `9.973 s` | `9.887 s` |
| Feature 12 target remote cursor | — | About tick `292` / `9.733 s` | About tick `290` / `9.667 s` |

Snapshot IDs `29` and `7` do not conflict: each belongs to its own client session. Server ticks
`298` and `296` share one server timeline and reveal that Client 2 currently has older knowledge.
Both estimates can legitimately differ from each other and from live tick 300.

Client 1's owned predicted position may already include inputs newer than the ACK in snapshot 29.
Name that state with its rollback base and replay range—for example, “snapshot 29/server tick 298,
ACK 42, replayed inputs 43–44”—rather than assigning it an invented server tick.

#### What reconciliation rewinds

When a newer snapshot arrives, reconciliation rolls the owned predicted state back to that
authoritative controlled-player sample and replays the remaining unacknowledged input. The word
*rollback* applies to that state reconstruction, not to every clock:

| Value | Effect of reconciliation |
|---|---|
| Authoritative server tick/world time | Never rewound by the client. |
| Client session time | Never rewound. |
| Accepted snapshot ID/latest-known server tick | Advance to the newer accepted sample. |
| Owned predicted state | Rebuilt from the new authoritative base plus the retained input suffix. |
| Local presentation position | Preserves continuity with a visual-only offset that decays over 100 ms. |
| Feature 12 remote presentation cursor | Advances monotonically or freezes during a hold; a forward-only hard rebase is presentation recovery, not gameplay rollback. |

| Clock or event | Current cadence | Approximate spacing | What happens |
|---|---:|---:|---|
| Client loop/render frame | Variable; commonly limited by vsync | 16.67 ms on a 60 Hz display | Poll input, run zero or more due fixed steps, extract presentation, submit rendering. |
| Authoritative simulation tick | Fixed 30 Hz | 33.33 ms | Apply current movement, move entities, resolve food collisions, increment server tick. |
| Client input packet | At each due client fixed step | 33.33 ms | Encode and send the newest sampled movement, plus configured redundancy, with a new sequence ID. |
| Full snapshot | Every two server ticks, fixed 15 Hz | 66.67 ms | Send the latest authoritative entity state and input acknowledgement. |
| Transport poll | Once or more at explicit loop points | Render-loop or server-tick dependent | Deliver queued connection and payload events to a runtime. |
| Display refresh | Monitor-dependent | 16.67 ms at 60 Hz | Make a completed GPU image physically visible. |

The 60 Hz render rate in examples below is illustrative, not guaranteed. The default client uses
vsync, but the actual display may refresh at 60, 120, or another rate, and a slow frame can take
longer. The fixed-step accumulator preserves the 30 Hz simulation cadence by running no
simulation step in some fast render frames or multiple catch-up steps in a slow render frame.

Dots does not currently have a separate physics clock. Movement, spatial-grid updates, and food
collision resolution are all part of `World::step()` on the 30 Hz authoritative simulation tick.
Likewise, there is no abstract “network frame.” There are encoded messages, transport events,
and explicit times when each runtime polls its endpoint.

At an illustrative 60 render frames per second, the clocks line up like this:

```text
time                       0 ms       16.7       33.3       50.0       66.7 ms

client render loop          R0          R1         R2          R3         R4
authoritative 30 Hz tick    T20                    T21                    T22
15 Hz snapshot              S10                                            S11

render frames per tick      |----------- two render frames --------------|
server ticks per snapshot   |---------------- two server ticks ----------------|
```

The exact phase can shift, so “two render frames per tick” is not a scheduling guarantee. It is
the simple ratio for a stable 60 Hz render loop and a 30 Hz simulation.

### Where the work occurs in the current in-memory loop

After the initial handshake, one client render-loop iteration is conceptually:

```text
poll SDL input once
measure elapsed wall time
ask the fixed-step accumulator how many 30 Hz steps are due

for each due step:
    map the latest input sample to movement
    client encodes and sends one bounded InputPacket
    server polls, validates, and queues its input samples
    server applies at most one queued sample for this tick
    server advances one authoritative World tick
    server sends a FullSnapshot if this is every second tick
    client polls and installs any delivered snapshot

extract the owned player/camera from smoothed prediction, or update the confirmed spectator camera
extract remotes from delayed replicated samples
record and submit GPU rendering
present through the swapchain
```

A fast render frame may execute the input-poll and render portions with zero fixed steps. It then
sends no input command and advances no server tick. A catch-up render frame may execute several
fixed steps using the same most-recent input sample. Catch-up is capped so one slow frame cannot
make the client unresponsive indefinitely.

The embedded server is called directly inside this loop only for `--in-memory`. In native mode,
the client and server processes each own their clock. The server loop polls its transport,
advances one 30 Hz tick, emits any due snapshots, and sleeps until its next tick. The client
cannot call or synchronize that loop; it only sends messages and processes messages that have
arrived.

### Concrete current timeline: prediction and authority

Assume a ready in-memory session, a stable 60 Hz client render loop, and that snapshot `S10`
contains server tick `T20`. The player presses and holds right immediately before render frame
`R102`:

```text
time       client/render work                 server work                  visible owned state
------------------------------------------------------------------------------------------------
33.3 ms    R102 polls RIGHT
           predicts and sends I42 ----------> validate I42
                                               step authoritative T21
                                               no snapshot due
           renders predicted I42                                          P(I42)

50.0 ms    R103 polls RIGHT
           no fixed step is due
           sends nothing
           renders current prediction                                     P(I42)

66.7 ms    R104 polls RIGHT
           predicts and sends I43 ----------> validate I43
                                               step authoritative T22
                          <------------------ send periodic snapshot S11
           installs S11, which contains T22
           and last_processed_input = I43
           reconciles with no correction
           renders prediction equal to S11                                P(I43) = S11
```

Input `I42` is measured and sent in client render frame `R102`. The server processes it before
advancing authoritative tick `T21`, inside that same outer loop iteration because the in-memory
transport adds no delay. The owned player already renders the predicted result of `I42`; remote
and replicated state remain at `S10` because no snapshot is due on that odd tick.

The server sends `S11` after tick `T22`, two render frames later in this example. `S11` is not a
special response to `I42`; snapshots are a periodic stream containing the newest cumulative
state. It also includes the newer `I43` acknowledgement because that input was processed before
`T22`. The client installs it, discards acknowledged history, and finds that replay produces its
existing prediction, so no smoothing correction is needed.

This gives four useful answers for the example:

| Question | Answer |
|---|---|
| When was the input measured? | SDL input polling in client render frame `R102`. |
| When did authority process it? | Immediately before server simulation tick `T21`. |
| When was its result sent back? | As part of periodic snapshot `S11`, after tick `T22`. |
| When did the player see it? | Predicted and submitted by `R102`, physically visible on a following display refresh. |

Depending on phase, a newly held input may just make a simulation tick and snapshot, or it may
wait nearly one input interval before processing and then one additional server tick for the next
snapshot. Together those waits can approach one 15 Hz snapshot interval. Even with zero network
latency, fixed sampling and display presentation therefore add some delay.

## What real latency adds

A native network separates the client and server timelines. An input can wait for the next
client send opportunity, travel to the server, wait for the next server tick, wait for the next
snapshot, travel back, wait for the next client render, and finally wait for display refresh.

Without prediction, input-to-photon delay is approximately the sum of:

```text
client input/send phase
+ client-to-server network time
+ server poll/tick phase
+ wait for the next due snapshot
+ server-to-client network time
+ client receive/render phase
+ GPU/display presentation
```

For a concrete example, assume 100 ms round-trip time with symmetric 50 ms travel in each
direction, a 60 Hz client, the current 30 Hz server, and 15 Hz snapshots. The phases below are
chosen to show the waits; real phases vary:

```text
time       event
------------------------------------------------------------------------------------------------
  0.0 ms   Client render R200 samples RIGHT and sends input I42.
           The uncompensated client continues rendering its old replicated state.

 50.0 ms   I42 reaches the server, just after a server tick deadline.

 66.7 ms   Server tick T201 polls and applies I42, then changes authoritative position.
           This tick is not snapshot-due, so nothing is returned yet.

100.0 ms   Server tick T202 advances and emits snapshot S101 containing the new position
           and last_processed_input = I42.

150.0 ms   S101 reaches the client. Its next render-loop poll installs the snapshot and
           submits the updated position.

150-166.7 ms
           The display refresh makes the submitted image visible.
```

In this example, a nominal 100 ms RTT produces roughly 150–167 ms from input sampling to visible
authoritative response. RTT is only the travel component; tick alignment, snapshot cadence, and
render/display phase add more time. A different phase could be faster or slower.

Latency is also not constant. **Jitter** changes arrival times, so a packet can miss one server
tick or snapshots can arrive unevenly. **Loss** can remove an unreliable input or snapshot.
**Reordering** can deliver an older packet after a newer one. Sequence IDs let the runtimes
ignore stale data, but an uncompensated client must hold its last snapshot longer when a newer
one is late or lost.

Prediction changes what the local player sees, not when the server becomes authoritative. In the
same 100 ms RTT example, a predicted client applies `I42` locally and can submit the movement in
`R200`, making it visible within roughly one display refresh. Around 150 ms later, `S101` still
arrives and reconciles that prediction. A correct prediction produces little or no correction;
a disagreement corrects gameplay state immediately while presentation smooths the visible jump.

## The eventual responsive-client mental model

Prediction and interpolation introduce more client-side views, but they do not weaken server
authority. Keep four categories separate:

```text
Authoritative state   complete server-owned gameplay World
Replicated state      latest validated network state received by this client
Predicted state       locally advanced state for the client's owned player
Presentation state    positions and effects actually drawn on screen
```

These states may briefly disagree for expected reasons. Their ownership and correction rules
are what keep the system understandable.

### Local player: predict, acknowledge, reconcile

For the local player, responsiveness will come from applying movement immediately:

1. The client creates input sequence 42.
2. It sends input 42 to the server, stores it in a bounded history, and immediately applies it
   to local predicted movement.
3. The next frame renders that predicted result without waiting for a round trip.
4. The server later validates and applies input 42 during its authoritative tick.
5. A snapshot returns authoritative player state and `last_processed_input = 42`.
6. The client discards acknowledged inputs through 42.
7. It resets predicted simulation state to the authoritative state from the snapshot.
8. It replays any still-unacknowledged inputs 43 and newer.

In shorthand:

```text
new predicted state = authoritative snapshot + replay(unacknowledged local inputs)
```

If prediction matched, the correction is invisible. If it differed because of collision,
floating-point drift, packet loss, or server validation, predicted simulation corrects
immediately. Only its rendered position eases toward the corrected value.

The critical rule is:

> Correct simulation state immediately; smooth presentation state.

Gradually moving gameplay state toward the server result would make later predictions start
from knowingly false state and compound the error.

Feature 14 step 7 predicts movement, food consumption, player absorption, split, launch,
cohesion, and merge inside a closed Dots World. A correction may therefore change mass or
topology as well as position. Predicted loss of the final owned piece remains speculative:
Playing/Spectating, defeat, follow, and respawn state change only from confirmed snapshots.

### Remote players: predict in the closure; extrapolate or interpolate outside it

The client cannot know another human's future input. Feature 12 provides the conservative
strategy: render slightly in the past between two known snapshots.

```text
received snapshots:        S10 -------- S11 -------- S12
render timeline:                 ^ interpolate here
                              delayed by 2-3 intervals
```

This adds a small intentional presentation delay but provides two known endpoints for smooth
motion. A later adaptive buffer can grow or shrink based on measured jitter. Remote gameplay
state is never invented by running guessed remote input.

The owned player and every entity in its interaction closure are rendered from the current
Predicted World. Remote participants in that closure use recorded last-known level movement and
no invented edge actions, so shared collision and gameplay logic can affect the owned island.
While Playing, entities outside the closure default to advancing their newest known owner
movement and launch velocity for at most six ticks/200 ms, then hold. This path shares the Dots
movement/launch integrator and launch decay but never runs cohesion, collision, food consumption,
absorption, split, merge, or checkpoint logic. New authority and closure entry smooth only the
visible presentation residual. Delayed interpolation remains the spectator, selectable fallback,
and comparison path.

### What “latency compensation” means here

The phrase is broad. For this project, the immediate roadmap means:

- **Client-side prediction** for responsive interaction-closed gameplay.
- **Reconciliation** against server acknowledgements and verified authoritative checkpoints.
- **Presentation smoothing** for visible local corrections.
- **Remote interpolation** for other players under snapshot delay and jitter.
- **Interaction-closed complete-World rollback** for reversible mechanics around owned pieces.

In some action games, “lag compensation” specifically means the server rewinds past world state
when evaluating a shot. Dots currently has no such mechanic planned. Prediction and
interpolation solve client responsiveness and smoothness; any future server rewind would be a
separate, gameplay-specific authority policy.

### Rollback programming-model direction — Features 13 and 14

Feature 13 first adds deterministic authoritative absorption, defeat, spectating, and respawn.
Those results remain unpredicted so the project has a trustworthy baseline for contested
outcomes and session transitions. The Gameplay tab and session logs expose those repeated,
recipient-specific results; its deadline countdown is presentation-only and does not participate
in simulation or eligibility.

Feature 14 now uses a game-neutral typed rollback timeline and restores a complete Dots World
from an authoritative checkpoint before atomically replaying retained stimuli. Sampled local
commands remain immutable; a typed, transactional refresh hook lets the game revise only
authority-derived assumptions before each retained step. The Dots default is an
owned-and-interacting fixed-point closure; full-replicated mode is an oracle/benchmark and owned
gameplay is the incomplete-state fallback. Closure includes non-spatial owner state, required
global rule/timer domains, mechanic dependencies, and explicit causal authority facts; it is not
only a radius query.

Reversible World state may be predicted. Deterministic simulation events are regenerated during
replay, while external presentation consequences run only after an atomic commit through
per-handler `PredictOnce`, `PredictCancelable`, or `ConfirmOnce` policies. A non-rewindable
occurrence ledger prevents replay from repeating one-shot feedback. Confirmed transient
consequences use sequenced receipts repeated until acknowledged; durable Playing/Spectating and
respawn state remains repeated snapshot state. The kernel and Dots adapter implement and test
these policies. Production Dots presentation uses `PredictOnce` split/consume flashes,
`PredictCancelable` split-launch, food-pop, and consume-collapse tokens, and a `ConfirmOnce`
kill/defeat banner plus monotonic future-audio hook.

The timeline completes replay in the frame that accepts the snapshot unless measured workloads
justify the separately planned conditional multi-frame spike.

The canonical checkpoint, replay, predicted-entity, event/consequence, adaptive-buffer, and
recovery contracts are in [`rollback_prediction_design.md`](rollback_prediction_design.md). The
phased implementation is in
[`plans/13-authoritative-interactions-spectating.md`](plans/13-authoritative-interactions-spectating.md)
and [`plans/14-selectable-world-rollback.md`](plans/14-selectable-world-rollback.md).

## How later replication work fits

Prediction makes movement responsive, but it does not make snapshots scalable. Later features
change what is sent without changing who owns truth:

| Feature | Adds | Mental-model effect |
|---|---|---|
| 10: native transport | Cross-process connections and network impairment testing | Same messages and authority over a real network. |
| 11: prediction/reconciliation | Input history, replay, and local correction smoothing | Owned movement responds immediately. |
| 12: remote interpolation | Buffered presentation snapshots | Other players move smoothly under jitter. |
| 13: authoritative interactions | Absorption, defeat, spectating, and optional respawn | Contested and durable outcomes gain a server-owned baseline. |
| 14: rollback programming model | Game-neutral typed timeline, interaction-closed Dots replay, structural lifecycle, consequence policies, and adaptive command timing | Reversible gameplay responds immediately while one-shot and durable consequences have explicit delivery semantics. |
| 15: interest management | Per-client area-of-interest filtering | Clients receive only relevant authoritative entities; prediction membership follows coherent AOI entry/exit. |
| 16: delta snapshots | Baselines, field masks, quantization, and byte budgets | Snapshots describe changes rather than replacing everything on the wire. |

A delta snapshot is still authoritative. The client reconstructs a newer replicated view from a
known baseline and validated changes. Missing or reordered data must cause recovery from a newer
self-contained baseline, never partial mutation into an uncertain world.

## Networking observability roadmap

The current overlay separates replication health from transport health. Replication data
includes server tick, snapshot ID, entity counts, latest snapshot age, and accepted snapshot
rate. Native transport data includes connection state, RTT, packet loss, traffic rates, outbound
queue depths, and queue delay. In-memory endpoints report connection state but leave measurements
they cannot provide unavailable instead of implying zero latency or loss.

The client runtime exposes these protocol/prediction values programmatically and the ImGui
**Session** and **Prediction** sections display them:

- Client session state.
- Last input sequence sent and last input sequence acknowledged by a snapshot.
- Number of currently unacknowledged inputs.
- Replay counts/durations, correction counts/distances, history pressure, server input depth,
  over-budget replays, and hard resyncs.

These are replication/runtime statistics, not measurements of a real network.

The recommended staging is:

| Feature | Debug information to add |
|---|---|
| 09: in-memory replication | Tick, snapshot, entity, session, and fixed-step data. |
| 10: native transport | Snapshot age/rate plus connection state, RTT, packet loss, traffic rates, outbound queues, and queue delay. |
| 11: prediction/reconciliation | Unacknowledged input count, replay count, correction distance, correction frequency, and presentation smoothing offset. |
| 12: remote interpolation | Snapshot-buffer fill, interpolation delay, measured jitter, late snapshots, and extrapolation/hold events. |
| 13: authoritative interactions | Session mode, owned pieces, confirmed killer, defeat/respawn ticks, and authoritative absorption/respawn results. |
| 14: rollback programming model | Prediction profile/closure, complete replay range/cost, structural divergence, event/consequence lifecycle, receipt health, command-buffer control, and state-layer overlays. |

Genuine **network transport stats come from GameNetworkingSockets**. They remain labeled
separately from replication health. One-way latency should not be inferred by simply halving RTT
because clocks and routes can differ.

## Useful invariants when changing networking code

- Only the server owns and steps the authoritative multiplayer `simulation::World`.
- A client connection can submit intent only for the player assigned to that session.
- The protocol owns Dots wire types; the transport carries opaque bytes.
- Simulation storage IDs and protocol IDs cross only through explicit mapping.
- Reliable delivery is for durable control transitions, not ordinary real-time snapshots.
- Stale input and snapshots are ignored by application sequence, not allowed to roll state back.
- Malformed peer data cannot corrupt trusted state or stop unrelated sessions.
- Prediction may speculate locally, but reconciliation always accepts server truth.
- Gameplay correction is immediate; visual correction may be smoothed.
- Feature 12 remotes are interpolated from known samples. Feature 14 predicts recorded held
  movement only inside a closed gameplay island, never guesses remote edge actions, and keeps
  outside-closure extrapolation presentation-only and bounded.
- Storage capacity, retained replay depth, causal scope horizon, authority input hold, and remote
  presentation delay are different bounds. Predicted or presentation state never certifies the
  authority provenance of a future stimulus. See the
  [Feature 14 prediction-stutter postmortem](feature14_prediction_stutter_postmortem.md).
- Presentation and replicated state are client views and never become server authority.

## Where to read the implementation

- `games/dots/protocol/`: message value types, strong wire IDs, framing, encoding, and decoding.
- `engine/net_transport/`: the game-neutral endpoint contract plus deterministic in-memory and
  native GameNetworkingSockets implementations.
- `games/dots/server/`: connection sessions, handshake validation, input ownership, stepping,
  snapshots, and peer rejection.
- `games/dots/replication/`: simulation/protocol ID mapping, snapshot construction, and the
  client-side replicated world.
- `games/dots/client_runtime/`: handshake state, input sequencing, snapshot application, and
  client session state.
- `games/dots/simulation/`: authoritative Dots rules and fixed-step world, independent of
  transport and rendering.
- `games/dots/presentation/`: extraction of replicated entities into client-only draw data.
- `games/dots/apps/client/src/client_app.cpp`: composition of the offline, in-memory, and native
  runtime modes.
- `docs/plans/08-protocol-binary-codec.md`: the implemented wire-format slice.
- `docs/plans/09-inmemory-transport-integration.md`: the implemented authoritative in-memory
  slice.
- `docs/plans/10-gamenetworkingsockets-transport.md`: the implemented native transport slice.
- `docs/development_branch_plan.md`: Features 11–14 and their exit criteria.

## Glossary

| Term | Meaning in this project |
|---|---|
| Authority | The system whose gameplay result is final; for multiplayer Dots, the server. |
| Baseline | An acknowledged snapshot used to reconstruct a later delta snapshot. |
| Client-side prediction | Applying owned input locally before the server result returns. |
| Codec | The encoder and hostile-input-safe decoder between typed messages and bytes. |
| Connection handle | Transport-owned identity for one endpoint relationship. |
| Delta snapshot | A snapshot containing changes relative to a known baseline. |
| Delivery mode | Reliable or unreliable transport intent for one payload. |
| Full snapshot | A complete replicated entity view that replaces the previous client view. |
| Input acknowledgement | The newest client input sequence included in authoritative state. |
| Input command | Sequenced client intent such as a normalized movement direction. |
| Input-to-photon latency | Time from sampling input until its result becomes visible on a display. |
| Interpolation | Rendering between two known samples, normally with an intentional delay. |
| Jitter | Variation in network arrival timing. |
| Latency | Time taken for information to travel and be processed. |
| Presentation state | Client-only state actually rendered, which may be visually smoothed. |
| Predicted state | Client-only owned-player state advanced using unacknowledged local input. |
| Protocol | The game-owned definition of message meanings and their wire representation. |
| Reconciliation | Resetting prediction to server state and replaying unacknowledged input. |
| Render frame | One variable-rate client loop iteration that polls input and submits an image. |
| Replicated state | The validated authoritative subset reconstructed on a client. |
| RTT | Round-trip time for information to travel to a peer and back, excluding some application waits. |
| Server tick | Monotonic fixed-step index of the authoritative simulation. |
| Snapshot cadence | Frequency at which authoritative replication samples are sent to a client. |
| Snapshot acknowledgement | The newest snapshot a client reports receiving. |
| Transport | Game-neutral connection and opaque byte-payload delivery. |
