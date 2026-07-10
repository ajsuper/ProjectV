#include "utils/streaming.h"

#include <string>
#include <algorithm>
#include <chrono>
#include <unordered_map>

#include <glm/gtc/quaternion.hpp>

#include "utils/compose_io.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"
#include "core/log.h"

namespace projv::utils {
    namespace {
        // Dedup key for a streamed grid block: canonical source path + block grid coords + mutability,
        // matching the eager loader's pool key so instanced/cross-grid blocks share one refcounted blob.
        std::string blockPoolKey(const std::string& path, int gx, int gy, int gz, Mutability m) {
            return path + "#" + std::to_string(gx) + "_" + std::to_string(gy) + "_" +
                   std::to_string(gz) + "#" + std::to_string(static_cast<int>(m));
        }

        // Linear cell index -> LOCAL grid coordinate, given grid dims. "Local" = this grid's own
        // cellToChunk indexing, which may have drifted from the on-disk block table's coordinate
        // system after a resize (see SceneGrid::cellCoordOffset).
        void cellToCoord(int cellIndex, const core::ivec3& dims, int& gx, int& gy, int& gz) {
            int plane = dims.x * dims.y;
            gz = cellIndex / plane;
            int rem = cellIndex % plane;
            gy = rem / dims.x;
            gx = rem % dims.x;
        }

        // Floor division/modulo (C++'s % and integer / truncate toward zero, not toward -infinity,
        // so they're wrong for splitting a possibly-negative voxel-space coordinate into
        // (cellCoord, localIndex) -- floorDiv/floorMod always land localIndex in [0, resolution)).
        int floorDiv(int a, int b) {
            int q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
            return q;
        }
        int floorMod(int a, int b) {
            int m = a % b;
            if (m != 0 && ((m < 0) != (b < 0))) m += b;
            return m;
        }

        core::ivec3 voxelToCellCoordLocal(core::ivec3 voxelPosition, uint32_t resolution) {
            int r = static_cast<int>(resolution);
            return core::ivec3(floorDiv(voxelPosition.x, r), floorDiv(voxelPosition.y, r), floorDiv(voxelPosition.z, r));
        }
        core::ivec3 voxelToLocalPositionLocal(core::ivec3 voxelPosition, uint32_t resolution) {
            int r = static_cast<int>(resolution);
            return core::ivec3(floorMod(voxelPosition.x, r), floorMod(voxelPosition.y, r), floorMod(voxelPosition.z, r));
        }

        // Shared by materializeGridCell / materializeEmptyGridCell: claims a chunk slot, synthesizes
        // the header from the grid descriptor, marks the cell resident, and enqueues an Add mutation.
        // Callers resolve/insert `poolIdx` (a blob already in scene.geometryPool) before calling.
        ChunkHandle synthesizeGridCellChunk(Scene& scene, StreamingContext& ctx, int gridIndex, int cellIndex,
                                             int32_t poolIdx, Mutability mutability,
                                             uint32_t resolution, float voxelScale) {
            SceneGrid& g = scene.grids[gridIndex];
            int lgx, lgy, lgz;
            cellToCoord(cellIndex, g.dims, lgx, lgy, lgz);

            // Claim a chunk slot (recycle a dead one so the handle == header row stays compact, else grow).
            ChunkHandle handle;
            if (!scene.chunkFreeList.empty()) {
                handle = scene.chunkFreeList.back();
                scene.chunkFreeList.pop_back();
                scene.chunks[handle] = Chunk{};
            } else {
                handle = static_cast<ChunkHandle>(scene.chunks.size());
                scene.chunks.push_back(Chunk{});
            }
            Chunk& c = scene.chunks[handle];
            c.alive = true;
            c.geometryPoolIndex = poolIdx;
            c.mutability = mutability;
            c.gridIndex = gridIndex;
            c.cellIndex = cellIndex;
            c.LOD = 0;

            // Synthesize the header from the grid descriptor (see loadComposeFromDisk's grid math):
            //   position = origin + rotation * (localCellCoord * cellSize);  scale = cellSize
            c.header.chunkID = handle;
            c.header.resolution = resolution;
            c.header.voxelScale = voxelScale;
            c.header.scale = g.cellSize;
            c.header.rotation = g.rotation;
            core::vec3 local = core::vec3(static_cast<float>(lgx), static_cast<float>(lgy),
                                          static_cast<float>(lgz)) * g.cellSize;
            c.header.position = g.origin + glm::mat3_cast(g.rotation) * local;

            // Mark the cell resident up front so a repeat materialize before the queue is applied is a no-op.
            g.cellToChunk[cellIndex] = static_cast<int32_t>(handle);

            ctx.mutations.push_back({graphics::SceneMutationKind::Add, handle});
            return handle;
        }

        // Shared merge step for addVoxelsToComponent/removeVoxelsFromComponent: reads a chunk's
        // ACTUAL existing content (pool-aware), splices in `localVoxels` (already resolved to this
        // chunk's own local coordinate space), and pushes exactly one Update mutation.
        void mergeVoxelsIntoChunk(Scene& scene, StreamingContext& ctx, ChunkHandle handle,
                                   VoxelBatch localVoxels, bool isAdd) {
            auto t0 = std::chrono::high_resolution_clock::now();
            if (handle >= scene.chunks.size() || !scene.chunks[handle].alive) {
                core::warn("mergeVoxelsIntoChunk: handle {} is not a live chunk", handle);
                return;
            }
            Chunk& chunk = scene.chunks[handle];
            if (chunk.geometryPoolIndex < 0) {
                core::warn("mergeVoxelsIntoChunk: chunk {} is not pooled; editing unsupported", handle);
                return;
            }
            VoxelBatch existing = getChunkVoxelBatch(scene, chunk, /*convertCompressedData=*/true);
            auto t1 = std::chrono::high_resolution_clock::now();
            removeVoxelBatchAFromVoxelBatchB(localVoxels, existing); // clear the target cells either way
            if (isAdd) {
                addVoxelBatchAToVoxelBatchB(localVoxels, existing);
            }
            auto t2 = std::chrono::high_resolution_clock::now();
            if (existing.empty()) {
                core::warn("mergeVoxelsIntoChunk: edit would empty chunk {} entirely; skipped", handle);
                return;
            }
            moveVoxelBatchToChunk(existing, chunk);
            applyChunkEdit(scene, ctx.mutations, handle);
            auto t3 = std::chrono::high_resolution_clock::now();
            double readMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            double mergeMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
            double bakeMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
            double totalMs = std::chrono::duration<double, std::milli>(t3 - t0).count();
            core::warn("[PERF] mergeVoxelsIntoChunk handle={} isAdd={}: read={:.2f}ms merge={:.2f}ms bake={:.2f}ms total={:.2f}ms",
                       handle, isAdd, readMs, mergeMs, bakeMs, totalMs);
        }
    }

    const DataFileHeader& streamGridBlockTable(StreamingContext& ctx, const Scene& scene, int gridIndex) {
        static const DataFileHeader empty{};
        if (gridIndex < 0 || gridIndex >= static_cast<int>(ctx.gridRecords.size())) {
            core::warn("streamGridBlockTable: grid index {} out of range", gridIndex);
            return empty;
        }
        (void)scene;
        const std::string& path = ctx.gridRecords[gridIndex].sourceDataPath;
        BlockTableCache& cache = ctx.blockTables;

        auto it = cache.tables.find(path);
        if (it == cache.tables.end()) {
            // Evict the least-recently-used table if we are at budget (never evict what we are adding).
            if (cache.budget > 0 && cache.tables.size() >= cache.budget) {
                std::string lruKey;
                uint64_t lruTick = UINT64_MAX;
                for (const auto& kv : cache.lastUsed) {
                    if (kv.second < lruTick) { lruTick = kv.second; lruKey = kv.first; }
                }
                if (!lruKey.empty()) { cache.tables.erase(lruKey); cache.lastUsed.erase(lruKey); }
            }
            it = cache.tables.emplace(path, readDataFileHeader(path)).first;
        }
        cache.lastUsed[path] = ++cache.tick;
        return it->second;
    }

    ChunkHandle materializeGridCell(Scene& scene, StreamingContext& ctx, int gridIndex, int cellIndex) {
        if (gridIndex < 0 || gridIndex >= static_cast<int>(scene.grids.size()) ||
            gridIndex >= static_cast<int>(ctx.gridRecords.size())) {
            core::warn("materializeGridCell: grid index {} out of range", gridIndex);
            return INVALID_CHUNK_HANDLE;
        }
        SceneGrid& g = scene.grids[gridIndex];
        if (cellIndex < 0 || cellIndex >= static_cast<int>(g.cellToChunk.size())) {
            core::warn("materializeGridCell: cell index {} out of range for grid {}", cellIndex, gridIndex);
            return INVALID_CHUNK_HANDLE;
        }
        // Already resident (or pending in the queue): return the existing handle, don't double-create.
        if (g.cellToChunk[cellIndex] >= 0) return static_cast<ChunkHandle>(g.cellToChunk[cellIndex]);

        const GridStreamRecord& rec = ctx.gridRecords[gridIndex];
        int lgx, lgy, lgz;
        cellToCoord(cellIndex, g.dims, lgx, lgy, lgz);
        // The on-disk block table is keyed by the FILE's own coordinates, which may have drifted
        // from this grid's local cellToChunk indexing after a resize (expandGridToInclude shifting
        // local index 0 away from the file's (0,0,0)) -- cellCoordOffset corrects for that drift.
        int gx = lgx + g.cellCoordOffset.x, gy = lgy + g.cellCoordOffset.y, gz = lgz + g.cellCoordOffset.z;

        // Find this cell's block in the file's table. A cell with no block is genuinely empty on disk.
        const DataFileHeader& hdr = streamGridBlockTable(ctx, scene, gridIndex);
        const BlockEntry* entry = nullptr;
        for (const BlockEntry& e : hdr.blocks) {
            if (e.gridX == gx && e.gridY == gy && e.gridZ == gz) { entry = &e; break; }
        }
        if (!entry) return INVALID_CHUNK_HANDLE; // empty cell

        // Resolve the geometry blob: share an already-resident one, else read + insert. The dedup entry
        // is validated against the blob's provenance so a recycled pool slot never aliases the wrong blob.
        std::string key = blockPoolKey(rec.sourceDataPath, gx, gy, gz, rec.mutability);
        int32_t poolIdx = -1;
        auto pit = ctx.poolKeyToIndex.find(key);
        if (pit != ctx.poolKeyToIndex.end() && pit->second < static_cast<int32_t>(scene.geometryPool.size())) {
            GeometryBlob& cand = scene.geometryPool[pit->second];
            if (cand.refCount > 0 && cand.sourceDataPath == rec.sourceDataPath &&
                cand.sourceBlockCoord == core::ivec3(gx, gy, gz)) {
                poolIdx = pit->second;
                cand.refCount++;
            }
        }
        if (poolIdx < 0) {
            DataBlock block = readDataBlock(rec.sourceDataPath, *entry);
            GeometryBlob blob;
            blob.geometry = std::move(block.geometry);
            blob.voxelTypeData = std::move(block.voxelTypeData);
            blob.sourceDataPath = rec.sourceDataPath;
            blob.sourceBlockCoord = core::ivec3(gx, gy, gz);
            blob.refCount = 1;
            poolIdx = poolInsertBlob(scene, std::move(blob));
            ctx.poolKeyToIndex[key] = poolIdx;
        }

        return synthesizeGridCellChunk(scene, ctx, gridIndex, cellIndex, poolIdx, rec.mutability,
                                        rec.resolution, rec.dataVoxelScale * rec.uniformScale);
    }

    ChunkHandle materializeEmptyGridCell(Scene& scene, StreamingContext& ctx, int gridIndex, int cellIndex) {
        if (gridIndex < 0 || gridIndex >= static_cast<int>(scene.grids.size()) ||
            gridIndex >= static_cast<int>(ctx.gridRecords.size())) {
            core::warn("materializeEmptyGridCell: grid index {} out of range", gridIndex);
            return INVALID_CHUNK_HANDLE;
        }
        SceneGrid& g = scene.grids[gridIndex];
        if (cellIndex < 0 || cellIndex >= static_cast<int>(g.cellToChunk.size())) {
            core::warn("materializeEmptyGridCell: cell index {} out of range for grid {}", cellIndex, gridIndex);
            return INVALID_CHUNK_HANDLE;
        }
        if (g.cellToChunk[cellIndex] >= 0) return static_cast<ChunkHandle>(g.cellToChunk[cellIndex]);

        const GridStreamRecord& rec = ctx.gridRecords[gridIndex];
        // Fresh, empty blob -- no source file yet (this cell was never authored on disk), so it isn't
        // entered into ctx.poolKeyToIndex (nothing on disk to dedup against). A second edit into the
        // same cell just reuses the chunk this call creates, via the normal residency check above.
        GeometryBlob blob;
        blob.sourceDataPath.clear();
        blob.ownsSourceFile = false;
        blob.refCount = 1;
        int32_t poolIdx = poolInsertBlob(scene, std::move(blob));

        return synthesizeGridCellChunk(scene, ctx, gridIndex, cellIndex, poolIdx, rec.mutability,
                                        rec.resolution, rec.dataVoxelScale * rec.uniformScale);
    }

    void expandGridToInclude(Scene& scene, SceneGrid& grid, core::ivec3 cellCoord) {
        core::ivec3 localCoord = cellCoord - grid.cellCoordOffset;
        bool inBounds = localCoord.x >= 0 && localCoord.x < grid.dims.x &&
                         localCoord.y >= 0 && localCoord.y < grid.dims.y &&
                         localCoord.z >= 0 && localCoord.z < grid.dims.z;
        if (inBounds) return;

        core::ivec3 newMin(std::min(0, localCoord.x), std::min(0, localCoord.y), std::min(0, localCoord.z));
        core::ivec3 newMax(std::max(grid.dims.x - 1, localCoord.x),
                            std::max(grid.dims.y - 1, localCoord.y),
                            std::max(grid.dims.z - 1, localCoord.z));
        core::ivec3 newDims = newMax - newMin + core::ivec3(1);

        std::vector<int32_t> newCellToChunk(
            static_cast<size_t>(newDims.x) * newDims.y * newDims.z, -1);
        for (int z = 0; z < grid.dims.z; z++) {
            for (int y = 0; y < grid.dims.y; y++) {
                for (int x = 0; x < grid.dims.x; x++) {
                    int32_t v = grid.cellToChunk[x + grid.dims.x * (y + grid.dims.y * z)];
                    if (v < 0) continue;
                    int nx = x - newMin.x, ny = y - newMin.y, nz = z - newMin.z;
                    int newLinear = nx + newDims.x * (ny + newDims.y * nz);
                    newCellToChunk[newLinear] = v;
                    scene.chunks[v].cellIndex = newLinear;
                }
            }
        }

        // Local index 0 now refers to what used to be local index newMin -- correct origin (so
        // already-resident cells keep their exact world position) and cellCoordOffset (so
        // materializeGridCell's on-disk block lookup keeps finding the right file coordinate).
        grid.origin += glm::mat3_cast(grid.rotation) * (core::vec3(newMin) * grid.cellSize);
        grid.cellCoordOffset += newMin;
        grid.dims = newDims;
        grid.cellToChunk = std::move(newCellToChunk);
    }

    void releaseGridCell(Scene& scene, StreamingContext& ctx, int gridIndex, int cellIndex) {
        if (gridIndex < 0 || gridIndex >= static_cast<int>(scene.grids.size())) return;
        SceneGrid& g = scene.grids[gridIndex];
        if (cellIndex < 0 || cellIndex >= static_cast<int>(g.cellToChunk.size())) return;
        int32_t h = g.cellToChunk[cellIndex];
        if (h < 0) return; // not resident
        // removeChunkFromGPU (on apply) does the CPU + GPU teardown: decrement refcount, free the range
        // at zero, recycle the chunk + pool slots, and null this cell. Nothing to do here but enqueue.
        ctx.mutations.push_back({graphics::SceneMutationKind::Remove, static_cast<ChunkHandle>(h)});
    }

    void applyChunkEdit(Scene& scene, std::vector<graphics::PendingSceneMutation>& queue, ChunkHandle handle) {
        if (handle >= scene.chunks.size() || !scene.chunks[handle].alive) {
            core::warn("applyChunkEdit: handle {} not a live chunk", handle);
            return;
        }
        editChunkVoxels(scene, scene.chunks[handle]); // bakes chunkQueue into the pool blob (honors COW)
        queue.push_back({graphics::SceneMutationKind::Update, handle});
    }

    void addVoxelsToComponent(Scene& scene, StreamingContext& ctx, ComponentHandle component,
                               const EditVoxelBatch& voxels) {
        auto t0 = std::chrono::high_resolution_clock::now();
        if (component >= scene.components.size()) {
            core::warn("addVoxelsToComponent: component {} out of range", component);
            return;
        }
        const ComponentRecord& rec = scene.components[component];
        if (rec.kind == ComponentKind::Chunk) {
            VoxelBatch localVoxels;
            localVoxels.reserve(voxels.size());
            for (const EditVoxel& v : voxels) localVoxels.push_back(createVoxel(v.color, v.position));
            mergeVoxelsIntoChunk(scene, ctx, rec.chunkHandle, std::move(localVoxels), /*isAdd=*/true);
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            core::warn("[PERF] addVoxelsToComponent component={} (Chunk) voxels={} total={:.2f}ms", component, voxels.size(), ms);
            return;
        }

        int gridIndex = rec.gridIndex;
        SceneGrid& grid = scene.grids[gridIndex];
        uint32_t resolution = ctx.gridRecords[gridIndex].resolution;
        if (resolution == 0) {
            core::warn("addVoxelsToComponent: grid component {} has no resolution on record", component);
            return;
        }

        // Grow the grid first (as needed) so bucketing below uses the final dims/cellCoordOffset.
        if (grid.resizeToFitVoxels) {
            for (const EditVoxel& v : voxels) {
                expandGridToInclude(scene, grid, voxelToCellCoordLocal(v.position, resolution));
            }
        }

        // Bucket by cell.
        std::unordered_map<int, VoxelBatch> perCellAdds;
        for (const EditVoxel& v : voxels) {
            core::ivec3 cellCoord = voxelToCellCoordLocal(v.position, resolution);
            core::ivec3 localCoord = cellCoord - grid.cellCoordOffset;
            if (localCoord.x < 0 || localCoord.x >= grid.dims.x ||
                localCoord.y < 0 || localCoord.y >= grid.dims.y ||
                localCoord.z < 0 || localCoord.z >= grid.dims.z) {
                core::warn("addVoxelsToComponent: voxel outside grid bounds and resizeToFitVoxels is "
                           "false; skipped");
                continue;
            }
            int linear = localCoord.x + grid.dims.x * (localCoord.y + grid.dims.y * localCoord.z);
            core::ivec3 local = voxelToLocalPositionLocal(v.position, resolution);
            perCellAdds[linear].push_back(createVoxel(v.color, local));
        }

        for (auto& [linear, batch] : perCellAdds) {
            ChunkHandle handle = grid.cellToChunk[linear] >= 0
                ? static_cast<ChunkHandle>(grid.cellToChunk[linear])
                : materializeGridCell(scene, ctx, gridIndex, linear);
            if (handle == INVALID_CHUNK_HANDLE) {
                handle = materializeEmptyGridCell(scene, ctx, gridIndex, linear);
            }
            if (handle == INVALID_CHUNK_HANDLE) {
                core::warn("addVoxelsToComponent: could not materialize cell {} of grid {}", linear, gridIndex);
                continue;
            }
            mergeVoxelsIntoChunk(scene, ctx, handle, std::move(batch), /*isAdd=*/true);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        core::warn("[PERF] addVoxelsToComponent component={} (Grid) voxels={} cells={} total={:.2f}ms",
                   component, voxels.size(), perCellAdds.size(), ms);
    }

    void removeVoxelsFromComponent(Scene& scene, StreamingContext& ctx, ComponentHandle component,
                                    const EditVoxelBatch& voxels) {
        auto t0 = std::chrono::high_resolution_clock::now();
        if (component >= scene.components.size()) {
            core::warn("removeVoxelsFromComponent: component {} out of range", component);
            return;
        }
        const ComponentRecord& rec = scene.components[component];
        if (rec.kind == ComponentKind::Chunk) {
            VoxelBatch localVoxels;
            localVoxels.reserve(voxels.size());
            for (const EditVoxel& v : voxels) localVoxels.push_back(createVoxel(v.color, v.position));
            mergeVoxelsIntoChunk(scene, ctx, rec.chunkHandle, std::move(localVoxels), /*isAdd=*/false);
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            core::warn("[PERF] removeVoxelsFromComponent component={} (Chunk) voxels={} total={:.2f}ms", component, voxels.size(), ms);
            return;
        }

        int gridIndex = rec.gridIndex;
        SceneGrid& grid = scene.grids[gridIndex];
        uint32_t resolution = ctx.gridRecords[gridIndex].resolution;
        if (resolution == 0) {
            core::warn("removeVoxelsFromComponent: grid component {} has no resolution on record", component);
            return;
        }

        std::unordered_map<int, VoxelBatch> perCellRemoves;
        for (const EditVoxel& v : voxels) {
            core::ivec3 cellCoord = voxelToCellCoordLocal(v.position, resolution);
            core::ivec3 localCoord = cellCoord - grid.cellCoordOffset;
            if (localCoord.x < 0 || localCoord.x >= grid.dims.x ||
                localCoord.y < 0 || localCoord.y >= grid.dims.y ||
                localCoord.z < 0 || localCoord.z >= grid.dims.z) {
                continue;
            }
            int linear = localCoord.x + grid.dims.x * (localCoord.y + grid.dims.y * localCoord.z);
            if (grid.cellToChunk[linear] < 0) continue;
            core::ivec3 local = voxelToLocalPositionLocal(v.position, resolution);
            perCellRemoves[linear].push_back(createVoxel(v.color, local));
        }

        for (auto& [linear, batch] : perCellRemoves) {
            ChunkHandle handle = static_cast<ChunkHandle>(grid.cellToChunk[linear]);
            mergeVoxelsIntoChunk(scene, ctx, handle, std::move(batch), /*isAdd=*/false);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        core::warn("[PERF] removeVoxelsFromComponent component={} (Grid) voxels={} cells={} total={:.2f}ms",
                   component, voxels.size(), perCellRemoves.size(), ms);
    }
}
