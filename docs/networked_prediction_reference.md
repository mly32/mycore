# Networked Prediction and Time Reference

This reference defines the terms used by Dots networking, presentation, and rollback work. It is
the canonical vocabulary for those concepts; feature plans define delivery order and
the networking and observability guides describe current behavior.

## State ownership

Keep these values separate. A client must not maintain one ambiguous "current world" that mixes
authority, speculation, and drawing state.

| State | Meaning | Owner | Current status |
|---|---|---|---|
| Authoritative World | Complete gameplay truth after one server tick. | Server | Current |
| Latest replicated snapshot | Newest validated authority known by one client. It is historical when received. | `Dots::ClientRuntime` | Current |
| Owned predicted state | Owned projection of the complete Predicted World rebuilt from latest authority plus unacknowledged local inputs. | `Dots::ClientRuntime` | Current |
| Local presentation state | Owned predicted state plus visual-only correction smoothing. | `Dots::Presentation` | Current |
| Remote snapshot history | Accepted semantic entity snapshots retained for delayed sampling. | `Dots::RemotePresentation` | Feature 12 |
| Remote presentation frame | Immutable remote entity states sampled between known historical endpoints. | `Dots::RemotePresentation` | Feature 12 |
| Composed presentation frame | Persistent semantic tracks combining predicted, extrapolated/interpolated, consequence, camera, and debug layers. | `Dots::Presentation` | Current, Feature 14 step 7 |
| Predicted World | Complete checkpoint replayed through retained stimuli: immutable sampled commands plus explicit authority-derived assumptions that reconciliation may refresh transactionally. | `MyCore::Rollback` timeline with `Dots::Prediction` model | Current, Feature 14 step 7 |
| Extrapolated presentation | Bounded visual-only advancement of replicated movement/launch outside the predicted interaction closure. | `Dots::Presentation` | Current, Feature 14 step 7 |
| Confirmed consequence | Durable session or game result exposed only after authority reports it. | Server decision, client display | Feature 13+ |

The server is authoritative even when a client runs matching code speculatively. Presentation
state never feeds simulation, replication, or server authority.

The [Feature 14 prediction-stutter postmortem](feature14_prediction_stutter_postmortem.md)
demonstrates the related provenance failure that occurs when a Predicted World value is mistaken
for a newest-authority fact while constructing a future replay stimulus.

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

The client applies its own input immediately to a complete interaction-closed Dots World, retains
that input and its remote movement assumptions, then rebuilds from a newer authoritative
checkpoint and replays the unacknowledged suffix. Simulation corrects immediately; only the
displayed primary-position correction is smoothed. Feature 14 step 7 predicts movement and
structural gameplay inside the closure; confirmed session transitions remain authoritative.

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

Feature 14 step 7 advances remote movement/launch vectors outside its prediction closure for at
most six ticks/200 ms and then holds by default while Playing. This is presentation-only: it
cannot collide, consume, split,
merge, seed a checkpoint, or influence closure construction. Feature 12 delayed interpolation
remains the fallback, comparison, and selectable delayed-spectator path. Spectators default to
that authoritative interpolation and use the bounded Dots kinematic path only for an uncovered
underrun tail, without creating a rollback timeline. Extrapolation is not a replacement for
scalable replication.

### Complete rollback prediction

Feature 14 step 7 predicts the fixed-point interaction closure around owned pieces by replaying a
complete Dots World from an authoritative checkpoint through the game-neutral rollback timeline.
Remote held movement is an explicit recorded assumption; unknown edge actions are zero. A
full-replicated profile remains an oracle/benchmark, and incomplete closure falls back to owned
movement rather than guessing contested gameplay.

That closure is causal rather than purely spatial. It also contains owner-local
cooldown/piece-count state, required global rule/timer domains, mechanic dependencies, and
explicit authority facts.
A mutable global aggregate is predictable only when every contribution that can change it during
the replay window is subscribed; otherwise presentation may show an authoritative base plus a
clearly speculative local delta without using that delta for authoritative consequences.

### Prediction extent and network delay

Owned prediction is command-driven, not RTT-multiplied clock extrapolation. Each accepted local
30 Hz command advances one rollback frame immediately. When authority arrives, the client
restores its checkpoint and replays the exact unacknowledged suffix. That suffix naturally spans
the input-to-authority-to-snapshot acknowledgement delay, so higher RTT normally increases the
number of replayed frames without a separate `RTT * tick_rate` lookahead setting.

The resulting predicted tick is a reconstruction coordinate: authoritative base tick plus
retained command frames. It is not a promise that the live server has reached that tick or will
apply those commands at the same tick numbers. The server applies each session's oldest available
command at its next authoritative tick. `InputSample::client_tick` records local sampling order
but is not trusted or mapped to `server_tick` by the current scheduler.

Adding another RTT of synthetic steps would double-count time already represented by the
retained suffix and require guessed input. Backdating an arriving command would instead require
a separate server rollback or historical-query authority policy.

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
