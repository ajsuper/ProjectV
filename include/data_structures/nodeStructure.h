#ifndef NODE_STRUCTURE_H
#define NODE_STRUCTURE_H

#include <stdint.h>

namespace projv{
    struct nodeStructure {
        uint32_t standardNode;
        uint32_t ZOrderIndex;
    };
}

#endif