# Incremental Feature Branch Plan

This roadmap breaks `game_engine_technology_plan.md` into small mergeable feature branches. Each branch should add one clear capability, include tests for that capability, and leave `main` buildable.

Dots is the first game and the sole implementation driver through the multiplayer and
scripting milestones. Engine libraries use `MyCore::` targets and `mycore/` public include
paths. Dots libraries use `Dots::` targets and `dots/` public include paths. A later offline
3D aim trainer validates reuse without influencing early systems speculatively.

Branch naming convention:

```text
feature/00-foundation
feature/01-vcpkg-catch2
feature/02-core-math-time
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
- Keep Dots types, policies, assets, and presentation out of engine libraries.
- Keep game executables under their owning game; top-level tools must be game-neutral.
- Extract a reusable engine facility only when it has a game-neutral contract.
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

- Add multi-game monorepo layout: `engine/`, `games/dots/`, `tests/`, `benchmarks/`,
  `fuzz/`, `tools/`, `cmake/`, and `docs/`.
- Add root `CMakeLists.txt`, `CMakePresets.json`, `.gitignore`, `.clang-format`, and `.clang-tidy`.
- Set C++20 globally through target-based CMake configuration.
- Add a warnings/options interface target for project code.
- Add a target-based executable helper that places runnable outputs under
  `build/<preset>/bin`.
- Add placeholder libraries and public aliases: `mycore_core`/`MyCore::Core` and
  `dots_simulation`/`Dots::Simulation`.
- Add placeholder executables under `games/dots/apps`: `dots_client`, `dots_server`, and
  `dots_bot`.
- Root engine public headers at `mycore/` and Dots public headers at `dots/`.

Tests:

- Add a minimal CTest target before Catch2 is introduced.
- Prove configure, build, and `ctest` work.

Exit criterion:

- The project configures, builds, and runs CTest without external game dependencies.
- No Dots target or header is reachable from `MyCore::Core`.
- All executable targets have predictable paths under the preset build directory's `bin`
  directory.

### `feature/01-vcpkg-catch2`

Purpose: introduce dependency management and the first real test framework.

Changes:

- Add `vcpkg.json` and `vcpkg-configuration.json` with a pinned registry baseline.
- Add initial dependency: `catch2`.
- Replace the placeholder test with a Catch2 v3 test executable.
- Register Catch2 tests through CTest.

Tests:

- Add `mycore_core_smoke_tests`.
- Test one trivial function from `MyCore::Core`.

Exit criterion:

- Configure, build, and test work through vcpkg manifest mode.

### `feature/02-core-math-time`

Purpose: add small platform-free Core, Math, and Time primitives without moving game domain
types or policy into the engine.

Changes:

- Add generic strong-ID machinery to `MyCore::Core`; avoid aliases that merely rename
  standard fixed-width integers.
- Add `mycore_math` with public alias `MyCore::Math`.
- Add only the 2D vector operations required by Dots movement.
- Add `mycore_time` with public alias `MyCore::Time` for monotonic tick values, duration
  conversion, and a policy-free fixed-step accumulator.
- Keep Dots tick rate, overrun handling, and simulation behavior in Dots.
- Keep concrete entity, client, snapshot, and input IDs in their owning Dots or transport
  modules when those modules are introduced.
- Keep Core, Math, and Time independent of platform, rendering, and game libraries.

Tests:

- Generic strong-ID validity and comparison behavior.
- Tick arithmetic, duration conversion, accumulator remainder, and maximum-step behavior.
- Vector math operations needed by simulation.

Exit criterion:

- Core, Math, and Time contain enough stable primitives for Dots simulation without
  encoding Dots domain policy.

### `feature/03-fixed-step-simulation`

Purpose: create the first headless gameplay loop.

Changes:

- Add `games/dots/simulation` world storage to `Dots::Simulation`, depending on
  `MyCore::Core`, Math, and Time and using explicit SoA-style data.
- Define concrete Dots entity and input-command IDs in Dots-owned headers using the generic
  Core strong-ID facility.
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

- The headless Dots simulation can move one player through shared C++ code and remains
  independent of SDL, rendering, and networking.

### `feature/04-spatial-grid-collision`

Purpose: add the first game-specific world systems.

Changes:

- Add uniform spatial grid for broad-phase queries.
- Add exact circle overlap checks.
- Add food entities.
- Add eating, mass, and radius rules.
- Keep collision and eating rules in C++.
- Keep the spatial grid and collision policy in `Dots::Simulation`; do not present them as a
  universal engine world or physics API.

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
- Add `MyCore::PlatformSDL` for SDL lifetime, window ownership, events, and raw device state.
- Implement `games/dots/apps/client` as the `dots_client` composition root.
- Convert raw keyboard and mouse state into Dots-owned simulation commands outside the
  platform library.
- Run local `Dots::Simulation` fixed-step simulation in the client.
- Render temporary debug visuals with the simplest acceptable SDL path.

Tests:

- Unit test input-command construction independently of SDL.
- Compile and test platform input snapshots without linking or including a Dots target.
- Add a client initialization smoke test in a disabled/headless mode if practical.

Exit criterion:

- A local client window opens and controls one simulated player.

### `feature/06-sdl-gpu-render-baseline`

Purpose: move rendering behind the intended engine layer.

Changes:

- Add game-neutral `MyCore::Render` thin API around SDL_GPU resource lifetime, upload, and
  command submission.
- Add game-neutral `MyCore::Assets` interfaces for locating and reading asset bytes;
  keep Dots formats and asset manifests game-owned.
- Add `Dots::Presentation` for render-data extraction, Dots shaders, circle pipelines, and
  the background/grid pass.
- Render instanced circles through `Dots::Presentation` and `MyCore::Render`.
- Keep renderer separate from simulation ownership.
- Keep circle, food, and Dots grid concepts out of `MyCore::Render`.
- Add minimal game-neutral shader loading through `MyCore::Assets`; keep Dots assets under
  `games/dots/assets`.

Tests:

- Unit test Dots render-data extraction from Dots simulation state.
- Add render API construction tests only where stable and not overly mocked.

Exit criterion:

- `dots_client` renders player and food circles through Dots presentation layered on the
  game-neutral engine render API.

### `feature/07-debug-observability`

Purpose: make behavior inspectable before networking.

Changes:

- Add `spdlog` and `fmt`.
- Add `MyCore::Debug` for game-neutral logging setup, frame metrics, and profiler hooks.
- Add Dear ImGui dependency and a Dots-owned debug overlay.
- Show tick, entity counts, frame timing, and grid stats.
- Add logging categories qualified by owner: Dots client/server/simulation/protocol and
  MyCore platform/render/transport.
- Keep Dots entity, grid, and networking panels out of `MyCore::Debug`.

Tests:

- Unit test metrics aggregation without UI.
- Unit test formatting for structured IDs where useful.

Exit criterion:

- Offline `dots_client` exposes enough debug data to understand simulation behavior.

### `feature/08-protocol-binary-codec`

Purpose: define hostile-input-safe wire messages before sockets.

Changes:

- Add `Dots::Protocol` under `games/dots/protocol`.
- Define concrete Dots client, snapshot, and input-sequence IDs in this domain rather than
  in `MyCore::Core`.
- Implement explicit encoders and decoders for protocol version, handshake, input command, and full snapshot.
- Use defined byte order, explicit widths, validation, and no `memcpy` struct serialization.
- Add packet size budget constants.

Tests:

- Round-trip encode/decode tests for every message.
- Golden packet tests for representative input and snapshot messages.
- Malformed decode tests for truncation, invalid enum, wrong version, and out-of-range fields.

Exit criterion:

- Dots protocol messages are fully testable without any network transport and no Dots wire
  type is exposed by an engine target.

### `feature/09-inmemory-transport-integration`

Purpose: prove authoritative client/server flow without external networking.

Changes:

- Add a game-neutral in-memory transport implementation for deterministic tests.
- Add the headless `dots_server` loop using `Dots::Simulation` and a Dots-owned server
  runtime library.
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

- The authoritative Dots server/client protocol works over in-memory transport while the
  transport remains unaware of Dots messages.

### `feature/10-gamenetworkingsockets-transport`

Purpose: adopt the real native networking transport.

Changes:

- Add `gamenetworkingsockets` dependency.
- Implement the `MyCore::NetTransport` wrapper under `engine/net_transport`.
- Define transport connection handles in the transport module, not in Core or Dots.
- Add connection lifecycle handling.
- Use reliable control messages and unreliable input/snapshot messages.
- Preserve the transport interface used by in-memory tests.
- Add the developer-only `games/dots/tools/dots_session.py` launcher. It accepts an explicit
  build directory and client count, starts `dots_server`, waits for a readiness signal,
  launches clients, prefixes child output, and shuts every child down on interruption or
  failure.

Tests:

- Loopback client/server integration test.
- Handshake success and failure tests.
- Packet validation tests through the transport boundary.
- Keep all in-memory integration tests running.
- Launcher smoke test with a short-lived server/client fixture; process failures propagate
  through the launcher's exit code and no child is left running.

Exit criterion:

- Two real `dots_client` processes can connect to a headless `dots_server` and see
  authoritative movement.

### `feature/11-prediction-reconciliation`

Purpose: make local movement responsive under latency.

Changes:

- Add Dots predicted local state separate from Dots replicated state.
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

- Add a Dots presentation snapshot buffer for remote entities.
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

- Reuse the Dots simulation uniform grid for AOI queries.
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

- Implement `dots_bot` under `games/dots/apps/bot`.
- Add configurable bot count, input pattern, spawn behavior, and connection ramp.
- Add metrics output for CPU, bandwidth, tick time, queue depth, and disconnects.
- Add benchmark targets for grid, world stepping, AOI, and snapshot build.
- Extend `dots_session.py` with bot count, connection ramp, input pattern, metrics directory,
  and optional client count. Keep load-test policy in the launcher rather than CMake custom
  targets.

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
- Add the game-neutral `MyCore::Scripting` RAII Lua host wrapper.
- Add Dots-owned scripting bindings that expose a capability-oriented rules API only.
- Support tick-boundary reload.
- Keep movement, collision, prediction, serialization, and hot loops in C++.

Tests:

- Load valid script.
- Reject invalid script with useful error.
- Dots script bindings can configure spawn and match rules.
- Reload occurs only at tick boundary.
- Script cannot access raw engine objects or arbitrary filesystem APIs.

Exit criterion:

- Dots match and spawn rules can be changed through Lua without destabilizing simulation,
  and `MyCore::Scripting` contains no Dots capabilities or types.

### `feature/17-aim-trainer-3d-slice`

Purpose: validate that the engine boundaries support a substantially different game without
generalizing Dots systems.

Changes:

- Add an offline desktop game under `games/aim_trainer` with its own simulation/state,
  presentation, assets, tests, and `aim_trainer` executable composition root.
- Reuse `MyCore::Core`, `MyCore::Math`, `MyCore::Time`, `MyCore::PlatformSDL`,
  `MyCore::Render`, `MyCore::Assets`, and `MyCore::Debug` without depending on any Dots
  target or header.
- Expand `MyCore::Math` only with the required 3D vectors, matrices, quaternions,
  transforms, rays, and bounds.
- Expand `MyCore::Render` only with the required depth attachment, vertex/index mesh,
  perspective transform, and material facilities.
- Add aim-trainer-owned perspective camera, mouse look, target storage, spawning, ray-hit
  rules, scoring, reset behavior, shaders, and render-data extraction.
- Do not add networking, a general physics engine, a generic scene graph, or a universal
  ECS.

Tests:

- 3D math operations used by the camera and ray queries.
- Camera orientation and projection behavior.
- Ray hit, miss, nearest-target, and boundary cases.
- Target spawn, score, and reset rules without SDL or rendering.
- Render-data extraction for a known target state.
- CMake dependency check or consumer smoke proving no Dots target is linked.

Exit criterion:

- The offline aim trainer opens a 3D world, supports mouse look, renders depth-tested mesh
  targets, registers ray-based hits, and tracks score while reusing engine libraries and no
  Dots code.

### `feature/18-cmake-package-consumer`

Purpose: make stabilized MyCore engine libraries consumable outside the monorepo.

Changes:

- Add install rules for public engine headers and reusable engine targets only.
- Export `MyCoreConfig.cmake`, version metadata, and component targets with stable
  `MyCore::` names.
- Preserve source-tree consumption through `add_subdirectory`.
- Add a separate minimal consumer fixture that uses
  `find_package(MyCore CONFIG REQUIRED COMPONENTS Core Math Time)` against a staged
  install.
- Do not install Dots or aim-trainer targets as part of the engine package by default.

Tests:

- Configure, build, install, and consume MyCore from a clean staging prefix.
- Link and run the external consumer against requested components.
- Reject or clearly diagnose unavailable components.
- Verify installed headers do not reference game-owned headers or build-tree paths.

Exit criterion:

- A separate CMake project can install and consume selected MyCore components without
  accessing the MyCore source tree.

## Research Branches

Create these only after the bot/load harness or Lua rules branch provides a stable
comparison point. The aim-trainer and package branches are validation work, not research
branches.

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

- `MyCore::Core`: minimal platform-free and game-neutral primitives; generic mechanisms,
  not concrete game or protocol IDs.
- `MyCore::Math`: small owned math types and operations added only for demonstrated needs.
- `MyCore::Time`: monotonic tick/duration helpers and fixed-step accumulation without a game
  tick rate or simulation policy.
- `MyCore::PlatformSDL`: SDL lifetime, windows, events, and raw device state; no game input
  commands.
- `MyCore::Render`: SDL_GPU-backed resources and command submission; no Dots circles,
  aim-trainer targets, or simulation ownership.
- `MyCore::Assets`: game-neutral asset lookup and byte loading; no Dots or aim-trainer asset
  schema.
- `MyCore::Debug`: game-neutral logging, metrics primitives, and profiler hooks; no
  game-specific panels or state.
- `MyCore::NetTransport`: connections and byte payload transport; no gameplay messages or
  replication policy.
- `MyCore::Scripting`: Lua VM lifetime and safe calls; no game capabilities.
- `Dots::Simulation`: authoritative Dots state and fixed-step rules; depends only on Core
  and Math.
- `Dots::Protocol`: Dots wire messages and concrete protocol IDs, independent of C++ memory
  layout and transport implementation.
- `Dots::Replication`: client-specific Dots snapshot construction.
- `Dots::Presentation`: Dots render extraction, shaders, and pipelines.
- `dots_server`: headless Dots authority; must not link renderer, SDL video, presentation,
  or ImGui rendering.
- `dots_client`: Dots presentation, input mapping, prediction, reconciliation, and runtime
  composition.
- `dots_bot`: Dots protocol/transport-driven load harness.
- `games/dots/tools/dots_session.py`: developer-only orchestration of built Dots processes;
  contains no simulation, protocol, or transport implementation.
- `AimTrainer::*`: aim-trainer state and presentation; may use MyCore targets but no Dots
  target.

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
- Package the Linux `dots_server`.
- After feature 18, stage-install MyCore and build the external consumer fixture.
- Store logs and crash artifacts.
- Cache vcpkg binary packages.

## Assumptions

- Initial development can happen on macOS, but CMake must keep Windows and Linux compatibility from the start.
- vcpkg manifest mode is the only dependency manager for mainline.
- SDL_GPU is the primary renderer, though one temporary simple SDL visualization branch is acceptable before the render layer.
- Dots is the only gameplay implementation until its multiplayer and scripting roadmap is
  complete.
- The first aim trainer is offline desktop-only and validates reuse after the engine
  platform and rendering boundaries exist.
- MyCore owns a small requirement-driven math library; it does not expose GLM types in its
  public API.
- Stable `MyCore::` targets and include roots are established before packaging, but
  installation is deferred until feature 18.
- Lua, EnTT, Vulkan, Conan, OpenGL, and fixed-point work are deferred until the baseline game loop and networking path are measurable.
- The first server architecture uses one owner thread for world simulation.
- Snapshot worker parallelism is introduced only after immutable replication views exist.
- The 1,000-client target is validated through staged bot milestones: 10, 100, 500, and 1,000.
