#include "utils/voxel_math.h"
#include "dependencies/glm/glm/glm.hpp"

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

    uint64_t createZOrderIndex(core::ivec3 vec3, const int& bitDepth){
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

    core::ivec3 reverseZOrderIndex(uint64_t z_order, int bitDepth) {
        // Forward LUTs
        static std::array<uint32_t, 512> xArray = precalculateArray(1);
        static std::array<uint32_t, 512> yArray = precalculateArray(2); // GOOD
        static std::array<uint32_t, 512> zArray = precalculateArray(3);

        // Reverse LUTs (maps partial z_order  -> coordinate)
        static std::unordered_map<uint32_t, uint16_t> revXArray = createReverseLUT(xArray);
        static std::unordered_map<uint32_t, uint16_t> revYArray = createReverseLUT(yArray);
        static std::unordered_map<uint32_t, uint16_t> revZArray = createReverseLUT(zArray);

        core::ivec3 coordinate;

        coordinate.x = revXArray[z_order & 0b100100100100100100100100100];
        coordinate.y = revYArray[z_order & 0b010010010010010010010010010];
        coordinate.z = revZArray[z_order & 0b001001001001001001001001001];
        return coordinate;
    }

}
