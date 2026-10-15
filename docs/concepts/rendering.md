# Rendering

A renderer is **data**: a folder of JSON and fragment shaders. The engine turns it into bgfx
objects and runs its passes each frame. Every pass is a full-screen quad, and the scene reaches the
shaders as textures they ray-march. Headers: [`graphics/`](../reference/graphics.md). Format:
[renderer folder](../reference/formats/renderer-folder.md).

## From folder to pixels

```text
renderer/                        loadRendererSpecification          constructRendererSpecification
├── render.json   (passes)  ──▶  RendererSpecification       ──▶  ConstructedRenderer
├── resources.json (uniforms,       { Resources,                    { bgfx textures, framebuffers,
│    shaders, textures, fbos)         DependencyGraph }               programs, uniforms, passes }
└── shaders/*.frag → *.bin                                                     │
                                                                                │ renderConstructedRenderer
Scene ──createTexturesForScene──▶ GPUData (scene textures) ─────────────────────┤  each frame
                                                                                ▼
                                                                    pass 1 → pass 2 → ... → window
```

```cpp
auto spec = graphics::loadRendererSpecification("renderer/");
renderInstance.addRendererSpecification(1, spec);
auto vs = graphics::loadShader("renderer/shaders/vs_quad.bin");
renderInstance.setActiveRenderer(
    graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vs));

// each frame
graphics::setUniformToValue(renderer, "cameraPos", cameraPos);
graphics::renderConstructedRenderer(renderInstance, renderer, &gpuData);
```

## What one frame does

`renderConstructedRenderer`:

1. Polls GLFW, unless `installPlatform` took that over.
2. Uploads every uniform value set with `setUniformToValue`.
3. Resets the back buffer if the window changed size, and `resizeRenderTargets` to the window.
4. `performRenderPasses`: for each pass in `render.json` order, binds the scene and the pass's
   inputs, sets the per-pass uniforms, and draws the quad into the pass's framebuffer.
5. `bgfx::frame()`.

A program rendering into an ImGui panel rather than the window calls `resizeRenderTargets` and
`performRenderPasses` itself with the panel's size. [`examples/60-scene-editor`](../../examples/60-scene-editor/) does.

## Passes

```json
{ "shaderID": 2, "frameBufferInputIDs": [1], "resourceTexturesIDs": [], "frameBufferOutputID": -1, "multiPass": 0 }
```

| Idea | How it works |
|---|---|
| **Output** | A framebuffer id, or `-1` for the window |
| **Inputs** | Framebuffers' textures first (in each framebuffer's order), then resource textures, bound to sampler slots 0, 1, 2, ... |
| **Size** | The output's size. Targets are `"relative"` (fraction of render resolution) or `"fixed"`. |
| **Feedback** | A pass reading its own output framebuffer gets a **ping-pong** pair automatically |
| **Repeats** | `"multiPass": N` runs the pass N more times. Each copy gets its own `multiPassPassNumber<i>` uniform (`i` = its index in the pass list) holding its repeat number. |
| **Skipping** | `BGFXDependencyGraph::enabled = false`: nothing submitted, the target keeps its contents |

## Texture stages

The scene occupies stages 9–15:

```text
stage:  0 1 2 3 4 5 6 7 8 │ 9           10              11         12        13       14         15
        pass inputs (≤ 9) │ materialIDs materialPalette looseList  gridInfo  cellMap  tree64Data headerData
```

A tenth input overwrites a scene texture (warned once per pass).

## Engine-owned uniforms

Declare them in the shader; not listed in `resources.json`:

| Uniform | Value |
|---|---|
| `passTargetRes` | `(w, h, 1/w, 1/h)` of this pass's target. Use it, not a renderer-wide `windowRes`, for per-pixel maths. |
| `passInputRes[8]` | The same for each bound input, by sampler slot |
| `tree64Dims`, `voxelTypeDims`, `paletteDims` | Sizes of the scene textures (used by `pjv_utils_DDA.sc`) |
| `pjvMotionSets[48]`, `pjvAnimTime` | The animation table and time (`GPUData::animation`) |

Your own uniforms are `vec4`/`mat3`/`mat4` in `resources.json`, set with `setUniformToValue`
(`float`/`vec2`/`vec3` are widened to `vec4`).

## Inside a shader

```glsl
#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>

uniform vec4 windowRes, cameraPos, cameraDir;

void main() {
    Ray ray;
    ray.origin    = cameraPos.xyz;
    ray.direction = rayStartDirection(v_texcoord0, windowRes.xy, cameraPos.xyz, normalize(cameraDir.xyz), 60.0);

    SceneIntersectData hit = raySceneIntersect(ray, pjvPrimaryQuery(100u)).hit;
    gl_FragColor = vec4(fetchVoxelColor(hit.foundBox, hit.headerIndex), 1.0);
}
```

Entry points, query options and materials: [Shader library](../reference/shader-library.md).

!!! warning "One march call site per shader"
    A second inlined `raySceneIntersect` (or `raySceneIntersectFrom`) call site roughly halved the
    speed of every mode of a renderer that had one. Branch inside one call instead.

## The scene on the GPU

`GPUData` ([`data_structures/gpuData.h`](../../include/data_structures/gpuData.h)):

| Texture | Holds |
|---|---|
| `tree64Data` | Every blob's tree64 (and envelope), suballocated by `RangeAllocator` |
| `materialIDs` | Every blob's material bytes |
| `materialPalette` | Every component's palette, one entry per texel; a chunk's header names its offset |
| `headerData` | One `GPUChunkHeader` row per chunk: placement, ranges, LOD, content box |
| `gridInfo`, `cellMap` | Grids and their cell → chunk maps |
| `looseList` | The loose chunks and the BVH over them |

Keeping it current: [Editing → Getting it to the GPU](editing.md#getting-it-to-the-gpu).

## The renderers in the repository

| Where | What |
|---|---|
| [`examples/00-hello-voxel/helloRenderer`](../../examples/00-hello-voxel/) | One pass, albedo plus a face tint |
| [`examples/10-scene-previewer`](../../examples/10-scene-previewer/) | Albedo only: shows what is in a scene |
| [`examples/30-renderers`](../../examples/30-renderers/) | Four renderers over one scene: accumulation, TAA, fast direct light, half-resolution reconstruction |
| [`examples/40-advanced-renderer`](../../examples/40-advanced-renderer/) | Fourteen passes: probe GI, animation, transparency, refraction |

Writing one: [Write a renderer](../guides/write-a-renderer.md).
