# ObjVoxelizer

A command-line tool that converts Wavefront OBJ models into ProjectV's Compose scene format. It parses mesh geometry and material data, samples diffuse textures at each voxel's surface intersection using barycentric UV interpolation, and writes the result as a grid-volume `.data` container plus the `compose.json` that places it — a folder that `loadComposeFromDisk` can open directly.

## Overview

The voxelizer rasterizes each triangle in the input mesh into a 3D grid of voxels using the Separating Axis Theorem (SAT) for triangle/AABB intersection. Occupied voxels are written into a sparse **brick map**, which is then turned into a tree64 (`updateChunkFromBrickMap`) and flattened into the `.data` container's per-voxel color array.

The model is normalized so its longest axis fills the voxel grid, and the grid is split into 256³ chunks — one `.data` block per occupied chunk, addressed by grid coordinate. Empty chunks are skipped entirely, so a thin-shelled model costs only the blocks it actually touches.

## How to Build

```bash
cd ObjVoxelizer
make
```

Requires ProjectV to be built at `../../../` with its libraries installed in `../../../lib/`.

## How to Use

```
./obj_voxelizer -m <model-directory> -f <path-to-obj> -o <output-directory> -r <resolution>
```

| Flag | Description |
|------|-------------|
| `-m, --modelDir` | Root directory of the OBJ model (used for MTL and texture lookup) |
| `-f, --objDir` | Path to the `.obj` file |
| `-o, --outputDir` | Directory to write the generated voxel scene |
| `-r, --resolution` | Voxelization resolution along the longest axis (default: `256`) |
| `--no-flip-v` | Sample textures without flipping the V texture coordinate |

`-r` is rounded **up** to whole chunks, and the summary reports both the requested and the effective resolution. Chunks are 256³, so `-r 300` voxelizes at 512. Below that there is one smaller step: `-r 64` (or less) emits a **single 64³ chunk** rather than padding a small model out into a mostly-empty 256³ one.

64 is the floor. A chunk's resolution has to be a power of 4 — that is what sets the tree64's depth — and below 256 it must also be a whole brick, because the brick map's per-row bitmasks *are* the tree64's leaf level. A 16³ chunk would leave the tree the wrong depth.

Small assets are what this step is for: scattering props into a scene wants a tree that costs a few thousand voxels, not a few million.

### `--no-flip-v`

OBJ nominally places `v = 0` at the **bottom** of the image while stb_image returns the top row first, so the voxelizer flips V by default. Plenty of models — particularly ones converted out of 3ds Max — carry unflipped coordinates instead, and sampling those with the flip on lands on the mirrored half of the atlas: you get whatever happens to sit opposite the artwork, often filler or a second material's texture.

The symptom is unmistakable — foliage comes out uniformly gray, or a trunk takes on the leaves' color. If the colors look wrong in a way that suggests the model is sampling *somewhere else in the same image*, try `--no-flip-v`.

**Example:**

```bash
./obj_voxelizer -m ./myModel/ -f ./myModel/scene.obj -o ./outputScene/ -r 512
```

The output directory contains `model.data` (the PVDT container holding every occupied chunk) and `compose.json` (which places it at the origin). Point the engine at the **folder**:

```cpp
projv::Scene scene = projv::utils::loadComposeFromDisk("./outputScene/");
```

## Color Quantization

Voxels do not store colors. A voxel stores an 8-bit **material ID** into its component's material palette, and that palette is capped at 255 entries for the whole component — the voxelizer's entire output is one component. A textured mesh sampled naively yields tens or hundreds of thousands of distinct colors, so the loader would hit the cap, log `internMaterial: palette full`, and hand every remaining voxel `INVALID_MATERIAL`.

The voxelizer therefore builds a fitting palette **before** voxelizing:

1. A pre-pass walks the parsed triangles and samples each one exactly the way voxelization will, spending samples in proportion to **3D surface area** — voxel counts follow area, so this approximates the distribution of colors that will actually reach a voxel. Samples go into a 32³ RGB histogram.
2. **Median cut** splits that histogram into at most 255 boxes, each contributing one palette entry equal to the mean color of the samples inside it.
3. A 32³ lookup table resolves any sampled color to its nearest palette slot in O(1), so voxelization itself stays a single pass.

Weighting by area rather than histogramming whole texture images matters more than it sounds. An MTL commonly declares materials the mesh never uses, and atlases commonly contain large unsampled regions — one tree texture in the wild is over half filler. Both would otherwise consume palette entries and starve the colors the model really uses.

## Output Format

`model.data` is a PVDT container (see [compose_data_structure.md](/docs/data_structures/compose_data_structure.md)). Each block carries:

- **`geometry`** — the tree64, built from the brick map. It is written *unbaked*: leaf nodes carry no material offsets, because those index a `materialIDs` array the container does not store.
- **`voxelTypeData`** — three `uint32`s per voxel: chunk-space Z-order, `R10G10B10` color, and a packed normal (left at zero; the renderer derives normals from the tree64). This is the on-disk home of the colors the palette IDs stand for.

`loadComposeFromDisk` reads `voxelTypeData` back, interns each distinct color into the component's material palette, and rebuilds both the brick map and the tree64 (with material offsets) from it.

## ProjectV Features Used

| Feature | Usage |
|---------|-------|
| **Core Math** | `vec2`/`vec3`/`ivec3`, `dot`, `cross`, `min`/`max`/`clamp` — intersection math and UV sampling |
| **Logging** | `info`/`warn`/`error` via the spdlog wrapper for structured, leveled output |
| **Compose I/O** | `writeDataFile` — serializes the `DataFile`/`DataBlock` set into a `.data` container |
| **Voxel Management** | `ChunkHeader`, `createChunk`, `createVoxelBrickMap`, `brickMapSetVoxel`, `updateChunkFromBrickMap`, `createChunkScaleFromVoxelScaleAndResolution` |
| **Materials** | `brickMapGetMaterial` — reads back the palette ID a voxel was written with |
| **Z-Order Indexing** | `createZOrderIndex` / `reverseZOrderIndex` — Morton-coded spatial layout, both for chunks within the grid and voxels within a chunk |

## `trees/` — prebuilt assets

`trees/` holds eight tree species voxelized at 64³ from a Wavefront tree pack, one folder each. The
terrain_generator example reads them from here and scatters them into its terrain (see
`../terrain_generator/include/tree_placement.hpp`). To regenerate:

```bash
./obj_voxelizer -m <modelRoot>/ -f <modelRoot>/split/<Name>.obj -o trees/<Name> -r 64 --no-flip-v
```

That source pack needs `--no-flip-v`; see above for how to tell when a model does.

## Third-Party Dependencies

- [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) — OBJ/MTL parsing
- [stb_image](https://github.com/nothings/stb) — diffuse texture loading
- [CLI11](https://github.com/CLIUtils/CLI11) — command-line argument parsing
