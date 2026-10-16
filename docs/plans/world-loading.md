# World loading: one rule decides what is loaded

Status: **draft**, 2026-10-16. Split out of the networking plan: loading works without networking,
without physics, and without EnTT. The networking plan (`networking.md`) builds on it.

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

struct LoadCandidate {           // one loadable unit (below)
    core::aabb  bounds;          // world space
    UnitId      id;              // stable: (component localId path, block grid coords)
    UnitKind    kind;            // Block (a chunk or grid cell) or Entity
    std::span<const std::string> tags;   // from compose.json / entities.json
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
- **Edge flicker** is the rule's business: `loadNear` keeps a 10% margin, and `withMargin(rule,
  0.1f)` wraps any rule that returns distances.

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

| Mutability (compose.json) | An edited block unloads |
|---|---|
| `direct` | edits are written to the source `.data` already; reload reads them |
| `copy`, `locked` | the edited blob moves to an **overlay**: in memory, spilled to a session file past a size limit; reload takes it from there |

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
