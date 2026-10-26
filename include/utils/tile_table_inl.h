// forEachTileVoxel, included by utils/tile_table.h.
#ifndef PROJECTV_TILE_TABLE_INL_H
#define PROJECTV_TILE_TABLE_INL_H

#include <algorithm>
#include <bit>

#include "utils/voxel_math.h"

namespace projv::utils {
    namespace tile_detail {
        template <typename Visit>
        void visitBrick(core::ivec3 origin, uint64_t mask, bool uniform, uint32_t offset,
                        const std::vector<uint8_t>& materials, Visit& visit) {
            uint32_t cursor = offset;
            for (uint32_t z = 0; z < 64; ++z) {
                if ((mask & (1ull << (63u - z))) == 0) continue;
                const uint32_t i = uniform ? offset : cursor++;
                visit(origin + reverseZOrderIndex(z), i < materials.size() ? materials[i] : uint8_t(0));
            }
        }
    }

    template <typename Visit>
    void forEachTileVoxel(const TileTable& t, Visit&& visit) {
        if (t.texels.empty()) return;
        const TileLayout L = tileTableLayout(t);
        auto mask64 = [](uint32_t high, uint32_t low) { return (uint64_t(high) << 32) | low; };
        for (int rz = 0; rz < L.regionDims.z; ++rz)
            for (int ry = 0; ry < L.regionDims.y; ++ry)
                for (int rx = 0; rx < L.regionDims.x; ++rx) {
                    const core::ivec3 r = L.regionOrigin + core::ivec3(rx, ry, rz);
                    const size_t rl = size_t(L.regionIndex(r));
                    const uint32_t* region = &t.texels[(L.regionMasks + rl) * 4];
                    const uint64_t occupied = mask64(region[0], region[1]), full = mask64(region[2], region[3]);
                    if (occupied == 0) continue;
                    uint32_t fullBlockMaterial = t.texels[L.regionStarts * 4 + rl];
                    for (uint32_t zb = 0; zb < 64; ++zb) {
                        const uint64_t bit = 1ull << (63u - zb);
                        if ((occupied & bit) == 0) continue;
                        const core::ivec3 b = r * 4 + reverseZOrderIndex(zb);
                        if (full & bit) {   // a solid 16^3 tile: every brick full, one material
                            for (uint32_t zr = 0; zr < 64; ++zr)
                                tile_detail::visitBrick(b * 16 + reverseZOrderIndex(zr) * 4, ~0ull, true, fullBlockMaterial,
                                                        t.materials, visit);
                            ++fullBlockMaterial;
                            continue;
                        }
                        const int64_t bl = L.blockIndex(b);
                        if (bl < 0) continue;   // malformed: tileTableWellFormed refuses it
                        const uint32_t* block = &t.texels[(L.blockMasks + size_t(bl)) * 4];
                        const uint32_t* start = &t.texels[L.blockStarts * 4 + size_t(bl) * 2];
                        const uint64_t bricks = mask64(block[0], block[1]), fullBricks = mask64(block[2], block[3]);
                        uint32_t leaf = t.leafStart + start[0], fullMaterial = start[1];
                        for (uint32_t zr = 0; zr < 64; ++zr) {
                            const uint64_t brickBit = 1ull << (63u - zr);
                            if ((bricks & brickBit) == 0) continue;
                            const core::ivec3 origin = b * 16 + reverseZOrderIndex(zr) * 4;
                            if (fullBricks & brickBit) {
                                tile_detail::visitBrick(origin, ~0ull, true, fullMaterial++, t.materials, visit);
                            } else {
                                const uint32_t* l = &t.texels[size_t(leaf++) * 4];
                                tile_detail::visitBrick(origin, mask64(l[0], l[1]), (l[2] & 2u) != 0, l[2] >> 2, t.materials, visit);
                            }
                        }
                    }
                }
    }

    template <typename Visit>
    void forEachTileVoxelIn(const TileTable& t, core::ivec3 low, core::ivec3 high, Visit&& visit) {
        if (t.texels.empty()) return;
        const int resolution = int(t.resolution);
        low = glm::max(low, core::ivec3(0));
        high = glm::min(high, core::ivec3(resolution - 1));
        if (glm::any(glm::greaterThan(low, high))) return;
        const TileLayout L = tileTableLayout(t);
        // Only the part of the box inside the table's block box can hold anything.
        low = glm::max(low, L.blockOrigin * 16);
        high = glm::min(high, (L.blockOrigin + L.blockDims) * 16 - core::ivec3(1));
        if (glm::any(glm::greaterThan(low, high))) return;
        auto bit = [](uint32_t high32, uint32_t low32, uint32_t z) {
            return ((((uint64_t(high32) << 32) | low32) >> (63u - z)) & 1ull) != 0;
        };
        auto rank = [](uint32_t high32, uint32_t low32, uint32_t z) -> uint32_t {
            const uint64_t mask = (uint64_t(high32) << 32) | low32;
            return z == 0 ? 0u : uint32_t(std::popcount(mask >> (64u - z)));
        };
        auto z4 = [](core::ivec3 l) { return uint32_t(computeLocalZOrder(l) & 63u); };
        auto slotAt = [&](size_t i) { return i < t.materials.size() ? t.materials[i] : uint8_t(0); };
        // Every cell of [a, b] (a cube's part inside the box), one slot.
        auto fill = [&](core::ivec3 a, core::ivec3 b, uint8_t slot) {
            for (int z = a.z; z <= b.z; ++z)
                for (int y = a.y; y <= b.y; ++y)
                    for (int x = a.x; x <= b.x; ++x) visit(core::ivec3(x, y, z), slot);
        };
        const core::ivec3 blockLow = low / 16, blockHigh = high / 16;
        for (int bz = blockLow.z; bz <= blockHigh.z; ++bz)
            for (int by = blockLow.y; by <= blockHigh.y; ++by)
                for (int bx = blockLow.x; bx <= blockHigh.x; ++bx) {
                    const core::ivec3 b{bx, by, bz};
                    const int64_t rl = L.regionIndex(b / 4);
                    if (rl < 0) continue;
                    const uint32_t* region = &t.texels[(L.regionMasks + size_t(rl)) * 4];
                    const uint32_t zb = z4(b % 4);
                    if (!bit(region[0], region[1], zb)) continue;
                    const core::ivec3 cubeLow = glm::max(low, b * 16), cubeHigh = glm::min(high, b * 16 + core::ivec3(15));
                    if (bit(region[2], region[3], zb)) {   // a solid 16^3 tile
                        fill(cubeLow, cubeHigh, slotAt(size_t(t.texels[L.regionStarts * 4 + size_t(rl)]) + rank(region[2], region[3], zb)));
                        continue;
                    }
                    const int64_t bl = L.blockIndex(b);
                    if (bl < 0) continue;
                    const uint32_t* block = &t.texels[(L.blockMasks + size_t(bl)) * 4];
                    const uint32_t* start = &t.texels[L.blockStarts * 4 + size_t(bl) * 2];
                    const uint32_t partialHigh = block[0] & ~block[2], partialLow = block[1] & ~block[3];
                    for (int rz = cubeLow.z / 4; rz <= cubeHigh.z / 4; ++rz)
                        for (int ry = cubeLow.y / 4; ry <= cubeHigh.y / 4; ++ry)
                            for (int rx = cubeLow.x / 4; rx <= cubeHigh.x / 4; ++rx) {
                                const core::ivec3 brick{rx, ry, rz};
                                const uint32_t zr = z4(brick % 4);
                                if (!bit(block[0], block[1], zr)) continue;
                                const core::ivec3 a = glm::max(cubeLow, brick * 4), c = glm::min(cubeHigh, brick * 4 + core::ivec3(3));
                                if (bit(block[2], block[3], zr)) {
                                    fill(a, c, slotAt(size_t(start[1]) + rank(block[2], block[3], zr)));
                                    continue;
                                }
                                const uint32_t* l = &t.texels[(size_t(t.leafStart) + start[0] + rank(partialHigh, partialLow, zr)) * 4];
                                const bool uniform = (l[2] & 2u) != 0;
                                const uint32_t offset = l[2] >> 2;
                                for (int z = a.z; z <= c.z; ++z)
                                    for (int y = a.y; y <= c.y; ++y)
                                        for (int x = a.x; x <= c.x; ++x) {
                                            const core::ivec3 p{x, y, z};
                                            const uint32_t zv = z4(p % 4);
                                            if (!bit(l[0], l[1], zv)) continue;
                                            visit(p, slotAt(uniform ? offset : size_t(offset) + rank(l[0], l[1], zv)));
                                        }
                            }
                }
    }
}

#endif
