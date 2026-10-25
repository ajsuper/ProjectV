#ifndef PROJECTV_COLLISION_GEOMETRY_H
#define PROJECTV_COLLISION_GEOMETRY_H

#include <array>
#include <cstdint>
#include <vector>

#include "core/math.h"
#include "data_structures/scene.h"

// What voxels collide as: a blob turned into convex pieces, in the blob's own voxel space (one unit
// per voxel, the chunk's corner at the origin). The physics layer builds its shapes from these and
// nothing else, so this is the one place that decides how voxel geometry collides -- and the seam
// that a different representation (voxel contours, other geometry that departs slightly from cubes)
// replaces, without physics or gameplay changing. No Jolt and no EnTT here: it is plain data, and
// it is tested exactly against the voxels.
//
// Rules every representation keeps (the physics plan, A6):
// - **Never bigger than the voxels.** Every piece lies inside the union of solid voxel cubes. A
//   representation may cut corners off; it may not add volume. (The one exception is the fallback
//   below, which is flagged so callers can see it.)
// - **Deterministic.** The same voxels and parameters give the same pieces in the same order on
//   every machine; networked peers build shapes locally and must agree.
// - **Normals come from the pieces.** Nothing derives a collision normal from voxel coordinates.
namespace projv::utils {

    enum class CollisionRepresentation : uint8_t {
        // Voxels merged greedily into axis-aligned boxes: exact, no overlaps, solid all the way
        // through (a body that ends up inside is pushed out, which a surface mesh would not do).
        Boxes = 1,
        // A body's voxels collide as its blob's tile table, read in place (runtime/physics's
        // VoxelShape; docs/plans/voxel-collision-shape.md): exact, nothing built, solid all the way
        // through as boxes are -- static bodies and, weighed from the table, moving ones, which collide
        // with each other box by box. pieceBudget and maxDecodedCells do not apply to it.
        Tiles = 2,
    };

    // Boxes, unless the environment says PROJV_COLLISION=tiles: what runtime::PhysicsConfig starts with,
    // to run a suite, or a game, on the other representation. A representation set explicitly is kept.
    CollisionRepresentation defaultCollisionRepresentation();

    struct CollisionParams {
        CollisionRepresentation representation = CollisionRepresentation::Boxes;
        // More pieces than this and the blob collides as its content box instead (flagged; logged).
        uint32_t pieceBudget = 1024;
        // A content box holding more voxel cells than this is not decoded at all, and collides as
        // its box. 32M cells is 32 MB of scratch, a 320^3 box.
        uint64_t maxDecodedCells = 1ull << 25;

        // Everything above, folded into one number: part of a shape cache key, and of the network
        // state hash, so two peers on different parameters are a detected mismatch.
        uint64_t key() const;
    };

    struct CollisionPiece {
        enum class Kind : uint8_t { Box };
        Kind        kind = Kind::Box;
        core::vec3  min{0.0f}, max{0.0f};       // Box, voxel space
        // The voxels the piece stands for, inclusive. How a contact or a ray hit on this piece is
        // traced back to voxels: for materials, damage, per-voxel friction.
        core::ivec3 voxelMin{0}, voxelMax{-1};
        // The physics material every voxel of the piece has (SlotMaterials): 0 for none, which is
        // the body's own values.
        uint16_t    material = 0;
    };

    // Which physics material each palette slot of a component names: an index into the physics
    // material library (runtime/physics.h), 0 for none. Pieces never span two materials, so slots
    // that differ only in look (three colours of one stone) merge as if they were one, and slots
    // that look alike but name different materials do not.
    using SlotMaterials = std::array<uint16_t, 256>;

    enum class CollisionFallback : uint8_t {
        None,
        OverBudget,     // the decomposition needed more than pieceBudget pieces
        TooLarge,       // the content box exceeded maxDecodedCells
        Malformed       // the tree could not be decoded
    };

    struct CollisionPieces {
        std::vector<CollisionPiece> pieces;     // empty: the blob has no voxels, nothing collides
        core::ivec3       contentMin{0}, contentMax{-1};
        // Not None: `pieces` is the single content box, which can be bigger than the voxels.
        CollisionFallback fallback = CollisionFallback::None;
        uint64_t          solidVoxels = 0;      // 0 for a fallback that never decoded the tree
    };

    // A blob's solid voxels as a dense box over its content bounds: one byte per cell (1 solid, 0
    // empty), x fastest. What collision pieces and voxel islands are both computed from.
    struct VoxelOccupancy {
        core::ivec3 origin{0}, dims{0};
        std::vector<uint8_t> cells;
        uint64_t solid = 0;
        size_t index(core::ivec3 p) const {   // p relative to origin
            return (size_t(p.z) * size_t(dims.y) + size_t(p.y)) * size_t(dims.x) + size_t(p.x);
        }
    };
    enum class DecodeResult : uint8_t { Ok, Empty, TooLarge, Malformed };
    DecodeResult decodeOccupancy(const GeometryBlob& blob, uint32_t resolution, uint64_t maxCells, VoxelOccupancy& out);
    // The same, and each solid cell's palette slot in `slots` (one per cell, 0 where empty).
    DecodeResult decodeVoxelSlots(const GeometryBlob& blob, uint32_t resolution, uint64_t maxCells, VoxelOccupancy& out,
                                  std::vector<uint8_t>& slots);

    // The pieces `blob` collides as. `resolution` is the chunk's native resolution (a power of
    // four), which fixes the tree's depth. With `materials`, each voxel's palette slot is looked up
    // in it and boxes stop where the material changes; without, every piece is material 0.
    CollisionPieces buildCollisionPieces(const GeometryBlob& blob, uint32_t resolution,
                                         const CollisionParams& params = {},
                                         const SlotMaterials* materials = nullptr);
}

#endif
