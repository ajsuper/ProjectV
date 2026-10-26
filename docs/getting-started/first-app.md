# Your first app

The smallest program that loads a scene and draws it.
[`examples/00-hello-voxel`](../../examples/00-hello-voxel/) is the same program with voxels built in
memory.

## The shape of every ProjectV program

```text
main()
 └─ Application app
     ├─ addSystem(Startup,  ...)   window, scene, renderer, GPU upload    (once)
     ├─ addSystem(Update,   ...)   gameplay: camera, input                (every frame)
     ├─ addSystem(Render,   ...)   set uniforms, draw                     (every frame)
     ├─ addSystem(Shutdown, ...)   free GPU data                          (once)
     └─ run()
```

Global state lives in the registry context: `app.world.ctx().emplace<T>()` once, `get<T>()` after.
See [the frame loop](../concepts/frame-loop.md).

## 1. Files

```text
my_app/
├── CMakeLists.txt
├── main.cpp
├── renderer/                  copy of examples/00-hello-voxel/helloRenderer
│   ├── render.json            the pass list
│   ├── resources.json         uniforms, shaders, textures
│   └── helloShaders/          hello.frag, vs_quad.sc, varying.def.sc
└── scene/                     any folder with a compose.json, e.g.
                               examples/10-scene-previewer/scenes/SmallVox
```

`resources.json` shader paths are relative to the working directory: change
`./helloRenderer/helloShaders/hello.bin` to `./renderer/helloShaders/hello.bin`.

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app CXX)
find_package(ProjectV CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE ProjectV::projectV)
projv_compile_shaders(TARGET my_app SHADER_DIRS renderer/helloShaders)
```

In-tree, use `projv_add_example(my_app SOURCES main.cpp SHADERS renderer/helloShaders ASSET_DIRS renderer scene)`,
which also stages the assets beside the binary.

## 2. Startup: window, scene, renderer, upload

```cpp
#include "core/application.h"
#include "core/paths.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/input.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "utils/compose_io.h"

using namespace projv;

struct Camera { core::vec3 position{0, 20, -60}; core::vec3 direction{0, 0, 1}; };

void startup(Application& app) {
    auto& window = app.world.ctx().emplace<graphics::RenderInstance>();
    window.initialize(1280, 720, "My App");                     // GLFW window + bgfx
    graphics::installPlatform(app, window);                     // Input, close button -> quit

    const auto here = core::executableDirectory();
    auto& scene = app.world.ctx().emplace<Scene>(
        utils::loadComposeFromDisk((here / "scene").string()));  // compose.json + .data
    app.world.ctx().emplace<Camera>();

    auto spec = graphics::loadRendererSpecification((here / "renderer").string() + "/");
    window.addRendererSpecification(1, spec);
    auto vertexShader = graphics::loadShader((here / "renderer/helloShaders/vs_quad.bin").string());
    window.setActiveRenderer(
        graphics::constructRendererSpecification(window.getRendererSpecification(1), vertexShader));

    app.world.ctx().emplace<GPUData>(graphics::createTexturesForScene(scene));   // upload
}
```

| Call | Header | What it does |
|---|---|---|
| `RenderInstance::initialize` | [`graphics/render_instance.h`](../../include/graphics/render_instance.h) | Opens the window and starts bgfx |
| `installPlatform` | [`graphics/input.h`](../../include/graphics/input.h) | Polls the window in PreUpdate, fills `Input`, sends `CloseRequested` |
| `loadComposeFromDisk` | [`utils/compose_io.h`](../../include/utils/compose_io.h) | Reads a compose folder into a `Scene` |
| `loadRendererSpecification` | [`graphics/disk_io.h`](../../include/graphics/disk_io.h) | Reads `render.json` + `resources.json` |
| `constructRendererSpecification` | [`graphics/manage_resources.h`](../../include/graphics/manage_resources.h) | Creates the GPU objects the description names |
| `createTexturesForScene` | [`graphics/gpu_interface.h`](../../include/graphics/gpu_interface.h) | Uploads the scene |

## 3. Update: move the camera

```cpp
#include "graphics/input.h"

void update(Application& app) {
    const auto& input = app.world.ctx().get<Input>();
    auto& camera = app.world.ctx().get<Camera>();
    const float step = 20.0f * app.time().delta;                 // units per second

    if (input.down(Key::W)) camera.position += camera.direction * step;
    if (input.down(Key::S)) camera.position -= camera.direction * step;
}
```

`Input` is filled in PreUpdate. Mouse look: [`examples/00-hello-voxel/main.cpp`](../../examples/00-hello-voxel/main.cpp).

## 4. Render: uniforms, then draw

```cpp
void render(Application& app) {
    auto& window = app.world.ctx().get<graphics::RenderInstance>();
    auto& camera = app.world.ctx().get<Camera>();
    auto renderer = window.getActiveRenderer();

    core::vec2 resolution = window.getWindowResolution();
    graphics::setUniformToValue(renderer, "cameraPos", camera.position);
    graphics::setUniformToValue(renderer, "cameraDir", camera.direction);
    graphics::setUniformToValue(renderer, "windowRes", resolution);

    graphics::renderConstructedRenderer(window, renderer, &app.world.ctx().get<GPUData>());
}
```

!!! warning "Pass non-const values to `setUniformToValue`"
    It picks the upload by the deduced type. A `const vec3` matches nothing, logs
    `Typename T for data is unknown` and the uniform never reaches the shader. Copy to a local
    first.

## 5. Shutdown and main

```cpp
void shutdown(Application& app) {
    graphics::destroyGPUData(app.world.ctx().get<GPUData>());   // bgfx/GLFW go with RenderInstance
}

int main() {
    Application app;
    app.addSystem(Stage::Startup,  "startup",  startup);
    app.addSystem(Stage::Update,   "camera",   update);
    app.addSystem(Stage::Render,   "render",   render);
    app.addSystem(Stage::Shutdown, "shutdown", shutdown);
    app.run();      // returns when the window closes
}
```

## 6. Run

Run from a directory holding `renderer/` and `scene/`. An empty frame with `Loaded 0 chunk(s)` in the
log means the loader rejected the scene's `.data` ([known issues](../plans/known-latent-issues.md)).

## Next

| To... | Read |
|---|---|
| Make things move and save gameplay data | [Entities](../concepts/entities.md), [`examples/15-entities`](../../examples/15-entities/) |
| Change voxels at runtime | [Edit voxels](../guides/edit-voxels.md) |
| Make it look better | [Rendering](../concepts/rendering.md), [`examples/30-renderers`](../../examples/30-renderers/) |
| Add physics | [Add physics](../guides/add-physics.md) |
