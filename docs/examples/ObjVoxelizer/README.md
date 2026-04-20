# ObjVoxelizer

A command-line tool that converts Wavefront OBJ models into ProjectV's chunked voxel scene format. It parses mesh geometry and material data, samples diffuse textures at each voxel's surface intersection using barycentric UV interpolation, and writes the result as a collection of spatially organized voxel chunks suitable for loading into a ProjectV renderer.

## Overview

The voxelizer rasterizes each triangle in the input mesh into a 3D grid of voxels using the Separating Axis Theorem (SAT) for triangle/AABB intersection. Each occupied voxel is assigned a color sampled from the triangle's diffuse texture (or its material fallback color). The output chunks are stored in Z-order (Morton) layout and written to disk in ProjectV's scene format.

## How to Build

```bash
cd ObjVoxelizer
make
```

Requires ProjectV to be built at `../../../` with its libraries installed in `../../../lib/`.

## How to Use

```
./main.o -m <model-directory> -f <path-to-obj> -o <output-directory> -r <resolution>
```

| Flag | Description |
|------|-------------|
| `-m, --modelDir` | Root directory of the OBJ model (used for MTL and texture lookup) |
| `-f, --objDir` | Path to the `.obj` file |
| `-o, --outputDir` | Directory to write the generated voxel scene |
| `-r, --resolution` | Voxelization resolution along the longest axis (default: `256`) |

**Example:**

```bash
./main.o -m ./myModel/ -f ./myModel/scene.obj -o ./outputScene/ -r 512
```

The output directory will contain a `headers.json` file and `tree64/` and `voxelTypeData/` subdirectories holding the binary chunk data. This output can be loaded directly by ProjectV using `loadSceneFromDisk`.

## ProjectV Features Used

| Feature | Usage |
|---------|-------|
| **Core ECS** | Foundation types for all ProjectV scene objects |
| **Core Math** | `vec2`/`vec3`/`ivec3`, `dot`, `cross`, `min`/`max`/`clamp` — intersection math and UV sampling |
| **Logging** | `info`/`warn`/`error` via the spdlog wrapper for structured, leveled output |
| **Voxel I/O** | `writeChunkToDisk` — serializes voxel chunks to the ProjectV scene format |
| **Voxel Management** | `ChunkHeader`, `VoxelBatch`, `createChunk`, `moveVoxelBatchToChunk`, `updateChunkFromItsVoxelBatch` |
| **Z-Order Indexing** | `createZOrderIndex` / `reverseZOrderIndex` — Morton-coded spatial layout within chunks |

## Third-Party Dependencies

- [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) — OBJ/MTL parsing
- [stb_image](https://github.com/nothings/stb) — diffuse texture loading
- [CLI11](https://github.com/CLIUtils/CLI11) — command-line argument parsing
