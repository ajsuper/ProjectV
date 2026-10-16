# World loading: one rule decides what is loaded

Status: **approved**, 2026-10-16. **L0, L1, L2 done** (2026-10-16). Next: L3. Split out of the networking plan:
loading works without networking, without physics, and without EnTT. The networking plan
(`networking.md`) builds on it.

## Goals

1. **The game writes one function; the engine does the rest.** The engine indexes what can be loaded,
   reads it from disk, keeps edits across unload, and handles the changes in between. The function
   says what should be loaded.
2. **Works at every layer.** Voxels plus renderer only: yes. Plus physics: far things can be kept but
   not simulated. Plus networking: the server runs each client's rule.
3. **Sensible defaults:** no rule means load everything, as today. `loadNear` and `loadInside` are
   one line each.
4. **Loading never loses an edit.** An edited block that unloads comes back edited.

## The rule

```cpp
enum class Presence : uint8_t { Absent, Kept, Active };

struct LoadCandidate {           // one block (L2 adds entities)
    BlockId         id;
    WorldBox        bounds;      // world space
    ComponentHandle component;   // read its name, attachments, anything: the rule's business
    core::ivec3     cell;        // grid coordinate
    Presence        current;     // what the rule said last time: for hysteresis
};
struct Viewer {                  // where the world is seen from; the game sets them
    core::vec3 position;
    uint32_t   id;               // a camera, a player, a client: the game's numbering
};

using LoadRule = std::function<Presence(const LoadCandidate&, std::span<const Viewer>)>;
struct LoadHint { float radius = 0; };   // >0: units beyond it from every viewer are Absent without a call

// Defaults
LoadRule loadEverything();                        // the default
LoadRule loadNear(float active, float kept);      // within `active` m Active, within `kept` Kept; 10% margin
LoadRule loadInside(core::aabb box, Presence inside = Presence::Active);
```

| Presence | Voxels + renderer | + physics | + networking (client) |
|---|---|---|---|
| **Absent** | not in memory | no body | not sent |
| **Kept** | loaded and drawn | a body, frozen: solid, asleep, not stepped | a body following the server's snapshots |
| **Active** | loaded and drawn | simulated | simulated, corrected by snapshots |

- Without physics, Kept and Active mean the same thing: loaded.
- **Kept is useful offline too:** a distant room keeps its stack of crates exactly where it was,
  costs no simulation, and wakes when the player comes near.
- **The rule sees viewers, not "the player".** A split-screen game has two; a strategy game's viewer
  is the camera; a server has one per player.

### Cost

- The engine keeps a spatial index of candidates. With a `LoadHint` it skips everything beyond the
  radius without calling the rule. Without one, every candidate is checked (fine for small worlds).
- It re-checks a slice of candidates each update so each is re-checked about every 0.5 s, and checks
  new candidates (spawns, a viewer that moved further than the hint) at once.
- Loads and unloads have a per-frame budget (bytes read, units made). Disk reads happen on a worker
  thread; units appear and go on the main thread, at a frame boundary.
- **Edge flicker** is the rule's business: the candidate carries what the rule said last time, and
  `loadNear` keeps a 10% margin with it.

## Units: what loads

The scene's own pieces, so nothing new is authored:

- **Blocks.** A `.data` file already has a block table (grid coords, offsets) built so one block can be
  read without the rest (`compose_data_structure.md`, "streamed"), and chunks already carry their
  residency key (`gridIndex`, `cellIndex`). A single-chunk asset is one block; a grid is one per
  cell. Bounds come from the compose transforms and the block table, without reading geometry.
- **Entities** (entities.json). An entity linked to a component goes with that component's blocks:
  present when any of them is. An unlinked entity has its own bounds from its Transform (a point, or
  the game's `LoadBounds` component), or is always present when it has none.
- **The compose tree is always loaded.** It is small: names, transforms, the hierarchy. Only voxel
  geometry and entities come and go.

## Edits survive unloading

Every edited block, whatever its mutability, is kept when it unloads: a `direct` block's edits reach
its `.data` only on save, so until then the file is the old voxels too.

- An edited block's blob moves to an **overlay** (L0: in memory; L1: spilled to a session file past
  a size limit). Reload takes it from there, and the block stays "edited" across further round trips.
- "Edited" = the chunk's blob content stamp differs from the one it was read with, or it came back
  from the overlay.

- Blocks made at runtime (shards, pieces) have no source file; they live in the overlay when unloaded.
- The overlay is what a networked server sends a client loading a block (the networking plan's
  "changes since load").
- Saving a game writes the overlay (a later decision; the format is the overlay's).

## Layering

```
utils/scene_loading.h   the index, units, the rule, the overlay, load / unload into a Scene   (no EnTT)
runtime/loading.h       entities as units, viewers from components, Presence -> physics      (EnTT)
```

- **Voxels and renderer only:** `utils::SceneLoader` alone. Open a scene, set viewers and a rule, call
  `update()` once a frame. No Application, no EnTT, no physics.
- **With the runtime:** `installWorldLoading(app, config)` adds a PreUpdate system that runs the
  loader, creates and destroys entities with their units, and sets each body's presence. Viewers
  default to the active camera; a game sets its own (`setViewers`).
- **Physics** learns nothing about loading. A body appears when its entity does (as today), and
  Kept is a new freeze mode (L3) it applies on request.

## Work in other systems

| # | Where | What |
|---|---|---|
| W1 | compose_io | open a scene with the tree and block tables only; load one block; unload one block (the residency keys exist) |
| W2 | physics | **static colliders over grids: a body per block of cells** (4x4x4 cells to start), rebuilt per block. Today one compound covers the whole component, so loading one cell would rebuild it all. Also makes edits to big static grids cheaper |
| W3 | physics | **freeze**: a body solid, asleep and not stepped until released, keeping its NetId and state. For Kept, and for the networking plan's proxies |
| W4 | physics | support and island passes over a partly loaded component: they read only loaded blocks and must not drop pieces whose support is not loaded. Rule: a block next to an unloaded block counts as anchored |
| W5 | renderer | blocks appearing and going: the GPU header rows and pool uploads already follow handles; check the cost of many per frame |

## L0, done 2026-10-16

- `utils::openComposeFromDisk(folder, blocks)`: the eager loader's walk with `lazy` set. Reads the
  `.data` headers and block tables only; each data component gets its data reference at open (an
  edit reads its resolution from it). Grids are laid out with every cell `CELL_NOT_LOADED` (-2); a
  single-block component has `chunkHandle == INVALID_CHUNK_HANDLE`.
- `utils::SceneBlocks` (`utils/scene_loading.h`): `open`, `load`, `unload`, `loadAll`, `bounds`
  (the block's cube in world space, from the component's transform as it is now), `edited`, the
  in-memory overlay (`keptBlocks`, `keptBytes`), `residentVoxelBytes`. Blocks of one source block
  and mutability share a blob, as the eager loader shares them.
- Unloaded is empty to every reader (`< 0`). Guards where a missing chunk would crash or lose data:
  edits to an unloaded block or cell are dropped and logged; `saveComposeToDisk` refuses a scene with
  unloaded blocks (`hasUnloadedBlocks`); rebake, physics' chunk lists and `voxelsInSphere`, and the
  animation chunk list skip a component with no chunk.
- Tests (`test_scene_loading.cpp`, headless): open reads no voxels; `loadAll` equals
  `loadComposeFromDisk` chunk for chunk (transforms, scales, geometry, sharing); one cell loads,
  unloads and reloads, memory back to zero and a hundred round trips leaving it where one did;
  bounds known before load and equal to the loaded chunk's cube; an edited block survives unload
  (twice) while its shared copy keeps the old voxels; edits to unloaded blocks dropped and save
  refused until everything is loaded; a deleted component's blocks are forgotten.
- Not yet (L1): cells an edit creates in a grid are not blocks, so they stay loaded; duplicating a
  partly loaded grid copies only its loaded cells.

## L1, done 2026-10-16

- `utils::SceneLoader` over `SceneBlocks`: `open`, `setRule(LoadPolicy | rule, hint)`, `setViewers`,
  `update` (once a frame), `settle` (until done: loading screens, tests), `presence`, `stats`,
  `refreshBounds` (after moving components that have blocks). Policies: `loadEverything` (default),
  `loadNear(active, kept, margin)` (sets its own hint), `loadInside(box, presence)`.
- Who is asked: everything on a full pass (after open, setRule, refreshBounds, or a viewer coming,
  going or moving `viewerMoveRecheck`, 2 m); otherwise a slice per update, so each block is asked
  within `recheckUpdates` (30); with a hint, blocks just come in range at once, blocks gone out of
  range Absent at once, and nothing beyond it ever (a spatial hash over block bounds, 64 m cells).
- Budgets: `maxLoadsPerUpdate` 32, `maxUnloadsPerUpdate` 64, `maxPendingReads` 64. Loads go nearest
  viewer first. A failed load is not retried until setRule.
- Reads on a worker thread (`threaded`, default on): it touches files only; results are installed in
  `update`, and a read for a block no longer wanted is dropped.
- Kept blocks past `keptMemoryBytes` (256 MB) spill, oldest first, to one-block `.data` files in a
  session folder removed with the loader. Kept blobs drop their brick map (rebuilt on the next edit).
- Cells an edit makes in a lazily opened grid are adopted as blocks (scanned `adoptCellsPerUpdate`
  cells a frame), edited from birth, so they unload into the kept set. `duplicateComponent` refuses a
  subtree with unloaded blocks.
- Tests (headless; the threaded ones run 50 times clean): budget and equality with the eager loader;
  `loadNear` nearest first and following the viewer, Active/Kept bands; no flicker on the edge; the
  rule never asked beyond the hint; recheck cadence without one; worker reads equal synchronous ones
  and dropped reads leave nothing; spill to files and back, files removed with the loader; an
  adopted cell after the grid grew downward is kept and restored.
- Known costs, for L2's measurements: `update` walks every block once (load/unload lists) -- fine to
  ~10^5 blocks; reading a spilled block back is synchronous on the main thread.

## L2, done 2026-10-16

- `runtime/loading.h`: `openWorld(app, folder, {loader, policy})` opens lazily into the Scene in
  ctx, installs the bridge, adds "world loading" to PreUpdate. `setLoadRule`, `setViewers` (the
  program's own), `updateWorldLoading` (the system), `settleWorld`, `worldLoader`,
  `unloadedEntityCount`.
- Components: `LoadViewer {id}` (saved; never unloaded; its position is a viewer), `LoadBounds
  {halfExtents}` (saved), `LoadState {presence}` (not saved: the rule's answer for a present entity).
- Which entities take part: Authored ones (from a file) and ones with LoadBounds. A linked one is
  present while any block under its component is loaded (a component with no blocks: always); an
  unlinked one with LoadBounds is judged by the rule (`LoadCandidate::entity` names it); the rest
  never unload.
- An absent entity is saved with `entityComponents` (new public helper, with
  `applyEntityComponents`; saveEntities uses it), its link let go with OnUnlink::Keep, and destroyed;
  it comes back as a new Entity with its Authored, link, components and NetId. `EntityUnloading` is
  triggered before it goes, `EntityReloaded` sent after. Its component's transform lives on in the
  Scene, so a moved entity comes back where it was moved to.
- Renderer (W5): nothing new needed. Blocks are chunks made and freed; the program's own
  `flushSceneUpdates` uploads them. Not measured on a GPU here (the windowed self-test is the place).
- Example `18-world-loading` (`world_loading`): writes a 768 m terrain (849 blocks of 32 m) on first
  run, flies over it with `loadNear(96, 192)`; keys switch rules. `--headless-check` flies the
  course with the loader alone: peak resident 26% of the world, worst update 0.12 ms, every block
  within 64 m loaded each second. `WORLD_LOADING_SELFTEST=1` flies it drawn.
- Tests (`test_world_loading.cpp`, an Application stepped by hand, no window): far linked and bounded
  entities unload and come back with their data, NetId and moved transform; events delivered;
  Kept/Active bands; a viewer never unloads; an entity whose component was deleted while it was out
  does not return; program viewers and a new rule apply at once.
- For L3: an unloaded entity's body loses its velocity (it is made again); Kept should freeze it
  instead. Threaded reads make *when* a block loads depend on timing, so a physics recording across
  streaming does not replay unless `threaded` is off -- the networked client loads what the server
  says, but single-player replays need a rule here.

## Milestones

| # | What | Done when |
|---|---|---|
| L0 | **W1.** Lazy open, load and unload of single blocks, bounds without geometry | A test loads one cell of a grid, unloads it, reloads it; memory returns to where it was |
| L1 | **`utils::SceneLoader`.** The rule, the index, slicing, budgets, the worker thread, the overlay | `loadNear` over a big grid in a headless test: only near blocks resident; an edited block survives unload; `loadEverything` loads what `loadComposeFromDisk` does |
| L2 | **Runtime and renderer.** `installWorldLoading`, entities as units, viewers, W5. An example: fly over a large terrain with `loadNear` | Frame time steady while flying; resident memory bounded |
| L3 | **Physics.** W2, W3, W4; Presence applied to bodies | Demo-style test: crates far away stay Kept and exactly in place, come back Active when near; a static grid streams under a walking character without a fall |

The networking plan's N6 builds on L3.

## Open questions

1. **Block-of-cells size for static bodies** (W2): 4x4x4 cells to start, measured in L3.
2. **Overlay on disk:** a session temp file now; whether it becomes the save-game format is a later
   decision.
3. **Entities without a voxel component and without bounds** load always. Should a game be able to
   say "never unless asked" instead? Likely a tag, decided in L2.
