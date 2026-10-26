#include "utils/tile_collision.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

#include "utils/voxel_math.h"

namespace projv::utils {
    namespace {
        // A 4^3 cell's Z-order (the bit a mask holds it in: bit 63 - z), by its linear index x + 4y + 16z.
        const std::array<uint8_t, 64>& zOrderOf() {
            static const std::array<uint8_t, 64> table = [] {
                std::array<uint8_t, 64> t{};
                for (int i = 0; i < 64; i++)
                    t[size_t(i)] = uint8_t(computeLocalZOrder(core::ivec3(i & 3, (i >> 2) & 3, i >> 4)) & 63u);
                return t;
            }();
            return table;
        }
        int linearOf(core::ivec3 l) { return l.x + 4 * l.y + 16 * l.z; }
        uint64_t mask64(uint32_t high, uint32_t low) { return (uint64_t(high) << 32) | low; }
        bool has(uint64_t mask, uint32_t z) { return (mask >> (63u - z)) & 1ull; }
        uint32_t rankBefore(uint64_t mask, uint32_t z) { return z == 0 ? 0u : uint32_t(std::popcount(mask >> (64u - z))); }
        uint8_t slotAt(const TileTable& t, size_t i) { return i < t.materials.size() ? t.materials[i] : uint8_t(0); }

        // One 16^3 block as the table holds it.
        struct Block {
            enum Kind : uint8_t { Empty, Full, Partial } kind = Empty;
            uint8_t   slot = 0;                         // Full: its one slot
            uint64_t  bricks = 0, fullBricks = 0;       // Partial: its brick masks
            uint32_t  leafStart = 0, fullStart = 0;     // Partial: its first leaf (absolute) and full-brick material
        };
        Block blockAt(const TileTable& t, const TileLayout& L, core::ivec3 b) {
            Block out;
            const int64_t rl = L.regionIndex(core::ivec3(b.x >> 2, b.y >> 2, b.z >> 2));
            if (rl < 0) return out;
            const uint32_t* region = &t.texels[(L.regionMasks + size_t(rl)) * 4];
            const uint64_t occupied = mask64(region[0], region[1]), full = mask64(region[2], region[3]);
            const uint32_t zb = zOrderOf()[size_t(linearOf(b & 3))];
            if (!has(occupied, zb)) return out;
            if (has(full, zb)) {
                out.kind = Block::Full;
                out.slot = slotAt(t, size_t(t.texels[L.regionStarts * 4 + size_t(rl)]) + rankBefore(full, zb));
                return out;
            }
            const int64_t bl = L.blockIndex(b);
            if (bl < 0) return out;
            const uint32_t* block = &t.texels[(L.blockMasks + size_t(bl)) * 4];
            const uint32_t* start = &t.texels[L.blockStarts * 4 + size_t(bl) * 2];
            out.kind = Block::Partial;
            out.bricks = mask64(block[0], block[1]);
            out.fullBricks = mask64(block[2], block[3]);
            out.leafStart = t.leafStart + start[0];
            out.fullStart = start[1];
            return out;
        }

        // One 4^3 brick of a partial block: its voxel mask (tree convention) and their slots' place.
        struct Brick {
            uint64_t mask = 0;
            bool     uniform = true;
            uint32_t offset = 0;   // the slot byte (uniform), or the first voxel's
        };
        Brick brickAt(const TileTable& t, const Block& block, uint32_t zr) {
            Brick out;
            if (!has(block.bricks, zr)) return out;
            if (has(block.fullBricks, zr)) {
                out.mask = ~0ull;
                out.offset = block.fullStart + rankBefore(block.fullBricks, zr);
                return out;
            }
            const size_t leaf = size_t(block.leafStart) + rankBefore(block.bricks & ~block.fullBricks, zr);
            if (leaf * 4 + 2 >= t.texels.size()) return out;
            const uint32_t* l = &t.texels[leaf * 4];
            out.mask = mask64(l[0], l[1]);
            out.uniform = (l[2] & 2u) != 0;
            out.offset = l[2] >> 2;
            return out;
        }
        uint8_t brickSlot(const TileTable& t, const Brick& brick, uint32_t z) {
            return slotAt(t, brick.uniform ? brick.offset : size_t(brick.offset) + rankBefore(brick.mask, z));
        }

        // Greedy boxes over a 4^3 grid of cells, each of one slot: from the first unmerged cell (x
        // fastest), as far along x as the slot holds, then y, then z. `slots[i]` is a cell's slot,
        // or -1 for an empty one.
        template <typename Emit>
        void mergeGrid(const std::array<int16_t, 64>& slots, Emit&& emit) {
            uint64_t used = 0;
            auto free = [&](int x, int y, int z, int16_t s) {
                const int i = x + 4 * y + 16 * z;
                return slots[size_t(i)] == s && !((used >> i) & 1ull);
            };
            for (int i = 0; i < 64; i++) {
                const int16_t s = slots[size_t(i)];
                if (s < 0 || ((used >> i) & 1ull)) continue;
                const int x0 = i & 3, y0 = (i >> 2) & 3, z0 = i >> 4;
                int x1 = x0, y1 = y0, z1 = z0;
                while (x1 + 1 < 4 && free(x1 + 1, y0, z0, s)) x1++;
                for (bool grow = true; grow && y1 + 1 < 4;) {
                    for (int x = x0; x <= x1; x++) grow = grow && free(x, y1 + 1, z0, s);
                    if (grow) y1++;
                }
                for (bool grow = true; grow && z1 + 1 < 4;) {
                    for (int y = y0; y <= y1; y++)
                        for (int x = x0; x <= x1; x++) grow = grow && free(x, y, z1 + 1, s);
                    if (grow) z1++;
                }
                for (int z = z0; z <= z1; z++)
                    for (int y = y0; y <= y1; y++)
                        for (int x = x0; x <= x1; x++) used |= 1ull << (x + 4 * y + 16 * z);
                emit(core::ivec3(x0, y0, z0), core::ivec3(x1, y1, z1), uint8_t(s));
            }
        }

        bool overlaps(core::ivec3 aLow, core::ivec3 aHigh, core::ivec3 bLow, core::ivec3 bHigh) {
            return glm::all(glm::lessThanEqual(aLow, bHigh)) && glm::all(glm::lessThanEqual(bLow, aHigh));
        }
    }

    void tileBoxesIn(const TileTable& t, const TileLayout& L, core::ivec3 low, core::ivec3 high, std::vector<TileBox>& out) {
        if (t.texels.empty()) return;
        low = glm::max(low, L.blockOrigin * 16);
        high = glm::min(high, (L.blockOrigin + L.blockDims) * 16 - core::ivec3(1));
        if (glm::any(glm::greaterThan(low, high))) return;
        const std::array<uint8_t, 64>& zo = zOrderOf();
        const core::ivec3 blockLow = low >> 4, blockHigh = high >> 4;
        for (int bz = blockLow.z; bz <= blockHigh.z; ++bz)
            for (int by = blockLow.y; by <= blockHigh.y; ++by)
                for (int bx = blockLow.x; bx <= blockHigh.x; ++bx) {
                    const core::ivec3 b{bx, by, bz};
                    const Block block = blockAt(t, L, b);
                    if (block.kind == Block::Empty) continue;
                    const core::ivec3 corner = b * 16;
                    if (block.kind == Block::Full) {
                        out.push_back({corner, corner + core::ivec3(15), block.slot});
                        continue;
                    }
                    // Its full bricks, merged over the whole block (so the boxes do not depend on the range).
                    if (block.fullBricks) {
                        std::array<int16_t, 64> slots;
                        for (int i = 0; i < 64; i++) {
                            const uint32_t zr = zo[size_t(i)];
                            slots[size_t(i)] = has(block.fullBricks, zr)
                                ? int16_t(slotAt(t, size_t(block.fullStart) + rankBefore(block.fullBricks, zr))) : int16_t(-1);
                        }
                        mergeGrid(slots, [&](core::ivec3 a, core::ivec3 c, uint8_t slot) {
                            const core::ivec3 boxLow = corner + a * 4, boxHigh = corner + c * 4 + core::ivec3(3);
                            if (overlaps(boxLow, boxHigh, low, high)) out.push_back({boxLow, boxHigh, slot});
                        });
                    }
                    // Its partly filled bricks the range reaches, each merged within itself.
                    const uint64_t partial = block.bricks & ~block.fullBricks;
                    if (!partial) continue;
                    const core::ivec3 brickLow = glm::max(low - corner, core::ivec3(0)) >> 2;
                    const core::ivec3 brickHigh = glm::min(high - corner, core::ivec3(15)) >> 2;
                    for (int rz = brickLow.z; rz <= brickHigh.z; ++rz)
                        for (int ry = brickLow.y; ry <= brickHigh.y; ++ry)
                            for (int rx = brickLow.x; rx <= brickHigh.x; ++rx) {
                                const uint32_t zr = zo[size_t(linearOf({rx, ry, rz}))];
                                if (!has(partial, zr)) continue;
                                const Brick brick = brickAt(t, block, zr);
                                std::array<int16_t, 64> slots;
                                for (int i = 0; i < 64; i++) {
                                    const uint32_t z = zo[size_t(i)];
                                    slots[size_t(i)] = has(brick.mask, z) ? int16_t(brickSlot(t, brick, z)) : int16_t(-1);
                                }
                                const core::ivec3 origin = corner + core::ivec3(rx, ry, rz) * 4;
                                mergeGrid(slots, [&](core::ivec3 a, core::ivec3 c, uint8_t slot) {
                                    if (overlaps(origin + a, origin + c, low, high)) out.push_back({origin + a, origin + c, slot});
                                });
                            }
                }
    }

    void tileBlockBoxes(const TileTable& t, const TileLayout& L, std::vector<TileBox>& boxes, std::vector<uint32_t>& starts) {
        boxes.clear();
        starts.assign(L.blockCount + 1, 0);
        if (t.texels.empty()) return;
        std::array<int16_t, 16 * 16 * 16> grid;   // a slot, or -1 for empty
        auto at = [](int x, int y, int z) { return size_t(x + 16 * (y + 16 * z)); };
        for (int bz = 0; bz < L.blockDims.z; ++bz)
            for (int by = 0; by < L.blockDims.y; ++by)
                for (int bx = 0; bx < L.blockDims.x; ++bx) {
                    const core::ivec3 b = L.blockOrigin + core::ivec3(bx, by, bz);
                    const int64_t index = L.blockIndex(b);
                    starts[size_t(index)] = uint32_t(boxes.size());
                    const Block block = blockAt(t, L, b);
                    if (block.kind == Block::Empty) continue;
                    const core::ivec3 corner = b * 16;
                    if (block.kind == Block::Full) {
                        boxes.push_back({corner, corner + core::ivec3(15), block.slot});
                        continue;
                    }
                    grid.fill(-1);
                    forEachTileVoxelIn(t, corner, corner + core::ivec3(15),
                                       [&](core::ivec3 p, uint8_t slot) { grid[at(p.x - corner.x, p.y - corner.y, p.z - corner.z)] = int16_t(slot); });
                    for (int z = 0; z < 16; z++)
                        for (int y = 0; y < 16; y++)
                            for (int x = 0; x < 16; x++) {
                                const int16_t s = grid[at(x, y, z)];
                                if (s < 0) continue;
                                int x1 = x, y1 = y, z1 = z;
                                while (x1 + 1 < 16 && grid[at(x1 + 1, y, z)] == s) x1++;
                                for (bool grow = true; grow && y1 + 1 < 16;) {
                                    for (int i = x; i <= x1 && grow; i++) grow = grid[at(i, y1 + 1, z)] == s;
                                    if (grow) y1++;
                                }
                                for (bool grow = true; grow && z1 + 1 < 16;) {
                                    for (int k = y; k <= y1 && grow; k++)
                                        for (int i = x; i <= x1 && grow; i++) grow = grid[at(i, k, z1 + 1)] == s;
                                    if (grow) z1++;
                                }
                                for (int k = z; k <= z1; k++)
                                    for (int j = y; j <= y1; j++)
                                        for (int i = x; i <= x1; i++) grid[at(i, j, k)] = -1;
                                boxes.push_back({corner + core::ivec3(x, y, z), corner + core::ivec3(x1, y1, z1), uint8_t(s)});
                            }
                }
        starts[L.blockCount] = uint32_t(boxes.size());
    }

    namespace {
        // Cells of size `size` along a ray, in order: a 3D DDA over [low, high) (voxel units, multiples of
        // `size`) from parameter `t` to `tEnd`. `visit(cell, t0, t1, axis)` sees each cell entered, the
        // span the ray is in it, and the axis it came in by (-1: it starts there); true stops the march.
        template <typename Visit>
        bool march(core::vec3 o, core::vec3 d, int size, core::ivec3 low, core::ivec3 high, float t, float tEnd,
                   int axis, Visit&& visit) {
            const float s = float(size);
            const core::vec3 p = o + d * t;
            core::ivec3 cell = core::ivec3(glm::floor(p / s));
            cell = glm::clamp(cell, low / size, high / size - core::ivec3(1));
            core::ivec3 step(0);
            core::vec3 next(std::numeric_limits<float>::infinity()), delta(std::numeric_limits<float>::infinity());
            for (int a = 0; a < 3; a++) {
                if (d[a] > 0.0f) {
                    step[a] = 1;
                    next[a] = (float(cell[a] + 1) * s - o[a]) / d[a];
                    delta[a] = s / d[a];
                } else if (d[a] < 0.0f) {
                    step[a] = -1;
                    next[a] = (float(cell[a]) * s - o[a]) / d[a];
                    delta[a] = -s / d[a];
                }
            }
            const core::ivec3 cellLow = low / size, cellHigh = high / size;   // high exclusive
            while (true) {
                const float exit = std::min({next.x, next.y, next.z, tEnd});
                if (visit(cell, t, exit, axis)) return true;
                if (exit >= tEnd) return false;
                axis = next.x <= next.y ? (next.x <= next.z ? 0 : 2) : (next.y <= next.z ? 1 : 2);
                t = next[axis];
                cell[axis] += step[axis];
                next[axis] += delta[axis];
                if (cell[axis] < cellLow[axis] || cell[axis] >= cellHigh[axis]) return false;
            }
        }
    }

    bool tileTableRaycast(const TileTable& t, const TileLayout& L, core::vec3 o, core::vec3 d, float maxT, TileRayHit& hit) {
        if (t.texels.empty() || !(maxT >= 0.0f)) return false;
        // Clipped to the block box: nothing outside it is solid.
        const core::ivec3 low = L.blockOrigin * 16, high = (L.blockOrigin + L.blockDims) * 16;
        float t0 = 0.0f, t1 = maxT;
        int entry = -1;
        for (int a = 0; a < 3; a++) {
            if (d[a] == 0.0f) {
                if (o[a] < float(low[a]) || o[a] > float(high[a])) return false;
                continue;
            }
            float near = (float(low[a]) - o[a]) / d[a], far = (float(high[a]) - o[a]) / d[a];
            if (near > far) std::swap(near, far);
            if (near > t0) { t0 = near; entry = a; }
            t1 = std::min(t1, far);
        }
        if (t0 > t1) return false;
        const std::array<uint8_t, 64>& zo = zOrderOf();
        auto found = [&](core::ivec3 voxel, float at, int axis) {
            hit.t = at;
            hit.voxel = voxel;
            hit.normal = core::ivec3(0);
            if (axis >= 0) hit.normal[axis] = d[axis] > 0.0f ? -1 : 1;
            return true;
        };
        return march(o, d, 16, low, high, t0, t1, entry, [&](core::ivec3 b, float bt0, float bt1, int baxis) {
            const Block block = blockAt(t, L, b);
            if (block.kind == Block::Empty) return false;
            if (block.kind == Block::Full) {
                const core::ivec3 v = glm::clamp(core::ivec3(glm::floor(o + d * bt0)), b * 16, b * 16 + core::ivec3(15));
                return found(v, bt0, baxis);
            }
            return march(o, d, 4, b * 16, b * 16 + core::ivec3(16), bt0, bt1, baxis,
                         [&](core::ivec3 r, float rt0, float rt1, int raxis) {
                const uint32_t zr = zo[size_t(linearOf(r & 3))];
                if (!has(block.bricks, zr)) return false;
                if (has(block.fullBricks, zr)) {
                    const core::ivec3 v = glm::clamp(core::ivec3(glm::floor(o + d * rt0)), r * 4, r * 4 + core::ivec3(3));
                    return found(v, rt0, raxis);
                }
                const Brick brick = brickAt(t, block, zr);
                return march(o, d, 1, r * 4, r * 4 + core::ivec3(4), rt0, rt1, raxis,
                             [&](core::ivec3 v, float vt0, float, int vaxis) {
                    return has(brick.mask, zo[size_t(linearOf(v & 3))]) && found(v, vt0, vaxis);
                });
            });
        });
    }

    core::ivec3 tileSurfaceNormal(const TileTable& t, core::vec3 p) {
        // The boundaries nearest the point, nearest first.
        std::array<int, 3> axes{0, 1, 2};
        core::vec3 off = glm::abs(p - glm::round(p));
        std::sort(axes.begin(), axes.end(), [&](int a, int b) { return off[a] < off[b]; });
        const core::ivec3 cell = core::ivec3(glm::floor(p));
        for (int a : axes) {
            if (off[a] > 0.25f) break;   // not on a face along this axis
            core::ivec3 plus = cell, minus = cell;
            plus[a] = int(std::lround(p[a]));
            minus[a] = plus[a] - 1;
            const bool solidPlus = tileTableVoxel(t, plus), solidMinus = tileTableVoxel(t, minus);
            core::ivec3 n(0);
            if (solidMinus && !solidPlus) n[a] = 1;
            else if (solidPlus && !solidMinus) n[a] = -1;
            else continue;
            return n;
        }
        return core::ivec3(0);
    }

    int tileCellShift(uint32_t resolution, uint32_t idBits) {
        int levels = 0;   // log2 of the resolution, rounded up
        while ((1u << levels) < std::max(resolution, 1u)) levels++;
        int shift = 0;
        while (shift < levels && 3 * (levels - shift) > int(idBits)) shift++;
        return shift;
    }

    bool tileBrickSlot(const TileTable& t, core::ivec3 v, uint8_t& slot) {
        if (tileTableVoxel(t, v, &slot)) return true;
        const core::ivec3 corner = (v >> 2) << 2;
        for (int i = 0; i < 64; i++)
            if (tileTableVoxel(t, corner + core::ivec3(i & 3, (i >> 2) & 3, i >> 4), &slot)) return true;
        return false;
    }
}
