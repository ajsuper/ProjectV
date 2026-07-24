#include "utils/editing.h"

#include "core/log.h"
#include "utils/voxel_math.h"
#include "utils/voxel_management.h"
#include "utils/material.h"

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

        // Ensure the blob has a brick map. If null, build one from the existing voxelTypeData.
        void ensureBrickMapExists(GeometryBlob& blob, uint32_t resolution) {
            if (blob.brickMap) return;
            core::ivec3 brickDims = computeBrickDims(resolution);
            blob.brickMap = createVoxelBrickMap(brickDims);
            if (!blob.voxelTypeData.empty()) {
                brickMapFromVoxelTypeData(*blob.brickMap, blob.voxelTypeData, blob);
            }
        }

        // Apply a batch of PendingVoxelOps directly to a brick map.
        // Positions are in continuous component-space coords; the brick map stores
        // them directly (no cell bucketing — that's for the Grid path).
        void applyOpsToBrickMap(VoxelBrickMap& map, uint32_t resolution,
                                const std::vector<PendingVoxelOp>& ops) {
            for (const PendingVoxelOp& op : ops) {
                int32_t x = static_cast<int32_t>(floorMod(op.position.x, static_cast<int32_t>(resolution)));
                int32_t y = static_cast<int32_t>(floorMod(op.position.y, static_cast<int32_t>(resolution)));
                int32_t z = static_cast<int32_t>(floorMod(op.position.z, static_cast<int32_t>(resolution)));
                if (x < 0 || y < 0 || z < 0 ||
                    x >= static_cast<int32_t>(resolution) ||
                    y >= static_cast<int32_t>(resolution) ||
                    z >= static_cast<int32_t>(resolution)) {
                    continue;
                }
                if (op.isAdd) {
                    brickMapSetVoxel(map, x, y, z, op.materialID);
                } else {
                    brickMapClearVoxel(map, x, y, z);
                }
            }
        }

        // Apply edits to a single existing chunk using the brick map.
        void applyEditsToChunk(Scene& scene, Chunk& chunk,
                               const std::vector<PendingVoxelOp>& ops) {
            if (ops.empty()) return;

            int32_t oldIdx = chunk.geometryPoolIndex;
            int32_t newIdx = forkBlob(scene, oldIdx);
            chunk.geometryPoolIndex = newIdx;
            core::edit(" applyEditsToChunk: chunkHandle={} oldPool={} newPool={} ops={}",
                       chunk.cellIndex, oldIdx, newIdx, ops.size());

            GeometryBlob& blob = scene.geometryPool[chunk.geometryPoolIndex];
            uint32_t res = chunk.header.resolution;
            ensureBrickMapExists(blob, res);
            applyOpsToBrickMap(*blob.brickMap, res, ops);

            updateChunkFromBrickMap(chunk, *blob.brickMap);

            bakeMaterialsFromBrickMap(chunk.geometryData, blob, *blob.brickMap);

            blob.geometry = std::move(chunk.geometryData);

            core::edit(" applyEditsToChunk: done res={} geomNodes={} materialIDs={}",
                       chunk.header.resolution,
                       chunk.geometryData.size() / 3,
                       blob.materialIDs.size());
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
                    applyEditsToChunk(scene, chunk, comp.editQueue.ops);
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
            std::unordered_map<int, std::vector<PendingVoxelOp>> perCellOps;
            for (const auto& op : comp.editQueue.ops) {
                core::ivec3 cellCoord(
                    floorDiv(op.position.x, static_cast<int32_t>(res)),
                    floorDiv(op.position.y, static_cast<int32_t>(res)),
                    floorDiv(op.position.z, static_cast<int32_t>(res))
                );
                int lin = (cellCoord.x - originCell.x)
                          + grid.dims.x * ((cellCoord.y - originCell.y)
                                           + grid.dims.y * (cellCoord.z - originCell.z));
                perCellOps[lin].push_back(op);
            }

            // 5. Collect all unique cell indices.
            std::unordered_set<int> allCells;
            for (const auto& pair : perCellOps) allCells.insert(pair.first);

            // 6. Apply per cell.
            for (int lin : allCells) {
                int32_t chunkIdx = grid.cellToChunk[lin];
                const auto& ops = perCellOps[lin];

                core::edit(" Grid path: cell lin={} chunkIdx={} ops={} dims=({},{},{}) originCell=({},{},{})",
                           lin, chunkIdx, ops.size(),
                           grid.dims.x, grid.dims.y, grid.dims.z,
                           originCell.x, originCell.y, originCell.z);

                if (chunkIdx < 0) {
                    // New cell: create an empty chunk and populate from ops.
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

                    // Create brick map and apply initial adds.
                    // Wrap op positions to cell-local coordinates (ops are in
                    // global component-space; the cell covers [cellOrigin, cellOrigin+res)).
                    core::ivec3 brickDims = computeBrickDims(res);
                    auto brickMap = createVoxelBrickMap(brickDims);
                    for (const auto& op : ops) {
                        if (op.isAdd) {
                            int32_t lx = static_cast<int32_t>(floorMod(op.position.x, static_cast<int32_t>(res)));
                            int32_t ly = static_cast<int32_t>(floorMod(op.position.y, static_cast<int32_t>(res)));
                            int32_t lz = static_cast<int32_t>(floorMod(op.position.z, static_cast<int32_t>(res)));
                            brickMapSetVoxel(*brickMap, lx, ly, lz, op.materialID);
                        }
                    }

                    core::edit(" Grid path: creating new chunk lin={} pos=({},{},{}) initRes={} ops={}",
                               lin, chunkPos.x, chunkPos.y, chunkPos.z,
                               newHdr.resolution, ops.size());

                    updateChunkFromBrickMap(newChunk, *brickMap);

                    // Bake materials: create a temporary blob, bake, then move data into intern.
                    GeometryBlob tempBlob;
                    bakeMaterialsFromBrickMap(newChunk.geometryData, tempBlob, *brickMap);

                    core::edit(" Grid path: after updateChunkFromBrickMap: finalRes={} scale={} geomNodes={} materialIDs={}",
                               newChunk.header.resolution, newChunk.header.scale,
                               newChunk.geometryData.size() / 3,
                               tempBlob.materialIDs.size());

                    internChunkGeometry(scene, newChunk);

                    // Transfer baked materials to the new blob.
                    if (newChunk.geometryPoolIndex >= 0) {
                        GeometryBlob& newBlob = scene.geometryPool[newChunk.geometryPoolIndex];
                        newBlob.materialIDs = std::move(tempBlob.materialIDs);
                        newBlob.materialPalette = std::move(tempBlob.materialPalette);
                        newBlob.brickMap = std::move(brickMap);
                    }

                    ChunkHandle newHandle = static_cast<ChunkHandle>(scene.chunks.size());
                    scene.chunks.push_back(std::move(newChunk));
                    grid.cellToChunk[lin] = static_cast<int32_t>(newHandle);

                    core::edit(" Grid path: created new chunk handle={} at cell lin={} pos=({},{},{}) res={}",
                               newHandle, lin, chunkPos.x, chunkPos.y, chunkPos.z,
                               newHdr.resolution);
                } else {
                    // Existing cell: COW fork + apply edits via brick map.
                    Chunk& existingChunk = scene.chunks[chunkIdx];
                    core::edit(" Grid path: editing existing chunk handle={} cellLin={} pool={} oldRes={} forcing to {}",
                               chunkIdx, existingChunk.cellIndex,
                               existingChunk.geometryPoolIndex,
                               existingChunk.header.resolution, res);
                    existingChunk.header.resolution = res;
                    applyEditsToChunk(scene, existingChunk, ops);
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