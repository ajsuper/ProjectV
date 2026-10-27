#ifndef PROJECTV_TEARDOWN_CANVAS_H
#define PROJECTV_TEARDOWN_CANVAS_H
// A sparse, unbounded voxel volume: material ids in 8^3 bricks keyed by brick coordinate.
//
// Everything the converter builds lands in one of these -- a shape in its own frame while its
// children are merged into it, then a body (or the static world) the shapes are stamped into. 8^3
// keeps a one-voxel-thick ground plane (common: a voxbox 2000 x 1 x 2000) at 1 KB per 0.8 m square
// rather than the 8 KB a 16^3 brick would cost.

#include <memory>
#include <unordered_map>

#include "common.h"
#include "materials.h"

namespace teardown {

class Canvas {
public:
    // Move-only. Its bricks are unique_ptrs, and MSVC's unordered_map has a move constructor that may throw (libstdc++'s
    // does not), so a std::vector<Canvas> growing would fall back to the copy constructor, which cannot exist.
    Canvas() = default;
    Canvas(Canvas&&) = default;
    Canvas& operator=(Canvas&&) = default;
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    static constexpr int B = 8, B3 = B * B * B;
    struct Brick {
        MatId v[B3] = {};
        uint16_t count = 0;
    };

    static uint64_t key(int bx, int by, int bz) {
        return (uint64_t(uint32_t(bx + (1 << 20)) & 0x1FFFFF)) | (uint64_t(uint32_t(by + (1 << 20)) & 0x1FFFFF) << 21) |
               (uint64_t(uint32_t(bz + (1 << 20)) & 0x1FFFFF) << 42);
    }
    static ivec3 unkey(uint64_t k) {
        return ivec3(int(k & 0x1FFFFF) - (1 << 20), int((k >> 21) & 0x1FFFFF) - (1 << 20),
                     int((k >> 42) & 0x1FFFFF) - (1 << 20));
    }
    static int local(int x, int y, int z) { return (x & (B - 1)) | (y & (B - 1)) << 3 | (z & (B - 1)) << 6; }

    void set(int x, int y, int z, MatId m);
    MatId get(int x, int y, int z) const;
    void clear() { bricks_.clear(); }
    bool empty() const { return bricks_.empty(); }
    size_t voxelCount() const;
    // Inclusive-exclusive voxel bounds of everything set. False when empty.
    bool bounds(ivec3& lo, ivec3& hi) const;

    template <typename F> void forEach(F&& f) const {
        for (const auto& [k, brick] : bricks_) {
            const ivec3 base = unkey(k) * B;
            for (int i = 0; i < B3; i++)
                if (brick->v[i]) f(base.x + (i & 7), base.y + ((i >> 3) & 7), base.z + (i >> 6), brick->v[i]);
        }
    }

    const std::unordered_map<uint64_t, std::unique_ptr<Brick>>& bricks() const { return bricks_; }

private:
    std::unordered_map<uint64_t, std::unique_ptr<Brick>> bricks_;
};

// Writes `src` into `dst`. `dstFromSrc` places src's frame in dst's frame (metres); each canvas's
// voxel (i,j,k) spans [i, i+1) * its voxel size along each axis of its frame.
//
// A src voxel overwrites what is under it. MAT_HOLE follows `holes`: Carve clears the dst voxel
// (a child shape carving its parent), Keep copies the mark (pieces of one shape assembled before
// it is merged), and Skip drops it, so a shape's hole voxels never reach past the shape they were
// drawn in. Axis-aligned placements at equal voxel size are a lossless permutation of
// voxels; anything else is resampled from dst back into src, nearest voxel, so a rotated wall
// comes out solid rather than speckled.
enum class Holes { Skip, Carve, Keep };
void stamp(Canvas& dst, float dstVoxel, const Canvas& src, float srcVoxel, const Transform& dstFromSrc, Holes holes);

} // namespace teardown

#endif // PROJECTV_TEARDOWN_CANVAS_H
