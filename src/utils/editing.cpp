#include "utils/editing.h"

#include "core/log.h"
#include "utils/voxel_math.h"
#include "utils/voxel_management.h"

#include <unordered_map>
#include <unordered_set>
#include <glm/gtc/quaternion.hpp>

namespace projv::utils {
    namespace {
        // Lazily allocate Scene.dataReferences[dataRefID] for a component, deduped by
        // (sourcePath, resolution, voxelScale). For a Grid component the resolution and voxelScale
        // are read from the first populated cell of the grid.
        int32_t ensureDataReference(Scene& scene, ComponentRecord& comp) {
            if (comp.dataRefID >= 0) return comp.dataRefID;
            DataReference ref;
            ref.sourceDataPath = comp.sourcePath;
            if (comp.kind == ComponentKind::Chunk) {
                const Chunk& chunk = scene.chunks[comp.chunkHandle];
                ref.resolution     = chunk.header.resolution;
                ref.voxelScale     = chunk.header.voxelScale;
            } else {
                SceneGrid& grid = scene.grids[comp.gridIndex];
                ref.resolution = 256;
                ref.voxelScale = 0.01f;
                for (int32_t ci : grid.cellToChunk) {
                    if (ci >= 0) {
                        const Chunk& chunk = scene.chunks[ci];
                        ref.resolution = chunk.header.resolution;
                        ref.voxelScale = chunk.header.voxelScale;
                        break;
                    }
                }
            }
            for (int32_t i = 0; i < static_cast<int32_t>(scene.dataReferences.size()); ++i) {
                const DataReference& r = scene.dataReferences[i];
                if (r.sourceDataPath == ref.sourceDataPath &&
                    r.resolution == ref.resolution &&
                    r.voxelScale == ref.voxelScale) {
                    comp.dataRefID = i;
                    return i;
                }
            }
            comp.dataRefID = static_cast<int32_t>(scene.dataReferences.size());
            scene.dataReferences.push_back(std::move(ref));
            return comp.dataRefID;
        }

        // Apply edits to a single existing chunk (COW fork + merge + rebuild).
        void applyEditsToChunk(Scene& scene, Chunk& chunk,
                               const std::vector<Voxel>& adds,
                               const std::vector<Voxel>& removes) {
            if (adds.empty() && removes.empty()) return;

            int32_t oldIdx = chunk.geometryPoolIndex;
            int32_t newIdx = forkBlob(scene, oldIdx);
            chunk.geometryPoolIndex = newIdx;

            VoxelBatch current = getChunkVoxelBatch(scene, chunk, true);

            if (!removes.empty()) {
                VoxelBatch rmBatch;
                rmBatch.reserve(removes.size());
                for (auto& v : removes) rmBatch.push_back(v);
                removeVoxelBatchAFromVoxelBatchB(rmBatch, current, {0, 0, 0});
            }
            if (!adds.empty()) {
                VoxelBatch addBatch;
                addBatch.reserve(adds.size());
                for (auto& v : adds) addBatch.push_back(v);
                addVoxelBatchAToVoxelBatchB(addBatch, current, {0, 0, 0});
            }

            moveVoxelBatchToChunk(current, chunk);
            updateChunkFromItsVoxelBatch(chunk, /*clearBatch=*/true);

            GeometryBlob& fork = scene.geometryPool[chunk.geometryPoolIndex];
            fork.geometry      = chunk.geometryData;
            fork.voxelTypeData = chunk.voxelTypeData;
        }

        // Drain one component's queue. Handles both Chunk (loose) and Grid components.
        bool applyComponentQueue(Scene& scene, ComponentHandle h) {
            ComponentRecord& comp = scene.components[h];
            if (comp.editQueue.ops.empty()) return true;

            ensureDataReference(scene, comp);
            uint32_t res = scene.dataReferences[comp.dataRefID].resolution;

            if (comp.kind == ComponentKind::Chunk) {
                Chunk& chunk = scene.chunks[comp.chunkHandle];

                // COW fork.
                int32_t oldIdx = chunk.geometryPoolIndex;
                int32_t newIdx = forkBlob(scene, oldIdx);
                chunk.geometryPoolIndex = newIdx;

                VoxelBatch current = getChunkVoxelBatch(scene, chunk, true);

                VoxelBatch adds, removes;
                adds.reserve(comp.editQueue.ops.size());
                removes.reserve(comp.editQueue.ops.size());
                for (const PendingVoxelOp& op : comp.editQueue.ops) {
                    core::ivec3 local{
                        floorMod(op.position.x, static_cast<int32_t>(res)),
                        floorMod(op.position.y, static_cast<int32_t>(res)),
                        floorMod(op.position.z, static_cast<int32_t>(res))
                    };
                    if (local.x < 0 || local.y < 0 || local.z < 0 ||
                        local.x >= static_cast<int32_t>(res) ||
                        local.y >= static_cast<int32_t>(res) ||
                        local.z >= static_cast<int32_t>(res)) {
                        continue;
                    }
                    Voxel v = createVoxel(op.color, local);
                    (op.isAdd ? adds : removes).push_back(v);
                }

                if (!removes.empty()) removeVoxelBatchAFromVoxelBatchB(removes, current);
                if (!adds.empty())    addVoxelBatchAToVoxelBatchB(adds, current, {0, 0, 0});

                moveVoxelBatchToChunk(current, chunk);
                updateChunkFromItsVoxelBatch(chunk, /*clearBatch=*/true);

                GeometryBlob& fork = scene.geometryPool[chunk.geometryPoolIndex];
                fork.geometry      = chunk.geometryData;
                fork.voxelTypeData = chunk.voxelTypeData;

                comp.editQueue.ops.clear();
                return true;
            }

            // --- Grid component path ---
            SceneGrid& grid = scene.grids[comp.gridIndex];

            // 1. Compute overall cell bounds across all ops.
            core::ivec3 overallNewMin(0);
            core::ivec3 overallNewMax(grid.dims.x - 1, grid.dims.y - 1, grid.dims.z - 1);
            for (const auto& op : comp.editQueue.ops) {
                core::ivec3 cellCoord(
                    floorDiv(op.position.x, static_cast<int32_t>(res)),
                    floorDiv(op.position.y, static_cast<int32_t>(res)),
                    floorDiv(op.position.z, static_cast<int32_t>(res))
                );
                overallNewMin = core::ivec3(
                    std::min(overallNewMin.x, cellCoord.x),
                    std::min(overallNewMin.y, cellCoord.y),
                    std::min(overallNewMin.z, cellCoord.z)
                );
                overallNewMax = core::ivec3(
                    std::max(overallNewMax.x, cellCoord.x),
                    std::max(overallNewMax.y, cellCoord.y),
                    std::max(overallNewMax.z, cellCoord.z)
                );
            }

            // 2. Expand grid to include both extremes.
            expandGridToInclude(grid, overallNewMax, scene, comp.gridIndex);
            expandGridToInclude(grid, overallNewMin, scene, comp.gridIndex);

            // 3. The cell coordinate at grid.origin after expansion.
            core::ivec3 originCell = grid.originCellCoord;

            // 4. Bucket by cell.
            std::unordered_map<int, std::vector<Voxel>> perCellAdds;
            std::unordered_map<int, std::vector<Voxel>> perCellRemoves;
            for (const auto& op : comp.editQueue.ops) {
                core::ivec3 cellCoord(
                    floorDiv(op.position.x, static_cast<int32_t>(res)),
                    floorDiv(op.position.y, static_cast<int32_t>(res)),
                    floorDiv(op.position.z, static_cast<int32_t>(res))
                );
                core::ivec3 localPos(
                    floorMod(op.position.x, static_cast<int32_t>(res)),
                    floorMod(op.position.y, static_cast<int32_t>(res)),
                    floorMod(op.position.z, static_cast<int32_t>(res))
                );
                int lin = (cellCoord.x - originCell.x)
                          + grid.dims.x * ((cellCoord.y - originCell.y)
                                           + grid.dims.y * (cellCoord.z - originCell.z));
                Voxel v = createVoxel(op.color, localPos);
                (op.isAdd ? perCellAdds[lin] : perCellRemoves[lin]).push_back(v);
            }

            // 5. Collect all unique cell indices.
            std::unordered_set<int> allCells;
            for (const auto& pair : perCellAdds)    allCells.insert(pair.first);
            for (const auto& pair : perCellRemoves) allCells.insert(pair.first);

            // 6. Apply per cell.
            for (int lin : allCells) {
                int32_t chunkIdx = grid.cellToChunk[lin];
                const auto& adds    = perCellAdds[lin];
                const auto& removes = perCellRemoves[lin];

                if (chunkIdx < 0) {
                    // New cell: create an empty chunk.
                    if (!removes.empty()) {
                        core::warn("editing: remove on empty cell {} -- skipped", lin);
                    }
                    if (adds.empty()) continue;

                    int iz = lin / (grid.dims.x * grid.dims.y);
                    int iy = (lin / grid.dims.x) % grid.dims.y;
                    int ix = lin % grid.dims.x;

                    core::vec3 chunkPos = grid.origin
                        + glm::mat3_cast(grid.rotation) * (core::vec3(ix, iy, iz) * grid.cellSize);

                    ChunkHeader newHdr;
                    newHdr.chunkID    = 0;
                    newHdr.position   = chunkPos;
                    newHdr.scale      = grid.cellSize;
                    newHdr.voxelScale = scene.dataReferences[comp.dataRefID].voxelScale;
                    newHdr.resolution = res;
                    newHdr.rotation   = grid.rotation;

                    Chunk newChunk = createChunk(newHdr);
                    newChunk.gridIndex       = comp.gridIndex;
                    newChunk.cellIndex       = lin;
                    newChunk.componentHandle = h;

                    VoxelBatch batch;
                    batch.reserve(adds.size());
                    for (auto& v : adds) batch.push_back(v);
                    moveVoxelBatchToChunk(batch, newChunk);
                    updateChunkFromItsVoxelBatch(newChunk, /*clearBatch=*/true);
                    internChunkGeometry(scene, newChunk);

                    ChunkHandle newHandle = static_cast<ChunkHandle>(scene.chunks.size());
                    scene.chunks.push_back(std::move(newChunk));
                    grid.cellToChunk[lin] = static_cast<int32_t>(newHandle);
                } else {
                    // Existing cell: COW fork + apply edits.
                    Chunk& existingChunk = scene.chunks[chunkIdx];
                    applyEditsToChunk(scene, existingChunk, adds, removes);
                }
            }

            comp.editQueue.ops.clear();
            return true;
        }
    } // anon

    void expandGridToInclude(SceneGrid& grid, core::ivec3 cellCoord,
                             Scene& scene, int gridIndex) {
        (void)gridIndex;
        core::ivec3 newMin(
            std::min(0, cellCoord.x),
            std::min(0, cellCoord.y),
            std::min(0, cellCoord.z)
        );
        core::ivec3 newMax(
            std::max(static_cast<int32_t>(grid.dims.x) - 1, cellCoord.x),
            std::max(static_cast<int32_t>(grid.dims.y) - 1, cellCoord.y),
            std::max(static_cast<int32_t>(grid.dims.z) - 1, cellCoord.z)
        );
        core::ivec3 newDims = newMax - newMin + core::ivec3(1);
        if (newDims == grid.dims && newMin == core::ivec3(0)) return;

        std::vector<int32_t> newMap(newDims.x * newDims.y * newDims.z, -1);
        for (int z = 0; z < grid.dims.z; z++) {
            for (int y = 0; y < grid.dims.y; y++) {
                for (int x = 0; x < grid.dims.x; x++) {
                    int oldLin = x + grid.dims.x * (y + grid.dims.y * z);
                    int newLin = (x - newMin.x) + newDims.x * ((y - newMin.y) + newDims.y * (z - newMin.z));
                    newMap[newLin] = grid.cellToChunk[oldLin];
                    if (grid.cellToChunk[oldLin] >= 0)
                        scene.chunks[grid.cellToChunk[oldLin]].cellIndex = newLin;
                }
            }
        }

        grid.origin += glm::mat3_cast(grid.rotation) *
                       (core::vec3(newMin) * grid.cellSize);
        grid.originCellCoord = grid.originCellCoord + newMin;
        grid.dims = newDims;
        grid.cellToChunk = std::move(newMap);
    }

    bool queueVoxelAdd(Scene& scene, ComponentHandle h,
                       const std::vector<PendingVoxelOp>& voxels) {
        if (h >= scene.components.size()) return false;
        auto& q = scene.components[h].editQueue.ops;
        q.reserve(q.size() + voxels.size());
        for (auto v : voxels) { v.isAdd = true; q.push_back(v); }
        return true;
    }

    bool queueVoxelRemove(Scene& scene, ComponentHandle h,
                          const std::vector<PendingVoxelOp>& voxels) {
        if (h >= scene.components.size()) return false;
        auto& q = scene.components[h].editQueue.ops;
        q.reserve(q.size() + voxels.size());
        for (auto v : voxels) { v.isAdd = false; q.push_back(v); }
        return true;
    }

    uint32_t updateScene(Scene& scene) {
        uint32_t processed = 0;
        for (ComponentHandle h = 0; h < scene.components.size(); ++h) {
            if (scene.components[h].editQueue.ops.empty()) continue;
            if (applyComponentQueue(scene, h)) ++processed;
        }
        return processed;
    }
} // namespace projv::utils