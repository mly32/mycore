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
offline SDL_GPU client; the server and bot remain foundation executables for now:

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

The client uses SDL_GPU through the game-neutral `MyCore::Render` layer and the engine-owned
`MyCore::Render2D` grid/circle renderer. Dots only extracts game state and maps food and players
to generic draw data. The GPU backend is Metal on macOS, Vulkan on Linux, and D3D12 on Windows.
The build compiles Render2D's canonical HLSL shaders to MSL, SPIR-V, or DXIL using
vcpkg-managed host tools, then stages them under `bin/assets/mycore/render_2d/` beside the
executable. No shader compiler is loaded at runtime.

Hybrid input is the default: WASD or the arrow keys take precedence while held, and otherwise
the player moves toward the mouse cursor. Mouse input stops while the cursor is inside the
player circle, and the window title shows the active input mode. Press Escape or close the
window to exit. The window is resizable and uses swapchain pixel dimensions so high-DPI display
and mouse coordinates stay aligned. Asset lookup is relative to the executable, so the client
can be launched from a different working directory. As the player consumes food, its color
shifts from `colors.player` toward `colors.player_growth` to make growth easier to see.

Run the checked-in complete configuration explicitly:

```bash
./build/macos-clang-debug/bin/dots_client \
    --config games/dots/config/dots-client.toml
```

Configuration precedence is built-in defaults, then `dots-client.toml` in the current
working directory when it exists. `--config <path>` replaces that automatic path. A missing
automatic file is allowed; an explicitly requested missing file, invalid TOML, unknown field,
or invalid value is a startup error. The sample documents settings for the window, input mode,
bindings, fixed-step catch-up, camera scale/grid, and debug colors. Binding names are
case-insensitive and support letters, digits, arrows, Escape, Space, Enter, Tab, Backspace,
left/right modifiers, navigation keys, and F1 through F12.

For an initialization-only run that creates a hidden window, polls input once, and does not
create a GPU device or enter the game loop:

```bash
SDL_VIDEODRIVER=dummy \
    ./build/macos-clang-debug/bin/dots_client --headless-smoke
```

Use `dots_client --help` for the complete CLI surface.

For a concise introduction to shaders, GPU resources, instanced drawing, and how one Dots
frame reaches the screen, see the
[SDL_GPU rendering and shaders guide](docs/sdl_gpu_rendering_guide.md).

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
