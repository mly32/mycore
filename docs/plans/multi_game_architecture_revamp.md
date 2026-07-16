# Multi-Game Architecture Revamp Plan

## Goal

Prepare MyCore to support multiple cleanly separated games without replacing the current
vertical-slice strategy with speculative engine design. The Dots multiplayer game remains
the first implementation driver. A later offline 3D aim trainer will validate that the
engine libraries are genuinely reusable.

## Documentation Gate

Documentation must be updated and reviewed before the existing foundation layout, CMake
targets, or C++ sources are changed.

The documentation phase updates:

- `docs/game_engine_technology_plan.md`
- `docs/development_branch_plan.md`
- `docs/plans/00-foundation.md`
- `docs/cpp_style_guide.md`
- `README.md`

The documentation must establish:

- `games/dots` as the owner of Dots simulation, protocol, replication, presentation,
  rules, applications, tests, and assets.
- `engine` as a collection of independently consumable, game-neutral libraries.
- `MyCore::` CMake targets and `mycore/` public include paths for engine libraries,
  including explicit Core, Math, Time, Assets, platform, rendering, debugging, transport,
  and scripting ownership.
- `Dots::` CMake targets, `dots/` public include paths, and `dots_*` executable names for
  the Dots game.
- A small owned math library that expands from required 2D operations to required 3D
  operations when the aim trainer is implemented.
- Source-tree consumption first and an installed CMake package only after several engine
  modules have stabilized.

## Foundation Refactor After Approval

After the documentation review gate, reorganize the current foundation so that:

```text
engine/
  core/
games/
  dots/
    simulation/
    apps/
      client/
      server/
      bot/
```

The foundation will expose `MyCore::Core` and `Dots::Simulation`, build `dots_client`,
`dots_server`, and `dots_bot`, place runtime outputs under `build/<preset>/bin`, and retain a
dependency-free CTest smoke test.

When real transport requires multiple processes, add a Dots-owned Python developer launcher
that starts the server, clients, and later bots from a selected preset build directory. It
will manage readiness, prefixed logs, metrics paths, exit-code propagation, and reliable
child cleanup; CMake remains responsible for building, not runtime orchestration.

## Later Reuse Validation

After the Dots roadmap establishes the reusable platform and renderer modules, add an
offline desktop aim trainer under `games/aim_trainer`. It will reuse MyCore libraries and
add requirement-driven 3D math, depth rendering, mesh rendering, a perspective camera,
mouse look, ray-based target hits, spawning, scoring, and reset behavior.

The aim trainer will not initially add multiplayer, a general physics engine, a generic
scene graph, or a universal ECS.

After that second in-tree game validates the public boundaries, add install/export support
and test a minimal out-of-tree consumer using `find_package(MyCore CONFIG REQUIRED)`.

## Acceptance Criteria

- The architecture and branch plans describe the same multi-game layout and target graph.
- Dots-specific types and behavior remain outside engine libraries.
- Game executables are owned by their games rather than a global `apps` directory.
- The renderer boundary distinguishes generic GPU facilities from game presentation.
- Documentation changes are reviewed before the project structure is modified.
