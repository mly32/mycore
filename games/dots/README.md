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

Permanent spectators can join without spawning a player or producing gameplay input. Use
`--spectate` on either networked executable, or give the launcher independent spectator counts:

```bash
python3 games/dots/tools/dots_session.py \
    --build-dir build/macos-clang-debug \
    --clients 1 \
    --spectator-bots 15 \
    --duration-seconds 30
```

The launcher waits for server readiness, prefixes child output, propagates failures, and cleans
up every process. Once every graphical client exits, the launcher terminates the remaining bots
and server; a client or bot failure ends the session immediately. Add `--fake-lag-ms` or
`--fake-loss-percent` to a native executable or the launcher to exercise impaired connections.
The lag value is outgoing one-way delay per process: the launcher passes it to both sides, so
`--fake-lag-ms 50` produces approximately 100 ms transport RTT before server-tick, snapshot, and
rendering phase. The value is not itself RTT.
For a bounded headless stability soak, set `--clients 0`, provide at least one bot, and use
`--duration-seconds`; the deadline is successful only if every process remains healthy:

```bash
python3 games/dots/tools/dots_session.py \
    --build-dir build/macos-clang-debug \
    --clients 0 \
    --bots 5 \
    --fake-lag-ms 100 \
    --fake-loss-percent 5 \
    --duration-seconds 30
```

Run an executable with `--help` for its complete CLI. The
[Feature 14 workload results](../../docs/feature14_rollback_workload_results.md) record the
current optimized replay matrix and native soak evidence.

| Executable | Purpose |
|---|---|
| `dots_client` | SDL_GPU client with offline, embedded-authority, and native modes |
| `dots_server` | Headless authoritative 30 Hz native server |
| `dots_bot` | Headless native client that repeatedly moves in a wide rectangle |

The bot runs the same rollback client runtime as the graphical client. It drains and discards
post-commit prediction event batches every poll because a headless composition has no
presentation consequences to play.

Networked clients use protocol v5 and receive authoritative snapshots at 15 Hz. Playing clients
normally submit input at 30 Hz; direct and defeated spectators instead send a 5 Hz status
heartbeat. Each input packet can repeat up to two unacknowledged samples by default. Snapshots
grant a 32-sequence authority receive window. A client retains locally accepted commands that
cannot yet be transmitted, pauses command production at eight unsent commands, and resumes at
two, so an overloaded authority does not turn an honest client into a 64-entry queue-overflow
disconnect. The controlled
player responds from a complete interaction-closed rollback World immediately, reconciles
against verified checkpoints and server ACKs, and smooths only visible primary-position
corrections over 100 ms. Movement, food, absorption, split, launch, cohesion, and merge are
predicted inside that island. While Playing, remote players outside it default to advancing
newest-authority movement and launch for at most six ticks/200 ms and then hold. Spectating
defaults to six-tick authoritative interpolation and uses bounded movement/launch extrapolation
only for the uncovered tail after that buffer underruns. Delayed mode uses the same interpolation
but holds immediately on underrun. Neither spectator mode runs rollback or speculative gameplay
mechanics. The
[networking guide](../../docs/server_authoritative_networking_guide.md) covers authority,
reliability, connection lifecycle, impairment, prediction, and compensation model.
The [networked prediction and time reference](../../docs/networked_prediction_reference.md)
defines the distinct authoritative, predicted, remote-presentation, and screen-presentation
states used by that model.

The server removes a transport connection that does not complete its Dots handshake within ten
seconds, and removes a ready client that supplies neither valid input nor a status heartbeat for
three seconds. These
are liveness fallbacks for lost connections; a normal disconnect removes the player immediately.
Native graphical clients drain a normal close for 50 ms plus configured fake outgoing lag (capped
at two seconds), allowing the close notification to leave before process teardown.
When input samples are briefly missing, the server holds the last movement for five ticks and then
stops that player while keeping the session alive.

Temporarily losing the graphical drawable surface, such as while minimized, pauses rendering but
not transport polling or fixed input production. Mouse steering and spectator wheel zoom become
neutral until the viewport returns; keyboard input remains available.

The authoritative server assigns each joining or respawning player through a deterministic,
active-player-count-indexed square-ring search on a 12-world-unit lattice. Every candidate is
checked against current live player circles; food does not block placement. Spawn position and
owner are included in the welcome-following full snapshot, and clients never choose them locally.
Player absorption and Playing/Spectating lifecycle are server-owned and repeated in snapshots.
The graphical spectator camera defaults to the confirmed killer's selected, composed
presentation sample. If that target disappears, it keeps the last valid camera position and
switches to free-camera mode.

The default respawn cooldown is 90 server ticks. Override it when starting the server, including
zero for immediate eligibility:

```bash
./build/macos-clang-debug/bin/dots_server \
    --listen 127.0.0.1:27020 \
    --respawn-cooldown-ticks 90
```

## Controls and configuration

The default hybrid mode uses WASD or the arrow keys while held and otherwise moves toward the
mouse cursor. Space requests a split while playing. While spectating, WASD or the arrows pan the
free camera, the mouse wheel or PageUp/PageDown zooms in 10 percent steps, `F` toggles
confirmed-killer follow, and `R` or Enter requests an authoritative respawn. Split and respawn
are edge-triggered: holding a key sends one request, not one request per input tick. Press Escape
to quit. Mouse steering and spectator wheel zoom pause while the debug panel owns the mouse.

For deterministic prediction investigation, set `[input].mode = "keyboard"` and
`[debug].prediction_log_level = "debug"` in a client config. The `info` level logs prediction
scope changes and command-frontier defer/recovery transitions; `debug` also reports nonzero
remote-player displacement across reconciliation.
`[debug].correction_history_count` controls how many recent local and comparable common-tick
remote corrections are drawn; it defaults to 8 and accepts 1 through 64. Correction circles
retain their magenta state-layer color and fade with age.
The [Feature 14 prediction-stutter postmortem](../../docs/feature14_prediction_stutter_postmortem.md)
explains the two-bot reproduction and how to interpret rollback corrections.

Split, launch, cohesion, and merge are implemented in the shared deterministic simulation and
rollback adapter. Protocol v5 carries the join role, status heartbeat, input receive grant,
split edge, complete checkpoints, immutable rules,
prediction identities, digests, and authority receipts, and the server executes a submitted
split. The graphical input path maps the configurable `split` binding to Space by default and
predicts the resulting topology immediately. The mechanic's immutable match rules are
simulation-owned rather than client presentation settings.

[`config/dots-client.toml`](config/dots-client.toml) documents window, network, input, simulation,
view, spectator, debug, and color settings. Its `#:schema` header connects the checked-in JSON
schema for editor completion and early validation. `[spectator].presentation_mode` is `live` by
default and accepts `live` or `delayed`. Both interpolate the six-tick authority buffer; live
extrapolates only a bounded underrun tail, while delayed holds at the newest endpoint. Spectator
pan speed defaults to 12 world units per second; zoom is clamped to the configured 5--80
pixels-per-world-unit range.

`[debug].remote_presentation_mode` selects `extrapolated` (default), `interpolated`, or
`comparison` for outside-closure players while Playing. Comparison draws the extrapolated
presentation plus the delayed interpolated position. The setting never changes simulation,
rollback membership, or spectator presentation; spectators use their separate
`[spectator].presentation_mode` setting.

Configuration precedence is built-in defaults, then `dots-client.toml` in the working directory
when present. `--config <path>` replaces that automatic path. CLI mode flags such as `--offline`,
`--in-memory`, and `--connect` override the configured network mode.

Set `[network].input_redundancy = false` to send only the current input sample. This is useful for
comparing recovery behavior under simulated loss; redundancy is enabled by default.

Set `[debug].enabled = false` to hide the in-game observability panel, suppress world-space
diagnostic layers, and prevent the panel from receiving input. It defaults to `true`; this does
not change simulation or gameplay presentation. Confirmed kill/defeat banners remain visible
because they are non-debug gameplay UI and never capture input.

```bash
./build/macos-clang-debug/bin/dots_client \
    --config games/dots/config/dots-client.toml
```

## Runtime visibility

When `[debug].enabled` is true, the in-game debug UI uses two panes: left **Dots session** has
**Runtime**, **Network**, and **Gameplay** tabs; right **Dots diagnostics** has **Rollback**,
**Interpolation**, and **Tools** tabs. It reports
server-assigned client/entity IDs, simulation and frame health, transport statistics, input ACK
and history pressure, receive/transmit/unsent frontiers and production-pause state, replay
percentiles and typed differences, scope/digest/receipt state,
consequence-handler delivery, persistent presentation handoffs, extrapolation age/cap/holds,
authoritative/predicted/presentation state, and confirmed absorption, defeat, follow,
respawn-deadline, and respawn-result state. The Gameplay
countdown projects the latest server tick using snapshot receipt age for presentation only; the
server tick decides eligibility. The compact top-left game-state panel repeats an active respawn
countdown so it remains visible without selecting the Gameplay tab.
Tools can inject prediction errors or a three-packet drop burst without changing measured
transport loss. Both publish bounded `ARMED`, `TRIGGERED`, and `COMPLETED` fault receipts.
Native connections expose RTT, loss, rates, and queues; unavailable in-memory
measurements are labeled rather than displayed as zero. See the
[debugging guide](../../docs/debugging_and_observability.md) for every field and visual layer.

Logs use owner-qualified categories such as `dots.client.session` and `dots.server.session`, and
record newly confirmed client lifecycle state plus authoritative server defeat, follow-loss, and
respawn decisions. On-demand Tracy zones cover the main client frame stages and prediction
reconciliation.

The [rendering guide](../../docs/sdl_gpu_rendering_guide.md) explains how Dots presentation data
reaches SDL_GPU through MyCore's renderer.

## Code ownership

- `simulation/` owns deterministic rules and remains independent of SDL, rendering, transport,
  and presentation.
- `prediction/` adapts complete Dots Worlds to `MyCore::Rollback` and owns mechanic contracts,
  causal scope construction, profile fallback, checkpoint digests, and typed differences.
- `protocol/` owns Dots wire messages and binary encoding.
- `replication/`, `server/`, and `client_runtime/` own authoritative sessions and replicated
  client state.
- `app_cli/` owns shared command-line value, network-address, and impairment-option parsing for
  Dots executables; each executable retains its own option policy.
- `presentation/` maps Dots state into game-neutral rendering data.
- `apps/` contains the executable composition roots.
- `config/` contains the sample client configuration and editor schema.

Reusable facilities live under [`engine/`](../../engine/); Dots-specific behavior remains here.
