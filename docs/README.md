# MyCore Documentation

## Games

- [Dots](../games/dots/README.md): run modes, controls, configuration, runtime visibility, and
  code ownership for the current vertical slice.

## Developer references

- [Building MyCore](building.md): prerequisites, presets, testing, packaging, and IDE setup.
- [C++ style guide](cpp_style_guide.md): conventions, ownership boundaries, and API design.
- [Server-authoritative networking](server_authoritative_networking_guide.md): protocol,
  transport, authority, replication, impairment, and the prediction/interpolation model.
- [SDL_GPU rendering](sdl_gpu_rendering_guide.md): shaders, resources, draw flow, and platform
  backends.

## Architecture and roadmap

- [Engine technology plan](game_engine_technology_plan.md): technology choices, module ownership,
  and cross-game reuse decisions.
- [Development branch plan](development_branch_plan.md): feature ordering, dependencies, and exit
  criteria.
- [Multi-game architecture revamp](plans/multi_game_architecture_revamp.md): proposed validation
  and ownership changes for future games.

## Feature plans

- [Foundation](plans/00-foundation.md)
- [Core simulation and spatial grid](plans/02-04-core-simulation-spatial-grid.md)
- [SDL client window and input](plans/05-sdl-client-window-input.md)
- [SDL_GPU render baseline](plans/06-sdl-gpu-render-baseline.md)
- [Debug observability](plans/07-debug-observability.md)
- [Protocol binary codec](plans/08-protocol-binary-codec.md)
- [In-memory transport integration](plans/09-inmemory-transport-integration.md)
- [GameNetworkingSockets transport](plans/10-gamenetworkingsockets-transport.md)

Plans record intended scope and acceptance criteria. Inspect the current code, tests, branch,
and worktree before treating a plan as implemented behavior.
