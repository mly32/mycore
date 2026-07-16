# Feature 00: Foundation Implementation Plan

## Goal

Create an empty but buildable C++20 multi-game monorepo foundation on
`feature/00-foundation`. The project must configure and build with CMake and Ninja, and its
dependency-free smoke test must pass through CTest. The initial game is named Dots, but its
code and executables must not define the ownership boundaries of reusable engine modules.

## Existing Work to Preserve

- Keep the existing `.clang-format` unchanged unless validation finds it incompatible with
  the project toolchain.
- Preserve the existing C++ style decisions; keep its architecture examples aligned with
  the approved multi-game boundaries.
- Follow the approved multi-game architecture and the revised roadmap.

## Planned Changes

### 1. Repository and build configuration

- Add the required monorepo directories: `engine/`, `games/dots/`, `tests/`, `benchmarks/`,
  `fuzz/`, `tools/`, and `cmake/`.
- Add the root `CMakeLists.txt` and small, focused CMake modules for project options and
  compiler warnings.
- Define interface targets that propagate C++20 and warning settings to project-owned
  targets without relying on directory-wide compiler flags.
- Define a target-based executable helper that places runtime outputs in
  `build/<preset>/bin` without relocating libraries.
- Add `CMakePresets.json` with Ninja configure, build, and test presets filtered for the
  supported host platforms. Include a working macOS debug preset for the current host.
- Add `.gitignore` for generated build output, local CMake presets, IDE files, and common
  platform artifacts.
- Add a conservative `.clang-tidy` baseline that catches useful correctness issues without
  requiring third-party dependencies.

### 2. Placeholder targets

- Add the `mycore_core` static library under `engine/core`, with public alias
  `MyCore::Core`, public headers rooted at `mycore/core/`, and no platform-specific or
  game-specific dependencies.
- Add the `dots_simulation` static library under `games/dots/simulation`, with public alias
  `Dots::Simulation`, public headers rooted at `dots/simulation/`, and a public dependency
  on `MyCore::Core`.
- Add placeholder `dots_client`, `dots_server`, and `dots_bot` executables under
  `games/dots/apps/`, linked to `Dots::Simulation`.
- Keep all placeholder code platform-free and free of external dependencies.

### 3. Foundation smoke test

- Add a small standalone test executable under `tests/` without introducing Catch2 early.
- Register it with CTest.
- Exercise a minimal symbol from `MyCore::Core` and `Dots::Simulation` so the test also
  verifies public include roots, linkage, and dependency direction.

### 4. Validation and review

- Configure with the current host's debug preset.
- Build all targets through the matching build preset.
- Run CTest through the matching test preset and require a clean pass.
- Inspect the diff and Git status to ensure no generated artifacts or unrelated changes are
  included.
- Report the resulting targets, validation commands, and any remaining limitations. Do not
  commit or merge unless explicitly requested.

## Expected File Layout

```text
.
|-- CMakeLists.txt
|-- CMakePresets.json
|-- benchmarks/
|-- cmake/
|-- docs/
|   `-- plans/
|-- engine/
|   `-- core/
|-- fuzz/
|-- games/
|   `-- dots/
|       |-- apps/
|       |   |-- bot/
|       |   |-- client/
|       |   `-- server/
|       `-- simulation/
|-- tests/
`-- tools/
```

## Exit Criteria

- `MyCore::Core` and `Dots::Simulation` resolve to buildable libraries.
- `dots_client`, `dots_server`, and `dots_bot` build as executables.
- Every executable, including test programs, is emitted under the preset build directory's
  `bin` directory.
- CMake configuration and compilation require no external game dependencies.
- The registered foundation smoke test passes through CTest.
- Generated build output remains ignored by Git.
- No Dots code or dependency is introduced into `MyCore::Core`.
