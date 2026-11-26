// Props (plan D18-D22): the small things on the ground -- boulders, cobbles, shrubs, cactus, deadfall --
// each a loose chunk with its own position, rotation and scale, off the rings' lattice.
//
// A library of shapes is built once at startup: a few voxel variants of each kind, at a resolution of
// its own (16, 32 or 64), with materials and shades baked in. A placed prop is one of them, turned,
// tilted, sized and sunk into the ground; thousands of props share a few dozen blobs.
//
// Where props stand is a pure function of world position: a jittered grid per kind, each cell's
// candidate kept by a density read from the exact landform and surface mix at its anchor. A prop
// belongs to the column its anchor is in; ring k's column emits the props whose lod is k or more, so
// ring 0 and ring 1 agree on every prop and the hand-off follows the rings'.
//
//   boulder    granite, gneiss (cores), basalt (mesa scarps), sandstone (basin); rounded, faceted, slab
//   cobble     the same stones, 0.15-0.6 m, everywhere, thicker on rock and scarps
//   chamisa    rabbitbrush, grey-green with faded yellow tops (ring 0; ring 1 still stamps it)
//   snakeweed  low yellow-green domes in the piedmont grassland
//   cholla     tree cholla: jointed arms, yellow fruit at the tips
//   yucca      a rosette of blades, some with last summer's stalk
//   log        deadfall in the ponderosa and the fir, lying along the slope
#pragma once

#include <array>
#include <cmath>
#include <vector>

#include "mountains/landform.hpp"
#include "mountains/surface.hpp"
#include "mountains/trees.hpp"
#include "utils/material.h"
#include "utils/tile_table.h"
#include "utils/voxel_management.h"

namespace mountains {

enum class PropKind : uint8_t { Boulder, Cobble, Chamisa, Snakeweed, Cholla, Yucca, Log, Count };
constexpr int kPropKinds = int(PropKind::Count);

// A shape's placement data, in its own voxels: the footprint's centre (cx, cz), the lowest and highest
// solid voxel, and the farthest a voxel reaches from the centre horizontally.
struct PropShape {
    PropKind kind = PropKind::Boulder;
    Material material = Material::Granite;   // the stone or wood a variant is made of
    int res = 16;
    double cx = 0, cz = 0, bottom = 0, top = 0, radius = 1;
};

struct PropLibrary {
    std::vector<PropShape> shapes;
    std::vector<projv::utils::TileTable> tiles;   // parallel to shapes; moved into blobs by the caller
    std::array<std::array<std::vector<int>, kMaterialCount>, kPropKinds> variants;   // [kind][material]

    const std::vector<int>& of(PropKind k, Material m) const { return variants[size_t(k)][size_t(m)]; }
};

// One placed prop: a chunk's header.
struct PropInstance {
    int shape = 0;
    double px = 0, py = 0, pz = 0;              // the chunk's corner, world
    double qw = 1, qx = 0, qy = 0, qz = 0;      // its rotation
    double size = 1;                            // its edge, m (voxels are size / res)
};

namespace props {

// ---- Small vector and quaternion maths -------------------------------------------------------------

struct V3 { double x = 0, y = 0, z = 0; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(V3 a) { const double l = length(a); return l > 1e-12 ? a * (1.0 / l) : V3{0, 1, 0}; }

struct Q { double w = 1, x = 0, y = 0, z = 0; };
inline Q operator*(Q a, Q b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
inline Q axisAngle(V3 axis, double angle) {
    axis = normalize(axis);
    const double s = std::sin(angle * 0.5);
    return {std::cos(angle * 0.5), axis.x * s, axis.y * s, axis.z * s};
}
// The shortest rotation taking unit vector a to unit vector b.
inline Q fromTo(V3 a, V3 b) {
    const double d = dot(a, b);
    if (d > 0.999999) return {};
    const V3 c = cross(a, b);
    Q q{1.0 + d, c.x, c.y, c.z};
    const double l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return {q.w / l, q.x / l, q.y / l, q.z / l};
}
inline V3 rotate(Q q, V3 v) {
    const V3 u{q.x, q.y, q.z};
    const V3 t = cross(u, v) * 2.0;
    return v + t * q.w + cross(u, t);
}

// ---- Shapes -----------------------------------------------------------------------------------------

// A shape's voxels while it is built: -1 empty, else a palette slot.
struct Grid {
    int res;
    std::vector<int16_t> v;
    explicit Grid(int r) : res(r), v(size_t(r) * size_t(r) * size_t(r), -1) {}
    bool inside(int x, int y, int z) const { return x >= 0 && y >= 0 && z >= 0 && x < res && y < res && z < res; }
    int16_t& at(int x, int y, int z) { return v[(size_t(z) * size_t(res) + size_t(y)) * size_t(res) + size_t(x)]; }
    int16_t get(int x, int y, int z) const { return v[(size_t(z) * size_t(res) + size_t(y)) * size_t(res) + size_t(x)]; }
    void set(int x, int y, int z, int slot) { if (inside(x, y, z)) at(x, y, z) = int16_t(slot); }
};

inline int shadeOf(double lit) { return std::clamp(int(lit * kShades), 0, kShades - 1); }

// A capsule from a to b of radius r (voxel units), painted by `paint(point, t along it)`.
template <typename Paint>
inline void capsule(Grid& g, V3 a, V3 b, double r, Paint&& paint) {
    const int x0 = std::max(0, int(std::floor(std::min(a.x, b.x) - r))), x1 = std::min(g.res - 1, int(std::ceil(std::max(a.x, b.x) + r)));
    const int y0 = std::max(0, int(std::floor(std::min(a.y, b.y) - r))), y1 = std::min(g.res - 1, int(std::ceil(std::max(a.y, b.y) + r)));
    const int z0 = std::max(0, int(std::floor(std::min(a.z, b.z) - r))), z1 = std::min(g.res - 1, int(std::ceil(std::max(a.z, b.z) + r)));
    const V3 e = b - a;
    const double ee = std::max(dot(e, e), 1e-9);
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const V3 p{x + 0.5, y + 0.5, z + 0.5};
                const double t = clamp01(dot(p - a, e) / ee);
                const V3 q = a + e * t - p;
                if (dot(q, q) < r * r) g.at(x, y, z) = int16_t(paint(p, t));
            }
}

// Stones. Style 0: rounded (granite weathers to domes and eggs); 1: faceted (cut by joint planes, as
// basalt and gneiss break); 2: a slab.
inline void makeStone(Grid& g, uint32_t seed, int style, Material rock, const Palette& pal) {
    const int R = g.res;
    auto u = [&](int k) { return unit(hash32(seed + uint32_t(k) * 0x9e3779b9U)); };
    double a = R * 0.40, b, c, p;
    switch (style) {
        case 0: b = R * (0.26 + 0.12 * u(1)); c = R * (0.28 + 0.10 * u(2)); p = 2.0 + 1.0 * u(3); break;
        case 1: b = R * (0.28 + 0.10 * u(1)); c = R * (0.30 + 0.08 * u(2)); p = 3.0 + 1.5 * u(3); break;
        default: b = R * (0.12 + 0.08 * u(1)); c = R * (0.26 + 0.12 * u(2)); p = 2.4 + 0.8 * u(3); break;
    }
    const double cy = b * 1.15 + 1.0;
    struct Plane { V3 n; double d; };
    std::vector<Plane> planes;
    const int cuts = style == 1 ? 5 + int(u(4) * 4.0) : (style == 2 ? 2 : int(u(4) * 2.5));
    for (int i = 0; i < cuts; ++i) {
        const double az = kTau * u(10 + i), el = std::asin(2.0 * u(20 + i) - 1.0);
        planes.push_back({{std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)},
                          (style == 1 ? 0.62 : 0.78) + 0.25 * u(30 + i)});
    }
    const double inv = 1.0 / p;
    for (int z = 0; z < R; ++z)
        for (int y = 0; y < R; ++y)
            for (int x = 0; x < R; ++x) {
                const V3 q{(x + 0.5 - R * 0.5) / a, (y + 0.5 - cy) / b, (z + 0.5 - R * 0.5) / c};
                double f = std::pow(std::pow(std::abs(q.x), p) + std::pow(std::abs(q.y), p) + std::pow(std::abs(q.z), p), inv) - 1.0;
                for (const Plane& pl : planes) f = std::max(f, dot(pl.n, q) - pl.d);
                const double n1 = valueNoise3(q.x * 2.2, q.y * 2.2, q.z * 2.2, seed + 1);
                const double n2 = valueNoise3(q.x * 5.5, q.y * 5.5, q.z * 5.5, seed + 2);
                f += 0.10 * n1 + 0.04 * n2;
                if (f >= 0.0) continue;
                Material m = rock;
                double lit = 0.52 + 0.18 * q.y + 0.22 * valueNoise3(q.x * 4.0, q.y * 4.0, q.z * 4.0, seed + 3);
                if (q.y < -0.55) lit -= 0.18;   // the soil's stain where it sits
                const uint32_t vh = hash32(seed ^ uint32_t(x * 73856093) ^ uint32_t(y * 19349663) ^ uint32_t(z * 83492791));
                if (rock == Material::Sediment) {
                    lit += 0.14 * std::sin(q.y * 9.0 + 1.5 * n1);   // bedding
                    if (unit(vh) < 0.12) m = Material::RedSoil;
                } else if (rock == Material::Basalt) {
                    lit -= 0.08 * (1.0 - clamp01(q.y + 0.5));     // desert varnish, darker low
                    if (q.y > 0.1 && valueNoise3(q.x * 3.5, q.y * 3.5, q.z * 3.5, seed + 4) > 0.42) m = Material::Lichen;
                } else if (q.y > -0.1 && valueNoise3(q.x * 3.0, q.y * 3.0, q.z * 3.0, seed + 4) > 0.30 + 0.25 * (1.0 - q.y)) {
                    m = Material::Lichen;   // orange crust on the tops of granite and gneiss
                }
                lit += (unit(vh) - 0.5) * 0.18;
                g.at(x, y, z) = int16_t(pal.at(m, shadeOf(lit)));
            }
}

// Chamisa: a loose dome of grey-green stems, the tops faded yellow.
inline void makeChamisa(Grid& g, uint32_t seed, const Palette& pal) {
    const int R = g.res;
    auto u = [&](int k) { return unit(hash32(seed + uint32_t(k) * 0x9e3779b9U)); };
    const double H = R * (0.55 + 0.15 * u(1)), W = R * 0.44;
    struct Ball { V3 c; double r; };
    std::vector<Ball> balls{{{R * 0.5, H * 0.5, R * 0.5}, W * 0.62}};
    const int n = 6 + int(u(2) * 5.0);
    for (int i = 0; i < n; ++i) {
        const double az = kTau * u(10 + i), r = W * 0.55 * std::sqrt(u(20 + i));
        balls.push_back({{R * 0.5 + std::cos(az) * r, H * (0.45 + 0.3 * u(30 + i)), R * 0.5 + std::sin(az) * r},
                         R * (0.15 + 0.08 * u(40 + i))});
    }
    // Stems first: from the root out to each ball, grey wood.
    for (size_t i = 1; i < balls.size(); ++i)
        capsule(g, {R * 0.5, 0.0, R * 0.5}, balls[i].c, 0.75, [&](V3, double t) {
            return pal.at(Material::Deadwood, shadeOf(0.35 + 0.3 * t));
        });
    for (int z = 0; z < R; ++z)
        for (int y = 0; y < R; ++y)
            for (int x = 0; x < R; ++x) {
                const V3 p{x + 0.5, y + 0.5, z + 0.5};
                double field = -1.0;
                for (const Ball& b : balls) {
                    const V3 d = (p - b.c) * (1.0 / b.r);
                    field = std::max(field, 1.0 - dot(d, d));
                }
                // Stems run up: the noise is stretched vertically, so the gaps are vertical too.
                const double nz = valueNoise3(p.x / 1.3, p.y / 3.0, p.z / 1.3, seed + 5);
                if (field + 0.55 * nz <= 0.22) continue;
                const double up = p.y / H;
                const bool bloom = up > 0.7 && valueNoise3(p.x / 2.5, p.y / 2.5, p.z / 2.5, seed + 6) > 0.15;
                const Material m = bloom ? Material::ChamisaBloom : Material::Chamisa;
                const uint32_t vh = hash32(seed ^ uint32_t(x * 73856093) ^ uint32_t(y * 19349663) ^ uint32_t(z * 83492791));
                const double lit = 0.25 + 0.3 * up + 0.2 * (1.0 - clamp01(field * 1.5)) + (unit(vh) - 0.5) * 0.25;
                g.at(x, y, z) = int16_t(pal.at(m, shadeOf(lit)));
            }
}

// Broom snakeweed: a low, dense dome of fine upright stems, yellow at the top.
inline void makeSnakeweed(Grid& g, uint32_t seed, const Palette& pal) {
    const int R = g.res;
    auto u = [&](int k) { return unit(hash32(seed + uint32_t(k) * 0x9e3779b9U)); };
    const double H = R * (0.42 + 0.15 * u(1)), W = R * (0.38 + 0.08 * u(2));
    for (int z = 0; z < R; ++z)
        for (int y = 0; y < R; ++y)
            for (int x = 0; x < R; ++x) {
                const V3 p{x + 0.5, y + 0.5, z + 0.5};
                const V3 d{(p.x - R * 0.5) / W, p.y / H, (p.z - R * 0.5) / W};
                const double field = 1.0 - dot(d, d);
                const double nz = valueNoise3(p.x / 1.0, p.y / 2.6, p.z / 1.0, seed + 5) + 0.6 * valueNoise3(p.x / 3.5, p.y / 3.5, p.z / 3.5, seed + 6);
                if (field + 0.45 * nz <= 0.15) continue;
                const double up = d.y;
                const Material m = up > 0.8 && nz > 0.25 ? Material::ChamisaBloom : Material::Snakeweed;
                const uint32_t vh = hash32(seed ^ uint32_t(x * 73856093) ^ uint32_t(y * 19349663) ^ uint32_t(z * 83492791));
                g.at(x, y, z) = int16_t(pal.at(m, shadeOf(0.2 + 0.4 * up + (unit(vh) - 0.5) * 0.3)));
            }
}

// Tree cholla: a short trunk and jointed arms branching up and out, knobbed, yellow fruit at the tips.
inline void makeCholla(Grid& g, uint32_t seed, const Palette& pal) {
    const int R = g.res;
    uint32_t k = 0;
    auto u = [&]() { return unit(hash32(seed + (k++) * 0x9e3779b9U)); };
    const double r0 = std::max(0.85, R * 0.03);
    std::vector<V3> tips;
    auto joint = [&](V3 a, V3 b, double r, bool old) {
        const double len = length(b - a);
        capsule(g, a, b, r * 1.25, [&](V3 p, double t) {
            // Knobs: tubercles along the joint, darker in their grooves.
            const double knob = 0.5 + 0.5 * std::sin(t * len * 2.2 + p.x * 1.7 + p.z * 1.3);
            const Material m = old && t < 0.8 ? Material::Deadwood : Material::Cholla;
            return pal.at(m, shadeOf(0.3 + 0.35 * knob + 0.2 * p.y / R));
        });
    };
    struct Arm { V3 a, dir; double len; int depth; };
    std::vector<Arm> arms;
    const V3 base{R * 0.5, 0.0, R * 0.5};
    const V3 trunkTop{R * 0.5 + (u() - 0.5) * R * 0.06, R * (0.18 + 0.1 * u()), R * 0.5 + (u() - 0.5) * R * 0.06};
    joint(base, trunkTop, r0 * 1.3, true);
    const int first = 3 + int(u() * 3.0);
    for (int i = 0; i < first; ++i) {
        const double az = kTau * u();
        arms.push_back({trunkTop, normalize({std::cos(az) * 0.8, 0.6 + 0.5 * u(), std::sin(az) * 0.8}), R * (0.12 + 0.06 * u()), 0});
    }
    while (!arms.empty()) {
        const Arm arm = arms.back();
        arms.pop_back();
        V3 b = arm.a + arm.dir * arm.len;
        b.x = std::clamp(b.x, 2.0, R - 2.0); b.z = std::clamp(b.z, 2.0, R - 2.0); b.y = std::clamp(b.y, 1.0, R - 2.0);
        joint(arm.a, b, r0, false);
        if (arm.depth >= 3 || (arm.depth >= 2 && u() < 0.4)) { tips.push_back(b); continue; }
        const int kids = 1 + int(u() * 2.2);
        for (int i = 0; i < kids; ++i) {
            const double az = kTau * u();
            const V3 d = normalize(arm.dir + V3{std::cos(az) * 0.9, 0.35 + 0.3 * u(), std::sin(az) * 0.9});
            arms.push_back({b, d, arm.len * (0.75 + 0.2 * u()), arm.depth + 1});
        }
    }
    for (const V3& t : tips)
        if (u() < 0.7) capsule(g, t, t + V3{0, 0.8, 0}, 1.1, [&](V3, double) { return pal.at(Material::ChamisaBloom, shadeOf(0.4 + 0.4 * u())); });
}

// Yucca: a rosette of stiff blades from a short base; old blades dead at the bottom; often a dry stalk.
inline void makeYucca(Grid& g, uint32_t seed, const Palette& pal) {
    const int R = g.res;
    uint32_t k = 0;
    auto u = [&]() { return unit(hash32(seed + (k++) * 0x9e3779b9U)); };
    const V3 c{R * 0.5, 1.5, R * 0.5};
    const int blades = 40 + int(u() * 20.0);
    for (int i = 0; i < blades; ++i) {
        const double az = kTau * u(), el = (12.0 + 76.0 * std::pow(u(), 0.7)) * kPi / 180.0;
        const double len = R * (0.28 + 0.14 * u());
        const bool dead = el < 0.45 && u() < 0.7;
        const V3 dir{std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)};
        V3 prev = c;
        const int steps = 6;
        for (int s = 1; s <= steps; ++s) {
            const double t = double(s) / steps;
            V3 p = c + dir * (len * t);
            p.y -= t * t * len * 0.18 * std::cos(el) * (dead ? 2.5 : 1.0);   // the blade bends under itself
            capsule(g, prev, p, 0.62, [&](V3, double) {
                const Material m = dead ? (u() < 0.5 ? Material::Grama : Material::Deadwood) : Material::Yucca;
                return pal.at(m, shadeOf(0.3 + 0.45 * t + (u() - 0.5) * 0.2));
            });
            prev = p;
        }
    }
    if (u() < 0.55) {   // last summer's flower stalk, dry, with pods
        const V3 top{R * 0.5 + (u() - 0.5) * 2.0, R * (0.8 + 0.15 * u()), R * 0.5 + (u() - 0.5) * 2.0};
        capsule(g, c, top, 0.7, [&](V3, double) { return pal.at(Material::Deadwood, shadeOf(0.4 + 0.2 * u())); });
        for (int i = 0; i < 6; ++i) {
            const double t = 0.7 + 0.28 * u(), az = kTau * u();
            const V3 p = c + (top - c) * t + V3{std::cos(az) * 1.3, 0.0, std::sin(az) * 1.3};
            capsule(g, p, p + V3{0, 1.2, 0}, 0.9, [&](V3, double) { return pal.at(Material::Bark, shadeOf(0.3 + 0.3 * u())); });
        }
    }
}

// A fallen log along x: tapered, a little bent, broken ends, a few branch stubs. Old ones silver.
inline void makeLog(Grid& g, uint32_t seed, Material wood, const Palette& pal) {
    const int R = g.res;
    uint32_t k = 0;
    auto u = [&]() { return unit(hash32(seed + (k++) * 0x9e3779b9U)); };
    const double len = R * (0.78 + 0.16 * u()), r0 = R * (0.045 + 0.025 * u());
    const double xa = R * 0.5 - len * 0.5, xb = R * 0.5 + len * 0.5;
    const double cy = r0 + 1.0, bend = (u() - 0.5) * R * 0.06;
    auto axisZ = [&](double x) { const double t = (x - xa) / len; return R * 0.5 + bend * std::sin(kPi * t); };
    for (int z = 0; z < R; ++z)
        for (int y = 0; y < R; ++y)
            for (int x = 0; x < R; ++x) {
                const V3 p{x + 0.5, y + 0.5, z + 0.5};
                const double t = (p.x - xa) / len;
                // Broken ends: the end planes are ragged.
                const double rag = 0.06 * valueNoise3(p.y / 1.5, p.z / 1.5, 0.0, seed + 7);
                if (t < rag + 0.0 || t > 1.0 + rag) continue;
                const double r = r0 * (1.0 - 0.35 * t) * (1.0 + 0.08 * valueNoise3(p.x / 3.0, p.y / 3.0, p.z / 3.0, seed + 8));
                const double dy = p.y - cy, dz = p.z - axisZ(p.x);
                const double d = std::sqrt(dy * dy + dz * dz);
                if (d > r) continue;
                const bool surface = d > r - 1.2;
                // Cracks run along the grain: noise stretched down the log.
                const double grain = valueNoise3(p.x / 7.0, p.y / 0.9, p.z / 0.9, seed + 9);
                Material m = surface ? wood : Material::Deadwood;
                double lit = 0.45 + 0.25 * (dy / r) + 0.15 * grain;
                if (surface && grain < -0.45) lit -= 0.25;
                if (!surface) lit += 0.2;   // the pale heartwood at a broken end
                g.at(x, y, z) = int16_t(pal.at(m, shadeOf(lit + (u() - 0.5) * 0.12)));
            }
    const int stubs = 2 + int(u() * 3.0);
    for (int i = 0; i < stubs; ++i) {
        const double x = xa + len * (0.2 + 0.6 * u()), az = kTau * u();
        const V3 a{x, cy, axisZ(x)};
        const V3 dir = normalize({0.3 * (u() - 0.5), std::sin(az), std::cos(az)});
        capsule(g, a, a + dir * (r0 * (1.6 + 1.6 * u())), std::max(0.7, r0 * 0.3), [&](V3, double t) {
            return pal.at(Material::Deadwood, shadeOf(0.4 + 0.2 * t));
        });
    }
}

// The voxels as a tile table, and the shape's placement data from them.
inline projv::utils::TileTable toTiles(const Grid& g, PropShape& shape) {
    const int R = g.res;
    auto map = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(R));
    double sx = 0, sz = 0, n = 0;
    int lo = R, hi = -1;
    for (int z = 0; z < R; ++z)
        for (int y = 0; y < R; ++y)
            for (int x = 0; x < R; ++x) {
                const int16_t s = g.get(x, y, z);
                if (s < 0) continue;
                projv::utils::brickMapSetVoxel(*map, x, y, z, uint8_t(s));
                lo = std::min(lo, y);
                hi = std::max(hi, y);
                if (y <= lo + 2) { sx += x + 0.5; sz += z + 0.5; n += 1.0; }
            }
    shape.res = R;
    shape.bottom = lo;
    shape.top = hi + 1;
    // The footprint's centre: where it touches the ground, not where its mass is (a cholla leans).
    shape.cx = n > 0 ? sx / n : R * 0.5;
    shape.cz = n > 0 ? sz / n : R * 0.5;
    double reach = 0.5;
    for (int z = 0; z < R; ++z)
        for (int y = 0; y < R; ++y)
            for (int x = 0; x < R; ++x)
                if (g.get(x, y, z) >= 0) reach = std::max(reach, std::hypot(x + 0.5 - shape.cx, z + 0.5 - shape.cz));
    shape.radius = reach;
    projv::ChunkHeader hdr;
    hdr.position = projv::core::vec3(0.0f);
    hdr.scale = float(R);
    hdr.voxelScale = 1.0f;
    hdr.resolution = uint32_t(R);
    hdr.rotation = projv::core::quat(1, 0, 0, 0);
    projv::Chunk chunk = projv::utils::createChunk(hdr);
    projv::utils::updateChunkFromBrickMap(chunk, *map);
    std::vector<uint8_t> materials;
    projv::utils::bakeMaterialsFromBrickMap(chunk.geometryData, materials, *map);
    return projv::utils::buildTileTable(chunk.geometryData, materials, R);
}

}  // namespace props

// Every variant of every kind. Deterministic in `seed`; a few hundred milliseconds.
inline PropLibrary buildPropLibrary(uint32_t seed, const Palette& pal) {
    PropLibrary lib;
    auto add = [&](PropKind kind, Material material, int res, auto&& make) {
        props::Grid g(res);
        make(g);
        PropShape shape;
        shape.kind = kind;
        shape.material = material;
        lib.tiles.push_back(props::toTiles(g, shape));
        lib.variants[size_t(kind)][size_t(material)].push_back(int(lib.shapes.size()));
        lib.shapes.push_back(shape);
    };
    uint32_t n = seed * 0x68E31DA4U + 11U;
    auto next = [&]() { return n = hash32(n + 0x9e3779b9U); };
    const Material stones[] = {Material::Granite, Material::Gneiss, Material::Basalt, Material::Sediment};
    for (Material rock : stones) {
        // Granite weathers round; basalt and gneiss break along joints; sandstone in slabs and blocks.
        auto style = [&](int i) {
            switch (rock) {
                case Material::Granite: return i < 6 ? 0 : (i < 8 ? 1 : 2);
                case Material::Basalt: return i < 6 ? 1 : (i < 8 ? 2 : 0);
                case Material::Gneiss: return i < 4 ? 1 : (i < 7 ? 0 : 2);
                default: return i < 4 ? 2 : (i < 7 ? 1 : 0);
            }
        };
        for (int i = 0; i < 9; ++i) {
            const uint32_t s = next();
            add(PropKind::Boulder, rock, 32, [&](props::Grid& g) { props::makeStone(g, s, style(i), rock, pal); });
        }
        for (int i = 0; i < 6; ++i) {
            const uint32_t s = next();
            add(PropKind::Cobble, rock, 16, [&](props::Grid& g) { props::makeStone(g, s, style(i + 2) , rock, pal); });
        }
    }
    for (int i = 0; i < 8; ++i) {
        const uint32_t s = next();
        add(PropKind::Chamisa, Material::Chamisa, 16, [&](props::Grid& g) { props::makeChamisa(g, s, pal); });
    }
    for (int i = 0; i < 6; ++i) {
        const uint32_t s = next();
        add(PropKind::Snakeweed, Material::Snakeweed, 16, [&](props::Grid& g) { props::makeSnakeweed(g, s, pal); });
    }
    for (int i = 0; i < 6; ++i) {
        const uint32_t s = next();
        add(PropKind::Cholla, Material::Cholla, 32, [&](props::Grid& g) { props::makeCholla(g, s, pal); });
    }
    for (int i = 0; i < 6; ++i) {
        const uint32_t s = next();
        add(PropKind::Yucca, Material::Yucca, 32, [&](props::Grid& g) { props::makeYucca(g, s, pal); });
    }
    for (Material wood : {Material::Deadwood, Material::PonderosaBark, Material::Bark}) {
        for (int i = 0; i < 4; ++i) {
            const uint32_t s = next();
            add(PropKind::Log, wood, 64, [&](props::Grid& g) { props::makeLog(g, s, wood, pal); });
        }
    }
    return lib;
}

// The props whose anchors fall in [x0, x0 + c) x [z0, z0 + c) and that ring `ring` draws (lod >= ring).
// `ground(x, z, sample)` gives the ground at a point; `trees`: the plants around the column, so nothing
// stands in a trunk.
template <typename Ground>
inline void collectProps(uint32_t seed, const PropLibrary& lib, int ring, double x0, double z0, double c, Ground&& ground,
                         const std::vector<Tree>& trees, std::vector<PropInstance>& out) {
    using namespace props;
    struct Rule { PropKind kind; double cell; uint32_t salt; int lod; };
    // Boulders are drawn to ring 2 when big (lod by size, below); the rest by kind.
    static const Rule rules[] = {
        {PropKind::Boulder, 5.0, 101U, 2}, {PropKind::Cobble, 1.6, 102U, 0},    {PropKind::Chamisa, 3.0, 103U, 0},
        {PropKind::Snakeweed, 2.0, 104U, 0}, {PropKind::Cholla, 7.0, 105U, 1}, {PropKind::Yucca, 5.0, 106U, 0},
        {PropKind::Log, 11.0, 107U, 1},
    };
    for (const Rule& rule : rules) {
        if (rule.lod < ring) continue;
        const int64_t i0 = int64_t(std::floor(x0 / rule.cell)), i1 = int64_t(std::floor((x0 + c) / rule.cell));
        const int64_t j0 = int64_t(std::floor(z0 / rule.cell)), j1 = int64_t(std::floor((z0 + c) / rule.cell));
        for (int64_t j = j0; j <= j1; ++j)
            for (int64_t i = i0; i <= i1; ++i) {
                const uint32_t h = hash2(i, j, seed * 0x2545F491U + rule.salt);
                auto u = [&](int k) { return unit(hash32(h + uint32_t(k) * 0x9e3779b9U)); };
                const double ax = (double(i) + u(1)) * rule.cell, az = (double(j) + u(2)) * rule.cell;
                if (ax < x0 || ax >= x0 + c || az < z0 || az >= z0 + c) continue;
                Sample s;
                if (!ground(ax, az, s)) continue;
                const SurfaceMix m = surfaceMix(s, ax, az, 100.0);
                if (m.water > 0.3) continue;
                const double slope = std::sqrt(s.gx * s.gx + s.gz * s.gz);
                const double e = s.height;
                double density = 0.0;   // per m^2
                double size = 1.0;      // m: a stone's or shrub's width, a log's length
                Material material = Material::Granite;
                const bool stone = rule.kind == PropKind::Boulder || rule.kind == PropKind::Cobble;
                if (stone) {
                    material = s.basalt > 0.5 ? Material::Basalt
                             : (s.core > 0.5 ? (m.rockMix > u(4) ? m.rockB : m.rockA)
                                             : (s.badland > 0.4 ? Material::Sediment : (u(4) < 0.6 ? Material::Granite : Material::Gneiss)));
                    if (material != Material::Granite && material != Material::Gneiss && material != Material::Basalt)
                        material = Material::Sediment;
                }
                if (m.snow > 0.6 && !stone) continue;
                switch (rule.kind) {
                    case PropKind::Boulder:
                        density = 0.0025 * s.core + 0.035 * m.rock + 0.06 * s.basalt * smoothstep(0.15, 0.6, slope) +
                                  0.03 * s.cirque + 0.004 * smoothstep(0.3, 0.8, slope);
                        size = 0.7 + 2.8 * std::pow(u(5), 2.4);
                        // lod by size: a 3 m boulder is drawn to 512 m, a 1 m one to 128 m.
                        if ((size < 1.3 ? 0 : (size < 2.4 ? 1 : 2)) < ring) continue;
                        break;
                    case PropKind::Cobble:
                        density = 0.05 + 0.3 * m.rock + 0.15 * smoothstep(0.2, 0.8, slope) +
                                  0.35 * s.basalt * smoothstep(0.1, 0.5, slope);
                        if (s.core < 0.5 && s.basalt < 0.5) density *= 0.6;
                        size = 0.14 + 0.45 * std::pow(u(5), 2.0);
                        break;
                    case PropKind::Chamisa: {
                        if (m.shrub != Material::Chamisa) continue;
                        size = 0.6 + 0.8 * u(5) * u(5) + 0.2 * u(6);
                        const double crown = kPi * 0.25 * size * size * 0.8;
                        density = m.understory / crown;
                        break;
                    }
                    case PropKind::Snakeweed:
                        density = 0.08 * (1.0 - smoothstep(2200.0, 2400.0, e)) * (1.0 - m.rock) * (1.0 - m.cover) *
                                  clamp01(0.5 + 1.5 * fbm(ax / 90.0, az / 90.0, 2, 141));
                        size = 0.3 + 0.35 * u(5);
                        break;
                    case PropKind::Cholla:
                        density = 0.003 * (1.0 - smoothstep(2080.0, 2220.0, e)) * (1.0 - m.rock) *
                                  clamp01(0.4 + 1.5 * fbm(ax / 300.0, az / 300.0, 2, 142));
                        size = 1.1 + 1.2 * u(5);
                        break;
                    case PropKind::Yucca:
                        density = 0.006 * (1.0 - smoothstep(2250.0, 2450.0, e)) * (1.0 - m.rock) *
                                  clamp01(0.4 + 1.5 * fbm(ax / 200.0, az / 200.0, 2, 143));
                        size = 0.6 + 0.5 * u(5);
                        break;
                    case PropKind::Log: {
                        const Material tree = m.treeMix > 0.5 ? m.treeB : m.treeA;
                        if (tree == Material::Ponderosa) material = u(4) < 0.5 ? Material::PonderosaBark : Material::Deadwood;
                        else if (tree == Material::Fir || tree == Material::Spruce || tree == Material::AspenGold || tree == Material::AspenBare)
                            material = u(4) < 0.4 ? Material::Bark : Material::Deadwood;
                        else continue;
                        density = 0.0022 * clamp01(m.cover / 0.5) * (1.0 - smoothstep(0.7, 1.0, slope));
                        size = 3.5 + 5.5 * u(5);
                        break;
                    }
                    default: continue;
                }
                if (u(7) >= density * rule.cell * rule.cell) continue;
                switch (rule.kind) {   // plants file their variants under their own material
                    case PropKind::Chamisa: material = Material::Chamisa; break;
                    case PropKind::Snakeweed: material = Material::Snakeweed; break;
                    case PropKind::Cholla: material = Material::Cholla; break;
                    case PropKind::Yucca: material = Material::Yucca; break;
                    default: break;
                }
                const std::vector<int>* pick = &lib.of(rule.kind, material);
                if (pick->empty()) continue;
                const int shape = (*pick)[size_t(u(8) * double(pick->size())) % pick->size()];
                const PropShape& sh = lib.shapes[size_t(shape)];

                // Nothing in a trunk.
                bool blocked = false;
                const double reach = rule.kind == PropKind::Log ? 0.15 * size : 0.4 * size;
                for (const Tree& t : trees) {
                    if (t.trunkR <= 0.0 || t.kind == TreeKind::Oak || t.kind == TreeKind::Chamisa) continue;
                    const double dx = t.x - ax, dz = t.z - az;
                    const double lim = t.trunkR + reach + (rule.kind == PropKind::Log ? 0.5 * size : 0.0);
                    if (dx * dx + dz * dz < lim * lim) { blocked = true; break; }
                }
                if (blocked) continue;

                // Orientation: turned at random, leaning with the slope (stones and logs more), tilted.
                const V3 normal = normalize({-s.gx, 1.0, -s.gz});
                double align = 0.2, tiltMax = 0.1;
                switch (rule.kind) {
                    case PropKind::Boulder: align = 0.5; tiltMax = 0.35; break;
                    case PropKind::Cobble: align = 0.6; tiltMax = 0.6; break;
                    case PropKind::Log: align = 1.0; tiltMax = 0.04; break;
                    case PropKind::Cholla: align = 0.05; tiltMax = 0.12; break;
                    default: break;
                }
                const V3 up = normalize(V3{0, 1, 0} * (1.0 - align) + normal * align);
                const double tiltAz = kTau * u(9), tilt = tiltMax * u(10);
                Q q = fromTo({0, 1, 0}, up) * axisAngle({std::cos(tiltAz), 0.0, std::sin(tiltAz)}, tilt) *
                      axisAngle({0, 1, 0}, kTau * u(11));

                // Size: a stone's or plant's width across its footprint; a log's length is its extent.
                const double voxel = rule.kind == PropKind::Log ? size / double(sh.res) / 0.86 : size / (2.0 * sh.radius);
                const double height = (sh.top - sh.bottom) * voxel;
                const double radius = sh.radius * voxel;
                // Sunk: stones by a share of their height, more on a slope (the downhill side would
                // float); plants a little; logs by a quarter of their thickness.
                double sink;
                if (stone) sink = height * (0.12 + 0.25 * u(12)) + slope * radius * 0.6;
                else if (rule.kind == PropKind::Log) sink = height * 0.25 + 0.05;
                else sink = 0.04 * height + slope * radius * 0.4;
                const V3 anchorLocal{sh.cx, sh.bottom + sink / voxel, sh.cz};
                const V3 world{ax, e, az};
                const V3 corner = world - rotate(q, anchorLocal * voxel);
                PropInstance p;
                p.shape = shape;
                p.px = corner.x; p.py = corner.y; p.pz = corner.z;
                p.qw = q.w; p.qx = q.x; p.qy = q.y; p.qz = q.z;
                p.size = voxel * sh.res;
                out.push_back(p);
            }
    }
}

}  // namespace mountains
