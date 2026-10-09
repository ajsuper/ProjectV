#include "utils/collision_geometry.h"

#include <algorithm>

#include "core/log.h"
#include "utils/loose_bvh.h"
#include "utils/voxel_math.h"

namespace projv::utils {
    namespace {
        // The solid voxels of a blob, decoded into a dense box over its content bounds: one byte per
        // cell, so the greedy merge below can test and claim cells in O(1).
        struct Occupancy {
            core::ivec3 origin{0}, dims{0};
            std::vector<uint8_t> cells;     // bit 0: solid; bit 1: claimed by a piece already

            size_t index(core::ivec3 p) const {
                return (size_t(p.z) * size_t(dims.y) + size_t(p.y)) * size_t(dims.x) + size_t(p.x);
            }
            bool free(core::ivec3 p) const { return cells[index(p)] == 1; }   // solid and unclaimed
        };

        constexpr uint8_t SOLID = 1, CLAIMED = 2;

        // Walks the tree the way blobContentBounds does -- positions are recovered from the descent
        // path, a child's Z-order within its parent being its bit index in the parent's mask -- and
        // marks every solid voxel. False if the tree points outside itself or outside the bounds it
        // was measured to have.
        bool decode(const std::vector<uint32_t>& geometry, Occupancy& occupancy, uint64_t& solid) {
            const size_t nodeCount = geometry.size() / 3;
            struct Pending { size_t node; uint64_t cellZOrder; };
            std::vector<Pending> stack;
            if (nodeCount > 0) stack.push_back({0, 0});
            const core::ivec3 high = occupancy.origin + occupancy.dims - core::ivec3(1);
            while (!stack.empty()) {
                Pending current = stack.back();
                stack.pop_back();
                if (current.node >= nodeCount) return false;
                uint64_t mask = (uint64_t(geometry[current.node * 3]) << 32) | uint64_t(geometry[current.node * 3 + 1]);
                uint32_t data3 = geometry[current.node * 3 + 2];
                if (mask == 0) continue;
                if (tree64IsLeaf(data3)) {
                    for (uint32_t child = 0; child < 64; child++) {
                        if ((mask & (1ull << (63 - child))) == 0) continue;   // bit 63 is Z-order 0
                        core::ivec3 p = reverseZOrderIndex(current.cellZOrder * 64 + child);
                        if (glm::any(glm::lessThan(p, occupancy.origin)) || glm::any(glm::greaterThan(p, high)))
                            return false;
                        uint8_t& cell = occupancy.cells[occupancy.index(p - occupancy.origin)];
                        if (!cell) { cell = SOLID; solid++; }
                    }
                    continue;
                }
                size_t firstChild = current.node + (data3 >> 1);
                uint32_t rank = 0;
                for (uint32_t child = 0; child < 64; child++) {
                    if ((mask & (1ull << (63 - child))) == 0) continue;
                    stack.push_back({firstChild + rank, current.cellZOrder * 64 + child});
                    rank++;
                }
            }
            return true;
        }

        // Greedy box merging, in a fixed order so the result is deterministic: the first unclaimed
        // solid cell in z, y, x order seeds a box, which grows along x as far as it can, then along y
        // while every cell of the next row is free, then along z while every cell of the next slab
        // is. Its cells are claimed and the scan goes on. Boxes therefore cover the voxels exactly
        // and never overlap. False as soon as the budget is exceeded.
        bool mergeBoxes(Occupancy& o, uint32_t budget, std::vector<CollisionPiece>& out) {
            const core::ivec3 d = o.dims;
            for (int z = 0; z < d.z; z++)
                for (int y = 0; y < d.y; y++)
                    for (int x = 0; x < d.x; x++) {
                        if (!o.free({x, y, z})) continue;
                        int x1 = x;
                        while (x1 + 1 < d.x && o.free({x1 + 1, y, z})) x1++;
                        auto rowFree = [&](int yy, int zz) {
                            for (int xx = x; xx <= x1; xx++)
                                if (!o.free({xx, yy, zz})) return false;
                            return true;
                        };
                        int y1 = y;
                        while (y1 + 1 < d.y && rowFree(y1 + 1, z)) y1++;
                        auto slabFree = [&](int zz) {
                            for (int yy = y; yy <= y1; yy++)
                                if (!rowFree(yy, zz)) return false;
                            return true;
                        };
                        int z1 = z;
                        while (z1 + 1 < d.z && slabFree(z1 + 1)) z1++;

                        for (int zz = z; zz <= z1; zz++)
                            for (int yy = y; yy <= y1; yy++)
                                for (int xx = x; xx <= x1; xx++) o.cells[o.index({xx, yy, zz})] |= CLAIMED;

                        if (out.size() >= budget) return false;
                        CollisionPiece piece;
                        piece.voxelMin = o.origin + core::ivec3(x, y, z);
                        piece.voxelMax = o.origin + core::ivec3(x1, y1, z1);
                        piece.min = core::vec3(piece.voxelMin);
                        piece.max = core::vec3(piece.voxelMax + core::ivec3(1));
                        out.push_back(piece);
                    }
            return true;
        }

        CollisionPieces contentBox(core::ivec3 low, core::ivec3 high, CollisionFallback why) {
            CollisionPieces result;
            result.contentMin = low;
            result.contentMax = high;
            result.fallback = why;
            CollisionPiece piece;
            piece.voxelMin = low;
            piece.voxelMax = high;
            piece.min = core::vec3(low);
            piece.max = core::vec3(high + core::ivec3(1));
            result.pieces.push_back(piece);
            return result;
        }
    }

    uint64_t CollisionParams::key() const {
        // Not a hash that must resist anything; it only has to change when a parameter does.
        uint64_t k = uint64_t(representation);
        k = k * 1000003ull ^ uint64_t(pieceBudget);
        k = k * 1000003ull ^ maxDecodedCells;
        return k;
    }

    CollisionPieces buildCollisionPieces(const GeometryBlob& blob, uint32_t resolution, const CollisionParams& params) {
        CollisionPieces result;
        core::ivec3 low, high;
        if (!blobContentBounds(blob, low, high)) return result;   // no voxels: nothing collides
        result.contentMin = low;
        result.contentMax = high;

        // A tree that claims voxels outside its own cube is corrupt; collide as the cube's part of it
        // rather than trust it.
        if (resolution > 0) {
            core::ivec3 cube(int(resolution) - 1);
            if (glm::any(glm::lessThan(low, core::ivec3(0))) || glm::any(glm::greaterThan(high, cube))) {
                core::warn("collision: blob claims voxels outside its {}^3 cube; colliding as its clamped bounds",
                           resolution);
                return contentBox(glm::clamp(low, core::ivec3(0), cube), glm::clamp(high, core::ivec3(0), cube),
                                  CollisionFallback::Malformed);
            }
        }

        core::ivec3 dims = high - low + core::ivec3(1);
        uint64_t cellCount = uint64_t(dims.x) * uint64_t(dims.y) * uint64_t(dims.z);
        if (cellCount > params.maxDecodedCells) {
            core::warn("collision: content box {}x{}x{} exceeds maxDecodedCells; colliding as the box",
                       dims.x, dims.y, dims.z);
            return contentBox(low, high, CollisionFallback::TooLarge);
        }

        Occupancy occupancy;
        occupancy.origin = low;
        occupancy.dims = dims;
        occupancy.cells.assign(size_t(cellCount), 0);
        if (!decode(blob.geometry, occupancy, result.solidVoxels)) {
            core::warn("collision: blob geometry is malformed; colliding as its content box");
            return contentBox(low, high, CollisionFallback::Malformed);
        }

        if (!mergeBoxes(occupancy, params.pieceBudget, result.pieces)) {
            core::warn("collision: {} solid voxels need more than {} boxes; colliding as the content box "
                       "{}x{}x{}", result.solidVoxels, params.pieceBudget, dims.x, dims.y, dims.z);
            CollisionPieces box = contentBox(low, high, CollisionFallback::OverBudget);
            box.solidVoxels = result.solidVoxels;
            return box;
        }
        return result;
    }
}
