// Trees and shrubs as voxels (plan D10): procedural shapes stamped into a column's chunks in the rings
// whose voxels are small enough to draw them; coarser rings keep the canopy as a dithered tint of their
// ground voxels (raised a voxel where the plants are tall enough), which averages to the same cover.
//
// Where plants stand is a pure function of world position -- a jittered grid per stand, each cell's
// plant there or not by the cover at its trunk -- so every ring and every column agrees on every plant,
// and a crown that crosses a column edge is stamped half in each.
//
// A crown is a cluster of lobes (ellipsoids) eroded by 3D noise, so its edge is ragged and the sky shows
// through it; each tree leans a little, has its own size and its own shift of colour, and is darker
// inside and lighter at its top and rim.
//
//   juniper   shaggy, 1.5-6 m, a twisted trunk showing between lobes; grey-blue-green
//   pinyon    rounder and denser, 3-8 m; deeper green
//   snag      a dead juniper or ponderosa: silver trunk and bare limbs
//   ponderosa a cinnamon trunk, crown in clumps up the top half; old ones flat-topped, 14-28 m
//   fir       tiers drooping down a spire, 10-24 m; spruce bluer and narrower
//   aspen     a white trunk, a small crown: gold in October, bare in November
//   oak       Gambel oak: thickets of low lobes, rust in October
//   chamisa   rabbitbrush: grey-green balls with faded yellow tops
#pragma once

#include <array>
#include <functional>
#include <vector>

#include "mountains/landform.hpp"
#include "mountains/surface.hpp"

namespace mountains {

enum class TreeKind : uint8_t { Juniper, Pinyon, Snag, Ponderosa, Fir, Spruce, Aspen, Oak, Chamisa };

struct Lobe { double x, y, z, rx, ry, rz; };   // relative to the trunk's foot
struct Limb { double ax, ay, az, bx, by, bz, r; };

struct Tree {
    double x = 0, z = 0;   // trunk
    double base = 0;       // ground at the trunk
    double height = 0, reach = 0;
    double leanX = 0, leanZ = 0;   // trunk offset per metre up
    double trunkTop = 0, trunkR = 0;
    TreeKind kind = TreeKind::Juniper;
    Material foliage = Material::Juniper, foliage2 = Material::Juniper, wood = Material::Bark;
    double tint = 0;       // -1..1, this tree's own shift of shade
    bool snowy = false;
    uint32_t seed = 0;
    int lobes = 0, limbs = 0;
    std::array<Lobe, 10> lobe{};
    std::array<Limb, 6> limb{};
};

// The stand grids: one candidate tree per 6 m cell, one shrub per 3 m cell.
constexpr double kTreeCell = 6.0;
constexpr double kShrubCell = 3.0;
constexpr double kTreeReach = 8.0;   // farthest a crown reaches from its trunk, m

// Value noise in 3D, -1..1, for eroding crowns.
inline double valueNoise3(double x, double y, double z, uint32_t seed) {
    const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int64_t ix = int64_t(fx), iy = int64_t(fy), iz = int64_t(fz);
    double tx = x - fx, ty = y - fy, tz = z - fz;
    tx = tx * tx * (3.0 - 2.0 * tx);
    ty = ty * ty * (3.0 - 2.0 * ty);
    tz = tz * tz * (3.0 - 2.0 * tz);
    auto v = [&](int64_t a, int64_t b, int64_t c) { return signedUnit(hash2(a + c * 7919, b, seed)); };
    const double x00 = mix(v(ix, iy, iz), v(ix + 1, iy, iz), tx), x10 = mix(v(ix, iy + 1, iz), v(ix + 1, iy + 1, iz), tx);
    const double x01 = mix(v(ix, iy, iz + 1), v(ix + 1, iy, iz + 1), tx), x11 = mix(v(ix, iy + 1, iz + 1), v(ix + 1, iy + 1, iz + 1), tx);
    return mix(mix(x00, x10, ty), mix(x01, x11, ty), tz);
}

namespace detail {
// The tree's shape from its kind, height and seed.
inline void growTree(Tree& t) {
    const uint32_t h = t.seed;
    auto u = [&](int k) { return unit(hash32(h + uint32_t(k) * 0x85ebca6bU)); };
    auto s = [&](int k) { return signedUnit(hash32(h + uint32_t(k) * 0x85ebca6bU)); };
    const double H = t.height;
    auto lobe = [&](double x, double y, double z, double rx, double ry, double rz) {
        if (t.lobes < int(t.lobe.size())) t.lobe[size_t(t.lobes++)] = {x, y, z, rx, ry, rz};
    };
    auto limb = [&](double ax, double ay, double az, double bx, double by, double bz, double r) {
        if (t.limbs < int(t.limb.size())) t.limb[size_t(t.limbs++)] = {ax, ay, az, bx, by, bz, r};
    };
    double R = 0.0;
    switch (t.kind) {
        case TreeKind::Juniper: {
            R = H * (0.42 + 0.12 * u(1));
            t.leanX = 0.18 * s(2); t.leanZ = 0.18 * s(3);
            t.trunkTop = H * 0.45; t.trunkR = 0.12 + 0.03 * H;
            const int n = 2 + int(u(4) * 3.0);
            for (int i = 0; i < n; ++i) {
                const double a = kTau * u(10 + i), r = R * 0.45 * std::sqrt(u(20 + i));
                const double y = H * (0.42 + 0.3 * u(30 + i));
                lobe(std::cos(a) * r + t.leanX * y, y, std::sin(a) * r + t.leanZ * y,
                     R * (0.45 + 0.25 * u(40 + i)), H * (0.28 + 0.14 * u(50 + i)), R * (0.45 + 0.25 * u(60 + i)));
            }
            break;
        }
        case TreeKind::Pinyon: {
            R = H * (0.36 + 0.08 * u(1));
            t.leanX = 0.08 * s(2); t.leanZ = 0.08 * s(3);
            t.trunkTop = H * 0.4; t.trunkR = 0.12 + 0.025 * H;
            const int n = 3 + int(u(4) * 3.0);
            for (int i = 0; i < n; ++i) {
                const double a = kTau * u(10 + i), r = R * 0.35 * std::sqrt(u(20 + i));
                const double y = H * (0.45 + 0.3 * u(30 + i));
                lobe(std::cos(a) * r + t.leanX * y, y, std::sin(a) * r + t.leanZ * y,
                     R * (0.55 + 0.2 * u(40 + i)), H * (0.25 + 0.12 * u(50 + i)), R * (0.55 + 0.2 * u(60 + i)));
            }
            break;
        }
        case TreeKind::Snag: {
            R = H * 0.4;
            t.leanX = 0.2 * s(2); t.leanZ = 0.2 * s(3);
            t.trunkTop = H * 0.85; t.trunkR = 0.14 + 0.02 * H;
            const int n = 3 + int(u(4) * 3.0);
            for (int i = 0; i < n; ++i) {
                const double y0 = H * (0.3 + 0.55 * u(10 + i)), a = kTau * u(20 + i), len = R * (0.35 + 0.45 * u(30 + i));
                limb(t.leanX * y0, y0, t.leanZ * y0, t.leanX * y0 + std::cos(a) * len, y0 + len * (0.3 + 0.6 * u(40 + i)),
                     t.leanZ * y0 + std::sin(a) * len, t.trunkR * (0.55 + 0.25 * u(50 + i)));
            }
            break;
        }
        case TreeKind::Ponderosa: {
            R = H * (0.14 + 0.05 * u(1));
            t.leanX = 0.03 * s(2); t.leanZ = 0.03 * s(3);
            t.trunkTop = H * 0.93; t.trunkR = 0.25 + 0.018 * H;
            const bool old = u(5) < 0.3;
            const int n = 5 + int(u(4) * 4.0);
            for (int i = 0; i < n; ++i) {
                const double g = (double(i) + u(10 + i)) / double(n);          // up the crown
                const double y = H * (0.5 + 0.45 * g);
                const double w = old ? (0.7 + 0.3 * g) : std::sin(kPi * (0.2 + 0.75 * g));   // old: flat-topped
                const double a = kTau * u(20 + i), r = R * 0.5 * u(30 + i);
                lobe(std::cos(a) * r + t.leanX * y, y, std::sin(a) * r + t.leanZ * y,
                     R * (0.45 + 0.4 * w) * (0.8 + 0.4 * u(40 + i)), H * (0.07 + 0.04 * u(50 + i)),
                     R * (0.45 + 0.4 * w) * (0.8 + 0.4 * u(60 + i)));
            }
            break;
        }
        case TreeKind::Fir:
        case TreeKind::Spruce: {
            const bool spruce = t.kind == TreeKind::Spruce;
            R = H * (spruce ? 0.15 : 0.18) * (0.85 + 0.3 * u(1));
            t.leanX = 0.02 * s(2); t.leanZ = 0.02 * s(3);
            t.trunkTop = H * 0.97; t.trunkR = 0.15 + 0.015 * H;
            const int tiers = 7 + int(u(4) * 3.0);
            for (int i = 0; i < tiers; ++i) {
                const double g = (double(i) + 0.5) / double(tiers);
                const double y = H * (0.1 + 0.85 * g);
                const double w = R * std::pow(1.0 - g, 0.85) * (0.8 + 0.35 * u(10 + i));
                // A drooping tier: a flat disc, a little off the trunk.
                lobe(t.leanX * y + 0.15 * w * s(20 + i), y, t.leanZ * y + 0.15 * w * s(30 + i),
                     w, H * 0.055, w);
            }
            lobe(t.leanX * H, H * 0.95, t.leanZ * H, R * 0.12, H * 0.08, R * 0.12);   // the spire
            break;
        }
        case TreeKind::Aspen: {
            R = H * (0.16 + 0.05 * u(1));
            t.leanX = 0.04 * s(2); t.leanZ = 0.04 * s(3);
            t.trunkTop = H * 0.9; t.trunkR = 0.1 + 0.01 * H;
            const int n = 3 + int(u(4) * 3.0);
            for (int i = 0; i < n; ++i) {
                const double y = H * (0.55 + 0.38 * u(10 + i)), a = kTau * u(20 + i), r = R * 0.4 * u(30 + i);
                lobe(std::cos(a) * r + t.leanX * y, y, std::sin(a) * r + t.leanZ * y,
                     R * (0.5 + 0.3 * u(40 + i)), H * (0.12 + 0.06 * u(50 + i)), R * (0.5 + 0.3 * u(60 + i)));
            }
            break;
        }
        case TreeKind::Oak: {
            R = H * (0.8 + 0.4 * u(1));
            t.trunkTop = H * 0.3; t.trunkR = 0.07;
            const int n = 3 + int(u(4) * 4.0);
            for (int i = 0; i < n; ++i) {
                const double a = kTau * u(10 + i), r = R * 0.6 * std::sqrt(u(20 + i));
                lobe(std::cos(a) * r, H * (0.35 + 0.3 * u(30 + i)), std::sin(a) * r,
                     R * (0.35 + 0.2 * u(40 + i)), H * (0.3 + 0.15 * u(50 + i)), R * (0.35 + 0.2 * u(60 + i)));
            }
            break;
        }
        case TreeKind::Chamisa: {
            R = H * (0.6 + 0.3 * u(1));
            t.trunkTop = 0.0; t.trunkR = 0.0;
            const int n = 1 + int(u(4) * 2.0);
            for (int i = 0; i < n; ++i)
                lobe(R * 0.3 * s(10 + i), H * 0.5, R * 0.3 * s(20 + i), R * 0.7, H * 0.5, R * 0.7);
            break;
        }
    }
    t.reach = R;
    for (int i = 0; i < t.lobes; ++i) {
        const Lobe& l = t.lobe[size_t(i)];
        t.reach = std::max(t.reach, std::hypot(l.x, l.z) + std::max(l.rx, l.rz));
    }
    for (int i = 0; i < t.limbs; ++i) {
        const Limb& l = t.limb[size_t(i)];
        t.reach = std::max(t.reach, std::hypot(l.bx, l.bz) + l.r);
    }
    t.reach = std::min(t.reach, kTreeReach);
}
}  // namespace detail

// `ground(x, z, sample)` gives the ground at a point (interpolated by the caller); plants whose crowns
// could reach [x0, x1] x [z0, z1] go into `out`. Shrubs only where `shrubs`.
inline void collectTrees(double x0, double z0, double x1, double z1, uint32_t seed, bool shrubs, bool smallShrubs,
                         const std::function<bool(double, double, Sample&)>& ground, std::vector<Tree>& out) {
    auto grid = [&](double cell, uint32_t salt, bool understory) {
        const int64_t i0 = int64_t(std::floor((x0 - kTreeReach) / cell)), i1 = int64_t(std::floor((x1 + kTreeReach) / cell));
        const int64_t j0 = int64_t(std::floor((z0 - kTreeReach) / cell)), j1 = int64_t(std::floor((z1 + kTreeReach) / cell));
        for (int64_t j = j0; j <= j1; ++j)
            for (int64_t i = i0; i <= i1; ++i) {
                const uint32_t h = hash2(i, j, seed * 0x2545F491U + salt);
                auto u = [&](int k) { return unit(hash32(h + uint32_t(k) * 0x9e3779b9U)); };
                Tree t;
                t.x = (double(i) + 0.1 + 0.8 * u(1)) * cell;
                t.z = (double(j) + 0.1 + 0.8 * u(2)) * cell;
                Sample s;
                if (!ground(t.x, t.z, s)) continue;
                // Continuous cover (a coarse footprint: no crowns-as-dots), and nothing on rock or snow.
                const SurfaceMix m = surfaceMix(s, t.x, t.z, 100.0);
                if (m.rock > 0.5 || m.water > 0.3) continue;
                double cover;
                const double size = 0.6 + 0.8 * u(4) * u(4) + 0.2 * u(7);   // many small, a few big
                if (understory) {
                    if (m.shrub == Material::Chamisa) {
                        if (!smallShrubs) continue;
                        t.kind = TreeKind::Chamisa;
                        t.height = 0.9 * size;
                        t.foliage = Material::Chamisa;
                        t.foliage2 = Material::ChamisaBloom;
                    } else {
                        t.kind = TreeKind::Oak;
                        t.height = 2.6 * size;
                        t.foliage = m.shrub;
                        t.foliage2 = november() ? Material::Bark : (u(8) < 0.5 ? Material::Oak : Material::RedSoil);
                    }
                    t.wood = Material::Bark;
                    cover = m.understory;
                } else {
                    const Material species = m.treeMix > u(3) ? m.treeB : m.treeA;
                    switch (species) {
                        case Material::Juniper:
                        case Material::Pinyon: {
                            const bool juniper = species == Material::Juniper;
                            if (u(5) < 0.035) {   // a dead one, standing for centuries
                                t.kind = TreeKind::Snag;
                                t.height = 4.0 * size;
                                t.wood = Material::Deadwood;
                                break;
                            }
                            t.kind = juniper ? TreeKind::Juniper : TreeKind::Pinyon;
                            t.height = (juniper ? 3.6 : 5.2) * size;
                            t.foliage = species;
                            t.foliage2 = juniper ? (u(6) < 0.3 ? Material::Pinyon : Material::Juniper) : Material::Ponderosa;
                            t.wood = Material::Bark;
                            break;
                        }
                        case Material::Ponderosa:
                            if (u(5) < 0.04) { t.kind = TreeKind::Snag; t.height = 12.0 * size; t.wood = Material::Deadwood; break; }
                            t.kind = TreeKind::Ponderosa;
                            t.height = 19.0 * size;
                            t.foliage = Material::Ponderosa;
                            t.foliage2 = Material::Pinyon;
                            t.wood = Material::PonderosaBark;
                            break;
                        case Material::Fir:
                        case Material::Spruce:
                            t.kind = species == Material::Fir ? TreeKind::Fir : TreeKind::Spruce;
                            t.height = (species == Material::Fir ? 17.0 : 20.0) * size;
                            t.foliage = species;
                            t.foliage2 = species == Material::Fir ? Material::Spruce : Material::Fir;
                            t.wood = Material::Bark;
                            break;
                        case Material::AspenGold:
                        case Material::AspenBare:
                            t.kind = TreeKind::Aspen;
                            t.height = 12.0 * size;
                            t.foliage = species;
                            t.foliage2 = species == Material::AspenGold ? (u(6) < 0.25 ? Material::Grama : Material::AspenGold)
                                                                        : Material::AspenBare;
                            t.wood = Material::AspenTrunk;
                            break;
                        default:
                            continue;
                    }
                    cover = m.cover;
                }
                t.seed = h;
                t.tint = signedUnit(hash32(h + 99U));
                detail::growTree(t);
                // Kept with the probability that makes the crowns cover what the canopy says.
                const double crown = kPi * t.reach * t.reach * 0.7;
                if (u(9) >= cover * cell * cell / std::max(crown, 0.5)) continue;
                t.base = s.height;
                t.snowy = m.snow > 0.4 && t.kind != TreeKind::Aspen;
                out.push_back(t);
            }
    };
    grid(kTreeCell, 17U, false);
    if (shrubs) grid(kShrubCell, 4243U, true);
}

// Visits every voxel of `t` inside the box [lo, hi) (world metres; voxels of edge v on the lattice
// at multiples of v), as (voxel x, voxel y, voxel z, material, shade).
template <typename Visit>
inline void stampTree(const Tree& t, double v, const double lo[3], const double hi[3], Visit&& visit) {
    const double top = t.base + t.height * 1.1;
    const double reach = t.reach + v;
    const int64_t ix0 = int64_t(std::floor(std::max(lo[0], t.x - reach) / v)), ix1 = int64_t(std::ceil(std::min(hi[0], t.x + reach) / v));
    const int64_t iz0 = int64_t(std::floor(std::max(lo[2], t.z - reach) / v)), iz1 = int64_t(std::ceil(std::min(hi[2], t.z + reach) / v));
    const int64_t iy0 = int64_t(std::floor(std::max(lo[1], t.base - 0.6) / v)), iy1 = int64_t(std::ceil(std::min(hi[1], top) / v));
    if (ix0 >= ix1 || iz0 >= iz1 || iy0 >= iy1) return;
    const double trunkR = std::max(v * 0.5, t.trunkR);
    // Erosion noise: finer than a lobe, never finer than the voxels drawing it.
    const double noiseScale = 1.0 / std::max({v * 1.5, 0.45, t.height * 0.08});
    const double ragged = (t.kind == TreeKind::Fir || t.kind == TreeKind::Spruce) ? 0.28 : 0.42;
    for (int64_t iz = iz0; iz < iz1; ++iz)
        for (int64_t ix = ix0; ix < ix1; ++ix) {
            const double px = (double(ix) + 0.5) * v - t.x, pz = (double(iz) + 0.5) * v - t.z;
            for (int64_t iy = iy0; iy < iy1; ++iy) {
                const double py = (double(iy) + 0.5) * v - t.base;
                if (py < -0.6) continue;
                // Foliage: the strongest lobe, its edge eroded.
                double field = -1.0;
                double lobeUp = 0.0;   // how far up its lobe this voxel sits, -1..1
                for (int i = 0; i < t.lobes; ++i) {
                    const Lobe& l = t.lobe[size_t(i)];
                    const double dx = (px - l.x) / l.rx, dy = (py - l.y) / l.ry, dz = (pz - l.z) / l.rz;
                    const double f = 1.0 - (dx * dx + dy * dy + dz * dz);
                    if (f > field) { field = f; lobeUp = dy; }
                }
                const double n = valueNoise3(px * noiseScale, py * noiseScale, pz * noiseScale, t.seed);
                const bool crown = field + ragged * n > 0.12;
                // Wood: the leaning trunk, and limbs.
                bool wood = false;
                if (py < t.trunkTop) {
                    const double tx = px - t.leanX * py, tz = pz - t.leanZ * py;
                    const double r = trunkR * (1.0 - 0.45 * py / std::max(t.trunkTop, 1e-3));
                    wood = tx * tx + tz * tz < r * r;
                }
                for (int i = 0; i < t.limbs && !wood; ++i) {
                    const Limb& l = t.limb[size_t(i)];
                    const double ex = l.bx - l.ax, ey = l.by - l.ay, ez = l.bz - l.az;
                    const double k = clamp01(((px - l.ax) * ex + (py - l.ay) * ey + (pz - l.az) * ez) / (ex * ex + ey * ey + ez * ez));
                    const double qx = l.ax + ex * k - px, qy = l.ay + ey * k - py, qz = l.az + ez * k - pz;
                    const double r = std::max(v * 0.5, l.r * (1.0 - 0.6 * k));
                    wood = qx * qx + qy * qy + qz * qz < r * r;
                }
                if (!crown && !wood) continue;
                const uint32_t vh = hash2(ix * 131 + iy, iz, t.seed);
                Material m;
                double lit;
                if (crown && !(wood && field < 0.3)) {
                    // Two greens per tree, in patches; darker inside, lighter toward the top and rim.
                    m = valueNoise3(px * noiseScale * 0.7 + 9.0, py * noiseScale * 0.7, pz * noiseScale * 0.7, t.seed + 5) > 0.25
                            ? t.foliage2 : t.foliage;
                    lit = 0.25 + 0.45 * (1.0 - clamp01(field * 1.6)) + 0.25 * clamp01(py / t.height) + 0.15 * lobeUp;
                    if (t.kind == TreeKind::Chamisa && lobeUp < -0.2) m = Material::Chamisa;
                    // Snow on the tops of the lobes and tiers, above the snowline.
                    if (t.snowy && lobeUp > 0.35 && unit(hash32(vh)) < 0.75) { m = Material::Snow; lit = 0.9; }
                } else {
                    m = t.wood;
                    lit = 0.35 + 0.3 * unit(vh);
                }
                lit += 0.18 * t.tint + (unit(vh) - 0.5) * 0.25;
                visit(ix, iy, iz, m, std::clamp(int(lit * kShades), 0, kShades - 1));
            }
        }
}

}  // namespace mountains
