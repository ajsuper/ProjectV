#ifndef VOXEL_H
#define VOXEL_H

#include "color.h"

namespace projv{
    /**
     * @brief This structure contains the properties of a voxel. It contains filled and a Color.
     * @struct voxel
     */
    struct voxel {
        bool filled;
        Color color;
    };
}

#endif