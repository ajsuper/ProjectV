#ifndef PROJECTV_VOXEL_ISLANDS_H
#define PROJECTV_VOXEL_ISLANDS_H

#include <cstdint>
#include <vector>

#include "core/math.h"
#include "data_structures/scene.h"

// Voxel islands: the groups of solid voxels connected face to face. What destruction is built on --
// a body carved in two becomes two bodies -- and so, like collision pieces, deterministic: the same
// voxels give the same islands, in the same order, on every machine.
namespace projv::utils {
    // A blob's islands, 6-connected, each a list of voxel coordinates. Ordered by each island's
    // first voxel in z, y, x scan order; the voxels of an island in the order the flood reached them.
    // Empty for an empty blob, or one too large to decode (maxCells, as for collision).
    std::vector<std::vector<core::ivec3>> voxelIslands(const GeometryBlob& blob, uint32_t resolution,
                                                       uint64_t maxCells = 1ull << 25);

    struct IslandSplit {
        // The new components, one per island beyond the one the original kept, in island order.
        std::vector<ComponentHandle> pieces;
        // Voxel counts: [0] what the original kept, then one per piece.
        std::vector<uint64_t> voxels;
    };

    // Splits a loose Chunk component whose voxels form more than one island: the original keeps the
    // largest (the first of equals), and each other island becomes a copy of the component -- its
    // palette and attachments -- holding only that island's voxels. The copies are siblings of the
    // original, or, with `piecesAsRoots`, top-level components placed where the original is in the
    // world (what a piece that falls away on its own needs). Applied (updateScene) before returning.
    // Nothing happens, and no pieces come back, for one island, for a component that is not a loose
    // chunk, or for one too large to decode.
    IslandSplit splitVoxelIslands(Scene& scene, ComponentHandle component, bool piecesAsRoots = false);
}

#endif
