## SCENE - v0.0

The `Scene` data structure is the in-memory representation of a loaded ProjectV scene: a flat pool of
chunks (the unit of geometry+placement), a shared refcounted geometry pool, optional grid volumes
(one per multi-block `.data` file), and a stable-handle scheme that survives add/remove without
rebasing. See [include/data_structures/scene.h](/include/data_structures/scene.h) for the
authoritative C++ definitions.

For the on-disk format that produces a `Scene`, see
[compose_data_structure.md](/docs/data_structures/compose_data_structure.md).

---

### Top-level: `Scene`

```cpp
struct Scene {
    std::vector<Chunk>          chunks;          // slot-indexed, with holes; see below
    std::vector<ChunkHandle>    chunkFreeList;   // recycled dead chunk slots (LIFO)
    std::vector<SceneGrid>      grids;           // one per multi-block .data volume
    std::vector<ChunkHandle>    looseChunks;     // explicit handle list of loose chunks
    uint32_t                    looseChunkCount; // legacy count (== looseChunks.size())
    std::vector<GeometryBlob>   geometryPool;    // refcounted shared geometry
    std::vector<uint32_t>       blobFreeList;    // recycled empty pool slots (LIFO)
    std::vector<ComponentRecord> components;     // one per loaded compose component
};
```

### Stable handles

A chunk's **handle** is its index into `Scene.chunks` and never moves for the chunk's lifetime. The
handle is also that chunk's row in the GPU header texture and the value stored in grid cells and
`looseChunks`. When a chunk is removed, its slot is marked dead and recycled via `chunkFreeList`;
the array never shifts, so all references stay valid. Same scheme for the geometry pool:
`GeometryBlob` slots are recycled via `blobFreeList` so live pool indices stay stable.

### `Chunk` and `ChunkHandle`

```cpp
using ChunkHandle = uint32_t;
static constexpr ChunkHandle INVALID_CHUNK_HANDLE = 0xFFFFFFFFu;
```

```cpp
struct Chunk {
    ChunkHeader header;                         // position, scale, voxelScale, resolution, rotation
    std::vector<uint32_t> geometryData;         // empty for pooled chunks (see geometryPoolIndex)
    std::vector<uint32_t> voxelTypeData;        // empty for pooled chunks
    VoxelBatch chunkQueue;                      // pending edits
    uint32_t LOD = 0;
    int32_t  geometryPoolIndex = -1;            // -1 until interned; >= 0 = shared pool blob
    Mutability mutability = Mutability::Locked; // from the compose component
    bool     copyDiverged = false;              // Copy COW bookkeeping
    bool     alive = true;                      // dead slots are recycled
    int32_t  gridIndex = -1;                    // -1 = loose; >= 0 = which grid this cell fills
    int32_t  cellIndex = -1;                    // linear index into SceneGrid.cellToChunk
    ComponentHandle componentHandle = ...;      // owning compose component (loose chunks only)
};
```

### `ChunkHeader` and `GPUChunkHeader`

`ChunkHeader` is the CPU/author-facing header (position, scale, voxelScale, resolution, quaternion
rotation). `GPUChunkHeader` is the packed-on-GPU version: same fields plus the geometry/voxelType
texture ranges and a packed rotation quat. The shader (`pjv_utils_DDA.sc`) reads the latter.

### `SceneGrid`

```cpp
struct SceneGrid {
    core::vec3 origin;                // world position of the grid's local (0,0,0) corner
    float      cellSize;              // world size of one cell
    core::ivec3 dims;                 // per-axis cell count
    core::quat  rotation;             // world rotation of the grid
    std::vector<int32_t> cellToChunk; // dims.x*dims.y*dims.z; -1 = empty cell
    ComponentHandle componentHandle;  // one compose component owns every cell
    bool resizeToFitVoxels = true;    // grow on out-of-bounds edits
    core::ivec3 cellCoordOffset;      // drift from the .data file's (0,0,0); starts at zero
};
```

A `SceneGrid` corresponds to exactly one multi-block `.data` file loaded in streaming mode, and to
the eagerly-loaded analogue in non-streaming mode. The shader walks a grid with a uniform-grid DDA
(`marchGrid` in `pjv_utils_DDA.sc`).

### `GeometryBlob` and the shared pool

```cpp
struct GeometryBlob {
    std::vector<uint32_t> geometry;
    std::vector<uint32_t> voxelTypeData;
    std::string           sourceDataPath;        // the .data file this blob came from
    core::ivec3           sourceBlockCoord;      // the block within the file
    bool                  ownsSourceFile = true; // false until first Copy persist
    uint32_t              refCount = 0;          // live chunks referencing this blob
    bool                  dirty = false;         // GPU copy is stale
    uint32_t              renderLOD = 0;         // tree64 levels to drop on upload (storage LOD)
};
```

Every `Chunk` in a Compose-loaded scene references one entry of `Scene.geometryPool` via
`Chunk.geometryPoolIndex`. Multiple chunks can share the same blob (instancing); the blob is freed
when its `refCount` hits zero. A `Copy` instance forks its own private blob (COW) the first time it
is made writable.

#### Storage LOD (`renderLOD`)

`renderLOD` is how many tree64 levels to drop when the blob is uploaded, each step being 4x coarser
per axis (64³ → 16³ → 4³). It is purely a VRAM knob: the blob's CPU arrays are always full
resolution, and the coarsened copy is built at upload time by `downsampleTree64` into scratch that
is discarded immediately. Editing, persistence and the brick map therefore never see it.

Set it with `setBlobRenderLOD(scene, poolIndex, lod)`, which marks the blob dirty. That is all
that is required: the next flush re-uploads the blob at the new detail and rewrites the header row
of every chunk referencing it.

The flag lives on the blob rather than the chunk because the blob is the GPU upload unit — that
makes it impossible for the uploaded geometry and the header's advertised `resolution` to disagree
about how deep the tree is. `GPUBlobRange::uploadedLOD` tracks what is actually resident, and
`makeHeader` reads that when computing the header's resolution.

Note what does **not** change: `Chunk.header.resolution` stays full-res on the CPU, and
`header.scale` is never touched. Holding `scale` fixed while the *uploaded* resolution shrinks is
exactly what grows each voxel's world size, so the chunk keeps its world footprint, position,
rotation and grid cell. See [tree64_data_structure.md](/docs/data_structures/tree64_data_structure.md)
for why one LOD step must be 4x and not 2x.

The unrelated `Chunk::LOD` field is a legacy leftover: it is written to 0 in a few places and read
nowhere.

### `ComponentRecord`, `ComponentHandle`, `ComponentKind`

```cpp
using ComponentHandle = uint32_t;
static constexpr ComponentHandle INVALID_COMPONENT_HANDLE = 0xFFFFFFFFu;
enum class ComponentKind { Chunk, Grid };
struct ComponentRecord {
    ComponentKind kind;
    ChunkHandle   chunkHandle;   // valid when kind == Chunk
    int32_t       gridIndex;     // valid when kind == Grid
    std::string   sourcePath;    // provenance/debugging
};
```

Each loaded `compose.json` component becomes exactly one `ComponentRecord`:
- `data` with one block → `Chunk` (a single loose `Chunk`).
- `data` with N>1 blocks → `Grid` (a `SceneGrid` with N cells, grid volume).

The editing mechanism (`utils::addVoxelsToComponent`, `utils::removeVoxelsFromComponent`) is
addressed by `ComponentHandle` so callers never need to know or care which kind the component
turned out to be. `projv::resolveComponentLocation(scene, chunkHandle)` maps a chunk handle back
to its owning component (used by the renderer gallery's picking).

### GPU side: `GPUData`

`GPUData` (in `include/data_structures/gpuData.h`) holds the bgfx handles and **persistent layout
state** — suballocators, per-blob GPU ranges, capacities, and a `blobEpoch` counter. This is what
makes incremental add/update/remove possible: the apply-mutation seam
(`graphics::applySceneMutations`) drains a queue of `PendingSceneMutation`s through the incremental
primitives (`addChunkToGPU`, `updateChunkGeometryOnGPU`, `removeChunkFromGPU`) and rebuilds the
small scene tables (gridInfo, cellMap, looseList) **once per frame**, not once per mutation.

### Editing

Live edits go through the streaming/edit seam in `utils/streaming.h`:
- `utils::addVoxelsToComponent` / `removeVoxelsFromComponent` — addressed by `ComponentHandle`,
  pool-aware, grid-bucketing, auto-grows `cellToChunk` via `expandGridToInclude` when
  `resizeToFitVoxels` is set, materializes missing cells on demand.
- `utils::materializeGridCell` / `releaseGridCell` — manual per-cell residency for custom policies.
- `utils::applyChunkEdit` — bakes a chunk's queued voxels into its pool blob (honoring COW) and
  enqueues an Update mutation.

### Persistence

- `utils::persistChunkData` — honors `mutability`:
  - `Locked` — no-op (in-memory only, never touches disk).
  - `Direct` — rewrites the chunk's block in place in the source `.data`.
  - `Copy` — first persist writes a new `.data`; subsequent persists write to that file in place.

### Legacy format (removed)

The previous flat layout — `headers.json` + per-chunk `tree64/<id>.bin` +
`voxelTypeData/<id>.bin` — has been removed. The current on-disk format is `.data` (PVDT
container) + `compose.json`, described in
[compose_data_structure.md](/docs/data_structures/compose_data_structure.md). The
`loadSceneFromDisk`/`writeSceneToDisk` entry points and the `voxel_io` / `lod` modules no longer
exist.

### More

For more information on this project, visit our [README.md](/README.md).
