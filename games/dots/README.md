# Dots

Dots is MyCore's first complete game vertical slice: an Agar.io-like game that drives concrete
requirements for deterministic simulation, rendering, input, networking, replication,
observability, and packaging.

The [Dots gameplay guide](../../docs/dots_gameplay.md) is the canonical description of current
rules, food, spawning, connection-loss behavior, presentation, and deferred mechanics.

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

To watch remote presentation without opening several windows, keep one graphical client and add
headless rectangle-moving bots:

```bash
python3 games/dots/tools/dots_session.py \
    --build-dir build/macos-clang-debug \
    --clients 1 \
    --bots 3
```

The launcher waits for server readiness, prefixes child output, propagates failures, and cleans
up every process. Once every graphical client exits, the launcher terminates the remaining bots
and server; a client or bot failure ends the session immediately. Add `--fake-lag-ms` or
`--fake-loss-percent` to a native executable or the launcher to exercise impaired connections.
Run an executable with `--help` for its complete CLI.

| Executable | Purpose |
|---|---|
| `dots_client` | SDL_GPU client with offline, embedded-authority, and native modes |
| `dots_server` | Headless authoritative 30 Hz native server |
| `dots_bot` | Headless native client that repeatedly moves in a wide rectangle |

Networked clients send protocol-v2 input packets at 30 Hz and receive authoritative snapshots at
15 Hz. Each input packet can repeat up to two unacknowledged samples by default. The controlled
player responds from bounded movement prediction immediately, reconciles against server ACKs,
and smooths only visible corrections over 100 ms. Remote players use six-tick delayed
interpolation from accepted snapshot history and hold the last known remote sample during an
underrun. The
[networking guide](../../docs/server_authoritative_networking_guide.md) covers authority,
reliability, connection lifecycle, impairment, prediction, and compensation model.
The [networked prediction and time reference](../../docs/networked_prediction_reference.md)
defines the distinct authoritative, predicted, remote-presentation, and screen-presentation
states used by that model.

The server removes a transport connection that does not complete its Dots handshake within ten
seconds, and removes a ready client that supplies no valid input packet for three seconds. These
are liveness fallbacks for lost connections; a normal disconnect removes the player immediately.
Native graphical clients drain a normal close for 50 ms plus configured fake outgoing lag (capped
at two seconds), allowing the close notification to leave before process teardown.
When input samples are briefly missing, the server holds the last movement for five ticks and then
stops that player while keeping the session alive.

Temporarily losing the graphical drawable surface, such as while minimized, pauses rendering but
not transport polling or fixed input production. Mouse steering becomes neutral until the
viewport returns; keyboard input remains available.

The authoritative server assigns each joining player a distinct, deterministically shuffled
spawn slot near the food field. Spawn position is included in the welcome-following full snapshot;
clients never choose it locally.

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

Set `[debug].enabled = false` to hide the in-game observability panel, suppress world-space
diagnostic layers, and prevent the panel from receiving input. It defaults to `true`; this does
not change simulation or gameplay presentation.

```bash
./build/macos-clang-debug/bin/dots_client \
    --config games/dots/config/dots-client.toml
```

## Runtime visibility

When `[debug].enabled` is true, the in-game debug UI uses two panes: left **Dots session** has
**Runtime** and **Network** tabs; right **Dots diagnostics** has **Prediction**,
**Interpolation**, and **Tools** tabs. It reports
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
- `app_cli/` owns shared command-line value, network-address, and impairment-option parsing for
  Dots executables; each executable retains its own option policy.
- `presentation/` maps Dots state into game-neutral rendering data.
- `apps/` contains the executable composition roots.
- `config/` contains the sample client configuration and editor schema.

Reusable facilities live under [`engine/`](../../engine/); Dots-specific behavior remains here.
