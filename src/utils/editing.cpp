#include "utils/editing.h"

#include "core/log.h"
#include "utils/voxel_math.h"
#include "utils/voxel_management.h"

namespace projv::utils {
    namespace {
        // Lazily allocate Scene.dataReferences[dataRefID] for a component, deduped by
        // (sourcePath, resolution, voxelScale). For P1, the only source of those values is the
        // loose chunk's header; P2/P3 will derive them from a chosen block of the .data grid.
        int32_t ensureDataReference(Scene& scene, ComponentRecord& comp) {
            if (comp.dataRefID >= 0) return comp.dataRefID;
            const Chunk& chunk = scene.chunks[comp.chunkHandle];
            DataReference ref;
            ref.sourceDataPath = comp.sourcePath;
            ref.resolution     = chunk.header.resolution;
            ref.voxelScale     = chunk.header.voxelScale;
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

        // Drain one component's queue. Phase 1: only Chunk components; Grid returns false after
        // clearing its queue (P2 owns grid support).
        bool applyComponentQueue(Scene& scene, ComponentHandle h) {
            ComponentRecord& comp = scene.components[h];
            if (comp.kind != ComponentKind::Chunk) {
                core::warn("editing::applyComponentQueue: component {} is a Grid; "
                           "Phase 2 will handle. Queue cleared.", h);
                comp.editQueue.ops.clear();
                return false;
            }
            if (comp.editQueue.ops.empty()) return true;

            ensureDataReference(scene, comp);
            Chunk& chunk = scene.chunks[comp.chunkHandle];

            // 1. COW: always fork the blob (P1 ignores Mutability). forkBlob leaves the original's
            //    refCount unchanged -- per the plan's spec.
            int32_t oldIdx = chunk.geometryPoolIndex;
            int32_t newIdx = forkBlob(scene, oldIdx);
            chunk.geometryPoolIndex = newIdx;

            // 2. Decompress current state into a working VoxelBatch.
            VoxelBatch current = getChunkVoxelBatch(scene, chunk, true);

            // 3. Bucket each queued op into adds vs removes; reject out-of-bounds (P2 handles).
            uint32_t res = chunk.header.resolution;
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
                    core::warn("editing: op position {} outside chunk resolution {}; "
                               "skipping (Phase 2 handles grid growth).",
                               op.position.x, op.position.y, op.position.z, res);
                    continue;
                }
                Voxel v = createVoxel(op.color, local);
                (op.isAdd ? adds : removes).push_back(v);
            }

            if (!removes.empty()) removeVoxelBatchAFromVoxelBatchB(removes, current);
            if (!adds.empty())    addVoxelBatchAToVoxelBatchB(adds, current, {0, 0, 0});

            // 4. Move batch into chunkQueue and rebuild tree64 + voxelTypeData.
            moveVoxelBatchToChunk(current, chunk);
            updateChunkFromItsVoxelBatch(chunk, /*clearBatch=*/true);

            // 5. Write the rebuilt arrays back to the (forked) pool blob. updateChunkFromItsVoxelBatch
            //    populates chunk.geometryData / chunk.voxelTypeData in place (voxel_management.cpp:373-375).
            GeometryBlob& fork = scene.geometryPool[chunk.geometryPoolIndex];
            fork.geometry      = chunk.geometryData;
            fork.voxelTypeData = chunk.voxelTypeData;

            // 6. Clear the queue.
            comp.editQueue.ops.clear();
            return true;
        }
    } // anon

    bool queueVoxelAdd(Scene& scene, ComponentHandle h,
                       const std::vector<PendingVoxelOp>& voxels) {
        if (h >= scene.components.size()) return false;
        if (scene.components[h].kind != ComponentKind::Chunk) return false;
        auto& q = scene.components[h].editQueue.ops;
        q.reserve(q.size() + voxels.size());
        for (auto v : voxels) { v.isAdd = true; q.push_back(v); }
        return true;
    }

    bool queueVoxelRemove(Scene& scene, ComponentHandle h,
                          const std::vector<PendingVoxelOp>& voxels) {
        if (h >= scene.components.size()) return false;
        if (scene.components[h].kind != ComponentKind::Chunk) return false;
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