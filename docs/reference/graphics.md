# graphics

The window and input (GLFW), bgfx, renderer descriptions, uploading the scene, and drawing.
Namespace `projv::graphics`; `Input`, `Key` and `MouseButton` are in `projv`.

| Belongs in graphics? | |
|---|---|
| ✅ `graphics::renderConstructedRenderer()`, `graphics::createTexturesForScene()` | Rendering, GPU upload |
| ❌ `graphics::loadComposeFromDisk()` | `utils/` |

Explained in: [Rendering](../concepts/rendering.md), [Editing → Getting it to the GPU](../concepts/editing.md#getting-it-to-the-gpu).
Formats: [renderer folder](formats/renderer-folder.md).

## `graphics/render_instance.h`

[`include/graphics/render_instance.h`](../../include/graphics/render_instance.h): the window,
bgfx, and the active renderer.

| Name | What |
|---|---|
| `RenderInstance::initialize(w, h, name)` | Open the window, start bgfx |
| `getWindowResolution()` | |
| `addRendererSpecification(id, spec)`, `getRendererSpecification(id)`, `removeRendererSpecification(id)` | Keep renderer descriptions by id |
| `setActiveRenderer(constructed)`, `getActiveRenderer()` | The one `ConstructedRenderer` in use |
| `window`, `shouldClose`, `pollEventsInRender` | The GLFW window; close flag; whether rendering polls events |
| `resizable` | Set before `initialize`; false asks for exactly the size given (a tiling window manager floats it), for captures and benchmarks |
| `addOverlay(order, fn)`, `drawOverlays(size)` | Drawn over the frame after the renderer's passes (the UI's layers); ascending `order` |

## `graphics/input.h`

[`include/graphics/input.h`](../../include/graphics/input.h): keyboard and mouse as a resource.

| Name | What |
|---|---|
| `Input` | `down` / `pressed` / `released` for `Key` and `MouseButton`; `cursor`, `cursorDelta`, `scroll`, `cursorCaptured`; `text` (this frame's typed code points); `wantCaptureKeyboard`/`Mouse` (a UI layer took it: [ui](ui.md)) |
| `Key`, `MouseButton` | Engine key names, mapped from GLFW once |
| `installPlatform(app, renderInstance)` | PreUpdate system: poll, fill `Input`, send `CloseRequested` / `WindowResized` |
| `setCursorCaptured(app, renderInstance, bool)` | Mouse-look on/off |

## `graphics/disk_io.h`

[`include/graphics/disk_io.h`](../../include/graphics/disk_io.h): reading renderer folders and shaders.

| Name | What |
|---|---|
| `loadRendererSpecification(folder)` | `render.json` + `resources.json` → `RendererSpecification` |
| `loadShader(path)` | A compiled `.bin` → `bgfx::ShaderHandle` |
| `loadUniformTypes`, `loadShaders`, `loadTextures`, `loadFrameBuffers`, `loadRenderPasses` | The parts, from parsed JSON |
| `validateFrameBufferAttachmentSizes` | Throws if one framebuffer's attachments disagree on size |
| `readFile(path)` | |

## `graphics/camera.h`

[`include/graphics/camera.h`](../../include/graphics/camera.h): the engine's camera model on the CPU,
the same arithmetic as [`pjv_camera.sc`](shader-library.md) on the GPU.

| Name | What |
|---|---|
| `setCamera(renderer, CameraView)` | What the renderer draws from next. The driver uploads it, and what that renderer last drew from as the previous frame, before every pass |
| `cameraBasis(forward)` | `{forward, right, up}`: no roll; world up +y, or +z within 0.999 of vertical |
| `cameraRayOrigin`, `cameraRayDirection(camera, uv, resolution)` | The ray through a pixel (uv top-left origin); orthographic rays start on the plane `orthographicBackoff` behind the camera |
| `cameraWorldToUV(camera, point, resolution, &inFront)` | Where a point lands: the ray's inverse |
| `cameraUniformValues(camera, previous)` | The seven `pjvCamera*` uniforms' values, in order; `pjvCameraLens.w` is `tan(fov / 2)`, in double precision |

```cpp
projv::CameraView view;
view.position = eye;
view.forward = direction;           // need not be unit length
graphics::setCamera(*renderer, view);
```

## `graphics/basic_renderer.h`

[`include/graphics/basic_renderer.h`](../../include/graphics/basic_renderer.h): the engine's basic
renderer, a way to see a scene without writing one. One ray a pixel, the voxel's colour under a sun and
sky; its shader is embedded (`basic`, below). Guide: [Draw a scene](../guides/draw-a-scene.md).

| Name | What |
|---|---|
| `installBasicRenderer(app, renderInstance, options)` | Creates it, makes it active, emplaces `BasicRenderer` and a `CameraView`, and adds the Render system `"basic renderer"`: flush the `Scene` into the `GPUData`, draw from the `CameraView` (all `world.ctx()`) |
| `BasicRendererOptions` | The look: `sunDirection`, `sunColour`, `ambientAbove`/`Below`, `skyAbove`/`Below` (sRGB), `crosshair` (until programs draw UI) |
| `BasicRenderer` | The `world.ctx()` resource: `renderer`, `options` (change any time) |
| `createBasicRenderer()` | → `std::shared_ptr<ConstructedRenderer>`, or nullptr (logged); bgfx must be up |
| `drawBasicRenderer(renderInstance, renderer, gpu, view, options)` | One frame, submitted; uploads nothing |

```cpp
graphics::installBasicRenderer(app, window, {.crosshair = true});
app.world.ctx().get<CameraView>() = view;   // each frame
```

## `graphics/engine_shaders.h`

[`include/graphics/engine_shaders.h`](../../include/graphics/engine_shaders.h): the engine's own
shaders ([`shaders/engine/`](../../shaders/engine/basic.frag)), compiled into the library per profile.
Nothing is staged or read from disk.

| Name | What |
|---|---|
| `loadEngineShader(name)` | → `bgfx::ShaderHandle` for the running renderer; `BGFX_INVALID_HANDLE` (logged) if absent |
| `engineShader(name)`, `engineShader(name, rendererType)` | The bytes; empty if absent |

| Shader | What |
|---|---|
| `vs_quad` | The fullscreen quad every pass draws: the vertex shader for `constructRendererSpecification` |
| `basic` | The basic renderer's fragment shader ([`basic_renderer.h`](#graphicsbasic_rendererh)) |

```cpp
auto renderer = graphics::constructRendererSpecification(spec, graphics::loadEngineShader("vs_quad"));
```

Profiles: `PROJV_ENGINE_SHADER_PROFILES` (`spirv` off Apple, `metal` on it), what
`RenderInstance` starts bgfx on.

## `graphics/manage_resources.h`

[`include/graphics/manage_resources.h`](../../include/graphics/manage_resources.h): turning a
description into GPU objects, and keeping render targets sized.

| Name | What |
|---|---|
| `constructRendererSpecification(spec, vertexShader)` | → `std::shared_ptr<ConstructedRenderer>` |
| `resizeRenderTargets(textures, framebuffers, w, h)` | Bring relative targets to `scale × (w, h)`; true if any changed (drop history) |
| `resolveTargetSize(textures, framebuffers, fboID, backBufferSize)` | The size a pass writing `fboID` draws at |
| `constructTextures`, `constructFramebuffers`, `constructUniforms`, `constructShaders`, `constructRenderPasses`, `createShaderProgram`, `getTextureAttachments` | The steps |

## `graphics/perform_renderer.h`

[`include/graphics/perform_renderer.h`](../../include/graphics/perform_renderer.h): running a
renderer for one frame.

| Name | What |
|---|---|
| `renderConstructedRenderer(renderInstance, renderer, &gpuData)` | Poll, upload uniforms, size targets to the window, run the passes, `endFrame` |
| `endFrame(renderInstance, backBufferSize)` | Draw the overlays, then `bgfx::frame()`: what a program that submits its own passes ends with |
| `performRenderPasses(renderToPrimary, renderer, renderInstance, w, h, view, proj, &gpuData)` | The passes alone: for drawing into a panel |
| `updateUniforms(handles, values)` | Upload uniform values |

## `graphics/gpu_interface.h`

[`include/graphics/gpu_interface.h`](../../include/graphics/gpu_interface.h): the scene's GPU
mirror, and setting uniforms and textures.

| Name | What |
|---|---|
| `createTexturesForScene(scene)` | First upload → `GPUData` |
| `flushSceneUpdates(scene, gpu)` | Incremental: dirty blobs, dirty headers, small tables |
| `rebuildSceneTextures(scene, gpu)` | Full repack, samplers kept |
| `updateChunkHeader(scene, gpu, chunk)` | One header row |
| `updatePaletteEntry(scene, gpu, component, slot)` | One palette texel |
| `destroyGPUData(gpu)` | |
| `setUniformToValue(renderer, name, value)` | `float`/`vec2`/`vec3` (widened), `vec4`, `mat3`, `mat4`; pass a non-const value |
| `setTextureToData(renderer, texID, data, w, h)` | Fill a `CPUBuffer` texture |
| `createArbitraryTexture`, `createHeaderTexture` | RGBA32U textures from CPU arrays |

## `graphics/range_allocator.h`

[`include/graphics/range_allocator.h`](../../include/graphics/range_allocator.h): first-fit
free-list over a 1-D range; the persistent layout of the GPU data textures.

| Name | What |
|---|---|
| `RangeAllocator::reset(capacity)`, `reserve(offset, len)`, `alloc(len)`, `free(offset, len)`, `largestFreeRun()` | `alloc` returns `INVALID` when full; the caller grows the texture |

## `graphics/type_mapping.h`

[`include/graphics/type_mapping.h`](../../include/graphics/type_mapping.h): JSON strings to bgfx enums.

| Name | What |
|---|---|
| `getFormat("RGBA16_FLOAT")` | Texture format |
| `getUniformType("vec4")`, `mapUniformType` | Uniform types (`vec4`, `mat3x3`, `mat4x4`) |
| `getOrigin("CPUBuffer")` | `CPUBuffer` or `CreateNew` |
| `getDependenciesList(framebuffers, samplers, pass)` | A pass's inputs in binding order |
