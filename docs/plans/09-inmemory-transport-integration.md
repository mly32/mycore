# Feature 09: In-Memory Transport Integration

## Goal

Prove an authoritative Dots client/server flow without sockets. Add a game-neutral transport
contract with a deterministic in-memory backend, Dots replication and runtime libraries, and an
opt-in graphical mode that renders replicated state without owning local simulation.

The dependency direction is:

```text
MyCore::NetTransport

Dots::Replication
  |-- Dots::Simulation
  `-- Dots::Protocol

Dots::Server / Dots::ClientRuntime
  |-- Dots::Replication
  |-- Dots::Protocol
  `-- MyCore::NetTransport
```

## Transport Contract

- Define transport-owned connection handles, reliable/unreliable delivery modes, connected,
  disconnected, and payload events, plus an endpoint interface for polling, sending copied byte
  payloads, and disconnecting.
- Add one deterministic in-memory network with a server endpoint and multiple stable client
  endpoints. Preserve FIFO order, deliver only through polling, isolate clients, and notify each
  side of disconnect exactly once.
- Keep the transport unaware of Dots messages. Feature 10 will implement the same endpoint
  contract over GameNetworkingSockets.

## Authoritative Flow

1. A connected client sends `ClientHello` reliably.
2. The server validates it, creates one authoritative player, assigns client/entity IDs, and
   sends `ServerWelcome` reliably plus an immediate full snapshot unreliably.
3. Ready clients send one sequenced input per 30 Hz tick. The server maps the connection to its
   owned entity, ignores stale input, and advances the authoritative world.
4. The server sends entity-ID-sorted full snapshots every two ticks (15 Hz) with a per-client
   snapshot sequence and last-processed-input acknowledgement.
5. The client atomically installs newer full snapshots and renders only replicated state.

Malformed packets, wrong-direction messages, duplicate handshakes, and pre-handshake input
disconnect only the offender. Disconnect removes its player before the next step. Transport or
runtime exhaustion and trusted-state failures return typed errors instead of corrupting state.

## Executables and Presentation

- Keep offline play as the default. Add `dots_client --in-memory` for one embedded authoritative
  server and client; it sends input and renders fixed replicated snapshots without prediction or
  interpolation.
- Make presentation frame circles identity-free after extraction, retaining typed IDs only while
  querying simulation or replication state. Add replicated-world extraction centered on the
  controlled player.
- Run `dots_server` as a continuous 30 Hz headless heartbeat until interrupted. Add
  `--ticks <positive-count>` for deterministic smoke tests. Cross-process connections remain
  Feature 10 work.
- Share the default food-field setup between offline and authoritative worlds.

## Tests and Exit Criteria

- Test two-client transport delivery, ordering, payload ownership, isolation, send-after-close,
  and exactly-once disconnect events.
- Test deterministic snapshot construction, explicit ID mapping, full replacement, stale
  snapshot rejection, and controlled-entity lookup.
- Test two clients handshaking, receiving snapshots, moving only their owned entities, and
  cleaning up on disconnect. Invalid bytes must disconnect one client without stopping another.
- Test replicated presentation, `dots_server --ticks`, client help, and existing smoke modes.
- The full build and CTest suite remain green; server and transport targets remain headless and
  no new external dependency is introduced.
