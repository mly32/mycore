# Dots

Dots is MyCore's first complete game vertical slice: an Agar.io-like game that drives concrete
requirements for deterministic simulation, rendering, input, networking, replication,
observability, and packaging.

Commands below run from the repository root and use the macOS preset as an example. See the
[build guide](../../docs/building.md) for other platforms.

## Run modes

Build the client and start a local offline simulation:

```bash
cmake --preset macos-clang-debug
cmake --build --preset macos-clang-debug --target dots_client
./build/macos-clang-debug/bin/dots_client
```

Run an embedded authoritative server through the deterministic in-memory transport:

```bash
./build/macos-clang-debug/bin/dots_client --in-memory
```

Run a native server and connect a separate client through GameNetworkingSockets:

```bash
./build/macos-clang-debug/bin/dots_server --listen 127.0.0.1:27020
./build/macos-clang-debug/bin/dots_client --connect 127.0.0.1:27020
```

Launch one server and multiple clients for local testing:

```bash
python3 games/dots/tools/dots_session.py \
    --build-dir build/macos-clang-debug \
    --clients 2 \
    --client-config games/dots/config/dots-client.toml
```

The launcher waits for server readiness, prefixes child output, propagates failures, and cleans
up every process. Add `--fake-lag-ms` or `--fake-loss-percent` to a native executable or the
launcher to exercise impaired connections. Run an executable with `--help` for its complete CLI.

| Executable | Purpose |
|---|---|
| `dots_client` | SDL_GPU client with offline, embedded-authority, and native modes |
| `dots_server` | Headless authoritative 30 Hz native server |
| `dots_bot` | Foundation executable for future load-testing clients |

Networked clients send protocol-v2 input packets at 30 Hz and receive authoritative snapshots at
15 Hz. Each input packet can repeat up to two unacknowledged samples by default. The controlled
player responds from bounded movement prediction immediately, reconciles against server ACKs,
and smooths only visible corrections over 100 ms. Remote players still use their newest received
snapshot until Feature 12 interpolation. The
[networking guide](../../docs/server_authoritative_networking_guide.md) covers authority,
reliability, connection lifecycle, impairment, prediction, and the planned compensation model.

## Controls and configuration

The default hybrid mode uses WASD or the arrow keys while held and otherwise moves toward the
mouse cursor. Press Escape to quit. Mouse steering pauses while the debug panel owns the mouse.

[`config/dots-client.toml`](config/dots-client.toml) documents window, network, input, simulation,
view, debug, and color settings. Its `#:schema` header connects the checked-in JSON schema for
editor completion and early validation.

Configuration precedence is built-in defaults, then `dots-client.toml` in the working directory
when present. `--config <path>` replaces that automatic path. CLI mode flags such as `--offline`,
`--in-memory`, and `--connect` override the configured network mode.

Set `[network].input_redundancy = false` to send only the current input sample. This is useful for
comparing recovery behavior under simulated loss; redundancy is enabled by default.

```bash
./build/macos-clang-debug/bin/dots_client \
    --config games/dots/config/dots-client.toml
```

## Runtime visibility

The in-game debug panel separates Runtime, Network, Prediction, and Tools output. It reports
server-assigned client/entity IDs, simulation and frame health, transport statistics, input ACK
and history pressure, replay/correction metrics, and authoritative/predicted/presentation state.
Tools can inject prediction errors or a three-packet drop burst without changing measured
transport loss. Native connections expose RTT, loss, rates, and queues; unavailable in-memory
measurements are labeled rather than displayed as zero. See the
[debugging guide](../../docs/debugging_and_observability.md) for every field and visual layer.

Logs use owner-qualified categories such as `dots.client.session` and `dots.server.session`, and
on-demand Tracy zones cover the main client frame stages and prediction reconciliation.

The [rendering guide](../../docs/sdl_gpu_rendering_guide.md) explains how Dots presentation data
reaches SDL_GPU through MyCore's renderer.

## Code ownership

- `simulation/` owns deterministic rules and remains independent of SDL, rendering, transport,
  and presentation.
- `protocol/` owns Dots wire messages and binary encoding.
- `replication/`, `server/`, and `client_runtime/` own authoritative sessions and replicated
  client state.
- `presentation/` maps Dots state into game-neutral rendering data.
- `apps/` contains the executable composition roots.
- `config/` contains the sample client configuration and editor schema.

Reusable facilities live under [`engine/`](../../engine/); Dots-specific behavior remains here.
