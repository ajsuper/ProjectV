# MeshVoxelizer

A command-line tool that converts polygon models **and Minecraft worlds** into ProjectV's Compose scene format. For meshes it parses geometry and material data and samples diffuse textures at each voxel's surface intersection using barycentric UV interpolation; for Minecraft it reads the save's block data directly. Either way the result is a grid-volume `.data` container plus the `compose.json` that places it — a folder that `loadComposeFromDisk` can open directly.

## Supported Formats

Any format [Assimp](https://github.com/assimp/assimp) can read — around 68 of them, including **OBJ**, **FBX**, **glTF / GLB**, **Collada (DAE)**, **STL**, **PLY**, **3DS**, **Blender (.blend)**, **DirectX (.x)**, **LWO**, **MD2/MD3/MD5** and **X3D**. Run `./mesh_voxelizer --list-formats` to print exactly what your build accepts.

Plus **Minecraft (Java Edition) worlds** — see [Minecraft Worlds](#minecraft-worlds) below. Point `-f` at a world directory and the tool detects it automatically.

Format handling is confined to [`include/mesh_import.hpp`](include/mesh_import.hpp), which flattens any input into a single world-space triangle soup; the voxelizer itself is format-blind. That header is also where the per-format quirks are absorbed:

- **Scene graphs are baked.** FBX, glTF and Collada express a model as a hierarchy of transformed nodes; the importer bakes it into world space so instanced and offset parts land where they belong instead of stacking at the origin.
- **Embedded textures are unpacked.** GLB and binary FBX carry their images inside the container rather than beside it, and those are decoded straight out of memory.
- **External textures are searched for, not just opened.** References in the wild carry Windows separators, absolute paths from the artist's machine, or a directory layout that did not survive being zipped. The lookup widens from the reference as written, to the asset root and the model's own directory, to the usual `textures/`-style subdirectories, and finally to a case-insensitive search of the whole asset tree.
- **Animation data is discarded** at import. Bones, morph targets and keyframes can dwarf the geometry in a rigged FBX, and none of it survives voxelization.

## Overview

The voxelizer rasterizes each triangle in the input mesh into a 3D grid of voxels using the Separating Axis Theorem (SAT) for triangle/AABB intersection. Occupied voxels are written into a sparse **brick map**, which is then turned into a tree64 (`updateChunkFromBrickMap`) and flattened into the `.data` container's per-voxel color array.

The model is normalized so its longest axis fills the voxel grid, and the grid is split into 256³ chunks — one `.data` block per occupied chunk, addressed by grid coordinate. Empty chunks are skipped entirely, so a thin-shelled model costs only the blocks it actually touches.

## How to Build

```bash
cd MeshVoxelizer
make
```

`make` forwards to CMake, which builds Assimp from the submodule in `external/assimp` and then the tool. The first build takes a few minutes for Assimp; later ones do not rebuild it. If the submodule was not checked out, `make` fetches it.

CMake can also be driven directly, which is the path to use on Windows:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

Requires ProjectV to be built at `../../../` with its libraries installed in `../../../lib/`.

Assimp is a submodule **of this example**, not of the engine — nothing in `src/` depends on it, and the engine builds without it.

## How to Use

```
./mesh_voxelizer -f <path-to-model> -o <output-directory> -r <resolution>
```

| Flag | Description |
|------|-------------|
| `-f, --file` | Path to the model file (any supported format) |
| `-o, --outputDir` | Directory to write the generated voxel scene |
| `-r, --resolution` | Voxelization resolution along the longest axis (default: `256`) |
| `-m, --modelDir`, `-a, --assetDir` | Root directory to search for the model's textures. Defaults to the model file's own directory |
| `--flip-v` / `--no-flip-v` | Force the V texture coordinate flip on or off, overriding the per-format default |
| `--list-formats` | Print every model format this build can read, then exit |

`-m` is only needed for asset packs that keep textures away from the model — a self-contained model finds its own. The older `-m <dir> -f <file>` invocation still works unchanged.

Minecraft worlds take their own flags, all prefixed `--mc-`; see [Minecraft Worlds](#minecraft-worlds).

`-r` is rounded **up** to whole chunks, and the summary reports both the requested and the effective resolution. Chunks are 256³, so `-r 300` voxelizes at 512. Below that there is one smaller step: `-r 64` (or less) emits a **single 64³ chunk** rather than padding a small model out into a mostly-empty 256³ one.

64 is the floor. A chunk's resolution has to be a power of 4 — that is what sets the tree64's depth — and below 256 it must also be a whole brick, because the brick map's per-row bitmasks *are* the tree64's leaf level. A 16³ chunk would leave the tree the wrong depth.

Small assets are what this step is for: scattering props into a scene wants a tree that costs a few thousand voxels, not a few million.

### `--flip-v` / `--no-flip-v`

Image loaders return the top row first, so V has to be flipped exactly when the format places `v = 0` at the **bottom** of the image. Formats disagree: OBJ, FBX, Collada and 3DS use a bottom-left origin, while glTF 2.0 specifies a top-left one. The voxelizer picks the default from the file extension, so normally neither flag is needed.

Individual models disagree with their own format often enough to need an override — particularly anything converted out of 3ds Max. Sampling with the wrong setting lands on the mirrored half of the atlas: you get whatever happens to sit opposite the artwork, often filler or a second material's texture.

The symptom is unmistakable — foliage comes out uniformly gray, or a trunk takes on the leaves' color. If the colors look wrong in a way that suggests the model is sampling *somewhere else in the same image*, flip the setting.

**Examples:**

```bash
./mesh_voxelizer -f ./myModel/scene.obj -o ./outputScene/ -r 512
./mesh_voxelizer -f ./character.glb -o ./outputScene/ -r 256
./mesh_voxelizer -f ./pack/props/crate.fbx -m ./pack/ -o ./outputScene/ -r 128
```

The output directory contains `model.data` (the PVDT container holding every occupied chunk) and `compose.json` (which places it at the origin). Point the engine at the **folder**:

```cpp
projv::Scene scene = projv::utils::loadComposeFromDisk("./outputScene/");
```

## Minecraft Worlds

A Minecraft save is already a voxel grid, so this path skips triangle rasterization entirely: **one block becomes one voxel**, straight into the brick maps. Everything downstream — the material palette, the tree64, the `.data` container — is shared with the mesh path.

```bash
./mesh_voxelizer -f ~/.minecraft/saves/MyWorld -o ./outputScene/
```

`-f` accepts the world directory (the one holding `level.dat`), a bare `region/` directory, or a single `.mca` file. Detection is automatic; `--minecraft` forces it for a save in an unusual layout.

| Flag | Description |
|------|-------------|
| `--mc-bounds <minX> <minZ> <maxX> <maxZ>` | Block area to import. Defaults to the world's extent, clamped by `--mc-area` |
| `--mc-y <minY> <maxY>` | Vertical range. Defaults to the world's full height |
| `--mc-area <blocks>` | Width of the area imported when `--mc-bounds` is absent (default: `512`) |
| `--mc-dimension <overworld\|nether\|end>` | Which dimension to read (default: `overworld`) |
| `--mc-resource-pack <dir>` | Resource pack to sample real block colors from |
| `--mc-no-water` | Skip water, leaving oceans and lakes hollow |
| `--mc-max-voxels <n>` | Memory guard; stops after this many blocks (default: `30000000`) |

**`-r` does not apply.** A block is always one voxel, so the grid size follows from how much world you select — use `--mc-bounds` to control it. Passing `-r` prints a warning rather than silently ignoring it.

### Choosing an area

Saved worlds are routinely tens of thousands of blocks across, which is not something anyone wants voxelized by accident. Without `--mc-bounds` the tool measures the extent of the region files and, if that exceeds `--mc-area`, imports a 512×512 block area centered on it and says so. Explicit bounds are the norm for anything but a small world:

```bash
# A 256x256 area around spawn, from bedrock to build height
./mesh_voxelizer -f ~/.minecraft/saves/MyWorld -o ./spawn/ --mc-bounds -128 -128 127 127

# Surface only, skipping the caves underneath
./mesh_voxelizer -f ~/.minecraft/saves/MyWorld -o ./surface/ --mc-bounds 0 0 511 511 --mc-y 50 200
```

### Block colors

The save stores block *identities* (`minecraft:oak_stairs`), not colors — those live in the game's textures, which are not part of the world. Colors are resolved in three tiers, in [`include/minecraft_blocks.hpp`](include/minecraft_blocks.hpp):

1. **A resource pack**, if `--mc-resource-pack` is given. Block textures are averaged over their opaque pixels. This is exact for the pack in use and is the only tier that can color modded blocks. Animated textures (water, lava) are stored as vertical frame strips, so only the first frame is sampled; grayscale foliage textures get the default biome tint applied, since without it they average to gray.
2. **An explicit table** of roughly 200 blocks — the ones that cover most of a world by volume.
3. **Naming rules**, which handle the long tail. Minecraft's naming is highly regular: the 16 dye colors prefix a dozen families (`red_wool`, `red_concrete`, `red_terracotta`, …), and stairs, slabs, walls, fences and doors always derive from a base material. Stripping a known suffix and re-resolving covers hundreds of blocks no table would be worth enumerating — `spruce_stairs` resolves through `spruce_planks`.

Anything still unmatched is drawn neutral gray and reported by name at the end of the run, so an unmapped block is a hole in the color scheme rather than a hole in the build.

### Format support

Reading is implemented in [`include/minecraft_import.hpp`](include/minecraft_import.hpp) over a small NBT reader in [`include/nbt.hpp`](include/nbt.hpp). Two format shifts matter and both are handled, keyed on the chunk's `DataVersion`:

- **1.18** moved sections from `Level.Sections` to `sections`, and the palette from `Palette`/`BlockStates` to `block_states.palette`/`block_states.data`.
- **1.16** changed the bit packing of palette indices. Before it, indices ran as a continuous bit stream and could span two 64-bit words; from 1.16 on each word is padded so they never do.

Worlds older than **1.13** stored numeric block ids instead of a palette; those are rejected with a message telling you to open the world once in a modern version to convert it. They are detected structurally — a section carrying a `Blocks` byte array and no palette — rather than by `DataVersion`, because that tag itself only exists from 1.9 on, and the worlds most likely to be handed to this tool are old enough to have none.

Compressed chunk payloads are inflated with zlib, which Assimp already vendors — Minecraft support adds no new dependency.

## Color Quantization

Voxels do not store colors. A voxel stores an 8-bit **material ID** into its component's material palette, and that palette is capped at 255 entries for the whole component — the voxelizer's entire output is one component. A textured mesh sampled naively yields tens or hundreds of thousands of distinct colors, so the loader would hit the cap, log `internMaterial: palette full`, and hand every remaining voxel `INVALID_MATERIAL`.

The voxelizer therefore builds a fitting palette **before** voxelizing:

1. A pre-pass walks the parsed triangles and samples each one exactly the way voxelization will, spending samples in proportion to **3D surface area** — voxel counts follow area, so this approximates the distribution of colors that will actually reach a voxel. Samples go into a 32³ RGB histogram.
2. **Median cut** splits that histogram into at most 255 boxes, each contributing one palette entry equal to the mean color of the samples inside it.
3. A 32³ lookup table resolves any sampled color to its nearest palette slot in O(1), so voxelization itself stays a single pass.

Weighting by area rather than histogramming whole texture images matters more than it sounds. A material library commonly declares materials the mesh never uses, and atlases commonly contain large unsampled regions — one tree texture in the wild is over half filler. Both would otherwise consume palette entries and starve the colors the model really uses.

Minecraft worlds take the same path from step 2 onward, fed the blocks that were actually read instead of surface samples. There is no pre-pass to weight, because the voxels are known up front. A world using more than 255 distinct block colors is quantized like any other input; most builds sit far below the cap.

## Output Format

`model.data` is a PVDT container, currently version 2 (see [compose_data_structure.md](/docs/data_structures/compose_data_structure.md)). It holds geometry and nothing else — neither the placement nor the palette is in there. Each block carries:

- **`geometry`** — the tree64, built from the brick map by `updateChunkFromBrickMap`.
- **`materialIDs`** — one byte per solid voxel, with uniform leaves collapsed to a single entry.

The two are written already **baked** against each other: `bakeMaterialsFromBrickMap` stamps each leaf node with its offset into `materialIDs`, so the pair on disk is exactly the pair the GPU reads and `loadComposeFromDisk` does not have to rebuild the tree.

The colors those bytes stand for live in `compose.json`, as a `palette` array on the component — one `"color"` entry per material ID, in the same order, as `R10G10B10` components (so each channel runs 0–1023, not 0–255). Keeping them out of the `.data` is what lets one container be instanced by several components that color it differently, and it puts the colors somewhere a person can edit by hand.

Splitting the colors out this way also keeps the container small — a byte per voxel rather than a color per voxel. The Lumberyard Bistro exterior at `-r 512` is 2.8 M triangles and 983,852 voxels in a 1.6 MB `.data`.

## ProjectV Features Used

| Feature | Usage |
|---------|-------|
| **Core Math** | `vec2`/`vec3`/`ivec3`, `dot`, `cross`, `min`/`max`/`clamp` — intersection math and UV sampling |
| **Logging** | `info`/`warn`/`error` via the spdlog wrapper for structured, leveled output |
| **Compose I/O** | `writeDataFile` — serializes the `DataFile`/`DataBlock` set into a `.data` container |
| **Voxel Management** | `ChunkHeader`, `createChunk`, `createVoxelBrickMap`, `brickMapSetVoxel`, `updateChunkFromBrickMap`, `createChunkScaleFromVoxelScaleAndResolution` |
| **Materials** | `bakeMaterialsFromBrickMap` — emits the per-voxel `materialIDs` array and stamps each tree64 leaf with its offset into it |
| **Z-Order Indexing** | `createZOrderIndex` / `reverseZOrderIndex` — Morton-coded spatial layout, both for chunks within the grid and voxels within a chunk |

## `trees/` — prebuilt assets

`trees/` holds eight tree species voxelized at 64³ from a Wavefront tree pack, one folder each. The
terrain_generator example reads them from here and scatters them into its terrain (see
`../terrain_generator/include/tree_placement.hpp`). To regenerate:

```bash
./mesh_voxelizer -m <modelRoot>/ -f <modelRoot>/split/<Name>.obj -o trees/<Name> -r 64 --no-flip-v
```

That source pack needs `--no-flip-v`; see above for how to tell when a model does.

## Third-Party Dependencies

- [Assimp](https://github.com/assimp/assimp) — mesh import for every supported format. Submodule at `external/assimp`, pinned to `v6.0.5`, built from source by CMake. It is a dependency of this example only; the engine does not use it
- [zlib](https://zlib.net/) — inflating the compressed NBT in Minecraft region files. Comes from Assimp's bundled copy, so it costs no additional dependency
- [stb_image](https://github.com/nothings/stb) — texture loading, from files, from embedded bytes, and from resource packs
- [CLI11](https://github.com/CLIUtils/CLI11) — command-line argument parsing

NBT and Anvil parsing are implemented directly in `include/` rather than taken as a dependency; the formats are small and fully specified, and the C++ libraries for them are heavier than the few hundred lines they replace.
