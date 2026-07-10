#ifndef VOXEL_H
#define VOXEL_H

#include <vector>
#include "color.h"
#include "core/math.h"

namespace projv{
    /**
     * @brief This structure contains the properties of a voxel. It contains filled and a Color.
     * @struct voxel
     */
    struct Voxel {
        uint32_t ZOrderPosition;
        Color color;
    };

    struct VoxelGrid {
        uint max;
        std::vector<Voxel> voxels;
    };

    using VoxelBatch = std::vector<Voxel>;

    // A voxel addressed by a plain integer position instead of a packed Z-order index. Voxel's
    // ZOrderPosition is only 9 bits/axis (createZOrderIndex, voxel_math.cpp) -- fine for a coordinate
    // local to one chunk, but not for a continuous coordinate spanning a whole grid (see
    // utils::addVoxelsToComponent in utils/streaming.h), which can exceed that range for any grid
    // more than a couple of cells across. EditVoxel is the public edit-batch currency for exactly
    // that reason; it's converted down to a local Voxel (via utils::createVoxel) only once bucketed
    // to a single chunk, whose local range is always safely within today's limits.
    struct EditVoxel {
        core::ivec3 position;
        Color color;
    };
    using EditVoxelBatch = std::vector<EditVoxel>;
}

#endif
