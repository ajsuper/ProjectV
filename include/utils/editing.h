#ifndef PROJECTV_EDITING_H
#define PROJECTV_EDITING_H

#include <vector>
#include <cstdint>

#include "data_structures/scene.h"
#include "data_structures/color.h"

namespace projv::utils {
    /**
     * Append add operations to a component's edit queue. Voxel positions are component-space
     * continuous integer coords (NOT Z-order). Phase 1 only accepts loose (kind == Chunk)
     * components; grid components return false and the queue is unchanged.
     * @param scene The scene owning the component.
     * @param component The ComponentHandle to enqueue ops for.
     * @param voxels The ops to append (their `isAdd` flag is overwritten with true).
     * @return true if the ops were queued; false if the component is not a Chunk kind.
     */
    bool queueVoxelAdd(Scene& scene, ComponentHandle component,
                       const std::vector<PendingVoxelOp>& voxels);

    /**
     * Append remove operations to a component's edit queue. Same semantics as queueVoxelAdd but
     * with `isAdd` overwritten to false.
     * @param scene The scene owning the component.
     * @param component The ComponentHandle to enqueue ops for.
     * @param voxels The ops to append.
     * @return true if the ops were queued; false if the component is not a Chunk kind.
     */
    bool queueVoxelRemove(Scene& scene, ComponentHandle component,
                          const std::vector<PendingVoxelOp>& voxels);

    /**
     * Drain every component's editQueue, applying COW forks and rebuilding the touched chunks'
     * pool blobs. CPU-only -- the GPU textures still reference the old blob ranges; the caller
     * must invoke rebuildSceneTextures (Phase 4) afterwards to see the edits on screen. Clears
     * every queue. Grid components are skipped with a warn (Phase 2 adds support).
     *
     * @param scene The scene to apply queued edits to.
     * @return The number of components whose queues were drained.
     */
    uint32_t updateScene(Scene& scene);
}

#endif