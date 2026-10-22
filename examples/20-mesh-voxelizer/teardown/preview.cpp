#include "preview.h"

#include <algorithm>
#include <limits>

#include "png.h"

namespace teardown {

namespace {
    struct View {
        vec3 dir, u, v;
        float scale = 1, u0 = 0, v0 = 0;
        int w = 0, h = 0;
        std::vector<float> depth;
        std::vector<uint8_t> rgb;
    };

    // Calls f(worldCentre, normal, material) for every voxel of `c` with an empty face neighbour.
    template <typename F>
    void surfaceVoxels(const Canvas& c, const Transform& frame, F&& f, float voxel = TD_VOXEL) {
        const auto& bricks = c.bricks();
        const mat3 R = glm::mat3_cast(frame.rot);
        for (const auto& [k, brick] : bricks) {
            const ivec3 bc = Canvas::unkey(k);
            const Canvas::Brick* nb[6];
            const ivec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            for (int d = 0; d < 6; d++) {
                ivec3 n = bc + dirs[d];
                auto it = bricks.find(Canvas::key(n.x, n.y, n.z));
                nb[d] = it == bricks.end() ? nullptr : it->second.get();
            }
            auto solid = [&](int x, int y, int z) -> bool {
                const Canvas::Brick* b = brick.get();
                if (x < 0) { b = nb[1]; x += 8; } else if (x > 7) { b = nb[0]; x -= 8; }
                else if (y < 0) { b = nb[3]; y += 8; } else if (y > 7) { b = nb[2]; y -= 8; }
                else if (z < 0) { b = nb[5]; z += 8; } else if (z > 7) { b = nb[4]; z -= 8; }
                if (!b) return false;
                MatId m = b->v[x | y << 3 | z << 6];
                return m && m != MAT_HOLE;
            };
            for (int i = 0; i < Canvas::B3; i++) {
                const MatId m = brick->v[i];
                if (!m || m == MAT_HOLE) continue;
                const int x = i & 7, y = (i >> 3) & 7, z = i >> 6;
                vec3 n(0.0f);
                for (int d = 0; d < 6; d++)
                    if (!solid(x + dirs[d].x, y + dirs[d].y, z + dirs[d].z)) n += vec3(dirs[d]);
                if (n == vec3(0.0f)) {
                    bool exposed = false;
                    for (int d = 0; d < 6 && !exposed; d++) exposed = !solid(x + dirs[d].x, y + dirs[d].y, z + dirs[d].z);
                    if (!exposed) continue;
                    n = vec3(0, 1, 0);
                }
                const vec3 local = (vec3(bc * Canvas::B + ivec3(x, y, z)) + 0.5f) * voxel;
                f(frame.apply(local), glm::normalize(R * n), m);
            }
        }
    }
}

bool renderPreview(const Level& level, const std::string& pngPath, int width, vec3 focus, float radius) {
    View view;
    view.dir = glm::normalize(vec3(-1.0f, -0.9f, -1.0f));
    view.u = glm::normalize(glm::cross(view.dir, vec3(0, 1, 0)));
    view.v = glm::cross(view.u, view.dir);

    // Bounds in view space, from every canvas's corners.
    float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
    auto inFocus = [&](const vec3& p) {
        return radius <= 0.0f || glm::length(glm::vec2(p.x - focus.x, p.z - focus.z)) <= radius;
    };
    auto extend = [&](const Canvas& c, const Transform& frame) {
        ivec3 lo, hi;
        if (!c.bounds(lo, hi)) return;
        if (radius > 0.0f) {
            // The focus disc's box stands in for the canvas; heights still come from the canvas.
            float y0 = frame.apply(vec3(lo) * TD_VOXEL).y, y1 = frame.apply(vec3(hi) * TD_VOXEL).y;
            for (int k = 0; k < 8; k++) {
                vec3 p(focus.x + (k & 1 ? radius : -radius), k & 2 ? std::max(y0, y1) : std::min(y0, y1),
                       focus.z + (k & 4 ? radius : -radius));
                umin = std::min(umin, glm::dot(p, view.u)); umax = std::max(umax, glm::dot(p, view.u));
                vmin = std::min(vmin, glm::dot(p, view.v)); vmax = std::max(vmax, glm::dot(p, view.v));
            }
            return;
        }
        for (int k = 0; k < 8; k++) {
            vec3 p = frame.apply(vec3(k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z) * TD_VOXEL);
            umin = std::min(umin, glm::dot(p, view.u)); umax = std::max(umax, glm::dot(p, view.u));
            vmin = std::min(vmin, glm::dot(p, view.v)); vmax = std::max(vmax, glm::dot(p, view.v));
        }
    };
    extend(level.world, Transform{});
    for (const auto& b : level.bodies) extend(b->voxels, b->frame);
    if (umin > umax) return false;

    view.scale = float(width) / std::max(umax - umin, 1e-3f);
    view.w = width;
    view.h = std::clamp(int((vmax - vmin) * view.scale) + 1, 16, width * 2);
    view.u0 = umin;
    view.v0 = vmax;
    view.depth.assign(size_t(view.w) * view.h, std::numeric_limits<float>::max());
    view.rgb.resize(size_t(view.w) * view.h * 3);
    for (int y = 0; y < view.h; y++)
        for (int x = 0; x < view.w; x++) {
            uint8_t* px = &view.rgb[(size_t(y) * view.w + x) * 3];
            px[0] = uint8_t(150 + 60 * y / view.h); px[1] = uint8_t(180 + 50 * y / view.h); px[2] = 225;
        }

    const vec3 sun = glm::normalize(vec3(0.45f, 0.8f, 0.35f));
    const int splat = std::max(1, int(std::ceil(TD_VOXEL * view.scale * 1.3f)));
    auto draw = [&](const vec3& p, const vec3& n, MatId m) {
        if (!inFocus(p)) return;
        const float depth = glm::dot(p, view.dir);
        const int cx = int((glm::dot(p, view.u) - view.u0) * view.scale);
        const int cy = int((view.v0 - glm::dot(p, view.v)) * view.scale);
        const VoxMaterial& mat = materials().get(m);
        const float light = 0.35f + 0.65f * std::max(0.0f, glm::dot(n, sun)) + (mat.emissive ? 0.6f : 0.0f);
        const uint8_t col[3] = {uint8_t(std::min(255.0f, mat.r * light)), uint8_t(std::min(255.0f, mat.g * light)),
                                uint8_t(std::min(255.0f, mat.b * light))};
        for (int dy = 0; dy < splat; dy++)
            for (int dx = 0; dx < splat; dx++) {
                const int x = cx + dx - splat / 2, y = cy + dy - splat / 2;
                if (x < 0 || y < 0 || x >= view.w || y >= view.h) continue;
                float& d = view.depth[size_t(y) * view.w + x];
                if (depth >= d) continue;
                d = depth;
                std::copy(col, col + 3, &view.rgb[(size_t(y) * view.w + x) * 3]);
            }
    };
    surfaceVoxels(level.world, Transform{}, draw);
    for (const auto& b : level.bodies) surfaceVoxels(b->voxels, b->frame, draw);
    // Water last, blended over what is under it rather than hiding it.
    const int waterSplat = std::max(1, int(std::ceil(WATER_VOXEL * view.scale * 1.3f)));
    surfaceVoxels(level.water, Transform{}, [&](const vec3& p, const vec3&, MatId m) {
        if (!inFocus(p)) return;
        const float depth = glm::dot(p, view.dir);
        const int cx = int((glm::dot(p, view.u) - view.u0) * view.scale);
        const int cy = int((view.v0 - glm::dot(p, view.v)) * view.scale);
        const VoxMaterial& mat = materials().get(m);
        for (int dy = 0; dy < waterSplat; dy++)
            for (int dx = 0; dx < waterSplat; dx++) {
                const int x = cx + dx - waterSplat / 2, y = cy + dy - waterSplat / 2;
                if (x < 0 || y < 0 || x >= view.w || y >= view.h) continue;
                if (depth >= view.depth[size_t(y) * view.w + x]) continue;
                uint8_t* px = &view.rgb[(size_t(y) * view.w + x) * 3];
                px[0] = uint8_t(px[0] * 0.35f + mat.r * 0.65f);
                px[1] = uint8_t(px[1] * 0.35f + mat.g * 0.65f);
                px[2] = uint8_t(px[2] * 0.35f + mat.b * 0.65f);
                view.depth[size_t(y) * view.w + x] = depth;
            }
    }, WATER_VOXEL);
    return writePng(pngPath, view.w, view.h, view.rgb);
}

} // namespace teardown
