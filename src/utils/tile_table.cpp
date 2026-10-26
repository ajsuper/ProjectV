#include "utils/tile_table.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <tuple>

#include "data_structures/voxel.h"
#include "utils/voxel_math.h"

namespace projv::utils {
    namespace {
        // The Z-order (0..63) of a child at local position `l` (0..3 per axis) inside its parent.
        uint32_t childZOrder(const core::ivec3& l) { return uint32_t(computeLocalZOrder(l) & 63u); }

        bool maskBit(uint32_t high, uint32_t low, uint32_t z) {
            uint64_t mask = (uint64_t(high) << 32) | uint64_t(low);
            return (mask & (1ull << (63u - z))) != 0;
        }

        void setBit(uint32_t& high, uint32_t& low, uint32_t z) {
            if (z < 32) high |= 0x80000000u >> z;
            else low |= 0x80000000u >> (z - 32);
        }

        uint32_t rankBefore(uint32_t high, uint32_t low, uint32_t z) {
            uint64_t mask = (uint64_t(high) << 32) | uint64_t(low);
            if (z == 0) return 0;
            return uint32_t(std::popcount(mask >> (64u - z)));
        }

        // The header's packing: 10 bits an axis.
        uint32_t pack3(core::ivec3 v) { return uint32_t(v.x) | (uint32_t(v.y) << 10) | (uint32_t(v.z) << 20); }
        core::ivec3 unpack3(uint32_t w) { return core::ivec3(int(w & 1023u), int((w >> 10) & 1023u), int((w >> 20) & 1023u)); }

        int64_t boxIndex(core::ivec3 c, core::ivec3 origin, core::ivec3 dims) {
            const core::ivec3 l = c - origin;
            if (l.x < 0 || l.y < 0 || l.z < 0 || l.x >= dims.x || l.y >= dims.y || l.z >= dims.z) return -1;
            return int64_t(l.x) + int64_t(dims.x) * (int64_t(l.y) + int64_t(dims.y) * int64_t(l.z));
        }

        TileLayout layoutFor(core::ivec3 regionOrigin, core::ivec3 regionDims, core::ivec3 blockOrigin, core::ivec3 blockDims) {
            TileLayout L;
            L.regionOrigin = regionOrigin;
            L.regionDims = regionDims;
            L.blockOrigin = blockOrigin;
            L.blockDims = blockDims;
            L.regionCount = size_t(regionDims.x) * size_t(regionDims.y) * size_t(regionDims.z);
            L.blockCount = size_t(blockDims.x) * size_t(blockDims.y) * size_t(blockDims.z);
            L.regionMasks = 1;
            L.regionStarts = L.regionMasks + L.regionCount;
            L.blockMasks = L.regionStarts + (L.regionCount + 3) / 4;
            L.blockStarts = L.blockMasks + L.blockCount;
            L.leafStart = L.blockStarts + (L.blockCount + 1) / 2;
            return L;
        }

        // A tree's 4^3 leaf, kept with the brick it covers.
        struct Brick { uint32_t z; uint32_t high, low, data3; };

        // One 16^3 block, as the emitter lays it out: a solid tile, or its bricks -- the full ones'
        // materials and the partial ones' leaves, each leaf's material offset relative to
        // `leafMaterials`.
        struct BlockData {
            bool empty = true, solid = false;
            uint8_t solidSlot = 0;
            uint32_t occupied[2] = {0, 0}, full[2] = {0, 0};
            std::vector<uint8_t> fullMaterials;
            std::vector<uint32_t> leaves;   // 4 words each
            std::vector<uint8_t> leafMaterials;
        };

        using BlockKey = std::tuple<int, int, int>;   // a block's chunk coordinates, z y x
        BlockKey keyOf(core::ivec3 b) { return BlockKey{b.z, b.y, b.x}; }
        core::ivec3 blockOf(const BlockKey& k) { return core::ivec3(std::get<2>(k), std::get<1>(k), std::get<0>(k)); }

        // A dense 16^3 block (x-fastest): 0 empty, slot + 1 solid -- 16 bits, so slot 255 is not empty.
        using Cells = std::array<uint16_t, 4096>;

        size_t cellIndex(core::ivec3 p) { return size_t(p.x) + 16 * (size_t(p.y) + 16 * size_t(p.z)); }

        // The cell of voxel z (Z-order) of brick zr (Z-order) in a block: brickCells()[zr * 64 + z].
        const std::array<uint16_t, 4096>& brickCells() {
            static const std::array<uint16_t, 4096> table = [] {
                std::array<uint16_t, 4096> t{};
                for (uint32_t zr = 0; zr < 64; ++zr)
                    for (uint32_t z = 0; z < 64; ++z)
                        t[zr * 64 + z] = uint16_t(cellIndex(reverseZOrderIndex(zr) * 4 + reverseZOrderIndex(z)));
                return t;
            }();
            return table;
        }

        // The block a dense grid holds, encoded.
        BlockData encodeBlock(const Cells& cells) {
            BlockData d;
            int fullCount = 0;
            bool oneSlot = true;
            const auto& at = brickCells();
            for (uint32_t zr = 0; zr < 64; ++zr) {
                const uint16_t* brick = &at[zr * 64];
                uint32_t high = 0, low = 0, solid = 0;
                bool uniform = true;
                uint16_t first = 0;
                for (uint32_t z = 0; z < 64; ++z) {
                    const uint16_t c = cells[brick[z]];
                    if (!c) continue;
                    setBit(high, low, z);
                    if (solid == 0) first = c;
                    else if (c != first) uniform = false;
                    ++solid;
                }
                if (solid == 0) continue;
                setBit(d.occupied[0], d.occupied[1], zr);
                if (solid == 64 && uniform) {
                    setBit(d.full[0], d.full[1], zr);
                    d.fullMaterials.push_back(uint8_t(first - 1));
                    if (fullCount == 0) d.solidSlot = uint8_t(first - 1);
                    else if (uint8_t(first - 1) != d.solidSlot) oneSlot = false;
                    ++fullCount;
                    continue;
                }
                d.leaves.insert(d.leaves.end(), {high, low, encodeLeafNode(uint32_t(d.leafMaterials.size()), uniform), 0u});
                if (uniform) {
                    d.leafMaterials.push_back(uint8_t(first - 1));
                } else {
                    for (uint32_t z = 0; z < 64; ++z) {
                        const uint16_t c = cells[brick[z]];
                        if (c) d.leafMaterials.push_back(uint8_t(c - 1));
                    }
                }
            }
            d.empty = (d.occupied[0] | d.occupied[1]) == 0;
            d.solid = fullCount == 64 && oneSlot;
            if (d.solid) { d.fullMaterials.clear(); d.leaves.clear(); d.leafMaterials.clear(); }
            return d;
        }

        // A block's voxels as a dense 16^3 grid.
        void expandBlock(const BlockData& d, Cells& cells) {
            cells.fill(0);
            if (d.empty) return;
            if (d.solid) { cells.fill(uint16_t(d.solidSlot + 1)); return; }
            uint32_t fullIndex = 0, leafIndex = 0;
            const auto& at = brickCells();
            for (uint32_t zr = 0; zr < 64; ++zr) {
                if (!maskBit(d.occupied[0], d.occupied[1], zr)) continue;
                const uint16_t* brick = &at[zr * 64];
                if (maskBit(d.full[0], d.full[1], zr)) {
                    const uint8_t slot = d.fullMaterials[fullIndex++];
                    for (uint32_t z = 0; z < 64; ++z) cells[brick[z]] = uint16_t(slot + 1);
                    continue;
                }
                const uint32_t* l = &d.leaves[size_t(leafIndex++) * 4];
                const bool uniform = tree64LeafIsUniform(l[2]);
                uint32_t cursor = tree64LeafMatOffset(l[2]);
                for (uint32_t z = 0; z < 64; ++z) {
                    if (!maskBit(l[0], l[1], z)) continue;
                    const uint8_t slot = d.leafMaterials[uniform ? tree64LeafMatOffset(l[2]) : cursor++];
                    cells[brick[z]] = uint16_t(slot + 1);
                }
            }
        }

        // The block a tree's 4^3 leaves make, encoded as encodeBlock would encode their voxels: a brick
        // whose voxels are all set and all one slot is full, a leaf is uniform when its slots are all one
        // (whatever its flag said), and a block of 64 full bricks of one slot is solid.
        // `bricks` in Z-order.
        BlockData encodeLeaves(const std::vector<Brick>& bricks, const std::vector<uint8_t>& materialIDs) {
            BlockData d;
            int fullCount = 0;
            bool oneSlot = true;
            uint8_t slots[64];
            for (const Brick& b : bricks) {
                const uint32_t count = uint32_t(std::popcount(b.high) + std::popcount(b.low));
                if (count == 0) continue;
                const uint32_t offset = tree64LeafMatOffset(b.data3);
                const bool flagged = tree64LeafIsUniform(b.data3);
                bool uniform = true;
                for (uint32_t k = 0; k < count; ++k) {
                    const size_t i = flagged ? offset : size_t(offset) + k;
                    slots[k] = i < materialIDs.size() ? materialIDs[i] : 0;
                    if (slots[k] != slots[0]) uniform = false;
                }
                setBit(d.occupied[0], d.occupied[1], b.z);
                if (count == 64 && uniform) {
                    setBit(d.full[0], d.full[1], b.z);
                    d.fullMaterials.push_back(slots[0]);
                    if (fullCount == 0) d.solidSlot = slots[0];
                    else if (slots[0] != d.solidSlot) oneSlot = false;
                    ++fullCount;
                    continue;
                }
                d.leaves.insert(d.leaves.end(), {b.high, b.low, encodeLeafNode(uint32_t(d.leafMaterials.size()), uniform), 0u});
                d.leafMaterials.insert(d.leafMaterials.end(), slots, slots + (uniform ? 1 : count));
            }
            d.empty = (d.occupied[0] | d.occupied[1]) == 0;
            d.solid = fullCount == 64 && oneSlot;
            if (d.solid) { d.fullMaterials.clear(); d.leaves.clear(); d.leafMaterials.clear(); }
            return d;
        }

        // Block `b` of table `t`, as stored.
        BlockData readBlock(const TileTable& t, const TileLayout& L, core::ivec3 b) {
            BlockData d;
            if (t.texels.empty()) return d;
            const int64_t rl = L.regionIndex(b / 4);
            if (rl < 0) return d;
            const uint32_t* region = &t.texels[(L.regionMasks + size_t(rl)) * 4];
            const uint32_t zb = childZOrder(b % 4);
            if (!maskBit(region[0], region[1], zb)) return d;
            d.empty = false;
            if (maskBit(region[2], region[3], zb)) {
                d.solid = true;
                const size_t i = t.texels[L.regionStarts * 4 + size_t(rl)] + rankBefore(region[2], region[3], zb);
                d.solidSlot = i < t.materials.size() ? t.materials[i] : 0;
                return d;
            }
            const int64_t bl = L.blockIndex(b);
            if (bl < 0) { d.empty = true; return d; }
            const uint32_t* block = &t.texels[(L.blockMasks + size_t(bl)) * 4];
            const uint32_t* start = &t.texels[L.blockStarts * 4 + size_t(bl) * 2];
            d.occupied[0] = block[0]; d.occupied[1] = block[1]; d.full[0] = block[2]; d.full[1] = block[3];
            const uint32_t fulls = uint32_t(std::popcount(block[2]) + std::popcount(block[3]));
            for (uint32_t k = 0; k < fulls; ++k) {
                const size_t i = size_t(start[1]) + k;
                d.fullMaterials.push_back(i < t.materials.size() ? t.materials[i] : 0);
            }
            const uint32_t partials = uint32_t(std::popcount(block[0] & ~block[2]) + std::popcount(block[1] & ~block[3]));
            for (uint32_t k = 0; k < partials; ++k) {
                const uint32_t* l = &t.texels[(size_t(t.leafStart) + start[0] + k) * 4];
                const bool uniform = tree64LeafIsUniform(l[2]);
                const uint32_t offset = tree64LeafMatOffset(l[2]);
                const uint32_t count = uniform ? 1u : uint32_t(std::popcount(l[0]) + std::popcount(l[1]));
                d.leaves.insert(d.leaves.end(), {l[0], l[1], encodeLeafNode(uint32_t(d.leafMaterials.size()), uniform), 0u});
                for (uint32_t m = 0; m < count; ++m) {
                    const size_t i = size_t(offset) + m;
                    d.leafMaterials.push_back(i < t.materials.size() ? t.materials[i] : 0);
                }
            }
            return d;
        }

        // A table holding `blocks` (every one non-empty): the dense levels over the box they span, and
        // the builder's order -- regions in linear order over the box, each one's blocks and each block's
        // bricks in Z-order, the order the full and partial masks rank them in. Both the builder and the
        // edit repack end here, so what an edit leaves is what a fresh build of the same voxels gives.
        TileTable emitTable(uint32_t resolution, const std::map<BlockKey, BlockData>& blocks) {
            TileTable t;
            t.resolution = resolution;
            if (blocks.empty()) return t;
            core::ivec3 low(1 << 20), high(-(1 << 20));
            for (const auto& [k, d] : blocks) {
                low = glm::min(low, blockOf(k));
                high = glm::max(high, blockOf(k));
            }
            const core::ivec3 regionLow = low / 4, regionHigh = high / 4;
            const TileLayout L = layoutFor(regionLow, regionHigh - regionLow + core::ivec3(1), low, high - low + core::ivec3(1));
            t.leafStart = uint32_t(L.leafStart);
            t.texels.assign(L.leafStart * 4, 0u);
            t.texels[0] = pack3(L.regionOrigin);
            t.texels[1] = pack3(L.regionDims - core::ivec3(1));
            t.texels[2] = pack3(L.blockOrigin);
            t.texels[3] = pack3(L.blockDims - core::ivec3(1));
            std::vector<uint32_t> leaves;
            for (int rz = regionLow.z; rz <= regionHigh.z; ++rz)
                for (int ry = regionLow.y; ry <= regionHigh.y; ++ry)
                    for (int rx = regionLow.x; rx <= regionHigh.x; ++rx) {
                        const core::ivec3 r{rx, ry, rz};
                        const size_t rl = size_t(L.regionIndex(r));
                        uint32_t* region = &t.texels[(L.regionMasks + rl) * 4];
                        t.texels[L.regionStarts * 4 + rl] = uint32_t(t.materials.size());
                        const BlockData* here[64] = {};
                        for (uint32_t zb = 0; zb < 64; ++zb) {
                            auto it = blocks.find(keyOf(r * 4 + reverseZOrderIndex(zb)));
                            if (it != blocks.end()) here[zb] = &it->second;
                        }
                        for (uint32_t zb = 0; zb < 64; ++zb) {
                            if (!here[zb]) continue;
                            setBit(region[0], region[1], zb);
                            if (!here[zb]->solid) continue;
                            setBit(region[2], region[3], zb);
                            t.materials.push_back(here[zb]->solidSlot);
                            ++t.fullBlocks;
                        }
                        for (uint32_t zb = 0; zb < 64; ++zb) {
                            const BlockData* d = here[zb];
                            if (!d || d->solid) continue;
                            const size_t bl = size_t(L.blockIndex(r * 4 + reverseZOrderIndex(zb)));
                            uint32_t* block = &t.texels[(L.blockMasks + bl) * 4];
                            uint32_t* start = &t.texels[L.blockStarts * 4 + bl * 2];
                            block[0] = d->occupied[0]; block[1] = d->occupied[1]; block[2] = d->full[0]; block[3] = d->full[1];
                            start[0] = uint32_t(leaves.size() / 4);
                            start[1] = uint32_t(t.materials.size());
                            t.materials.insert(t.materials.end(), d->fullMaterials.begin(), d->fullMaterials.end());
                            t.fullBricks += uint32_t(d->fullMaterials.size());
                            const uint32_t base = uint32_t(t.materials.size());
                            for (size_t k = 0; k < d->leaves.size(); k += 4) {
                                const uint32_t data3 = d->leaves[k + 2];
                                leaves.insert(leaves.end(), {d->leaves[k], d->leaves[k + 1],
                                                             encodeLeafNode(base + tree64LeafMatOffset(data3), tree64LeafIsUniform(data3)), 0u});
                                ++t.partialBricks;
                            }
                            t.materials.insert(t.materials.end(), d->leafMaterials.begin(), d->leafMaterials.end());
                        }
                    }
            t.texels.insert(t.texels.end(), leaves.begin(), leaves.end());
            return t;
        }

        // Every non-empty block of `t`.
        std::map<BlockKey, BlockData> readBlocks(const TileTable& t) {
            std::map<BlockKey, BlockData> out;
            if (t.texels.empty()) return out;
            const TileLayout L = tileTableLayout(t);
            for (int rz = 0; rz < L.regionDims.z; ++rz)
                for (int ry = 0; ry < L.regionDims.y; ++ry)
                    for (int rx = 0; rx < L.regionDims.x; ++rx) {
                        const core::ivec3 r = L.regionOrigin + core::ivec3(rx, ry, rz);
                        const uint32_t* region = &t.texels[(L.regionMasks + size_t(L.regionIndex(r))) * 4];
                        for (uint32_t zb = 0; zb < 64; ++zb) {
                            if (!maskBit(region[0], region[1], zb)) continue;
                            const core::ivec3 b = r * 4 + reverseZOrderIndex(zb);
                            BlockData d = readBlock(t, L, b);
                            if (!d.empty) out.emplace(keyOf(b), std::move(d));
                        }
                    }
            return out;
        }
    }

    int64_t TileLayout::regionIndex(core::ivec3 region) const { return boxIndex(region, regionOrigin, regionDims); }
    int64_t TileLayout::blockIndex(core::ivec3 block) const { return boxIndex(block, blockOrigin, blockDims); }

    TileLayout tileTableLayout(const TileTable& t) {
        if (t.texels.size() < 4) return TileLayout{};
        return layoutFor(unpack3(t.texels[0]), unpack3(t.texels[1]) + core::ivec3(1),
                         unpack3(t.texels[2]), unpack3(t.texels[3]) + core::ivec3(1));
    }

    TileTable buildTileTable(const std::vector<uint32_t>& tree64, const std::vector<uint8_t>& materialIDs,
                             uint32_t resolution, uint32_t* strayLeaves) {
        if (strayLeaves) *strayLeaves = 0;
        const size_t nodeCount = tree64.size() / 3;
        if (nodeCount == 0 || resolution < 4 || (tree64[0] | tree64[1]) == 0) {
            TileTable empty;
            empty.resolution = resolution;
            return empty;
        }

        // The tree walked with an explicit stack, a node's position recovered from the path to it. A 16^3
        // node's children are its block's leaves, in Z-order, so each block is encoded where it is met --
        // from its leaves, as encodeBlock would encode their voxels, so a built table and an edited one
        // encode the same voxels the same way.
        std::map<BlockKey, BlockData> blocks;
        std::vector<Brick> bricks;
        auto addBlock = [&](core::ivec3 block) {
            BlockData d = encodeLeaves(bricks, materialIDs);
            if (!d.empty) blocks.emplace(keyOf(block), std::move(d));
            bricks.clear();
        };
        struct Pending { size_t node; core::ivec3 origin; uint32_t size; };
        std::vector<Pending> stack;
        if (resolution == 4) {
            if (tree64IsLeaf(tree64[2])) {
                bricks.push_back({0, tree64[0], tree64[1], tree64[2]});
                addBlock(core::ivec3(0));
            } else if (strayLeaves) {
                ++*strayLeaves;
            }
        } else {
            stack.push_back({0, core::ivec3(0), resolution});
        }
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
            const uint32_t childSize = at.size / 4;
            uint32_t rank = 0;
            for (uint32_t z = 0; z < 64; ++z) {
                if (!maskBit(high, low, z)) continue;
                const size_t child = firstChild + rank++;
                if (childSize == 4) {
                    if (child < nodeCount) bricks.push_back({z, tree64[child * 3], tree64[child * 3 + 1], tree64[child * 3 + 2]});
                } else {
                    stack.push_back({child, at.origin + reverseZOrderIndex(z) * int(childSize), childSize});
                }
            }
            if (childSize == 4) addBlock(at.origin / 16);
        }
        return emitTable(resolution, blocks);
    }

    namespace {
        // Block `b`'s new encoding `n` (its old one `o`) written into `t` in place: its region bits, its
        // solid byte, its block entry, its run of materials and its leaves replaced, and every offset
        // behind them shifted -- the bytes emitTable would lay out for the same blocks, without decoding
        // the rest. `b` must be inside the block box, and the box must not change.
        void spliceBlock(TileTable& t, const TileLayout& L, core::ivec3 b, const BlockData& o, const BlockData& n) {
            const size_t rl = size_t(L.regionIndex(b / 4)), bl = size_t(L.blockIndex(b));
            const uint32_t zb = childZOrder(b % 4);
            uint32_t* const words = t.texels.data();
            auto regionStart = [&](size_t r) -> uint32_t& { return words[L.regionStarts * 4 + r]; };
            auto blockStart = [&](size_t i) { return &words[L.blockStarts * 4 + i * 2]; };
            auto regionMask = [&](size_t r) { return &words[(L.regionMasks + r) * 4]; };
            // Whether block z of region r has a run (leaves or full-brick bytes): occupied, not a solid tile.
            auto hasRun = [&](size_t r, uint32_t z) {
                const uint32_t* m = regionMask(r);
                return maskBit(m[0], m[1], z) && !maskBit(m[2], m[3], z);
            };
            const bool oRun = !o.empty && !o.solid, nRun = !n.empty && !n.solid;
            const size_t leafCount = t.texels.size() / 4 - t.leafStart;

            // Where its solid byte goes, and where its run (materials, leaves) is or would be: before the
            // next run in the layout's order, or at the end of its region's materials / of the leaves.
            const uint32_t* region = regionMask(rl);
            const uint32_t pSolid = regionStart(rl) + rankBefore(region[2], region[3], zb);
            uint32_t pRun = 0, lRun = 0;
            if (oRun) {
                pRun = blockStart(bl)[1];
                lRun = blockStart(bl)[0];
            } else {
                bool materialFound = false, leafFound = false;
                for (size_t r = rl; r < L.regionCount && !leafFound; ++r) {
                    const core::ivec3 rc = L.regionOrigin + core::ivec3(int(r % size_t(L.regionDims.x)),
                                                                        int(r / size_t(L.regionDims.x) % size_t(L.regionDims.y)),
                                                                        int(r / (size_t(L.regionDims.x) * size_t(L.regionDims.y))));
                    for (uint32_t z = (r == rl ? zb + 1 : 0); z < 64; ++z) {
                        const core::ivec3 c = rc * 4 + reverseZOrderIndex(z);
                        if (!hasRun(r, z)) continue;
                        const uint32_t* st = blockStart(size_t(L.blockIndex(c)));
                        if (r == rl) { pRun = st[1]; materialFound = true; }
                        lRun = st[0];
                        leafFound = true;
                        break;
                    }
                    if (r == rl && !materialFound) {
                        pRun = rl + 1 < L.regionCount ? regionStart(rl + 1) : uint32_t(t.materials.size());
                        materialFound = true;
                    }
                }
                if (!leafFound) lRun = uint32_t(leafCount);
            }
            const uint32_t oRunLength = oRun ? uint32_t(o.fullMaterials.size() + o.leafMaterials.size()) : 0u;
            const uint32_t oLeaves = oRun ? uint32_t(o.leaves.size() / 4) : 0u;
            const uint32_t nLeaves = nRun ? uint32_t(n.leaves.size() / 4) : 0u;
            const int64_t dSolid = int64_t(n.solid) - int64_t(o.solid);
            const int64_t dRun = int64_t(nRun ? n.fullMaterials.size() + n.leafMaterials.size() : 0u) - int64_t(oRunLength);
            const int64_t dLeaves = int64_t(nLeaves) - int64_t(oLeaves);

            // A material index that was at or past the solid byte's place moves by it; past the run's
            // place, by the run's change too.
            auto shifted = [&](uint32_t m) -> uint32_t {
                if (m >= pRun) return uint32_t(int64_t(m) + dSolid + dRun);
                if (m >= pSolid) return uint32_t(int64_t(m) + dSolid);
                return m;
            };
            if (dSolid != 0 || dRun != 0) {
                for (size_t k = 0; k < leafCount; ++k) {
                    if (k >= lRun && k < size_t(lRun) + oLeaves) continue;
                    uint32_t& d3 = words[(t.leafStart + k) * 4 + 2];
                    d3 = encodeLeafNode(shifted(tree64LeafMatOffset(d3)), tree64LeafIsUniform(d3));
                }
                for (size_t r = rl + 1; r < L.regionCount; ++r) regionStart(r) = uint32_t(int64_t(regionStart(r)) + dSolid + dRun);
            }
            for (size_t i = 0; i < L.blockCount; ++i) {
                if (i == bl) continue;
                const core::ivec3 c = L.blockOrigin + core::ivec3(int(i % size_t(L.blockDims.x)),
                                                                  int(i / size_t(L.blockDims.x) % size_t(L.blockDims.y)),
                                                                  int(i / (size_t(L.blockDims.x) * size_t(L.blockDims.y))));
                const size_t r = size_t(L.regionIndex(c / 4));
                const uint32_t z = childZOrder(c % 4);
                if (!hasRun(r, z)) continue;
                uint32_t* st = blockStart(i);
                st[1] = shifted(st[1]);
                if (r > rl || (r == rl && z > zb)) st[0] = uint32_t(int64_t(st[0]) + dLeaves);
            }

            // The arrays: the run (the later place) first, then the solid byte before it.
            std::vector<uint8_t> run;
            if (nRun) {
                run = n.fullMaterials;
                run.insert(run.end(), n.leafMaterials.begin(), n.leafMaterials.end());
            }
            t.materials.erase(t.materials.begin() + pRun, t.materials.begin() + pRun + oRunLength);
            t.materials.insert(t.materials.begin() + pRun, run.begin(), run.end());
            if (o.solid && n.solid) t.materials[pSolid] = n.solidSlot;
            else if (o.solid) t.materials.erase(t.materials.begin() + pSolid);
            else if (n.solid) t.materials.insert(t.materials.begin() + pSolid, n.solidSlot);

            const uint32_t runAt = uint32_t(int64_t(pRun) + dSolid);
            std::vector<uint32_t> leaves;
            if (nRun) {
                leaves.reserve(n.leaves.size());
                const uint32_t base = runAt + uint32_t(n.fullMaterials.size());
                for (size_t k = 0; k < n.leaves.size(); k += 4) {
                    const uint32_t d3 = n.leaves[k + 2];
                    leaves.insert(leaves.end(), {n.leaves[k], n.leaves[k + 1], encodeLeafNode(base + tree64LeafMatOffset(d3), tree64LeafIsUniform(d3)), 0u});
                }
            }
            const size_t leafAt = (size_t(t.leafStart) + lRun) * 4;
            t.texels.erase(t.texels.begin() + leafAt, t.texels.begin() + leafAt + size_t(oLeaves) * 4);
            t.texels.insert(t.texels.begin() + leafAt, leaves.begin(), leaves.end());

            // Its own entries (the texels may have moved).
            uint32_t* m = &t.texels[(L.regionMasks + rl) * 4];
            const uint64_t bit = 1ull << (63u - zb);
            auto setMask = [&](uint32_t& high, uint32_t& low, bool on) {
                uint64_t v = (uint64_t(high) << 32) | low;
                v = on ? (v | bit) : (v & ~bit);
                high = uint32_t(v >> 32);
                low = uint32_t(v);
            };
            setMask(m[0], m[1], !n.empty);
            setMask(m[2], m[3], n.solid);
            uint32_t* block = &t.texels[(L.blockMasks + bl) * 4];
            uint32_t* st = &t.texels[L.blockStarts * 4 + bl * 2];
            if (nRun) {
                block[0] = n.occupied[0]; block[1] = n.occupied[1]; block[2] = n.full[0]; block[3] = n.full[1];
                st[0] = lRun;
                st[1] = runAt;
            } else {
                block[0] = block[1] = block[2] = block[3] = 0;
                st[0] = st[1] = 0;
            }
            t.fullBlocks = uint32_t(int64_t(t.fullBlocks) + dSolid);
            t.fullBricks = uint32_t(int64_t(t.fullBricks) + int64_t(nRun ? n.fullMaterials.size() : 0u) - int64_t(oRun ? o.fullMaterials.size() : 0u));
            t.partialBricks = uint32_t(int64_t(t.partialBricks) + dLeaves);
        }
    }

    void applyTileEdits(TileTable& t, const std::vector<TileEdit>& edits) {
        const uint32_t resolution = t.resolution;
        if (edits.empty() || resolution < 4) return;

        // The touched blocks, decoded, edited and re-encoded.
        const TileLayout L = tileTableLayout(t);
        std::map<BlockKey, Cells> touched;
        std::map<BlockKey, BlockData> before;
        for (const TileEdit& e : edits) {
            const core::ivec3 v = e.voxel;
            if (v.x < 0 || v.y < 0 || v.z < 0 || v.x >= int(resolution) || v.y >= int(resolution) || v.z >= int(resolution)) continue;
            const BlockKey key = keyOf(v / 16);
            auto [it, fresh] = touched.try_emplace(key);
            if (fresh) {
                BlockData old = readBlock(t, L, v / 16);
                expandBlock(old, it->second);
                before.emplace(key, std::move(old));
            }
            it->second[cellIndex(v % 16)] = e.add ? uint16_t(e.slot + 1) : uint16_t(0);
        }
        if (touched.empty()) return;
        std::map<BlockKey, BlockData> after;
        for (const auto& [key, cells] : touched) after.emplace(key, encodeBlock(cells));

        // In place, unless the box would change -- a block filled outside it, or one emptied on its edge
        // (the box might shrink) -- or so many blocks are touched that one repack is cheaper.
        bool inPlace = !t.texels.empty() && touched.size() <= 32;
        const core::ivec3 lo = L.blockOrigin, hi = L.blockOrigin + L.blockDims - core::ivec3(1);
        for (const auto& [key, d] : after) {
            if (!inPlace) break;
            const core::ivec3 b = blockOf(key);
            const bool inside = L.blockIndex(b) >= 0;
            if (!d.empty && !inside) inPlace = false;
            else if (d.empty && !before.at(key).empty &&
                     (b.x == lo.x || b.y == lo.y || b.z == lo.z || b.x == hi.x || b.y == hi.y || b.z == hi.z)) inPlace = false;
        }
        if (inPlace) {
            for (const auto& [key, d] : after) {
                const BlockData& o = before.at(key);
                if (o.empty && d.empty) continue;
                spliceBlock(t, L, blockOf(key), o, d);
            }
            return;
        }

        // A repack: every block the table holds, the touched ones replaced.
        std::map<BlockKey, BlockData> blocks = readBlocks(t);
        for (auto& [key, d] : after) {
            if (d.empty) blocks.erase(key);
            else blocks[key] = std::move(d);
        }
        t = emitTable(resolution, blocks);   // edited down to nothing: empty, as an empty tree builds
    }

    TileTable downsampleTileTable(const TileTable& t, uint32_t lodSteps) {
        const uint32_t lod = clampRenderLOD(t.resolution, lodSteps);
        if (lod == 0) return t;
        const uint32_t coarseResolution = resolutionAtLOD(t.resolution, lod);
        if (t.texels.empty()) {
            TileTable empty;
            empty.resolution = coarseResolution;
            return empty;
        }

        // A fine brick (4^3) is the smallest thing a coarse voxel covers -- one step makes each brick a
        // voxel -- and the first voxel of a brick in Z-order is its first material byte (a tile's byte, a
        // leaf's first). So each coarse voxel takes the brick it covers that comes first in Z-order, by the
        // brick's position inside it (nested Z-orders are one Z-order over the cube).
        struct Coarse {
            Cells cells{};
            std::array<uint32_t, 4096> first;
        };
        std::map<BlockKey, Coarse> coarse;
        const int span = 1 << (2 * (lod - 1));   // fine bricks per coarse voxel, per axis
        Coarse* at = nullptr;   // the last coarse block visited: a fine block's bricks all land in one
        core::ivec3 atBlock(-1);
        auto visitBrick = [&](core::ivec3 brick, uint8_t slot) {
            const core::ivec3 v = brick / span;
            const uint32_t order = computeLocalZOrder(brick - v * span);
            if (!at || v / 16 != atBlock) {
                atBlock = v / 16;
                auto [it, fresh] = coarse.try_emplace(keyOf(atBlock));
                if (fresh) it->second.first.fill(~0u);
                at = &it->second;
            }
            const size_t i = cellIndex(v % 16);
            if (order >= at->first[i]) return;
            at->first[i] = order;
            at->cells[i] = uint16_t(slot + 1);
        };

        // From two steps up a whole block lies inside one coarse voxel, so only its first brick can win.
        const bool wholeBlocks = span >= 4;
        const TileLayout L = tileTableLayout(t);
        auto slotAt = [&](size_t i) { return i < t.materials.size() ? t.materials[i] : uint8_t(0); };
        for (size_t rl = 0; rl < L.regionCount; ++rl) {
            const core::ivec3 r = L.regionOrigin + core::ivec3(int(rl % size_t(L.regionDims.x)),
                                                               int(rl / size_t(L.regionDims.x) % size_t(L.regionDims.y)),
                                                               int(rl / (size_t(L.regionDims.x) * size_t(L.regionDims.y))));
            const uint32_t* region = &t.texels[(L.regionMasks + rl) * 4];
            uint32_t fullBlock = t.texels[L.regionStarts * 4 + rl];
            for (uint32_t zb = 0; zb < 64; ++zb) {
                if (!maskBit(region[0], region[1], zb)) continue;
                const core::ivec3 b = r * 4 + reverseZOrderIndex(zb);
                if (maskBit(region[2], region[3], zb)) {   // a solid tile: every brick its one slot
                    const uint8_t slot = slotAt(fullBlock++);
                    for (uint32_t zr = 0; zr < (wholeBlocks ? 1u : 64u); ++zr) visitBrick(b * 4 + reverseZOrderIndex(zr), slot);
                    continue;
                }
                const int64_t bl = L.blockIndex(b);
                if (bl < 0) continue;
                const uint32_t* block = &t.texels[(L.blockMasks + size_t(bl)) * 4];
                const uint32_t* start = &t.texels[L.blockStarts * 4 + size_t(bl) * 2];
                uint32_t leaf = t.leafStart + start[0], fullBrick = start[1];
                for (uint32_t zr = 0; zr < 64; ++zr) {
                    if (!maskBit(block[0], block[1], zr)) continue;
                    const size_t first = maskBit(block[2], block[3], zr) ? fullBrick++
                                                                         : tree64LeafMatOffset(t.texels[size_t(leaf++) * 4 + 2]);
                    visitBrick(b * 4 + reverseZOrderIndex(zr), slotAt(first));
                    if (wholeBlocks) break;
                }
            }
        }

        std::map<BlockKey, BlockData> blocks;
        for (const auto& [key, c] : coarse) {
            BlockData d = encodeBlock(c.cells);
            if (!d.empty) blocks.emplace(key, std::move(d));
        }
        return emitTable(coarseResolution, blocks);
    }

    bool tileTableWellFormed(const TileTable& t) {
        if (t.texels.empty()) return t.materials.empty();
        if (t.resolution < 4 || t.resolution > (1u << 14) || (t.resolution & (t.resolution - 1)) != 0) return false;
        if (t.texels.size() % 4 != 0 || t.texels.size() < 4) return false;
        const TileLayout L = tileTableLayout(t);
        // The boxes inside the chunk's grids, the block box inside the region box.
        const core::ivec3 regionGrid(int(std::max(t.resolution / 64u, 1u))), blockGrid(int(std::max(t.resolution / 16u, 1u)));
        if (glm::any(glm::greaterThan(L.regionOrigin + L.regionDims, regionGrid)) ||
            glm::any(glm::greaterThan(L.blockOrigin + L.blockDims, blockGrid)) ||
            glm::any(glm::lessThan(L.blockOrigin, L.regionOrigin * 4)) ||
            glm::any(glm::greaterThan(L.blockOrigin + L.blockDims, (L.regionOrigin + L.regionDims) * 4)))
            return false;
        if (t.leafStart != L.leafStart || t.texels.size() / 4 < t.leafStart) return false;
        const uint64_t leaves = t.texels.size() / 4 - t.leafStart, materials = t.materials.size();
        auto pop = [](uint32_t a, uint32_t b) { return uint64_t(std::popcount(a) + std::popcount(b)); };
        for (int rz = 0; rz < L.regionDims.z; ++rz)
            for (int ry = 0; ry < L.regionDims.y; ++ry)
                for (int rx = 0; rx < L.regionDims.x; ++rx) {
                    const core::ivec3 r = L.regionOrigin + core::ivec3(rx, ry, rz);
                    const size_t rl = size_t(L.regionIndex(r));
                    const uint32_t* region = &t.texels[(L.regionMasks + rl) * 4];
                    if ((region[2] & ~region[0]) || (region[3] & ~region[1])) return false;   // full within occupied
                    if (uint64_t(t.texels[L.regionStarts * 4 + rl]) + pop(region[2], region[3]) > materials) return false;
                    for (uint32_t zb = 0; zb < 64; ++zb)   // an occupied block that is not a tile lies in the block box
                        if (maskBit(region[0], region[1], zb) && !maskBit(region[2], region[3], zb) &&
                            L.blockIndex(r * 4 + reverseZOrderIndex(zb)) < 0)
                            return false;
                }
        for (size_t bl = 0; bl < L.blockCount; ++bl) {
            const uint32_t* block = &t.texels[(L.blockMasks + bl) * 4];
            if ((block[0] | block[1]) == 0) continue;
            if ((block[2] & ~block[0]) || (block[3] & ~block[1])) return false;
            const uint32_t* start = &t.texels[L.blockStarts * 4 + bl * 2];
            if (uint64_t(start[1]) + pop(block[2], block[3]) > materials) return false;
            if (uint64_t(start[0]) + pop(block[0] & ~block[2], block[1] & ~block[3]) > leaves) return false;
        }
        for (uint64_t k = 0; k < leaves; ++k) {
            const uint32_t* l = &t.texels[(t.leafStart + k) * 4];
            if (!tree64IsLeaf(l[2])) return false;
            const uint64_t count = tree64LeafIsUniform(l[2]) ? 1 : pop(l[0], l[1]);
            if (uint64_t(tree64LeafMatOffset(l[2])) + count > materials) return false;
        }
        return true;
    }

    void tileTableRecount(TileTable& t) {
        t.fullBlocks = t.fullBricks = t.partialBricks = 0;
        if (t.texels.empty()) return;
        const TileLayout L = tileTableLayout(t);
        for (size_t rl = 0; rl < L.regionCount; ++rl) {
            const uint32_t* region = &t.texels[(L.regionMasks + rl) * 4];
            t.fullBlocks += uint32_t(std::popcount(region[2]) + std::popcount(region[3]));
        }
        for (size_t bl = 0; bl < L.blockCount; ++bl) {
            const uint32_t* block = &t.texels[(L.blockMasks + bl) * 4];
            t.fullBricks += uint32_t(std::popcount(block[2]) + std::popcount(block[3]));
        }
        t.partialBricks = uint32_t(t.texels.size() / 4 - std::min<size_t>(t.texels.size() / 4, t.leafStart));
    }

    uint64_t tileTableVoxelCount(const TileTable& t) {
        if (t.texels.empty()) return 0;
        uint64_t total = uint64_t(t.fullBlocks) * 4096 + uint64_t(t.fullBricks) * 64;
        for (size_t i = size_t(t.leafStart) * 4; i + 1 < t.texels.size(); i += 4)
            total += uint64_t(std::popcount(t.texels[i]) + std::popcount(t.texels[i + 1]));
        return total;
    }

    bool tileTableBounds(const TileTable& t, core::ivec3& low, core::ivec3& high) {
        bool any = false;
        low = core::ivec3(0);
        high = core::ivec3(-1);
        forEachTileVoxel(t, [&](core::ivec3 p, uint8_t) {
            low = any ? glm::min(low, p) : p;
            high = any ? glm::max(high, p) : p;
            any = true;
        });
        return any;
    }

    bool tileTableVoxel(const TileTable& t, const core::ivec3& v, uint8_t* slot) {
        const uint32_t resolution = t.resolution;
        if (t.texels.empty() || v.x < 0 || v.y < 0 || v.z < 0 ||
            v.x >= int(resolution) || v.y >= int(resolution) || v.z >= int(resolution)) return false;
        const TileLayout L = tileTableLayout(t);
        const int64_t rl = L.regionIndex(v / 64);
        if (rl < 0) return false;
        const uint32_t* region = &t.texels[(L.regionMasks + size_t(rl)) * 4];
        const uint32_t zBlock = childZOrder((v / 16) % 4);
        if (!maskBit(region[0], region[1], zBlock)) return false;
        auto material = [&](size_t i) { if (slot) *slot = i < t.materials.size() ? t.materials[i] : 0; return true; };
        if (maskBit(region[2], region[3], zBlock))
            return material(t.texels[L.regionStarts * 4 + size_t(rl)] + rankBefore(region[2], region[3], zBlock));
        const int64_t bl = L.blockIndex(v / 16);
        if (bl < 0) return false;
        const uint32_t* block = &t.texels[(L.blockMasks + size_t(bl)) * 4];
        const uint32_t* start = &t.texels[L.blockStarts * 4 + size_t(bl) * 2];
        const uint32_t zBrick = childZOrder((v / 4) % 4);
        if (!maskBit(block[0], block[1], zBrick)) return false;
        if (maskBit(block[2], block[3], zBrick)) return material(start[1] + rankBefore(block[2], block[3], zBrick));
        const size_t leaf = size_t(t.leafStart) + start[0] + rankBefore(block[0] & ~block[2], block[1] & ~block[3], zBrick);
        if (leaf * 4 + 2 >= t.texels.size()) return false;
        const uint32_t* l = &t.texels[leaf * 4];
        const uint32_t z = childZOrder(v % 4);
        if (!maskBit(l[0], l[1], z)) return false;
        const uint32_t offset = tree64LeafMatOffset(l[2]);
        return material(tree64LeafIsUniform(l[2]) ? offset : offset + rankBefore(l[0], l[1], z));
    }
}
