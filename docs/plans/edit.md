New Editing System — Final Plan (with negative expansion)
Data Structure Changes
Removed:
- Mutability from Chunk and DataReference — always copy-on-edit
- copyDiverged from Chunk, ownsSourceFile from GeometryBlob
- resizeToFitVoxels, cellCoordOffset from SceneGrid
- StreamingContext, GridStreamRecord, BlockTableCache (already deleted)
Added/Modified:
// In Scene:
struct DataReference {
    std::string sourceDataPath;
    uint32_t    resolution;
    float       voxelScale;
};

// Per-component edit queue:
struct PendingVoxelOp {
    bool            isAdd;
    core::ivec3     position;      // component-space, continuous integer coords (NOT Z-order)
    Color           color;
};

struct ComponentRecord {
    ComponentKind kind;
    int32_t       dataRefID;
    ChunkHandle   chunkHandle;
    int32_t       gridIndex;
    std::string   sourcePath;
    
    std::vector<PendingVoxelOp> editQueue;
};

// Chunk — simplified:
struct Chunk {
    ChunkHeader header;
    std::vector<uint32_t> geometryData;
    std::vector<uint32_t> voxelTypeData;
    VoxelBatch chunkQueue;
    uint32_t LOD;
    int32_t  geometryPoolIndex = -1;
    int32_t  gridIndex = -1;
    int32_t  cellIndex = -1;
    ComponentHandle componentHandle = INVALID_COMPONENT_HANDLE;
};

// SceneGrid — simplified:
struct SceneGrid {
    core::vec3 origin;
    float      cellSize;
    core::ivec3 dims;
    core::quat rotation;
    std::vector<int32_t> cellToChunk;
    ComponentHandle componentHandle = INVALID_COMPONENT_HANDLE;
};

// GPUChunkHeader — dataRefID replaces old component mechanism:
struct GPUChunkHeader {
    ...existing fields...
    uint32_t dataRefID;
    uint32_t padding[1];
    ...rotation fields...
};
Public API
void queueVoxelAdd(Scene& scene, ComponentHandle component,
                   const std::vector<PendingVoxelOp>& voxels,
                   bool resizeToFitVoxels = true);

void queueVoxelRemove(Scene& scene, ComponentHandle component,
                      const std::vector<PendingVoxelOp>& voxels,
                      bool resizeToFitVoxels = true);

// Resolve all queues across all components. CPU-only. Clears all queues.
void updateScene(Scene& scene);

// Full GPU rebuild. Reuses existing bgfx textures (destroy+recreate).
// TODO: incremental path — track changed pool blobs, only re-upload those.
void rebuildSceneTextures(Scene& scene, GPUData& gpuData);
Grid Expansion (Both Directions)
void expandGridToInclude(SceneGrid& grid, core::ivec3 cellCoord,
                         Scene& scene, int gridIndex) {
    core::ivec3 newMin(
        std::min(0, cellCoord.x),
        std::min(0, cellCoord.y),
        std::min(0, cellCoord.z)
    );
    core::ivec3 newMax(
        std::max(grid.dims.x - 1, cellCoord.x),
        std::max(grid.dims.y - 1, cellCoord.y),
        std::max(grid.dims.z - 1, cellCoord.z)
    );
    core::ivec3 newDims = newMax - newMin + core::ivec3(1);
    if (newDims == grid.dims && newMin == core::ivec3(0)) return;

    std::vector<int32_t> newMap(newDims.x * newDims.y * newDims.z, -1);
    for (int z = 0; z < grid.dims.z; z++)
        for (int y = 0; y < grid.dims.y; y++)
            for (int x = 0; x < grid.dims.x; x++) {
                int oldLin = x + grid.dims.x * (y + grid.dims.y * z);
                int newLin = (x - newMin.x) + newDims.x * ((y - newMin.y) + newDims.y * (z - newMin.z));
                newMap[newLin] = grid.cellToChunk[oldLin];
                if (grid.cellToChunk[oldLin] >= 0)
                    scene.chunks[grid.cellToChunk[oldLin]].cellIndex = newLin;
            }

    // Shift origin so existing chunks stay at the same world position
    grid.origin += glm::mat3_cast(grid.rotation) *
                   (core::vec3(newMin) * grid.cellSize);
    grid.dims = newDims;
    grid.cellToChunk = std::move(newMap);
}
Note: cellCoord here is the cell coordinate in the grid's current indexing space (where cell (0,0,0) is at grid.origin). This means expandGridToInclude handles both directions — cells at negative coordinates shift the origin so existing chunks don't move.
Loose Chunk → 1-Cell Grid Conversion
void convertChunkToGrid(Scene& scene, ComponentHandle compHandle) {
    ComponentRecord& comp = scene.components[compHandle];
    Chunk& chunk = scene.chunks[comp.chunkHandle];
    
    SceneGrid g;
    g.origin = chunk.header.position;
    g.cellSize = chunk.header.scale;
    g.rotation = chunk.header.rotation;
    g.dims = core::ivec3(1);
    g.cellToChunk = {static_cast<int32_t>(comp.chunkHandle)};
    
    int32_t gridIdx = static_cast<int32_t>(scene.grids.size());
    chunk.gridIndex = gridIdx;
    chunk.cellIndex = 0;
    g.componentHandle = compHandle;
    scene.grids.push_back(std::move(g));
    
    comp.kind = ComponentKind::Grid;
    comp.gridIndex = gridIdx;
}
updateScene Resolution Logic
for each component with non-empty editQueue:

    // Phase A: determine component kind, ensure it's a Grid
    if component.kind == Chunk:
        for each edit voxel:
            if any axis of voxel.position >= chunk.header.resolution:
                convertChunkToGrid(scene, compHandle)
                break   // re-process as grid below
    
    if component.kind == Grid:
        SceneGrid& grid = scene.grids[component.gridIndex];
        uint32_t res = scene.dataReferences[comp.dataRefID].resolution;
        
        // Grow grid if needed
        if (resizeToFitVoxels) {
            for each edit voxel:
                core::ivec3 cellCoord(
                    floorDiv(voxel.position.x, res),
                    floorDiv(voxel.position.y, res),
                    floorDiv(voxel.position.z, res)
                );
                expandGridToInclude(grid, cellCoord, scene, component.gridIndex);
        }
        
        // Bucket by cell
        std::unordered_map<int, VoxelBatch> perCell;
        for each edit voxel:
            core::ivec3 cellCoord = floorDiv(voxel.position, res);
            core::ivec3 localPos = floorMod(voxel.position, res);
            int lin = cellToLinear(cellCoord - gridOriginInCellCoords, grid.dims);
            if (lin out of bounds && !resizeToFitVoxels) skip;
            perCell[lin].push_back(createVoxel(voxel.color, localPos));
        
        // Apply per cell
        for each (linear, batch):
            if cell is empty (cellToChunk == -1):
                create empty chunk at that cell
                moveVoxelBatchToChunk(batch, newChunk)
                updateChunkFromItsVoxelBatch(newChunk)
                // Always COW: create a new pool blob
                internChunkGeometry(scene, newChunk)
                register in cellToChunk
            else:
                Chunk& existing = scene.chunks[cellToChunk[linear]];
                // COW: fork the pool blob
                forkBlob(scene, existing);
                // Read, merge, rebuild
                VoxelBatch current = getChunkVoxelBatch(scene, existing, true);
                for batch voxels:
                    if isAdd: addVoxelBatchAToVoxelBatchB(batch, current)
                    else: removeVoxelBatchAFromVoxelBatchB(batch, current)
                moveVoxelBatchToChunk(current, existing)
                updateChunkFromItsVoxelBatch(existing)
                // Write back to forked pool blob
                geometryPool[existing.geometryPoolIndex].geometry = existing.geometryData;
                geometryPool[existing.geometryPoolIndex].voxelTypeData = existing.voxelTypeData;
rebuildSceneTextures
Identical to existing createTexturesForScene — calls buildDataAndHeaderTextures + syncSceneTables. The comment at the top notes:
// TODO (Phase 5): incremental upload.
// Instead of rebuilding all textures from scratch, track which pool blobs changed
// (dirty flag on GeometryBlob) and only re-upload those ranges + rewrite their
// header rows. This requires keeping the RangeAllocator + GPUBlobRange state
// alive across calls (currently they're embedded in GPUData which gets rebuilt
// every frame by createTexturesForScene).
Implementation Phases
Phase	Files to Create/Modify	What
P1	include/utils/editing.h + src/utils/editing.cpp (NEW)	queueVoxelAdd, queueVoxelRemove, updateScene for loose chunks only. floorDiv/floorMod helpers. No grid expansion, no chunk→grid conversion
P2	Extend editing.cpp	Grid expansion (both directions) + cell bucketing + boundary rollover
P3	Extend editing.cpp	Loose chunk → 1-cell grid conversion on overflow
P4	include/graphics/gpu_interface.h + src/graphics/gpu_interface.cpp	rebuildSceneTextures from existing createTexturesForScene logic. Add dataRefID to GPUChunkHeader
P5	Sporadic	Incremental GPU upload (dirty tracking + range update)
Open Questions for You
1. floorDiv/floorMod vs C++ %: The cell bucketing needs floor-style division for negative coordinates to work correctly (e.g. coordinate -5 in a 256-res grid should land in cell -1, local 249). Already handled in the old streaming.cpp. We'll move those helpers to core/math.h or utils/voxel_math.h.
2. Grid cell coordinate system after expansion: After negative expansion, cell (0,0,0) in cellToChunk no longer corresponds to grid.origin. The expanded version above already accounts for this by shifting grid.origin — but we no longer need cellCoordOffset since the expansion function itself tracks the offset internally by adjusting the origin. Does this make sense to you?
3. Fork-on-write: When do we free the original blob's refcount? If two components share the same .data, the first edit on either component should fork a new blob but NOT decrement the original's refcount (the other component still needs it). forkBlob should:
- Create a new GeometryBlob copy of the current one
- Set the new blob's refCount = 1 (the editing component)
- NOT touch the original blob's refcount (the non-editing component still references it)
- Point the editing chunk at the new blob
- Is this the model you had in mind?
