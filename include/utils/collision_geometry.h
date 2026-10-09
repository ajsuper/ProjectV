#ifndef PROJECTV_COLLISION_GEOMETRY_H
#define PROJECTV_COLLISION_GEOMETRY_H

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
    };

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
    };

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

    // The pieces `blob` collides as. `resolution` is the chunk's native resolution (a power of
    // four), which fixes the tree's depth.
    CollisionPieces buildCollisionPieces(const GeometryBlob& blob, uint32_t resolution,
                                         const CollisionParams& params = {});
}

#endif
