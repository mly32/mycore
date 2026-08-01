# C++ Style Guide

This project uses a small, explicit C++ style optimized for readable engine and network code. The formatter is authoritative for whitespace and line wrapping; this guide covers the choices `clang-format` cannot express.

## Reference Guides

Use these as background references, not as strict rulebooks:

- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
- [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html)
- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)

Recommended baseline: LLVM-like formatting, C++ Core Guidelines safety bias, and Google-style caution around global state, implicit ownership, and macro use.

## Formatting

- Run `clang-format` using the repository `.clang-format`; do not hand-format around it.
- Use 4 spaces, no tabs, 100-column soft limit.
- Use attached braces:

```cpp
if (is_connected) {
    send_snapshot(client);
}
```

- Always use braces for `if`, `for`, `while`, and `switch` bodies, even for one-line bodies.
- Prefer one declaration per line when initializers are meaningful.
- Let `clang-format` sort includes, but keep the associated header first in `.cpp` files.

## Files and Names

- File names use `snake_case`: `snapshot_encoder.hpp`, `world_simulation.cpp`.
- Public C++ headers use `.hpp`; source files use `.cpp`.
- Namespaces use lower-case names: `mycore`, `mycore::sim`, `mycore::net`.
- Types use `PascalCase`: `World`, `EntityId`, `SnapshotHeader`.
- Functions and methods use `snake_case`: `step_world`, `encode_snapshot`.
- Local variables and parameters use `snake_case`.
- Private data members use a trailing underscore: `server_tick_`.
- Constants use `kPascalCase`: `kMaxClients`, `kSnapshotRateHz`.
- Enum classes use `PascalCase`; enumerators use `PascalCase`:

```cpp
enum class ConnectionState {
    Connecting,
    Connected,
    Disconnected,
};
```

- Macros use `MYCORE_UPPER_SNAKE_CASE`, and should be rare.

## Headers and Includes

- Use `#pragma once`.
- Headers include what they use; do not rely on transitive includes.
- Prefer forward declarations in headers when they materially reduce dependencies.
- Avoid platform, SDL, rendering, and networking transport includes in simulation headers.
- Keep include order in `.cpp` files:

```cpp
#include "world.hpp"

#include "dots/simulation/tick.hpp"

#include <algorithm>
#include <span>
```

## Ownership and Lifetime

- Prefer values, stable IDs, handles, `std::unique_ptr`, and non-owning views.
- Avoid `std::shared_ptr` in simulation and replication code.
- Use `std::span` for non-owning contiguous ranges.
- Do not store raw owning pointers.
- Keep entity lifetime explicit through IDs and generation checks.
- Avoid hidden lifecycle callbacks in hot simulation paths.

## Error Handling

- Use exceptions for startup, tooling, and unrecoverable configuration failures.
- Use explicit result values at runtime subsystem boundaries such as protocol decoding, asset loading, network message handling, and script calls.
- Decoders must treat input as hostile and validate lengths, ranges, versions, and enum values before use.
- Do not serialize C++ structs with `memcpy`; encode wire fields explicitly.

## Engine Boundaries

- Engine public headers use the `mycore/` include root and must not include game headers.
- Game public headers use their game include root, such as `dots/`.
- `MyCore::Core` must remain platform-free and game-neutral.
- `MyCore::Math` owns only requirement-driven math types and operations, not game policy.
- `MyCore::Time` owns policy-free tick, duration, and fixed-step accumulation helpers.
- `MyCore::Assets` owns game-neutral asset lookup and byte loading, not game formats.
- `MyCore::Debug` owns game-neutral logging, metrics primitives, and profiler hooks, not
  game-specific debug panels.
- `Dots::Simulation` depends only on Core, Math, and Time.
- `Dots::Protocol` owns Dots wire formats and concrete protocol IDs.
- `Dots::Replication` converts Dots simulation state into client-specific snapshots.
- `MyCore::NetTransport` owns connection mechanics and byte payload transport, not gameplay
  messages or replication.
- `MyCore::Render` owns game-neutral GPU resources and submission, not simulation state or
  game-specific pipelines.
- `Dots::Presentation` owns Dots render extraction, shaders, and pipelines.
- `dots_server` must stay headless and must not link renderer, SDL video, Dots presentation,
  or ImGui rendering.
- Each game owns its executable composition roots under `games/<game>/apps`.

## C++ Usage

- Target C++20.
- Do not use C++ modules in the first implementation.
- Keep exceptions and RTTI enabled.
- Prefer standard containers until profiling proves a replacement is needed.
- Use `fmt` for formatting instead of relying on uneven standard-library formatting support.
- Avoid global mutable state. Prefer explicit context objects passed to subsystems.
- Avoid macros for constants and small utilities; use `constexpr`, templates, or functions.

## Tests

- Add tests in the same branch as the behavior they cover.
- Unit tests should exercise real core/simulation/protocol objects.
- Protocol tests need round-trip, golden packet, malformed input, and fuzz coverage over time.
- Replay tests should verify fixed input sequences produce expected quantized state.
- Load and benchmark tests should report metrics, not just pass/fail.
