# Renderer folder

A renderer is a folder: `resources.json` declares what exists, `render.json` says what runs in which
order, and the shaders do the work. Loaded by `graphics::loadRendererSpecification(folder)`
([`graphics/disk_io.h`](../../../include/graphics/disk_io.h)). Concepts:
[Rendering](../../concepts/rendering.md). Walkthrough: [Write a renderer](../../guides/write-a-renderer.md).

```text
myRenderer/
├── resources.json      uniforms, shaders, textures, framebuffers
├── render.json         the passes, in order
└── shaders/
    ├── vs_quad.sc      the full-screen-quad vertex shader (copy one from an example)
    ├── varying.def.sc
    ├── main.frag  →  main.bin      compiled by projv_compile_shaders()
    └── post.frag  →  post.bin
```

## resources.json

```json
{
  "uniforms": [
    { "name": "cameraPos", "type": "vec4" },
    { "name": "viewProj",  "type": "mat4x4" }
  ],
  "shaders": [
    { "shaderID": 1, "path": "./myRenderer/shaders/main.bin" },
    { "shaderID": 2, "path": "./myRenderer/shaders/post.bin" }
  ],
  "textures": [
    { "texID": 1, "name": "blueNoise", "format": "RGBA8",        "sizeMode": "fixed", "resX": 256, "resY": 256, "origin": "CPUBuffer" },
    { "texID": 2, "name": "color",     "format": "RGBA16_FLOAT", "sizeMode": "relative", "scale": 1.0, "origin": "CreateNew" },
    { "texID": 3, "name": "normal",    "format": "RGBA16_FLOAT", "sizeMode": "relative", "scale": 1.0, "origin": "CreateNew" }
  ],
  "framebuffers": [
    { "fboID": 1, "textureIDs": [2, 3] }
  ]
}
```

### `uniforms`

| Key | Values |
|---|---|
| `name` | The name the shader declares (`uniform vec4 cameraPos;`) and `setUniformToValue` sets |
| `type` | `"vec4"`, `"mat3x3"`, `"mat4x4"` |

Engine uniforms (`passTargetRes`, `passInputRes`, `pjvMotionSets`, `pjvAnimTime`, the scene's
`tree64Dims` etc.) are **not** declared here. See [Rendering](../../concepts/rendering.md#engine-owned-uniforms).

### `shaders`

| Key | Values |
|---|---|
| `shaderID` | > 0, referenced by passes |
| `path` | The compiled `.bin`, **relative to the working directory**, not to this folder |

### `textures`

| Key | Required | Values |
|---|---|---|
| `texID` | yes | > 0 |
| `name` | yes | The sampler name. A shader reads it as `SAMPLER2D(<name>, slot)`. |
| `format` | yes | `RGBA8`, `RGBA8_INT`, `RGBA8_UINT`, `RGBA16_FLOAT`, `RGBA16_INT`, `RGBA16_UINT`, `RGBA32_FLOAT`, `RGBA32_INT`, `RGBA32_UINT`, `RGB8_INT`, `RGB8_UINT`, `RG8_INT`, `RG8_UINT`, `RG16_FLOAT`, `RG16_INT`, `RG16_UINT`, `RG32_FLOAT`, `RG32_INT`, `RG32_UINT`, `R1_INT`, `R8_INT`, `R8_UINT`, `R16_FLOAT`, `R16_INT`, `R16_UINT`, `R32_FLOAT`, `R32_INT`, `R32_UINT` |
| `sizeMode` | yes | `"relative"`: `scale` × the render resolution, rounded up. `"fixed"`: `resX` × `resY` for life. |
| `scale` | relative | > 0, default 1.0. 0.5 is half on each axis. |
| `resX`, `resY` | fixed | Pixel size |
| `origin` | yes | `"CreateNew"`: a render target, created by the engine. `"CPUBuffer"`: filled from the CPU with `graphics::setTextureToData`. |
| `readBack` | no | `true` creates it blit-able and readable back to the CPU (`bgfx::readTexture`) |

### `framebuffers`

| Key | Values |
|---|---|
| `fboID` | Referenced by passes. `-1` is reserved for the window. |
| `textureIDs` | The attachments, **in order**: output 0, 1, 2, ... All must share one size rule. |

## render.json

```json
{
  "renderer": [
    { "shaderID": 1, "frameBufferInputIDs": [],  "resourceTexturesIDs": [1], "frameBufferOutputID": 1,  "multiPass": 0 },
    { "shaderID": 2, "frameBufferInputIDs": [1], "resourceTexturesIDs": [],  "frameBufferOutputID": -1, "multiPass": 0 }
  ]
}
```

| Key | Values |
|---|---|
| `shaderID` | The fragment shader this pass runs |
| `frameBufferInputIDs` | Framebuffers whose textures this pass reads |
| `resourceTexturesIDs` | Other textures it reads (typically `CPUBuffer` ones) |
| `frameBufferOutputID` | Where it writes; `-1` is the window |
| `multiPass` | Run this pass this many extra times. Each copy gets a `multiPassPassNumber<i>` uniform (`i` = its index in the expanded pass list) holding its repeat number. |

Every key is required. Passes run in array order, every frame. A pass draws at the size of its
output.

### Ping-pong (feedback)

A framebuffer that some pass **both reads and writes** is detected at load and double-buffered:
the pass reads last frame's copy and writes the other. Nothing to declare.

## Binding rules in shaders

### Reading

Inputs bind to sampler slots `0, 1, 2, ...` in this order:

1. each framebuffer in `frameBufferInputIDs`, in order, its textures in its `textureIDs` order;
2. then each texture in `resourceTexturesIDs`, in order.

```text
pass:  frameBufferInputIDs [1]   resourceTexturesIDs [1]
fbo 1: textureIDs [2, 3]

slot 0 = texture 2 "color"    slot 1 = texture 3 "normal"    slot 2 = texture 1 "blueNoise"
```

```glsl
SAMPLER2D(color,     0);
SAMPLER2D(normal,    1);
SAMPLER2D(blueNoise, 2);
// integer formats: USAMPLER2D / ISAMPLER2D, read with texelFetch
```

Slots 0–8 are yours; 9–15 hold the scene. A tenth input overwrites a scene texture (the engine warns
once).

### Writing

| Output | Write with |
|---|---|
| A framebuffer | `gl_FragData[i]`, where `i` is the texture's **position in that framebuffer's `textureIDs`**, not its `texID` |
| `-1` (the window) | `gl_FragColor` |

```glsl
gl_FragData[0] = vec4(litColor, 1.0);   // texture 2, "color"
gl_FragData[1] = vec4(normal, 0.0);     // texture 3, "normal"
```

## What the loader refuses

`loadRendererSpecification` throws `std::runtime_error` with the texture or framebuffer named, for:

| Problem | Message says |
|---|---|
| A texture still has `"resizable"` | Replaced by `sizeMode`; how to migrate (window-following → `relative`, everything else → `fixed`) |
| No `sizeMode` | Use `relative` or `fixed` |
| Unknown `sizeMode` | Expected `relative` or `fixed` |
| `relative` with `scale` ≤ 0 | A scale must be greater than zero |
| `fixed` with no `resX`/`resY` | Nothing to be fixed at |
| A framebuffer lists a `textureID` no texture declares | Which one |
| A framebuffer's attachments have different size rules | The two textures; all must share `sizeMode` and `scale` or `resX`/`resY` |
| A shader file can't be opened | `failed to open file!` |

A `shaderID` or `texID` ≤ 0 is logged as an error but not refused.

## Examples

| Renderer | Shows |
|---|---|
| [`examples/00-hello-voxel/helloRenderer`](../../../examples/00-hello-voxel/) | One pass, no textures |
| [`examples/30-renderers/renderers/taa`](../../../examples/30-renderers/renderers/taa/) | G-buffer, history ping-pong, denoise, display |
| [`examples/40-advanced-renderer/advancedRenderer`](../../../examples/40-advanced-renderer/) | Fourteen passes |
