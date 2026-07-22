# Dots Gameplay

This guide is the current gameplay contract for Dots. It describes implemented rules, not the
future mechanics outlined in feature plans. The server is authoritative for every rule below.

## Current objective

Dots is an early Agar.io-like movement and growth slice. Players move through a shared field and
consume food to gain mass. There is currently no score, win condition, player-versus-player
absorption, defeat, respawn, split, merge, or separate energy system. Food is the current resource
that fills the role an energy pickup might later fill.

## World and food

- The simulation runs at 30 server ticks per second.
- Each player starts with mass `16`; radius is `sqrt(mass)`.
- Food has mass `1`. When a player's circle overlaps food, the food is removed and its mass is
  added to that player.
- The default field contains 272 food entities in an 8-world-unit grid from `x = -80..80` and
  `y = -48..48`, excluding the origin. Food does not currently respawn.

## Joining and spawning

The authoritative server chooses player spawns. It cycles through a deterministic shuffled set of
77 nearby slots in an 11-by-7 layout, skips occupied slots, and includes the assigned position in
the first full snapshot after `ServerWelcome`. Clients never choose or predict their own spawn.

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

## What each client sees

The local controlled player is responsive through client-side prediction: the client applies its
own input immediately, then reconciles with the server's acknowledged snapshot and smooths only
the visual correction.

Remote players are not extrapolated from guessed inputs. The client stores authoritative snapshots
and renders remotes about six server ticks (200 ms) behind the newest known server state,
interpolating between two known samples. If a newer sample is unavailable, the remote holds rather
than inventing movement. See the [networked prediction and time reference](networked_prediction_reference.md)
for state ownership and timing terminology.

Player fill color is a deterministic hash of the authoritative entity ID, so the same player has
the same color on every client for the life of that server world. Mass changes radius, not fill
color.

## Planned gameplay, not current rules

Future plans may add player interactions, defeat/spectating, respawn, scoring or winning,
split/merge actions, cooldowns, and richer resource/energy mechanics. These must be specified in
this guide when they become implemented gameplay rules; feature plans remain design documents
until then.
