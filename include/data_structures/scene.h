#ifndef OCTREE_H
#define OCTREE_H

#include <vector>
#include <stdint.h>

namespace projv{
    struct CPUChunkHeader { // Designed to be user interfacable on CPU. Stored in disk. Only the necessary information for loading the chunk.
        uint32_t chunkID;
        uint32_t position;
        uint32_t scale;
        uint32_t resolution;
    };

    struct GPUChunkHeader { // Not designed to be user interfacable on CPU. Only exists during runtime, mainly on GPU. Only the necessary information for rendering.
        uint32_t position;
        uint32_t scale;
        uint32_t resolution;
        uint32_t geometryStartIndex;
        uint32_t geometryEndIndex;
        uint32_t voxelTypeDataStartIndex;
        uint32_t voxelTypeDataEndIndex;
    };
    
    struct RuntimeChunkData { // Only exists during runtime. Contains all of the header data and our geometry and color data, along with any extra runtime data not used in rendering.
        CPUChunkHeader header;
        std::vector<uint32_t> geometryData;
        std::vector<uint32_t> voxelTypeData;
        uint32_t LOD;
    };
    
    struct Scene {
        std::vector<RuntimeChunkData> chunks;
    };
}

#endif
