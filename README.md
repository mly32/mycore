# MyCore

MyCore is a clean, minimal C++20 game-engine project developed through complete game
vertical slices rather than speculative general-purpose systems.

The first game is **Dots**, an Agar.io-like server-authoritative multiplayer game targeting
up to 1,000 connected clients. Dots drives the initial work on simulation, rendering,
transport, replication, prediction, observability, and load testing.

A later offline 3D aim trainer will be the first cross-game validation. It will reuse the
engine's platform, math, rendering, asset, and debugging libraries while keeping its camera,
targets, scoring, and presentation game-owned.

The engine is intended to remain a set of small libraries with explicit dependencies. Game
code lives under `games/<game>`, and game executables belong to their game. Stable CMake
targets and public include boundaries will support source-tree consumers first, followed by
an installable CMake package after the reusable modules stabilize.

## Getting Started

### Prerequisites

- CMake 3.25 or newer
- Ninja
- A C++20 compiler: AppleClang/Clang, GCC, or MSVC
- vcpkg with the `VCPKG_ROOT` environment variable set to its installation directory
- pkg-config when building dependencies on macOS or Linux
- A Metal-, Vulkan-, or D3D12-capable GPU driver to run the graphical client

Confirm the tools are available:

```bash
cmake --version
ninja --version
clang++ --version
vcpkg version
```

On macOS, CMake and Ninja can be installed with Homebrew, while AppleClang is provided by
the Xcode Command Line Tools:

```bash
brew install cmake ninja vcpkg pkg-config
xcode-select --install
```

On Debian and Ubuntu derivatives, install the compiler/build tools, the Autotools programs
used by some vcpkg ports, and SDL's Linux window/input backend development packages:

```bash
sudo apt-get update
sudo apt-get install --yes \
    build-essential clang cmake ninja-build pkg-config \
    autoconf autoconf-archive automake libtool libltdl-dev \
    libx11-dev libxft-dev libxext-dev \
    libxcursor-dev libxfixes-dev libxi-dev libxrandr-dev \
    libxss-dev libxtst-dev \
    libwayland-dev libxkbcommon-dev libegl1-mesa-dev \
    libibus-1.0-dev
```

vcpkg manages the project libraries, but Unix ports may still use host build tools and
system-specific development packages supplied by the operating-system package manager.

If vcpkg was installed another way, set `VCPKG_ROOT` to the directory containing its
`scripts/` folder before configuring. CMake uses vcpkg manifest mode to install the
dependencies declared in `vcpkg.json` automatically.

### Configure, Build, and Test

List the presets available for the current operating system:

```bash
cmake --list-presets
```

On macOS, configure and build the debug preset, then run all tests:

```bash
cmake --preset macos-clang-debug
cmake --build --preset macos-clang-debug
ctest --preset macos-clang-debug
```

Linux and Windows use the corresponding host presets from `CMakePresets.json`, such as
`linux-clang-debug` or `windows-msvc-debug`.

Build one target instead of the entire project:

```bash
cmake --build --preset macos-clang-debug --target dots_client
```

Use a fresh CMake cache when changing compilers or troubleshooting configuration:

```bash
cmake --fresh --preset macos-clang-debug
cmake --build --preset macos-clang-debug
```

### Run Dots

All runnable targets are placed under `build/<preset>/bin`. `dots_client` runs a playable
offline SDL_GPU client by default, can embed the authoritative server through the in-memory
transport, or can connect to a separate native server. `dots_server` listens for native clients
while running the headless authoritative heartbeat; the bot remains a foundation executable for
now:

| Target | Executable path for `macos-clang-debug` |
|---|---|
| `dots_client` | `build/macos-clang-debug/bin/dots_client` |
| `dots_server` | `build/macos-clang-debug/bin/dots_server` |
| `dots_bot` | `build/macos-clang-debug/bin/dots_bot` |

```bash
./build/macos-clang-debug/bin/dots_client
./build/macos-clang-debug/bin/dots_server
./build/macos-clang-debug/bin/dots_bot
```

Run the client against an embedded authoritative server without opening sockets:

```bash
./build/macos-clang-debug/bin/dots_client --in-memory
```

Run separate server and client processes over GameNetworkingSockets:

```bash
./build/macos-clang-debug/bin/dots_server --listen 127.0.0.1:27020
./build/macos-clang-debug/bin/dots_client --connect 127.0.0.1:27020
```

For a local multi-client session, use the developer launcher. It waits for the server readiness
line, prefixes child output, and cleans up every process on exit:

```bash
python3 games/dots/tools/dots_session.py \
    --build-dir build/macos-clang-debug \
    --clients 2 \
    --client-config games/dots/config/dots-client.toml
```

`--client-config` resolves the TOML path once and passes it to every graphical client. The
launcher's `--connect` argument still selects native mode and overrides `[network].mode` and
`[network].server_address` from that file for the session.

Add `--fake-lag-ms 50` or `--fake-loss-percent 5` to a native executable or the launcher to
impair outgoing packets. Lag is one-way, so applying 50 ms at both endpoints produces roughly
100 ms of transport RTT.

Networked clients send sequenced input at 30 Hz and render full replicated snapshots at 15 Hz.
Only the authoritative server—embedded or separate—advances the simulation. The client
deliberately has no prediction or remote interpolation yet, so movement is less smooth than
offline mode. Those behaviors arrive in later networking features. The transport remains
unaware of Dots messages.

Closing a networked client normally, including Escape or the window close control, requests a
graceful transport disconnect. The server removes that client's authoritative player and both
sides log the connection and session lifecycle under `dots.client.session` and
`dots.server.session`. A crashed or forcibly killed process cannot send the graceful request;
the peer instead reports a transport failure when it detects the loss.

The standalone server runs until interrupted. Use a bounded run for smoke testing:

```bash
./build/macos-clang-debug/bin/dots_server --ticks 10
```

The client uses SDL_GPU through the game-neutral `MyCore::Render` layer and the engine-owned
`MyCore::Render2D` grid/circle renderer. Dots only extracts game state and maps food and players
to generic draw data. The GPU backend is Metal on macOS, Vulkan on Linux, and D3D12 on Windows.
The build compiles Render2D's canonical HLSL shaders to MSL, SPIR-V, or DXIL using
vcpkg-managed host tools, then stages them under `bin/assets/mycore/render_2d/` beside the
executable. No shader compiler is loaded at runtime.

Hybrid input is the default: WASD or the arrow keys take precedence while held, and otherwise
the player moves toward the mouse cursor. Mouse input stops while the cursor is inside the
player circle. Press Escape or close the window to exit. The window is resizable and uses
swapchain pixel dimensions so high-DPI display and mouse coordinates stay aligned. Asset lookup
is relative to the executable, so the client can be launched from a different working directory.
As the player consumes food, its color shifts from `colors.player` toward
`colors.player_growth` to make growth easier to see.

A non-interactive Dear ImGui overlay in the bottom-right reports the active input mode, world
tick, player and food counts, occupied spatial-grid cells or replicated snapshot ID, frame timing,
actual and target tick rates, simulation cost, backlog, catch-up frames, step-cap hits, deadline
misses, and discarded time. Networked modes separately report replication snapshot age/rate and
transport state, RTT, loss, traffic rates, and queues. Measurements unavailable from the
in-memory backend are labeled instead of displayed as zero. Logs use owner-qualified categories
such as `dots.client`, `dots.server`, and `dots.bot`.
Fixed-step overload produces rate-limited warnings, escalates to an error log after ten sustained
seconds, and reports recovery. The offline client records and discards only whole excess backlog
after its configured catch-up cap so it remains responsive while preserving the fractional time
used for interpolation. The local player and its following camera use that same interpolated
position, preventing simulation-tick jitter. The client also includes on-demand Tracy zones for
its frame, simulation, presentation, and render-submission work; without a Tracy profiler
connected, those hooks remain dormant.

Run the checked-in complete configuration explicitly:

```bash
./build/macos-clang-debug/bin/dots_client \
    --config games/dots/config/dots-client.toml
```

Configuration precedence is built-in defaults, then `dots-client.toml` in the current
working directory when it exists. `--config <path>` replaces that automatic path. A missing
automatic file is allowed; an explicitly requested missing file, invalid TOML, unknown field,
or invalid value is a startup error. The sample documents settings for the window, network mode
and address, input mode, bindings, fixed-step catch-up, camera scale/grid, and debug colors. The
built-in and sample network mode is `offline`; select `in_memory` or `native` in `[network]`, or
override it with `--offline`, `--in-memory`, or `--connect`. Binding names are
case-insensitive and support letters, digits, arrows, Escape, Space, Enter, Tab, Backspace,
left/right modifiers, navigation keys, and F1 through F12.

The sample's `#:schema` header associates it with the checked-in
`games/dots/config/dots-client.schema.json`. With the recommended Even Better TOML VS Code
extension installed, the schema provides setting completion, hover descriptions, defaults, and
early validation of types, ranges, names, and colors. The C++ loader remains authoritative for
runtime-only checks such as conflicting key bindings. Keep the schema beside copied sample files,
or update the header's relative path.

The `[debug].presentation_mode` setting controls how the followed player is displayed:

- `interpolated` is the normal smooth view.
- `fixed` displays the latest completed simulation tick without interpolation.
- `comparison` keeps the smooth view and draws a transparent, white outlined ghost at the
  latest fixed-tick position.

These modes change presentation only; simulation, collision, and authoritative state always use
the same fixed-tick world data.

For an initialization-only run that creates a hidden window, polls input once, and does not
create a GPU device or enter the game loop:

```bash
SDL_VIDEODRIVER=dummy \
    ./build/macos-clang-debug/bin/dots_client --headless-smoke
```

To produce and verify a relocatable client archive containing the executable, platform shader
assets, an example configuration, and its editor schema:

```bash
cmake --build --preset macos-clang-debug --target dots_client_package
```

Packages and SHA-256 checksum files are written under
`build/<preset>/packages/`. Windows packages are ZIP files, while macOS and Linux packages are
`.tar.gz` archives. The macOS archive contains `Dots.app`; its shaders live under
`Contents/Resources/assets/`, where `SDL_GetBasePath()` resolves them. The package target
extracts its own archive and runs `--package-smoke`, which creates a dummy-driver window and
reads all four shaders without creating a GPU device. CI uploads one verified archive per
platform for short-lived testing.

Use `dots_client --help` for the complete CLI surface.

For a concise introduction to shaders, GPU resources, instanced drawing, and how one Dots
frame reaches the screen, see the
[SDL_GPU rendering and shaders guide](docs/sdl_gpu_rendering_guide.md).

For the corresponding networking model, including protocol versus transport, server authority,
the current uncompensated networked flow, and the later prediction/reconciliation/interpolation
model, see the
[protocol, transport, and server-authoritative networking guide](docs/server_authoritative_networking_guide.md).

### Visual Studio Code

Open the repository root in VS Code and install the recommended workspace extensions. The
first time the workspace opens, use the Command Palette to run:

1. `CMake: Select Configure Preset` and choose `macOS Clang Debug`.
2. `CMake: Configure`.
3. `CMake: Select Build Preset` and choose `macos-clang-debug`.
4. `CMake: Build`.

Select `dots_client`, `dots_server`, or `dots_bot` as the CMake launch target before using
Run or Debug. The shared configuration is under `.vscode/`; developers can put local CMake
preset overrides in the ignored `CMakeUserPresets.json`.

## Plans

- [Architecture and technology plan](docs/game_engine_technology_plan.md)
- [Incremental feature branch plan](docs/development_branch_plan.md)
- [Multi-game architecture revamp](docs/plans/multi_game_architecture_revamp.md)
- [Feature 05 SDL client plan](docs/plans/05-sdl-client-window-input.md)
- [Feature 06 SDL_GPU render plan](docs/plans/06-sdl-gpu-render-baseline.md)
- [Feature 07 debug observability plan](docs/plans/07-debug-observability.md)
- [Feature 08 protocol binary codec plan](docs/plans/08-protocol-binary-codec.md)
- [Feature 09 in-memory transport integration plan](docs/plans/09-inmemory-transport-integration.md)
- [Feature 10 GameNetworkingSockets transport plan](docs/plans/10-gamenetworkingsockets-transport.md)
