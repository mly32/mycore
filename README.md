# MyCore

> A small cross-platform C++20 game engine built by shipping complete playable vertical slices.

Reusable, game-neutral libraries live under `engine/`, while each game owns its rules,
presentation, and executable composition roots under `games/`. This keeps abstractions grounded
in demonstrated game requirements rather than speculative systems.

[Dots](games/dots/README.md), an Agar.io-like server-authoritative multiplayer game, is the
current implementation driver. It exercises deterministic simulation, SDL_GPU rendering,
native transport, replication, observability, configuration, and packaging. A later offline 3D
aim trainer will provide the first cross-game validation.

## Quick start

You need CMake 3.25+, Ninja, a C++20 compiler, Python 3, and vcpkg with `VCPKG_ROOT` pointing to
the directory that contains `scripts/`. macOS and Linux also require `pkg-config`; graphical
clients require a Metal-, Vulkan-, or D3D12-capable driver.

Configure, build, and test with the preset for your host. For example, on macOS:

```bash
cmake --preset macos-clang-debug
cmake --build --preset macos-clang-debug
ctest --preset macos-clang-debug
```

The corresponding Linux and Windows presets are `linux-clang-debug` and
`windows-msvc-x64-debug`. See the [build guide](docs/building.md) for platform prerequisites,
focused builds, packaging, and IDE setup.

Run Dots offline:

```bash
./build/macos-clang-debug/bin/dots_client
```

Run separate native server and client processes:

```bash
./build/macos-clang-debug/bin/dots_server --listen 127.0.0.1:27020
./build/macos-clang-debug/bin/dots_client --connect 127.0.0.1:27020
```

See the [Dots README](games/dots/README.md) for all run modes, controls, configuration, and the
multi-client launcher.

## Repository layout

| Path | Ownership |
|---|---|
| `engine/` | Small reusable libraries with `MyCore::` CMake targets |
| `games/dots/` | Dots rules, protocol, replication, presentation, and executables |
| `tests/` | Unit, integration, native-loopback, and smoke tests |
| `cmake/` | Shared target policy, packaging, shaders, and warnings |
| `docs/` | Build instructions, engineering guides, architecture, and feature plans |

The simulation remains independent of SDL, rendering, networking transport, scripting, and
presentation. The server is authoritative and headless. Engine abstractions are extracted only
when a concrete game-neutral contract exists.

## Documentation

Start with the [documentation index](docs/README.md). It separates current engineering guides
from plans, which describe intended scope and must not be treated as implementation status.

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow
and project boundaries. Report security issues according to [SECURITY.md](SECURITY.md).

## License

MyCore is available under the [MIT License](LICENSE). Third-party dependencies retain their
respective licenses.
