#include "utils/voxel_math.h"
#include "dependencies/glm/glm/glm.hpp"

namespace projv::utils {
    uint32_t convertVec3ToHeaderPosition(core::ivec3 vec3) {
        return (vec3.x & 0x3FF) | ((vec3.y & 0x3FF) << 10) | ((vec3.z & 0x3FF) << 20);
    }

    core::ivec3 convertHeaderPositionToVec3(uint32_t headerPosition) {
        return {headerPosition & 0x3FF, (headerPosition >> 10) & 0x3FF, (headerPosition >> 20) & 0x3FF};
    }

    uint64_t createZOrderIndex(core::ivec3 vec3, const int& bitDepth){
        uint64_t z_order = 0;

        // TODO: Correct the ordering/naming. For some reason it only works when flipped. Changes requried in generator and renderer.
        for (int i = 0; i < bitDepth; ++i) {
            // Swapping x and y axes
            uint64_t bitY = (vec3.z >> i) & 1;
            uint64_t bitX = (vec3.y >> i) & 1; 
            uint64_t bitZ = (vec3.x >> i) & 1;

            // Interleave them with swapped x and y (y -> x -> z).
            z_order |= (bitY << (3 * i)) | (bitX << (3 * i + 1)) | (bitZ << (3 * i + 2));
        }

        return z_order;
    }

    core::ivec3 reverseZOrderIndex(uint64_t z_order, int bitDepth) {
        int x = 0;
        int y = 0;
        int z = 0;

        // TODO: Correct the ordering. For some reason it only works when flipped. Changes requried in generator and renderer.
        for (int i = 0; i < bitDepth; ++i) {
            // Extract bits from z_order in the swapped y -> x -> z pattern.
            uint64_t bitY = (z_order >> (3 * i)) & 1; 
            uint64_t bitX = (z_order >> (3 * i + 1)) & 1; 
            uint64_t bitZ = (z_order >> (3 * i + 2)) & 1;

            // Reconstruct the original x, y, and z by setting the appropriate bits.
            z |= (bitY << i);  // bitY came from z.
            y |= (bitX << i);  // bitX came from y.
            x |= (bitZ << i);  // bitZ came from x.
        }
        return {x, y, z};
    }
}
