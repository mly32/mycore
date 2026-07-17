# SDL_GPU Rendering and Shaders

This guide assumes you understand the basic CPU-side idea of a scene: objects have geometry,
positions, sizes, and colors, and a renderer turns that information into an image. It explains
what changes when most of that drawing work moves to the GPU and how MyCore uses SDL_GPU.

## From CPU drawing to GPU drawing

In Feature 05, the client drew a circle by calculating every horizontal scanline on the CPU
and sending those lines through `SDL_Renderer`. SDL hid its own GPU implementation and shaders
behind functions such as `SDL_RenderLine`.

With SDL_GPU, the CPU instead describes the work in larger pieces:

```text
CPU                                          GPU
simulation state                             many parallel shader invocations
      |                                                   |
extract circles, camera, colors                           |
      |                                                   |
upload geometry and circle instances -------------------->|
record pipelines, render pass, and draws ---------------->|
submit commands -----------------------------------------> render pixels
                                                          |
window <------------------------------------------ present completed image
```

The CPU remains responsible for gameplay, deciding what is visible, and preparing render data.
The GPU is responsible for transforming that data into pixels. GPU work is asynchronous: a
submission may still be running after the CPU starts preparing a later frame.

## Where SDL ends and the renderer begins

SDL offers APIs at different levels. Feature 05 used the convenient 2D `SDL_Renderer` API.
Calls such as `SDL_RenderLine` describe familiar shapes, while SDL privately supplies the GPU
buffers, shaders, pipelines, and batching needed to draw them.

`SDL_GPU` is a lower-level cross-platform graphics API:

| SDL_GPU provides | The renderer using SDL_GPU provides |
|---|---|
| A device backed by Metal, Vulkan, or D3D12 | Geometry and object data |
| Buffers, textures, and samplers | Shader programs |
| Pipelines, command buffers, and render passes | Pipeline configuration and draw commands |
| Swapchain acquisition and presentation | Camera and other shader inputs |

SDL translates the common API calls to the selected native backend. It cannot decide what a
circle, sprite, player, or food pellet means, so it cannot supply the corresponding shader or
data layout.

Full game engines have the same GPU concepts, but usually hide their standard implementations
behind APIs such as `draw_circle`, `Sprite`, `Material`, or a visual editor. Their built-in
shaders are still present and compiled; ordinary game code simply does not manage them.

MyCore currently exposes the low-level boundary because Feature 06 establishes the SDL_GPU
foundation. A later `Render2D` layer can own common circle, rectangle, sprite, and grid shaders
and let games submit only visual data. Custom game shaders would then be an advanced escape
hatch rather than a requirement for ordinary drawing.

## What a shader is

A shader is a small program executed many times in parallel by the GPU. Dots uses two shader
stages:

- A **vertex shader** calculates where a vertex appears on screen and passes values such as
  color or local coordinates to the next stage.
- A **fragment shader** calculates the color and transparency of a potential output pixel.

The canonical shader source is written in HLSL. It resembles a small C-like function, but its
inputs, outputs, memory layout, and execution model follow GPU rules rather than normal C++
rules.

Dots has three small shader groups under `games/dots/assets/shaders/`:

- `grid`: draws the background and calculates grid lines across a full-screen triangle.
- `circle`: expands circle instances into screen-space quads and evaluates smooth circular
  edges in the fragment shader.
- `solid`: draws the rectangles used for the input-mode HUD.

Shaders only control presentation. They do not contain movement, collision, eating, or other
gameplay rules.

## How C++ and shaders work together

C++ does not call a shader like a normal function. During setup, it gives SDL compiled shader
bytes and describes how data will enter those shaders. SDL combines that information into a
graphics pipeline:

```cpp
auto vertex_shader = device.create_shader(circle_vertex_code);
auto fragment_shader = device.create_shader(circle_fragment_code);
auto pipeline = device.create_pipeline(vertex_shader, fragment_shader, circle_layout);
```

During a frame, C++ uploads data, binds the pipeline, supplies shared inputs, and records a draw:

```cpp
commands.upload(circle_buffer, circles);
pass.bind(pipeline);
pass.bind(circle_buffer);
commands.push_uniform(camera);
pass.draw(/* vertices */ 6, /* instances */ circles.size());
```

After submission, the GPU runs the bound vertex and fragment shaders automatically. Shader
inputs normally come from:

- **Vertex or instance buffers**, containing geometry or per-object data such as center,
  radius, and color.
- **Uniforms**, containing small values shared by a draw, such as camera position and output
  dimensions.
- **Textures and samplers**, containing images and rules for reading them.

The C++ and shader definitions form a contract: their field layouts, binding slots, vertex
attributes, and resource counts must agree. SDL connects the resources, but it cannot infer
what the bytes mean.

## How one quad becomes many circles

The CPU uploads one reusable quad made from two triangles. For every live food or player
entity, it also uploads a compact instance containing approximately:

```text
world center + radius + color
```

An instanced draw tells the GPU to reuse the quad once for every circle instance. The vertex
shader moves and scales each copy around its world-space center. The fragment shader receives
coordinates within the quad and makes pixels outside the circle transparent.

```text
one six-vertex quad + 273 circle instances
                       |
                       v
                 one instanced draw
                       |
                       v
              273 independently placed circles
```

This avoids generating circle scanlines on the CPU and avoids issuing a separate draw for
every circle. It also leaves the highly parallel pixel work on the GPU.

## Why the shaders are compiled separately

SDL_GPU provides one C API over several native graphics systems, but those systems do not use
one universal shader format:

| Target | SDL_GPU backend | Runtime shader asset |
|---|---|---|
| macOS | Metal | MSL |
| Linux | Vulkan | SPIR-V |
| Windows | D3D12 | DXIL |

MyCore keeps one HLSL source and translates it during the CMake build:

```text
macOS:   HLSL -> SPIR-V -> MSL
Linux:   HLSL -> SPIR-V
Windows: HLSL ---------> DXIL
```

Doing this at build time provides four practical benefits:

1. Shader syntax and translation failures break the build instead of surprising a player.
2. The client ships only the format needed by its target platform.
3. Shader compiler libraries do not need to be linked or distributed with the game.
4. Every build uses known source and tools, making results easier to reproduce.

On macOS, MSL is translated Metal source; the Metal driver performs the final hardware-specific
compilation when the graphics pipeline is created. The cross-language translation still
happens during our build.

The build tools are host-only vcpkg dependencies:

- `glslang[tools]` compiles HLSL to SPIR-V on macOS and Linux.
- `spirv-cross` translates SPIR-V to MSL on macOS.
- `directx-dxc` compiles HLSL to DXIL on Windows.

They run while building MyCore and are not runtime dependencies of `dots_client`.

Moving a built-in shader from Dots into a future engine `Render2D` layer would not remove this
compilation step. It would only change who owns the source and hides the pipeline. Engine-owned
compiled shaders could remain staged assets or be converted to generated C++ byte arrays and
embedded in the executable; game-specific shaders could remain ordinary assets.

## Shader assets at runtime

Compiled shaders are assets rather than C++ object files. The build places them beside the
executable:

```text
build/<preset>/bin/
|-- dots_client
`-- assets/
    `-- dots/
        `-- shaders/
            |-- circle.vert.<platform-format>
            |-- circle.frag.<platform-format>
            |-- grid.vert.<platform-format>
            |-- grid.frag.<platform-format>
            |-- solid.vert.<platform-format>
            `-- solid.frag.<platform-format>
```

They are not embedded in `dots_client`. At startup, `MyCore::PlatformSDL` finds the executable
directory, `MyCore::Assets` reads the appropriate shader bytes, and `Dots::Presentation` asks
`MyCore::Render` to create SDL_GPU shader and pipeline objects. A packaged game must therefore
include both the executable and its `assets/` directory.

## The SDL_GPU objects involved

SDL_GPU uses explicit objects because modern GPUs execute queued work and may keep using a
resource after the CPU has moved on.

### Device

The GPU device is the connection to the selected backend and physical GPU. MyCore creates it
for MSL, SPIR-V, or DXIL, claims the SDL window, and configures the swapchain and present mode.

### Buffers and uploads

A GPU buffer is GPU-visible memory. Dots uses buffers for the reusable quad and for the current
circle and HUD instances. CPU data first enters a transfer buffer, then a copy command uploads
it to the GPU buffer.

Dynamic buffers are reused and grown only when necessary. Uploads use **cycling**, which lets
SDL provide safe backing storage instead of overwriting data that an earlier GPU frame may
still be reading.

### Graphics pipeline

A graphics pipeline combines the state required for a particular kind of draw:

- Vertex and fragment shaders.
- The layout of vertex and instance data.
- Triangle topology.
- Transparency/blending rules.
- The output texture format.

Creating this state together allows the native graphics backend to validate and prepare it
before drawing. The grid, circles, and HUD use separate Dots-owned pipelines.

### Command list and render pass

`MyCore::Render` calls SDL's GPU command buffer a **command list**. It records uploads, state
bindings, and draws without executing each call immediately. Submission sends the completed
batch to the GPU.

A render pass is not a pass over the game world's objects. It is a bounded drawing session for
one set of output textures, also called render targets or attachments. It answers:

- Which color and optional depth textures may these draws write?
- Should their old contents be loaded, cleared, or ignored at the beginning?
- Should their new contents be stored or discarded at the end?

Dots uses one render pass per frame:

```text
begin pass targeting the acquired window texture
    clear it to the background color
    bind grid pipeline    -> draw grid
    bind circle pipeline  -> draw all circles
    bind HUD pipeline     -> draw HUD rectangles
end pass and retain the resulting image
submit command list and present
```

Pipelines and their resources are bound inside the pass. Uploads happen before the pass because
they are copy operations rather than drawing operations. More complex renderers use multiple
passes when they must produce intermediate textures, shadows, reflections, or post-processing;
Dots needs only the final window image.

### Swapchain

The swapchain supplies images that can be presented in the SDL window. Each frame acquires one
swapchain texture, renders into it, and presents it when the command list is submitted. Its
pixel dimensions are used so resizing and high-DPI displays remain aligned.

## One Dots frame

At a high level, a rendered frame now follows this sequence:

1. Poll input and advance zero or more fixed simulation steps on the CPU.
2. Extract live food and player circles from `World` without changing simulation state.
3. Interpolate the camera position.
4. Create a command list and upload current circle instances.
5. Acquire the window's next swapchain texture and prepare the HUD instances.
6. Begin one render pass and clear the target.
7. Bind and draw the grid pipeline.
8. Bind the circle pipeline and issue one instanced circle draw.
9. Bind and draw the HUD pipeline.
10. End the pass and submit the command list for presentation.

## World state versus visual state

The authoritative game `World` should not become an engine-owned render scene. It contains
gameplay meaning and must remain usable by the server, bot, replays, and tests without a GPU.
Instead, a client extracts temporary presentation data from it:

```text
Dots::World
    players, food, rules, collision
             |
             | read-only Dots-owned extraction
             v
presentation snapshot / draw list
    camera, circles, rectangles, colors
             |
             | engine-owned batching and drawing
             v
MyCore::Render -> SDL_GPU
```

The game must still decide that a food entity should look like a pink circle. A reusable engine
renderer can decide how a generic pink circle becomes buffers, a pipeline, shaders, and a draw.
This keeps game meaning out of the engine without forcing every game to implement routine GPU
machinery.

The current Feature 06 ownership is:

```text
Dots::Simulation     owns authoritative game state and rules
Dots::Presentation   owns Dots render extraction, shaders, and pipelines
MyCore::Render       owns game-neutral SDL_GPU lifetime and commands
MyCore::Assets       owns game-neutral file lookup and byte loading
MyCore::PlatformSDL  owns SDL initialization, windows, and input
```

This is a deliberately thin baseline, not necessarily the final high-level API. A future
`MyCore::Render2D` can move generic primitive shaders and batching below `Dots::Presentation`.
Dots would then retain only the domain-to-visual conversion. The server and bot would continue
to link neither presentation nor rendering code.

## Where to read the implementation

- `cmake/CompileShaders.cmake`: platform shader compilation.
- `engine/assets/`: executable-relative asset loading.
- `engine/render/`: the small game-neutral SDL_GPU wrapper.
- `games/dots/presentation/`: Dots extraction, resources, pipelines, and frame recording.
- `games/dots/apps/client/src/client_app.cpp`: composition with input and simulation.

## Further reading

Read these in order rather than beginning with a complete native graphics API:

1. [SDL_GPU API overview](https://wiki.libsdl.org/SDL3/CategoryGPU) for the complete frame
   workflow and SDL terminology.
2. [SDL_GPU: It Begins With a Triangle](https://hamdy-elzanqali.medium.com/let-there-be-triangles-sdl-gpu-edition-bd82cf2ef615)
   for a beginner-oriented C++ walkthrough.
3. [Vulkan tutorial overview](https://docs.vulkan.org/tutorial/latest/01_Overview.html) for a
   second explanation of swapchains, pipelines, command buffers, and asynchronous submission.
   The overview is enough; the full Vulkan API is below SDL_GPU and considerably more detailed.
4. [SDL_GPU examples](https://github.com/TheSpydog/SDL_gpu_examples) to connect each concept to
   small working programs.
5. [SDL_GPU sprite batcher](https://moonside.games/posts/sdl-gpu-sprite-batcher/) after the
   basics, to see how a higher-level renderer hides repeated low-level drawing work.

## Glossary

| Term | Meaning in this project |
|---|---|
| Asset | Runtime data loaded by the program, such as a compiled shader. |
| Attachment | A texture used as an output of a render pass, such as its color or depth image. |
| Backend | Native graphics implementation selected by SDL_GPU: Metal, Vulkan, or D3D12. |
| Buffer | GPU-visible memory containing vertices, instances, or other structured data. |
| Command list | A recorded batch of GPU uploads and drawing operations. SDL calls it a command buffer. |
| Cycling | Requesting safe backing storage when a GPU resource may still be in use. |
| Draw call | A command asking the GPU to draw geometry with the currently bound state. |
| Draw list | Temporary visual primitives collected for a renderer to batch and submit. |
| DXIL | Compiled shader format used by D3D12 on Windows. |
| Fragment | A candidate output pixel produced while rasterizing geometry. |
| Fragment shader | Program that calculates a fragment's color and transparency. |
| GPU device | The application's SDL_GPU connection to a graphics backend and GPU. |
| Graphics pipeline | Prepared shaders plus vertex layout, blending, topology, and target state. |
| HLSL | The human-authored shader language used as MyCore's canonical source. |
| Instance | One reuse of shared geometry with its own position, size, color, or other data. |
| Instanced draw | One draw command that renders many instances of the same base geometry. |
| MSL | Metal Shading Language consumed by the Metal backend on macOS. |
| Present | Make a completed swapchain image visible in the window. |
| Rasterization | Conversion of positioned triangles into fragments covered by those triangles. |
| Render pass | A group of draws targeting specified textures with defined clear/load/store behavior. |
| Render target | A texture that receives rendered pixels; it becomes an attachment during a pass. |
| Shader | A small GPU program executed in parallel across vertices or fragments. |
| SPIR-V | Binary shader representation consumed by Vulkan on Linux. |
| Swapchain | SDL-managed sequence of window images available for rendering and presentation. |
| Transfer buffer | CPU-accessible staging memory used to upload data into GPU resources. |
| Uniform | A small shader input that stays constant across all vertices or fragments in a draw. |
| Vertex | One input point of geometry; three vertices normally describe a triangle. |
| Vertex shader | Program that calculates a vertex's output position and values for later stages. |
