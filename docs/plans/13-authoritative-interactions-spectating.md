# Feature 13: Authoritative Interactions and Spectating

## Purpose

Add the first contested player outcome before optimizing replication. The server resolves player
absorption, owns defeat and respawn eligibility, and keeps defeated connections alive as
spectators. This feature proves authority and session lifecycle without yet expanding Feature
11's movement-only prediction.

Feature 14 will later speculate these simulation results through the contracts in
`../rollback_prediction_design.md`. Feature 13 must first provide a deterministic authoritative
baseline against which that prediction can be tested.

## Scope and Authority

The client continues to submit intent only. It cannot submit position, mass, collision result,
killer, session state, cooldown completion, or spawn location.

Feature 13 includes:

- Deterministic player-versus-player absorption.
- Player ownership and per-tick absorption events in Dots simulation.
- Playing and spectating session states.
- Confirmed free-camera and follow-killer spectator presentation.
- Optional server-authorized respawn after a configurable deadline.
- Gameplay/session debugging and documentation.

Feature 13 does not include split/merge, full-World rollback, predicted death, predicted respawn,
AOI filtering, delta snapshots, scoring, kill feeds, server-side rewind, or dynamic food
replenishment. Agar.io-style food replenishment remains a separate post-Feature-14 gameplay
feature so its spawn schedule and any random state are designed as rollback-checkpoint state.

## Deterministic Absorption Rules

Use post-movement positions and the mass/radius state present at the start of the collision
phase.

1. Gather player overlaps before changing mass or topology. Touching circles count as overlap.
2. A player is eligible to absorb another player only when its mass is strictly larger. Equal
   masses never absorb one another.
3. Sort candidates by descending absorber mass, ascending absorber entity ID, then ascending
   victim entity ID.
4. Process the sorted candidates against a live/dead set. A removed absorber cannot consume a
   later victim; a victim can be consumed once.
5. Transfer the victim's entire mass and remove the victim.
6. Resolve food claims after player absorption, discarding claims from removed players.
7. Apply mass/radius changes after arbitration. Growth cannot create a new overlap or eligibility
   decision until the following tick.

The procedure is deterministic even when several players overlap. Same-owner pieces are exempt
from absorption once Feature 14 adds multi-piece ownership.

The World reports value events for the completed tick:

```cpp
struct PlayerAbsorbed {
    mycore::time::Tick tick;
    EntityId absorber_entity_id;
    EntityId victim_entity_id;
    PlayerOwnerId absorber_owner_id;
    PlayerOwnerId victim_owner_id;
    float transferred_mass;
};
```

`World::step()` remains all-or-nothing and makes the completed tick's events available without
introducing server, protocol, SDL, or presentation dependencies into simulation.

## Ownership and Session State

Add a Dots simulation `PlayerOwnerId` independent of protocol `ClientId`. Replication maps between
them. Food has no player owner.

The server session state is:

```cpp
enum class SessionMode : std::uint8_t {
    Playing,
    Spectating,
};
```

A session also owns:

- Its current player pieces; Feature 13 has zero or one, but the representation supports many.
- Last processed input and pending input queue.
- Confirmed killer/follow entity when available.
- Defeat tick and `respawn_available_tick`.

The session enters `Spectating` only after the authoritative World reports that its last piece was
absorbed. It stays connected and continues receiving snapshots. Disconnect cleanup removes owned
pieces but does not create a replacement.

## Safe Spawn and Respawn

Feature 13 initially used the following server-owned deterministic search:

1. Enumerate a square spiral centered at the origin on a 12-world-unit lattice.
2. Choose the first point where the initial player circle does not touch or overlap any live
   player circle.
3. Food does not invalidate a candidate.
4. Continue until a safe point is found or the next spiral point is outside the spatial grid's
   representable lattice.
5. Fail explicitly and distinguish entity-ID exhaustion from no representable safe spawn.

The client never proposes a spawn position.

The implemented Feature 13 follow-up replaces the origin restart with the collision-safe,
active-count indexed square-ring search in
[`authoritative-spawn-search.md`](authoritative-spawn-search.md). The client-authority and failure
contracts above remain unchanged.

Feature 13 has no fixed simultaneous-session target. The search must not retain the current
77-slot ceiling, and a deterministic test places 1,000 initial-size players without overlap, but
that is a spawn-contract test rather than a 1,000-session networking or performance commitment.

The default respawn cooldown is 90 server ticks, or three seconds. Add a server runtime setting
and `dots_server --respawn-cooldown-ticks <count>` override. The immutable value is announced to
the client during the handshake.

Respawn is never automatic. A spectator may request it after the deadline or continue spectating
indefinitely. A request before the deadline is consumed and acknowledged but rejected by gameplay
rules. A successful request creates a new authoritative player, clears killer/defeat state, and
returns the session to `Playing`. The server repeats the latest request sequence and an explicit
result (`Accepted`, `RejectedCooldown`, `RejectedNotSpectating`, or `RejectedNoSafeSpawn`) in
snapshots. An input acknowledgement alone never communicates gameplay success.

## Protocol Version 3

Bump the Dots protocol to version 3 with no dual-version negotiation.

Add:

- Entity owner ID for player entities.
- Recipient session mode in every full snapshot.
- Optional primary/follow entity IDs.
- Defeat and respawn-available server ticks.
- Immutable match configuration in `ServerWelcome`, beginning with respawn cooldown.
- A known respawn action bit in `InputSample`.
- The latest authoritative absorption involving the recipient session.
- The latest respawn request sequence and explicit result.

The durable session state repeats in unreliable snapshots, so one lost transition packet cannot
strand the client. `last_processed_input_id` means that the sample was consumed; snapshot session
state reveals whether respawn succeeded.

The message model groups those recipient-specific fields in one `RecipientSessionState` value.
Replication validates and commits that value with the entity list, giving Feature 14 one atomic
authoritative topology-and-lifecycle checkpoint instead of inferring defeat from a missing entity.

Codec validation rejects unknown modes/bits, invalid owner/session combinations, impossible
deadline ordering, duplicate owned entities, non-finite gameplay values, and malformed optional
IDs.

The client runtime must no longer treat a missing controlled entity as a protocol error when the
snapshot says `Spectating`. It exposes session mode, owned entity IDs, primary entity, killer, and
respawn deadline rather than assuming one permanent controlled entity.

## Spectator Presentation and Controls

Enter spectator presentation only from confirmed session state.

Modes:

- **Follow killer:** default after defeat when the confirmed killer still exists. Camera position
  follows the same presentation sample used to draw that entity.
- **Free camera:** pan in world space and retain independent zoom.

If the follow target disappears, switch to free camera at its last valid presentation position.
Do not invent another target.

Default controls while spectating:

- WASD or arrows: free-camera pan.
- Mouse wheel or PageUp/PageDown: zoom, clamped to a finite positive configured range.
- `F`: toggle between free camera and the confirmed killer when available.
- `R` or Enter: request respawn.

Extend the SDL input snapshot with wheel delta because zoom is a demonstrated cross-game platform
input need. Reuse the configured movement bindings for free-camera pan and add Dots-owned
configuration/schema fields for follow, respawn, and keyboard zoom bindings. Default free-camera
pan speed is 12 world units per second. Zoom is clamped to 5--80 pixels per world unit and changes
in 10 percent steps from either the wheel or keyboard. Holding a respawn key produces one
edge-triggered action rather than one request per input tick.

The spectator camera is client presentation state. Feature 15 will define a bounded, validated
camera-interest intent for AOI; until then it changes no server state.

## Debugging and Documentation

Add a **Gameplay** debug tab rather than lengthening the Prediction tab. Display:

- Server-assigned client ID and session mode.
- Owned piece count and primary entity.
- Confirmed killer/follow entity.
- Defeat tick, respawn-available tick, and countdown derived from estimated server time.
- Latest authoritative absorption event involving this session.
- Latest respawn request sequence and confirmed/rejected result.

The countdown is presentation only; the server tick deadline decides eligibility.

Update the networking, rollback-design, and debugging guides with current/planned status. Update
README runtime controls and server options when the phase lands.

## Implementation Checkpoints

Do not start a checkpoint until the preceding checkpoint is reviewed and approved.

### Phase 13.1: Deterministic simulation

- [x] Add player ownership and deterministic safe-spawn selection.
- [x] Add absorption arbitration, mass transfer, removal, and step events.
- [x] Define player/food ordering and mass-conservation invariants in tests.
- [x] Keep protocol-v2 server sessions in a temporary shared-owner compatibility group so the
  simulation checkpoint cannot remove a client's permanent controlled entity. Phase 13.2
  replaces this gate with distinct owners and durable lifecycle replication.
- [x] Phase 13.1 approved.

### Phase 13.2: Protocol and session lifecycle

- [x] Add protocol version 3, session/config fields, and respawn action validation.
- [x] Keep defeated sessions connected and repeat their durable state in snapshots.
- [x] Add optional server-configured respawn and safe authoritative re-entry.
- [x] Phase 13.2 approved.

### Phase 13.3: Spectator presentation

- [x] Add confirmed follow-killer and free-camera modes.
- [x] Add pan, zoom, follow-toggle, and respawn controls plus configuration/schema updates.
- [x] Handle missing follow targets without changing authority.
- [x] Phase 13.3 approved.

### Phase 13.4: Observability and validation

- [x] Add Gameplay tab fields, logs, and authoritative event visibility.
- [x] Run focused simulation/protocol/session/presentation tests.
- [x] Run two-client native and in-memory impairment scenarios.
- [x] Synchronize canonical documentation and README.
- [ ] Feature 13 completion approved before Feature 14 implementation.

## Test Plan

Simulation tests cover:

- Equal masses touching without absorption.
- Any strictly larger touching player absorbing the smaller.
- Several eligible absorbers and victims with deterministic ordering.
- An absorber that is itself removed earlier in ordering.
- Player absorption and contested food in one tick.
- Mass conservation, radius updates, spatial-grid consistency, and emitted events.
- Safe spawn repeatability and non-overlap for many players.
- Safe spawn of 1,000 initial-size players without a fixed slot ceiling.

Protocol/runtime tests cover:

- Version-3 golden bytes and hostile-input validation.
- Playing-to-spectating transition and repeated state after snapshot loss.
- Input ACK independent from respawn success.
- Early respawn rejection, eligible respawn success, and indefinite spectating.
- Disconnects in both session modes.
- No client message capable of choosing position, mass, killer, deadline, or session mode.

Presentation tests cover follow/free transitions, zoom bounds, missing targets, respawn edge input,
and consistent camera/entity sampling.

## Exit Criteria

- Contested player absorption is deterministic and server-owned.
- Defeated clients remain connected, can freely spectate, and optionally respawn after the
  configured deadline.
- Packet loss cannot lose durable session state.
- Debug output explains who was absorbed, by whom, at which server tick, and when respawn becomes
  eligible.
- No prediction beyond Feature 11 movement is introduced yet.
