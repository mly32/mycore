# Networked Prediction and Time Reference

This reference defines the terms used by Dots networking, presentation, and future rollback
work. It is the canonical vocabulary for those concepts; feature plans define delivery order and
the networking and observability guides describe current behavior.

## State ownership

Keep these values separate. A client must not maintain one ambiguous "current world" that mixes
authority, speculation, and drawing state.

| State | Meaning | Owner | Current status |
|---|---|---|---|
| Authoritative World | Complete gameplay truth after one server tick. | Server | Current |
| Latest replicated snapshot | Newest validated authority known by one client. It is historical when received. | `Dots::ClientRuntime` | Current |
| Owned predicted state | Controlled-player state rebuilt from latest authority plus unacknowledged local inputs. | `Dots::ClientRuntime` | Current |
| Local presentation state | Owned predicted state plus visual-only correction smoothing. | `Dots::Presentation` | Current |
| Remote snapshot history | Accepted semantic entity snapshots retained for delayed sampling. | `Dots::RemotePresentation` | Feature 12 |
| Remote presentation frame | Immutable remote entity states sampled between known historical endpoints. | `Dots::RemotePresentation` | Feature 12 |
| Composed presentation frame | Local presentation combined with remote presentation, camera, and debug layers. | `Dots::Presentation` | Feature 12 |
| Predicted World | Complete checkpoint replayed through retained immutable stimuli, including commands and recorded remote assumptions. | `MyCore::Rollback` timeline with `Dots::Prediction` model | Feature 14 |
| Extrapolated presentation | Bounded visual-only advancement of replicated movement outside the predicted interaction closure. | `Dots::Presentation` | Feature 14 |
| Confirmed consequence | Durable session or game result exposed only after authority reports it. | Server decision, client display | Feature 13+ |

The server is authoritative even when a client runs matching code speculatively. Presentation
state never feeds simulation, replication, or server authority.

## Time coordinates

| Term | Definition | Do not call it |
|---|---|---|
| Authoritative server tick | Count of completed server `World::step()` calls. | Client frame or snapshot ID |
| World simulation time | Authoritative server tick divided by 30 Hz. | Server wall uptime |
| Snapshot ID | Per-session ordering sequence. | A clock or elapsed time |
| Latest-known authoritative tick | Server tick in the newest accepted snapshot. It is exact for that sample but historical. | Server now |
| Client steady/session time | Local monotonic duration used for polling, rendering, and visual smoothing. | Gameplay authority |
| Remote presentation tick | Feature 12 fractional server-tick coordinate used to draw remotes. It targets six ticks behind the newest known tick. | Estimated live server time |
| Owned prediction extent | Authoritative base snapshot/tick plus the exact unacknowledged local-input range replayed from it. | A trustworthy single server tick |
| Correction smoothing age | Local elapsed time within the visual correction decay. | Simulation time |

Feature 12 therefore draws remote entities roughly one network-delivery age plus its intentional
200 ms buffer behind server now. That delay is a presentation tradeoff: it supplies two known
authoritative endpoints at render time. It does not slow the server, alter local input cadence,
or grant the client authority.

## Compensation terms

### Local input prediction and reconciliation

The controlled player applies its own input immediately, retains that input, then rebuilds from a
newer authoritative base and replays the unacknowledged suffix. Simulation corrects immediately;
only the displayed correction is smoothed. Feature 11 currently predicts only owned movement.

### Remote interpolation

The client renders a remote entity at a deliberately delayed tick between two known snapshots.
For example, with snapshots every two server ticks and a six-tick delay, the cursor can normally
blend a pair of historical samples while newer samples absorb jitter and loss. The fallback is a
hold, never guessed movement. Feature 12 implements this policy.

### Extrapolation and dead reckoning

Extrapolation estimates forward from the latest remote state, normally using velocity or a motion
model. It can make a remote look newer, but a human can turn, stop, collide, or change state
without the client knowing. A later authoritative sample may therefore require a visual
correction. This is not rollback unless the client also retained deterministic assumptions and
replayed a simulation history.

Feature 14 may advance remote movement/launch vectors outside its prediction closure for at most
six ticks/200 ms and then hold. This is presentation-only: it cannot collide, consume, split,
merge, seed a checkpoint, or influence closure construction. Feature 12 delayed interpolation
remains the spectator, fallback, and comparison path. Extrapolation is not a replacement for
scalable replication.

### Complete rollback prediction

Feature 14 predicts the fixed-point interaction closure around owned pieces by replaying a
complete Dots World from an authoritative checkpoint through the game-neutral rollback timeline.
Remote held movement is an explicit recorded assumption; unknown edge actions are zero. A
full-replicated profile remains an oracle/benchmark, and incomplete closure falls back to owned
movement rather than guessing contested gameplay.

### Shooter lag compensation / server rewind

In shooter terminology, lag compensation commonly means a server-only historical query for an
arriving shot. The server chooses a bounded past command time, temporarily restores relevant
target collision state, evaluates the authoritative hit, and restores the current World. It is
not client-side prediction, interpolation, or generic rollback. Dots has no planned shot mechanic
or server rewind; absorption remains resolved at the authoritative tick.

## Scale policy

For 1,000 connected players, local input prediction is bounded by owned entities and remote
interpolation is bounded by the entities a client receives. The expensive path is replication
fanout: the current full-snapshot entity payload is 17 bytes, so 1,000 entities to 1,000 clients
at 15 Hz is about 255 MB/s before packet and transport overhead.

Feature 12 establishes a stable presentation baseline. Feature 15 reduces each recipient to an
area of interest (AOI), Feature 16 sends budgeted deltas, and Feature 17 measures staged loads.
Feature 14's interaction closure reduces predicted simulation before AOI, while its
full-replicated mode remains a controlled experiment rather than the claimed steady-state design
for a 1,000-player world.

## Research basis

- [Valve: latency-compensating client/server protocol](https://developer.valvesoftware.com/w/index.php?title=Latency_Compensating_Methods_in_Client%2FServer_In-game_Protocol_Design_and_Optimization&uselang=en)
- [Valve: lag compensation](https://developer.valvesoftware.com/wiki/Lag_compensation)
- [Psyonix: *It IS Rocket Science!*](https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf)
- [Ricci and Carlini: Area of Interest Management in MMOGs](https://arpi.unipi.it/handle/11568/1055121)
