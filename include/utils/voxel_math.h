#ifndef PROJECTV_VOXEL_MATH_H
#define PROJECTV_VOXEL_MATH_H

#include <array>
#include <stdint.h>

namespace projv::utils {
    /**
     * Converts a 3D vector to a single uint32_t storing that position.
     * @param x, y, z The 3d vector to convert to a single uint32_t.
     * @return A single uint32_t storing the 3D vector.
     */
    uint32_t convertVec3ToHeaderPosition(uint32_t x, uint32_t y, uint32_t z);

    /**
     * Converts a single uint32_t storing a 3D vector to an std::array<int, 3>.
     * @param headerPosition A single uint32_t storing a 3D vector.
     * @return An std::array<int, 3> containing the 3D vector.
     */
    std::array<int, 3> convertHeaderPositionToVec3(uint32_t headerPosition);

    /**
     * Creates the Z-Order Index of a point in 3D space with a certain bit depth.
     * @param x, y, z Position in 3D space to calculate Z-Order of.
     * @param bitDepth How many bits the index takes up.
     * @return Retruns the Z-Order
     */
    uint64_t createZOrderIndex(const int& x, const int& y, const int& z, const int& bitDepth);

    /**
     * Creates a 3D point from a Z-Order index and the number of bits the index takes up.
     * @param z_order The Z-Order of the point
     * @param bitDepth The number of bits the index takes up.
     * @return Returns an std::array<int, 3> containg the 3D point given from the Z-Order index.
     */
    std::array<int, 3> reverseZOrderIndex(uint64_t z_order, int bitDepth);
}

#endif