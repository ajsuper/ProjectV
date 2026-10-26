# The scene

`projv::Scene` is everything loaded: a tree of components, the chunks they place, the grids they
fill, and one shared pool of geometry. Header:
[`data_structures/scene.h`](../../include/data_structures/scene.h). Tree operations:
[`utils/scene_query.h`](../../include/utils/scene_query.h).

## The picture

```text
Scene
├── components[]   ComponentRecord, indexed by ComponentHandle ── the tree (parent/children)
│     kind = Asset ── a folder: children only, no voxels
│     kind = Chunk ── one loose chunk ──────────────┐
│     kind = Grid  ── one SceneGrid ───────┐        │
├── grids[]        SceneGrid               │        │
│     cellToChunk[x,y,z] ─────────────┐ ◀──┘        │
├── chunks[]       Chunk, indexed by ChunkHandle ◀──┴── also the GPU header row
│     header (position, rotation, scale, resolution)
│     geometryPoolIndex ──────────────┐
├── looseChunks[]  ChunkHandles the renderer walks one by one
└── geometryPool[] GeometryBlob ◀─────┘   refcounted, shared by every chunk of the same voxels
      tile table + materials (or a tree64) · brickMap (authoring only) · contentStamp
```

## Components

One `ComponentRecord` per `compose.json` entry. All kinds have a name, local transform, parent and
`localId`; the rest depends on kind:

| Kind | From compose.json | Owns | Voxels in |
|---|---|---|---|
| `Asset` | `"type": "asset"` | `children` | (its descendants) |
| `Chunk` | `"type": "data"`, a `.data` with one block | `chunkHandle`, `materialPalette` | one loose `Chunk` |
| `Grid` | `"type": "data"`, a `.data` with several blocks | `gridIndex`, `materialPalette` | one `SceneGrid`, a chunk per cell |

```cpp
const ComponentRecord& c = scene.components[handle];
c.name;  c.localId;  c.parent;  c.children;                 // identity and tree
c.localPosition;  c.localRotation;  c.localScale;           // relative to parent; uniform scale
c.materialPalette;                                          // up to 255 Material entries
```

A chunk component grows into a grid automatically when an edit runs past its edge
(`utils::convertChunkToGrid`).

### The tree API

All in `projv::utils` ([`utils/scene_query.h`](../../include/utils/scene_query.h)):

| Want | Call |
|---|---|
| Find | `findComponentByPath(scene, "bird/wing/left")`, `findComponentsByName`, `findComponentByLocalId(scene, document, id)` |
| List | `listComponents(scene)` (depth-first, with world positions), `rootComponents(scene)` |
| World transform | `getComponentWorldMatrix`, `getComponentWorldPosition`, `getComponentWorldRotation` |
| Move | `setComponentPosition` / `Rotation` / `Scale` / `Transform`: immediate, rebakes the subtree |
| Create | `addComponent(scene, kind, name, parent, resolution, voxelScale)` |
| Copy | `duplicateComponent(scene, source, parent)` (geometry shared until edited) |
| Reparent | `setComponentParent(scene, child, newParent)` (refuses cycles) |
| Delete | `deleteComponent(scene, handle)`: the whole subtree, returns every handle removed |

`resolution` must be a power of **four**; see [Voxel data](voxel-data.md#resolution-is-a-power-of-four).

## Chunks and grids

A **chunk** is one cube of voxels with a placement: `ChunkHeader { position, rotation, scale,
voxelScale, resolution }`. Its handle is its index in `Scene::chunks` **and** its row in the GPU
header texture.

| | Loose chunk | Grid cell |
|---|---|---|
| Placed by | Its own transform | The grid: `origin + cell * cellSize`, all cells share a rotation |
| Reached by the renderer | Walking `Scene::looseChunks` (and a BVH) | DDA over `SceneGrid::cellToChunk` |
| Typical use | Props, bodies, anything that moves | Terrain, large static volumes |
| `chunk.gridIndex` | `-1` | the grid |

A grid can grow in any direction (`expandGridToInclude`). Growing below its low corner shifts
`originCellCoord` and `origin` together, so existing cells keep their world positions.

## The geometry pool

A `GeometryBlob` is shared by every chunk showing the same voxels, and refcounted:

```text
chunk A ─┐
chunk B ─┼──▶ blob 7  (refCount 3)        edit chunk B:  forkBlob(7) ──▶ blob 12 (refCount 1)
chunk C ─┘                                               blob 7 keeps A and C (refCount 2)
```

- **`chunk.geometryPoolIndex < 0` means unpooled.** `internChunkGeometry` moves a chunk's own
  `geometryData` into a fresh blob. The GPU path interns anything left over.
- **`forkBlob` is copy-on-write**: a deep copy with refCount 1. It does not decrement the original;
  the caller repoints its chunk and releases. A sole owner (refCount 1) is edited in place.
- **`releaseBlob` at refCount 0** frees the slot (`blobFreeList`) and, on the next flush, the GPU range.
- **`contentStamp` names a blob's voxels.** Equal stamps, equal geometry. Physics caches collision
  shapes by it. Code that replaces `blob.geometry` in place calls `markBlobContentChanged(blob)`,
  which also clears the cached content bounds. Never clear `contentBoundsValid` alone.
- **`renderLOD`** coarsens only the GPU copy. See [Voxel data](voxel-data.md#level-of-detail).

## Handles, refs and slot reuse

**Freed rows are reused** (`Scene::slots`), so a bare handle is only good within a frame.

```cpp
ComponentRef ref = utils::refOf(scene, handle);           // handle + generation

// ... frames later ...
ComponentHandle h = utils::resolve(scene, ref);           // INVALID_COMPONENT_HANDLE once deleted,
if (h != INVALID_COMPONENT_HANDLE) { /* still the same component */ }   // whatever reused the row
```

| Rule | Why |
|---|---|
| **Hold a `ComponentRef` across frames**, check it with `utils::resolve` | `deleteComponent` bumps the row's `generation`; a later `addComponent` may take the row |
| **`utils::isComponentAlive(scene, handle)`** is the liveness check for a bare handle | Deleted rows are renamed `"__deleted__"` and emptied |
| **Create rows through the engine**: `addComponent`, `reserveComponentSlot` + `placeComponent`, `allocateChunkSlot` | `push_back` skips the free list and the root index |
| **`Scene::deletions`** counts deletes | Code holding handles (the Scene bridge) rechecks only when it moves |
| **`scene.slots.enabled = false`** keeps rows as tombstones forever | For programs that still keep bare handles (the scene editor) |
| **Never persist a handle.** Files use `localId` | Handles are positions for one load |

### `localId`

Unique among siblings, saved as the entry's `"id"`, stable across reloads, renames and reorders.
`entities.json` links by it. Ids are per folder: a folder loaded twice resolves them against each
copy's own node.

## A Scene is a value

`Scene` copies and moves, attachments and all. **A member added to `Scene` must be added to its
four copy/move operations**, or a copy silently drops it.

`documentPath` is the folder `loadComposeFromDisk` was given, which is how the runtime finds
`entities.json` beside the top-level `compose.json`.
