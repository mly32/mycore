# Repository Guide

MyCore is a C++20 game-engine project developed through complete game vertical slices.
Dots is the current implementation driver; avoid speculative general-purpose systems.

## Start Here

Read only the documentation relevant to the task:

- `README.md`: project overview, quick start, and documentation routing.
- `docs/README.md`: index of current guides, architecture documents, and feature plans.
- `docs/building.md`: prerequisites, configure/build/test workflows, packaging, and IDE setup.
- `games/dots/README.md`: Dots run modes, controls, configuration, and code ownership.
- `docs/cpp_style_guide.md`: C++ conventions and engine boundary rules.
- `docs/server_authoritative_networking_guide.md`: protocol, transport, server authority,
  replication, and the prediction/interpolation mental model.
- `docs/game_engine_technology_plan.md`: architecture, technology choices, module ownership,
  and cross-game reuse decisions.
- `docs/development_branch_plan.md`: feature order, planned scope, dependencies, and exit
  criteria.
- `docs/plans/<feature>.md`: implementation details and acceptance criteria for that feature.

Do not assume a plan describes the current implementation. Inspect the code, tests, branch,
and worktree before making changes.

## Planning and Design Records

- Do not leave substantial feature plans only in agent conversation. When Plan mode covers a
  large, novel, or architecturally significant feature, create or update
  `docs/plans/<feature>.md` before implementation begins. If Plan mode cannot write files, make
  this the first implementation step.
- Treat a feature as substantial when it crosses module boundaries, changes a public API or
  protocol, introduces a dependency or platform constraint, involves concurrency or persistent
  state, or requires meaningful design tradeoffs or several implementation stages.
- Record the problem and constraints, goals and non-goals, considered approaches and tradeoffs,
  chosen design and rationale, ownership boundaries and important data flow, validation and exit
  criteria, and unresolved questions. Preserve decisions that will help a future contributor
  understand why the system has its current shape.
- Keep the plan current when implementation changes a decision. Link new plans from
  `docs/README.md` and align their scope with `docs/development_branch_plan.md` when applicable.
- Routine fixes and small, local changes do not need standalone plan documents.

## Branches and Commits

- Branch from an up-to-date `main` unless the task explicitly targets another branch.
- Use lowercase kebab-case after one of these prefixes:
  - `feature/<id-or-short-description>` for user-facing or architectural capabilities.
  - `bug/<id-or-short-description>` for behavior corrections.
  - `chore/<id-or-short-description>` for tooling, dependencies, CI, and maintenance.
  - `docs/<id-or-short-description>` for documentation-only changes.
  - `refactor/<id-or-short-description>` for behavior-preserving restructuring.
  - `spike/<short-description>` for disposable investigation branches.
- Include an issue or feature identifier when one exists, for example
  `feature/10-gamenetworkingsockets-transport`.
- Keep each branch focused on one outcome. Do not mix opportunistic cleanup into feature work.
- Use concise, imperative commit subjects and explain non-obvious design choices in the body.

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
the broadest relevant validation that is practical.

Before declaring a feature complete:

1. Format changed C++ files with the repository `.clang-format`, then run the same full tracked
   source check used by CI:

   ```bash
   git ls-files -z \
       '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' \
       | xargs -0 -r clang-format --dry-run --Werror --style=file
   ```

2. Run clang-tidy against the configured compilation database:

   ```bash
   run-clang-tidy \
       -p build/<local-debug-preset> \
       -warnings-as-errors='*' \
       -quiet
   ```

3. Build the affected targets, run focused tests, then run the full host test preset.
4. Build `dots_client_package` when runtime layout, assets, configuration, or packaging changes.
5. Update the canonical guide or README when behavior or a durable workflow changes.

Do not skip format or clang-tidy silently. If a required tool or platform is unavailable, report
the exact check not run and rely on the corresponding CI job before merge.
