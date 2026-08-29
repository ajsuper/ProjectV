## COMPOSE - v0.0

The **Compose** system describes *where physical objects live in a scene* and *how they are assembled from reusable pieces*. It is a folder-based, recursive scene-graph format. It deliberately describes **only the physical layout** (geometry + transforms). Gameplay, state, and behavior are **not** part of this format — see [Non-Goals](#non-goals).

This format supersedes the flat `headers.json` layout described in [scene_data_structure.md](/docs/data_structures/scene_data_structure.md) for authored scenes. The legacy `headers.json` format has been **removed** from the engine (the old `loadSceneFromDisk` / `writeSceneToDisk` entry points and the `voxel_io` / `lod` modules no longer exist); Compose is the only on-disk scene format. See [Relationship to the old `headers.json` format](#relationship-to-the-old-headersjson-format).

- [Core Concept](#core-concept)
- [`compose.json` Format](#composejson-format)
  - [Top-level fields](#top-level-fields)
  - [Component fields](#component-fields)
  - [Transforms](#transforms)
  - [Boolean ops](#boolean-ops)
  - [Mutability](#mutability)
- [`.data` Container Format](#data-container-format)
- [Loading Rules](#loading-rules)
  - [Path resolution](#path-resolution)
  - [Instancing / deduplication](#instancing--deduplication)
  - [Recursion safety](#recursion-safety)
- [Relationship to the old `headers.json` format](#relationship-to-the-old-headersjson-format)
- [Non-Goals](#non-goals)
- [Worked Example](#worked-example)
- [Open Questions](#open-questions)

---

### Core Concept

**A folder is the unit of composition.** Every composable folder contains a `compose.json`. Pointing the engine at *any* such folder loads its `compose.json` and recursively constructs everything it references. There is no engine-level difference between a "scene" and an "asset" — both are just folders with a `compose.json`. This means:

- Loading `Example 1/Scene/` loads the whole world.
- Loading `Example 1/Assets/Bird/` loads just the bird, standalone.
- The same asset folder can be referenced by many scenes.

A `compose.json` lists **components**. A component is one of two kinds:

- `data` — references a **`.data`** file: raw voxel geometry + voxel-type data (a tree64, or a grid of tree64s). This is a leaf.
- `asset` — references **another folder** (which has its own `compose.json`). This recurses.

Every component carries a **transform** (position / rotation / scale) applied relative to its parent. This produces a standard scene graph.

---

### `compose.json` Format

The wire format is **strict JSON** (RFC 8259). The loader is configured with `ignore_comments = true`, so `//` line comments and `/* */` block comments are permitted **for authoring convenience only** — commas are still mandatory. `#` comments are **not** valid. Do not rely on comments surviving a load/save round-trip.

#### Top-level fields

```json
{
    "version": 1,
    "name": "Square Earth Theory",
    "components": [ /* ... */ ]
}
```

| Field | Type | Required | Meaning |
|-------|------|----------|---------|
| `version` | integer | **yes** | Format version. Current: `1`. Lets the loader reject/upgrade old files. |
| `name` | string | no | Human identification only. The engine assigns an internal ID on load; `name` has no runtime meaning and need not be unique. |
| `components` | array | **yes** | The list of components (see below). May be empty. |

#### Component fields

```json
{
    "type": "data",
    "source": "terrain.data",
    "position": [0.0, 0.0, 0.0],
    "rotation": [0.0, 0.0, 0.0],
    "scale": 1.0,
    "mutability": "direct"
}
```

| Field | Type | Required | Meaning |
|-------|------|----------|---------|
| `type` | `"data"` \| `"asset"` | **yes** | `data` → `source` is a `.data` file. `asset` → `source` is a folder containing a `compose.json`. |
| `source` | string | **yes** | Path to the file (`data`) or folder (`asset`), resolved relative to **this** `compose.json`'s directory. See [Path resolution](#path-resolution). |
| `position` | `[x, y, z]` float | no (default `[0,0,0]`) | Translation, relative to the parent. |
| `rotation` | float array | no (default identity) | Length **3** = Euler degrees `[x, y, z]`; length **4** = quaternion `[x, y, z, w]`. See [Transforms](#transforms). |
| `scale` | number \| `[x,y,z]` | no (default `1.0`) | A single number is uniform scale. A 3-array is per-axis scale. |
| `mutability` | `"locked"` \| `"direct"` \| `"copy"` | no (default `"locked"`) | Only meaningful for `type: data`. See [Mutability](#mutability). Ignored on `asset` (each inner `data` declares its own). |
| `op` | `"none"` \| `"union"` \| `"subtract"` \| `"intersect"` | no (default `"none"`) | How this entry combines with the ones above it in the list. See [Boolean ops](#boolean-ops). |

> **Note on `source` naming:** the key is `source`, not `data`, because it points to a *file* for `type:data` but a *folder* for `type:asset`. One key, two resolutions, disambiguated by `type`.

#### Transforms

Each component's local transform is composed **T · R · S** (scale first, then rotate, then translate) and applied relative to its parent:

```
worldMatrix(component) = worldMatrix(parent) · Translate(position) · Rotate(rotation) · Scale(scale)
```

For an `asset` component, `worldMatrix(component)` becomes the parent transform for **everything inside that asset's `compose.json`**. Asset transforms stack multiplicatively down the tree.

**Rotation encoding** is disambiguated by array length:
- **3 elements** → Euler angles in **degrees**, applied intrinsically in order **X (pitch) → Y (yaw) → Z (roll)**. Converted to a quaternion internally.
- **4 elements** → a quaternion `[x, y, z, w]`, used directly. Preferred when authored by tools, since it avoids gimbal ambiguity.

> The engine stores rotation internally as a quaternion (or matrix). Euler input is a convenience for hand-authoring. This is a change from the current renderer, which assumes axis-aligned chunks and has **no** rotation support — see [Open Questions](#open-questions).

#### Boolean ops

`op` turns an ordered placement list into a **constructive-solid stack**: the parent's voxels become the fold of its children, evaluated left to right from an empty accumulator.

| Value | Means |
|-------|-------|
| `none` | **Placed.** The component is parented, transformed and rendered as its own geometry, with no relationship to its siblings. |
| `union` | Add its cells to the accumulator. Its own colours win. |
| `subtract` | Remove its cells from the accumulator. Contributes no colour. |
| `intersect` | Keep only cells in both. The accumulator's colours survive. |

**The default is `none`, and that matters in both directions.** Every `compose.json` written before this field existed is a pure placement list; a default of `union` would silently reinterpret all of them as boolean resolves. And a loader that does not know the field reads a composed asset as its placed parts — a degraded picture, but a coherent one, rather than a parse failure.

Two rules the evaluator has to state, because neither is discoverable from the file:

- **The first contributing entry seeds the accumulator whatever its `op` says.** An empty set intersected with anything is empty, and subtracting from an empty set leaves it empty, so a stack whose first boolean entry is `intersect` or `subtract` would resolve to nothing at all — an outcome with no visible cause.
- **`op` on a `type: data` component whose `.data` is a grid volume is treated as `none`.** A grid is many blocks across many cells, and folding one into its parent's single-lattice `.data` is a rebuild rather than a bake. The writer emits `none` for these rather than recording a promise nothing keeps.

Because `asset` entries recurse, this one field gives **nested CSG** with no new tree and no second evaluator: subtracting a whole sub-assembly is an `asset` entry with `"op": "subtract"`, and the ordinary load walk already reaches it.

A **shared lattice** is required of everything that folds — one `resolution` and one `voxelScale` — because that is the invariant of the `.data` file the fold has to produce. Composition by *placement* never needed that; only composition by boolean does, which is exactly why stating it per entry lets both live in one list.

#### Mutability

`mutability` governs what happens when a `data` component's voxel data is **modified at runtime and persisted**. It applies only to `type: data`.

| Value | On persist | Aliases one buffer across instances? | Intended for |
|-------|-----------|--------------------------------------|--------------|
| `locked` *(default)* | Nothing is written back. Runtime edits are in-memory only and are never saved to the source `.data`. | **Yes** (same-policy) | Reusable library assets instanced many times (Bird, Cow). Guarantees the source master is never persisted-over. |
| `direct` | Edits are written **in place** to the source `.data`. | **Yes** (same-policy) | Large data you edit and keep, e.g. **terrain**. Edits are shared across all `direct` instances of the source (they *are* the same block) and written through to the source `.data`. |
| `copy` | Edits are written to a **new** `.data`; the original is left untouched (copy-on-write). The runtime records the override that points the affected instance at the new file. | **Yes, until first edited**, then a private buffer | A unique object that diverges from a shared source, e.g. a damaged bird you want to keep alongside the pristine one. |

The dedup key includes `mutability` (see [Instancing / deduplication](#instancing--deduplication)), so the **same source referenced with two different policies gets two separate buffers** — a `direct` edit can never mutate a `locked` sibling. Within one policy, instances share.

Notes:
- The original source `.data` on disk is **never destroyed** by `copy`, and **never** touched at all by `locked`. Only `direct` writes through to it — by explicit opt-in.
- Mutability is a **policy the loader and runtime enforce**, not stored state about "which version is current." Version/override bookkeeping (which instance points at which `.data`) lives in the runtime, not in this static description.

---

### `.data` Container Format

A `.data` file holds the **intrinsic** voxel data for one component: one tree64, or a grid of equally-sized, grid-aligned tree64s. It carries an index so individual blocks can be **seeked/streamed without reading the whole file**. Placement (position/rotation/scale) is **not** stored here — it lives in `compose.json`.

Binary layout, little-endian:

```
Offset  Size  Field
------  ----  -----
0       4     magic         = "PVDT" (0x50 0x56 0x44 0x54)
4       4     uint32 version        (current: 1)
8       4     uint32 flags          (bit0: voxelTypeData present)
12      4     uint32 blockCount     (1 for a simple asset; N for a grid volume)
16      4     uint32 resolution     (edge resolution; shared by all blocks)
20      4     float  voxelScale     (world size of one voxel at resolution)
24      ...   BlockEntry[blockCount] (block table, see below)
...     ...   blob region: raw uint32 arrays, referenced by offsets below
```

`BlockEntry` (fixed 40 bytes):

```
int32  gridX, gridY, gridZ     grid coordinate of this block (0,0,0 for a single-block asset)
uint64 geometryOffset          byte offset into the file of this block's tree64 uint32[]
uint32 geometryLength          length of the tree64 array, in uint32 units
uint64 voxelTypeOffset         byte offset of this block's voxelTypeData uint32[] (0 if absent)
uint32 voxelTypeLength         length in uint32 units (0 if absent)
```

This one format covers both cases the plan needs:

- **Simple asset** (bird, cow): `blockCount = 1`, one block at `(0,0,0)`.
- **Grid volume** (large terrain): `blockCount = N`, one block per occupied grid cell, all sharing `resolution`/`voxelScale`. The DDA acceleration structure walks the grid at the top level; the block table lets the streamer `seek`/`mmap` and load **only** the cells near the camera — solving the "must I load the whole file to stream one chunk?" problem. One file, still streamable.

> The grid-volume path is the optimization target from the plan (grid-aligned chunks for efficient DDA instead of brute-forcing every chunk). It is intentionally *separate* from the asset/transform graph, which is for sparse, movable objects placed by arbitrary transform. See [Open Questions](#open-questions) for what still needs deciding there.

---

### Loading Rules

#### Path resolution

`source` is always resolved relative to the directory containing the `compose.json` that names it. `../` is permitted (e.g. a `Scene` referencing `../Assets/Bird/`). The loader canonicalizes each resolved path (absolute, symlinks and `..` collapsed) before use — this canonical path is the key for dedup and cycle detection below.

#### Instancing / deduplication

The loader maintains a cache keyed by canonical path, and a geometry pool keyed by **(canonical path, block grid coords, mutability)**:

- A `.data` file loaded once is reused for every component that references it. Its per-block geometry is stored **once** in the pool and shared by every instance that resolves to the same key. `locked` and `direct` instances of a source thus **share one buffer** (a `direct` edit is meant to be seen by all of them); a `copy` instance shares too, **until it is first edited**, at which point it forks a private buffer (copy-on-write). Because `mutability` is part of the key, the **same source referenced with two different policies gets separate buffers** — the differing group is a forced copy.
- An `asset` folder loaded once can be re-instanced by transform without re-parsing.

This is how instancing works: **one component entry = one instance (one transform)**, and sharing happens automatically at the data layer. There is intentionally no "instance count" field in v0.0; an optional `transforms: [ ... ]` array on a component may be added later as pure authoring sugar (see [Open Questions](#open-questions)).

#### Recursion safety

`asset` references can form cycles (A composes B composes A) or pathological depth. Cyclic dependencies are **allowed** — each level re-applies the asset transform, producing repeated / fractal structures — and are kept safe purely by a depth bound:

1. The loader maintains a stack of canonical folder paths currently being expanded. If `source` resolves to a path already on the stack, it logs a **one-time warning** (per distinct cycle) that the dependency is cyclic and will be capped, then keeps recursing.
2. It enforces a maximum recursion depth (default **32**, configurable). Reaching it stops the descent for that branch.

So recursion — cyclic or not — is a supported feature bounded by the depth cap, so it cannot run away. A cycle through a node that contains a `data` leaf emits one instance of that geometry per level, each at the accumulated transform.

#### Runtime representation

A loaded `Scene` is designed so chunks can be added, edited, and removed at runtime (streaming, live edits) without rebuilding everything — one **stable handle scheme** spans CPU and GPU:

- **Stable handles.** A chunk's index into `Scene.chunks` is a **`ChunkHandle`** that never moves: it is also the chunk's row in the GPU header texture and the value stored in grid cells and the loose list. Removing a chunk marks its slot dead and recycles the handle via a free list (the array never shifts), so grid `cellToChunk` entries and loose-list handles stay valid across add/remove.
- **Loose vs grid, decoupled from order.** Loose (transform-placed) chunks are an explicit `looseChunks` handle list the renderer iterates, not a positional prefix — so a loose chunk can be inserted or dropped with no reordering. Grid blocks are reached only through `SceneGrid.cellToChunk`, and each chunk carries its residency key (`gridIndex`, `cellIndex`) for O(1) eviction.
- **Refcounted geometry pool.** Shared geometry lives once in `Scene.geometryPool` (deduped by canonical path + block coords + mutability). Each blob tracks a `refCount` of the chunks using it; at zero its GPU range is freed and the pool slot recycled, so a long dynamic session doesn't leak.
- **Managed GPU pools.** The geometry, voxel-type, and header textures are **suballocated** with headroom rather than packed exactly to content. A persistent allocation table (per-blob GPU ranges + free-list allocators) lets a blob be uploaded, resized, or freed in place; a texture is only reallocated when a pool overflows its capacity. This replaces the earlier one-shot build that discarded its layout and could not be updated incrementally.

#### Streaming (mechanism, not policy)

Grid volumes can be loaded **lazily**: `loadComposeFromDisk(folder, &streamingContext)` builds the full grid topology (every `SceneGrid` descriptor) but loads **no grid geometry** — each cell starts empty — while single-block (loose) assets still load eagerly. Cells become resident on demand. This follows ProjectV's rule that the **engine provides mechanisms, the user supplies policy** (as with the ECS and the render pipeline):

- **Per-block IO.** `readDataFileHeader` reads only a `.data`'s header + block table (grid coords + byte offsets/lengths, ~40 bytes/block, no geometry); `readDataBlock` seeks and reads exactly one block. The block table already in the `.data` format *is* the streaming index.
- **Materialize / release.** `materializeGridCell(scene, ctx, grid, cell)` reads a cell's block, dedup-or-interns its refcounted blob, synthesizes the chunk header from the grid descriptor, and enqueues an add; `releaseGridCell` enqueues a remove. Both are coordinate-level and **policy-free**.
- **Apply seam.** Producers enqueue `PendingSceneMutation`s; `applySceneMutations` drains them through the incremental GPU primitives and rebuilds the small scene tables **once per frame** (not once per cell). Live edits feed the same seam via `applyChunkEdit`.
- **Residency is user policy.** *Which* cells should be resident is decided by a user system — keyed on one camera, several (split-screen), networked interest, portals, predicted paths, anything. The engine ships **no default**; the renderer gallery example includes one interest-source policy (`residencyPolicy.h`), selectable/replaceable like a renderer module.

**Memory & scaling.** The engine's mandatory footprint is only `O(#grids)` descriptors plus the resident set; the on-disk block-table index is read on demand and held in a **budgeted** LRU cache (a policy choice), never fully pinned. The remaining per-grid ceiling is the dense `SceneGrid.cellToChunk` (and its GPU `cellMap`), sized by a grid's cell count — so worlds scale by composing **many bounded grid volumes**, each streamed independently. Lifting that ceiling for a single unbounded volume would need sparse/hierarchical grids (deferred).

---

### Relationship to the old `headers.json` format

| Concern | `headers.json` (removed) | Compose (current) |
|---------|--------------------------|--------------------|
| Placement | `position` baked into each `ChunkHeader` | Lives in `compose.json` transforms |
| Rotation | none | `rotation` per component |
| Intrinsic data | `tree64/<id>.bin` + `voxelTypeData/<id>.bin`, `resolution`/`voxelScale` in header | Bundled in a `.data` container |
| Reuse / hierarchy | none (flat list) | `asset` references + scene graph |
| Chunk ID | authored in file | assigned internally on load |
| Status | **Removed** — no loader/writer remains in the engine | Current; produced by MeshVoxelizer, loaded by `loadComposeFromDisk` |

The legacy loader/writer (`utils/voxel_io.h` / `loadSceneFromDisk` / `writeSceneToDisk`) was removed because the per-chunk `.bin` layout offered nothing Compose doesn't, and the only existing scenes can be regenerated from their source meshes via [MeshVoxelizer](/examples/20-mesh-voxelizer). The intrinsic fields a `.data` carries (`resolution`, `voxelScale`, geometry `uint32[]`, `voxelTypeData` `uint32[]`) are exactly the per-chunk fields the old `loadChunkFromDisk` read.

---

### Non-Goals

Compose describes **physical objects only**. It does **not** describe:

- Gameplay state, scripts, health, AI, or any per-entity behavior.
- ECS component assignment beyond what a scene graph implies.

Loading a `compose.json` **emits entities into the ECS**: each `data` component becomes an entity with a transform and a geometry/chunk-reference component; the asset `name` becomes a tag/archetype. Other systems, keyed on that tag, attach behavior and state separately. This keeps the scene format a pure, reusable description and keeps game logic out of it.

---

### Worked Example

```
Example 1/
├── Scene/
│   ├── compose.json     # references terrain.data, water.data, ../Assets/Bird/, ../Assets/Cow/
│   ├── terrain.data
│   └── water.data
└── Assets/
    ├── Bird/
    │   ├── compose.json # references bird.data
    │   └── bird.data
    └── Cow/
        ├── compose.json # references cow.data
        └── cow.data
```

`Scene/compose.json`:

```json
{
    "version": 1,
    "name": "Square Earth Theory",
    "components": [
        { "type": "data",  "source": "terrain.data",     "position": [0, 0, 0],       "mutability": "direct" },
        { "type": "data",  "source": "water.data",       "position": [0, 0, 0],       "mutability": "direct" },
        { "type": "asset", "source": "../Assets/Bird/",  "position": [110, 240, 30],  "rotation": [45, 5, 0] },
        { "type": "asset", "source": "../Assets/Cow/",   "position": [45, 80, 150],   "rotation": [90, 0, 0], "scale": 1.2 }
    ]
}
```

`Assets/Bird/compose.json`:

```json
{
    "version": 1,
    "name": "Bird",
    "components": [
        { "type": "data", "source": "bird.data", "position": [0, 0, 0], "mutability": "locked" }
    ]
}
```

Loading `Scene/` yields: terrain + water as directly-editable grid volumes at the origin, one bird instanced at (110,240,30) rotated (45°,5°), and one cow at (45,80,150) rotated 90° and scaled 1.2×. Loading `Assets/Bird/` alone yields just the bird at the origin.

---

### Open Questions

These were unresolved in earlier revisions of this document. The status below reflects the decisions
locked in for v0.0.

1. **Renderer rotation support.** ✅ **Resolved.** The shader transforms rays into chunk-local
   space via `rotationFromQuat` (`pjv_utils_DDA.sc`) and the loader bakes the world rotation into
   each `Chunk` / `SceneGrid` header. Rotated chunks and rotated grids both render correctly.

2. **Multi-instance authoring sugar.** ⏸ **Deferred.** v0.0 keeps one component = one instance.
   The data model supports it (sharing happens automatically at the geometry-pool layer); an
   optional `transforms: [ [pos,rot,scale], ... ]` array on a component may be added later as pure
   authoring sugar. Not needed yet.

3. **Grid-volume acceleration structure.** ✅ **Decided: uniform-grid DDA.** The v0.0 top-level
   structure is the uniform-grid DDA implemented in `marchGrid` (`pjv_utils_DDA.sc`). The
   tree64-of-blocks alternative for very large or sparse volumes is left as a future
   optimization — the current DDA is sufficient for the example scenes and is paired with
   per-cell streaming (BlockTableCache + materialize/release) so even a large grid never has to
   be fully resident.

4. **Per-axis vs uniform scale + non-uniform scale under rotation.** ✅ **Decided: uniform scale
   only in v0.0.** Non-uniform scale on a `data` leaf is rejected by the loader
   (`loadComposeFromDisk`) with an error and the component is skipped. The 3-array form of
   `scale` is parsed and accepted on input (so JSON is forward-compatible) but only used when the
   three axis scales match. Per-axis scale may be revisited on `asset` nodes in a future
   revision; for v0.0 the simplification is the right call — non-uniform scale under rotation
   shears geometry, which is almost never what the user actually wanted.

5. **`mutability` cardinality.** ✅ **Decided: three values (`locked`/`direct`/`copy`).** The
   shared-buffer guarantee of `locked` is kept because it is the mechanism that makes "100
   instanced library assets edited through one source" cheap. `direct` covers large editable
   volumes (terrain) where every instance should see the same edits. `copy` covers unique
   divergent instances. The `mutability` part of the loader's dedup key guarantees the three
   policies never accidentally share a buffer across a policy boundary.

---

### More

For more information on this project, visit our [README.md](/README.md). Related: [scene_data_structure.md](/docs/data_structures/scene_data_structure.md), [tree64_data_structure.md](/docs/data_structures/tree64_data_structure.md), [voxel_type_data_structure.md](/docs/data_structures/voxel_type_data_structure.md).
