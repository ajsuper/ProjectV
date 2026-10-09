#ifndef PROJECTV_LOOSE_BVH_H
#define PROJECTV_LOOSE_BVH_H

#include <cstdint>
#include <vector>

#include "core/math.h"
#include "data_structures/scene.h"

// A bounding-volume hierarchy over loose chunks.
//
// Loose chunks -- the ones with a transform of their own: bodies, props, anything the Scene bridge
// moves -- had no acceleration structure. The shader visited every one for every pixel, and so did
// the CPU picker; in examples/16-sandbox that was about 1.2 ms of GPU per body per frame on an
// integrated GPU. Grids are unaffected: they have their own DDA over cells.
//
// **Nothing authors it.** The GPU upload builds one from the scene's loose chunks on every flush
// (graphics::flushSceneUpdates), and everything that reads the scene's tables gets it: every
// renderer that includes pjv_utils_DDA.sc, with no change to the renderer. A few hundred leaves
// build in well under a millisecond, so there is nothing worth caching across frames or on disk;
// what *is* expensive -- the tight bounds of a chunk's voxels, a walk of its tree -- is cached per
// geometry blob (GeometryBlob::contentMin/Max) and recomputed only when the geometry changes.
//
// Leaves are each chunk's *content* box, transformed to the world, not its cube: a ball in an
// 8-unit chunk is a 3-unit leaf. A chunk with no voxels is not in the tree at all.
//
// PROJV_LOOSE_BVH=0 in the environment turns it off (the shader falls back to its linear loop), so
// the two can be compared on the same scene.
namespace projv::utils {
    // The voxels' bounds in chunk voxel coordinates, inclusive, from the cache or a walk of the
    // tree. False when the blob holds no voxels.
    bool blobContentBounds(const GeometryBlob& blob, core::ivec3& minimum, core::ivec3& maximum);

    // A chunk's content box as a world-space AABB (header position, rotation and scale applied).
    // Falls back to the chunk's whole cube when it has no pooled geometry to measure. False when the
    // chunk is dead or holds no voxels.
    bool looseChunkWorldBounds(const Scene& scene, ChunkHandle chunk, core::vec3& minimum, core::vec3& maximum);

    struct LooseBVHNode {
        static constexpr uint32_t LEAF = 0x80000000u;
        core::vec3 minimum{0.0f};
        core::vec3 maximum{0.0f};
        // Interior: `a` and `b` are the child node indices. Leaf: `a` is LEAF | the first index into
        // LooseBVH::chunks, `b` the count.
        uint32_t a = 0;
        uint32_t b = 0;
        bool leaf() const { return (a & LEAF) != 0; }
        uint32_t first() const { return a & ~LEAF; }
    };

    struct LooseBVH {
        std::vector<LooseBVHNode> nodes;   // nodes[0] is the root; empty when there are no leaves
        std::vector<ChunkHandle> chunks;   // in leaf order: each leaf is a contiguous range
        // Each chunk's own world box, parallel to `chunks`: a leaf holds several chunks, and a query
        // answers for the chunks it actually touches, not for every chunk sharing a leaf with one.
        std::vector<core::vec3> chunkMinimum;
        std::vector<core::vec3> chunkMaximum;
        bool empty() const { return nodes.empty(); }
    };

    // Binned-SAH build over `chunks` (typically Scene::looseChunks). Leaves hold up to
    // `maxLeafSize` chunks; chunks that are dead or hold no voxels are left out.
    LooseBVH buildLooseBVH(const Scene& scene, const std::vector<ChunkHandle>& chunks, uint32_t maxLeafSize = 2);

    // Entry and exit of a ray against an AABB, the slab test both the CPU and the shader use.
    // False when the ray misses or the box is entirely behind the origin.
    bool rayBoxInterval(core::vec3 origin, core::vec3 inverseDirection, core::vec3 minimum, core::vec3 maximum,
                        float& entry, float& exit);

    // Visits the chunks whose own box the ray enters before `maxDistance`, roughly nearest first.
    // `visit(chunk, entryDistance, maxDistance)` returns the distance to keep searching up to -- the
    // nearest hit so far -- so a caller that finds something prunes everything beyond it.
    template<typename Visit>
    void traverseLooseBVH(const LooseBVH& bvh, core::vec3 origin, core::vec3 direction, float maxDistance, Visit visit);

    // Every chunk whose own box overlaps [minimum, maximum].
    template<typename Visit>
    void overlapLooseBVH(const LooseBVH& bvh, core::vec3 minimum, core::vec3 maximum, Visit visit);

    // False when PROJV_LOOSE_BVH=0 is set in the environment.
    bool looseBVHEnabled();
}

// ---- Template definitions -------------------------------------------------------------------

namespace projv::utils {
    template<typename Visit>
    void traverseLooseBVH(const LooseBVH& bvh, core::vec3 origin, core::vec3 direction, float maxDistance, Visit visit) {
        if (bvh.empty()) return;
        core::vec3 inverse(1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z);
        uint32_t stack[64];
        int top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const LooseBVHNode& node = bvh.nodes[stack[--top]];
            float entry = 0.0f, exit = 0.0f;
            if (!rayBoxInterval(origin, inverse, node.minimum, node.maximum, entry, exit) || entry >= maxDistance) continue;
            if (node.leaf()) {
                for (uint32_t i = node.first(); i < node.first() + node.b; i++) {
                    float chunkEntry = 0.0f, chunkExit = 0.0f;
                    if (!rayBoxInterval(origin, inverse, bvh.chunkMinimum[i], bvh.chunkMaximum[i], chunkEntry, chunkExit) ||
                        chunkEntry >= maxDistance) continue;
                    maxDistance = visit(bvh.chunks[i], chunkEntry, maxDistance);
                }
                continue;
            }
            // Near child popped first: pushed last.
            const LooseBVHNode& left = bvh.nodes[node.a];
            const LooseBVHNode& right = bvh.nodes[node.b];
            float leftEntry = 0.0f, rightEntry = 0.0f, ignore = 0.0f;
            bool hitLeft = rayBoxInterval(origin, inverse, left.minimum, left.maximum, leftEntry, ignore);
            bool hitRight = rayBoxInterval(origin, inverse, right.minimum, right.maximum, rightEntry, ignore);
            uint32_t nearChild = node.a, farChild = node.b;
            bool nearHit = hitLeft, farHit = hitRight;
            if (hitRight && (!hitLeft || rightEntry < leftEntry)) {
                nearChild = node.b; farChild = node.a; nearHit = hitRight; farHit = hitLeft;
            }
            if (farHit && top < 64) stack[top++] = farChild;
            if (nearHit && top < 64) stack[top++] = nearChild;
        }
    }

    template<typename Visit>
    void overlapLooseBVH(const LooseBVH& bvh, core::vec3 minimum, core::vec3 maximum, Visit visit) {
        if (bvh.empty()) return;
        uint32_t stack[64];
        int top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const LooseBVHNode& node = bvh.nodes[stack[--top]];
            if (node.maximum.x < minimum.x || node.minimum.x > maximum.x ||
                node.maximum.y < minimum.y || node.minimum.y > maximum.y ||
                node.maximum.z < minimum.z || node.minimum.z > maximum.z) continue;
            if (node.leaf()) {
                for (uint32_t i = node.first(); i < node.first() + node.b; i++) {
                    const core::vec3& low = bvh.chunkMinimum[i];
                    const core::vec3& high = bvh.chunkMaximum[i];
                    if (high.x < minimum.x || low.x > maximum.x || high.y < minimum.y || low.y > maximum.y ||
                        high.z < minimum.z || low.z > maximum.z) continue;
                    visit(bvh.chunks[i]);
                }
                continue;
            }
            if (top < 63) { stack[top++] = node.a; stack[top++] = node.b; }
        }
    }
}

#endif
