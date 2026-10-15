# graphics

The window and input (GLFW), bgfx, renderer descriptions, uploading the scene, and drawing.
Namespace `projv::graphics`; `Input`, `Key` and `MouseButton` are in `projv`.

| Belongs in graphics? | |
|---|---|
| ✅ `graphics::renderShaderToTargetBuffer()`, `graphics::passSceneToGPU()` | Rendering, GPU upload |
| ❌ `graphics::createVoxelScene()` | `utils/` |

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

## `graphics/input.h`

[`include/graphics/input.h`](../../include/graphics/input.h): keyboard and mouse as a resource.

| Name | What |
|---|---|
| `Input` | `down` / `pressed` / `released` for `Key` and `MouseButton`; `cursor`, `cursorDelta`, `scroll`, `cursorCaptured`, `wantCaptureKeyboard`/`Mouse` |
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

## `graphics/manage_resources.h`

[`include/graphics/manage_resources.h`](../../include/graphics/manage_resources.h): turning a
description into GPU objects, and keeping render targets sized.

| Name | What |
|---|---|
| `constructRendererSpecification(spec, vertexShader)` | → `std::shared_ptr<ConstructedRenderer>` |
| `resizeRenderTargets(textures, framebuffers, w, h)` | Bring relative targets to `scale × (w, h)`; true if any changed (drop history) |
| `resolveTargetSize(textures, framebuffers, fboID, backBuffer)` | The size a pass writing `fboID` draws at |
| `constructTextures`, `constructFramebuffers`, `constructUniforms`, `constructShaders`, `constructRenderPasses`, `createShaderProgram`, `getTextureAttachments` | The steps |

## `graphics/perform_renderer.h`

[`include/graphics/perform_renderer.h`](../../include/graphics/perform_renderer.h): running a
renderer for one frame.

| Name | What |
|---|---|
| `renderConstructedRenderer(renderInstance, renderer, &gpuData)` | Poll, upload uniforms, size targets to the window, run the passes, `bgfx::frame()` |
| `performRenderPasses(toPrimary, renderer, renderInstance, w, h, view, proj, &gpuData)` | The passes alone: for drawing into a panel |
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
