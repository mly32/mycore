# Dots

Dots is MyCore's first complete game vertical slice: an Agar.io-like game used to drive the
engine through concrete simulation, rendering, input, networking, observability, and packaging
requirements.

## Build and play

Configure and build MyCore with the preset for your host platform, as described in the
[repository README](../../README.md):

```bash
cmake --preset macos-clang-debug
cmake --build --preset macos-clang-debug --target dots_client
./build/macos-clang-debug/bin/dots_client
```

The default client runs a local offline simulation. Pass `--in-memory` to run an embedded
authoritative server and present replicated snapshots through the deterministic in-memory
transport:

```bash
./build/macos-clang-debug/bin/dots_client --in-memory
```

The current `main` branch does not connect separate client and server processes. The standalone
`dots_server` exercises the headless authoritative loop, while native transport is developed as
the next networking slice.

| Executable | Purpose |
|---|---|
| `dots_client` | Playable SDL_GPU client with offline and embedded-authority modes |
| `dots_server` | Headless authoritative 30 Hz simulation process |
| `dots_bot` | Foundation executable for future load-testing clients |

## Controls and configuration

The default hybrid input mode uses WASD or the arrow keys while they are held and otherwise
moves toward the mouse cursor. Press Escape to quit. Input, window, rendering, simulation, debug,
and color settings are documented in [`config/dots-client.toml`](config/dots-client.toml) and its
checked-in JSON schema.

Load a configuration explicitly with:

```bash
./build/macos-clang-debug/bin/dots_client \
    --config games/dots/config/dots-client.toml
```

Run `dots_client --help` or `dots_server --help` for the complete command-line interface.

## Code ownership

- `simulation/` owns deterministic game rules and stays independent of SDL, rendering,
  transport, and presentation.
- `protocol/` owns Dots wire messages and binary encoding.
- `replication/`, `server/`, and `client_runtime/` own authoritative session behavior and
  replicated client state.
- `presentation/` maps Dots state into game-neutral rendering data.
- `apps/` contains the Dots executable composition roots.
- `config/` contains the sample client configuration and editor schema.

Reusable facilities live under [`engine/`](../../engine/); Dots-specific behavior remains here.
See the [server-authoritative networking guide](../../docs/server_authoritative_networking_guide.md)
and [SDL_GPU rendering guide](../../docs/sdl_gpu_rendering_guide.md) for deeper design context.
