# Contributing to MyCore

Thanks for taking an interest in MyCore. The project grows through complete game vertical
slices, with Dots driving the current requirements. Please discuss substantial changes in an
issue before investing in an implementation.

## Development workflow

1. Create a focused branch from `main` using the naming conventions below.
2. Follow the repository boundaries and C++ conventions in
   [`docs/cpp_style_guide.md`](docs/cpp_style_guide.md).
3. Add or update tests for behavior changes.
4. Configure, build, and test with the host preset documented in
   [`docs/building.md`](docs/building.md).
5. Run `clang-format` with the checked-in configuration and run `run-clang-tidy` against the
   configured build directory. Treat warnings as errors.
6. Open a pull request that explains the behavior, design choices, and validation performed.

Keep reusable, game-neutral code under `engine/` and Dots-specific code under `games/dots/`.
Avoid speculative abstractions: extract an engine facility only when a game-neutral contract is
clear from a concrete use case.

## Branch names

Use lowercase kebab-case with a prefix that describes the work:

- `feature/<id-or-description>`
- `bug/<id-or-description>`
- `chore/<id-or-description>`
- `docs/<id-or-description>`
- `refactor/<id-or-description>`
- `spike/<description>`

Include an issue or feature identifier when one exists, and keep each branch scoped to one
outcome.

By contributing, you agree that your contributions will be licensed under the MIT License that
covers this repository.
