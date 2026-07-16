# Clean Minimal C++ Game Engine + Authoritative Multiplayer Game

> **Purpose:** Architecture and technology plan for a learning-focused game engine and an Agar.io-like server-authoritative game.
>
> This document is intended to serve as a source-of-truth brief for implementation with Codex. The project should prioritize research value, understandable systems, and controlled technology experiments over feature breadth.

---

## 1. Project Direction

Build **a networked game vertical slice with reusable engine libraries**, not a general-purpose engine followed by a game.

The Agar.io-like game should drive every subsystem requirement.

### Primary research targets

1. Authoritative simulation.
2. Input prediction and reconciliation.
3. Snapshot replication and interest management.
4. Scaling one simple world toward 1,000 clients.
5. Clean client/server/engine boundaries.
6. Controlled comparisons of rendering, ECS, scripting, and packaging approaches.

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
| Shader language | **HLSL**, compiled offline through SDL_shadercross |
| Networking transport | **GameNetworkingSockets** |
| Scripting | **Lua 5.5**, using a small direct C API wrapper |
| Debug UI | **Dear ImGui** |
| Logging | **spdlog + fmt** |
| Profiling | **Tracy**, plus GPU capture tools |
| Static analysis | **clang-tidy + clang-format** |
| Client platforms | Windows x64 and Linux x64 first; macOS arm64 compile-tested early |
| Server platform | Linux x64 headless |
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
linux-clang-asan
linux-clang-tsan
linux-gcc-release
linux-server-release
macos-clang-debug
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
glm
```

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

For this monorepo game, start with vcpkg.

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

- Windows x64.
- Linux x64.

### Tier 1 server platform

- Linux x64 headless.

### Tier 2 client platform

- macOS arm64.

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

Keep the engine wrapper small:

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

### Initial renderer scope

The first renderer should contain:

- One instanced-quad pipeline for circles.
- Signed-distance circle evaluation in the fragment shader.
- A background or grid pass.
- One text or font-atlas path.
- A small debug-line renderer.
- Dear ImGui integration.

This is enough for thousands of circles while remaining understandable.

## 7.2 Shader pipeline

Use HLSL as the canonical shader source.

Compile shaders during the asset build.

SDL_shadercross can accept HLSL or SPIR-V and produce formats such as:

- DXIL
- SPIR-V
- MSL

Reference:

- [SDL_shadercross README](https://github.com/libsdl-org/SDL_shadercross/blob/master/README.txt)

Suggested pipeline:

```text
assets/shaders/circle.hlsl
        ↓ asset build
generated/shaders/dxil/circle.bin
generated/shaders/spirv/circle.bin
generated/shaders/msl/circle.bin
```

Prefer offline shader compilation. Do not make runtime shader compilation a shipping requirement.

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

Assume the 1,000 clients are in one shared world.

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

---

## 13. World and Entity Representation

Do not begin by designing a universal ECS.

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

Do not use a general physics engine initially.

Implement:

1. Spatial-grid broad phase.
2. Exact circle-circle tests.
3. Game-specific eating and overlap rules.
4. Simple mass and radius rules.

This keeps the authority logic inspectable and makes scaling behavior easier to understand.

---

## 14. Scripting

## 14.1 Use Lua 5.5

Lua is designed to be embedded as a library inside a host application.

Reference:

- [Lua downloads](https://www.lua.org/download.html)

Start with the Lua C API and build a small RAII layer around:

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

Expose capabilities rather than raw engine objects.

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

Use a monorepo:

```text
/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── vcpkg-configuration.json
├── cmake/
├── engine/
│   ├── core/
│   ├── platform_sdl/
│   ├── render/
│   ├── net_transport/
│   ├── scripting/
│   └── debug/
├── game/
│   ├── protocol/
│   ├── simulation/
│   ├── replication/
│   ├── rules/
│   └── presentation/
├── apps/
│   ├── client/
│   ├── server/
│   └── bot/
├── tools/
│   ├── assetc/
│   ├── replay_inspector/
│   └── packet_inspector/
├── assets/
├── tests/
├── fuzz/
└── benchmarks/
```

### Dependency rules

```text
engine/core
    depends on nothing platform-specific

game/simulation
    depends on core only

game/protocol
    depends on core only

game/replication
    depends on simulation + protocol

server
    depends on simulation + replication + transport + scripting

client
    depends on simulation + protocol + transport + platform + render

bot
    depends on protocol + transport
    optionally depends on simulation for predictive bots
```

The headless server must not link:

- SDL video.
- Renderer libraries.
- Client presentation code.
- ImGui rendering code.

Treat the engine as a set of libraries, not as one global singleton object that owns the entire process.

---

## 16. Debugging and Observability

Use Dear ImGui for development tooling instead of building an editor.

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
- Run sanitizer builds.
- Run selected benchmarks.
- Package the Linux server.
- Store logs and crash artifacts.
- Cache vcpkg binary packages.

---

## 17. Build Versus Adopt

## 17.1 Build internally

These systems align directly with the learning goals:

| System | Why build it |
|---|---|
| Fixed-step simulation | Core engine architecture |
| Spatial hash | Essential to AOI and collision |
| Snapshot format | Central networking research |
| Delta and baseline system | Central replication research |
| Interest management | Required for 1,000 clients |
| Prediction and reconciliation | Primary gameplay networking goal |
| Interpolation | Client presentation architecture |
| Entity/component storage | Useful controlled design experiment |
| Replay format | Debugging and regression infrastructure |
| Thin rendering layer | Useful RHI and lifetime experiment |
| Bot/load harness | Required to validate scale |
| Lua-facing game API | Teaches scripting-boundary design |

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
| 1. Offline game | One client renders and controls circles through a fixed-step simulation |
| 2. Headless authority | Server runs the same simulation library and accepts two clients |
| 3. Basic replication | Authoritative positions and game state arrive as full snapshots |
| 4. Prediction | Local movement remains responsive under simulated 100–200 ms latency |
| 5. Reconciliation | Corrections replay unacknowledged input without corrupting state |
| 6. Remote interpolation | Other players remain visually smooth under jitter and loss |
| 7. Interest management | Clients receive only entities in their AOI |
| 8. Delta snapshots | Baselines, quantization, byte budgets, and recovery work under loss |
| 9. Load harness | Automated bots reach 1,000 connections with recorded CPU and bandwidth metrics |
| 10. Lua rules | Match and spawn rules can be reloaded safely at tick boundaries |
| 11. Research branches | Direct Vulkan, EnTT, Conan, or fixed-point implementations are compared against recorded workloads |

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

vcpkg manifest mode
pinned vcpkg baseline

Catch2
CTest
Google Benchmark
libFuzzer

SDL3
SDL_GPU

HLSL
offline SDL_shadercross compilation

GameNetworkingSockets

Lua 5.5
thin direct-C-API wrapper

Windows x64 client
Linux x64 client
Linux x64 headless server
macOS arm64 compile-tested from the beginning

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

dedicated headless bot executable
```

---

## 20. Suggested First Implementation Order

Codex should implement the project in the following order.

### Phase A: Foundation

1. Create the repository layout.
2. Add root CMake configuration.
3. Add CMake presets.
4. Add vcpkg manifest.
5. Add `engine_core`.
6. Add `game_simulation`.
7. Add Catch2 and one passing unit test.
8. Add CI builds.
9. Add sanitizer presets.
10. Add clang-format and clang-tidy configuration.

### Phase B: Offline vertical slice

1. Create SDL3 client application.
2. Add SDL_GPU initialization.
3. Render one instanced circle.
4. Add fixed-step simulation.
5. Add keyboard and mouse input.
6. Move one player circle.
7. Add food entities.
8. Add spatial grid.
9. Add circle collision and eating.
10. Add Dear ImGui metrics.

### Phase C: Authoritative networking

1. Add a headless server executable.
2. Add GameNetworkingSockets transport wrapper.
3. Add connection handshake.
4. Add explicit protocol encoding and decoding.
5. Send sequenced input commands.
6. Run the world only on the server.
7. Send full snapshots.
8. Render replicated state on clients.
9. Add packet validation.
10. Add malformed-packet fuzz targets.

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
9. Build the bot executable.
10. Run 10, 100, 500, and 1,000-client tests.

### Phase F: Scripting and research branches

1. Embed Lua.
2. Expose capability-oriented rules.
3. Add tick-boundary reload.
4. Add script state migration.
5. Compare EnTT against the manual representation.
6. Compare direct Vulkan against SDL_GPU.
7. Compare Conan packaging against the vcpkg monorepo.
8. Compare floating point against fixed-point simulation where useful.

---

## 21. Architectural Principles for Codex

When generating code for this project, preserve these principles:

1. Keep the simulation independent of rendering and platform APIs.
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

---

## 22. Intended Outcome

This scope should produce a working game early while reserving the highest-value engine research for custom implementation:

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

The project should remain small enough to understand end to end, while still exercising the types of design decisions encountered when maintaining a production game engine.
