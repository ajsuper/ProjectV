#include "utils/editing.h"

#include "core/log.h"
#include "utils/voxel_math.h"
#include "utils/voxel_management.h"

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
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
            core::edit(" applyEditsToChunk: chunkHandle={} oldPool={} newPool={} adds={} removes={}",
                       chunk.cellIndex, oldIdx, newIdx, adds.size(), removes.size());

            VoxelBatch current = getChunkVoxelBatch(scene, chunk, true);
            size_t preVoxelCount = current.size();
            core::edit(" applyEditsToChunk: pre-edit voxels={}", preVoxelCount);

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

            core::edit(" applyEditsToChunk: post-edit voxels={}", current.size());

            moveVoxelBatchToChunk(current, chunk);
            updateChunkFromItsVoxelBatch(chunk, /*clearBatch=*/true);

            core::edit(" applyEditsToChunk: done finalRes={} scale={} geomNodes={} voxelTypes={}",
                       chunk.header.resolution, chunk.header.scale,
                       chunk.geometryData.size() / 3,
                       chunk.voxelTypeData.size());

            GeometryBlob& fork = scene.geometryPool[chunk.geometryPoolIndex];
            fork.geometry      = chunk.geometryData;
            fork.voxelTypeData = chunk.voxelTypeData;

            core::edit(" applyEditsToChunk: done res={} geomNodes={} voxelType={}",
                       chunk.header.resolution,
                       chunk.geometryData.size() / 3,
                       chunk.voxelTypeData.size());
        }

        // Drain one component's queue. Handles both Chunk (loose) and Grid components.
        bool applyComponentQueue(Scene& scene, ComponentHandle h) {
            ComponentRecord& comp = scene.components[h];
            if (comp.editQueue.ops.empty()) return true;

            // P6: Asset components should never have edit ops, but be defensive.
            if (comp.kind == ComponentKind::Asset) {
                comp.editQueue.ops.clear();
                return true;
            }

            ensureDataReference(scene, comp);
            uint32_t res = scene.dataReferences[comp.dataRefID].resolution;

            if (comp.kind == ComponentKind::Chunk) {
                Chunk& chunk = scene.chunks[comp.chunkHandle];

                // P3: Check if any op overflows the chunk's resolution. If so, convert to Grid.
                bool overflows = false;
                for (const auto& op : comp.editQueue.ops) {
                    if (op.position.x >= static_cast<int32_t>(res) ||
                        op.position.y >= static_cast<int32_t>(res) ||
                        op.position.z >= static_cast<int32_t>(res) ||
                        op.position.x < 0 || op.position.y < 0 || op.position.z < 0) {
                        overflows = true;
                        break;
                    }
                }

                if (overflows) {
                    core::edit(" Chunk overflow detected at chunkHandle={} res={} -- converting to Grid",
                               comp.chunkHandle, res);
                    convertChunkToGrid(scene, h);
                    core::edit(" Chunk converted: new gridIndex={} dims=({},{},{})",
                               scene.components[h].gridIndex,
                               scene.grids[scene.components[h].gridIndex].dims.x,
                               scene.grids[scene.components[h].gridIndex].dims.y,
                               scene.grids[scene.components[h].gridIndex].dims.z);
                    // Fall through to Grid path below with the same editQueue.
                } else {
                    core::edit(" Chunk path: processing {} ops on chunkHandle={} res={}",
                               comp.editQueue.ops.size(), comp.chunkHandle, res);
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
            core::edit(" Grid path: comp={} ops={} res={} cellBounds min=({},{},{}) max=({},{},{})",
                       h, comp.editQueue.ops.size(), res,
                       overallNewMin.x, overallNewMin.y, overallNewMin.z,
                       overallNewMax.x, overallNewMax.y, overallNewMax.z);

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

                core::edit(" Grid path: cell lin={} chunkIdx={} adds={} removes={} dims=({},{},{}) originCell=({},{},{})",
                           lin, chunkIdx, adds.size(), removes.size(),
                           grid.dims.x, grid.dims.y, grid.dims.z,
                           originCell.x, originCell.y, originCell.z);

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

                    core::edit(" Grid path: creating new chunk lin={} pos=({},{},{}) initRes={} voxelsInBatch={}",
                               lin, chunkPos.x, chunkPos.y, chunkPos.z,
                               newHdr.resolution, batch.size());

                    moveVoxelBatchToChunk(batch, newChunk);
                    updateChunkFromItsVoxelBatch(newChunk, /*clearBatch=*/true);

                    core::edit(" Grid path: after updateChunkFromItsVoxelBatch: finalRes={} scale={} geomNodes={} voxelTypes={}",
                               newChunk.header.resolution, newChunk.header.scale,
                               newChunk.geometryData.size() / 3,
                               newChunk.voxelTypeData.size());

                    internChunkGeometry(scene, newChunk);

                    ChunkHandle newHandle = static_cast<ChunkHandle>(scene.chunks.size());
                    scene.chunks.push_back(std::move(newChunk));
                    grid.cellToChunk[lin] = static_cast<int32_t>(newHandle);

                    core::edit(" Grid path: created new chunk handle={} at cell lin={} pos=({},{},{}) res={} voxels={}",
                               newHandle, lin, chunkPos.x, chunkPos.y, chunkPos.z,
                               newHdr.resolution, batch.size());
                } else {
                    // Existing cell: COW fork + apply edits.
                    Chunk& existingChunk = scene.chunks[chunkIdx];
                    core::edit(" Grid path: editing existing chunk handle={} cellLin={} pool={} oldRes={} forcing to {}",
                               chunkIdx, existingChunk.cellIndex,
                               existingChunk.geometryPoolIndex,
                               existingChunk.header.resolution, res);
                    existingChunk.header.resolution = res;
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
        core::ivec3 oldDims = grid.dims;
        core::ivec3 oldOriginCell = grid.originCellCoord;
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

        core::edit(" expandGridToInclude: cellCoord=({},{},{}) oldDims=({},{},{}) newDims=({},{},{}) newMin=({},{},{}) oldOriginCell=({},{},{})",
                   cellCoord.x, cellCoord.y, cellCoord.z,
                   oldDims.x, oldDims.y, oldDims.z,
                   newDims.x, newDims.y, newDims.z,
                   newMin.x, newMin.y, newMin.z,
                   oldOriginCell.x, oldOriginCell.y, oldOriginCell.z);

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

    void convertChunkToGrid(Scene& scene, ComponentHandle compHandle) {
        ComponentRecord& comp = scene.components[compHandle];
        Chunk& chunk = scene.chunks[comp.chunkHandle];

        core::trace("convertChunkToGrid: comp={} chunkHandle={} position=({},{},{})",
                    compHandle, comp.chunkHandle,
                    chunk.header.position.x, chunk.header.position.y, chunk.header.position.z);

        SceneGrid g;
        g.origin    = chunk.header.position;
        g.cellSize  = chunk.header.scale;
        g.rotation  = chunk.header.rotation;
        g.dims      = core::ivec3(1);
        g.cellToChunk = {static_cast<int32_t>(comp.chunkHandle)};

        int32_t gridIdx = static_cast<int32_t>(scene.grids.size());
        chunk.gridIndex = gridIdx;
        chunk.cellIndex = 0;
        g.componentHandle = compHandle;
        scene.grids.push_back(std::move(g));

        // Remove from loose chunk tracking.
        auto it = std::find(scene.looseChunks.begin(), scene.looseChunks.end(), comp.chunkHandle);
        if (it != scene.looseChunks.end()) {
            scene.looseChunks.erase(it);
            scene.looseChunkCount = static_cast<uint32_t>(scene.looseChunks.size());
        }

        comp.kind = ComponentKind::Grid;
        comp.gridIndex = gridIdx;

        core::trace("convertChunkToGrid: done gridIndex={} looseCount={}",
                    gridIdx, scene.looseChunkCount);
        // chunkHandle stays valid — points at the chunk now resident in grid cell 0.
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
            core::edit("updateScene: draining queue for component[{}] name=\"{}\" ops={}",
                       h, scene.components[h].name,
                       scene.components[h].editQueue.ops.size());
            if (applyComponentQueue(scene, h)) ++processed;
        }
        if (processed > 0)
            core::edit("updateScene: processed {} component edit queues", processed);
        return processed;
    }
} // namespace projv::utils