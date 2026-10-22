#ifndef PROJECTV_BLOCK_TABLE_H
#define PROJECTV_BLOCK_TABLE_H

#include <cstdint>
#include <vector>

#include "core/math.h"

// A chunk's block table: the dense top of its tree64, for the GPU march.
//
// The tree64 is walked from the root down -- 64^3, 16^3, 4^3 -- and each level's node can only be read
// once its parent has been, so a ray's every step waited on a chain of four dependent texture reads.
// The block table flattens the two levels above the leaves into arrays addressed straight from a cell's
// coordinate, so a step reads them in parallel and waits on one read only: the leaf, which is still the
// tree's own node (materials, uniform leaves and everything else about leaves are unchanged).
//
// **Nothing authors it.** The GPU upload builds one per geometry blob, from exactly the tree it
// uploads (after storage LOD), and one for the blob's animation envelope; it lives in the tree64Data
// texture right after each tree's nodes (GPUBlobRange::geomNodeCount / envNodeCount say where;
// GPUChunkHeader::blockTableStartIndex / envelopeTableStartIndex). It is never stored.
// Measured in examples/99-ray-lab (modes 14 and 15 against the tree march): about 2x faster for ~14%
// of the tree's size. See docs/plans/block-table-traversal.md.
//
// Layout, in RGBA32U texels (4 words each), for a tree of resolution R:
//   [0, regions^3)                  one per 64^3 region, regions = max(R/64, 1):
//                                   (mask high, mask low, 0, 0) -- the region node's mask: which of its
//                                   4x4x4 16^3 blocks hold voxels
//   [regions^3, + blocks^3)         one per 16^3 block, blocks = max(R/16, 1):
//                                   (mask high, mask low, first leaf, 0) -- which of its 4x4x4 bricks
//                                   hold voxels, and the node index of the first of its leaves,
//                                   relative to the tree's root (the leaves are contiguous, in rank order)
// Both arrays are x-fastest (x + n*(y + n*z)). Masks keep the tree's convention: bit 63 of
// (high << 32 | low) is Z-order child 0. A tree too small to have a region or a block node of its own
// (R = 16, R = 4) gets a synthetic one holding a single child at Z-order 0, so the march needs no
// special case.
namespace projv::utils {
    // Texels in a resolution-R tree's block table.
    uint32_t blockTableTexels(uint32_t resolution);

    // The resolution a tree describes, from its depth: 4 per level, following the first child of each
    // node down to a leaf. Every tree the engine builds or downsamples keeps its leaves on its last
    // level, so the first path's depth is the tree's. 0 for an empty tree.
    uint32_t tree64Resolution(const std::vector<uint32_t>& tree64);

    // The block table of `tree64` (3 words per node, root first). Empty when the tree is empty.
    // `strayLeaves`, when given, counts leaves found above the 4^3 level -- which no tree the engine
    // builds or downsamples contains, and which the table cannot represent (they read as empty).
    std::vector<uint32_t> buildBlockTable(const std::vector<uint32_t>& tree64, uint32_t resolution,
                                          uint32_t* strayLeaves = nullptr);

    // Whether voxel `v` is set, looked up the way the shader does -- region, block, leaf. On a hit,
    // `leafNode` is the leaf's node index (relative to the root) and `zInLeaf` the voxel's Z-order in it.
    bool blockTableVoxel(const std::vector<uint32_t>& table, const std::vector<uint32_t>& tree64,
                         uint32_t resolution, const core::ivec3& v,
                         uint32_t* leafNode = nullptr, uint32_t* zInLeaf = nullptr);
}

#endif
