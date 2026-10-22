#include "canvas.h"

#include <algorithm>
#include <climits>

namespace teardown {

void Canvas::set(int x, int y, int z, MatId m) {
    const uint64_t k = key(x >> 3, y >> 3, z >> 3);
    const int i = local(x, y, z);
    if (m == MAT_EMPTY) {
        auto it = bricks_.find(k);
        if (it == bricks_.end() || !it->second->v[i]) return;
        it->second->v[i] = MAT_EMPTY;
        if (--it->second->count == 0) bricks_.erase(it);
        return;
    }
    auto& brick = bricks_[k];
    if (!brick) brick = std::make_unique<Brick>();
    if (!brick->v[i]) brick->count++;
    brick->v[i] = m;
}

MatId Canvas::get(int x, int y, int z) const {
    auto it = bricks_.find(key(x >> 3, y >> 3, z >> 3));
    return it == bricks_.end() ? MAT_EMPTY : it->second->v[local(x, y, z)];
}

size_t Canvas::voxelCount() const {
    size_t n = 0;
    for (const auto& [k, brick] : bricks_) n += brick->count;
    return n;
}

bool Canvas::bounds(ivec3& lo, ivec3& hi) const {
    if (bricks_.empty()) return false;
    lo = ivec3(INT_MAX);
    hi = ivec3(INT_MIN);
    for (const auto& [k, brick] : bricks_) {
        const ivec3 base = unkey(k) * B;
        for (int i = 0; i < B3; i++) {
            if (!brick->v[i]) continue;
            ivec3 p = base + ivec3(i & 7, (i >> 3) & 7, i >> 6);
            lo = glm::min(lo, p);
            hi = glm::max(hi, p + 1);
        }
    }
    return true;
}

namespace {
    // The rotation as an integer matrix, when it is one (every entry 0 or +-1).
    bool axisAligned(const mat3& m, glm::imat3x3& out) {
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 3; r++) {
                float v = m[c][r];
                int iv = int(std::lround(v));
                if (std::abs(v - float(iv)) > 1e-3f) return false;
                out[c][r] = iv;
            }
        return true;
    }

    inline void write(Canvas& dst, int x, int y, int z, MatId m, Holes holes) {
        if (m != MAT_HOLE || holes == Holes::Keep) dst.set(x, y, z, m);
        else if (holes == Holes::Carve) dst.set(x, y, z, MAT_EMPTY);
    }
}

void stamp(Canvas& dst, float dstVoxel, const Canvas& src, float srcVoxel, const Transform& dstFromSrc, Holes holes) {
    const mat3 R = glm::mat3_cast(dstFromSrc.rot);
    glm::imat3x3 Ri;
    if (std::abs(dstVoxel - srcVoxel) < 1e-6f && axisAligned(R, Ri)) {
        // dst = floor(R * (v + 0.5) + t / s) = Ri * v + floor(R * 0.5 + t / s), exactly, for integer v.
        const vec3 k = R * vec3(0.5f) + dstFromSrc.pos / dstVoxel;
        const ivec3 off(int(std::floor(k.x + 1e-4f)), int(std::floor(k.y + 1e-4f)), int(std::floor(k.z + 1e-4f)));
        src.forEach([&](int x, int y, int z, MatId m) {
            ivec3 d = Ri * ivec3(x, y, z) + off;
            write(dst, d.x, d.y, d.z, m, holes);
        });
        return;
    }

    // Resample: for every dst voxel a src brick could cover, look back at which src voxel its centre
    // is in. A dst voxel is accepted only from the brick that owns its src voxel, so each is written
    // once however the bricks' footprints overlap.
    const mat3 Rinv = glm::transpose(R);
    for (const auto& [k, brick] : src.bricks()) {
        const ivec3 base = Canvas::unkey(k) * Canvas::B;
        vec3 lo(1e30f), hi(-1e30f);
        for (int c = 0; c < 8; c++) {
            vec3 corner = vec3(base + ivec3(c & 1, (c >> 1) & 1, c >> 2) * Canvas::B) * srcVoxel;
            vec3 p = dstFromSrc.apply(corner) / dstVoxel;
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        const ivec3 a(glm::floor(lo)), b(glm::ceil(hi));
        for (int z = a.z; z < b.z; z++)
            for (int y = a.y; y < b.y; y++)
                for (int x = a.x; x < b.x; x++) {
                    vec3 centre = (vec3(x, y, z) + 0.5f) * dstVoxel;
                    vec3 s = Rinv * (centre - dstFromSrc.pos) / srcVoxel;
                    ivec3 sv(glm::floor(s));
                    ivec3 rel = sv - base;
                    if (rel.x < 0 || rel.y < 0 || rel.z < 0 || rel.x >= Canvas::B || rel.y >= Canvas::B || rel.z >= Canvas::B)
                        continue;
                    MatId m = brick->v[rel.x | rel.y << 3 | rel.z << 6];
                    if (m) write(dst, x, y, z, m, holes);
                }
    }
}

} // namespace teardown
