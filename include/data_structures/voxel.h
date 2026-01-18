#ifndef VOXEL_H
#define VOXEL_H

#include <vector>
#include "color.h"

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
}

#endif
