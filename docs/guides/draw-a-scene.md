# Draw a scene

See a scene without writing a renderer: the engine's basic renderer. Header:
[`graphics/basic_renderer.h`](../../include/graphics/basic_renderer.h). Concepts:
[Rendering](../concepts/rendering.md).

One primary ray a pixel, the voxel's colour under a sun and a sky, straight to the window. No shadows,
AO or anti-aliasing. Its shader is compiled into the library
([`shaders/engine/basic.frag`](../../shaders/engine/basic.frag)), so there is no folder to stage and no
shader to compile. The feature examples draw with it.

```text
 Startup                                  every frame, Render
 ─────────                                ──────────────────────────────────────────────
 installBasicRenderer ──▶ "basic renderer" system:  flushSceneUpdates(Scene, GPUData)
                                                    draw from CameraView        (all world.ctx())
```

## Install

```cpp
#include "graphics/basic_renderer.h"

void startup(Application& app) {
    auto& window = app.world.ctx().emplace<graphics::RenderInstance>();
    window.initialize(1280, 720, "My App");
    graphics::installPlatform(app, window);

    auto& scene = app.world.ctx().emplace<Scene>(utils::loadComposeFromDisk("scene"));
    app.world.ctx().emplace<GPUData>(graphics::createTexturesForScene(scene));
    graphics::installBasicRenderer(app, window);
}
```

- **After `initialize`**: it creates GPU objects, so bgfx must be up.
- **It emplaces a `CameraView`** (if there is none) and a `graphics::BasicRenderer`, and makes the
  renderer the active one.
- **The Scene's changes are uploaded for you** each frame. Do not also call `flushSceneUpdates`.

## Point the camera

```cpp
void camera(Application& app) {
    auto& view = app.world.ctx().get<CameraView>();
    view.position = eye;
    view.forward = direction;            // need not be unit length
}

app.addSystem(Stage::Update, "camera", camera);
```

Set it in Update, or in Render ordered `{.before = "basic renderer"}`. A Render system that has to run
after the frame is drawn (timings, a screenshot) orders itself `{.after = "basic renderer"}`.

## The look

| `BasicRendererOptions` | Default | |
|---|---|---|
| `sunDirection` | `(0.35, 0.85, 0.4)` | Toward the sun; normalised when drawn |
| `sunColour` | `0.75` grey | Linear |
| `ambientAbove`, `ambientBelow` | `0.4`, `0.2` grey | Linear light on a face turned up, down; mixed by the normal |
| `skyAbove`, `skyBelow` | `(0.42, 0.55, 0.72)`, `(0.10, 0.11, 0.14)` | sRGB, as picked; mixed by the ray's height, squared |
| `crosshair` | `false` | Two bars through the centre. Temporary: goes when programs can draw UI ([ui.md](../plans/ui.md)) |

```cpp
graphics::installBasicRenderer(app, window, {.sunDirection = {-0.3f, 0.6f, 0.2f}, .crosshair = true});
app.world.ctx().get<graphics::BasicRenderer>().options.sunColour = {1.0f, 0.8f, 0.6f};   // any time
```

## By hand

Without the system, for a program that owns its frame:

```cpp
auto renderer = graphics::createBasicRenderer();                     // once; nullptr if unavailable
graphics::flushSceneUpdates(scene, gpu);                              // each frame
graphics::drawBasicRenderer(window, renderer, gpu, view, options);   // submits the frame
```

## Outgrowing it

The same renderer as a folder you can change is
[`examples/00-hello-voxel/renderer`](../../examples/00-hello-voxel/renderer/), loaded step by step in
[Your first app](../getting-started/first-app.md). From there: [Write a renderer](write-a-renderer.md).
