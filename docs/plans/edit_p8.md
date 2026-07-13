# Phase 8 — Persistence (writeComposeToDisk)

## Goal

Add the ability to write a runtime `Scene` back to disk as a compose folder
(compose.json + .data files). Honor mutability policies: Locked = skip,
Direct = write in-place, Copy = write new file. Preserve the hierarchical
component tree structure.

Also harden the GPU incremental upload path and fix any rough edges from P6/P7
before the team builds on top of them.

Depends on P6 (hierarchical component tree) and P7 (EditingPolicy).

## What exists (before P8)

- `loadComposeFromDisk` is complete (P6 rewrite).
- `EditingPolicy` carries mutability per component (P7).
- `SceneDiff` reports what changed after edits (P7).
- `writeDataFile` exists (`compose_io.cpp`) — writes a single .data file.
- `flushSceneUpdates` exists for incremental GPU upload (Phase 5).

## What's missing

1. `writeComposeToDisk(Scene&, std::string path, EditingPolicy&)` — the
   top-level persist function.
2. Policy-driven write behavior: Locked = skip, Direct = write source in
   place, Copy = write new file + update sourcePath.
3. Tree structure reconstruction — write compose.json with asset hierarchy
   from the component tree.
4. Round-trip correctness: load → edit → save → reload produces an identical
   tree (handles will differ, but structure/names/transforms/geometry match).
5. `flushSceneUpdates` hardening from the P5 "TODO" (only needed if the P5
   implementation left edge cases).

## Sub-phases

### P8.1 — writeComposeToDisk top-level (compose_io.h, compose_io.cpp)

**Files:** `include/utils/compose_io.h`, `src/utils/compose_io.cpp`

**P8.1a — Declare in compose_io.h**

```cpp
/**
 * Write a Scene back to disk as a compose folder. Creates or overwrites:
 *   folder/compose.json          — component tree with hierarchy
 *   folder/<name>.data           — one .data file per unique source
 *   folder/<name>_<fork>.data    — new .data files for Copy-forked components
 *
 * @param scene    The scene to write.
 * @param folderPath The folder to write into (must exist or be creatable).
 * @param policy   The editing policy (controls Locked/Direct/Copy behavior).
 * @return true on success, false on error.
 */
bool writeComposeToDisk(const Scene& scene, const std::string& folderPath,
                        const EditingPolicy& policy);
```

**P8.1b — Implementation overview**

```cpp
bool writeComposeToDisk(const Scene& scene, const std::string& folderPath,
                        const EditingPolicy& policy) {
    // 1. Collect unique source .data files and their write-back status.
    //    (Direct = overwrite original; Copy = write new file;
    //     Locked = skip)
    struct DataWriteAction {
        std::string originalPath;      // absolute path from load
        std::string writePath;         // where to write (original or new)
        DataFile    data;              // to be written
        bool        isNewFile;         // true if Copy-forked (new path)
    };
    std::vector<DataWriteAction> writeActions;

    // 2. Walk the component tree. For each Data component:
    //    a. Skip if Locked (no persist).
    //    b. Direct: rebuild DataFile from current chunks, write to original path.
    //    c. Copy: rebuild DataFile, write to new path (append _fork to stem).

    // 3. Write compose.json by walking the component tree (P8.2).

    // 4. Execute data writes.
    for (const auto& action : writeActions) {
        // Ensure directory exists
        std::filesystem::create_directories(
            std::filesystem::path(action.writePath).parent_path());
        writeDataFile(action.writePath, action.data);
    }

    return true;
}
```

**P8.1c — Rebuild DataFile from component state**

For a Chunk component (single block):

```cpp
DataFile rebuildDataFile(const Scene& scene, ComponentHandle h) {
    const ComponentRecord& rec = scene.components[h];
    Chunk chunk = scene.chunks[rec.chunkHandle];
    GeometryBlob& blob = scene.geometryPool[chunk.geometryPoolIndex];

    DataFile df;
    df.version = 1;
    df.resolution = chunk.header.resolution;
    df.voxelScale = chunk.header.voxelScale;
    df.hasVoxelTypeData = !blob.voxelTypeData.empty();

    DataBlock block;
    block.gridX = 0; block.gridY = 0; block.gridZ = 0;
    block.geometry = blob.geometry;
    block.voxelTypeData = blob.voxelTypeData;
    df.blocks.push_back(std::move(block));

    return df;
}
```

For a Grid component (multi-block): iterate `grid.cellToChunk`, rebuild a
`DataBlock` per populated cell, extracting the block's grid coords from the
cell index.

### P8.2 — Write compose.json with hierarchy (compose_io.cpp)

**Files:** `src/utils/compose_io.cpp`

**P8.2a — Walk the component tree recursively**

```cpp
static void writeComposeJsonRecursive(const Scene& scene,
                                       ComponentHandle parent,
                                       nlohmann::json& components) {
    // Find all children of `parent`
    for (ComponentHandle h = 0; h < scene.components.size(); ++h) {
        const ComponentRecord& rec = scene.components[h];
        if (rec.parent != parent) continue;

        nlohmann::json comp;

        // Name (if different from auto-generated default)
        comp["name"] = rec.name;

        // Transform (only write if non-default)
        if (rec.localPosition != core::vec3(0.0f))
            comp["position"] = {rec.localPosition.x, rec.localPosition.y, rec.localPosition.z};
        // ... rotation, scale ...

        if (rec.kind == ComponentKind::Asset) {
            comp["type"] = "asset";
            comp["source"] = rec.sourcePath;  // original asset folder path
            // Recurse into children
            comp["components"] = nlohmann::json::array();
            writeComposeJsonRecursive(scene, h, comp["components"]);
        } else {
            comp["type"] = "data";
            // source depends on write action (may point at original or forked .data)
            comp["source"] = getDataWritePath(scene, h);  // filename only
            // Mutability from EditingPolicy
            // (loaded into policy by the caller; we don't store it in Scene)
        }

        components.push_back(std::move(comp));
    }
}
```

**P8.2b — Write compose.json**

```cpp
void writeComposeJson(const Scene& scene, const std::string& folderPath,
                       const EditingPolicy& policy) {
    nlohmann::json root;
    root["version"] = 1;
    root["components"] = nlohmann::json::array();
    writeComposeJsonRecursive(scene, INVALID_COMPONENT_HANDLE, root["components"]);
    // Also write mutability for data components from policy
    // (handled during recursing — write "mutability" for each data component)

    std::string path = (std::filesystem::path(folderPath) / "compose.json").string();
    std::ofstream out(path);
    out << root.dump(4);
}
```

### P8.3 — Copy-forked file naming convention

For `Copy` components whose blob diverged from the original, write a new
.data file alongside the original:

```cpp
std::string getForkedDataPath(const std::string& originalPath, int forkId) {
    std::filesystem::path p(originalPath);
    std::string stem = p.stem().string();
    return (p.parent_path() / (stem + "_fork" + std::to_string(forkId) + ".data")).string();
}
```

The `forkId` is per-original-file, incremented for each divergent copy. The
`ComponentRecord::sourcePath` is updated (in the scene, not persisted to
disk — the compose.json will reference the new filename; the original
sourcePath in the scene points at the pre-fork .data for display/debugging).

### P8.4 — Update tests (editing_p1)

**Files:** `docs/examples/editing_p1/main.cpp`

Add round-trip test:

```cpp
// --- Round-trip test ---
{
    // Load scene
    Scene scene = loadComposeFromDisk("./SponzaScene/");
    EditingPolicy policy = /* default: all Copy */;

    // Edit something
    ComponentHandle h = findComponentByPath(scene, "Sponza/terrain");
    queueVoxelAdd(scene, h, /* some ops */);
    updateScene(EditingContext{scene, policy});

    // Save
    bool ok = writeComposeToDisk(scene, "./roundtrip_test/", policy);
    check(ok, "writeComposeToDisk succeeded");

    // Reload from saved
    Scene reloaded = loadComposeFromDisk("./roundtrip_test/");
    check(reloaded.chunks.size() == scene.chunks.size(),
          "round-trip: same chunk count");
    check(reloaded.components.size() == scene.components.size(),
          "round-trip: same component count");

    // Clean up
    std::filesystem::remove_all("./roundtrip_test/");
}
```

### P8.5 — flushSceneUpdates hardening

**Files:** `src/graphics/gpu_interface.cpp`

Review the P5 implementation for any rough edges:

- Ensure `uploadDirtyBlobs` handles `blobRanges` correctly after pool slot
  recycling (the P5.2 checklist item).
- Ensure `growDataTextures` correctly repacks when allocators fill.
- Add a `gpuData.hasGPU` bool field (separate from the static flag) for
  per-instance GPU state tracking.
- Verify that `syncSceneTables` correctly handles Asset components (which
  have no chunks or grids). It already iterates grids and loose chunks —
  should be fine, but add a comment.

No major API changes. This sub-phase is about correctness and edge case
coverage.

## File change summary

| File | Change |
|------|--------|
| `include/utils/compose_io.h` | Declare `writeComposeToDisk`. |
| `src/utils/compose_io.cpp` | Implement `writeComposeToDisk`. Implement compose.json writer. Implement DataFile rebuild from scene state. Forked file naming. |
| `src/graphics/gpu_interface.cpp` | Add `GPUData::hasGPU` field. Review/harden P5 flush path. |
| `include/data_structures/gpuData.h` | Add `bool hasGPU = false` field. |
| `docs/examples/editing_p1/main.cpp` | Add round-trip tests. |
| `AGENTS.md` | Update Phase 8 status. |

## Definition of done

- `writeComposeToDisk` writes a compose folder containing:
  - `compose.json` with the full component tree (Asset + Data components,
    hierarchy preserved, names preserved, local transforms preserved).
  - One `.data` file per unique blob, with the correct current geometry.
- Locked components are NOT written to disk.
- Direct components overwrite their original `.data` files.
- Copy components write new `.data` files (`_fork1`, `_fork2`, etc.) and
  the compose.json references the new files.
- Load → edit → save → reload produces a scene with the same chunk count,
  component count, component tree structure, and geometry content.
- All P1-P7 assertions still pass.