#ifndef PROJECTV_EDITING_H
#define PROJECTV_EDITING_H

#include <vector>
#include <cstdint>

#include "data_structures/scene.h"
#include "data_structures/color.h"

namespace projv::utils {
    /**
     * Expand a SceneGrid to cover the given cell, in the **absolute** cell space that
     * SceneGrid::originCellCoord is measured in -- the same space floorDiv(voxelPosition, resolution)
     * produces, and that a grid's cells are addressed by everywhere else. NOT an index into the
     * grid's current cellToChunk: a grid covers [originCellCoord, originCellCoord + dims - 1], so
     * handing this an already-rebased index makes it read the grid as though it still started at
     * (0,0,0) and expand on every call.
     *
     * Handles both directions: a cell below the current low corner shifts originCellCoord, and
     * grid.origin by the same number of cells, so every existing chunk keeps its world position.
     * Idempotent -- expanding to a cell the grid already covers changes nothing.
     *
     * @param grid     The grid to expand.
     * @param cellCoord Absolute cell coordinate to include.
     * @param scene    The owning scene (needed to update chunk cellIndex on remap).
     * @param gridIndex Index of this grid in scene.grids.
     */
    void expandGridToInclude(SceneGrid& grid, core::ivec3 cellCoord,
                             Scene& scene, int gridIndex);

    /**
     * Convert a loose Chunk-kind component to a 1-cell Grid. Called automatically by updateScene
     * when an edit overflows the chunk's resolution. The existing chunk becomes grid cell 0.
     * @param scene     The owning scene.
     * @param component The ComponentHandle of the loose chunk to convert.
     */
    void convertChunkToGrid(Scene& scene, ComponentHandle component);

    /**
     * Append add operations to a component's edit queue. Voxel positions are component-space
     * continuous integer coords (NOT Z-order). Accepts both Chunk and Grid components.
     * @param scene The scene owning the component.
     * @param component The ComponentHandle to enqueue ops for.
     * @param voxels The ops to append (their `isAdd` flag is overwritten with true).
     * @return true if the ops were queued; false if component handle is invalid.
     */
    bool queueVoxelAdd(Scene& scene, ComponentHandle component,
                       const std::vector<PendingVoxelOp>& voxels);

    /**
     * Append remove operations to a component's edit queue. Same semantics as queueVoxelAdd but
     * with `isAdd` overwritten to false.
     * @param scene The scene owning the component.
     * @param component The ComponentHandle to enqueue ops for.
     * @param voxels The ops to append.
     * @return true if the ops were queued; false if component handle is invalid.
     */
    bool queueVoxelRemove(Scene& scene, ComponentHandle component,
                          const std::vector<PendingVoxelOp>& voxels);

    /**
     * Drain every component's editQueue, applying COW forks and rebuilding the touched chunks'
     * pool blobs. CPU-only -- the GPU textures still reference the old blob ranges; the caller
     * must invoke rebuildSceneTextures (Phase 4) afterwards to see the edits on screen. Clears
     * every queue. Handles both Chunk (loose) and Grid components with cell bucketing.
     *
     * @param scene The scene to apply queued edits to.
     * @return The number of components whose queues were drained.
     */
    uint32_t updateScene(Scene& scene);
}

#endif