#ifndef OCTREE_H
#define OCTREE_H

#include <vector>
#include <stdint.h>

namespace projv{
    struct chunkHeader {
        uint32_t ID;
        uint32_t position;
        uint32_t scale;
        uint32_t resolution;
        uint32_t geometryStartIndex;
        uint32_t geometryEndIndex;
        uint32_t voxelTypeDataStartIndex;
        uint32_t voxelTypeDataEndIndex;
    };

    struct scene {
        uint32_t version;
        std::vector<chunkHeader> chunkHeaders; // Index should correspond with corresponding serializedGeometry for most optimization.
        std::vector<uint32_t> serializedGeometry;
        std::vector<uint32_t> serializedVoxelTypeData;
    };
}

#endif
