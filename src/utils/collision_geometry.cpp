#include "utils/blob_voxels.h"
#include "utils/collision_geometry.h"

#include <algorithm>
#include <cstdlib>
#include <string>

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

        // Marks every solid voxel of the blob's tile table. False if one lies outside the bounds the
        // table was measured to have.
        //
        // With `materials`, each voxel's material is written to `material` (its palette slot through
        // `materials`); with `slots`, its palette slot.
        bool decode(const TileTable& tiles, VoxelOccupancy& occupancy, uint64_t& solid,
                    const SlotMaterials* materials = nullptr, std::vector<uint16_t>* material = nullptr,
                    std::vector<uint8_t>* slots = nullptr) {
            const core::ivec3 high = occupancy.origin + occupancy.dims - core::ivec3(1);
            bool inside = true;
            forEachTileVoxel(tiles, [&](core::ivec3 p, uint8_t slot) {
                if (glm::any(glm::lessThan(p, occupancy.origin)) || glm::any(glm::greaterThan(p, high))) {
                    inside = false;
                    return;
                }
                const size_t at = occupancy.index(p - occupancy.origin);
                uint8_t& cell = occupancy.cells[at];
                if (!cell) { cell = 1; solid++; }
                if (material) (*material)[at] = (*materials)[slot];
                if (slots) (*slots)[at] = slot;
            });
            return inside;
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

    DecodeResult decodeVoxelSlots(const GeometryBlob& blob, uint32_t resolution, uint64_t maxCells, VoxelOccupancy& out,
                                  std::vector<uint8_t>& slots) {
        core::ivec3 low, high;
        out = VoxelOccupancy{};
        slots.clear();
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
        slots.assign(size_t(cellCount), 0);
        if (!decode(blobTiles(blob), out, out.solid, nullptr, nullptr, &slots)) return DecodeResult::Malformed;
        return DecodeResult::Ok;
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
        if (!decode(blobTiles(blob), out, out.solid, materials, materials ? material : nullptr))
            return DecodeResult::Malformed;
        return DecodeResult::Ok;
    }
    }

    CollisionRepresentation defaultCollisionRepresentation() {
        static const CollisionRepresentation chosen = [] {
            const char* name = std::getenv("PROJV_COLLISION");
            if (!name || std::string(name) == "boxes") return CollisionRepresentation::Boxes;
            if (std::string(name) == "tiles") return CollisionRepresentation::Tiles;
            core::warn("collision: PROJV_COLLISION is '{}'; it takes tiles or boxes", name);
            return CollisionRepresentation::Boxes;
        }();
        return chosen;
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
