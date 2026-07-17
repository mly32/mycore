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
- pkg-config when building dependencies on macOS

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

All runnable targets are placed under `build/<preset>/bin`. The foundation executables
currently print their startup status and exit:

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
