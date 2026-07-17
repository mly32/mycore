# Feature 07: Debug Observability

## Goal

Make the offline Dots client inspectable before networking work begins. Add game-neutral
logging, rolling frame metrics, and profiler hooks without pulling SDL or Dots concepts into
headless engine code. Add a client-only Dear ImGui integration over the existing SDL3 and
SDL_GPU layers, while keeping the actual Dots panel game-owned.

The intended dependency split is:

```text
MyCore::Debug
  |-- spdlog + fmt
  `-- Tracy client hooks

MyCore::DebugUI
  |-- MyCore::Debug
  |-- MyCore::PlatformSDL
  |-- MyCore::Render
  `-- Dear ImGui SDL3 + SDL_GPU backends

dots_client
  |-- MyCore::DebugUI
  `-- Dots-owned overlay contents

dots_server / dots_bot
  `-- MyCore::Debug only
```

## Implementation

1. Add `spdlog`, `fmt`, Tracy, and Dear ImGui with its SDL3/SDL_GPU backend through the pinned
   vcpkg manifest.
2. Add `MyCore::Debug` with owner-qualified logging, a bounded rolling frame-metrics
   accumulator, and small Tracy macros. Logging and metrics public headers remain free of SDL
   and Dots types.
3. Add event observation to `MyCore::PlatformSDL` so Dear ImGui can receive every drained SDL
   event without owning the event loop or changing input snapshots.
4. Extend `MyCore::Render` only with the native-handle and load-pass seams required by the
   official Dear ImGui SDL_GPU backend. Let `MyCore::Render2D` record its world pass and then a
   generic frame-extension callback on the same command buffer and swapchain texture.
5. Add move-disabled `MyCore::DebugUI` RAII ownership of the Dear ImGui context and official
   SDL3/SDL_GPU backends. It owns no Dots panels.
6. Add a compact, non-interactive Dots overlay in the bottom-right showing input mode, world
   tick, player/food counts, occupied spatial-grid cells, frame timing, actual/target tick rate,
   simulation cost, backlog, catch-up/cap/deadline counters, and discarded time. Replace the
   temporary input-mode window-title suffix.
7. Add useful CPU zones around the client frame, simulation stepping, presentation extraction,
   and render submission. Use on-demand Tracy so disconnected profiling has minimal overhead.
8. Report fixed-step cap hits and deadline misses with rate-limited warnings, escalate sustained
   overload, and record recovery. Let the offline client discard whole excess backlog only after
   measuring it so a hitch cannot create an unbounded catch-up spiral.
9. Apply the same interpolated position to the local player and its following camera so the
   player remains centered instead of oscillating between fixed simulation ticks.

### Local-player jitter fix

The original fixed-step client interpolated its following camera between the player's previous
and current simulation positions, but presentation still drew the player at the newest discrete
world position. After each 30 Hz tick, the player therefore jumped ahead of the camera and then
appeared to slide back toward the center while the camera caught up. Static food and the grid
remained sharp, which made the mismatch look like player or input jitter.

`extract_interpolated_follow_frame()` now wraps the presentation rule: it calculates one
interpolated sample and applies it to both the followed entity and camera through a transient
position override. This keeps the player centered while the world moves smoothly around it.
Simulation state remains fixed-step and unchanged; only the rendered frame is interpolated.
Future moving entities should likewise interpolate their presentation transforms rather than
mixing current simulation state with an interpolated camera.

The client exposes this distinction through `[debug].presentation_mode`: `interpolated` renders
the normal smooth view, `fixed` renders the latest completed simulation tick, and `comparison`
adds a transparent outlined ghost at that fixed-tick position over the interpolated view.
Render2D supplies the reusable independent fill/outline circle style; the choice of which Dots
position is the comparison truth remains game-owned presentation logic.

Switching `kTickRateHz` from 30 to 5 makes the issue very apparent.

## Tests

- Rolling frame and fixed-step metrics defaults, rates, timing, backlog, overload counters,
  reset, and invalid samples.
- Fixed-step capped backlog reporting and explicit whole-step discard behavior.
- Dots presentation applies interpolated entity-position overrides without moving static food.
- SDL event observers receive drained events without changing quit/window-close behavior.
- Existing Render, Render2D, Dots presentation, client input/configuration, and package smoke
  tests continue to pass.
- Headless server and bot targets do not link Dear ImGui or SDL video through `MyCore::Debug`.
- Manual client check confirms the overlay remains aligned after resize and high-DPI changes.

## Exit Criteria

- The playable client presents Dots debug information through Dear ImGui over SDL_GPU.
- Logs use owner-qualified categories and the core client frame exposes Tracy zones.
- Metrics aggregation is deterministic and UI-independent.
- Dots panel content remains outside `MyCore::Debug` and `MyCore::DebugUI`.
- Headless targets gain logging/profiling support without gaining rendering dependencies.

## References

- [Dear ImGui backends](https://github.com/ocornut/imgui/blob/master/docs/BACKENDS.md)
- [Dear ImGui SDL_GPU backend](https://github.com/ocornut/imgui/blob/master/backends/imgui_impl_sdlgpu3.h)
- [spdlog](https://github.com/gabime/spdlog)
- [Tracy profiler](https://github.com/wolfpld/tracy)
