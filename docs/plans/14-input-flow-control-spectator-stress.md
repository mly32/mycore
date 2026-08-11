# Feature 14 Input Flow Control and Spectator Stress

## Problem

Feature 14's client command controller normally keeps about two inputs queued on the server, but
its five-percent cadence correction cannot protect an honest client when the authoritative
process runs below the clients' production rate for a sustained period. A Debug session with one
graphical client and fifteen bots eventually filled one session's 64-entry server input queue.
The server correctly applied its hostile-peer overflow guard, but disconnecting a conforming
client is not acceptable overload recovery.

Defeated spectators also continued sending meaningless 30 Hz neutral input samples. That made it
hard to separate full-snapshot replication cost from player prediction and command cost when
testing many mostly-spectating sessions.

## Goals and Non-goals

- Bound the input sequence lead accepted by authority so a conforming client cannot overflow the
  authoritative queue.
- Preserve locally predicted commands, including edge actions, through a temporary authority
  stall.
- Pause and resume command production with hysteresis before retained history can fill.
- Add a first-class, permanent spectator join role for snapshot replication stress.
- Report server timing and input-flow frontiers clearly enough to distinguish overload from
  protocol failure.
- Keep rollback, protocol, session, and presentation mutation single-thread owned.

This work does not make full snapshots scalable, guarantee Debug-build 30 Hz at a particular
client count, add reconnect, or publish participant-only consequence receipts globally.
Interest management, delta snapshots, and formal scale targets remain Features 15--17.

## Chosen Design

Protocol v5 adds a requested/accepted `JoinRole`, an unreliable `ClientStatus` heartbeat, and an
`input_receive_through` frontier in player snapshots. Authority advertises a 32-sequence receive
window derived from its processed-input frontier and rejects samples beyond that grant. The
existing 64-entry queue remains a defensive invariant and is unreachable for a conforming peer.

The client separates transport retention from the existing fixed 256-entry prediction history.
Submitted player commands enter both prediction and a fixed 256-entry unsent outbox. Outbox
entries transmit only through the advertised grant, at no more than four new packets per flush.
At eight unsent commands, composition roots stop producing new command ticks; they resume at two
or fewer.
Paused time is not repaid as a burst. Snapshot processing, presentation, transport polling, and a
5 Hz status heartbeat continue, and edge actions stay latched until accepted.

`JoinRole::Spectator` creates no owner or entity and remains `SessionMode::Spectating` without a
defeat/respawn deadline. Direct spectators receive full snapshots but no participant-only
authority receipts. Both direct and defeated spectators use status heartbeats instead of neutral
30 Hz commands; a defeated player may still submit an explicit respawn action.

The server permits at most five immediate catch-up ticks before rebasing its wall-clock deadline.
No authoritative simulation tick is fabricated or skipped; only unrecoverable wall-time debt is
discarded.

All runtime state remains synchronously owned by its composition-root thread. Transport
implementations may use internal threads, but their `poll`/`send` boundary does not make the
rollback timeline, prediction history, or unsent outbox concurrently mutable.

## Implementation Status

Implemented as Feature 14 step 9 and merged into `main`. Protocol, replication, runtime, app,
launcher, and focused flow-control/spectator tests are complete. The full host test preset
passes. Native application handshakes retry on the same connection and the server handles a
repeated same-role hello idempotently, preventing burst connection fan-in from turning one
stranded initial hello into a whole-session launcher failure. The native soak commands below
remain repeatable operational checks rather than a claimed client-count performance target.

## Interfaces and Data Flow

- `ClientHello` requests `JoinRole::Player` or `JoinRole::Spectator`; `ServerWelcome` echoes it.
- `ClientStatus` carries the newest accepted snapshot and authority-receipt acknowledgement.
- `FullSnapshot::input_receive_through` grants the highest input sequence the session may send;
  it is invalid for direct spectators.
- Client input submission reports local acceptance rather than promising immediate transport.
- Input-flow statistics expose ACK, grant, transmission, unsent/high-water, and pause state.
- `dots_client` and `dots_bot` accept `--spectate`.
- `dots_session.py` accepts independent player and spectator client/bot counts.

## Operational Stress Commands

Run each against a Debug build when changing protocol, replication, or input flow:

```bash
python3 games/dots/tools/dots_session.py \
  --build-dir build/macos-clang-debug --clients 0 --bots 1 --spectator-bots 15 \
  --fake-lag-ms 5 --fake-loss-percent 10 --duration-seconds 180

python3 games/dots/tools/dots_session.py \
  --build-dir build/macos-clang-debug --clients 0 --bots 5 --spectator-bots 10 \
  --fake-lag-ms 5 --fake-loss-percent 10 --duration-seconds 180

python3 games/dots/tools/dots_session.py \
  --build-dir build/macos-clang-debug --clients 0 --bots 15 \
  --fake-lag-ms 5 --fake-loss-percent 10 --duration-seconds 180
```

## Validation and Exit Criteria

- Protocol round trips and rejects invalid roles, statuses, grants, and old versions.
- A direct spectator joins without changing player count, stays live through status heartbeats,
  receives snapshots, cannot submit gameplay input, and cannot respawn.
- A deterministic authority stall fills the client's unsent suffix, pauses at eight, drains
  through grants, resumes at two, and applies a buffered split exactly once.
- A conforming peer cannot exceed the 32-input server receive window; an out-of-window peer is
  isolated without affecting healthy sessions.
- Server overload and recovery logs include tick rate/timing, queue high-water, and all relevant
  sequence frontiers. Post-rejection close logging does not claim a completed session was still
  handshaking.
- Debug native soaks cover one player plus fifteen direct spectators, a mixed player/spectator
  workload, and the original fifteen-player-bot scenario for 180 seconds with 5 ms configured
  one-way lag and ten-percent loss. No unexpected exit, queue overflow, invalid acknowledgement,
  prediction failure, or lost buffered action is allowed. Tick rate is recorded, not gated.
- A burst of at least twelve native peers completes the application handshake; immediate reliable
  messages are drained across poll-group assignment and unanswered hellos retry before the fixed
  startup deadline.
