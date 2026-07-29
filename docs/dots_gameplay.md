# Dots Gameplay

This guide is the current gameplay contract for Dots. It describes implemented rules, not the
future mechanics outlined in feature plans. The server is authoritative for every rule below.

## Current objective

Dots is an early Agar.io-like movement and growth slice. Players move through a shared field,
consume food, and can absorb smaller opponents. The shared deterministic World and offline
rollback model also implement split, launch, cohesion, and merge rules. The graphical client
submits split as an edge-triggered action on Space; protocol v4 carries it to authority while the
complete client rollback timeline predicts its immediate topology and motion.
There is no score, win condition, or separate energy system. Food is the current resource that
fills the role an energy pickup might later fill.

## World and food

- The simulation runs at 30 server ticks per second.
- Each player starts with mass `16`; radius is `sqrt(mass)`.
- Food has mass `1`. When a player's circle overlaps food, the food is removed and its mass is
  added to that player.
- A player can absorb an overlapping opponent only when its mass is strictly larger. Equal-mass
  and same-owner players cannot absorb one another. The victim's full mass transfers to the
  winner before contested food is resolved, using deterministic mass/entity ordering.
- The default field contains 272 food entities in an 8-world-unit grid from `x = -80..80` and
  `y = -48..48`, excluding the origin. Food does not currently respawn.

## Split, launch, cohesion, and merge

A split is an edge action on an owner input command. The default immutable rules are:

- A 15-tick/0.5-second owner recast.
- A maximum of eight pieces per owner.
- Minimum parent mass `16`; each eligible parent divides its mass evenly with one child.
- Child launch speed 18 world units per second with linear decay of 18 world units per second
  squared.
- A 150-tick/5-second merge delay and post-deadline cohesion speed of 3 world units per second.

Eligible parents are processed by stable entity identity until the piece cap is reached. Each
child receives `PredictionKey{owner, input, child ordinal}`. Launch uses current movement,
last non-zero movement, then positive X as deterministic fallbacks. A split input is consumed
even when cooldown, mass, piece cap, or allocator state rejects the action.

After the per-piece merge deadline, pieces move toward their mass-weighted owner centroid.
Touching eligible pieces merge in stable entity order; the lower entity identity survives, mass
is conserved, and position and launch velocity are mass weighted. Enemy absorption still occurs
before same-owner merge, and food consumption occurs afterward. Newly spawned players receive
the same merge-delay baseline, although normal join and respawn create an owner with one piece.

## Joining and spawning

The authoritative server chooses player spawns from a directly indexed deterministic square-ring
sequence on a 12-world-unit lattice. A search begins at the ordinal equal to the current live
player count and advances until it finds an initial-size circle that does not touch a live player.
Every candidate is checked against current player positions and radii through the spatial grid;
food does not block a spawn. Movement, growth, or removal can leave a lower-order hole, but the
server is not required to fill that hole before choosing a later clear point.

The chosen entity and position are included in the first full snapshot after `ServerWelcome`;
clients never choose or predict them. The implementation and alternatives are recorded in the
[authoritative spawn search plan](plans/authoritative-spawn-search.md).

Offline play starts its local player at the origin; the food field deliberately leaves that point
empty. It advances the complete local World through the Dots rollback model using the
full-replicated profile; because no server exists in this mode, it periodically promotes the
committed local checkpoint as its new history baseline. Native and in-memory multiplayer use
server-assigned spawns.

## Movement and connection loss

Clients submit normalized movement intent at 30 Hz. A player moves at 6 world units per second.
The server consumes at most one queued input sample per owner per tick. The simulation installs
at most one owner command and applies its held movement to every piece owned by that owner. A live
graphical network session may own up to eight pieces through edge-triggered split input.

Brief missing input does not immediately stop a player: the server holds the last applied movement
for five ticks. On the following missing-input tick it neutralizes movement, while keeping the
session connected. If the server receives no valid input packet for 90 ticks (three seconds), it
logs a liveness warning, closes the connection, and removes the player. A normal transport
disconnect removes the player promptly instead.

## Defeat and respawn lifecycle

Each network session has a distinct player owner and a confirmed `Playing` or `Spectating` mode.
When the server reports that a session's last piece was absorbed, the session remains connected,
owns no player, and continues receiving full snapshots. Those snapshots repeat its confirmed
killer/follow entity, defeat tick, and respawn deadline, so losing the first transition snapshot
cannot lose the lifecycle state.

Respawn is optional and never automatic. The default deadline is 90 server ticks after defeat;
`dots_server --respawn-cooldown-ticks <count>` changes the immutable value announced during the
handshake. A respawn action before the deadline is consumed and acknowledged but records
`RejectedCooldown`. Requests while playing record `RejectedNotSpectating`; an eligible request
uses the same safe server-owned spawn search and records `Accepted` or `RejectedNoSafeSpawn`.
An input acknowledgement means only that the sample was consumed—the repeated result field
communicates whether the gameplay action succeeded.

## What each client sees

While `Playing`, the local primary player is responsive through client-side prediction: the
client applies its own input immediately to a complete interaction-closed World, then reconciles
with the server's acknowledged checkpoint and replays the retained input suffix. Movement, food
consumption, player absorption, split, launch, cohesion, and merge run through the same
deterministic tick used by authority. Simulation corrects atomically; the primary player's
position correction alone is visually smoothed.

Each retained tick keeps its sampled local command unchanged. Remote movement inside the
prediction closure is a last-known-authority assumption: reconciliation refreshes that derived
field across the retained suffix before replay, and future prediction also uses the newest
authority. A remote input edge that the client could not know may therefore cause one visible
correction; it must not make the remote repeatedly jump between an obsolete guess and the newest
server direction.

The root cause and cross-mechanic prevention rules are documented in the
[Feature 14 prediction-stutter postmortem](feature14_prediction_stutter_postmortem.md).

Space submits one split request per press; holding it does not split every input tick. A predicted
child appears immediately and is matched to authority by `PredictionKey`, even if the server
assigns a different entity ID. A predicted absorption may temporarily remove the final local
piece, but it cannot enter spectator mode or disable input. Only confirmed replicated session
state can do that.

The network client runtime accepts confirmed spectating snapshots without requiring a permanent
controlled entity and continues sending session input/heartbeats. The graphical client enters
spectator presentation only from that confirmed mode. It follows the confirmed killer by default,
using the same delayed interpolated sample for the camera and the killer's circle. `F` toggles a
free camera when that target is available. If the killer disappears, the client switches to free
camera at the last valid presentation position rather than choosing another entity.

In free-camera mode, WASD or the arrows pan at 12 world units per second by default. The mouse
wheel or PageUp/PageDown changes zoom in 10 percent steps, clamped to the configured 5--80
pixels-per-world-unit range. `R` or Enter submits one edge-triggered respawn action; holding the
key does not submit a request every input tick. Camera state and zoom are presentation-only and do
not grant authority or alter server interest filtering.

The debug **Gameplay** tab repeats the confirmed session mode, owned pieces, primary/follow
entities, defeat and respawn ticks, latest session-related absorption, and latest respawn result.
Its respawn countdown advances the latest replicated server tick by local time since snapshot
receipt. That display is approximate and presentation-only; only the server's current tick decides
whether a request is eligible.

Remote entities inside the local interaction closure participate in rollback because their held
level movement, momentum, topology, and collisions can affect an owned piece. The retained
assumption uses their newest authoritative movement; unknown remote edge actions are neutral.
While Playing, remote players outside that closure default to presentation-only extrapolation
from the newest accepted authoritative movement and launch velocity. The client uses the shared
movement/launch integrator for at most six ticks (200 ms), then holds; it does not run cohesion,
collision, consumption, absorption, split, merge, or other gameplay there. A config switch can
instead use six-tick delayed interpolation or overlay that delayed position for comparison.
Spectators always use delayed interpolation and hold on underrun. See the
[networked prediction and time reference](networked_prediction_reference.md) for state ownership
and timing terminology.

Presentation keeps semantic tracks by predicted spawn key when available and entity ID
otherwise. Predicted fixed-tick poses interpolate with the client accumulator. Reconciliation,
prediction-to-remote source changes, and authoritative child-ID remaps preserve the old visual
pose and decay only the presentation residual over 100 ms. Removed circles fade for 100 ms, and
the followed piece leaves an eight-sample/300 ms motion trail.

Rollback-generated event journals drive Dots-owned consequence handlers only after an atomic
timeline commit:

| Mechanic | Visible feedback | Delivery behavior |
|---|---|---|
| Split | 180 ms flash | Predicted once; replay and confirmation cannot duplicate it. |
| Split | Child launch ring/trail, up to one second | Revised, canceled, or confirmed through a keyed token. |
| Food consumption | 250 ms four-particle food pop | Canceled with an 80 ms fade if rollback restores the food. |
| Player absorption | 150 ms consume flash | Predicted once; one brief false positive is accepted. |
| Player absorption | 300 ms victim-collapse pulse | Revised, canceled, or confirmed through a keyed token. |
| Confirmed absorption | 1.5 second kill/defeat banner and monotonic stinger hook | Appears only from authority and only once. |
| Merge | Survivor smoothing and consumed-piece fade | Derived from committed state rather than an event side effect. |

Dots has no audio backend yet. The confirmed stinger sequence is an explicit future-audio hook;
it does not play sound today.

Player fill color is a deterministic hash of the authoritative owner ID. Every split piece owned
by one player therefore keeps the same color, including while a predicted child waits for its
authoritative entity-ID mapping. Mass changes radius, not fill color.

## Planned gameplay, not current rules

Future plans may add scoring or winning, richer resource/energy mechanics, and additional
cooldowns. These remain unimplemented; feature plans remain design documents until the behavior
is added here.
