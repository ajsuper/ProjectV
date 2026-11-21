// Trees as voxels (plan D10, a first pass): procedural shapes stamped into a column's chunks in the
// rings whose voxels are small enough to draw them (2 m and finer); coarser rings keep the canopy as a
// dithered tint of their ground voxels, which averages to the same cover.
//
// Where trees stand is a pure function of world position -- a jittered grid per stand density, each
// cell's tree there or not by the canopy cover at its trunk -- so every ring and every column agrees on
// every tree, and a crown that crosses a column edge is stamped half in each.
//
//   juniper, pinyon   round, bushy, 2.5-6 m: the dotted foothills
//   ponderosa         a bare orange trunk, then an oval crown, 14-24 m
//   fir, spruce       cones on short trunks, 12-28 m, dusted with snow above the snowline
//   aspen             bare in November: a pale trunk and a thin grey crown
#pragma once

#include <functional>
#include <vector>

#include "mountains/landform.hpp"
#include "mountains/surface.hpp"

namespace mountains {

enum class TreeKind : uint8_t { Juniper, Pinyon, Ponderosa, Fir, Spruce, Aspen };

struct Tree {
    double x = 0, z = 0;   // trunk
    double base = 0;       // ground at the trunk
    double height = 0, radius = 0;
    TreeKind kind = TreeKind::Juniper;
    Material foliage = Material::Pinyon;
    bool snowy = false;
    uint32_t seed = 0;
};

// The stand grid: one candidate tree per cell. 6 m cells; a crown is about half a cell across, so a
// cover of c keeps a cell's tree with probability ~ c * cell^2 / crown area.
constexpr double kTreeCell = 6.0;
constexpr double kTreeReach = 8.0;   // farthest a crown reaches from its trunk, m

// `ground(x, z, sample)` gives the ground at a point (interpolated by the caller); trees whose crowns
// could reach [x0, x1] x [z0, z1] go into `out`.
inline void collectTrees(double x0, double z0, double x1, double z1, uint32_t seed,
                         const std::function<bool(double, double, Sample&)>& ground, std::vector<Tree>& out) {
    const int64_t i0 = int64_t(std::floor((x0 - kTreeReach) / kTreeCell)), i1 = int64_t(std::floor((x1 + kTreeReach) / kTreeCell));
    const int64_t j0 = int64_t(std::floor((z0 - kTreeReach) / kTreeCell)), j1 = int64_t(std::floor((z1 + kTreeReach) / kTreeCell));
    for (int64_t j = j0; j <= j1; ++j)
        for (int64_t i = i0; i <= i1; ++i) {
            const uint32_t h = hash2(i, j, seed * 0x2545F491U + 17U);
            auto u = [&](int k) { return unit(hash32(h + uint32_t(k) * 0x9e3779b9U)); };
            Tree t;
            t.x = (double(i) + 0.15 + 0.7 * u(1)) * kTreeCell;
            t.z = (double(j) + 0.15 + 0.7 * u(2)) * kTreeCell;
            Sample s;
            if (!ground(t.x, t.z, s)) continue;
            // Continuous cover (a coarse footprint: no crowns-as-dots), and nothing on rock or snow.
            SurfaceMix m = surfaceMix(s, t.x, t.z, 100.0);
            if (m.rock > 0.5 || m.water > 0.3) continue;
            Material species = m.treeMix > u(3) ? m.treeB : m.treeA;
            double size = 0.75 + 0.5 * u(4);
            switch (species) {
                case Material::Pinyon:
                    t.kind = u(5) < 0.6 ? TreeKind::Juniper : TreeKind::Pinyon;
                    t.height = (t.kind == TreeKind::Juniper ? 3.5 : 5.0) * size;
                    t.radius = t.height * (t.kind == TreeKind::Juniper ? 0.48 : 0.4);
                    break;
                case Material::Ponderosa:
                    t.kind = TreeKind::Ponderosa;
                    t.height = 19.0 * size;
                    t.radius = 3.2 * size;
                    break;
                case Material::Fir:
                    t.kind = TreeKind::Fir;
                    t.height = 18.0 * size;
                    t.radius = 3.3 * size;
                    break;
                case Material::Spruce:
                    t.kind = TreeKind::Spruce;
                    t.height = 21.0 * size;
                    t.radius = 3.0 * size;
                    break;
                case Material::AspenBare:
                    t.kind = TreeKind::Aspen;
                    t.height = 13.0 * size;
                    t.radius = 2.4 * size;
                    break;
                default:
                    continue;
            }
            // Keep it with the probability that makes the crowns cover what the canopy says.
            const double crown = kPi * t.radius * t.radius * 0.8;
            if (u(6) >= m.cover * kTreeCell * kTreeCell / crown) continue;
            t.foliage = species;
            t.base = s.height;
            t.snowy = m.snow > 0.4 && t.kind != TreeKind::Aspen;
            t.seed = h;
            out.push_back(t);
        }
}

// Visits every voxel of `t` inside the box [lo, hi) (world metres; voxels of edge v on the lattice
// at multiples of v), as (voxel x, voxel y, voxel z, material, shade).
template <typename Visit>
inline void stampTree(const Tree& t, double v, const double lo[3], const double hi[3], Visit&& visit) {
    const double top = t.base + t.height;
    const double reach = t.radius + v;
    const int64_t ix0 = int64_t(std::floor(std::max(lo[0], t.x - reach) / v)), ix1 = int64_t(std::ceil(std::min(hi[0], t.x + reach) / v));
    const int64_t iz0 = int64_t(std::floor(std::max(lo[2], t.z - reach) / v)), iz1 = int64_t(std::ceil(std::min(hi[2], t.z + reach) / v));
    const int64_t iy0 = int64_t(std::floor(std::max(lo[1], t.base - 0.6) / v)), iy1 = int64_t(std::ceil(std::min(hi[1], top) / v));
    if (ix0 >= ix1 || iz0 >= iz1 || iy0 >= iy1) return;
    const double trunkR = std::max(v * 0.5, t.height * (t.kind == TreeKind::Ponderosa ? 0.025 : 0.02));
    for (int64_t iz = iz0; iz < iz1; ++iz)
        for (int64_t ix = ix0; ix < ix1; ++ix) {
            const double x = (double(ix) + 0.5) * v - t.x, z = (double(iz) + 0.5) * v - t.z;
            const double rr = std::sqrt(x * x + z * z);
            const double ang = std::atan2(z, x);
            for (int64_t iy = iy0; iy < iy1; ++iy) {
                const double y = (double(iy) + 0.5) * v - t.base;   // height over the ground at the trunk
                if (y < -0.6 || y > t.height) continue;
                const double f = y / t.height;
                // Lumps: the crown's edge in and out with angle and height.
                const uint32_t hs = t.seed;
                const double lump = 1.0 + 0.18 * std::sin(ang * 3.0 + unit(hs) * 6.28 + f * 4.0) +
                                    0.12 * std::sin(ang * 5.0 + unit(hash32(hs)) * 6.28 - f * 7.0);
                bool crown = false, trunk = false;
                switch (t.kind) {
                    case TreeKind::Juniper:
                    case TreeKind::Pinyon: {
                        // A round bush on a short trunk, widest a little above half way.
                        const double cy = t.height * 0.55, ry = t.height * 0.47;
                        const double dy = (y - cy) / ry;
                        crown = rr < t.radius * lump * std::sqrt(std::max(0.0, 1.0 - dy * dy));
                        trunk = f < 0.3 && rr < trunkR;
                        break;
                    }
                    case TreeKind::Ponderosa: {
                        const double c0 = 0.45;   // bare trunk below
                        if (f > c0) {
                            const double g = (f - c0) / (1.0 - c0);
                            crown = rr < t.radius * lump * std::sqrt(std::max(0.0, std::sin(kPi * std::min(1.0, g * 1.05))));
                        }
                        trunk = f < 0.8 && rr < trunkR;
                        break;
                    }
                    case TreeKind::Fir:
                    case TreeKind::Spruce: {
                        const double c0 = t.kind == TreeKind::Spruce ? 0.08 : 0.12;
                        if (f > c0) {
                            const double g = (f - c0) / (1.0 - c0);
                            // A cone, its tiers showing as the edge steps in and out.
                            const double tiers = 0.85 + 0.15 * std::fabs(std::sin(g * 18.0));
                            crown = rr < t.radius * (1.0 - g) * tiers * (0.9 + 0.1 * lump);
                        }
                        trunk = rr < trunkR && f < 0.9;
                        break;
                    }
                    case TreeKind::Aspen: {
                        const double c0 = 0.4;
                        if (f > c0) {
                            const double g = (f - c0) / (1.0 - c0);
                            const double shell = t.radius * lump * std::sqrt(std::max(0.0, std::sin(kPi * g)));
                            // Bare: only some of the crown's voxels, the twigs.
                            crown = rr < shell && hash2(ix * 7 + iy, iz, hs) % 5 == 0;
                        }
                        trunk = rr < trunkR;
                        break;
                    }
                }
                if (!crown && !trunk) continue;
                Material m;
                int shade;
                const uint32_t vh = hash2(ix * 131 + iy, iz, hs);
                if (trunk && !crown) {
                    m = t.kind == TreeKind::Aspen ? Material::AspenBare : Material::Bark;
                    shade = t.kind == TreeKind::Ponderosa ? 12 : 4;
                } else {
                    m = t.kind == TreeKind::Aspen ? Material::AspenBare : t.foliage;
                    // Lighter toward the top and the outside, with a speckle.
                    const double lit = 0.35 + 0.4 * f + 0.25 * std::min(1.0, rr / std::max(t.radius, 1e-3));
                    shade = std::clamp(int((lit + (unit(vh) - 0.5) * 0.3) * kShades), 0, kShades - 1);
                    // Snow on the upper side of the branches, above the snowline.
                    if (t.snowy && unit(hash32(vh)) < 0.35 + 0.3 * f) { m = Material::Snow; shade = 10; }
                }
                visit(ix, iy, iz, m, shade);
            }
        }
}

}  // namespace mountains
