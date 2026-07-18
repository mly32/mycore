# Protocol, Transport, and Server-Authoritative Networking

This guide assumes you understand the basic idea of a client and server: the client collects
input and draws the game, while the server runs shared gameplay. It explains the boundaries
between Dots protocol messages, the game-neutral transport, authoritative simulation, replicated
client state, and the prediction and interpolation planned for later features.

## The short version

The current networked path is deliberately simple:

```text
client input
    |
    | InputCommand bytes
    v
transport -> authoritative server simulation -> FullSnapshot bytes -> replicated client world
                                                                  |
                                                                  v
                                                               rendering
```

The server owns the only gameplay `World`. The client sends movement requests and renders the
latest state received from the server. It does not currently simulate its player locally in
networked mode.

`dots_client --in-memory` exercises this complete path without opening a socket. Its client and
server live in one process and communicate through the same abstract transport interface that a
native network backend will implement. This proves the ownership and message flow, but the
in-memory backend is FIFO and lossless and does not introduce real latency, jitter, or packet
loss.

The current mode has **no client-side prediction, reconciliation, or remote interpolation**.
That is why its overlay says `NETWORKED FIXED` and why movement advances at the 15 Hz snapshot
rate even though the server simulates at 30 Hz.

## Three different responsibilities

Protocol, transport, and authority answer different questions:

| Layer | Question it answers | Current owner |
|---|---|---|
| Protocol | What do these bytes mean? | `Dots::Protocol` |
| Transport | How do byte payloads move between connected endpoints? | `MyCore::NetTransport` |
| Server runtime and simulation | Which requests are accepted, and what is true now? | `Dots::Server` and `Dots::Simulation` |
| Replication | Which authoritative state is sent and installed? | `Dots::Replication` |
| Client runtime | What is this client's session and latest replicated view? | `Dots::ClientRuntime` |
| Presentation | How does received state become visible circles and a camera? | `Dots::Presentation` |

Keeping these separate lets protocol tests run without sockets, transport tests run without Dots,
and server tests run without a window or GPU.

## Protocol: the meaning of the bytes

The Dots protocol is a game-owned contract. It defines four messages today:

| Message | Direction | Delivery | Purpose |
|---|---|---|---|
| `ClientHello` | Client to server | Reliable | Request a session using the supported protocol version. |
| `ServerWelcome` | Server to client | Reliable | Assign the client ID and controlled entity ID. |
| `InputCommand` | Client to server | Unreliable | Submit sequenced movement and acknowledge the latest received snapshot. |
| `FullSnapshot` | Server to client | Unreliable | Replace the client's replicated view with authoritative entity state. |

Every encoded message starts with a 12-byte header containing:

```text
magic "DOTS" + protocol version + message kind + flags + payload length
```

Integers and floating-point bit patterns have defined widths and use big-endian network byte
order. Fields are encoded individually; the implementation never copies a C++ struct directly
onto the wire. This keeps the format independent of compiler padding, machine byte order, and
the server's in-memory component layout.

The decoder treats every packet as hostile input. It checks framing, lengths, version, enums,
IDs, finite numbers, movement ranges, entity uniqueness, and size limits before accepting a
message. A malformed packet produces a typed codec error. At the server boundary, malformed or
wrong-direction messages disconnect only the offending peer.

### Why reliable and unreliable are both useful

Handshake messages are reliable because they are infrequent state transitions that must arrive
in order. Losing a welcome message cannot be fixed by receiving a newer welcome message.

Input and snapshots are time-sensitive. A delayed old movement command or snapshot is usually
less useful than the newest one, so they are sent unreliably and carry application sequence IDs.
The receiver can ignore stale data without waiting for retransmission. Later input redundancy
and self-healing snapshot deltas will tolerate loss without turning the real-time stream into a
reliable queue of obsolete state.

The current in-memory implementation is lossless for both delivery modes. The mode still records
the intended semantics so tests and runtimes do not need a different contract when the native
transport arrives.

### Packet size policy

The codec distinguishes a preferred transport payload from a hostile-input safety limit. Normal
messages should eventually fit within about 1,200 bytes so the application does not routinely
depend on network fragmentation. The decoder's hard limit is 64 KiB so oversized input cannot
cause unbounded allocation or parsing work.

Current full-world snapshots are allowed to exceed the 1,200-byte target while remaining under
the hard limit. The in-memory transport does not model a network MTU. Interest management,
delta encoding, quantization, prioritization, and per-client byte budgets in Features 13–14 will
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

Feature 10 will put a GameNetworkingSockets implementation behind the same endpoint contract:

```text
                 MyCore::NetTransport::Endpoint
                         /              \
                        /                \
        deterministic tests          native network
          InMemoryNetwork        GameNetworkingSockets
```

The protocol and Dots runtimes should not care which implementation carries their bytes. The
native backend adds cross-process connections, encryption, congestion behavior, and realistic
latency/loss simulation; it does not add gameplay replication or prediction by itself.

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
`InputCommand` does not choose an entity to control. The server validates the command, maps its
sequence to a simulation input ID, applies it only to the session's owned player, and advances
the fixed-step world. Movement, collision, eating, mass, spawning, and removal remain server
decisions.

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
2. It creates the next sequenced `InputCommand`, including its latest snapshot ID.
3. The protocol encoder produces bytes and the client sends them unreliably.
4. The server polls, decodes, validates, and applies fresh input to that connection's player.
5. The server advances its authoritative `World` by one 30 Hz tick.
6. Every two ticks, the server builds and sends a 15 Hz full snapshot for each ready client.
7. The client polls and atomically installs a newer snapshot.
8. Presentation extracts circles from the replicated world and centers the camera on the
   controlled protocol entity.

Each full snapshot currently contains every player and food entity, sorted by protocol entity
ID, plus:

- Its snapshot sequence ID.
- The authoritative server tick.
- The last input sequence processed for that client.

`ReplicatedWorld` rejects invalid snapshots, ignores stale snapshots, and fully replaces its
entity collection when it accepts a newer one. It is a client view, not a second authoritative
`simulation::World`.

## The current no-compensation mental model

Today, the networked client renders the last authoritative sample it has received:

```text
server truth at ticks:       0 -- 1 -- 2 -- 3 -- 4 -- 5 -- 6
snapshots sent:              S0      S1      S2      S3
client renders:              S0------S1------S2------S3
```

There is no state between `S1` and `S2` for presentation to estimate. The client holds `S1`
until `S2` arrives, then jumps to `S2`. With the in-memory backend this appears as 15 Hz stepping
inside a faster render loop.

On a real network without compensation, the same model would also put round-trip and scheduling
delay between pressing a key and seeing the returned authoritative result:

```text
press key -> input travels to server -> later server tick -> snapshot travels back -> draw
```

The in-memory mode validates the architecture but largely hides that responsiveness problem
because it has no simulated network delay. Feature 10 makes separate processes possible;
Features 11 and 12 address how the game feels under latency and jitter.

Some groundwork is already present. Input commands have sequence IDs, snapshots acknowledge
`last_processed_input`, and inputs report the latest received snapshot. The current client does
not retain and replay input history, and the server does not yet use snapshot acknowledgements
for delta baselines.

## There is no single “game frame”

The word *frame* is overloaded. Rendering, simulation, replication, transport polling, and the
physical display do not have to advance together.

| Clock or event | Current cadence | Approximate spacing | What happens |
|---|---:|---:|---|
| Client loop/render frame | Variable; commonly limited by vsync | 16.67 ms on a 60 Hz display | Poll input, run zero or more due fixed steps, extract presentation, submit rendering. |
| Authoritative simulation tick | Fixed 30 Hz | 33.33 ms | Apply current movement, move entities, resolve food collisions, increment server tick. |
| Client input command | At each due fixed step in current in-memory mode | 33.33 ms | Encode and send the newest sampled movement with a new sequence ID. |
| Full snapshot | Every two server ticks, fixed 15 Hz | 66.67 ms | Send the latest authoritative entity state and input acknowledgement. |
| Transport poll | No independent frequency in Feature 09 | Called at explicit loop points | Deliver queued connection and payload events to a runtime. |
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
    client encodes and sends one InputCommand
    server polls, decodes, validates, and applies the input
    server advances one authoritative World tick
    server sends a FullSnapshot if this is every second tick
    client polls and installs any delivered snapshot

extract presentation from the latest replicated state
record and submit GPU rendering
present through the swapchain
```

A fast render frame may execute the input-poll and render portions with zero fixed steps. It then
sends no input command and advances no server tick. A catch-up render frame may execute several
fixed steps using the same most-recent input sample. Catch-up is capped so one slow frame cannot
make the client unresponsive indefinitely.

The embedded server is called directly inside this loop only for `--in-memory`. Once Feature 10
runs client and server in separate processes, each owns its own clock. The server loop polls its
transport, advances one 30 Hz tick, emits any due snapshots, and sleeps until its next tick. The
client cannot call or synchronize that loop; it only sends messages and processes messages that
have arrived.

### Concrete current timeline: when one input becomes visible

Assume a ready in-memory session, a stable 60 Hz client render loop, and that snapshot `S10`
contains server tick `T20`. The player presses and holds right immediately before render frame
`R102`:

```text
time       client/render work                 server work                  visible state
------------------------------------------------------------------------------------------------
33.3 ms    R102 polls RIGHT
           sends input I42 -----------------> validate I42
                                               step authoritative T21
                                               no snapshot due
           renders old S10                                                still S10

50.0 ms    R103 polls RIGHT
           no fixed step is due
           sends nothing
           renders old S10                                                still S10

66.7 ms    R104 polls RIGHT
           sends input I43 -----------------> validate I43
                                               step authoritative T22
                          <------------------ send periodic snapshot S11
           installs S11, which contains T22
           and last_processed_input = I43
           submits a render using S11                                      S11 at next refresh
```

Input `I42` is measured and sent in client render frame `R102`. The server processes it before
advancing authoritative tick `T21`, inside that same outer loop iteration because the in-memory
transport adds no delay. The player position changes on the server at `T21`, but no snapshot is
due on that odd tick, so the client still renders `S10`.

The server sends `S11` after tick `T22`, two render frames later in this example. `S11` is not a
special response to `I42`; snapshots are a periodic stream containing the newest cumulative
state. It also includes the newer `I43` acknowledgement because that input was processed before
`T22`. The client installs and submits the new state during `R104`. With vsync, the monitor shows
that submitted image at or shortly after the next refresh.

This gives four useful answers for the example:

| Question | Answer |
|---|---|
| When was the input measured? | SDL input polling in client render frame `R102`. |
| When did authority process it? | Immediately before server simulation tick `T21`. |
| When was its result sent back? | As part of periodic snapshot `S11`, after tick `T22`. |
| When did the player see it? | Submitted by render frame `R104`, physically visible on a following display refresh. |

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

Initial prediction should cover only owned-player movement. Eating, mass changes, spawning,
death, and collisions involving other players remain visibly confirmed by snapshots until there
is a demonstrated need for more speculation.

### Remote players: buffer and interpolate

The client cannot predict another human's future input. Remote players therefore use a
different strategy: render slightly in the past between two known snapshots.

```text
received snapshots:        S10 -------- S11 -------- S12
render timeline:                 ^ interpolate here
                              delayed by 2-3 intervals
```

This adds a small intentional presentation delay but provides two known endpoints for smooth
motion. A later adaptive buffer can grow or shrink based on measured jitter. Remote gameplay
state is never invented by running guessed remote input.

The owned player is therefore usually rendered from current predicted state, while remote
players are rendered from delayed interpolated state. Both are corrected from the same stream of
authoritative snapshots.

### What “latency compensation” means here

The phrase is broad. For this project, the immediate roadmap means:

- **Client-side prediction** for responsive owned-player movement.
- **Reconciliation** against server acknowledgements and authoritative snapshots.
- **Presentation smoothing** for visible local corrections.
- **Remote interpolation** for other players under snapshot delay and jitter.

In some action games, “lag compensation” specifically means the server rewinds past world state
when evaluating a shot. Dots currently has no such mechanic planned. Prediction and
interpolation solve client responsiveness and smoothness; any future server rewind would be a
separate, gameplay-specific authority policy.

## How later replication work fits

Prediction makes movement responsive, but it does not make snapshots scalable. Later features
change what is sent without changing who owns truth:

| Feature | Adds | Mental-model effect |
|---|---|---|
| 10: native transport | Cross-process connections and network impairment testing | Same messages and authority over a real network. |
| 11: prediction/reconciliation | Input history, replay, and local correction smoothing | Owned movement responds immediately. |
| 12: remote interpolation | Buffered presentation snapshots | Other players move smoothly under jitter. |
| 13: interest management | Per-client area-of-interest filtering | Clients receive only relevant authoritative entities. |
| 14: delta snapshots | Baselines, field masks, quantization, and byte budgets | Snapshots describe changes rather than replacing everything on the wire. |

A delta snapshot is still authoritative. The client reconstructs a newer replicated view from a
known baseline and validated changes. Missing or reordered data must cause recovery from a newer
self-contained baseline, never partial mutation into an uncertain world.

## Networking observability roadmap

The current overlay already shows useful Feature 09 replication evidence: presentation mode,
server tick, snapshot ID, entity counts, fixed-step rate, and simulation health. It would also be
reasonable to expose semantic values that the current protocol genuinely knows, such as:

- Client session state.
- Last input sequence sent and last input sequence acknowledged by a snapshot.
- Number of currently unacknowledged inputs.
- Age of the latest installed snapshot.
- Applied, stale, and invalid snapshot counts.

Those are replication/runtime statistics, not measurements of a real network. The FIFO,
lossless in-memory transport cannot provide meaningful ping, RTT, jitter, packet loss,
congestion, or socket-queue values. Displaying zeros for those fields would imply a realism the
backend does not have.

The recommended staging is:

| Feature | Debug information to add |
|---|---|
| 09: in-memory replication | Existing tick/snapshot/entity data; optionally session, input ACK gap, and snapshot age. |
| 10: native transport | Connection state, RTT, packet loss, bytes/packets per second, reliable queue depth, and transport diagnostics. |
| 11: prediction/reconciliation | Unacknowledged input count, replay count, correction distance, correction frequency, and presentation smoothing offset. |
| 12: remote interpolation | Snapshot-buffer fill, interpolation delay, measured jitter, late snapshots, and extrapolation/hold events. |

Therefore, genuine **network transport stats belong in Feature 10**, when GameNetworkingSockets
can supply real measurements and simulated impairment. Replication acknowledgements and snapshot
age may be added earlier if they help inspect Feature 09, but they should be labeled separately
from transport health. One-way latency should not be inferred by simply halving RTT once clocks
and routes can differ.

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
- Remote players are interpolated from known samples, not predicted from guessed input.
- Presentation and replicated state are client views and never become server authority.

## Where to read the implementation

- `games/dots/protocol/`: message value types, strong wire IDs, framing, encoding, and decoding.
- `engine/net_transport/`: the game-neutral endpoint contract and deterministic in-memory
  implementation.
- `games/dots/server/`: connection sessions, handshake validation, input ownership, stepping,
  snapshots, and peer rejection.
- `games/dots/replication/`: simulation/protocol ID mapping, snapshot construction, and the
  client-side replicated world.
- `games/dots/client_runtime/`: handshake state, input sequencing, snapshot application, and
  client session state.
- `games/dots/simulation/`: authoritative Dots rules and fixed-step world, independent of
  transport and rendering.
- `games/dots/presentation/`: extraction of replicated entities into client-only draw data.
- `games/dots/apps/client/src/client_app.cpp`: composition of the current offline and in-memory
  runtime modes.
- `docs/plans/08-protocol-binary-codec.md`: the implemented wire-format slice.
- `docs/plans/09-inmemory-transport-integration.md`: the implemented authoritative in-memory
  slice.
- `docs/development_branch_plan.md`: Features 10–14 and their exit criteria.

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
