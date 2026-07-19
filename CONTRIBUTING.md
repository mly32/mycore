# Contributing to MyCore

Thanks for taking an interest in MyCore. The project grows through complete game vertical
slices, with Dots driving the current requirements. Please discuss substantial changes in an
issue before investing in an implementation.

## Development workflow

1. Create a focused branch from `main`.
2. Follow the repository boundaries and C++ conventions in
   [`docs/cpp_style_guide.md`](docs/cpp_style_guide.md).
3. Add or update tests for behavior changes.
4. Configure, build, and test with the host preset documented in [`README.md`](README.md).
5. Run `clang-format` using the checked-in `.clang-format` configuration.
6. Open a pull request that explains the behavior, design choices, and validation performed.

Keep reusable, game-neutral code under `engine/` and Dots-specific code under `games/dots/`.
Avoid speculative abstractions: extract an engine facility only when a game-neutral contract is
clear from a concrete use case.

By contributing, you agree that your contributions will be licensed under the MIT License that
covers this repository.
