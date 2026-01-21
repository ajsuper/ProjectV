#ifndef NODE_STRUCTURE_H
#define NODE_STRUCTURE_H

#include <stdint.h>

namespace projv{
    struct nodeStructure {
        uint32_t standardNode;
        uint32_t ZOrderIndex;
    };

    struct nodeStructureTree64 {
        uint32_t mask1;
        uint32_t mask2;
        uint32_t pointerAndLeafFlag; // 31 bits for pointer, rightmost bit for leaf flag.
        uint32_t ZOrderIndex;
    };
}

#endif
