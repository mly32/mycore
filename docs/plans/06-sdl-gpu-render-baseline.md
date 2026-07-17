# Feature 06: SDL_GPU Render Baseline

## Goal

Replace the temporary `SDL_Renderer` code in `dots_client` with a small game-neutral
`MyCore::Render` layer over SDL_GPU, an engine-owned `MyCore::Render2D` layer, and a pure
Dots-owned presentation conversion. Preserve the playable Feature 05 client while making GPU
lifetime, asset loading, command submission, and common 2D drawing reusable by later games.

The completed dependency path is:

```text
dots_client
  |-- dots_client_support
  |-- Dots::Presentation
  |     |-- Dots::Simulation
  |     `-- MyCore::Render2D
  |           |-- MyCore::Assets
  |           `-- MyCore::Render
  `-- MyCore::PlatformSDL

dots_server and dots_bot
  `-- Dots::Simulation
```

SDL remains a target-scoped dependency of graphical engine modules. It does not become a
dependency of `MyCore::Core`, Dots simulation, the server, or the bot.

## Design Decisions

### Rendering boundary

- Add `mycore_assets` / `MyCore::Assets`, depending only on `MyCore::Core`. A rooted directory
  asset source resolves relative asset names and returns byte buffers with clear path-aware
  errors. It rejects absolute paths and traversal outside its root.
- Add `mycore_render` / `MyCore::Render`, depending on SDL3 and the SDL platform window layer.
  Public APIs describe devices, buffers, shaders, graphics pipelines, command lists, render
  passes, uploads, and draws without any Dots vocabulary.
- Use move-only RAII wrappers for SDL_GPU resources. A render device must outlive its window
  claim, buffers, shaders, and pipelines; destructors release resources through their owning
  device. Acquired command buffers are submitted or safely cancelled.
- Keep the first API requirement-driven. Do not add a scene graph, render graph, material
  system, generic camera, backend abstraction, or global renderer singleton.
- Add `mycore_render_2d` / `MyCore::Render2D`. Its transient draw list contains a camera,
  optional grid, clear color, and circles. Its renderer owns the generic circle/grid shaders,
  vertex layouts, pipelines, buffers, batching, and frame recording.
- Add `dots_presentation` / `Dots::Presentation`. It owns read-only Dots render extraction and
  the pure mapping from players/food plus client settings into a Render2D draw list. It has no
  simulation ownership and never mutates `World`.

### Shaders and assets

- Keep the built-in Render2D HLSL shader sources under `engine/render_2d/assets/shaders/`.
- Add target-appropriate host shader tools and invoke them from CMake custom commands. Use
  glslang to compile HLSL to SPIR-V on Linux, glslang plus SPIRV-Cross to produce MSL on Apple
  platforms, and DirectX Shader Compiler to produce DXIL on Windows. Shader compilation is
  part of the normal `dots_client` build, so invalid shaders fail CI.
- Stage generated shaders under `assets/mycore/render_2d/shaders/` beside `dots_client`. Resolve that root
  from the executable base directory at startup, not from the process working directory.
- Load compiled bytes through `MyCore::Assets`; keep platform-format filename conventions,
  shader entry points, resource counts, and other shader metadata in `MyCore::Render2D`.
- Do not link shader compiler libraries into the client or compile shaders at runtime.

SDL_GPU requires backend-specific shader formats. The device will advertise only the format
compiled for the target platform, allowing SDL to select the matching Metal, D3D12, or Vulkan
backend. MSL uses its translated entry point while DXIL and SPIR-V use the HLSL entry point.

### Frame and presentation model

- Create the window hidden, create the GPU device, claim the window, select the configured
  present mode, create engine Render2D resources, and show the window only after startup
  succeeds.
- Map `vsync = true` to the SDL_GPU vsync present mode. For `vsync = false`, prefer immediate,
  then mailbox, and fall back to vsync when the platform does not support either non-vsync
  mode.
- Acquire one command list and a swapchain texture per rendered frame. Use the acquired
  swapchain pixel dimensions for projection and grid placement; keep high-DPI input conversion
  aligned through window pixel dimensions. Treat an unavailable swapchain as a skipped frame.
- Upload changing instance data before the render pass. Retain GPU buffers between frames,
  grow their capacity geometrically, and use SDL_GPU cycling so uploads do not overwrite data
  still used by an in-flight frame.
- Use one render pass with a clear color, an optional analytic full-screen grid draw, and an
  instanced-quad signed-distance circle draw. Render2D owns both pipelines and their shaders;
  `MyCore::Render` only records generic bindings and draws.
- Preserve the fixed-step loop, camera interpolation, input behavior, colors, and resizing.
  Remove the temporary bottom-right glyph implementation and append the startup-only input mode
  to the window title until Dear ImGui provides the debug overlay in Feature 07.

## Planned Implementation

### 1. Asset and shader build foundation

- Add the `engine/assets` target with a directory-backed byte reader and focused startup
  errors.
- Add asset tests for successful binary reads, missing files, absolute names, and parent-path
  traversal.
- Add portable host shader-tool dependencies and a CMake helper that compiles a named HLSL
  stage to the active target format with explicit inputs, outputs, and dependencies.
- Add engine-owned grid and circle shader sources and stage their generated outputs beside the
  client executable without modifying the source asset directory.

### 2. Thin SDL_GPU engine layer

- Add the `engine/render` target and `MyCore::Render` public API.
- Implement device creation, SDL window claim/release, swapchain present-mode selection,
  shader/buffer/pipeline creation, buffer upload, command acquisition, render-pass recording,
  instanced drawing, submission, and path-rich SDL error reporting.
- Keep raw SDL_GPU handles private except for narrowly scoped implementation access. Avoid
  mocks of SDL internals; test descriptor defaults, ownership traits, validation, and any pure
  format-selection helpers where those contracts are stable.

### 3. Engine Render2D and Dots presentation

- Add read-only player-ID and food-ID views to `World` so presentation can enumerate live
  entities without retaining stale spawn lists or exposing storage mutation.
- Define pure Dots render data containing world-space circle instances and the camera/view
  values needed for a frame. Extract live players and food from `World` and validate geometry.
- Unit test empty and populated extraction, player and food classification, entity removal,
  position/radius propagation, camera data, and stable separation from simulation mutation.
- Define a generic Render2D draw list and renderer. Build engine-owned grid and circle pipelines
  from compiled shader assets; render circles as instanced quads with signed-distance edges.
- Map Dots frame data and settings into the generic draw list in a pure, testable function.

### 4. Client integration and cleanup

- Replace `SDL_CreateRenderer`, scanline circle drawing, debug text, render scaling, and all
  other `SDL_Renderer` helpers in `client_app.cpp` with `MyCore::Render`,
  `MyCore::Render2D`, and `Dots::Presentation` composition.
- Keep `--headless-smoke` unchanged in intent: initialize SDL video, create a hidden window,
  poll one input snapshot, and exit before GPU-device or presentation creation. The dummy
  video driver is not a GPU integration environment.
- Preserve startup failures for device, window claim, shader/asset, pipeline, command, and
  submission errors. Include the failed operation or asset path in each message.
- Ensure only `dots_client` and presentation/render targets gain GPU and asset dependencies;
  verify `dots_server`, `dots_bot`, and simulation tests remain headless.

### 5. Documentation and verification

- Update the README to describe the SDL_GPU client, generated shader assets, supported GPU
  backends, build-time shader compilation, and practical manual testing commands.
- Configure, build, and run all tests through each CI preset. The shader custom commands must
  execute in normal CI builds.
- Retain the existing help and headless-smoke CTest cases. Do not add a dummy-driver GPU test
  that cannot exercise a real swapchain.
- Manually run the client on macOS, Linux, and Windows where hardware runners are available.
  Verify resizing, high-DPI mouse alignment, vsync selection, all input modes, the title
  indicator, food consumption/growth, clean shutdown, and asset lookup from a working directory
  outside the repository.

### 6. Runtime package follow-up

- Add a `DotsClient` CMake install component containing only the client executable, its exact
  generated Render2D shader outputs, and a complete example configuration. Do not copy the
  whole build-tree asset directory because removed assets may remain there after an incremental
  build.
- Build a standard macOS `Dots.app` layout with code under `Contents/MacOS` and assets under
  `Contents/Resources`; use a flat executable-plus-assets layout on Windows and Linux. Include
  discovered runtime DLL dependencies in the Windows install.
- Add a verified `dots_client_package` target. Use ZIP on Windows and `.tar.gz` on macOS/Linux,
  preserve executable permissions, and emit a SHA-256 checksum.
- Add `--package-smoke` to initialize a hidden dummy-driver window and read all packaged shaders
  without creating a GPU device. Extract every generated archive, require exactly the expected
  shader set, and run both `--help` and `--package-smoke`.
- Have each platform CI job upload its verified archive as a short-lived workflow artifact.
  Code signing, notarization, installers, and permanent tagged release publication remain
  separate release-engineering work.

## Test Matrix

| Area | Automated coverage |
|---|---|
| Assets | Binary reads, empty files, missing paths, rooted resolution, and traversal rejection |
| Render API | Move/copy traits, descriptor validation, shader-format choice, and stable pure helpers |
| Render2D data | Defaults, value semantics, ownership traits, and engine-only dependency boundary |
| Dots extraction | Live instances, removals, geometry, draw-list colors/grid/camera, and no mutation |
| Shader pipeline | HLSL compilation for the preset platform and staged runtime outputs |
| Client regression | Existing config/input tests, `--help`, and GPU-free `--headless-smoke` |
| Runtime package | Exact archive contents, executable help, shader reads, and dummy-driver package smoke |
| Dependency boundaries | Server, bot, and simulation targets build without presentation or render linkage |

Real GPU device and swapchain behavior remains a manual/platform integration check; CI's dummy
video driver is suitable only for the existing platform initialization smoke test.

## Exit Criteria

- `dots_client` contains no `SDL_Renderer` creation or temporary 2D draw path.
- `dots_client` renders the grid, live food, and local player through engine-owned Render2D
  layered on `MyCore::Render` and SDL_GPU; its title reports the configured input mode.
- HLSL shaders compile during the build and load from staged assets independently of the
  current working directory.
- Rendering remains separate from `World` ownership and Dots concepts do not enter engine
  asset or render APIs.
- The existing playable behavior, configuration, fixed-step simulation, resize/high-DPI
  alignment, headless smoke test, and non-client target boundaries continue to work.
- `dots_client_package` produces a verified relocatable archive on each desktop platform; CI
  retains the archive for developer testing.

## Deferred Work

- Dear ImGui, GPU timing, the in-frame input-mode overlay, and renderer observability (Feature 07).
- Generic text/font systems, texture asset formats, render graphs, scene graphs, materials,
  depth buffers, meshes, 3D cameras, and GPU-driven rendering.
- Direct Vulkan and OpenGL comparison branches.
- Runtime shader compilation, shader hot reload, custom asset packs, signing/notarization,
  installers, and permanent release publication.
- Engine-owned platform user-directory discovery and automatic per-user configuration fallback;
  the packaged example remains read-only until that later polish feature.

## References

- [SDL_GPU overview](https://wiki.libsdl.org/SDL3/CategoryGPU)
- [Khronos glslang compiler](https://github.com/KhronosGroup/glslang)
- [Khronos SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)
- [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)
- [SDL swapchain acquisition](https://wiki.libsdl.org/SDL3/SDL_WaitAndAcquireGPUSwapchainTexture)
