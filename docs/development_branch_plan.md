# Incremental Feature Branch Plan

This roadmap breaks `game_engine_technology_plan.md` into small mergeable feature branches. Each branch should add one clear capability, include tests for that capability, and leave `main` buildable.

Branch naming convention:

```text
feature/00-foundation
feature/01-vcpkg-catch2
feature/02-core-types
```

Research branches should use:

```text
research/<topic>
```

## Ground Rules

- Merge branches sequentially unless explicitly marked as research.
- Keep `main` buildable after every merge.
- Add or update tests in the same branch as the feature.
- Prefer real systems over heavy mocking: real `World`, serializers, protocol messages, in-memory transport, loopback transport, and replay inputs.
- Keep simulation independent of SDL, rendering, networking transport, and scripting.
- Keep the server authoritative and headless.
- Keep wire formats independent of C++ memory layout.
- Defer research branches until a measurable baseline exists.

## Validation Commands

Use the local preset name that matches the platform. The first branch creates the presets; later branches should keep these commands working.

```bash
cmake --preset <local-debug>
cmake --build --preset <local-debug>
ctest --preset <local-debug>
```

When sanitizer presets exist:

```bash
cmake --preset <local-asan>
cmake --build --preset <local-asan>
ctest --preset <local-asan>
```

## Branch Roadmap

### `feature/00-foundation`

Purpose: create the empty buildable project.

Changes:

- Add monorepo layout: `engine/`, `game/`, `apps/`, `tests/`, `benchmarks/`, `fuzz/`, `tools/`, `assets/`, `cmake/`, and `docs/`.
- Add root `CMakeLists.txt`, `CMakePresets.json`, `.gitignore`, `.clang-format`, and `.clang-tidy`.
- Set C++20 globally through target-based CMake configuration.
- Add a warnings/options interface target for project code.
- Add placeholder libraries: `engine_core` and `game_simulation`.
- Add placeholder executables: `game_client`, `game_server`, and `game_bot`.

Tests:

- Add a minimal CTest target before Catch2 is introduced.
- Prove configure, build, and `ctest` work.

Exit criterion:

- The project configures, builds, and runs CTest without external game dependencies.

### `feature/01-vcpkg-catch2`

Purpose: introduce dependency management and the first real test framework.

Changes:

- Add `vcpkg.json` and `vcpkg-configuration.json` with a pinned registry baseline.
- Add initial dependency: `catch2`.
- Replace the placeholder test with a Catch2 v3 test executable.
- Register Catch2 tests through CTest.

Tests:

- Add `engine_core_smoke_tests`.
- Test one trivial function from `engine_core`.

Exit criterion:

- Configure, build, and test work through vcpkg manifest mode.

### `feature/02-core-types`

Purpose: add platform-free engine primitives.

Changes:

- Add fixed-width aliases or type helpers where useful.
- Add strong IDs for entities, clients, snapshots, and input commands.
- Add tick/time types and fixed-step helpers.
- Add basic 2D math types used by movement.
- Keep `engine_core` independent of platform and rendering libraries.

Tests:

- Entity/client/input ID validity and generation behavior.
- Tick arithmetic and fixed timestep helpers.
- Vector math operations needed by simulation.

Exit criterion:

- Core contains enough stable primitives for simulation and protocol work.

### `feature/03-fixed-step-simulation`

Purpose: create the first headless gameplay loop.

Changes:

- Add `game/simulation` world storage using explicit SoA-style data.
- Add player entity creation/removal.
- Add movement input commands.
- Add fixed `30 Hz` world stepping.
- Add one controllable player entity without rendering.

Tests:

- Spawn and remove a player entity.
- Apply movement input for one tick.
- Step multiple ticks and assert expected position.
- Replay a recorded list of inputs and assert final state.

Exit criterion:

- A headless simulation can move one player through shared C++ code.

### `feature/04-spatial-grid-collision`

Purpose: add the first game-specific world systems.

Changes:

- Add uniform spatial grid for broad-phase queries.
- Add exact circle overlap checks.
- Add food entities.
- Add eating, mass, and radius rules.
- Keep collision and eating rules in C++.

Tests:

- Grid insertion, movement, removal, and query.
- Circle overlap edge cases.
- Player eats food and updates mass/radius.
- Invalid or out-of-range entities are rejected.
- Scenario replay with moving entities and food.

Exit criterion:

- The offline world supports basic Agar.io-like movement and eating.

### `feature/05-sdl-client-window-input`

Purpose: create the first playable local client.

Changes:

- Add `sdl3` dependency.
- Implement `apps/client` window lifecycle.
- Poll keyboard and mouse input.
- Convert input into simulation commands.
- Run local fixed-step simulation in the client.
- Render temporary debug visuals with the simplest acceptable SDL path.

Tests:

- Unit test input-command construction independently of SDL.
- Add a client initialization smoke test in a disabled/headless mode if practical.

Exit criterion:

- A local client window opens and controls one simulated player.

### `feature/06-sdl-gpu-render-baseline`

Purpose: move rendering behind the intended engine layer.

Changes:

- Add `engine/render` thin API around SDL_GPU.
- Render instanced circles.
- Add a background or grid pass.
- Keep renderer separate from simulation ownership.
- Add minimal shader and asset loading placeholders.

Tests:

- Unit test render data extraction from simulation state.
- Add render API construction tests only where stable and not overly mocked.

Exit criterion:

- Client renders player and food circles through the engine render layer.

### `feature/07-debug-observability`

Purpose: make behavior inspectable before networking.

Changes:

- Add `spdlog` and `fmt`.
- Add Dear ImGui dependency and debug overlay.
- Show tick, entity counts, frame timing, and grid stats.
- Add logging categories for client, server, simulation, protocol, and transport.

Tests:

- Unit test metrics aggregation without UI.
- Unit test formatting for structured IDs where useful.

Exit criterion:

- Offline client exposes enough debug data to understand simulation behavior.

### `feature/08-protocol-binary-codec`

Purpose: define hostile-input-safe wire messages before sockets.

Changes:

- Add `game/protocol`.
- Implement explicit encoders and decoders for protocol version, handshake, input command, and full snapshot.
- Use defined byte order, explicit widths, validation, and no `memcpy` struct serialization.
- Add packet size budget constants.

Tests:

- Round-trip encode/decode tests for every message.
- Golden packet tests for representative input and snapshot messages.
- Malformed decode tests for truncation, invalid enum, wrong version, and out-of-range fields.

Exit criterion:

- Protocol messages are fully testable without any network transport.

### `feature/09-inmemory-transport-integration`

Purpose: prove authoritative client/server flow without external networking.

Changes:

- Add an in-memory transport interface for deterministic tests.
- Add a headless server loop using `game_simulation`.
- Send sequenced input commands from client to server.
- Send full snapshots from server to client.
- Render replicated state on clients.
- Disable local simulation ownership in networked client mode.

Tests:

- Two clients connect to an in-memory server.
- Server receives input and advances authority.
- Clients receive full snapshots.
- Disconnect cleanup removes owned entities.
- Invalid packets are rejected without crashing.

Exit criterion:

- Authoritative server/client protocol works over in-memory transport.

### `feature/10-gamenetworkingsockets-transport`

Purpose: adopt the real native networking transport.

Changes:

- Add `gamenetworkingsockets` dependency.
- Implement `engine/net_transport` wrapper.
- Add connection lifecycle handling.
- Use reliable control messages and unreliable input/snapshot messages.
- Preserve the transport interface used by in-memory tests.

Tests:

- Loopback client/server integration test.
- Handshake success and failure tests.
- Packet validation tests through the transport boundary.
- Keep all in-memory integration tests running.

Exit criterion:

- Two real clients can connect to a headless local server and see authoritative movement.

### `feature/11-prediction-reconciliation`

Purpose: make local movement responsive under latency.

Changes:

- Add predicted local state separate from replicated state.
- Add input history ring buffer.
- Include `last_processed_input` in snapshots.
- Reconcile by installing authoritative state and replaying unacknowledged input.
- Smooth presentation only; never smooth authoritative simulation state.

Tests:

- Prediction applies input immediately.
- Reconciliation replays unacknowledged commands.
- Dropped or delayed snapshots do not corrupt simulation state.
- Artificial `100-200 ms` latency test remains playable in loopback or in-memory mode.

Exit criterion:

- Local player movement remains responsive and corrects cleanly under simulated latency.

### `feature/12-remote-interpolation`

Purpose: smooth presentation for non-owned entities.

Changes:

- Add snapshot buffer for remote entities.
- Interpolate presentation state with a two-to-three snapshot interval delay.
- Track jitter and buffer fill metrics.
- Keep remote entities non-predicted.

Tests:

- Interpolation between known snapshots.
- Jitter/loss simulation without presentation state explosions.
- Remote entity create, update, and remove behavior.

Exit criterion:

- Remote players render smoothly under simulated jitter and packet loss.

### `feature/13-interest-management`

Purpose: reduce replication to each client's area of interest.

Changes:

- Reuse uniform grid for AOI queries.
- Add per-client camera/interest rectangle with margin.
- Build full snapshots using AOI filtering.
- Add debug visualization for AOI and replicated entity count.

Tests:

- AOI includes visible and margin entities.
- AOI excludes distant entities.
- Owned entity always has highest priority.
- Snapshot entity counts shrink when entities exist outside view.

Exit criterion:

- Clients receive only relevant entities plus required owned state.

### `feature/14-delta-snapshots-byte-budget`

Purpose: move from full snapshots to scalable replication.

Changes:

- Add snapshot IDs, baseline IDs, and baseline ACKs.
- Add create, update, and remove records.
- Add changed-field masks.
- Add quantized positions, radii, and mass.
- Add per-client byte budgets and priority sorting.
- Add self-healing create resend when a baseline is missing.

Tests:

- Delta encode/decode round trips.
- Golden packet tests for quantized deltas.
- Baseline miss recovery.
- Byte budget enforcement.
- Loss and reordering impairment tests.

Exit criterion:

- Snapshot stream remains correct under loss while respecting byte budgets.

### `feature/15-bot-load-harness`

Purpose: measure scalability instead of guessing.

Changes:

- Implement `apps/bot`.
- Add configurable bot count, input pattern, spawn behavior, and connection ramp.
- Add metrics output for CPU, bandwidth, tick time, queue depth, and disconnects.
- Add benchmark targets for grid, world stepping, AOI, and snapshot build.

Tests:

- 10-bot automated integration test in CI-friendly mode.
- Local/manual scenarios for 100, 500, and 1,000 clients.
- Benchmark targets compile and run.

Exit criterion:

- The project can produce repeatable load metrics and reach staged bot counts locally.

### `feature/16-lua-rules`

Purpose: add scripting after the C++ gameplay baseline exists.

Changes:

- Add Lua dependency.
- Add `engine/scripting` RAII Lua wrapper.
- Expose capability-oriented rules API only.
- Support tick-boundary reload.
- Keep movement, collision, prediction, serialization, and hot loops in C++.

Tests:

- Load valid script.
- Reject invalid script with useful error.
- Script can configure spawn and match rules.
- Reload occurs only at tick boundary.
- Script cannot access raw engine objects or arbitrary filesystem APIs.

Exit criterion:

- Match and spawn rules can be changed through Lua without destabilizing core simulation.

## Research Branches

Create these only after the bot/load harness or Lua rules branch provides a stable comparison point.

### `research/entt-world-representation`

Compare EnTT against the manual SoA representation.

Measure:

- Code complexity.
- Iteration speed.
- Serialization integration.
- Debuggability.
- Entity lifetime behavior.

### `research/direct-vulkan-renderer`

Compare direct Vulkan against SDL_GPU.

Measure:

- Swapchain complexity.
- Synchronization and descriptor-management complexity.
- Render submission cost.
- Debug tooling value.
- Portability cost, especially on macOS.

### `research/conan-packaging`

Compare Conan 2 against vcpkg manifest mode.

Measure:

- Package authoring effort.
- Profile management.
- CI cache behavior.
- Binary package reproducibility.

### `research/fixed-point-simulation`

Compare fixed-point simulation against floating point plus reconciliation.

Measure:

- Prediction error distribution.
- Runtime cost.
- Implementation complexity.
- Cross-platform behavior.

### `research/opengl-renderer`

Optional legacy comparison against SDL_GPU.

Measure:

- API simplicity.
- Resource lifetime clarity.
- Synchronization model mismatch.
- Rendering performance for the current workload.

Each research branch must include:

- `docs/research/<topic>.md`.
- The replay, benchmark, or load workload used for comparison.
- A measured outcome and recommendation.

## Public Boundaries

Preserve these subsystem boundaries:

- `engine/core`: platform-free primitives and utilities.
- `game/simulation`: authoritative gameplay state and fixed-step rules; depends only on core.
- `game/protocol`: explicit binary protocol independent of memory layout.
- `game/replication`: client-specific snapshot construction.
- `engine/net_transport`: transport wrapper with no gameplay replication logic.
- `engine/render`: SDL_GPU-backed rendering API with no simulation ownership.
- `apps/server`: headless authority; must not link renderer, SDL video, or ImGui rendering.
- `apps/client`: presentation, input, prediction, reconciliation, and rendering.
- `apps/bot`: protocol/transport-driven load harness.

## CI Progression

Start CI small and expand as subsystems appear:

1. Linux configure/build/test.
2. macOS configure/build/test.
3. Windows configure/build/test.
4. Sanitizer preset after core and protocol exist.
5. Benchmark smoke after spatial grid exists.
6. Loopback networking tests after GameNetworkingSockets exists.
7. Bot/load tests as manual or scheduled jobs before making them required.

CI should eventually:

- Configure through CMake presets.
- Build Windows, Linux, and macOS.
- Run tests.
- Run sanitizer builds.
- Run selected benchmarks.
- Package the Linux server.
- Store logs and crash artifacts.
- Cache vcpkg binary packages.

## Assumptions

- Initial development can happen on macOS, but CMake must keep Windows and Linux compatibility from the start.
- vcpkg manifest mode is the only dependency manager for mainline.
- SDL_GPU is the primary renderer, though one temporary simple SDL visualization branch is acceptable before the render layer.
- Lua, EnTT, Vulkan, Conan, OpenGL, and fixed-point work are deferred until the baseline game loop and networking path are measurable.
- The first server architecture uses one owner thread for world simulation.
- Snapshot worker parallelism is introduced only after immutable replication views exist.
- The 1,000-client target is validated through staged bot milestones: 10, 100, 500, and 1,000.
