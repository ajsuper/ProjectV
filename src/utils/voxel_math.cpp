#include "utils/voxel_math.h"

namespace projv::utils {
    uint32_t convertVec3ToHeaderPosition(uint32_t x, uint32_t y, uint32_t z){
        return (x & 0x3FF) | ((y & 0x3FF) << 10) | ((z & 0x3FF) << 20);
    }

    std::array<int, 3> convertHeaderPositionToVec3(uint32_t headerPosition){
        return {headerPosition & 0x3FF, (headerPosition >> 10) & 0x3FF, (headerPosition >> 20) & 0x3FF};
    }

    uint64_t createZOrderIndex(const int& x, const int& y, const int& z, const int& bitDepth){
        uint64_t z_order = 0;

        // TODO: Correct the ordering/naming. For some reason it only works when flipped. Changes requried in generator and renderer.
        for (int i = 0; i < bitDepth; ++i) {
            // Swapping x and y axes
            uint64_t bitY = (z >> i) & 1;
            uint64_t bitX = (y >> i) & 1; 
            uint64_t bitZ = (x >> i) & 1;

            // Interleave them with swapped x and y (y -> x -> z).
            z_order |= (bitY << (3 * i)) | (bitX << (3 * i + 1)) | (bitZ << (3 * i + 2));
        }

        return z_order;
    }

    std::array<int, 3> reverseZOrderIndex(uint64_t z_order, int bitDepth) {
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