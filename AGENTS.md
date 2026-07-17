# Repository Guide

MyCore is a C++20 game-engine project developed through complete game vertical slices.
Dots is the current implementation driver; avoid speculative general-purpose systems.

## Start Here

Read only the documentation relevant to the task:

- `README.md`: setup, build, test, packaging, and runtime usage.
- `docs/cpp_style_guide.md`: C++ conventions and engine boundary rules.
- `docs/game_engine_technology_plan.md`: architecture, technology choices, module ownership,
  and cross-game reuse decisions.
- `docs/development_branch_plan.md`: feature order, planned scope, dependencies, and exit
  criteria.
- `docs/plans/<feature>.md`: implementation details and acceptance criteria for that feature.

Do not assume a plan describes the current implementation. Inspect the code, tests, branch,
and worktree before making changes.

## Architecture

- Put reusable, game-neutral libraries under `engine/` and Dots-specific code under
  `games/dots/`.
- Extract an engine abstraction only when it has a clear game-neutral contract.
- Keep Dots simulation independent of SDL, rendering, networking transport, scripting, and
  presentation.
- Keep the server authoritative and headless.
- Each game owns its executable composition roots under `games/<game>/apps/`.
- Use target-based CMake. Public aliases use `MyCore::` or `Dots::`; public include roots are
  `mycore/` and `dots/` respectively.
- Preserve existing public boundaries and avoid introducing global CMake state.

## Working Practices

- Inspect `git status` first and preserve unrelated worktree changes.
- Follow `.clang-format` and `docs/cpp_style_guide.md` for C++ changes.
- Add or update tests with behavior changes. Prefer real project objects over heavy mocking.
- Do not edit generated or dependency output under `build/` or `vcpkg_installed/`.
- Keep changes scoped to the requested task; update canonical documentation when behavior or
  durable workflows change.

## Build and Validation

Use the preset matching the host from `CMakePresets.json`:

```bash
cmake --preset <local-debug-preset>
cmake --build --preset <local-debug-preset>
ctest --preset <local-debug-preset>
```

During iteration, build the affected target and run focused tests first. Before handoff, run
the broadest relevant validation that is practical. Report commands not run and why.
