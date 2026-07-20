# MyCore Engine + Dots Authoritative Multiplayer Game

> **Purpose:** Architecture and technology plan for a learning-focused, multi-game C++
> engine and its first vertical slice: Dots, an Agar.io-like server-authoritative game.
>
> This document is intended to serve as a source-of-truth brief for implementation with Codex. The project should prioritize research value, understandable systems, and controlled technology experiments over feature breadth.

---

## 1. Project Direction

Build **a networked game vertical slice with reusable engine libraries**, not a
general-purpose engine followed by a game. Dots is the first implementation driver. A
later offline 3D aim trainer will validate reuse without forcing the initial design to
predict every 3D requirement.

### Product and ownership model

- `engine/` contains game-neutral libraries that can eventually be consumed independently.
- `games/dots/` owns all Dots simulation, protocol, replication, presentation, rules,
  executables, tests, and assets.
- `games/aim_trainer/` is deferred until Dots establishes the reusable platform and render
  layers.
- A facility becomes an engine abstraction only when it has a clear game-neutral contract.
  Code can remain game-owned until a second use demonstrates the reusable boundary.
- Each game owns its executable composition roots. There is no global game client, server,
  or bot application.

Dots should drive every subsystem requirement until the aim-trainer validation milestone.

### Primary research targets

1. Authoritative simulation.
2. Input prediction and reconciliation.
3. Snapshot replication and interest management.
4. Scaling one simple world toward 1,000 clients.
5. Clean client/server/engine boundaries.
6. Controlled comparisons of rendering, ECS, scripting, and packaging approaches.
7. Reusing engine libraries in a second game with substantially different rendering and
   input requirements.

### Avoid at the beginning

Do not start with:

- A general-purpose editor.
- A plugin ABI.
- Generic physics.
- A custom allocator framework.
- A render graph.
- A job system.
- A custom reliable-UDP transport.
- A generic scene graph.
- A universal game framework shared by Dots and a hypothetical future game.
- Microservices or orchestration infrastructure.

These can become later experiments after a working reference implementation exists.

---

## 2. Baseline Technology Stack

| Area | Recommendation |
|---|---|
| Language | **C++20** |
| Build | **CMake + Ninja + CMake Presets** |
| Dependencies | **vcpkg manifest mode**, pinned baseline |
| Unit tests | **Catch2 v3 + CTest** |
| Benchmarks | **Google Benchmark** |
| Fuzzing | **libFuzzer** for packet and asset decoders |
| Platform layer | **SDL3** |
| Renderer | **SDL_GPU** behind a thin engine API |
| Shader language | **HLSL**, compiled offline through glslang/DXC/SPIRV-Cross initially |
| Networking transport | **GameNetworkingSockets** |
| Scripting | **Lua 5.5**, using a small direct C API wrapper |
| Debug UI | **Dear ImGui** |
| Logging | **spdlog + fmt** |
| Profiling | **Tracy**, plus GPU capture tools |
| Static analysis | **clang-tidy + clang-format** |
| Client platforms | Windows and Linux first; macOS compile-tested early |
| Server platform | Linux headless |
| World representation | Explicit structures-of-arrays and a uniform spatial hash |
| CI | GitHub Actions |
| Server deployment | A simple Linux container; no orchestration platform initially |

---

## 3. C++ Version and Compiler Policy

### Use C++20

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

C++20 provides useful features such as:

- `std::span`
- Concepts
- `std::jthread`
- `std::stop_token`
- Designated initializers
- `std::bit_cast`
- Better compile-time programming facilities

It is modern enough to be useful without forcing the project onto uneven C++23 or C++26 implementation support.

Reference:

- [MSVC C++ language conformance](https://learn.microsoft.com/en-us/cpp/overview/visual-cpp-language-conformance?view=msvc-170)

### Language and runtime policies

- Do not use C++ modules in the first version.
- Do not disable exceptions or RTTI merely because this is an engine.
- Use exceptions for startup and tooling failures.
- Use explicit result values at runtime subsystem boundaries.
- Avoid `std::shared_ptr` inside the simulation.
- Prefer values, `std::unique_ptr`, stable IDs, handles, and non-owning views.
- Use standard containers until profiling demonstrates a reason to replace them.
- Use `std::pmr` for a measured workload before designing a custom allocator hierarchy.
- Use `fmt` rather than making standard-library formatting support a portability concern.

### Compiler matrix

| Platform | Primary compiler | Secondary compiler |
|---|---|---|
| Windows | MSVC | clang-cl |
| Linux | Clang | GCC |
| macOS | AppleClang | — |

Use Clang for sanitizer and fuzzing configurations.

---

## 4. Build and Dependency Management

## 4.1 CMake, Ninja, and Presets

Use modern target-based CMake.

Every library should expose its:

- Dependencies
- Include paths
- Compile features
- Warnings
- Public and private compile definitions

through its CMake target rather than global CMake state.

Use stable public target names from the beginning:

```text
MyCore::Core
MyCore::Math
MyCore::Time
MyCore::PlatformPaths
MyCore::PlatformSDL
MyCore::Render
MyCore::Render2D
MyCore::Assets
MyCore::Debug
MyCore::DebugUI
MyCore::NetTransport
MyCore::Scripting

Dots::Simulation
Dots::Protocol
Dots::Replication
Dots::Presentation
```

Engine public headers use the `mycore/` include root. Dots public headers use the `dots/`
include root. Executable targets are game-qualified, such as `dots_client`, `dots_server`,
and `dots_bot`.

Place runnable outputs under `build/<preset>/bin` through a target property helper. Keep
libraries in their normal target-specific build directories because consumers link them by
target name rather than locate them manually.

Check in:

```text
CMakePresets.json
```

Ignore:

```text
CMakeUserPresets.json
```

This lets developers add machine-local settings without changing shared build configuration.

Reference:

- [CMake Presets documentation](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)

### Suggested initial presets

```text
windows-msvc-debug
windows-msvc-release
windows-clangcl-asan
linux-clang-debug
linux-clang-release
linux-clang-asan
linux-clang-tsan
linux-server-release
macos-clang-debug
macos-clang-release
```

Use Ninja in CI and for command-line development. IDEs can consume the CMake project and presets directly.

## 4.2 Dependency management: vcpkg manifest mode

Use `vcpkg` in manifest mode.

Check in:

```text
vcpkg.json
vcpkg-configuration.json
```

Pin a registry baseline so dependency resolution is reproducible.

Reference:

- [vcpkg manifest mode](https://learn.microsoft.com/en-us/vcpkg/consume/manifest-mode)
- [SDL3 package in vcpkg](https://vcpkg.io/en/package/sdl3.html)

### Likely initial dependencies

```text
sdl3
gamenetworkingsockets
lua
catch2
benchmark
spdlog
fmt
imgui
tracy
```

### Optional later additions

```text
sdl3-image
sdl3-ttf
sdl3-shadercross
```

MyCore owns a small requirement-driven math library rather than exposing a third-party math
type system through public interfaces. Start with the 2D operations Dots needs. Add 3D
vectors, matrices, quaternions, transforms, rays, and bounds only when the aim trainer
provides concrete requirements.

Use a vcpkg binary cache in CI.

Avoid mixing all of the following without a compelling reason:

- vcpkg
- CMake `FetchContent`
- Git submodules
- Manually copied third-party source
- System-installed libraries

### When Conan would be a better experiment

Conan 2 becomes attractive if a research goal is:

- Producing independently versioned binary engine packages.
- Testing multiple package profiles.
- Modeling a large internal dependency graph.
- Exploring package creation and editable packages.
- Cross-compiling packaged libraries.

Reference:

- [Conan 2 documentation](https://docs.conan.io/2/tutorial.html)

For this monorepo project, start with vcpkg.

## 4.3 External consumption

Support source-tree consumption first:

```cmake
add_subdirectory(external/mycore)
target_link_libraries(my_game PRIVATE MyCore::Core MyCore::Render)
```

Keep public headers and target names compatible with a future installed package, but do not
add engine-library export machinery before several engine modules stabilize. Runtime game
bundle install rules are independent. After Dots and the aim trainer validate the boundaries,
export reusable engine targets through
`MyCoreConfig.cmake` so an external project can use:

```cmake
find_package(MyCore CONFIG REQUIRED COMPONENTS Core Render)
target_link_libraries(my_game PRIVATE MyCore::Core MyCore::Render)
```

Game targets are not part of the MyCore engine package by default. Validate packaging with
a separate minimal consumer project that installs MyCore, calls `find_package`, and links
only the requested components.

## 4.4 Playable game distribution

Keep runtime game bundles separate from the future MyCore developer package. A game bundle
contains an executable, cooked platform assets, runtime shared libraries, and example user
configuration; the engine package contains headers, libraries, and CMake metadata for another
developer.

Feature 06 establishes a `DotsClient` install component and verified archive:

```text
Windows/Linux                 macOS
dots_client[.exe]             Dots.app/Contents/MacOS/dots_client
assets/...                    Dots.app/Contents/Resources/assets/...
dots-client.example.toml      Dots.app/Contents/Resources/dots-client.example.toml
```

CPack produces ZIP on Windows and `.tar.gz` on macOS/Linux. CI extracts the archive, verifies
the exact target-declared shader set, and runs a GPU-free package smoke path before retaining
the archive as a short-lived artifact. Do not package the entire incremental build output
directory: deleted source assets can remain there until a clean build.

Code signing, macOS notarization, installers, Linux compatibility baselines, and permanent
tagged release publication are later release-engineering concerns rather than renderer APIs.

## 4.5 Per-user configuration and writable paths

The packaged example configuration is a read-only reference, not the live settings file. A
desktop application launched from Finder, Explorer, or a Linux menu must not depend on its
current working directory to locate user settings.

Add an eventual game-neutral `MyCore::PlatformPaths` facility that takes a vendor/application
identity and returns native locations for configuration, persistent data, cache, and logs. Its
configuration convention should be:

```text
macOS    ~/Library/Application Support/<vendor>/<game>/
Windows  %LOCALAPPDATA%\\<vendor>\\<game>\\
Linux    $XDG_CONFIG_HOME/<vendor>/<game>/
         ~/.config/<vendor>/<game>/ when XDG_CONFIG_HOME is unset
```

Do not put TOML policy in that engine module. The engine discovers directories; Dots or another
game chooses `dots-client.toml`, supplies defaults, parses its schema, and reports field errors.
For Dots, preserve an explicit `--config` as the highest-priority source, retain the
current-directory file as a useful developer/portable override, then check the per-user file
before falling back to built-in defaults.

Path lookup is read-only. Saving settings, creating directories, migrating versions, and
deciding whether a setting roams or stays machine-local must be explicit application actions.
Keep configuration, saves/data, cache, and logs distinct so cleanup or cloud synchronization
does not accidentally treat them alike. Test path rules with injected platform/environment
values rather than the test runner's real home directory.

---

## 5. Testing Strategy

## 5.1 Main test framework

Use **Catch2 v3** and register tests through **CTest**.

Reference:

- [Catch2 CMake integration](https://github.com/catchorg/Catch2/blob/devel/docs/cmake-integration.md)

Use **Google Benchmark** for stable microbenchmarks.

### Initial benchmark targets

- Spatial-grid insertion.
- Spatial-grid removal and movement.
- Spatial queries.
- Circle collision broad phase.
- Circle collision narrow phase.
- World stepping.
- Snapshot construction.
- Delta encoding.
- Delta decoding.
- Per-client interest calculations.
- Allocation counts.
- Serialization throughput.

## 5.2 Test categories

| Category | Examples |
|---|---|
| Unit | Vector math, handle generations, bit streams, grid queries |
| Simulation scenario | Two cells collide, split, eat food, reconnect |
| Protocol round-trip | Encode, decode, compare semantic values |
| Golden packet | Known input produces a byte-for-byte expected packet |
| Property tests | Invalid IDs rejected, values remain in legal ranges |
| Fuzz tests | Arbitrary bytes passed to every network decoder |
| Integration | Real client and server transport over loopback |
| Impairment | Latency, jitter, loss, duplication, reordering |
| Replay regression | Recorded commands reproduce expected quantized state |
| Load | 10, 100, 500, and 1,000 bot connections |
| Soak | Long-running server with joins, leaves, and packet loss |

Use libFuzzer with AddressSanitizer and UndefinedBehaviorSanitizer for packet decoders and asset readers.

Reference:

- [Clang AddressSanitizer documentation](https://clang.llvm.org/docs/AddressSanitizer.html)

Avoid making the tests mock-heavy.

The most valuable tests should exercise:

- A real `World`.
- Real serializers.
- Real protocol messages.
- In-memory transport.
- Loopback transport.
- Recorded replay inputs.

---

## 6. Target Platforms

### Tier 1 client platforms

- Windows.
- Linux.

### Tier 1 server platform

- Linux headless.

### Tier 2 client platform

- macOS.

SDL3 supports Windows, Linux, and macOS.

Reference:

- [SDL supported platforms](https://github.com/libsdl-org/SDL/blob/main/docs/README-platforms.md)

Compile the macOS client in CI early, even if full runtime testing comes later. This prevents Windows-only or Linux-only assumptions from accumulating.

Do not target the browser in the first version.

A browser client changes the transport model because browsers do not expose ordinary native UDP sockets. Browser support should become a later, separate experiment using a gateway or a browser-supported transport.

---

## 7. Rendering

## 7.1 Primary recommendation: SDL_GPU

Between OpenGL and Vulkan, use **neither directly as the primary game-facing API**.

Use:

```text
SDL3
  └── SDL_GPU
        └── Small engine rendering layer
```

SDL_GPU exposes a modern model based on:

- Command buffers.
- Pipelines.
- Render passes.
- Explicit resources.
- Backend-independent GPU objects.

Its backends include Vulkan, D3D12, and Metal.

Reference:

- [SDL_GPU documentation](https://wiki.libsdl.org/SDL3/CategoryGPU/raw)

This gives exposure to modern graphics concepts without spending the first several months on:

- Swapchain edge cases.
- Per-platform surface creation.
- Explicit synchronization everywhere.
- Backend boilerplate.
- Platform-specific shader formats.

### Thin rendering API

`MyCore::Render` owns game-neutral GPU resource lifetime and command submission. Keep this
engine wrapper small:

```text
RenderDevice
BufferHandle
TextureHandle
SamplerHandle
ShaderHandle
PipelineHandle
CommandList
RenderPassDescription
```

Do not design a large generic RHI before understanding the game’s real needs.

Game presentation owns render-data extraction and the mapping from game meaning to generic
visual data. `MyCore::Render2D` owns reusable circle/grid shaders, pipelines, batching, and
passes; `Dots::Presentation` converts Dots state into its transient draw list. A later small
Render3D layer may similarly own demonstrated mesh/depth facilities while aim-trainer
presentation owns its camera behavior, target appearance, and scene submission without
depending on Dots.

### Initial renderer scope

The first engine render layer should contain only the facilities needed to support:

- Buffer, texture, sampler, shader, pipeline, command-list, and render-pass ownership.
- The resource upload and frame submission paths required by Dots.
- A small shared debug-line path and Dear ImGui integration where their contracts are
  genuinely game-neutral.

`MyCore::Render2D` initially adds:

- One instanced-quad pipeline for circles.
- Signed-distance circle evaluation in the fragment shader.
- A background or grid pass.

Dear ImGui owns developer-facing text in the next observability feature. SDL_ttf or a UI
toolkit should be adopted when player-facing text and layout become demonstrated needs rather
than implementing glyph rasterization in a game presentation target.

When the aim trainer is implemented, extend `MyCore::Render` only with demonstrated needs
such as depth attachments, vertex/index meshes, perspective transforms, and material
resources. Do not introduce a generic scene graph or render graph merely to prepare for it.

## 7.2 Shader pipeline

Use HLSL as the canonical shader source.

Compile shaders during the asset build. The current portable host-tool chain is:

- `glslang[tools]` for HLSL to SPIR-V on Linux and macOS.
- `spirv-cross` for SPIR-V to MSL on macOS.
- `directx-dxc` for HLSL to DXIL on Windows.

These are build tools, not client runtime libraries. SDL_shadercross remains a possible future
way to consolidate translation if its packaging makes that simpler.

Suggested pipeline:

```text
engine/render_2d/assets/shaders/circle.vert.hlsl + circle.frag.hlsl
        ↓ asset build
assets/mycore/render_2d/shaders/circle.vert.<platform-format>
assets/mycore/render_2d/shaders/circle.frag.<platform-format>
```

Prefer offline shader compilation. Do not make runtime shader compilation a shipping requirement.

Keep loose compiled shader files while Render2D has only two programs and no variants. When the
aim trainer demonstrates multiple materials or feature combinations, evolve the build pipeline
without changing game-facing draw APIs:

```text
HLSL + platform-neutral shader manifest + material references
        |
        v
host-only shader cooker
  enumerate only required variants
  compile DXIL / SPIR-V / MSL or metallib
  reflect and validate resource bindings
  hash and deduplicate blobs
        |
        v
platform shader library
  logical shader ID + variant key -> blob + entry point + binding metadata
        |
        v
runtime ShaderLibrary -> PipelineCache -> MyCore::Render GPU objects
```

The manifest may use TOML but is an engine-owned format, not an industry standard. It should
describe logical programs, stages, entries, variant axes, and platform constraints rather than
hard-code output paths. Prefer compiler reflection for resource counts and bindings instead of
duplicating that contract manually in C++.

Keep these runtime responsibilities distinct:

- `ShaderLibrary`: locate compiled stage blobs and reflection metadata by stable ID and variant.
- `MaterialDefinition`: choose a shader program and variant features plus textures, parameters,
  blending, depth, and culling intent.
- `PipelineCache`: combine shader stages, vertex layout, fixed state, and target formats into a
  live graphics pipeline; support prewarming only after profiling shows first-use stalls.
- `AssetManager`: own cooked shader, texture, mesh, and material lifetime independently of scene
  meaning.

A general asset pack becomes worthwhile only when indexing, compression, integrity checks,
streaming, patches, or file-count overhead are demonstrated. Preserve loose-file development
mode even after release builds gain packed content.

The intended runtime layering is:

```text
authoritative game world
        -> game-owned presentation extraction
        -> transient RenderSnapshot / optional client-only RenderWorld
        -> Render2D or future Render3D visibility, sorting, and batching
        -> materials + ShaderLibrary + PipelineCache
        -> frame passes / optional render graph
        -> MyCore::Render resources and command submission
        -> SDL_GPU -> Metal / Vulkan / D3D12
```

Do not turn the authoritative world into an engine scene. Add a persistent client-only
`RenderWorld` only when culling, LOD, interpolation, render-thread ownership, or stable
mesh/material handles make it simpler than a transient snapshot.

## 7.3 Direct Vulkan as a research branch

After the SDL_GPU renderer works, create a direct Vulkan branch to investigate:

- Swapchain recreation.
- Explicit synchronization.
- Descriptor management.
- Upload allocators.
- Transient allocators.
- Pipeline caching.
- Bindless resources.
- Render graphs.
- GPU-driven rendering.

On macOS, direct Vulkan generally means a portability implementation over Metal such as MoltenVK.

Reference:

- [MoltenVK README](https://github.com/KhronosGroup/MoltenVK/blob/main/README.md)

## 7.4 Why not OpenGL as the primary renderer

OpenGL is acceptable for a small comparison implementation, but it is not the best primary renderer for this project.

Its resource and synchronization model is less representative of modern explicit APIs.

A useful controlled comparison is:

1. Main implementation: SDL_GPU.
2. Research implementation: direct Vulkan.
3. Optional legacy comparison: OpenGL.

---

## 8. Networking Transport

## 8.1 Use GameNetworkingSockets

Use **GameNetworkingSockets**, not raw UDP, as the first transport.

It provides:

- A connection-oriented, message-based API.
- Reliable messages.
- Unreliable messages.
- Fragmentation.
- Retransmission for reliable data.
- Encryption.
- Multiple traffic lanes.
- Network statistics.
- Packet-loss simulation.
- Latency simulation.

Steam is not required for the open-source library.

Reference:

- [GameNetworkingSockets README](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/README.md)

GameNetworkingSockets does **not** provide:

- Entity serialization.
- Delta snapshots.
- Interest management.
- Prediction.
- Reconciliation.
- Gameplay-level replication.

Those remain part of this project.

### Recommended ownership boundary

```text
Adopt:
  socket handling
  connection lifecycle
  encryption
  reliable delivery
  congestion behavior
  loss simulation

Build:
  protocol
  input commands
  snapshots
  delta encoding
  prediction
  interpolation
  interest management
  entity replication
```

Do not build reliable UDP yourself in the first implementation. It would combine two difficult projects and make replication bugs harder to isolate.

---

## 9. Protocol Design

Dots owns its protocol, replication schema, prediction policy, and validation rules. Assume
its 1,000 clients are in one shared world. `MyCore::NetTransport` carries framed byte
payloads and connection events without knowing Dots message types.

Start with the following message classes:

| Message | Delivery | Initial rate | Contents |
|---|---|---:|---|
| Input | Unreliable, application-sequenced | 30 Hz | Sequence, client tick, movement, actions, snapshot ACK |
| Snapshot | Unreliable, superseding older data | 15–20 Hz | Server tick, snapshot ID, baseline, input ACK, entity deltas |
| Control | Reliable ordered | Event-driven | Handshake, rules, disconnect reason, chat |
| Time/statistics | Unreliable | 1–2 Hz | Clock samples and network diagnostics |

### Input redundancy

Each input packet should include:

- The newest input command.
- Optionally the previous one or two commands.

This makes occasional input packet loss less harmful without reliably retransmitting commands that are already stale.

### Packet size

Keep normal packet payloads below a conservative application budget, approximately:

```text
1,100–1,200 bytes
```

Although the transport can fragment messages, ordinary snapshots should be prioritized and spread across ticks rather than habitually fragmented.

### Protocol versioning

Every protocol message should contain:

- A protocol version.
- Or a negotiated feature version.

Never serialize a C++ struct with `memcpy`.

Encode fields explicitly with:

- Defined byte order.
- Defined ranges.
- Explicit bit widths.
- Validation on decode.

### Example snapshot header

```cpp
struct SnapshotHeader
{
    std::uint32_t protocol_version;
    std::uint32_t server_tick;
    std::uint32_t snapshot_id;
    std::uint32_t baseline_snapshot_id;
    std::uint32_t last_processed_input;
};
```

The wire format must remain independent of the in-memory component layout.

---

## 10. Prediction and Reconciliation

Separate four state categories:

```text
Authoritative state
    Server-owned complete world

Replicated state
    Minimal network representation sent to a client

Predicted state
    Locally simulated state for the player's owned cell or cells

Presentation state
    Smoothed/interpolated positions actually rendered
```

## 10.1 Local-player flow

1. The client creates an `InputCommand` with a monotonically increasing sequence.
2. It applies the command immediately to local predicted state.
3. It stores the command and resulting state in a ring buffer.
4. The server receives and validates the command.
5. The server applies it during a fixed simulation tick.
6. A later snapshot includes `last_processed_input`.
7. The client installs the authoritative local state.
8. It replays all stored commands newer than the acknowledged sequence.
9. The simulation state is corrected immediately.
10. The rendered transform converges smoothly toward the corrected simulation state.

### Critical rule

**Correct the simulation immediately; smooth the presentation.**

Do not gradually move authoritative simulation state toward the server state. That changes gameplay behavior and compounds error.

## 10.2 Initial prediction scope

Predict only movement at first.

Keep the following server-authoritative until movement reconciliation is stable:

- Eating another entity.
- Mass changes.
- Death.
- Spawning.
- Splitting.
- Collision outcomes involving other players.

Later, add:

- Speculative local visual effects.
- Predicted splitting.
- Temporary client-generated IDs.
- Server remapping of speculative entities.

## 10.3 Remote entities

Do not predict remote players.

Buffer snapshots and interpolate them with a delay of roughly two or three snapshot intervals.

Make this buffer adaptive later using measured jitter.

## 10.4 Numerical model

Prediction does not require cross-platform bit-identical lockstep.

Use:

- Shared C++ movement code.
- The same fixed timestep.
- The same constants.
- Reconciliation to correct differences.

Start with floating-point simulation.

Quantize values only on the wire.

Fixed-point simulation can become a later controlled experiment.

---

## 11. Designing for 1,000 Clients

For an Agar.io-like game, the first scaling problem is likely to be replication and outbound bandwidth rather than integrating the motion of 1,000 circles.

## 11.1 Interest management

Interest management is mandatory.

Use a **uniform spatial hash or grid** first.

It is:

- Easy to update for moving 2D circles.
- Easy to visualize.
- Easy to benchmark.
- Suitable for collision broad phase.
- Suitable for replication area-of-interest queries.

Each client should subscribe to:

```text
camera rectangle
+ interaction margin
+ interpolation/prefetch margin
```

The replication system should query only grid cells intersecting that region.

## 11.2 Replication priorities

| Priority | Examples |
|---|---|
| Highest | Client-owned entities |
| High | Nearby threats and entities capable of immediate interaction |
| Normal | Other visible moving players |
| Low | Distant visible movement |
| Chunked | Food, decorations, and mostly static objects |

Food pellets should not necessarily be replicated as independent moving entities every snapshot.

Consider region chunks containing:

- Initial food state.
- Sparse add deltas.
- Sparse remove deltas.
- Region revisions.

## 11.3 Why AOI matters

Illustrative naive design:

```text
1,000 entities
× 16 bytes per entity
× 20 snapshots/second
= 320 KB/second per client

× 1,000 clients
= 320 MB/second
≈ 2.56 Gbit/second
```

Illustrative interest-managed design:

```text
100 visible entities
× 12 bytes per entity
× 20 snapshots/second
= 24 KB/second per client

× 1,000 clients
= 24 MB/second
≈ 192 Mbit/second
```

These estimates do not include:

- Packet headers.
- Control traffic.
- Retransmission.
- Keyframes.
- Hosting overhead.
- Encryption overhead.
- Protocol framing.

They demonstrate why:

- Per-client visibility.
- Quantization.
- Delta state.
- Prioritization.
- Byte budgets.

must be core architecture rather than later optimizations.

## 11.4 Snapshot representation

A mature snapshot system should support:

- Snapshot ID.
- Baseline snapshot ID.
- Application-level acknowledgment of received baselines.
- Entity create records.
- Entity update records.
- Entity remove records.
- Per-entity changed-field masks.
- Quantized positions.
- Quantized radii and masses.
- Per-client byte budgets.
- Priority sorting.
- Periodic recovery or keyframe behavior.
- Graceful behavior when a client falls behind.

Do not reliably send ordinary snapshots.

New state supersedes old state.

Make the snapshot stream self-healing:

- Repeat an unknown entity’s create information.
- Continue until the client acknowledges a baseline containing it.

Reliable messages should be reserved for information that cannot be reconstructed from newer state, such as:

- Handshake results.
- Match transitions.
- Chat.
- Administrative messages.
- Disconnect reasons.

---

## 12. Server Threading Model

Begin with a **single owner thread for world simulation**.

This creates a clear authority boundary and makes debugging much easier.

### Suggested architecture

```text
Network I/O thread(s)
        │
        ▼
Validated input queues
        │
        ▼
Single simulation thread
  - fixed tick
  - movement
  - collisions
  - game rules
  - spatial grid update
        │
        ▼
Immutable replication view
        │
        ├── snapshot worker 1
        ├── snapshot worker 2
        └── snapshot worker N
        │
        ▼
Outbound network queues
```

### Conceptual server loop

```cpp
while (running)
{
    poll_network();
    validate_and_enqueue_messages();

    while (clock.has_fixed_tick())
    {
        consume_latest_inputs();
        step_world(fixed_dt);
        update_spatial_index();
        publish_replication_view();
    }

    encode_due_snapshots();
    flush_network();
}
```

### Initial rates

- Simulation: **30 Hz**
- Client input transmission: **30 Hz**
- Snapshot transmission: **15 Hz**
- Client rendering: display rate, independent of simulation
- Server timing: monotonic clock plus integer ticks

Thirty hertz provides approximately 33.3 ms per server tick.

Aim to consume only a fraction of that budget so the server has headroom for spikes.

Prediction hides much of the local perceptual cost of a lower server tick rate.

### Parallelism policy

Do not add a generic job system at the beginning.

Snapshot generation is the first natural parallel workload because it can consume an immutable post-tick view.

Only partition the actual simulation after profiling proves it necessary.

A task scheduler becomes worthwhile when Tracy and load tests identify several bounded CPU
work units that can run concurrently and the single-threaded version misses a measured budget.
Likely candidates are per-client snapshot encoding, visibility/culling, animation, asset
decompression, path queries, and render-command preparation. Prefer adopting and wrapping a
small proven scheduler over building work stealing, dependency graphs, and shutdown semantics
from scratch.

Do not model every long-lived subsystem as an arbitrary task:

- The authoritative game heartbeat remains an owner-thread fixed tick. It may dispatch
  deterministic sub-work and join it before publishing state, but tick ordering is explicit.
- Network I/O normally owns its poll thread or library callback context and communicates through
  queues; packet validation or snapshot encoding may use worker tasks.
- GPU submission and SDL window/event operations stay on their required owner thread. A renderer
  may parallelize culling or draw-packet construction before ordered submission.
- An audio callback or mixer thread is real-time-sensitive and must not wait on a general worker
  pool; non-real-time decoding or streaming preparation may use tasks.
- Physics remains inside the simulation dependency graph. If a future physics library has its
  own job integration, adapt that deliberately rather than running a whole physics step as an
  unsynchronized background task.

Keep the first scheduler contract small: task groups, explicit dependencies or fences,
cooperative shutdown, bounded worker count, exception/error propagation, and profiler labels.
Do not allow jobs to outlive the state they reference. Preserve a deterministic single-threaded
mode for tests, replay comparison, and debugging.

---

## 13. World and Entity Representation

This section describes the Dots world, not a universal MyCore world model. Do not begin by
designing a universal ECS.

Use explicit component arrays:

```text
EntityId[]
Position[]
Velocity[]
Radius[]
Mass[]
OwnerClient[]
EntityFlags[]
```

Possible implementation options:

- Structures of arrays.
- Sparse-set indexing.
- Separate dense arrays for player entities and food entities.
- Generational entity handles.

### Advantages for this game

- Easy serialization.
- Easy iteration.
- Straightforward profiling.
- Clear ownership.
- No hidden lifecycle callbacks.
- Simple spatial-grid integration.
- Easy replication filtering.

After the working version exists, create an EnTT branch and compare:

- Code complexity.
- Iteration speed.
- Serialization integration.
- Debuggability.
- Tooling.
- Entity lifetime behavior.

Reference:

- [EnTT Entity-Component-System documentation](https://github.com/skypjack/entt/wiki/Entity-Component-System)

This creates a real experiment rather than choosing an ECS based only on preference.

## 13.1 Collision and physics

Dots should not use a general physics engine initially.

Implement:

1. Spatial-grid broad phase.
2. Exact circle-circle tests.
3. Game-specific eating and overlap rules.
4. Simple mass and radius rules.

This keeps the authority logic inspectable and makes scaling behavior easier to understand.

The later aim trainer owns its target storage, hit rules, and ray queries. Shared math or
spatial utilities should move into an engine library only after both games establish a
clear common contract; their world representations do not need to match.

---

## 14. Scripting

## 14.1 Use Lua 5.5

Lua is designed to be embedded as a library inside a host application.

Reference:

- [Lua downloads](https://www.lua.org/download.html)

`MyCore::Scripting` starts with the Lua C API and builds a small game-neutral RAII layer
around:

- State lifetime.
- Stack guards.
- Loading scripts.
- Protected calls.
- Error conversion.
- Host function registration.
- Opaque entity handles.
- Memory limits.
- Instruction limits if needed.
- Script version tracking.

This teaches the embedding boundary without coupling the public API to a large C++ binding framework.

## 14.2 Capability-oriented API

Each game owns its script-facing bindings. Dots exposes capabilities rather than raw engine
objects.

Example:

```lua
world.spawn_food(x, y, mass)
world.get_player(id)
rules.set_match_timer(seconds)

events.on_player_join(function(player)
    -- game rule
end)

events.on_entity_eaten(function(eater, victim)
    -- game rule
end)
```

Do not expose:

- Arbitrary engine pointers.
- Direct component memory.
- The whole C++ object graph.
- Unrestricted filesystem access.
- Unrestricted operating-system access.
- Dynamic-library loading.
- Debug facilities in production.

## 14.3 Good scripting responsibilities

- Match rules.
- Spawn rules.
- Tunable values.
- Event reactions.
- Game-mode configuration.
- Noncritical client presentation.
- Debug commands.
- Development cheats.

## 14.4 Keep these in C++ initially

- Movement.
- Prediction.
- Reconciliation.
- Collision hot loops.
- Snapshot construction.
- Serialization.
- Spatial queries.
- Per-entity server tick processing.
- Security validation.

Movement should remain shared C++ code between server and client.

Putting predicted movement in Lua would introduce:

- Script-version differences.
- State differences.
- Garbage-collection behavior.
- Hot-reload differences.

into the most timing-sensitive path.

Perform script reloads only at a tick boundary and use an explicit state migration function.

A later Luau branch could investigate gradual typing and stronger sandboxing.

Reference:

- [Luau sandboxing documentation](https://luau.org/sandbox/)

---

## 15. Repository and Target Layout

Use a multi-game monorepo with ownership visible in the directory tree:

```text
/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── vcpkg-configuration.json
├── cmake/
├── engine/
│   ├── core/
│   ├── math/
│   ├── time/
│   ├── platform_sdl/
│   ├── render/
│   ├── assets/
│   ├── net_transport/
│   ├── scripting/
│   └── debug/
├── games/
│   ├── dots/
│   │   ├── simulation/
│   │   ├── protocol/
│   │   ├── replication/
│   │   ├── presentation/
│   │   ├── rules/
│   │   ├── apps/
│   │   │   ├── client/
│   │   │   ├── server/
│   │   │   └── bot/
│   │   ├── tools/
│   │   │   └── dots_session.py
│   │   ├── assets/
│   │   └── tests/
│   └── aim_trainer/             # Added only at its later milestone
├── tools/
│   ├── assetc/
│   ├── replay_inspector/
│   └── packet_inspector/
└── tests/                       # Cross-module integration and package tests
```

Keep assets, unit tests, and game-specific fuzz or benchmark inputs near their owning
module. Top-level test, fuzz, and benchmark directories are for cross-module drivers and
shared build entry points. Do not create empty top-level fuzz or benchmark directories;
introduce one only when a concrete cross-module target requires it.

### Dependency rules

```text
MyCore::Core
    depends on nothing platform-specific or game-specific

MyCore::Math
    depends on Core only; contains requirement-driven math, not game policy

MyCore::Time
    depends on Core only; owns monotonic ticks, durations, and policy-free fixed-step
    accumulation

MyCore::PlatformPaths
    depends on Core only; discovers OS-native writable locations without owning game schemas
    or requiring SDL video initialization

MyCore::Assets
    depends on Core only; owns game-neutral asset lookup and byte loading, not game formats

MyCore::Render2D
    depends on Render + Assets + Math; owns reusable 2D draw data, shaders, and batching

MyCore::Debug
    depends on Core and adopted logging/profiling libraries; owns no game-specific panels

MyCore::DebugUI
    depends on Debug + PlatformSDL + Render + Dear ImGui; owns backend lifetime but no
    game-specific panels

Dots::Simulation
    depends on Core + Math + Time only

Dots::Protocol
    depends on Core and owns Dots wire messages and concrete protocol IDs

Dots::Replication
    depends on Dots Simulation + Dots Protocol

Dots::Presentation
    depends on Dots Simulation + Dots Replication + MyCore Render2D, not on server runtime code

Dots::Server
    depends on Dots Simulation + Dots Replication + Dots Protocol + MyCore NetTransport

Dots::ClientRuntime
    depends on Dots Replication + Dots Protocol + MyCore NetTransport; owns replicated state,
    not an authoritative simulation world

dots_server
    depends on Dots Server; remains headless

dots_client
    depends on Dots Simulation + Protocol + Presentation + MyCore PlatformSDL + Render2D +
    DebugUI + NetTransport

dots_bot
    depends on Dots Protocol + MyCore NetTransport
    optionally depends on Dots Simulation for predictive bots
```

The headless Dots server must not link SDL video, renderer libraries, Dots presentation, or
ImGui rendering code. Engine libraries must not include Dots headers or depend on Dots
targets.

Treat the engine as a set of libraries, not as one global singleton object that owns the
entire process. Treat each game executable as a thin composition root for libraries owned
by that game and the engine.

---

## 16. Debugging and Observability

Use Dear ImGui for development tooling instead of building an editor.

`MyCore::DebugUI` owns the Dear ImGui context and its SDL3/SDL_GPU backends. Each graphical
game owns the contents of its panels and passes them through the client composition root.
Headless processes may use `MyCore::Debug` logging, metrics, and Tracy hooks without linking
Dear ImGui or SDL video.

Fixed-step observability must distinguish per-render-frame step count from actual tick rate.
Record simulation cost per step, retained backlog, catch-up frames, cap hits, deadline misses,
and discarded time. Rate-limit overload logs and report recovery instead of logging every slow
frame. An offline client may explicitly discard measured excess whole-step backlog to preserve
responsiveness; an authoritative server must not silently discard simulation ticks and instead
uses overload reporting, bounded catch-up, and load shedding.

Reference:

- [Dear ImGui README](https://github.com/ocornut/imgui/blob/master/docs/README.md)

### Useful debug panels

- Current server tick.
- Current client tick.
- Round-trip time.
- Jitter.
- Packet loss.
- Bytes per second by message class.
- Snapshot entity count.
- Baseline IDs.
- Baseline acknowledgment age.
- Input sequence.
- Last processed input.
- Reconciliation count.
- Prediction error magnitude.
- Interpolation-buffer fill.
- Spatial-grid visualization.
- AOI boundaries.
- Per-client byte budget.
- Server tick timing.
- Entity counts by category.
- Snapshot queue depth.
- Outbound queue depth.

## 16.1 Profiling

Use Tracy from the beginning.

Instrument:

```text
ServerTick
Movement
CollisionBroadPhase
CollisionNarrowPhase
SpatialGridUpdate
InterestQuery
SnapshotBuild
SnapshotEncode
NetworkReceive
NetworkSend
ClientPrediction
Reconciliation
RenderSubmission
```

Reference:

- [Tracy package information](https://vcpkg.io/en/package/tracy.html)

## 16.2 Logging

Use spdlog and fmt.

Reference:

- [spdlog README](https://github.com/gabime/spdlog/blob/v1.x/README.md)

Do not log per-entity or per-packet information unconditionally in production builds.

Use:

- Structured categories.
- Log levels.
- Connection IDs.
- Client IDs.
- Server ticks.
- Snapshot IDs.
- Input sequence IDs.

## 16.3 Static analysis

Use clang-tidy in CI.

Reference:

- [clang-tidy documentation](https://clang.llvm.org/extra/clang-tidy/)

Run:

- Fast checks on changed code.
- Full scheduled analysis separately.
- clang-format verification.
- Warnings-as-errors in CI for project code.

## 16.4 CI

Use GitHub Actions.

Reference:

- [GitHub Actions documentation](https://docs.github.com/en/actions)

CI should:

- Configure through CMake presets.
- Build Windows, Linux, and macOS.
- Run tests.
- Verify clang-format on tracked C/C++ sources.
- Run clang-tidy against the Linux compile database with warnings treated as errors.
- Run sanitizer builds.
- Run selected benchmarks.
- Package the Linux `dots_server`.
- Store logs and crash artifacts.
- Cache vcpkg binary packages.

---

## 17. Build Versus Adopt

## 17.1 Build internally

These systems align directly with the learning goals:

| System | Why build it |
|---|---|
| Fixed-step scheduling | Reusable timing boundary driven by concrete game loops |
| Dots simulation and spatial hash | Essential to Dots AOI and collision; not universal engine state |
| Dots snapshot format | Central networking research |
| Delta and baseline system | Central replication research |
| Interest management | Required for 1,000 clients |
| Prediction and reconciliation | Primary gameplay networking goal |
| Interpolation | Client presentation architecture |
| Entity/component storage | Useful controlled design experiment |
| Replay format | Debugging and regression infrastructure |
| Thin rendering layer | Useful RHI and lifetime experiment |
| Bot/load harness | Required to validate scale |
| Lua host wrapper and Dots bindings | Separates reusable VM ownership from game capabilities |
| Small owned math library | Keeps public math conventions stable and requirement-driven |

## 17.2 Adopt existing libraries

| System | Recommended dependency |
|---|---|
| Window/input/controller/audio | SDL3 |
| Portable GPU backend | SDL_GPU |
| Native transport and encryption | GameNetworkingSockets |
| Script VM | Lua |
| Logging | spdlog and fmt |
| Debug UI | Dear ImGui |
| Profiling | Tracy |
| Test registration | Catch2 and CTest |
| Package resolution | vcpkg |
| CI runner | GitHub Actions |
| Developer process orchestration | Python 3 |

## 17.3 Explicitly defer

- General-purpose editor.
- Shared-library plugin ABI.
- Hot-reloaded C++ modules.
- Custom memory allocator framework.
- Generic task graph.
- Generic scene graph.
- General physics engine.
- Custom cryptography.
- Custom reliable UDP.
- Microservices.
- Kubernetes.
- Account persistence.
- Matchmaking.
- Full browser support.

---

## 18. Development Milestones

| Milestone | Exit criterion |
|---|---|
| 0. Foundation | All three desktop platforms configure; Windows and Linux build and run; CI and sanitizers work |
| 1. Offline Dots | `dots_client` renders and controls circles through a fixed-step simulation |
| 2. Headless authority | Server runs the same simulation library and accepts two clients |
| 3. Basic replication | Authoritative positions and game state arrive as full snapshots |
| 4. Prediction | Local movement remains responsive under simulated 100–200 ms latency |
| 5. Reconciliation | Corrections replay unacknowledged input without corrupting state |
| 6. Remote interpolation | Other players remain visually smooth under jitter and loss |
| 7. Interest management | Clients receive only entities in their AOI |
| 8. Delta snapshots | Baselines, quantization, byte budgets, and recovery work under loss |
| 9. Load harness | Automated bots reach 1,000 connections with recorded CPU and bandwidth metrics |
| 10. Lua rules | Match and spawn rules can be reloaded safely at tick boundaries |
| 11. Aim-trainer reuse | An offline 3D aim trainer reuses engine libraries without depending on Dots |
| 12. Engine package | A separate consumer installs MyCore and links selected `MyCore::` components with `find_package` |
| 13. Conditional task scheduling | A measured workload improves without weakening deterministic ownership or tick-tail latency |
| 14. Platform user settings | Packaged games discover per-user configuration through native OS locations without moving game schemas into the engine |
| 15. Research branches | Direct Vulkan, EnTT, Conan, or fixed-point implementations are compared against recorded workloads |

## 18.1 Metrics at the 1,000-client milestone

Measure at least:

```text
server tick p50 / p95 / p99
snapshot-build p50 / p95 / p99
bytes per client per second
packets per second
entities considered per AOI query
entities replicated per snapshot
prediction corrections per minute
correction distance distribution
memory per connection
network queue depths
snapshot queue depths
disconnect failures
packet-decode failures
```

Also track:

- Total server CPU usage.
- Per-thread CPU usage.
- Resident memory.
- Peak allocations.
- Number of grid buckets.
- Number of collision candidates.
- Number of dropped low-priority updates.
- Snapshot baseline miss rate.
- Average snapshot size.
- Maximum snapshot size.
- Input age on arrival.
- Simulation overrun count.

---

## 19. Initial Locked-In Decisions

Use the following as the initial architecture baseline:

```text
C++20

CMake
Ninja
CMakePresets

multi-game monorepo
game-owned executable composition roots
MyCore:: public engine targets
Dots:: public game targets

vcpkg manifest mode
pinned vcpkg baseline

Catch2
CTest
Google Benchmark
libFuzzer

SDL3
SDL_GPU

small owned math library
2D operations first
requirement-driven 3D expansion

HLSL
offline glslang/DXC compilation with SPIRV-Cross translation

GameNetworkingSockets

Lua 5.5
thin direct-C-API wrapper

Windows client
Linux client
Linux headless server
macOS compile-tested from the beginning

30 Hz authoritative simulation
30 Hz client input
15 Hz initial snapshot rate

single-writer simulation thread
parallelizable immutable snapshot view

plain SoA or sparse-set world representation
uniform spatial hash

custom bit-packed replication protocol

Dear ImGui
Tracy
spdlog
fmt

Dots as the first vertical slice
offline 3D aim trainer as the second-game reuse validation

dedicated dots_bot executable
source-tree consumption before installed CMake packaging
Python 3 only for developer-side process orchestration
```

---

## 20. Suggested First Implementation Order

Codex should implement the project in the following order.

### Phase A: Foundation

1. Create the multi-game repository layout with `engine/` and `games/dots/`.
2. Add root CMake configuration.
3. Add CMake presets.
4. Add vcpkg manifest.
5. Add `MyCore::Core`.
6. Add the requirement-driven `MyCore::Math` and policy-free `MyCore::Time` libraries.
7. Add `Dots::Simulation`.
8. Add Catch2 and one passing unit test.
9. Add CI builds.
10. Add sanitizer presets.
11. Add clang-format and clang-tidy configuration.

### Phase B: Offline vertical slice

1. Create the `dots_client` SDL3 application under `games/dots/apps`.
2. Add `MyCore::PlatformSDL`, `MyCore::Assets`, and game-neutral SDL_GPU initialization.
3. Add `MyCore::Render`, `MyCore::Render2D`, and `Dots::Presentation`; render one instanced circle.
4. Add fixed-step simulation.
5. Add keyboard and mouse input.
6. Move one player circle.
7. Add food entities.
8. Add spatial grid.
9. Add circle collision and eating.
10. Add Dear ImGui metrics.

### Phase C: Authoritative networking

1. Add the headless `dots_server` executable.
2. Add the game-neutral `MyCore::NetTransport` GameNetworkingSockets wrapper.
3. Add connection handshake.
4. Add explicit Dots protocol encoding and decoding.
5. Send sequenced input commands.
6. Run the world only on the server.
7. Send full snapshots.
8. Render replicated state on clients.
9. Add packet validation.
10. Add malformed-packet fuzz targets.
11. Add `dots_session.py` to launch a local server and configurable clients from a selected
    preset build directory.

### Phase D: Responsive client

1. Add local input prediction.
2. Add input history ring buffer.
3. Add server acknowledgment of processed input.
4. Add reconciliation.
5. Separate simulation state from presentation state.
6. Add smoothing for local corrections.
7. Add remote snapshot buffering.
8. Add interpolation for remote entities.
9. Test under artificial latency, jitter, and loss.

### Phase E: Scale

1. Add per-client AOI.
2. Add uniform-grid interest queries.
3. Add snapshot byte budgets.
4. Add priorities.
5. Add quantized fields.
6. Add delta snapshots.
7. Add baseline acknowledgments.
8. Add snapshot recovery.
9. Build the `dots_bot` executable.
10. Extend `dots_session.py` with bot counts, connection ramps, input patterns, metrics
    capture, prefixed logs, and reliable child cleanup.
11. Run 10, 100, 500, and 1,000-client tests.

### Phase F: Dots scripting

1. Add the game-neutral `MyCore::Scripting` Lua host wrapper.
2. Add Dots-owned capability-oriented bindings and rules.
3. Add tick-boundary reload.
4. Add script state migration.

### Phase G: Second-game and package validation

1. Add an offline desktop game under `games/aim_trainer`.
2. Reuse `MyCore::Core`, Math, Time, PlatformSDL, Render, Assets, and Debug facilities.
3. Expand Math with the required 3D vectors, matrices, quaternions, transforms, rays, and
   bounds.
4. Expand Render with the required depth, mesh, perspective, and material facilities.
5. Add aim-trainer-owned camera, mouse-look, target, hit, spawn, score, reset, and
   presentation code.
6. Verify that the aim trainer has no dependency on Dots targets or headers.
7. Export stabilized engine targets through an installed CMake package.
8. Validate `find_package(MyCore CONFIG REQUIRED)` from a separate minimal consumer.

### Phase H: Conditional task-scheduler validation

Begin this phase only when load-harness or second-game profiles expose independent CPU work and
a missed budget.

1. Record the single-threaded baseline and tail latency.
2. Compare a proven task library with a minimal fixed worker pool.
3. Introduce `MyCore::Tasks` around one immutable workload, preferably snapshot construction.
4. Preserve deterministic single-thread mode and explicit subsystem owner threads.
5. Keep the scheduler only if the measured result justifies its complexity.

### Phase I: Platform user settings

1. Add game-neutral config/data/cache/log directory discovery behind `MyCore::PlatformPaths`.
2. Preserve game-owned filenames, TOML parsing, defaults, and validation.
3. Add Dots per-user config fallback while retaining explicit CLI and developer overrides.
4. Test all platform conventions with injected profile locations and no real-home writes.

### Phase J: Research branches

1. Compare EnTT against the Dots manual world representation.
2. Compare direct Vulkan against SDL_GPU.
3. Compare Conan packaging against the installed CMake/vcpkg workflow.
4. Compare floating point against fixed-point Dots simulation where useful.

---

## 21. Architectural Principles for Codex

When generating code for this project, preserve these principles:

1. Keep every game simulation independent of rendering and platform APIs.
2. Keep wire formats independent of C++ memory layout.
3. Keep the server authoritative.
4. Keep prediction limited and explicit.
5. Correct simulation immediately and smooth only presentation.
6. Prefer simple data structures before generic frameworks.
7. Measure before adding concurrency or custom allocators.
8. Make every decoder hostile-input safe.
9. Keep snapshots unreliable and self-healing.
10. Treat bandwidth as a fixed per-client budget.
11. Keep the server build fully headless.
12. Use immutable post-tick views for parallel snapshot work.
13. Build tooling that exposes network and simulation behavior.
14. Create research branches only after a measurable baseline exists.
15. Prefer a working vertical slice over speculative generality.
16. Keep game-owned types, policies, assets, and presentation out of engine libraries.
17. Give every game ownership of its client, server, bot, and other executable composition
    roots.
18. Extract a reusable facility only when its contract is game-neutral and supported by a
    concrete use.

---

## 22. Intended Outcome

This scope should produce a working Dots game early while reserving the highest-value
engine research for custom implementation:

- Authority.
- Replication.
- Prediction.
- Reconciliation.
- Spatial data structures.
- Serialization.
- Observability.
- API boundaries.
- Server scaling.
- Client/server code sharing.
- Cross-game reuse demonstrated by a distinct offline 3D game.
- External consumption through stable CMake targets and an installed package.

The project should remain small enough to understand end to end while establishing clean
engine/game ownership. The aim trainer and external consumer are validation milestones, not
permission to build speculative universal systems before Dots works.
