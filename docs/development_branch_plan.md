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

- Follow the current delivery sequence below. Feature identifiers remain stable historical names
  and are not required to merge in numeric order.
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
- When a detailed plan contains approval checkpoints, complete and review one checkpoint at a
  time; do not begin the next checkpoint from an unchecked approval marker.

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

## Current Status and Delivery Sequence

`main` is implemented through Feature 14, including authoritative spawn search, complete-World
rollback, input flow control, direct spectators, and the measured decision to retain same-frame
replay. Feature 17 is partially implemented: `dots_bot`, the multi-process session launcher,
player/spectator counts, impairments, bounded sessions, and child cleanup exist, but scale-grade
connection ramping, configurable patterns, metrics artifacts, benchmarks, and 100--1,000-client
reporting do not.

The next delivery sequence is evidence-first:

1. `chore/validation-baseline` adds the sanitizer, protocol-fuzzing, and rollback correctness gate.
2. `feature/24-authoritative-input-provenance` makes the existing scheduler measurable without
   changing it.
3. `feature/17-bot-load-harness` completes the harness and records a full-snapshot baseline.
4. `research/multi-rate-simulation-cadence` compares the current 30 Hz World step with measured
   60, 120, and 240 Hz internal substeps while keeping gameplay publication and networking at
   their current cadences. It may conclude that 30 Hz plus presentation interpolation is correct.
5. `feature/15-interest-management` adds AOI filtering and repeats the same scale matrix.
6. `feature/16-delta-snapshots-byte-budget` adds scalable snapshots and repeats the matrix again.
7. `feature/18-lua-rules`, `feature/22-platform-user-settings`, and
   `feature/23-client-input-observability` finish the current Dots and desktop-platform work.
8. `feature/19-aim-trainer-3d-slice` validates in-tree reuse, followed by
   `feature/20-cmake-package-consumer` and `chore/linux-server-package`.

Evaluate `feature/21-profile-guided-task-scheduler` after Feature 16 and again after Feature 19,
but activate it only when its measured entry criteria are satisfied. Research branches remain
parked until they have stable workloads and a concrete comparison question.

## Branch Roadmap

### `chore/validation-baseline`

Purpose: close the sanitizer, hostile-input fuzzing, and prediction-kernel correctness
commitments before replication-scale work changes the protocol and server boundary.

Changes:

- Add a target-scoped Linux Clang AddressSanitizer plus UndefinedBehaviorSanitizer preset.
- Run the complete test suite under that preset in required CI.
- Add a Clang/libFuzzer target that passes arbitrary bytes to the real Dots protocol decoder.
- Audit rollback invariants, Dots scope closure, client lifecycle boundaries, and borrowed views.
- Add a structured rollback libFuzzer target and required bounded CI smoke.
- Add representative framing seeds, a protocol-aware dictionary, and a bounded CI smoke run.
- Keep ThreadSanitizer deferred until project-owned concurrent execution exists; keep performance
  benchmarks in Feature 17 and Linux server packaging in its later release branch.

Tests:

- Normal host Debug builds and tests remain unchanged.
- The sanitizer preset builds and runs the full suite without findings.
- Both fuzzer targets build independently and complete their bounded smoke corpora without a crash.
- Unsupported sanitizer/fuzzer compiler requests fail during configuration.

Exit criterion:

- ASan/UBSan and protocol fuzz smoke are reproducible locally and required by CI before Features
  24, 17, 15, and 16 merge.

Detailed plan:
[`plans/validation-baseline.md`](plans/validation-baseline.md) and
[`plans/rollback-correctness-audit.md`](plans/rollback-correctness-audit.md).

### `feature/00-foundation`

Purpose: create the empty buildable project.

Changes:

- Add minimal multi-game monorepo layout: `engine/`, `games/dots/`, `tests/`, `tools/`,
  `cmake/`, and `docs/`.
- Add benchmark and fuzz directories only with the first real target, beside the module or
  game that owns it.
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
- Add game-neutral `MyCore::Render2D` with a transient camera/grid/circle draw list and
  engine-owned built-in shaders, pipelines, and batching.
- Add game-neutral `MyCore::Assets` interfaces for locating and reading asset bytes;
  keep game-specific formats and asset manifests game-owned.
- Add `Dots::Presentation` for render-data extraction and pure conversion from Dots meaning to
  a generic Render2D draw list.
- Render instanced circles through `MyCore::Render2D` and `MyCore::Render`.
- Keep renderer separate from simulation ownership.
- Keep food and player concepts out of engine targets; circles and grids are reusable Render2D
  primitives and remain out of the low-level `MyCore::Render` target.
- Add minimal game-neutral shader loading through `MyCore::Assets`; stage engine built-ins
  under `assets/mycore/render_2d`.
- Add a verified `dots_client_package` CMake target with exact target-owned shader assets, a
  macOS `.app` or flat Windows/Linux bundle, checksums, and short-lived per-platform CI
  artifacts. Keep this playable-game bundle separate from the Feature 20 engine SDK package.

Tests:

- Unit test Dots render-data extraction from Dots simulation state.
- Add render API construction tests only where stable and not overly mocked.
- Extract the generated client archive, verify its exact shader set, and run the packaged
  `--help` and GPU-free `--package-smoke` paths.

Exit criterion:

- `dots_client` converts player and food state into a generic draw list rendered through the
  engine-owned Render2D layer, and each platform produces a verified relocatable client archive.

### `feature/07-debug-observability`

Purpose: make behavior inspectable before networking.

Changes:

- Add `spdlog`, `fmt`, Tracy, and Dear ImGui with its SDL3/SDL_GPU backends.
- Add `MyCore::Debug` for game-neutral logging setup, frame metrics, and profiler hooks.
- Add client-only `MyCore::DebugUI` for Dear ImGui lifetime and backend integration.
- Add a Dots-owned overlay showing input mode, tick, entity counts, simulation steps, frame
  timing, and occupied spatial-grid cells.
- Measure actual tick rate, simulation cost, retained backlog, deadline misses, cap hits, and
  discarded time; emit rate-limited overload and recovery logs.
- Keep the offline client responsive by explicitly recording and dropping excess whole-step
  backlog after its catch-up cap, while preserving fractional interpolation time.
- Present the local player and its following camera at the same interpolated position.
- Let SDL input polling forward drained events to observers and let Render2D append a generic
  same-frame pass without making either layer ImGui-specific.
- Add logging categories qualified by owner: Dots client/server/simulation/protocol and
  MyCore platform/render/transport.
- Keep Dots entity, grid, and networking panels out of `MyCore::Debug`.

Tests:

- Unit test metrics aggregation without UI.
- Unit test SDL event observation independently of Dots.
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
- Expose a game-neutral transport-statistics snapshot for connection state, RTT, packet loss,
  byte/packet rates, and reliable/outbound queue depth where the native backend provides them.
- Extend the Dots debug overlay with native transport health plus replication-level snapshot age
  and receive rate. Keep those categories labeled separately, and do not fabricate ping, loss,
  or queue measurements for the deterministic in-memory backend.
- Add the developer-only `games/dots/tools/dots_session.py` launcher. It accepts an explicit
  build directory and client count, starts `dots_server`, waits for a readiness signal,
  launches clients, prefixes child output, and shuts every child down on interruption or
  failure.

Tests:

- Loopback client/server integration test.
- Handshake success and failure tests.
- Packet validation tests through the transport boundary.
- Transport-statistics mapping tests cover unavailable values, counter/rate updates, and
  connection-state transitions without requiring Dear ImGui in the test target.
- Loopback impairment tests confirm that the debug metrics reflect simulated latency and loss
  while the session remains authoritative.
- Keep all in-memory integration tests running.
- Launcher smoke test with a short-lived server/client fixture; process failures propagate
  through the launcher's exit code and no child is left running.

Exit criterion:

- Two real `dots_client` processes can connect to a headless `dots_server`, see authoritative
  movement, and inspect truthful connection, transport, and snapshot-flow health.

### `feature/11-prediction-reconciliation`

Purpose: make local movement responsive under latency.

Changes:

- Add Dots predicted local state separate from Dots replicated state.
- Add input history ring buffer.
- Include `last_processed_input` in snapshots.
- Reconcile by installing authoritative state and replaying unacknowledged input.
- Smooth presentation only; never smooth authoritative simulation state.
- Track and display last input sent/acknowledged, unacknowledged input count, replay count,
  correction distance and frequency, and the remaining presentation-smoothing offset.

Tests:

- Prediction applies input immediately.
- Reconciliation replays unacknowledged commands.
- Dropped or delayed snapshots do not corrupt simulation state.
- Deterministic correction cases verify the prediction metrics, including a matching prediction
  that records no correction and a mismatch that records replay and correction magnitude.
- Artificial `100-200 ms` latency test remains playable in loopback or in-memory mode.

Exit criterion:

- Local player movement remains responsive and corrects cleanly under simulated latency, with
  enough input-acknowledgement and correction telemetry to explain each reconciliation.

### `feature/12-remote-interpolation`

Purpose: smooth presentation for non-owned entities.

Changes:

- Add a Dots presentation snapshot buffer for remote entities.
- Interpolate presentation state with a two-to-three snapshot interval delay.
- Track jitter and buffer fill metrics.
- Display snapshot-buffer fill, target interpolation delay, measured jitter, late-snapshot count,
  and buffer-underrun/hold count without treating presentation delay as transport RTT.
- Keep remote entities non-predicted.

Tests:

- Interpolation between known snapshots.
- Jitter/loss simulation without presentation state explosions.
- Deterministic arrival schedules verify buffer-fill, late-snapshot, jitter, and underrun/hold
  metrics.
- Remote entity create, update, and remove behavior.

Exit criterion:

- Remote players render smoothly under simulated jitter and packet loss, and the interpolation
  panel explains buffer health and any visible holds.

### `feature/13-authoritative-interactions-spectating`

Purpose: exercise contested server authority and session lifecycle before optimizing replication.

Changes:

- Add deterministic larger-player absorption, mass transfer, and player ownership.
- Keep defeated connections alive in a replicated spectator session state.
- Add free-camera and follow-killer spectator presentation.
- Add optional server-authorized respawn after a configurable tick deadline.
- Add Gameplay debugging for absorption, killer, session mode, and respawn eligibility.

Tests:

- Deterministic overlap arbitration, mass conservation, and player/food ordering.
- Playing, spectating, early respawn rejection, eligible respawn, and disconnect lifecycle.
- Snapshot loss cannot lose durable session state.
- Spectator pan, zoom, follow-target loss, and camera transitions.

Exit criterion:

- The server alone resolves absorption, defeat, and respawn while defeated clients can continue
  spectating and debug output explains every transition.

Detailed plan:
[`plans/13-authoritative-interactions-spectating.md`](plans/13-authoritative-interactions-spectating.md).

### `feature/13a-authoritative-spawn-search`

Purpose: retain deterministic collision-safe authoritative placement without repeatedly scanning
every normally occupied spawn candidate from the origin.

Changes:

- Add an allocation-free spatial-grid traversal that exits on the first exact player collision.
- Classify a spawn candidate once as clear, blocked, or outside the representable grid.
- Replace the origin restart with a directly indexed compact lattice sequence beginning at the
  active player count.
- Keep food non-blocking, preserve distinct failure causes, and add no mutable cursor or random
  state that Feature 14 would need to checkpoint.

Tests:

- Golden indexed-ring coordinates and deterministic World-to-World placement.
- Live-radius collision checks after movement, growth, removal, and respawn.
- A 1,000-player fresh-World test with linear candidate-classification counts.
- Entity-ID exhaustion, spatial-bound failure, and deterministic simultaneous request ordering.

Exit criterion:

- Join and respawn placement remains server-owned, deterministic, unbounded, and exactly
  collision-safe while the normal fresh-World batch path performs one successful candidate
  classification per player.

Detailed plan:
[`plans/authoritative-spawn-search.md`](plans/authoritative-spawn-search.md).

### `feature/14-selectable-world-rollback`

Purpose: replace position-only prediction with a game-neutral rollback timeline and a complete,
interaction-closed predicted Dots World.

Changes:

- Add the statically typed `MyCore::Rollback` mechanism and Dots-owned model defined in
  [`rollback_prediction_design.md`](rollback_prediction_design.md).
- Restore and resimulate complete gameplay state with an interaction-closure default,
  full-replicated oracle/benchmark, and owner-local gameplay fallback.
- Treat closure as causal state membership, including owner-local cooldown/piece-count state and
  required global domains/authority facts rather than only spatial entities.
- Add predicted split/launch/remerge, cooldowns, and predicted-spawn classification.
- Add typed event journals and post-commit `PredictOnce`, `PredictCancelable`, and `ConfirmOnce`
  consequence delivery.
- Compose predicted closure state with bounded presentation-only remote extrapolation while
  retaining Feature 12 interpolation as fallback and comparison.
- Add adaptive command-buffer timing plus Rollback metrics, overlays, and deliberate faults.
- Add protocol receive-window flow control, retained unsent commands, hysteresis pause/resume,
  and first-class direct spectator stress roles.
- Keep outer sent/retained command frontiers distinct from timeline submission so speculative
  local elimination can recover a validated ACK gap and roll the remaining suffix forward.

Tests:

- A non-Dots toy model proving generic history, replay, atomic commit, and consequence behavior.
- Matching/mismatching continuous and structural Dots rollback, non-spatial owner/global
  dependencies, and closure fallback.
- Accepted/rejected predicted spawns, stable event identity, every consequence policy, and
  guarded session state.
- Dynamic latency, loss, reordering, queue-depth convergence, and hard resync.
- Authority-stall pause/drain with a buffered edge action, malicious out-of-window isolation, and
  direct-spectator heartbeat liveness.
- Recorded 10, 100, 500, and 1,000-entity replay workloads.

Exit criterion:

- Complete interaction-closed Dots gameplay converges atomically to server truth, Dots exercises
  every generic consequence policy, and `MyCore::Rollback` contains no game or protocol policy.

Implementation status: steps 1 through 9 are merged into `main`. Adaptive command timing,
expanded fault/diagnostic coverage, bounded router-ledger pruning, explicit correction
generations, measured scale workloads, bounded native soaks, receive-window overload recovery,
and direct spectator stress modes are implemented. The optimized
target 200 ms, 1,000-entity workload measured 0.738 ms p99 with 0% rollback-only 30 Hz frame
overruns, so the conditional multi-frame spike was not activated. The evidence is recorded in
[`feature14_rollback_workload_results.md`](feature14_rollback_workload_results.md).

Feature 14 was implemented incrementally beginning with the game-neutral rollback kernel. Its
commits remained independently reviewable without separate branches or approval gates for every
implementation step.

Detailed plan:
[`plans/14-selectable-world-rollback.md`](plans/14-selectable-world-rollback.md). The step 9
hardening record is
[`plans/14-input-flow-control-spectator-stress.md`](plans/14-input-flow-control-spectator-stress.md).

### `spike/multi-frame-resimulation` — conditional

Purpose: evaluate time-sliced scratch replay only if Feature 14's optimized same-frame baseline
exceeds 4 ms at p99 or causes rollback-attributable frame overruns in more than 1% of
reconciliations in the target 200 ms impairment scenario.

Do not create this branch from anticipated scale alone. The spike must use the recorded Feature
14 workloads, keep presenting immutable committed state, accept commands while catching up,
supersede work for newer authority, atomically swap only after reaching its moving target, and
hard-resync before history exhaustion.

Status: not activated by Feature 14. The recorded target workload remained below both thresholds.

Exit criterion:

- A measured comparison either rejects multi-frame replay or produces a separately reviewed
  production feature plan; spike code does not merge as an unreviewed second scheduler.

### `feature/15-interest-management`

Purpose: reduce replication to each client's area of interest.

Changes:

- Reuse the Dots simulation uniform grid for AOI queries.
- Add per-client camera/interest rectangle with margin.
- Build full snapshots using AOI filtering.
- Add debug visualization for AOI and replicated entity count.
- Adapt Feature 14 prediction-set membership to AOI entry/exit, collision safety margins, and
  atomic initialization/removal of predicted entities.
- Add a server-derived, epoch-versioned event-interest subscription covering owned events and
  selected visible remote event categories. Change AOI membership and event subscription
  atomically, and retain in-flight receipts across an epoch transition.

Tests:

- AOI includes visible and margin entities.
- AOI excludes distant entities.
- Owned entity always has highest priority.
- Snapshot entity counts shrink when entities exist outside view.
- Event receipts follow the server-derived interest epoch without gaps or delivery outside the
  subscribed owned/AOI set.

Exit criterion:

- Clients receive only relevant entities plus required owned state.

### `feature/16-delta-snapshots-byte-budget`

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

### `feature/17-bot-load-harness`

Purpose: measure scalability instead of guessing.

Implementation status: partially implemented by Features 10 and 14. The native headless bot,
launcher bot counts, direct spectator roles, impairment controls, bounded duration, prefixed
output, exit propagation, and child cleanup already exist. This feature completes scale-oriented
connection generation, workload control, metrics artifacts, benchmarks, and staged reporting;
it must preserve rather than recreate the existing pieces.

Changes:

- Retain `dots_bot` under `games/dots/apps/bot` as the headless load-client composition root.
- Extend `dots_bot` to multiplex or shard logical sessions so the 1,000-client workload does not
  require one operating-system process per simulated connection.
- Add configurable bot count, input pattern, spawn behavior, and connection ramp.
- Add metrics output for CPU, bandwidth, tick time, queue depth, and disconnects.
- Write versioned machine-readable run summaries and time series under the selected metrics
  directory, including Feature 24 input-provenance distributions when enabled.
- Add benchmark targets for grid, world stepping, AOI, and snapshot build.
- Extend `dots_session.py` with bot count, connection ramp, input pattern, metrics directory,
  and optional client count. Keep load-test policy in the launcher rather than CMake custom
  targets.

Tests:

- 10-bot automated integration test in CI-friendly mode.
- Local/manual scenarios for 100, 500, and 1,000 clients.
- Benchmark targets compile and run.
- Record the pre-AOI full-snapshot capacity or failure threshold, then rerun the identical matrix
  after Features 15 and 16.

Exit criterion:

- The project can produce repeatable load metrics and reach staged bot counts locally.

### `feature/18-lua-rules`

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

### `feature/19-aim-trainer-3d-slice`

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
- If the demonstrated material set needs shader variants, add a platform-neutral shader
  manifest, reflected offline cooker output, runtime `ShaderLibrary`, and `PipelineCache`.
  Otherwise retain the simpler target-owned loose shader outputs.
- Add aim-trainer-owned perspective camera, mouse look, target storage, spawning, ray-hit
  rules, scoring, reset behavior, shaders, and render-data extraction.
- Start with a transient aim-trainer render snapshot. Add a persistent client-only render world
  only if culling, LOD, interpolation, or render-thread ownership demonstrates the need.
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

### `feature/20-cmake-package-consumer`

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

### `chore/linux-server-package`

Purpose: fulfill the deferred CI/release commitment for a verified headless Linux server artifact
after scalable replication has stabilized.

Changes:

- Install only `dots_server` and its required runtime libraries into a Linux server component.
- Produce a versioned archive independently from the Dots client and MyCore SDK packages.
- Extract the archive in CI and run `--help` plus a bounded dynamic-port tick smoke test.

Exit criterion:

- CI retains a verified relocatable Linux `dots_server` archive without renderer, SDL video,
  client assets, Dots client configuration, or engine development headers.

### `feature/21-profile-guided-task-scheduler`

Purpose: adopt task scheduling only after networking, replication, the bot harness, and Tracy
produce measured parallel workloads.

Entry criteria:

- A recorded 100-1,000 client or second-game workload misses a CPU budget.
- Profiling identifies independent bounded work such as snapshot encoding, visibility/culling,
  animation, asset decoding, or render-command preparation.
- The work consumes immutable inputs or has explicit ownership and join points.

Changes:

- Benchmark a small proven task library against a minimal fixed worker pool before selecting
  one; do not implement a general work-stealing runtime by default.
- Add a narrow `MyCore::Tasks` facade for task groups, dependencies/fences, worker limits,
  profiler labels, error propagation, and cooperative shutdown.
- Keep the authoritative fixed tick on one owner thread. It may dispatch deterministic work and
  join it before state publication, but the scheduler does not define game-tick ordering.
- Keep network polling, SDL window/event ownership, GPU submission, and the real-time audio
  callback on their appropriate threads. Schedule only bounded processing around them.
- If render-command preparation becomes a scheduled workload, its only game-state input is the
  immutable game-owned presentation snapshot. Render tasks and camera code do not borrow the
  mutable simulation, replication, or prediction worlds.
- Preserve a deterministic single-threaded execution mode for tests and replay comparisons.
- Introduce one measured workload first, preferably immutable per-client snapshot construction;
  add render preparation or other consumers only after that boundary is proven.

Tests:

- Task dependencies, completion, error propagation, shutdown, and worker-count limits.
- No task outlives referenced state; add ThreadSanitizer coverage where supported.
- Single-threaded and scheduled modes produce identical snapshot bytes or render preparation
  output for a recorded workload.
- Scheduled render preparation and its single-threaded equivalent consume the same immutable
  presentation snapshot and produce identical camera and entity transforms.
- Benchmarks show a useful improvement without causing server tick-tail regressions.

Exit criterion:

- A measured workload improves while authoritative tick ordering, deterministic tests, and
  subsystem thread-affinity rules remain explicit.

### `feature/22-platform-user-settings`

Purpose: let packaged games find user-editable configuration and other writable files in the
locations expected by each desktop OS instead of depending on a Finder- or shell-selected
working directory.

Changes:

- Add a game-neutral `MyCore::PlatformPaths` facility that accepts an application identity and
  reports separate configuration, persistent-data, cache, and log directories without
  initializing SDL video.
- Follow native conventions: `~/Library/Application Support/<vendor>/<game>` on macOS,
  `%LOCALAPPDATA%\\<vendor>\\<game>` on Windows, and
  `$XDG_CONFIG_HOME/<vendor>/<game>` with the `~/.config` fallback on Linux. Keep data and cache
  paths distinct where the platform provides distinct conventions.
- Keep path discovery in the engine, but keep filenames, TOML schemas, defaults, parsing, and
  validation game-owned.
- For Dots configuration, use this source precedence from highest to lowest: explicit
  `--config`, current-directory developer/portable override, per-user OS configuration, then
  built-in defaults. The example inside the application bundle remains read-only documentation
  and is never silently edited.
- Make directory creation an explicit write operation. Merely querying paths or starting with
  built-in defaults must not modify the user's filesystem.

Tests:

- Inject home/environment/known-folder results and verify paths for macOS, Windows, and Linux
  without reading or writing the developer's real profile.
- Verify the Dots source precedence, missing optional files, explicit missing-file errors, and
  paths containing spaces and non-ASCII characters.
- Package smoke tests continue to work without a user configuration, while a launched package
  can load a configuration from an injected per-user directory.

Exit criterion:

- Double-clicking or launching an installed client can discover per-user configuration in the
  native OS location, while command-line overrides and game-owned validation retain their
  current behavior.

### `feature/23-client-input-observability`

Purpose: make the Dots client's active configured controls and live local input context visible
without confusing device state with authoritative gameplay input.

Changes:

- Add a Dots-owned input view that displays the configured input mode and resolved keyboard and
  mouse bindings for the current Playing or Spectating context.
- Show currently held movement keys, edge-triggered actions, wheel activity, derived movement
  intent, and whether debug-UI capture suppressed mouse steering or spectator wheel zoom.
- Keep the view client-only. Do not add protocol fields, transmit raw device state, or describe
  spectator camera input as authoritative gameplay.
- Keep input remapping and binding persistence out of scope; this branch observes the existing
  configuration.
- Choose during detailed planning between a **Dots session / Input** tab and a separately
  toggleable compact streamer-style overlay. Use one presentation first rather than maintaining
  duplicate views.

Tests:

- Build the displayed binding and action state from real `ClientConfig`, input snapshots, and
  control intents.
- Cover Playing and Spectating contexts, held versus edge-triggered actions, mouse/wheel input,
  and debug-UI capture.
- Verify the view does not change generated gameplay commands, spectator controls, or protocol
  bytes.

Exit criterion:

- A developer can identify the active local bindings, device activity, derived intent, and UI
  capture state in-game without consulting the TOML file or mistaking the display for server
  authority.

### `feature/24-authoritative-input-provenance`

Purpose: make the current arrival-based authoritative input scheduler directly observable without
turning client-local ticks into trusted server timestamps or changing gameplay scheduling.

Implementation status: implemented on `feature/24-authoritative-input-provenance`. The runtime
captures bounded first-receipt/application records and exact cumulative distributions; the server
and session launcher expose paired, explicitly capped JSONL trace options plus rate-limited
aggregate logs. Host format, clang-tidy, build, full CTest, capped-artifact, and impaired native
trace validation pass; CI remains the merge gate.

Changes:

- Add a bounded, opt-in server diagnostic record for accepted Player inputs containing client ID,
  input sequence, protocol `client_tick`, receive-side server tick, authoritative application
  tick, and queue wait in ticks.
- Preserve current scheduling: each server tick consumes at most the oldest queued sequence per
  session. Diagnostics must not backdate commands, alter ordering, or implement lag compensation.
- Emit a schema-versioned, explicitly capped developer JSONL trace and rate-limited aggregate
  server logs. Do not add a replicated per-input stream to every snapshot without measured need.
- Summarize counts and queue-wait distributions for many-client soaks so opt-in diagnostics do
  not flood normal logs.
- Cross-reference the protocol/networking time model and clearly label `client_tick` as local
  provenance rather than an authoritative clock.

Tests:

- Controlled arrival schedules prove exact receive/application ticks and queue waits for two
  clients with different delays, including both commands applied in one shared server tick.
- Loss, redundancy, reordering, and duplicate packets produce one application record per
  sequence without changing authoritative results.
- Diagnostics remain bounded and disabled by default; enabling them produces byte-identical
  authoritative snapshots and checkpoints for the same input schedule.

Exit criterion:

- A developer can answer when a sequenced input arrived and when authority applied it without
  subtracting client and server clocks or mistaking the trace for a server-rewind policy.

Detailed plan: [`plans/24-authoritative-input-provenance.md`](plans/24-authoritative-input-provenance.md).

Delivery note: implement this feature immediately after `chore/validation-baseline` so Feature
17's first scale baseline includes exact, bounded scheduler and queue-wait distributions.

### `feature/portable-replay-artifact` — conditional

Purpose: add a versioned persistent replay artifact only when cross-version debugging, external
reproduction, or another concrete consumer requires more than the current deterministic workload
fixtures.

Do not activate this feature merely because replay is useful internally. A detailed plan must
first name the producer and consumer, compatibility window, authoritative inputs/checkpoints,
and migration or rejection behavior. Until then, code-owned recorded workloads remain the
regression and benchmark mechanism.

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

### `research/multi-rate-simulation-cadence`

Compare the current 30 Hz Dots World step with two 60 Hz, four 120 Hz, and eight 240 Hz fixed
physics substeps inside each 30 Hz gameplay tick. Keep client input at 30 Hz, snapshots at 15 Hz,
and rendering variable-rate during the comparison.

Measure:

- Collision tunneling and contested-contact outcomes against a high-resolution oracle.
- Authoritative tick and prediction-replay mean, p95, p99, and deadline misses.
- Deterministic checkpoint, journal, receipt, and consequence equality across repeated runs.
- The event-identity and rule-ordering cost of resolving contacts inside a gameplay tick.
- Whether an immutable presentation snapshot prevents high-refresh motion defects without a
  higher simulation cadence.

Use integer substeps on the authoritative owner thread; do not prototype an independent physics
thread. Select the lowest cadence that fixes a demonstrated correctness problem within the
Feature 17 budgets, or retain 30 Hz when presentation interpolation is sufficient.

Detailed plan:
[`plans/multi-rate-simulation-presentation.md`](plans/multi-rate-simulation-presentation.md).

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
- `MyCore::PlatformPaths`: platform-native user config/data/cache/log locations; no game
  filenames, schemas, parsing, or implicit filesystem writes.
- `MyCore::Render`: SDL_GPU-backed resources and command submission; no Dots circles,
  aim-trainer targets, or simulation ownership.
- `MyCore::Assets`: game-neutral asset lookup and byte loading; no Dots or aim-trainer asset
  schema.
- `MyCore::Debug`: game-neutral logging, metrics primitives, and profiler hooks; no
  game-specific panels or state.
- `MyCore::DebugUI`: client-only Dear ImGui lifetime and SDL3/SDL_GPU backend integration; no
  game-specific panels or state.
- `MyCore::NetTransport`: connections and byte payload transport; no gameplay messages or
  replication policy.
- `MyCore::Rollback`: typed game-neutral checkpoint/step history, atomic reconciliation, event
  transitions, and consequence-delivery bookkeeping; no Dots state, mechanics, protocol,
  presentation, or audio policy.
- `MyCore::Tasks`: conditional, profile-driven bounded CPU task groups and fences; no game tick,
  render-submission, network-poll, or audio-callback ownership policy.
- `MyCore::Scripting`: Lua VM lifetime and safe calls; no game capabilities.
- `Dots::Simulation`: authoritative Dots state and fixed-step rules; depends only on Core
  and Math.
- `Dots::Protocol`: Dots wire messages and concrete protocol IDs, independent of C++ memory
  layout and transport implementation.
- `Dots::Replication`: client-specific Dots snapshot construction.
- `Dots::Prediction`: Dots rollback model, checkpoint adapter, mechanic closure, event identity,
  typed differences, and prediction profiles.
- `Dots::Presentation`: Dots render extraction and conversion to engine draw data.
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
4. Required sanitizer and protocol-fuzz smoke after `chore/validation-baseline`.
5. Benchmark smoke after spatial grid exists.
6. Loopback networking tests after GameNetworkingSockets exists.
7. Bot/load tests as manual or scheduled jobs before making them required.

CI should eventually:

- Configure through CMake presets.
- Build Windows, Linux, and macOS.
- Run tests.
- Run clang-format verification on tracked C/C++ sources.
- Run clang-tidy against the Linux compile database with warnings treated as errors.
- Run sanitizer builds and a bounded protocol-fuzz smoke.
- Run selected benchmarks.
- Build and retain verified Dots client archives for Windows, Linux, and macOS.
- Package the Linux `dots_server`.
- After feature 20, stage-install MyCore and build the external consumer fixture.
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
- Stable `MyCore::` targets and include roots are established before packaging. Playable game
  bundles may use runtime install rules earlier; engine SDK exports remain deferred until
  Feature 20.
- Lua, EnTT, Vulkan, Conan, OpenGL, and fixed-point work are deferred until the baseline game loop and networking path are measurable.
- The first server architecture uses one owner thread for world simulation.
- Snapshot worker parallelism is introduced only after immutable replication views exist.
- A general task scheduler is conditional on profiles from the load harness or second game and
  never replaces explicit simulation, render-submission, network-poll, or audio-callback
  ownership.
- The 1,000-client target is validated through staged bot milestones: 10, 100, 500, and 1,000.
