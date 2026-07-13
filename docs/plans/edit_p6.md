# Phase 6 — Hierarchical Component Tree + Identity + Transform API

## Goal

Replace the current flat component list with a full hierarchical scene graph
matching the compose.json on-disk structure. Every compose.json component
(`data` or `asset`) gets its own `ComponentRecord` in the loaded `Scene`, and
the tree preserves parent/child relationships, local transforms, and
human-readable names.

This is the foundation every later phase depends on.

## What exists (before P6)

- `ComponentRecord` has `kind` (`Chunk` or `Grid`), `chunkHandle`,
  `gridIndex`, `sourcePath`. No hierarchy, no transform, no name.
- `loadComposeFromDisk` recursively expands `asset` components but creates
  `ComponentRecord`s **only for `data` leaves**. Asset folders vanish.
- World transforms are baked directly into `Chunk::header.position` — the
  component tree's structure is lost.
- `ComponentHandle` = vector index, stable within session, but has no
  persistent identifier. No way to look up "the bird" by name.

## What's missing

1. `ComponentKind::Asset` — asset folder references need component records.
2. Parent/child — `parent` handle + `children` list on `ComponentRecord`.
3. Local transform — `localPosition`, `localRotation`, `localScale` on each
   component (relative to parent).
4. `name` — human-readable identifier from compose.json (or auto-generated).
5. Loader must instantiate the tree: each reference to an asset folder creates
   a per-instance subtree.
6. Lookup API — `findComponentByPath`, `getComponentPath`.
7. Transform API — `setComponentPosition`, `setComponentRotation`,
   `getComponentWorldMatrix`.
8. Query API — `getComponentVoxelCount`, `listComponents`.

## Design Summary

**Instance Tree model:** Every compose.json component creates a
`ComponentRecord`. Placing an `asset` reference instantiates a subtree.
Geometry is shared via `geometryPool` (dedup by file+block+mutability);
components are per-instance.

Example: two birds, each with a wing sub-asset:

```
bird_A (Asset)          bird_B (Asset)
├── wing (Asset)         ├── wing (Asset)
│   ├── leftWing (Data)  │   ├── leftWing (Data)   // same blob
│   └── rightWing (Data) │   └── rightWing (Data)  // same blob
└── body (Data)          └── body (Data)           // same blob
```

10 components, 6 chunks, 3 geometry blobs.

**Transforms are local, world is baked.** Each `ComponentRecord` stores
`localPosition`/`localRotation`/`localScale`. World transforms are computed
by walking parents and still baked into chunk headers for the renderer (no
renderer changes).

**Names are local, paths computed.** Each component has a short local `name`
(from compose.json or auto-generated). `getComponentPath` walks parents to
produce paths like `"bird_A/wing/leftWing"`.

## Sub-phases

### P6.1 — Data structure changes (scene.h)

**Files:** `include/data_structures/scene.h`

Add `ComponentKind::Asset`, `ComponentRecord` fields for hierarchy, local
transform, and name:

```cpp
enum class ComponentKind { Chunk, Grid, Asset };

struct ComponentRecord {
    ComponentKind kind = ComponentKind::Asset;

    // Identity
    std::string name;              // local name (from compose.json or auto-generated)
    std::string sourcePath;        // .data path (Chunk/Grid) or folder path (Asset)

    // Hierarchy (Chunk, Grid, Asset — all three participate)
    ComponentHandle parent = INVALID_COMPONENT_HANDLE;
    std::vector<ComponentHandle> children;   // populated for Asset; empty for Chunk/Grid

    // Local transform (relative to parent; all three use this)
    core::vec3 localPosition = core::vec3(0.0f);
    core::quat localRotation = core::quat(1.0f, 0.0f, 0.0f, 0.0f);
    float      localScale = 1.0f;              // uniform only (v0.0)

    // Data linkage (valid only for Chunk/Grid)
    ChunkHandle chunkHandle = 0;               // Chunk
    int32_t     gridIndex = -1;                // Grid

    // Editing (P6 — placeholder, populated by P7)
    ComponentEditQueue editQueue;
    int32_t            dataRefID = -1;
};
```

Constructor changes: keep the aggregate init working. The default constructor
starts with `kind=Asset`, `parent=INVALID_COMPONENT_HANDLE`, empty children.

**Verify:** `ComponentRecord` is the right size. Existing code that uses
`ComponentRecord{kind, chunkHandle, gridIndex, sourcePath}` still compiles
(aggregate init — the remaining fields take defaults).

### P6.2 — Add `name` to ComposeComponent (compose.h + compose_io.cpp)

**Files:** `include/data_structures/compose.h`, `src/utils/compose_io.cpp`

Add `name` field:

```cpp
struct ComposeComponent {
    ComponentType type = ComponentType::Data;
    std::string   source;
    std::string   name;          // NEW: from "name" in JSON, or auto-generated
    core::vec3    position = core::vec3(0.0f);
    core::quat    rotation = core::quat(1.0f, 0.0f, 0.0f, 0.0f);
    core::vec3    scale = core::vec3(1.0f);
    Mutability    mutability = Mutability::Locked;
};
```

**P6.2a — JSON parsing for `name`**

In `parseComposeJson`, add a case for `"name"` in the component parsing
code. Treat it as an optional string. If absent, leave it empty (auto-
generation happens in the loader, not the parser).

Search for where `"source"`, `"position"`, `"rotation"`, `"scale"`, and
`"mutability"` are parsed. Add `"name"` alongside them.

**P6.2b — Name auto-generation in loader**

In `loadComposeFromDisk`, after parsing each `ComposeComponent`:

```cpp
// Auto-generate name if absent
if (c.name.empty()) {
    if (c.type == ComponentType::Data) {
        // filename stem: "bird.data" → "bird"
        std::filesystem::path p(c.source);
        c.name = p.stem().string();
    } else {
        // folder name: "Bird/" → "Bird"  (or use the compose.json's name field)
        std::filesystem::path p(c.source);
        c.name = p.filename().string();
    }
}

// Disambiguate siblings at the same level
// Keep a per-level set of seen names; append "_1", "_2" etc. for duplicates
```

Store the disambiguation set per recursion level (a `std::unordered_set<std::string>`
created in each `expand` call and passed down, or maintained on the stack).

### P6.3 — Loader rewrite: create Asset components + parent/child (compose_io.cpp)

**Files:** `src/utils/compose_io.cpp`

This is the largest single change. The loader's `expand` lambda must be
rewritten to create component records for **every** compose.json entry, not
just data leaves.

**P6.3a — Change `expand` signature**

```cpp
// Before: transparent recursion, no asset components
std::function<void(const std::string& folder, const core::mat4& parentWorld,
                   int depth, bool ancestorRotated)> expand = ...;

// After: each call is responsible for its own component
// parentHandle = INVALID for root; ComponentHandle of the asset component
//                for nested calls
std::function<void(const std::string& folder, const core::mat4& parentWorld,
                   ComponentHandle parentHandle, int depth)> expand = ...;
```

**P6.3b — Inside the component loop, always create a ComponentRecord first**

Pseudo-code for the `for (const ComposeComponent& c : doc.components)` loop:

```cpp
// 1. Compute local and world transforms (unchanged from current)
core::mat4 local = glm::translate(core::mat4(1.0f), c.position)
                 * glm::mat4_cast(c.rotation)
                 * glm::scale(core::mat4(1.0f), c.scale);
core::mat4 world = parentWorld * local;

// 2. Create ComponentRecord (every component gets one)
ComponentHandle myHandle = static_cast<ComponentHandle>(scene.components.size());
scene.components.push_back(ComponentRecord{});
ComponentRecord& rec = scene.components.back();
rec.name = c.name;                    // already auto-generated above
rec.sourcePath = resolved;
rec.localPosition = c.position;
rec.localRotation = c.rotation;
rec.localScale = c.scale.x;           // uniform scale, v0.0
rec.parent = parentHandle;

if (c.type == ComponentType::Data) {
    // 3a. Read .data, create chunks/grid exactly as now
    //     Set rec.kind = Chunk or Grid, rec.chunkHandle / rec.gridIndex
    //     Bake world into chunk headers (unchanged)
    //     chunk.componentHandle = myHandle  (was already set!)
    rec.kind = (dataFile.blocks.size() > 1) ? ComponentKind::Grid : ComponentKind::Chunk;
    // ... existing chunk creation code, but use `scene.chunks` directly
    //     instead of looseChunks / pendingGrids separate containers
} else {
    // 3b. Asset — recurse
    rec.kind = ComponentKind::Asset;
    // Folder push/pop as today
    expand(resolved, world, myHandle, depth + 1);
}

// 4. Link to parent
if (parentHandle != INVALID_COMPONENT_HANDLE) {
    scene.components[parentHandle].children.push_back(myHandle);
}
```

**P6.3c — Merge chunk creation into tree order**

The current code uses separate `looseChunks` and `pendingGrids` containers,
then assembles them in order (loose first, then grids). With the tree model:

- Create chunks directly in `scene.chunks` during the tree traversal.
- No more `looseChunks` vector — the component tree's data leaves are the
  authoritative list of chunks.
- The `looseChunks` and `looseChunkCount` fields on `Scene` are
  **deprecated but kept** for backward compatibility with the renderer's
  loose-list path. Compute them from the tree after assembly:

```cpp
// After all expands complete, rebuild loose list from tree
scene.looseChunks.clear();
for (ChunkHandle h = 0; h < scene.chunks.size(); ++h) {
    if (scene.chunks[h].alive && scene.chunks[h].gridIndex < 0)
        scene.looseChunks.push_back(h);
}
scene.looseChunkCount = static_cast<uint32_t>(scene.looseChunks.size());
```

This is a key change. The existing assembly logic that assigns grid cell
chunks to absolute indices must be replaced with direct creation.

**P6.3d — Rebase grid cellToChunk**

Currently, grid cell chunks are stored in `pendingGrids` as relative indices
(0, 1, 2 within the grid), then rebased to absolute during assembly by
adding `base`. With tree-order creation, chunks are absolute from the start:

```cpp
if (dataFile.blocks.size() > 1) {
    // Grid: create grid component, then create each block's chunk
    rec.kind = ComponentKind::Grid;
    rec.gridIndex = static_cast<int32_t>(scene.grids.size());

    SceneGrid grid;
    // ... populate grid fields as today ...
    grid.cellToChunk.assign(dims.x * dims.y * dims.z, -1);

    for (const DataBlock& block : dataFile.blocks) {
        // Create chunk directly (not via pendingGrids)
        Chunk chunk = makeChunk(block);
        chunk.gridIndex = rec.gridIndex;
        int lin = block.gridX + dims.x * (block.gridY + dims.y * block.gridZ);
        chunk.cellIndex = lin;
        chunk.componentHandle = myHandle;
        grid.cellToChunk[lin] = static_cast<int32_t>(scene.chunks.size());
        scene.chunks.push_back(std::move(chunk));
    }
    scene.grids.push_back(std::move(grid));
} else {
    // Single block: loose chunk
    rec.kind = ComponentKind::Chunk;
    for (const DataBlock& block : dataFile.blocks) {
        Chunk chunk = makeChunk(block);
        chunk.componentHandle = myHandle;
        rec.chunkHandle = static_cast<ChunkHandle>(scene.chunks.size());
        scene.chunks.push_back(std::move(chunk));
    }
}
```

### P6.4 — Component naming + lookup API (include/utils/scene_query.h + src/utils/scene_query.cpp)

**Files (new):** `include/utils/scene_query.h`, `src/utils/scene_query.cpp`

Create a new utility module for scene interrogation (separate from editing):

```cpp
namespace projv::utils {

    // --- Path-based lookup ---

    // Walk parents to build the full path: "bird_A/wing/leftWing"
    std::string getComponentPath(const Scene& scene, ComponentHandle h);

    // Parse a "/"-separated path and walk the tree to find the component.
    // Returns INVALID_COMPONENT_HANDLE if not found.
    // Path is from the root: "bird_A/wing" means start at root children,
    // find "bird_A", find child named "wing".
    ComponentHandle findComponentByPath(const Scene& scene, const std::string& path);

    // Find all components whose local name matches (fast linear scan).
    // May return multiple results (siblings with the same name).
    std::vector<ComponentHandle> findComponentsByName(const Scene& scene,
                                                       const std::string& localName);

    // --- Enumeration ---

    struct ComponentInfo {
        ComponentHandle handle;
        std::string     name;
        std::string     fullPath;
        ComponentKind   kind;
        std::string     sourcePath;
        core::vec3      worldPosition;
        uint32_t        voxelCount;  // 0 for Asset components
    };

    // List all components in tree order (depth-first), with metadata.
    std::vector<ComponentInfo> listComponents(const Scene& scene);

    // --- Per-component queries ---

    uint32_t getComponentVoxelCount(const Scene& scene, ComponentHandle h);
}
```

Implementation notes for `findComponentByPath`:

```cpp
ComponentHandle findComponentByPath(const Scene& scene, const std::string& path) {
    if (path.empty() || path == "/") return INVALID_COMPONENT_HANDLE;
    auto parts = split(path, '/');

    // Start at root: search components with parent == INVALID
    for (ComponentHandle h = 0; h < scene.components.size(); ++h) {
        if (scene.components[h].parent != INVALID_COMPONENT_HANDLE) continue;
        if (scene.components[h].name == parts[0]) {
            // Found root-level match, walk children for remaining parts
            return findChildByPath(scene, h, parts, 1);
        }
    }
    return INVALID_COMPONENT_HANDLE;
}

// Recursive helper
ComponentHandle findChildByPath(const Scene& scene, ComponentHandle parent,
                                 const std::vector<std::string>& parts, size_t idx) {
    if (idx >= parts.size()) return parent;
    for (ComponentHandle child : scene.components[parent].children) {
        if (scene.components[child].name == parts[idx]) {
            return findChildByPath(scene, child, parts, idx + 1);
        }
    }
    return INVALID_COMPONENT_HANDLE;
}
```

Implementation notes for `getComponentPath`:

```cpp
std::string getComponentPath(const Scene& scene, ComponentHandle h) {
    std::vector<std::string> parts;
    while (h != INVALID_COMPONENT_HANDLE) {
        parts.push_back(scene.components[h].name);
        h = scene.components[h].parent;
    }
    std::reverse(parts.begin(), parts.end());
    return join(parts, "/");
}
```

### P6.5 — Transform API (src/utils/scene_query.cpp, same file)

**Files:** `include/utils/scene_query.h`, `src/utils/scene_query.cpp`

```cpp
namespace projv::utils {

    // --- World transform computation ---

    // Walk parent chain, compose local transforms: world = parent * local.
    core::mat4 getComponentWorldMatrix(const Scene& scene, ComponentHandle h);

    // Convenience: extract position from the world matrix.
    core::vec3 getComponentWorldPosition(const Scene& scene, ComponentHandle h);

    // Convenience: extract rotation from the world matrix.
    core::quat getComponentWorldRotation(const Scene& scene, ComponentHandle h);

    // --- Transform mutation (position + rotation only; scale is load-time in v0.0) ---

    // Set the LOCAL position of a component. Does NOT affect children —
    // children keep their local transforms; the world transform of the
    // entire subtree changes because the parent moved.
    // After calling, rebakes chunk headers for all data leaves in the subtree.
    // Caller must call flushSceneUpdates or updateChunkHeader per affected
    // chunk to push changes to GPU.
    void setComponentPosition(Scene& scene, ComponentHandle h, const core::vec3& localPos);

    // Set the LOCAL rotation. Same semantics as setComponentPosition.
    void setComponentRotation(Scene& scene, ComponentHandle h, const core::quat& localRot);

    // Set both position and rotation in one call (avoids double rebake).
    void setComponentTransform(Scene& scene, ComponentHandle h,
                               const core::vec3& localPos, const core::quat& localRot);
}
```

Implementation for `getComponentWorldMatrix`:

```cpp
core::mat4 getComponentWorldMatrix(const Scene& scene, ComponentHandle h) {
    core::mat4 m(1.0f);
    while (h != INVALID_COMPONENT_HANDLE) {
        const ComponentRecord& c = scene.components[h];
        core::mat4 local = glm::translate(core::mat4(1.0f), c.localPosition)
                         * glm::mat4_cast(c.localRotation)
                         * glm::scale(core::mat4(1.0f), core::vec3(c.localScale));
        m = local * m;   // compose from root outward
        h = c.parent;
    }
    return m;
}
```

Implementation for `setComponentTransform` (the "rebake" step):

```cpp
void setComponentTransform(Scene& scene, ComponentHandle h,
                           const core::vec3& localPos, const core::quat& localRot) {
    ComponentRecord& c = scene.components[h];
    c.localPosition = localPos;
    c.localRotation = localRot;

    // Rebake world transforms for this component and all descendants
    rebakeSubtree(scene, h, getComponentWorldMatrix(scene, h));
}

// Recursive helper: recompute and bake world transforms
static void rebakeSubtree(Scene& scene, ComponentHandle h, const core::mat4& world) {
    ComponentRecord& c = scene.components[h];

    if (c.kind == ComponentKind::Chunk) {
        Chunk& chunk = scene.chunks[c.chunkHandle];
        chunk.header.position = core::vec3(world * core::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        core::mat3 rotMat(world);
        chunk.header.rotation = core::quat_cast(rotMat);
        // scale was baked at load time and stays fixed (v0.0)
    } else if (c.kind == ComponentKind::Grid) {
        SceneGrid& grid = scene.grids[c.gridIndex];
        grid.origin = core::vec3(world * core::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        core::mat3 rotMat(world);
        grid.rotation = core::quat_cast(rotMat);
        // Recompute each cell chunk header position from grid + cell offset
        for (int32_t ci = 0; ci < static_cast<int32_t>(grid.cellToChunk.size()); ++ci) {
            int32_t chIdx = grid.cellToChunk[ci];
            if (chIdx < 0) continue;
            int iz = ci / (grid.dims.x * grid.dims.y);
            int iy = (ci / grid.dims.x) % grid.dims.y;
            int ix = ci % grid.dims.x;
            core::vec3 cellOffset = glm::mat3_cast(grid.rotation) *
                                    (core::vec3(ix, iy, iz) * grid.cellSize);
            Chunk& chunk = scene.chunks[chIdx];
            chunk.header.position = grid.origin + cellOffset;
            chunk.header.rotation = grid.rotation;
        }
    }
    // Asset: no chunk to rebake; recurse into children
    for (ComponentHandle child : c.children) {
        const ComponentRecord& childRec = scene.components[child];
        core::mat4 childLocal = glm::translate(core::mat4(1.0f), childRec.localPosition)
                              * glm::mat4_cast(childRec.localRotation)
                              * glm::scale(core::mat4(1.0f), core::vec3(childRec.localScale));
        rebakeSubtree(scene, child, world * childLocal);
    }
}
```

### P6.6 — Update existing code for the new ComponentKind

**Files:** `src/utils/editing.cpp`, `docs/examples/editing_p1/main.cpp`,
`docs/examples/edit_demo/main.cpp`, `src/graphics/gpu_interface.cpp`

**P6.6a — editing.cpp: skip Asset components**

In `applyComponentQueue` and `updateScene`, add an early skip:

```cpp
if (comp.kind == ComponentKind::Asset) {
    comp.editQueue.ops.clear();  // Asset components should never have ops,
    return true;                  // but be defensive.
}
```

**P6.6b — edit_demo and editing_p1: skip Asset kinds in find loops**

In both examples, the code that searches for Chunk/Grid components must skip
Asset:

```cpp
for (ComponentHandle h = 0; h < scene.components.size(); ++h) {
    auto kind = scene.components[h].kind;
    if (kind == ComponentKind::Asset) continue;  // skip folders
    if (kind == ComponentKind::Chunk && looseOut == INVALID) looseOut = h;
    if (kind == ComponentKind::Grid && gridOut == INVALID) gridOut = h;
}
```

**P6.6c — gpu_interface.cpp: no change needed**

`makeHeader` reads `chunk.componentHandle` → `ComponentRecord::dataRefID`.
Data components still have valid dataRefID. Asset components are skipped
because they don't appear in the chunk loop (only chunks reference them).
**No GPU code changes.**

## Verification

Before each sub-phase, compile and run the existing test driver to ensure
nothing broke:

```bash
cmake -S . -B build && cmake --build build --target projectV-editing
make -C docs/examples/editing_p1 clean && make -C docs/examples/editing_p1
./docs/examples/editing_p1/editing_p1
```

After P6.3 (loader rewrite), verify that the loaded scene has the expected
tree structure: correct number of components, correct parent/child links,
correct local transforms, correct world positions baked into chunk headers.

After P6.4 and P6.5, add new test code to `editing_p1/main.cpp`:
- Load a scene, call `getComponentPath` on a known component, verify path.
- Call `findComponentByPath` with that path, verify it returns the same handle.
- Call `setComponentPosition` on an Asset component, verify child chunk headers
  updated (world position changed).

## File change summary

| File | Change |
|------|--------|
| `include/data_structures/scene.h` | Add `ComponentKind::Asset`. Add `name`, `parent`, `children`, `localPosition`, `localRotation`, `localScale` to `ComponentRecord`. |
| `include/data_structures/compose.h` | Add `name` to `ComposeComponent`. |
| `src/utils/compose_io.cpp` | Parse `"name"` in JSON. Auto-generate names. Rewrite loader for instance tree. Create Asset components. Parent/child linking. |
| `include/utils/scene_query.h` | **New file.** Declare lookup + transform + query API. |
| `src/utils/scene_query.cpp` | **New file.** Implement lookup + transform + query API. |
| `src/utils/editing.cpp` | Skip `ComponentKind::Asset` in `applyComponentQueue`. |
| `docs/examples/editing_p1/main.cpp` | Skip Asset kinds; add P6-specific tests. |
| `docs/examples/edit_demo/main.cpp` | Skip Asset kinds in component search. |
| `include/utils/editing.h` | No change (public API unchanged). |
| `AGENTS.md` | Update Phase 6 status. |

## Definition of done

- Every compose.json component (`data` or `asset`) gets exactly one
  `ComponentRecord` in the loaded scene.
- Parent/child links are correct for nested assets.
- Local transforms on `ComponentRecord` match what's in compose.json.
- World positions baked into chunk headers match `getComponentWorldMatrix`.
- `getComponentPath("bird_A/wing")` works and returns the correct handle.
- `findComponentByPath("bird_A/wing/leftWing")` round-trips correctly.
- `setComponentPosition(bird_A, newPos)` updates all descendant chunk headers.
- No renderer changes needed (GPU code reads baked headers).
- All existing P1-P5 assertions still pass.