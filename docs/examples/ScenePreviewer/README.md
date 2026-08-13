# ScenePreviewer

A minimal, fast viewer for any Compose scene folder. It renders a scene's **pure albedos** — the colours stored in the material palette — with no lighting of any kind: no shadow ray, no ambient occlusion, no global illumination, no sky. One primary ray per pixel, and the voxel's colour written straight out.

It is derived from the [PathTracer](../PathTracer/) example's `fast` renderer, the cheapest of that example's six, with everything except the primary march removed.

## Why

The lit renderers answer "how does this scene look". This one answers "what is actually *in* this scene". Those are different questions, and the second one is the one you have after voxelizing something — a [MeshVoxelizer](../MeshVoxelizer/) import, a Minecraft world, a tree asset — when you want to know whether the data is right before spending path-tracer frames on it.

Because nothing is modulated by light transport, what you see is exactly what a voxelizer wrote. A material that reads wrong here is wrong in the **data**, not in the lighting — which makes this the fastest way to tell those two failure modes apart. Common things it catches immediately: a model that sampled the mirrored half of its texture atlas (see MeshVoxelizer's `--flip-v`), a palette that quantized badly, an import that came out uniformly grey because its textures never resolved.

## How to Build

```bash
cd ScenePreviewer
make               # builds ./scene_previewer
./compPreview.sh   # (re)compiles the three shaders to .bin
```

Requires ProjectV to be built at `../../../` with its libraries in `../../../lib/` and bgfx built.

## How to Use

```bash
./scene_previewer [scene-directory]
```

The scene directory is any folder holding a `compose.json` — whatever `loadComposeFromDisk` opens. With no argument it opens `scenes/StonehillCastle/`, so it runs out of the box.

```bash
./scene_previewer                                      # the bundled Minecraft castle
./scene_previewer scenes/Sibenik                       # another bundled scene
./scene_previewer ../MeshVoxelizer/trees/Oak_Leav      # a 64^3 asset
./scene_previewer ../PathTracer/SponzaScene/           # the Sponza atrium
./scene_previewer ~/voxelized/MyWorld/                 # anything you just voxelized
```

## Bundled scenes

`scenes/` holds four scenes covering the range of inputs [MeshVoxelizer](../MeshVoxelizer/) accepts — a real Minecraft save, textured OBJ meshes at three very different triangle counts, and a Minecraft map that arrives as a *mesh* rather than a save. All four are quantized to the 255-entry material palette, so they load without a single `internMaterial` complaint.

| Scene | Source | Voxels | Materials | Size |
|-------|--------|--------|-----------|------|
| `StonehillCastle` | Minecraft world (Java, 1.21) | 1,727,740 | 65 | 21 MB |
| `Sibenik` | `sibenik.obj` — 75 k tris, 3 textures | 1,301,862 | 241 | 16 MB |
| `Bistro` | Amazon Lumberyard Bistro exterior — 2.8 M tris, 114 textures | 983,852 | 255 | 1.6 MB |
| `LostEmpire` | `lost_empire.obj` — a Minecraft map exported as a mesh | 975,087 | 224 | 12 MB |

`Bistro` is the one mesh scene regenerated since the `.data` container went to version 2, which is why it is the only row measured in single-digit megabytes: v2 stores one material byte per voxel and moves the colors into `compose.json`, where v1 carried a full color per voxel.

`Sibenik` and `LostEmpire` are still version 1. They **load as zero chunks** — `readDataFile` rejects the container outright and says so — until they are re-run with the commands below, and the sizes quoted for them are v1 sizes that will drop by roughly the same factor when they are. `StonehillCastle` loads (10 chunks) because its `compose.json` references only v2 components; the 21 MB v1 `model.data` still sitting in that folder is an unreferenced leftover, and the figures in its row above describe that stale file rather than what currently loads.

To regenerate any of them:

```bash
cd ../MeshVoxelizer

./mesh_voxelizer -f <world>/"Stonehill Castle - v1.3" -o ../ScenePreviewer/scenes/StonehillCastle \
    --mc-bounds -416 164 -224 356 --mc-y 70 200
./mesh_voxelizer -f <models>/sibenik.obj            -o ../ScenePreviewer/scenes/Sibenik    -r 512
./mesh_voxelizer -f <models>/Amazon-Lumberyard-Bistro/Exterior/exterior.obj \
                                                    -o ../ScenePreviewer/scenes/Bistro     -r 512
./mesh_voxelizer -f <models>/MinecraftMap/lost_empire.obj \
                                                    -o ../ScenePreviewer/scenes/LostEmpire -r 512
```

The Minecraft bounds are not arbitrary. A saved world is mostly underground stone, and importing a 512×512 area at full height blows past the voxel limit on rock nobody will ever see — so the selection is a 192×192 block box around the castle at (-320, 260) with `--mc-y` clamped to the surface. Finding it is a two-minute exercise in narrowing: import a wide area with a high `--mc-y` floor and read back the reported bounds, which localize whatever is tall.

| Input | Action |
|-------|--------|
| `W` / `S` | Move forward / backward |
| `A` / `D` | Strafe left / right |
| `R` / `F` | Move up / down |
| Mouse | Look around (cursor is captured; `Esc` releases it, left-click re-captures) |
| Scroll wheel | Movement speed (the lit renderers use this for the sun; there is no sun here) |
| `H` | Re-frame the camera on the scene |

### Automatic framing

A previewer gets pointed at scenes of wildly different scale, so the camera is placed from the scene's own bounding box rather than hardcoded: back far enough that the bounding sphere fits the 60° vertical FOV, raised and turned to a three-quarter view. Movement speed is derived the same way — roughly two seconds to cross the scene — so the controls feel identical whether the subject is a 64³ tree or a 2048³ world. The bounds come from chunk headers only (position + scale), so this stays instant no matter how much geometry is loaded.

Both are reported at startup, along with the chunk and component counts:

```
[INF] Scene bounds: (0.0, 0.0, 0.0) -> (64.0, 64.0, 64.0)
[INF] Scene extents: 64.0 x 64.0 x 64.0 (radius 55.4)
[INF] Framed camera at (-47.7, 73.1, -47.7), move speed 0.92/frame
```

## Renderer

Three passes (`previewRenderer/render.json`), against the `fast` renderer's five and the tree64 path tracer's four:

1. **albedo** — one primary ray per pixel through the shared voxel DDA (`pjv_utils_DDA.sc`), writing `fetchVoxelColor` straight out. Sub-pixel jittered (Halton) so the next pass can resolve anti-aliased edges; that jitter is the only stochastic input in the entire renderer. Misses write a neutral dark gradient rather than the sky model, so a bright or tinted backdrop does not drag your perception of the albedos in front of it.
2. **accumulate** — a plain running mean of a still camera's jittered samples, reset the moment the camera moves. Not a denoiser and not a reprojecting TAA: the input is already noise-free, so all this does is anti-alias.
3. **display** — copies to the back buffer, deliberately doing nothing to the image. See below.

Per pixel that is **one** scene ray, against `2 + AO_SAMPLES` for the fast renderer and a full path for the others.

### No tone map, no gamma

The lit renderers ACES tone-map and gamma-encode on output because they produce open-ended HDR radiance. This one produces *reflectance*, already in `[0, 1]` and exactly the value in the material palette (`fetchVoxelColor` decodes R10G10B10 straight to 0–1). Tone mapping would desaturate and lift it; a gamma curve would wash it out relative to the source texture the voxelizer sampled. Either one means the previewer is no longer showing you the colour that is in the file, which is the one thing it exists to do — so the display pass is a straight copy.

If a scene's palette was authored linear rather than sRGB, set `PREVIEW_APPLY_GAMMA` to `1` at the top of `display.frag`.

### Reading shape in an unlit image

Pure albedo means every face of a voxel is the same colour, so form is carried entirely by silhouette and flat regions read as flat. That is the honest view of the data and it is the default. If you want form legible instead, set `PREVIEW_FACE_SHADING` to `1` at the top of `albedo.frag`: it multiplies in a fixed per-face gradient from the voxel normal — no rays, no light source, just a constant tint per axis. The image is then no longer the unmodified stored colour, which is why it is off by default.

## Note on the shader build scripts

`compPreview.sh` passes `-i $PROJECTV_DIR/include` so the engine's `pjv_utils_DDA.sc` resolves. The PathTracer example's `comp*.sh` scripts are currently missing that include path, so any shader of theirs that includes the DDA fails to compile (`Cannot open include file "pjv_utils_DDA.sc"`, followed by a cascade of parse errors on the now-undefined `Ray` / `RayQuery` / `raySceneIntersect`). Their committed `.bin` files still run, so it only bites when you recompile them.

## ProjectV Features Used

| Feature | Usage |
|---------|-------|
| **Core ECS** | `Application`, `world`, global resources, system stage assignment (`Startup`/`Update`/`Render`) |
| **Core Math** | `vec2`/`vec3`/`vec4`, `min`/`max`/`length`, `cos`/`sin` — bounding box, framing, camera basis |
| **Logging** | `info`/`warn`/`perf` via the spdlog wrapper for the scene report and frame profiling |
| **Compose I/O** | `loadComposeFromDisk` — opens any Compose scene folder |
| **GPU Interface** | `createTexturesForScene`, `GPUData` — uploads the voxel scene to the GPU |
| **Manage Resources** | `RendererSpecification`, `ConstructedRenderer`, `constructRendererSpecification` |
| **Disk I/O** | `loadRendererSpecification`, `loadShader` |
| **Render Instance** | Window creation, active renderer management, window resolution query |
| **Perform Renderer** | `renderConstructedRenderer`, `setUniformToValue` |
| **Scene** | `Scene::chunks`, `ChunkHeader::position` / `scale` — read directly for automatic framing |

## Third-Party Dependencies

- [bgfx](https://github.com/bkaradzic/bgfx) — cross-platform GPU abstraction (via ProjectV)
- [GLFW](https://www.glfw.org/) — window and input handling (via bgfx/ProjectV)

No blue-noise LUT and no `stb_image`: there is nothing stochastic to decorrelate beyond the Halton sub-pixel jitter, which is analytic, and no textures to load.
