#ifndef PROJECTV_VOXEL_MATH_H
#define PROJECTV_VOXEL_MATH_H

#include <array>
#include <stdint.h>
#include <iostream>
#include <map>
#include "core/math.h"
#include "data_structures/voxel.h"
//#include "robin.h"

namespace projv::utils {
    /**
     * Converts a 3D vector to a single uint32_t storing that position.
     * @param vec3 The 3d vector to convert to a single uint32_t.
     * @return A single uint32_t storing the 3D vector.
     */
    uint32_t convertVec3ToHeaderPosition(core::ivec3 vec3);

    /**
     * Converts a single uint32_t storing a 3D vector to an std::array<int, 3>.
     * @param headerPosition A single uint32_t storing a 3D vector.
     * @return A core::ivec3 containing the 3D vector.
     */
    core::ivec3 convertHeaderPositionToVec3(uint32_t headerPosition);

    /**
     * Creates the Z-Order Index of a point in 3D space with a certain bit depth.
     * @param x, y, z Position in 3D space to calculate Z-Order of.
     * @param bitDepth How many bits the index takes up.
     * @return Retruns the Z-Order
     */
    uint64_t createZOrderIndex(core::ivec3 vec3);

    /**
     * Creates a 3D point from a Z-Order index and the number of bits the index takes up.
     * @param z_order The Z-Order of the point
     * @param bitDepth The number of bits the index takes up.
     * @return Returns an std::array<int, 3> containg the 3D point given from the Z-Order index.
     */
    core::ivec3 reverseZOrderIndex(uint64_t z_order);

    /**
     * Floor-style integer division (truncation toward -infinity). C++ `/` on negative ints truncates
     * toward 0, which is wrong for cell bucketing (e.g. (-5) / 256 should be -1, not 0). Only valid
     * for `b > 0` -- the only case the editing system uses.
     * @param a The numerator.
     * @param b The denominator (must be > 0).
     * @return The floor-divisor quotient.
     */
    int32_t floorDiv(int32_t a, int32_t b);

    /**
     * Floor-style integer modulo (always in [0, b)). Pairs with floorDiv so that for every (a, b),
     * a == b * floorDiv(a, b) + floorMod(a, b). Only valid for `b > 0`.
     * @param a The numerator.
     * @param b The modulus (must be > 0).
     * @return The non-negative floor-modulo remainder.
     */
    int32_t floorMod(int32_t a, int32_t b);

    // ---- Brick map coordinate helpers ----

    /**
     * Compute the brick coordinate for a voxel position.
     * brickCoord = floorDiv(pos, BRICK_SIZE)
     */
    core::ivec3 computeBrickCoord(int x, int y, int z);

    /**
     * Compute the local position within a brick for a voxel position.
     * localPos = floorMod(pos, BRICK_SIZE)
     */
    core::ivec3 computeBrickLocalPos(int x, int y, int z);

    /**
     * Compute the Z-order of a brick coordinate within a 4-ary grid.
     * Since brickDims is a power-of-2 of 4 (64 = 4^3 per brick axis),
     * this is just the standard Z-order of the brick coord.
     */
    uint32_t computeBrickZOrder(const core::ivec3& brickCoord,
                                const core::ivec3& brickDims);

    /**
     * Compute the Z-order of a local position within a 64^3 brick.
     * Uses 6 bits per axis.
     */
    uint32_t computeLocalZOrder(const core::ivec3& localPos);

    /**
     * Compute brickDims from a resolution (power of 4, >= 64).
     * Returns {1,1,1} for resolution < 64.
     */
    core::ivec3 computeBrickDims(uint32_t resolution);
}

#endif
