#ifndef PROJECTV_TILE_COLLISION_H
#define PROJECTV_TILE_COLLISION_H

#include <cstdint>
#include <vector>

#include "core/math.h"
#include "utils/tile_table.h"

// What collision asks of a tile table (docs/plans/voxel-collision-shape.md): the solid boxes near a
// body, a ray's first solid voxel, and the face a point on the surface lies on. Read straight from the
// table's levels -- regions and blocks with nothing in them are skipped, a full block or brick is one
// box -- so nothing is built beforehand and nothing is kept. In voxel units, the chunk's corner at the
// origin. No physics engine here: runtime/physics's voxel shape is these, in Jolt's terms.
namespace projv::utils {
    // A box of solid voxels, all of one palette slot, inclusive.
    struct TileBox {
        core::ivec3 low{0}, high{-1};
        uint8_t     slot = 0;
    };

    // The boxes covering the table's solid voxels that reach into [low, high] (inclusive voxels),
    // appended to `out`: a full 16^3 block as one box; a block's full bricks merged greedily into boxes
    // of one slot; a partly filled brick's voxels merged likewise within it. Each box is whole, not cut
    // to the range, and how voxels merge does not depend on the range -- a body moving over a surface
    // meets the same boxes wherever it is. Blocks in z-y-x order, boxes within a block in a fixed order.
    void tileBoxesIn(const TileTable& table, const TileLayout& layout, core::ivec3 low, core::ivec3 high,
                     std::vector<TileBox>& out);

    // The table's solid voxels as few boxes as a greedy merge within each 16^3 block makes -- along x,
    // then y, then z, boxes of one slot, across the block's bricks -- block by block in the layout's
    // order, with `starts[i]` the first box of block i (layout.blockIndex) and `starts.back()` the
    // count: what a moving body's shape collides by, made once, and looked up by block as tileBoxesIn
    // is. (tileBoxesIn's merge stops at every 4^3 brick, and a piece met box by box costs a test a box.)
    void tileBlockBoxes(const TileTable& table, const TileLayout& layout, std::vector<TileBox>& boxes,
                        std::vector<uint32_t>& starts);

    struct TileRayHit {
        float       t = 0.0f;          // origin + t * direction is where it enters the voxel
        core::ivec3 voxel{0};
        core::ivec3 normal{0};         // the face it enters by; zero when the ray starts inside it
    };

    // The first solid voxel along origin + t * direction, t in [0, maxT]: the table marched region,
    // block, brick, voxel, each level only where the one above holds something. A ray that starts in a
    // solid voxel hits it at t = 0, as a box's would: collision treats voxels as solid.
    bool tileTableRaycast(const TileTable& table, const TileLayout& layout, core::vec3 origin, core::vec3 direction,
                          float maxT, TileRayHit& hit);

    // The outward normal of the solid voxel face `point` lies on (or nearest to), as a unit axis: of the
    // voxel boundaries near the point, the nearest with solid on one side and empty on the other. Zero
    // when no boundary near it is one (a point not on the surface).
    core::ivec3 tileSurfaceNormal(const TileTable& table, core::vec3 point);

    // log2 of the cells a collision shape names its contacts by, given `idBits` to name them in: single
    // voxels (0) when their index fits -- a 256^3 chunk's takes 24 bits -- else the smallest cells whose
    // does. Jolt's sub-shape ids are 32 bits, shared with every compound above the shape: a body of n
    // chunks leaves each 32 - ceil(log2 n).
    int tileCellShift(uint32_t resolution, uint32_t idBits);

    // The palette slot of voxel `v`, or of the first solid voxel of the 4^3 brick holding it when `v`
    // is empty; false when the brick is empty. What a contact named only by its brick is made of.
    bool tileBrickSlot(const TileTable& table, core::ivec3 v, uint8_t& slot);
}

#endif
