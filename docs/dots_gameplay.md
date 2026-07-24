# Dots Gameplay

This guide is the current gameplay contract for Dots. It describes implemented rules, not the
future mechanics outlined in feature plans. The server is authoritative for every rule below.

## Current objective

Dots is an early Agar.io-like movement and growth slice. Players move through a shared field,
consume food, and can absorb smaller opponents. There is currently no score, win condition,
split, merge, or separate energy system. Food is the current resource that fills the role an
energy pickup might later fill.

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

## Joining and spawning

The authoritative server chooses player spawns. It searches an unbounded deterministic square
spiral centered at the origin on a 12-world-unit lattice and selects the first initial-size circle
that does not touch a live player. Food does not block a spawn. The chosen entity and position are
included in the first full snapshot after `ServerWelcome`; clients never choose or predict them.

Offline play starts its local player at the origin; the food field deliberately leaves that point
empty. Native and in-memory multiplayer use server-assigned spawns.

## Movement and connection loss

Clients submit normalized movement intent at 30 Hz. A player moves at 6 world units per second.
The server consumes at most one queued input sample per player per tick.

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
client applies its own input immediately, then reconciles with the server's acknowledged snapshot
and smooths only the visual correction.

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

Remote players are not extrapolated from guessed inputs. The client stores authoritative snapshots
and renders remotes about six server ticks (200 ms) behind the newest known server state,
interpolating between two known samples. If a newer sample is unavailable, the remote holds rather
than inventing movement. See the [networked prediction and time reference](networked_prediction_reference.md)
for state ownership and timing terminology.

Player fill color is a deterministic hash of the authoritative entity ID, so the same player has
the same color on every client for the life of that server world. Mass changes radius, not fill
color.

## Planned gameplay, not current rules

Future plans may add scoring or winning, split/merge actions, additional cooldowns, and richer
resource/energy mechanics. These must be specified in this guide when they become implemented
gameplay rules; feature plans remain design documents until then.
