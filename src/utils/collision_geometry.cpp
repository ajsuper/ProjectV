#include "utils/collision_geometry.h"

#include <algorithm>

#include "core/log.h"
#include "utils/loose_bvh.h"
#include "utils/voxel_math.h"

namespace projv::utils {
    namespace {
        // The merge's working copy: bit 0 solid, bit 1 claimed by a piece already.
        // With materials, `material` holds each cell's physics material (SlotMaterials), and a box
        // only takes cells of its seed's.
        struct Occupancy : VoxelOccupancy {
            std::vector<uint16_t> material;
            bool free(core::ivec3 p) const { return cells[index(p)] == 1; }   // solid and unclaimed
            uint16_t materialAt(core::ivec3 p) const { return material.empty() ? 0 : material[index(p)]; }
            bool free(core::ivec3 p, uint16_t of) const { return free(p) && materialAt(p) == of; }
        };

        constexpr uint8_t CLAIMED = 2;

        // Walks the tree the way blobContentBounds does -- positions are recovered from the descent
        // path, a child's Z-order within its parent being its bit index in the parent's mask -- and
        // marks every solid voxel. False if the tree points outside itself or outside the bounds it
        // was measured to have.
        //
        // With `materials`, each voxel's palette slot is also read (a uniform leaf stores one byte for
        // all its voxels, any other one per voxel in mask order) and its material written to
        // `material`. A slot byte the blob does not have is material 0.
        bool decode(const std::vector<uint32_t>& geometry, VoxelOccupancy& occupancy, uint64_t& solid,
                    const std::vector<uint8_t>* materialIDs = nullptr, const SlotMaterials* materials = nullptr,
                    std::vector<uint16_t>* material = nullptr) {
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
                    const size_t offset = tree64LeafMatOffset(data3);
                    const bool uniform = tree64LeafIsUniform(data3);
                    uint32_t rank = 0;
                    for (uint32_t child = 0; child < 64; child++) {
                        if ((mask & (1ull << (63 - child))) == 0) continue;   // bit 63 is Z-order 0
                        core::ivec3 p = reverseZOrderIndex(current.cellZOrder * 64 + child);
                        if (glm::any(glm::lessThan(p, occupancy.origin)) || glm::any(glm::greaterThan(p, high)))
                            return false;
                        size_t at = occupancy.index(p - occupancy.origin);
                        uint8_t& cell = occupancy.cells[at];
                        if (!cell) { cell = 1; solid++; }
                        if (material) {
                            size_t byte = offset + (uniform ? 0 : rank);
                            (*material)[at] = byte < materialIDs->size() ? (*materials)[(*materialIDs)[byte]] : 0;
                        }
                        rank++;
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
                        const uint16_t material = o.materialAt({x, y, z});
                        int x1 = x;
                        while (x1 + 1 < d.x && o.free({x1 + 1, y, z}, material)) x1++;
                        auto rowFree = [&](int yy, int zz) {
                            for (int xx = x; xx <= x1; xx++)
                                if (!o.free({xx, yy, zz}, material)) return false;
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
                        piece.material = material;
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

    namespace {
        DecodeResult decodeInto(const GeometryBlob& blob, uint32_t resolution, uint64_t maxCells, VoxelOccupancy& out,
                                const SlotMaterials* materials, std::vector<uint16_t>* material);
    }

    DecodeResult decodeOccupancy(const GeometryBlob& blob, uint32_t resolution, uint64_t maxCells, VoxelOccupancy& out) {
        return decodeInto(blob, resolution, maxCells, out, nullptr, nullptr);
    }

    namespace {
    DecodeResult decodeInto(const GeometryBlob& blob, uint32_t resolution, uint64_t maxCells, VoxelOccupancy& out,
                            const SlotMaterials* materials, std::vector<uint16_t>* material) {
        core::ivec3 low, high;
        out = VoxelOccupancy{};
        if (!blobContentBounds(blob, low, high)) return DecodeResult::Empty;
        if (resolution > 0) {
            core::ivec3 cube(int(resolution) - 1);
            if (glm::any(glm::lessThan(low, core::ivec3(0))) || glm::any(glm::greaterThan(high, cube)))
                return DecodeResult::Malformed;
        }
        core::ivec3 dims = high - low + core::ivec3(1);
        uint64_t cellCount = uint64_t(dims.x) * uint64_t(dims.y) * uint64_t(dims.z);
        out.origin = low;
        out.dims = dims;
        if (cellCount > maxCells) return DecodeResult::TooLarge;
        out.cells.assign(size_t(cellCount), 0);
        if (materials) material->assign(size_t(cellCount), 0);
        if (!decode(blob.geometry, out, out.solid, &blob.materialIDs, materials, materials ? material : nullptr))
            return DecodeResult::Malformed;
        return DecodeResult::Ok;
    }
    }

    uint64_t CollisionParams::key() const {
        // Not a hash that must resist anything; it only has to change when a parameter does.
        uint64_t k = uint64_t(representation);
        k = k * 1000003ull ^ uint64_t(pieceBudget);
        k = k * 1000003ull ^ maxDecodedCells;
        return k;
    }

    CollisionPieces buildCollisionPieces(const GeometryBlob& blob, uint32_t resolution, const CollisionParams& params,
                                         const SlotMaterials* materials) {
        CollisionPieces result;
        core::ivec3 low, high;
        if (!blobContentBounds(blob, low, high)) return result;   // no voxels: nothing collides
        result.contentMin = low;
        result.contentMax = high;

        Occupancy occupancy;
        bool anyMaterial = false;
        if (materials)
            for (uint16_t m : *materials) anyMaterial = anyMaterial || m != 0;
        switch (decodeInto(blob, resolution, params.maxDecodedCells, occupancy, anyMaterial ? materials : nullptr,
                           &occupancy.material)) {
            case DecodeResult::Empty: return result;
            case DecodeResult::Ok: break;
            case DecodeResult::TooLarge: {
                core::ivec3 dims = high - low + core::ivec3(1);
                core::warn("collision: content box {}x{}x{} exceeds maxDecodedCells; colliding as the box",
                           dims.x, dims.y, dims.z);
                return contentBox(low, high, CollisionFallback::TooLarge);
            }
            case DecodeResult::Malformed: {
                // A tree that claims voxels outside its own cube, or points outside itself, is
                // corrupt: collide as the part of its bounds inside the cube rather than trust it.
                core::ivec3 cube(int(std::max(resolution, 1u)) - 1);
                core::warn("collision: blob geometry is malformed; colliding as its (clamped) content box");
                return contentBox(glm::clamp(low, core::ivec3(0), cube), glm::clamp(high, core::ivec3(0), cube),
                                  CollisionFallback::Malformed);
            }
        }
        result.solidVoxels = occupancy.solid;
        core::ivec3 dims = occupancy.dims;

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
