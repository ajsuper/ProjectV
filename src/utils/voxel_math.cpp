#include "utils/voxel_math.h"

namespace projv::utils {
    uint32_t convertVec3ToHeaderPosition(core::ivec3 vec3) {
        return (vec3.x & 0x3FF) | ((vec3.y & 0x3FF) << 10) | ((vec3.z & 0x3FF) << 20);
    }

    core::ivec3 convertHeaderPositionToVec3(uint32_t headerPosition) {
        return {headerPosition & 0x3FF, (headerPosition >> 10) & 0x3FF, (headerPosition >> 20) & 0x3FF};
    }

    uint64_t createZOrderIndexNOLUT(core::ivec3 vec3, const int& bitDepth){
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

    // 1 is x, 2 is y, 3 is z.
    std::array<uint32_t, 512> precalculateArray(int axis) {
        std::array<uint32_t, 512> LUT;
        for(int i = 0; i < 512; i++) {
            core::ivec3 position{ 0, 0, 0};
            switch (axis) {
                case 1:
                    position.x = i;
                    break;
                case 2:
                    position.y = i;
                    break;
                case 3:
                    position.z = i;
                    break;
            }

            LUT[i] = createZOrderIndexNOLUT(position, 9);
        }
        return LUT;
    }

    uint64_t createZOrderIndex(core::ivec3 vec3){
        static std::array<uint32_t, 512> xArray = precalculateArray(1);
        static std::array<uint32_t, 512> yArray = precalculateArray(2);
        static std::array<uint32_t, 512> zArray = precalculateArray(3);

        return xArray[vec3.x] + yArray[vec3.y] + zArray[vec3.z];
    }

    std::unordered_map<uint32_t, uint16_t> createReverseLUT(const std::array<uint32_t, 512>& forwardArray) {
        std::cout << "CreatingLUT!!!" << std::endl;
        std::unordered_map<uint32_t, uint16_t> reverseArray{};
        for(size_t i = 0; i < 512; i++) {
            reverseArray[forwardArray[i]] = i;
        }
        return reverseArray;
    };

    core::ivec3 reverseZOrderIndex2(uint64_t z_order) {
        // Forward LUTs
        static std::array<uint32_t, 512> xArray = precalculateArray(1);
        static std::array<uint32_t, 512> yArray = precalculateArray(2);
        static std::array<uint32_t, 512> zArray = precalculateArray(3);

        // Reverse LUTs (maps partial z_order  -> coordinate)
        static std::unordered_map<uint32_t, uint16_t> revXArray = createReverseLUT(xArray);
        static std::unordered_map<uint32_t, uint16_t> revYArray = createReverseLUT(yArray);
        static std::unordered_map<uint32_t, uint16_t> revZArray = createReverseLUT(zArray);

        core::ivec3 coordinate;
        uint32_t xMasked = z_order & 0b100100100100100100100100100;
        uint32_t yMasked = z_order & 0b010010010010010010010010010;
        uint32_t zMasked = z_order & 0b001001001001001001001001001;

        coordinate.x = revXArray[xMasked];
        coordinate.y = revYArray[yMasked];
        coordinate.z = revZArray[zMasked];
        return coordinate;
    }
    
    // Deinterleave every third bit starting from given offset (0 = Y, 1 = X, 2 = Z)
    inline uint32_t compactBits(uint64_t code, int offset) {
        uint64_t x = code >> offset;
        x &= 0x1249249249249249ULL; // Keep every third bit
        x = (x ^ (x >> 2))  & 0x10C30C30C30C30C3ULL;
        x = (x ^ (x >> 4))  & 0x100F00F00F00F00FULL;
        x = (x ^ (x >> 8))  & 0x1F0000FF0000FFULL;
        x = (x ^ (x >> 16)) & 0x1F00000000FFFFULL;
        x = (x ^ (x >> 32)) & 0x1FFFFF;
        return static_cast<uint32_t>(x);
    }

    core::ivec3 reverseZOrderIndex(uint64_t z_order) {
        return {
            compactBits(z_order, 2),  // x came from bit positions 1, 4, 7, ...
            compactBits(z_order, 1),  // y came from bit positions 0, 3, 6, ...
            compactBits(z_order, 0)   // z came from bit positions 2, 5, 8, ...
        };
    }

}
