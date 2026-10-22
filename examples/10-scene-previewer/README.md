# 10 — Scene Previewer

A minimal, fast viewer for any Compose scene folder. It draws with the feature examples' [basic renderer](../shared/basicRenderer/): one primary ray per pixel, and the colour stored in the material palette under one fixed sun. No shadow ray, no ambient occlusion, no global illumination.

## Why

The full renderers answer "how does this scene look". This one answers "what is actually *in* this scene". Those are different questions, and the second one is the one you have after voxelizing something — a [MeshVoxelizer](../20-mesh-voxelizer/) import, a Minecraft world, a tree asset — when you want to know whether the data is right before looking at it in a better renderer.

Because nothing but plain N·L sits on top of the stored colour, what you see is what a voxelizer wrote. A material that reads wrong here is wrong in the **data**, not in the lighting — which makes this the fastest way to tell those two failure modes apart. Common things it catches immediately: a model that sampled the mirrored half of its texture atlas (see MeshVoxelizer's `--flip-v`), a palette that quantized badly, an import that came out uniformly grey because its textures never resolved.

## How to Build

```bash
cmake --preset dev
cmake --build --preset dev --target scene_previewer
```

The binary and its staged renderer folder and scenes land in `build/examples/scene_previewer/`.

## How to Use

```bash
cd build/examples/scene_previewer
./scene_previewer [scene-directory]
```

The scene directory is any folder holding a `compose.json` — whatever `loadComposeFromDisk` opens. With no argument it opens `scenes/StonehillCastle/`, so it runs out of the box.

```bash
./scene_previewer                                       # the bundled Minecraft castle
./scene_previewer scenes/Sibenik                        # another bundled scene
./scene_previewer ../renderer_gallery/SponzaScene/      # the Sponza atrium (see the caveat below)
./scene_previewer ~/voxelized/MyWorld/                  # anything you just voxelized
```

## Bundled scenes

`scenes/` covers the range of inputs [MeshVoxelizer](../20-mesh-voxelizer/) accepts — a real
Minecraft save, textured OBJ meshes at three very different triangle counts, and a Minecraft map
that arrives as a *mesh* rather than a save — alongside several scenes authored in
[60-scene-editor](../60-scene-editor/). Each is quantized to the 255-entry material palette, so it
loads without a single `internMaterial` complaint.

**Not all of them load.** `Sibenik` and `LostEmpire` are `.data` version 1, which the current
loader rejects; they open as zero chunks. `SmallVox` and `StonehillCastle` are the reliable ones to
reach for. The table below describes the files as they were authored, not all of which is what
currently loads — see the note under it.

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
cd build/examples/mesh_voxelizer

./mesh_voxelizer -f <world>/"Stonehill Castle - v1.3" -o ../10-scene-previewer/scenes/StonehillCastle \
    --mc-bounds -416 164 -224 356 --mc-y 70 200
./mesh_voxelizer -f <models>/sibenik.obj            -o ../10-scene-previewer/scenes/Sibenik    -r 512
./mesh_voxelizer -f <models>/Amazon-Lumberyard-Bistro/Exterior/exterior.obj \
                                                    -o ../10-scene-previewer/scenes/Bistro     -r 512
./mesh_voxelizer -f <models>/MinecraftMap/lost_empire.obj \
                                                    -o ../10-scene-previewer/scenes/LostEmpire -r 512
```

The Minecraft bounds are not arbitrary. A saved world is mostly underground stone, and importing a 512×512 area at full height blows past the voxel limit on rock nobody will ever see — so the selection is a 192×192 block box around the castle at (-320, 260) with `--mc-y` clamped to the surface. Finding it is a two-minute exercise in narrowing: import a wide area with a high `--mc-y` floor and read back the reported bounds, which localize whatever is tall.

| Input | Action |
|-------|--------|
| `W` / `S` | Move forward / backward |
| `A` / `D` | Strafe left / right |
| `R` / `F` | Move up / down |
| Mouse | Look around (cursor is captured; `Esc` releases it, left-click re-captures) |
| Scroll wheel | Movement speed (the lit renderers use this for the sun; the sun is fixed here) |
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

One pass: [`examples/shared/basicRenderer`](../shared/basicRenderer/), staged beside the binary as
`basicRenderer/` and set up by [`basic_renderer.h`](../shared/basic_renderer.h). It is the renderer every
feature example uses. It fires one ray per pixel through the voxel DDA (`pjv_utils_DDA.sc`) and shades
`fetchVoxelColor` by a fixed sun (N·L) and a little sky light, so every face is readable. There is no
anti-aliasing and no tone map, so a lit face shows the palette colour almost unchanged.

## ProjectV Features Used

| Feature | Usage |
|---------|-------|
| **Core ECS** | `Application`, `world`, global resources, system stage assignment (`Startup`/`Update`/`Render`) |
| **Core Math** | `vec2`/`vec3`/`vec4`, `min`/`max`/`length`, `cos`/`sin` — bounding box, framing, camera basis |
| **Logging** | `info`/`warn`/`perf` via the spdlog wrapper for the scene report and frame profiling |
| **Compose I/O** | `loadComposeFromDisk` — opens any Compose scene folder |
| **GPU Interface** | `createTexturesForScene`, `GPUData` — uploads the voxel scene to the GPU |
| **Render Instance** | Window creation, window resolution query; the renderer through `basic_renderer.h` |
| **Scene** | `Scene::chunks`, `ChunkHeader::position` / `scale` — read directly for automatic framing |

## Third-Party Dependencies

- [bgfx](https://github.com/bkaradzic/bgfx) — cross-platform GPU abstraction (via ProjectV)
- [GLFW](https://www.glfw.org/) — window and input handling (via bgfx/ProjectV)
