# Glossary

Engine-specific terms, with where each is explained.

| Term | Meaning |
|---|---|
| **Asset** | A component of kind `Asset`: a folder node with children and no voxels of its own. [Scene](../concepts/scene.md#components) |
| **Attachment** | Tool data about the voxel structure, saved with a component in `compose.json`, never interpreted by the engine. [Where data goes](../concepts/where-data-goes.md) |
| **Blob** | A `GeometryBlob` in `Scene::geometryPool`: one set of voxels, refcounted and shared by every chunk showing it. [Scene](../concepts/scene.md#the-geometry-pool) |
| **Brick map** | The authoring form of a blob: 64³ bricks, each a bitmask plus a slot map. Derived, never saved. [Voxel data](../concepts/voxel-data.md#brick-maps) |
| **Brick / block / region** | The tile table's three levels: 4³, 16³ and 64³ voxels. [Tile table](formats/tile-table.md) |
| **Chunk** | One placed cube of voxels (`Chunk`): a header plus a blob. Its handle is its row in `Scene::chunks` and on the GPU. [Scene](../concepts/scene.md#chunks-and-grids) |
| **Component** | One entry of a `compose.json` in memory (`ComponentRecord`): `Asset`, `Chunk` or `Grid`. Not an ECS component. [Scene](../concepts/scene.md#components) |
| **ComponentRef** | A component handle plus its generation: the safe way to hold a component across frames. [Scene](../concepts/scene.md#handles-refs-and-slot-reuse) |
| **Compose folder** | A folder with a `compose.json`: the unit of loading. A scene and an asset are the same thing. [compose.json](formats/compose-json.md) |
| **Content stamp** | `GeometryBlob::contentStamp`: names a blob's voxels; changes when they change. Physics shapes are cached by it. [Scene](../concepts/scene.md#the-geometry-pool) |
| **Document node** | The Asset node a folder becomes when loaded inside something else; `"link": "document"` in `entities.json`. [Entities](../concepts/entities.md) |
| **Envelope** | A quarter-resolution tree64 marking where animated voxels may be drawn. [Voxel data](../concepts/voxel-data.md#animation-envelopes) |
| **Entity** | Local to the process: never send or save one; use a `NetId`. [Entities](../concepts/entities.md) |
| **Generation** | Bumped each time a component row is freed, so a `ComponentRef` can tell its component from the row's next occupant. [Scene](../concepts/scene.md#handles-refs-and-slot-reuse) |
| **Grid** | A component of kind `Grid`: a `SceneGrid` of equal, aligned chunks, one per cell. [Scene](../concepts/scene.md#chunks-and-grids) |
| **Handle** | `ComponentHandle` / `ChunkHandle`: a row index. Only good within a frame; never persisted. [Scene](../concepts/scene.md#handles-refs-and-slot-reuse) |
| **Header row** | A chunk's `GPUChunkHeader` in the header texture: placement, data ranges, LOD, content box. [Rendering](../concepts/rendering.md#the-scene-on-the-gpu) |
| **Interning** | Asking for a palette slot by colour (`internMaterial`) or moving a chunk's geometry into the pool (`internChunkGeometry`). [Materials](../concepts/materials.md) |
| **Link** | An entity's `VoxelComponent`: **Root** (a top-level component, world transform) or **Part** (inside an asset, parent space). [Entities](../concepts/entities.md) |
| **localId** | A component's persistent id, unique among siblings, saved as `"id"`. What `entities.json` links by. [Scene](../concepts/scene.md#localid) |
| **Loose chunk** | A chunk placed by its own transform, listed in `Scene::looseChunks`. Props and bodies. [Scene](../concepts/scene.md#chunks-and-grids) |
| **Material slot** | A voxel's byte: an index into its component's palette. [Materials](../concepts/materials.md) |
| **Motion set** | One of 16 rows of the animation table a material names: sway or advection. [Materials](../concepts/materials.md#flags-byte) |
| **Mutability** | `locked` / `direct` / `copy` on a `data` entry: today decides instancing at load. [Editing](../concepts/editing.md#saving) |
| **NetId** | The id that names an entity on every machine. Everything networked, replayed or compared uses it. [Entities](../concepts/entities.md#netid-the-name-that-crosses-machines) |
| **Palette** | Per component, up to 255 `Material` entries (slot 255 is none). [Materials](../concepts/materials.md) |
| **Pass** | One full-screen shader draw in a renderer, from inputs to a framebuffer. [Rendering](../concepts/rendering.md#passes) |
| **Piece** | What a break made: a shard or an island that came away. Carries `Piece`; the oldest past `maxPieces` are destroyed. [Physics](../concepts/physics.md#fracture) |
| **Cheap debris** | A small piece of a break moved by a fall and a bounce, with no body. [Physics](../concepts/physics.md#cheap-debris) |
| **Physics material** | Density, friction, restitution and breakage, by name in a library; palette entries and bodies name one. [Materials](../concepts/materials.md#physics-materials) |
| **Ping-pong** | A framebuffer a pass reads and writes gets two copies, swapped each frame. [Rendering](../concepts/rendering.md#passes) |
| **Prefab** | A compose folder (plus `entities.json`) grafted at runtime with `instantiatePrefab`; cached and shared. [Entities](../concepts/entities.md#prefabs) |
| **renderLOD** | How many levels a blob's GPU copy drops (4× per level). VRAM only. [Voxel data](../concepts/voxel-data.md#level-of-detail) |
| **Renderer** | A folder: `render.json`, `resources.json`, shaders. [Renderer folder](formats/renderer-folder.md) |
| **Resolution** | A chunk's edge length in voxels: a power of four. [Voxel data](../concepts/voxel-data.md#resolution-is-a-power-of-four) |
| **Resource** | Global state in `world.ctx()`: `Scene`, `GPUData`, `Time`, `Input`, your own. [Frame loop](../concepts/frame-loop.md#resources-worldctx) |
| **Scene** | `projv::Scene`: components, chunks, grids, the geometry pool. [Scene](../concepts/scene.md) |
| **Scene bridge** | The runtime system that writes changed entity `Transform`s into their linked components. [Entities](../concepts/entities.md#the-scene-bridge) |
| **Slot reuse** | Freed component and chunk rows are reused (`Scene::slots`). [Scene](../concepts/scene.md#handles-refs-and-slot-reuse) |
| **Tile / tile table** | A tile is a child that is entirely solid and one material: a bit and a byte, nothing below it. A tile table is a blob's voxels as regions, blocks and bricks. [Voxel data](../concepts/voxel-data.md#tile-tables), [format](formats/tile-table.md) |
| **tree64** | The sparse 64-way tree voxels were stored and marched in; now the form of `.data` v2–v3 and the authoring path. [Voxel data](../concepts/voxel-data.md#tree64), [format](formats/tree64.md) |
| **Uniform leaf** | A leaf (tree64 or tile table) whose voxels all share a slot, storing one material byte. [Voxel data](../concepts/voxel-data.md#tree64) |
| **voxelScale** | World size of one voxel. With resolution, fixes a component's size. [Voxel data](../concepts/voxel-data.md#resolution-is-a-power-of-four) |
| **World** | `projv::World` = `entt::registry`; resources live in `ctx()`. [Architecture](../concepts/architecture.md#two-worlds-linked) |
