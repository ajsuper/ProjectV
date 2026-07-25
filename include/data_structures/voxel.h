#ifndef VOXEL_H
#define VOXEL_H

#include <vector>
#include <memory>
#include <unordered_map>
#include "color.h"
#include "core/math.h"

namespace projv{

    struct VoxelBrickMap;

    constexpr uint32_t BRICK_SIZE = 64;
    constexpr uint32_t BRICK_SIZE_LOG2 = 6;
    constexpr uint32_t BRICK_VOXEL_COUNT = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE;   // 262144
    constexpr uint32_t BRICK_MASK_ROWS  = BRICK_VOXEL_COUNT / 64;                   // 4096

    // ---- tree64 node data3 layout ----
    //
    // Internal node: bit 0 clear, bits 1..31 are the child pointer (childPtr == data3 >> 1).
    // Leaf node:     bit 0 set, bit 1 is the uniform flag, bits 2..31 are the material offset
    //                (a byte offset into the blob's materialIDs array).
    //
    // A leaf is "uniform" when every one of its set voxels shares a material. Uniform leaves store
    // exactly ONE material byte instead of one per voxel, and the shader ignores the within-leaf
    // index. On heightfield terrain ~96% of leaves are uniform, which is a ~17x reduction in
    // material texture bytes; random access is unaffected. Non-uniform leaves are unchanged.
    //
    // 30 bits of offset addresses 1G material bytes per blob -- far beyond any single chunk.
    constexpr uint32_t TREE64_LEAF_FLAG    = 1u;
    constexpr uint32_t TREE64_UNIFORM_FLAG = 2u;
    constexpr uint32_t TREE64_MATOFF_SHIFT = 2u;

    inline uint32_t encodeLeafNode(uint32_t materialOffset, bool uniform) {
        return (materialOffset << TREE64_MATOFF_SHIFT)
             | (uniform ? TREE64_UNIFORM_FLAG : 0u)
             | TREE64_LEAF_FLAG;
    }
    inline bool     tree64IsLeaf(uint32_t data3)        { return (data3 & TREE64_LEAF_FLAG) != 0u; }
    inline bool     tree64LeafIsUniform(uint32_t data3) { return (data3 & TREE64_UNIFORM_FLAG) != 0u; }
    inline uint32_t tree64LeafMatOffset(uint32_t data3) { return data3 >> TREE64_MATOFF_SHIFT; }

    struct Voxel {
        uint32_t ZOrderPosition;
        Color color;
    };

    struct VoxelGrid {
        uint max;
        std::vector<Voxel> voxels;
    };

    using VoxelBatch = std::vector<Voxel>;

    /**
     * Per-brick data. Each brick is BRICK_SIZE^3 = 64^3 = 262144 voxels.
     * mask[64] stores 4096 bits (one bit per voxel), row-major within the brick
     * in Z-order. colors is a hash map from Z-order-within-brick (18 bits) to a
     * packed RGBA color (R10G10B10A2, with A=3 for "filled voxel").
     */
    struct BrickData {
        uint64_t mask[BRICK_MASK_ROWS] = {};  // 4096 rows x 64 bits = 262144 bits
        std::unordered_map<uint32_t, uint8_t> materials;  // Z-order-in-brick -> materialID
    };

    /**
     * Sparse voxel brick map. The chunk's volume is divided into
     * brickDims = (resolution/BRICK_SIZE)^3 bricks. Only bricks with a set
     * bit in brickMask have a non-null BrickData entry in the bricks array.
     *
     * Editing is O(1) by writing directly to the brick map. The tree64 and
     * voxelTypeData GPU arrays are rebuilt from the brick map when edits are
     * finalized.
     */
    struct VoxelBrickMap {
        core::ivec3 brickDims;                    // (R/64, R/64, R/64)
        uint32_t totalBricks;                     // brickDims.x * brickDims.y * brickDims.z

        std::vector<uint64_t> brickMask;           // ceil(totalBricks/64) entries, bit i = brick i exists
        std::vector<std::unique_ptr<BrickData>> bricks;  // size = totalBricks, null = absent

        // Serialized normal for new voxels. 0 = (0,0,0) normal (no surface data).
        // Set from existing voxelTypeData when building from disk data.
        uint32_t defaultNormal = 0;
    };

    // Pack Color (8-bit RGB) into R10G10B10A2 format (A=3 for filled).
    inline uint32_t packColor(Color c) {
        return (static_cast<uint32_t>(c.r * 4) << 20) |  // R10
               (static_cast<uint32_t>(c.g * 4) << 10) |  // G10
               (static_cast<uint32_t>(c.b * 4));          // B10
    }

    // Unpack Color from R10G10B10A2 format.
    inline Color unpackColor(uint32_t packed) {
        Color c;
        c.r = static_cast<uint8_t>(((packed >> 20) & 0x3FF) / 4);
        c.g = static_cast<uint8_t>(((packed >> 10) & 0x3FF) / 4);
        c.b = static_cast<uint8_t>((packed & 0x3FF) / 4);
        return c;
    }
}

#endif
