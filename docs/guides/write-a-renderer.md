# Write a renderer

From one pass to a G-buffer with accumulation. Concepts:
[Rendering](../concepts/rendering.md). Full format:
[renderer folder](../reference/formats/renderer-folder.md). Shader API:
[shader library](../reference/shader-library.md).

```text
myRenderer/
├── render.json          the passes, in order
├── resources.json       uniforms, shaders, textures, framebuffers
└── shaders/
    ├── varying.def.sc   copy from an example
    └── *.frag           one per pass
```

## 1. One pass to the window

Start from [`examples/shared/basicRenderer`](../../examples/shared/basicRenderer/), the renderer
[`examples/00-hello-voxel`](../../examples/00-hello-voxel/) draws with. `resources.json`
(strict JSON, no comments):

```json
{
  "uniforms": [ {"name": "windowRes", "type": "vec4"},
                {"name": "cameraPos", "type": "vec4"},
                {"name": "cameraDir", "type": "vec4"} ],
  "shaders":  [ {"shaderID": 1, "path": "./myRenderer/shaders/main.bin"} ],
  "textures": [],
  "framebuffers": []
}
```

`render.json`:

```json
{ "renderer": [
  { "shaderID": 1, "frameBufferInputIDs": [], "resourceTexturesIDs": [],
    "frameBufferOutputID": -1, "multiPass": 0 }
] }
```

- **Shader paths are relative to the working directory**, not to the folder.
- **`-1` is the window.** A pass writing the window uses `gl_FragColor`.
- **Every pass key is required**, `multiPass` included.

`main.frag`: [basic.frag](../../examples/shared/basicRenderer/shaders/basic.frag)
(`rayStartDirection` → `raySceneIntersect` → `fetchVoxelColor`).

## 2. Compile the shaders

```cmake
projv_compile_shaders(TARGET my_app SHADER_DIRS myRenderer/shaders)
projv_compile_shaders(TARGET my_app SHADER_DIRS myRenderer/shaders INCLUDE_DIRS sharedShaders)
```

`vs_*.sc` → vertex, `*.frag` → fragment, `.bin` beside each; other `.sc` files are includes.
`pjv_utils_DDA.sc` and `bgfx_shader.sh` are always on the path. In-repo:
`projv_add_example(... SHADERS dir SHADER_INCLUDE_DIRS dir)`.

## 3. A G-buffer: two passes

```json
"textures": [
  {"texID": 1, "name": "albedo", "format": "RGBA8",        "sizeMode": "relative", "scale": 1.0, "origin": "CreateNew"},
  {"texID": 2, "name": "normal", "format": "RGBA16_FLOAT", "sizeMode": "relative", "scale": 1.0, "origin": "CreateNew"}
],
"framebuffers": [ {"fboID": 1, "textureIDs": [1, 2]} ]
```

```json
"renderer": [
  { "shaderID": 1, "frameBufferInputIDs": [],  "resourceTexturesIDs": [], "frameBufferOutputID": 1,  "multiPass": 0 },
  { "shaderID": 2, "frameBufferInputIDs": [1], "resourceTexturesIDs": [], "frameBufferOutputID": -1, "multiPass": 0 }
]
```

```glsl
// pass 1: gbuffer.frag. One output per framebuffer texture, in the framebuffer's order
gl_FragData[0] = vec4(albedo, 1.0);          // texID 1
gl_FragData[1] = vec4(normal * 0.5 + 0.5, 1.0);   // texID 2

// pass 2: shade.frag. Samplers in binding order, from slot 0
SAMPLER2D(albedo, 0);
SAMPLER2D(normal, 1);
```

**Binding order**: each input framebuffer's textures in its `textureIDs` order, then the pass's
`resourceTexturesIDs`. Slots matter, not sampler names; adding a texture to a framebuffer shifts every
later slot.

## 4. Accumulation: read your own output

A pass that lists its output framebuffer as an input gets a ping-pong pair automatically:

```json
{ "shaderID": 3, "frameBufferInputIDs": [1, 2], "resourceTexturesIDs": [], "frameBufferOutputID": 2, "multiPass": 0 }
```

```glsl
SAMPLER2D(current, 0);      // fbo 1
SAMPLER2D(history, 1);      // fbo 2: last frame's
uniform vec4 frameCount;    // x: frames since reset, y: 1 on the frame the camera moved

void main() {
    vec3 now  = texture2D(current, v_texcoord0).rgb;
    vec3 past = texture2D(history, v_texcoord0).rgb;
    float n = max(frameCount.x, 1.0);
    gl_FragColor = vec4(frameCount.y != 0.0 ? now : mix(past, now, 1.0 / n), 1.0);
}
```

The program owns the reset (camera moved, `resizeRenderTargets` returned true). [`pt-reference`](../../examples/30-renderers/renderers/pt-reference/shaders/accumulate.frag)
is this pattern; [`restir-pt`](../../examples/30-renderers/renderers/restir-pt/) reprojects with motion
vectors instead of resetting.

## 5. A texture from the CPU

`"origin": "CPUBuffer"`, listed in `resourceTexturesIDs`, filled after construction:

```json
{"texID": 3, "name": "blueNoise", "format": "RGBA8", "resX": 256, "resY": 256, "sizeMode": "fixed", "origin": "CPUBuffer"}
```

```cpp
int w, h, channels;
unsigned char* pixels = stbi_load("LDR_RGBA_7.png", &w, &h, &channels, 4);
graphics::setTextureToData(renderer, 3, pixels, w, h);        // texID 3
stbi_image_free(pixels);
```

## 6. Resolution

| Declare | Size |
|---|---|
| `"sizeMode": "relative", "scale": 0.5` | Scale × the render resolution (window or panel) |
| `"sizeMode": "fixed", "resX": 256, "resY": 256` | Fixed |

- **A pass draws at its output's size.** One framebuffer's textures must share a sizing rule (the
  loader throws).
- **Use `passTargetRes`** `(w, h, 1/w, 1/h)` for anything in this pass's pixels, not `windowRes`,
  which is wrong once passes differ in size.
- **`passInputRes[i]`**: the same for the input at slot `i`.

```glsl
uniform vec4 passTargetRes;
uniform vec4 passInputRes[8];
vec2 oneTargetPixel = passTargetRes.zw;
vec2 oneSourceTexel = passInputRes[0].zw;
```

[`30-renderers/renderers/reconstruction`](../../examples/30-renderers/renderers/reconstruction/)
traces at half resolution and reconstructs to full.

## 7. Drive it from C++

```cpp
auto renderer = renderInstance.getActiveRenderer();
core::vec4 frame{float(frames), float(cameraMoved), 0, 0};      // non-const locals
graphics::setUniformToValue(renderer, "frameCount", frame);
graphics::renderConstructedRenderer(renderInstance, renderer, &gpuData);

renderer->dependencyGraph[2].enabled = false;                    // skip pass 2 outright
```

`setUniformToValue` takes non-const `float`/`vec2`/`vec3` (widened to `vec4`), `vec4`, `mat3`,
`mat4`. A `const` value matches nothing and is dropped with an error.

## Limits

- **Nine inputs per pass** (slots 0–8). The scene holds stages 9–15; a tenth input replaces a scene
  texture (warned once).
- **Uniforms are `vec4`, `mat3x3` or `mat4x4`.**
- **One `raySceneIntersect` call site per shader.** A second inlined one made every mode ~2× slower;
  branch inside one call.

## Debugging

| Symptom | Look at |
|---|---|
| Black / garbage in one input | Binding order: count slots across the input framebuffers, then resource textures |
| A target stays 1×1 or the wrong size | Something must call `resizeRenderTargets` each frame (`renderConstructedRenderer` does) |
| A uniform has no effect | Declared in `resources.json`? Set from a non-const value? Named the same in the shader? |
| Shader edit not picked up | A shared include outside the shader dir isn't tracked; delete the `.bin` files |
| Loader throws about `resizable` | Old format: use `"sizeMode"` (see the [format](../reference/formats/renderer-folder.md)) |
| Empty scene | `Loaded N chunk(s)` in the log, and `fetchVoxelColor` only on a hit |

Renderers to read, smallest first: [`00-hello-voxel`](../../examples/00-hello-voxel/),
[`10-scene-previewer`](../../examples/10-scene-previewer/), the renderers in
[`30-renderers`](../../examples/30-renderers/), then
[`40-advanced-renderer`](../../examples/40-advanced-renderer/) (fourteen passes).
