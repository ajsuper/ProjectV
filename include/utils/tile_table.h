#ifndef PROJECTV_TILE_TABLE_H
#define PROJECTV_TILE_TABLE_H

#include <cstdint>
#include <vector>

#include "core/math.h"

// A chunk's tile table: its voxels as three fixed levels -- 64^3 regions, 16^3 blocks, 4^3 bricks --
// the form the GPU marches (docs/plans/voxel-storage-tiles.md).
//
// The region and block levels are dense over the box the chunk's content spans and addressed straight
// from a voxel's coordinate, as the block table's were, so no read waits on another. Each entry carries two masks
// over its 64 children:
//   occupied  which children hold anything
//   full      which children are entirely solid, all one material -- a "tile"
// A full child has nothing below it: its material is one byte in a list ranked by the full mask. Only
// partly filled bricks have leaves. So a solid interior costs a bit and a byte per brick (per block when
// the whole block is one material), where the tree64 it replaces paid a leaf node for every 4^3 of it:
// memory follows the surface, not the volume.
//
// Leaves keep the tree64's leaf encoding exactly (voxel mask; data3 = encodeLeafNode(material offset,
// uniform)), and a full child is read as a synthetic leaf -- every bit set, uniform, its material offset
// the tile's byte -- so everything that reads leaves (materials, animation, the peel) is unchanged.
//
// It is a blob's stored form (utils/blob_voxels.h), what .data v4 holds, and what the GPU is given as
// it is; buildTileTable makes one from a tree (an old file, the brick-map authoring path).
//
// Layout, in RGBA32U texels (4 words). The dense levels cover only the box the content spans -- in
// regions (64^3, the region level's own grid) and in blocks (16^3) -- not the chunk's whole cube, so a
// big, sparse chunk (a long pipe, a crane) pays for its box rather than its cube:
//   [0]                         header: region box origin, region box size - 1, block box origin, block
//                               box size - 1, each packed 10 bits an axis (x | y << 10 | z << 20)
//   [1, 1 + RC)                 region masks:  (occupied hi, occupied lo, full hi, full lo), RC the
//                               region box's volume, x-fastest over the box
//   [.., + RS)                  region starts: one word per region, four to a texel -- its first
//                               full-block material (region i: texel i/4, word i%4)
//   [.., + BC)                  block masks:   (occupied hi, occupied lo, full hi, full lo), BC the block
//                               box's volume, x-fastest over the box
//   [.., + BS)                  block starts:  two words per block, two blocks to a texel -- its first
//                               partial leaf (relative to the leaves) and its first full-brick material
//   [leafStart, ...)            leaves:        (voxel mask hi, lo, encodeLeafNode(material, uniform), 0),
//                               in rank order over each block's partial bricks (occupied & ~full)
// with RS = ceil(RC/4), BS = ceil(BC/2). A coordinate outside a box is empty. The region box holds the
// block box (block b is in region b/4); a region's mask bits are Z-orders of its blocks by their
// position in the chunk, so boxes never change what a bit means. Masks keep the tree's convention
// (bit 63 of hi:lo is Z-order child 0). Material offsets index `materials`. A block that is itself a
// full tile has nothing stored at its block entry: the region's full bit answers for all of it.
namespace projv::utils {
    struct TileTable {
        uint32_t resolution = 0;
        std::vector<uint32_t> texels;     // 4 words each; empty for an empty tree
        uint32_t leafStart = 0;           // the texel the leaves start at (TileLayout::leafStart)
        std::vector<uint8_t> materials;   // palette slots, indexed by the material offsets above
        // What it holds
        uint32_t fullBlocks = 0, fullBricks = 0, partialBricks = 0;
    };

    // Where a table's arrays are, from its header texel.
    struct TileLayout {
        core::ivec3 regionOrigin{0}, regionDims{0}, blockOrigin{0}, blockDims{0};
        size_t regionCount = 0, blockCount = 0;
        size_t regionMasks = 1, regionStarts = 1, blockMasks = 1, blockStarts = 1, leafStart = 1;
        // A region's / block's index in its box (chunk coordinates of the region / block), -1 outside it.
        int64_t regionIndex(core::ivec3 region) const;
        int64_t blockIndex(core::ivec3 block) const;
    };
    TileLayout tileTableLayout(const TileTable& table);

    // The tile table of `tree64` (3 words per node, root first) and its materials. `strayLeaves`, when
    // given, counts leaves found above the 4^3 level, which no tree the engine builds or downsamples
    // contains, and which the table cannot represent (they are dropped).
    TileTable buildTileTable(const std::vector<uint32_t>& tree64, const std::vector<uint8_t>& materialIDs,
                             uint32_t resolution, uint32_t* strayLeaves = nullptr);

    // Whether voxel `v` is set, looked up the way the shader does: region, block, then a tile or a leaf.
    // On a hit, `slot` is its palette slot.
    bool tileTableVoxel(const TileTable& table, const core::ivec3& v, uint8_t* slot = nullptr);

    // One voxel edit: set `voxel` to palette slot `slot` (`add`), or clear it.
    struct TileEdit {
        core::ivec3 voxel;
        bool add = true;
        uint8_t slot = 0;
    };

    // Applies `edits`, in order, to the table in place. Only the 16^3 blocks they touch are decoded and
    // re-encoded -- a block that ends up solid and one material becomes a tile, a tile cut into becomes
    // leaves only where it has to -- and the rest are carried over as they are. Edits outside the
    // table's cube are ignored. A table edited down to nothing is left empty (no texels), as an empty
    // tree builds one. `table.resolution` must be set, which it is on any table built from a tree.
    void applyTileEdits(TileTable& table, const std::vector<TileEdit>& edits);

    // Storage LOD: the table `lodSteps` levels coarser, each step 4x per axis, at
    // resolutionAtLOD(resolution, lodSteps) (clamped by clampRenderLOD, so the result is never below 4^3).
    // A coarse voxel is solid when any voxel it covers is, and takes the slot of the first of them in
    // Z-order -- the voxels downsampleTree64 keeps, so the result is the table of the downsampled tree,
    // made without one. Zero steps returns the table as it is.
    TileTable downsampleTileTable(const TileTable& table, uint32_t lodSteps);

    // Whether a table's offsets all stay inside it: every leaf and material index its masks and starts
    // imply, and the full bits within the occupied ones. For a table from somewhere untrusted -- the wire,
    // a file -- before anything reads it; a table this engine built always passes.
    bool tileTableWellFormed(const TileTable& table);

    // Recomputes the counts (fullBlocks, fullBricks, partialBricks) of a table whose texels, materials,
    // resolution and leafStart were set directly -- one read back from a file.
    void tileTableRecount(TileTable& table);

    // How many solid voxels the table holds: its leaves' set bits, 64 a full brick, 4096 a full block.
    uint64_t tileTableVoxelCount(const TileTable& table);

    // The box its solid voxels occupy, inclusive. False when it holds none.
    bool tileTableBounds(const TileTable& table, core::ivec3& low, core::ivec3& high);

    // Every solid voxel of the table, `visit(position, slot)`, block by block (regions in linear order,
    // blocks and bricks in Z-order). A tile visits all of its voxels.
    template <typename Visit>
    void forEachTileVoxel(const TileTable& table, Visit&& visit);

    // The solid voxels inside the box [low, high] (inclusive), `visit(position, slot)`: only the blocks and
    // bricks the box reaches are looked at, and in a tile only the cells inside it. In no fixed order.
    template <typename Visit>
    void forEachTileVoxelIn(const TileTable& table, core::ivec3 low, core::ivec3 high, Visit&& visit);

    namespace tile_detail {
        // One brick's voxels: `mask` (the tree's convention, bit 63 = Z-order 0) at `origin`, their
        // slots from `materials` at `offset` -- one byte for all when `uniform`, else one per set bit.
        template <typename Visit>
        void visitBrick(core::ivec3 origin, uint64_t mask, bool uniform, uint32_t offset,
                        const std::vector<uint8_t>& materials, Visit& visit);
    }
}

#include "utils/tile_table_inl.h"

#endif
