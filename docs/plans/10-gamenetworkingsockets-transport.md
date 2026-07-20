# Feature 10: GameNetworkingSockets Transport

## Goal

Add a native GameNetworkingSockets backend behind the existing game-neutral endpoint contract.
Keep offline play as the client default, retain deterministic in-memory sessions, and allow
separate graphical clients to connect to the authoritative headless server.

## Transport Contract

- Add direct numeric IPv4 and bracketed-IPv6 listen/connect endpoints while keeping native
  GameNetworkingSockets types private.
- Preserve connected, disconnected, payload, reliable, and unreliable semantics. Send control
  messages without Nagle reliably and input/snapshots without Nagle unreliably.
- Expose connection state and optional native RTT, packet-loss, rate, and outbound-queue
  measurements. In-memory endpoints report state but leave network-only measurements absent.
- Support network-instance outgoing fake lag and loss for development and loopback validation;
  GameNetworkingSockets exposes these simulation settings at global scope.

## Executables and Observability

- Keep offline client play as the built-in and sample default. Add strict `[network]` mode and
  server-address configuration plus `--offline`, `--in-memory`, and `--connect` overrides.
- Make `dots_server` listen natively on `[::]:27020` by default, retain `--ticks`, and emit a
  machine-readable readiness line after binding.
- Add native transport health and replication snapshot age/rate to separately labeled overlay
  sections. Do not display unavailable measurements as zero.
- Add `games/dots/tools/dots_session.py` to launch one server and a configurable number of
  clients, optionally pass one TOML configuration to every client, prefix output, propagate
  failures, and clean up all children.

## Tests and Exit Criteria

- Keep every existing in-memory test passing.
- Test native loopback lifecycle, reliable/unreliable delivery, multiple clients, statistics,
  impairment, malformed packets, handshake success/failure, and disconnect cleanup.
- Test network configuration, CLI precedence, snapshot age/rate, and launcher process cleanup.
- Use dynamically selected private loopback ports in automated tests and validate the full local
  preset. GameNetworkingSockets does not accept port zero directly, so the wrapper resolves
  `:0` by trying available ports from the dynamic/private range.
- Two real `dots_client` processes can connect to `dots_server`, see authoritative movement,
  and inspect truthful transport and replication health.
