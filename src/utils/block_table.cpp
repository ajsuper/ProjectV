#include "utils/block_table.h"

#include <algorithm>
#include <bit>

#include "data_structures/voxel.h"
#include "utils/voxel_math.h"

namespace projv::utils {
    namespace {
        uint32_t regionsPerAxis(uint32_t resolution) { return std::max(resolution / 64u, 1u); }
        uint32_t blocksPerAxis(uint32_t resolution) { return std::max(resolution / 16u, 1u); }

        size_t linear(const core::ivec3& c, uint32_t n) {
            return size_t(c.x) + size_t(n) * (size_t(c.y) + size_t(n) * size_t(c.z));
        }

        // The Z-order (0..63) of a child at local position `l` (0..3 per axis) inside its parent.
        uint32_t childZOrder(const core::ivec3& l) {
            return uint32_t(computeLocalZOrder(l) & 63u);
        }

        bool maskBit(uint32_t high, uint32_t low, uint32_t z) {
            uint64_t mask = (uint64_t(high) << 32) | uint64_t(low);
            return (mask & (1ull << (63u - z))) != 0;
        }

        uint32_t rankBefore(uint32_t high, uint32_t low, uint32_t z) {
            uint64_t mask = (uint64_t(high) << 32) | uint64_t(low);
            if (z == 0) return 0;
            return uint32_t(std::popcount(mask >> (64u - z)));
        }
    }

    uint32_t blockTableTexels(uint32_t resolution) {
        uint32_t r = regionsPerAxis(resolution), b = blocksPerAxis(resolution);
        return r * r * r + b * b * b;
    }

    uint32_t tree64Resolution(const std::vector<uint32_t>& tree64) {
        const size_t nodeCount = tree64.size() / 3;
        size_t node = 0;
        uint32_t resolution = 4;
        while (node < nodeCount) {
            const uint32_t high = tree64[node * 3], low = tree64[node * 3 + 1], data3 = tree64[node * 3 + 2];
            if ((high | low) == 0) return node == 0 ? 0u : resolution;
            if (tree64IsLeaf(data3)) return resolution;
            node += data3 >> 1;   // the first child: rank 0
            resolution *= 4;
            if (resolution > (1u << 20)) return 0;   // malformed: deeper than any chunk
        }
        return 0;
    }

    std::vector<uint32_t> buildBlockTable(const std::vector<uint32_t>& tree64, uint32_t resolution, uint32_t* strayLeaves) {
        if (strayLeaves) *strayLeaves = 0;
        const size_t nodeCount = tree64.size() / 3;
        if (nodeCount == 0 || resolution < 4) return {};

        const uint32_t regions = regionsPerAxis(resolution), blocks = blocksPerAxis(resolution);
        const size_t regionTexels = size_t(regions) * regions * regions;
        std::vector<uint32_t> table(size_t(blockTableTexels(resolution)) * 4, 0u);
        uint32_t* regionAt = table.data();
        uint32_t* blockAt = table.data() + regionTexels * 4;

        // Synthetic levels for trees too small to have them.
        if (resolution <= 16) regionAt[0] = 0x80000000u;   // one block, at Z-order 0
        if (resolution <= 4) {
            if (!tree64IsLeaf(tree64[2])) { if (strayLeaves) ++*strayLeaves; return {}; }
            blockAt[0] = 0x80000000u;   // one brick, at Z-order 0: the root, which is the leaf
            blockAt[2] = 0;
            return table;
        }

        // The same explicit-stack walk as blobContentBounds: a node's position is recovered from the
        // path to it. Only nodes above the leaves are visited; the leaves are read in place.
        struct Pending { size_t node; core::ivec3 origin; uint32_t size; };
        std::vector<Pending> stack{{0, core::ivec3(0), resolution}};
        while (!stack.empty()) {
            const Pending at = stack.back();
            stack.pop_back();
            if (at.node >= nodeCount) continue;
            const uint32_t high = tree64[at.node * 3], low = tree64[at.node * 3 + 1], data3 = tree64[at.node * 3 + 2];
            if ((high | low) == 0) continue;
            if (tree64IsLeaf(data3)) {   // a leaf above the 4^3 level (the 4^3 leaves are never pushed)
                if (strayLeaves) ++*strayLeaves;
                continue;
            }
            const size_t firstChild = at.node + (data3 >> 1);
            if (at.size == 64) {
                uint32_t* r = regionAt + linear(at.origin / 64, regions) * 4;
                r[0] = high;
                r[1] = low;
            }
            if (at.size == 16) {
                uint32_t* b = blockAt + linear(at.origin / 16, blocks) * 4;
                b[0] = high;
                b[1] = low;
                b[2] = uint32_t(firstChild);
                continue;   // its children are the leaves
            }
            const uint32_t childSize = at.size / 4;
            uint32_t rank = 0;
            for (uint32_t z = 0; z < 64; ++z) {
                if (!maskBit(high, low, z)) continue;
                stack.push_back({firstChild + rank, at.origin + reverseZOrderIndex(z) * int(childSize), childSize});
                ++rank;
            }
        }
        return table;
    }

    bool blockTableVoxel(const std::vector<uint32_t>& table, const std::vector<uint32_t>& tree64,
                         uint32_t resolution, const core::ivec3& v, uint32_t* leafNode, uint32_t* zInLeaf) {
        if (table.empty() || v.x < 0 || v.y < 0 || v.z < 0 ||
            v.x >= int(resolution) || v.y >= int(resolution) || v.z >= int(resolution)) return false;
        const uint32_t regions = regionsPerAxis(resolution), blocks = blocksPerAxis(resolution);
        const size_t regionTexels = size_t(regions) * regions * regions;
        const uint32_t* r = table.data() + linear(v / 64, regions) * 4;
        const uint32_t* b = table.data() + (regionTexels + linear(v / 16, blocks)) * 4;
        if (!maskBit(r[0], r[1], childZOrder((v / 16) % 4))) return false;
        const uint32_t zBrick = childZOrder((v / 4) % 4);
        if (!maskBit(b[0], b[1], zBrick)) return false;
        const size_t leaf = size_t(b[2]) + rankBefore(b[0], b[1], zBrick);
        if (leaf * 3 + 2 >= tree64.size()) return false;
        const uint32_t z = childZOrder(v % 4);
        if (!maskBit(tree64[leaf * 3], tree64[leaf * 3 + 1], z)) return false;
        if (leafNode) *leafNode = uint32_t(leaf);
        if (zInLeaf) *zInLeaf = z;
        return true;
    }
}
