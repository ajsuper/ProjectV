// The island: a seed in, a world folder out (docs/plans/island-demo.md, D1-D4).
//
// A pure function of world position and the seed, so the columns can be made on any thread in any
// order and still meet at their seams. The land is a 3D density field: solid where
//
//     base(x, z) - y + amplitude(x, z) * noise3(x, y, z) > 0
//
// `base` is rolling hills plus ridged mountains under a radial falloff into a shallow sea; the 3D term,
// strongest where the ground is steep or mountainous, makes cliffs, ledges and overhangs. Materials
// come from the surface above each voxel and its normal: rock where it faces sideways or is high, sand
// by the sea, grass over dirt elsewhere, stone under all of it in blotches with wandering cracks, every
// threshold wavering with noise so nothing reads as ruled. Trees
// -- a trunk and a crown of a few blobs of leaves, each one different -- stand on gentle grass.
//
// It writes the world as the engine's own scene, one grid of 64^3 cells at 0.25 m (compose.json and
// island.data), which is also what a save is (D11): a new world and a loaded one go the same way in.
#ifndef ISLAND_DEMO_ISLAND_H
#define ISLAND_DEMO_ISLAND_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/math.h"
#include "data_structures/compose.h"
#include "utils/compose_io.h"
#include "utils/material.h"
#include "utils/voxel_management.h"

namespace island {
    using projv::core::vec3;

    constexpr int   RES = 64;                 // voxels per cell edge
    constexpr float VOXEL = 0.125f;           // metres
    constexpr int   CELLS = 24;               // across (24 x 64 x 0.125 = 192 m)
    constexpr int   LAYERS = 7;               // high (56 m)
    constexpr int   COLUMN = RES * LAYERS;    // voxels in a column, bottom to top
    constexpr float SIZE = CELLS * RES * VOXEL;
    constexpr float HEIGHT = LAYERS * RES * VOXEL;
    constexpr float CENTRE = SIZE * 0.5f;
    constexpr float SEA = 6.0f;               // the sea's surface, m
    constexpr float RADIUS = 82.0f;           // where the island meets the sea, on average
    constexpr float ROCK_LINE = 25.0f;        // above this (give or take) the ground is bare rock

    // The palette's slots: a few shades of each material, each naming its physics material (D4).
    enum Slot : uint8_t {
        Grass0, Grass1, Grass2, Dirt0, Dirt1, Sand0, Sand1,
        Stone0, Stone1, Stone2, Stone3, Stone4, StoneDark,
        Bark, Wood, Leaves0, Leaves1, Leaves2, Water,
        Blade0, Blade1, BladeDry, FlowerRed, FlowerYellow, FlowerWhite, FlowerPurple, FlowerBlue,
        LeafPink0, LeafPink1, LeafOrange, LeafYellow, Apple, Orange, BerryRed, BerryBlue,
        MushStem, CapRed, CapBrown, CapSpot, Gills, Pebble0, Pebble1, Shell0, Shell1, Reed, Cattail, Vine0, Vine1, Moss, Driftwood, GlowCap, Needle0, Needle1,
        SLOTS
    };
    constexpr int STONE_SHADES = 5;
    struct Shade { const char* name; int r, g, b; const char* physics; };
    constexpr Shade PALETTE[SLOTS] = {
        {"grass", 310, 560, 210, "grass"}, {"grass", 350, 600, 230, "grass"}, {"grass", 280, 520, 190, "grass"},
        {"dirt", 450, 320, 200, "dirt"}, {"dirt", 410, 290, 185, "dirt"},
        {"sand", 860, 790, 590, "sand"}, {"sand", 820, 750, 560, "sand"},
        {"stone", 520, 510, 500, "stone"}, {"stone", 450, 445, 450, "stone"}, {"stone", 590, 570, 540, "stone"},
        {"stone", 490, 470, 440, "stone"}, {"stone", 400, 400, 415, "stone"}, {"stone", 330, 325, 330, "stone"},
        {"wood", 400, 280, 180, "wood"}, {"wood", 560, 420, 280, "wood"},
        {"leaves", 220, 470, 170, "leaves"}, {"leaves", 260, 520, 190, "leaves"}, {"leaves", 190, 420, 150, "leaves"},
        {"water", 180, 380, 560, "water"},
        // Plants: drawn, never collided with (the plants component has no collider).
        {"grass blade", 330, 620, 220, "leaves"}, {"grass blade", 280, 560, 200, "leaves"}, {"dry grass", 560, 580, 300, "leaves"},
        {"flower", 860, 230, 210, "leaves"}, {"flower", 950, 820, 250, "leaves"}, {"flower", 940, 930, 900, "leaves"},
        {"flower", 640, 330, 820, "leaves"}, {"flower", 330, 520, 900, "leaves"},
        // Blossom and autumn crowns, fruit and berries.
        {"leaves", 960, 640, 780, "leaves"}, {"leaves", 990, 760, 860, "leaves"}, {"leaves", 900, 450, 150, "leaves"},
        {"leaves", 950, 780, 220, "leaves"}, {"apple", 860, 120, 110, "leaves"}, {"orange", 980, 560, 90, "leaves"},
        {"berry", 820, 90, 160, "leaves"}, {"berry", 260, 300, 820, "leaves"},
        // Mushrooms, the shore, vines, moss, driftwood.
        {"mushroom", 930, 900, 820, "mushroom"}, {"mushroom", 880, 140, 120, "mushroom"}, {"mushroom", 600, 400, 250, "mushroom"},
        {"mushroom", 990, 980, 960, "mushroom"}, {"mushroom", 820, 760, 660, "mushroom"},
        {"pebble", 650, 640, 620, "stone"}, {"pebble", 470, 460, 470, "stone"}, {"shell", 990, 960, 920, "stone"},
        {"shell", 980, 770, 760, "stone"}, {"reed", 400, 560, 230, "leaves"}, {"cattail", 420, 260, 150, "leaves"},
        {"vine", 200, 450, 160, "leaves"}, {"vine", 260, 530, 180, "leaves"}, {"moss", 300, 520, 170, "grass"},
        {"driftwood", 760, 720, 660, "wood"}, {"glowcap", 300, 900, 1000, "mushroom"},
        {"needles", 130, 330, 190, "leaves"}, {"needles", 160, 380, 210, "leaves"},
    };

    // ---- Noise: gradient noise from a hash of the seed and the lattice point -----------------------

    inline uint32_t hash(uint32_t x) {
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return x;
    }
    inline uint32_t hash(int x, int z, uint32_t seed) { return hash(uint32_t(x) * 0x9e3779b1u ^ hash(uint32_t(z) + seed * 0x85ebca77u)); }
    inline uint32_t hash(int x, int y, int z, uint32_t seed) { return hash(uint32_t(x) * 0x9e3779b1u ^ hash(uint32_t(y) * 0x85ebca77u ^ hash(uint32_t(z) + seed * 0xc2b2ae3du))); }
    inline float unit(uint32_t h) { return float(h & 0xffffffu) / float(0x1000000); }   // [0, 1)
    inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

    inline float gradientNoise(float x, float z, uint32_t seed) {
        const int ix = int(std::floor(x)), iz = int(std::floor(z));
        const float fx = x - float(ix), fz = z - float(iz);
        auto corner = [&](int cx, int cz) {
            const float a = unit(hash(ix + cx, iz + cz, seed)) * 6.2831853f;
            return std::cos(a) * (fx - float(cx)) + std::sin(a) * (fz - float(cz));
        };
        const float ux = fade(fx), uz = fade(fz);
        const float a = corner(0, 0) + (corner(1, 0) - corner(0, 0)) * ux;
        const float b = corner(0, 1) + (corner(1, 1) - corner(0, 1)) * ux;
        return a + (b - a) * uz;   // about [-0.7, 0.7]
    }
    inline float fbm(float x, float z, uint32_t seed, int octaves) {
        float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, total = 0.0f;
        for (int i = 0; i < octaves; i++) {
            sum += gradientNoise(x * frequency, z * frequency, seed + uint32_t(i) * 101u) * amplitude;
            total += amplitude;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }
        return sum / total;
    }
    // Ridges: 1 - |noise|, squared, so the octaves meet in sharp crests. [0, 1].
    inline float ridged(float x, float z, uint32_t seed, int octaves) {
        float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, total = 0.0f;
        for (int i = 0; i < octaves; i++) {
            const float n = 1.0f - std::min(1.0f, std::abs(gradientNoise(x * frequency, z * frequency, seed + uint32_t(i) * 131u)) * 1.4f);
            sum += n * n * amplitude;
            total += amplitude;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }
        return sum / total;
    }

    // 3D gradient noise on the twelve cube-edge gradients (no trigonometry). About [-1, 1].
    inline float gradient3(uint32_t h, float x, float y, float z) {
        switch (h % 12u) {
            case 0: return x + y;   case 1: return -x + y;  case 2: return x - y;   case 3: return -x - y;
            case 4: return x + z;   case 5: return -x + z;  case 6: return x - z;   case 7: return -x - z;
            case 8: return y + z;   case 9: return -y + z;  case 10: return y - z;  default: return -y - z;
        }
    }
    inline float gradientNoise3(float x, float y, float z, uint32_t seed) {
        const int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
        const float fx = x - float(ix), fy = y - float(iy), fz = z - float(iz);
        auto g = [&](int cx, int cy, int cz) {
            return gradient3(hash(ix + cx, iy + cy, iz + cz, seed), fx - float(cx), fy - float(cy), fz - float(cz));
        };
        const float ux = fade(fx), uy = fade(fy), uz = fade(fz);
        auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
        const float x00 = lerp(g(0, 0, 0), g(1, 0, 0), ux), x10 = lerp(g(0, 1, 0), g(1, 1, 0), ux);
        const float x01 = lerp(g(0, 0, 1), g(1, 0, 1), ux), x11 = lerp(g(0, 1, 1), g(1, 1, 1), ux);
        return lerp(lerp(x00, x10, uy), lerp(x01, x11, uy), uz);
    }
    inline float fbm3(float x, float y, float z, uint32_t seed, int octaves) {
        float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, total = 0.0f;
        for (int i = 0; i < octaves; i++) {
            sum += gradientNoise3(x * frequency, y * frequency, z * frequency, seed + uint32_t(i) * 173u) * amplitude;
            total += amplitude;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }
        return sum / total;
    }
    inline float smoothstep(float a, float b, float x) {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // ---- The land ----------------------------------------------------------------------------------

    // The 2D part of the density at (x, z): the ground's height before the 3D terms, how strong the 3D
    // term is there, and how strongly tunnels are carved.
    struct Base {
        float height = 0.0f;      // m
        float amplitude = 0.0f;   // m: the 3D term moves the surface by up to this much
        float carve = 0.0f;       // 0..1: tunnels (arches where they break a slope, caves where they don't)
    };
    constexpr float CARVE_DEPTH = 16.0f;   // how far under the surface a tunnel or a chamber can reach, m

    // Rolling hills and rounded mountains -- domes, not ridges -- under a falloff into the sea.
    inline float baseHeight(float x, float z, uint32_t seed, float* mountainOut = nullptr) {
        const float dx = x - CENTRE, dz = z - CENTRE;
        const float shore = RADIUS * (1.0f + 0.22f * fbm(x * 0.012f, z * 0.012f, seed + 7u, 3));
        const float r = std::sqrt(dx * dx + dz * dz) / shore;
        const float land = 1.0f - smoothstep(0.55f, 1.05f, r);              // 1 inland, 0 out at sea
        const float hills = 0.5f + 0.5f * fbm(x * 0.01f, z * 0.01f, seed, 4);
        // Where the mountains are: a broad mask, biased up toward the middle so every island has some.
        const float mountain = smoothstep(-0.2f, 0.3f, fbm(x * 0.006f, z * 0.006f, seed + 71u, 3) + 0.25f * (1.0f - r)) * land;
        const float dome = smoothstep(0.3f, 0.75f, 0.5f + 0.8f * fbm(x * 0.016f, z * 0.016f, seed + 31u, 3));
        float h = SEA - 3.0f + land * (4.0f + 11.0f * hills * hills) + 24.0f * dome * mountain;
        if (h > 28.0f) h = 28.0f + (h - 28.0f) * 0.5f;                       // tops round off
        if (mountainOut) *mountainOut = mountain * dome;
        return std::clamp(h, 1.0f, HEIGHT - 22.0f);
    }

    // Regions: where the ground is rocky (lumpier, boulders, stony patches) and where the forest is thick
    // (trees; the open ground between is meadow, with flowers). Slow noises, so each covers tens of metres.
    inline float rockyAt(float x, float z, uint32_t seed) { return smoothstep(0.0f, 0.35f, fbm(x * 0.014f, z * 0.014f, seed + 300u, 2)); }
    inline float forestAt(float x, float z, uint32_t seed) { return 0.5f + 0.5f * fbm(x * 0.02f, z * 0.02f, seed + 211u, 3); }

    inline Base baseAt(float x, float z, uint32_t seed) {
        Base b;
        float mountain = 0.0f;
        b.height = baseHeight(x, z, seed, &mountain);
        const float e = 1.0f;
        const float steep = std::max(std::abs(baseHeight(x + e, z, seed) - baseHeight(x - e, z, seed)),
                                     std::abs(baseHeight(x, z + e, seed) - baseHeight(x, z - e, seed))) / (2.0f * e);
        // Gentler near the sea (beaches stay beaches, if bumpy), strongest on steep and mountainous ground;
        // a broad noise varies it, so some meadows are smooth and some are broken up.
        const float shore = 0.3f + 0.7f * smoothstep(SEA, SEA + 5.0f, b.height);
        const float lumpy = (0.35f + 1.3f * (0.5f + 0.5f * fbm(x * 0.01f, z * 0.01f, seed + 61u, 2))) * (0.7f + 0.8f * rockyAt(x, z, seed));
        b.amplitude = shore * std::min(17.0f, lumpy * (5.0f + 5.0f * std::min(steep, 1.2f) + 7.0f * mountain));
        // Tunnels in patches, inland only.
        b.carve = smoothstep(0.0f, 0.25f, fbm(x * 0.012f, z * 0.012f, seed + 81u, 2) + 0.3f) * smoothstep(SEA + 2.0f, SEA + 6.0f, b.height);
        return b;
    }
    inline float bandLow(const Base& b) { return b.height - b.amplitude - (b.carve > 0.0f ? CARVE_DEPTH : 0.0f); }
    inline float bandHigh(const Base& b) { return b.height + b.amplitude; }

    // The density at a point, given its column's base: positive inside the ground. Outside the band the 3D
    // terms can reach it is just the base. Inside: a 3D noise squashed in y (so it makes shelves and
    // overhangs, not just lumps), its domain warped so the shapes lean and twist; then tunnels, where two
    // noises are both near zero, which leave arches where they cut through a slope.
    inline float density(const Base& b, float x, float y, float z, uint32_t seed) {
        if (y < bandLow(b) || y > bandHigh(b)) return b.height - y;
        const float w = 0.03f;
        const vec3 warp(gradientNoise3(x * w, y * w, z * w, seed + 410u), gradientNoise3(x * w, y * w, z * w, seed + 411u),
                        gradientNoise3(x * w, y * w, z * w, seed + 412u));
        const float qx = x + 13.0f * warp.x, qy = y + 7.0f * warp.y, qz = z + 13.0f * warp.z;
        float d = b.height - y + b.amplitude * fbm3(qx * 0.05f, qy * 0.12f, qz * 0.05f, seed + 400u, 3);
        if (b.carve > 0.0f && y > SEA + 1.0f) {
            const float fade = b.carve * smoothstep(bandLow(b), bandLow(b) + 4.0f, y);
            // Tunnels, three or four metres across.
            const float t = std::abs(gradientNoise3(qx * 0.026f, qy * 0.04f, qz * 0.026f, seed + 440u)) +
                            std::abs(gradientNoise3(qx * 0.026f, qy * 0.04f, qz * 0.026f, seed + 441u));
            d = std::min(d, (t - 0.24f * fade) * 30.0f);
            // Chambers: rooms where a slow noise peaks, which the tunnels run into.
            const float chamber = gradientNoise3(qx * 0.022f, qy * 0.04f, qz * 0.022f, seed + 450u);
            d = std::min(d, (0.62f - 0.25f * fade - chamber) * 40.0f + (1.0f - fade) * 40.0f);
        }
        return d;
    }

    // The top surface at (x, z): the highest point where the density turns positive, walking down.
    inline float surfaceAt(float x, float z, uint32_t seed) {
        const Base b = baseAt(x, z, seed);
        for (float y = std::min(HEIGHT - 0.5f * VOXEL, b.height + b.amplitude); y > 0.0f; y -= VOXEL)
            if (density(b, x, y, z, seed) > 0.0f) return y + 0.5f * VOXEL;
        return 0.0f;
    }
    // The top surface's height and how steep it is around (x, z) (rise over run).
    inline float slopeAt(float x, float z, float surface, uint32_t seed) {
        const float e = 1.0f;
        float steepest = 0.0f;
        for (const vec3 d : {vec3(e, 0, 0), vec3(-e, 0, 0), vec3(0, 0, e), vec3(0, 0, -e)})
            steepest = std::max(steepest, std::abs(surfaceAt(x + d.x, z + d.z, seed) - surface) / e);
        return steepest;
    }

    // Where the flower fields are, 0..1: open ground (not forest, not rocky) in patches.
    inline float flowerField(float x, float z, uint32_t seed) {
        return smoothstep(0.05f, 0.35f, fbm(x * 0.06f, z * 0.06f, seed + 610u, 2)) * smoothstep(0.6f, 0.35f, forestAt(x, z, seed)) *
               (1.0f - rockyAt(x, z, seed));
    }

    // ---- Trees and boulders -------------------------------------------------------------------------

    enum class TreeKind : uint8_t { Broad, Bush, Pine };
    constexpr int MAX_CLUSTERS = 10;
    struct Tree {
        TreeKind kind = TreeKind::Broad;
        vec3  base;          // the trunk's foot, in the ground
        float height;        // trunk, m (a pine's: to its tip)
        float trunk;         // trunk radius at the foot, m
        float crown;         // how far the crown spreads from the trunk, m
        vec3  lean;          // the trunk top's offset from the foot
        vec3  crownCentre;   // the middle of the crown (where falling leaves start round)
        // A broadleaf's crown: lumpy leaf clusters, each on a branch from the trunk.
        vec3  cluster[MAX_CLUSTERS];
        float clusterRadius[MAX_CLUSTERS];
        vec3  branchFrom[MAX_CLUSTERS];
        int   clusters = 0;
        float reach = 0.0f;  // the furthest any voxel is from the foot, sideways
        float top = 0.0f;    // the highest any voxel is
        uint8_t leaves = 0;  // 0 green, 1 blossom, 2 autumn
        uint8_t fruit = SLOTS;   // fruit or berries on the crown's outside, or none
        uint32_t seed;
    };
    constexpr float TREE_SPACING = 4.0f;   // one candidate per square this size
    constexpr float TREE_REACH = 9.5f;     // the most any tree reaches sideways (checked as trees are made)

    // The trees whose voxels can reach the box [lo, hi]: candidates on a jittered lattice, kept where the
    // forest is thick (and now and then a bush in the open), on gentle grass.
    inline std::vector<Tree> treesNear(vec3 lo, vec3 hi, uint32_t seed) {
        std::vector<Tree> out;
        const int x0 = int(std::floor((lo.x - TREE_REACH) / TREE_SPACING)), x1 = int(std::floor((hi.x + TREE_REACH) / TREE_SPACING));
        const int z0 = int(std::floor((lo.z - TREE_REACH) / TREE_SPACING)), z1 = int(std::floor((hi.z + TREE_REACH) / TREE_SPACING));
        for (int gz = z0; gz <= z1; gz++)
            for (int gx = x0; gx <= x1; gx++) {
                const uint32_t h = hash(gx, gz, seed + 977u);
                const float x = (float(gx) + 0.1f + 0.8f * unit(h)) * TREE_SPACING;
                const float z = (float(gz) + 0.1f + 0.8f * unit(hash(h))) * TREE_SPACING;
                const float forest = forestAt(x, z, seed);
                // Forests closed overhead in their hearts, thinning to scattered trees at their edges.
                const bool tree = unit(hash(h + 1u)) < forest * 2.4f - 0.75f;
                const bool bush = !tree && unit(hash(h + 21u)) < 0.12f + 0.2f * forest;
                if (!tree && !bush) continue;
                if (baseHeight(x, z, seed) < SEA + 1.5f) continue;   // cheap: no forest in the sea
                const float surface = surfaceAt(x, z, seed);
                if (surface < SEA + 2.0f || surface > ROCK_LINE - 3.0f) continue;
                if (slopeAt(x, z, surface, seed) > 0.55f) continue;
                Tree t;
                t.kind = bush ? TreeKind::Bush : unit(hash(h + 20u)) < 0.22f + 0.25f * smoothstep(14.0f, 22.0f, surface) ? TreeKind::Pine : TreeKind::Broad;
                t.base = vec3(x, surface - 0.8f, z);
                t.seed = h;
                const float a = unit(hash(h + 5u)) * 6.2831853f;
                auto r = [&](uint32_t k) { return unit(hash(h + k)); };
                if (t.kind == TreeKind::Pine) {
                    t.height = 8.0f + 6.0f * r(2u);
                    t.trunk = 0.2f + 0.12f * r(3u);
                    t.crown = 1.8f + 1.2f * r(4u);
                    t.lean = vec3(0.0f);
                    t.crownCentre = t.base + vec3(0.0f, t.height * 0.55f, 0.0f);
                    t.reach = t.crown + 0.5f;
                    t.top = t.base.y + t.height + 0.5f;
                } else {
                    const bool small = t.kind == TreeKind::Bush;
                    const float l = small ? 0.0f : 0.8f * r(6u);
                    t.lean = vec3(std::cos(a) * l, 0.0f, std::sin(a) * l);
                    t.height = small ? 0.7f + 0.3f * r(2u) : 3.5f + 3.5f * r(2u);
                    t.trunk = small ? 0.08f : 0.3f + 0.16f * r(3u);
                    t.crown = small ? 0.8f + 0.6f * r(4u) : 2.8f + 1.7f * r(4u) * (0.6f + 0.4f * forest);
                    const vec3 trunkTop = t.base + t.lean + vec3(0.0f, t.height, 0.0f);
                    // Clusters round a wide dome: a ring of them out on branches, a few higher in, one on top.
                    t.clusters = small ? 3 : 6 + int(hash(h + 9u) % 4u);
                    float reach = 0.0f, top = 0.0f;
                    for (int i = 0; i < t.clusters; i++) {
                        const bool crownTop = !small && i == t.clusters - 1;
                        const float angle = a + 6.2831853f * (float(i) + 0.35f * r(40u + uint32_t(i))) / float(t.clusters - (small ? 0 : 1));
                        const float out = crownTop ? 0.0f : t.crown * (small ? 0.35f : 0.45f + 0.35f * r(50u + uint32_t(i)));
                        const float rise = crownTop ? t.crown * 0.55f : t.crown * (small ? 0.1f : -0.05f + 0.45f * r(60u + uint32_t(i)));
                        t.cluster[i] = trunkTop + vec3(std::cos(angle) * out, rise, std::sin(angle) * out);
                        t.clusterRadius[i] = t.crown * (small ? 0.6f : 0.42f + 0.2f * r(70u + uint32_t(i)));
                        // Leaves are ground (you bump into them): a tree's keep clear of your head.
                        if (!small) t.cluster[i].y = std::max(t.cluster[i].y, t.base.y + 0.8f + 2.7f + t.clusterRadius[i] / 1.5f);
                        t.branchFrom[i] = t.base + t.lean * (0.75f) + vec3(0.0f, t.height * (0.6f + 0.3f * r(80u + uint32_t(i))), 0.0f);
                        const float dx = t.cluster[i].x - t.base.x, dz = t.cluster[i].z - t.base.z;
                        reach = std::max(reach, std::sqrt(dx * dx + dz * dz) + t.clusterRadius[i] * 1.15f);
                        top = std::max(top, t.cluster[i].y + t.clusterRadius[i] * 0.85f);
                    }
                    t.crownCentre = trunkTop + vec3(0.0f, t.crown * 0.2f, 0.0f);
                    t.reach = reach + 0.3f;
                    t.top = top + 0.3f;
                }
                if (t.reach > TREE_REACH) continue;   // never: the sizes above keep under it
                const float look = r(30u), crop = r(31u);
                if (t.kind == TreeKind::Broad) {
                    t.leaves = look < 0.15f ? 1 : look < 0.28f ? 2 : 0;
                    if (t.leaves == 0 && crop < 0.25f) t.fruit = crop < 0.15f ? uint8_t(Apple) : uint8_t(Orange);
                } else if (t.kind == TreeKind::Bush && crop < 0.45f) {
                    t.fruit = crop < 0.25f ? uint8_t(BerryRed) : uint8_t(BerryBlue);
                }
                out.push_back(t);
            }
        return out;
    }

    inline float segmentDistance(vec3 p, vec3 a, vec3 b, float& along) {
        const vec3 ab = b - a;
        along = std::clamp(glm::dot(p - a, ab) / std::max(1e-6f, glm::dot(ab, ab)), 0.0f, 1.0f);
        return glm::length(p - (a + ab * along));
    }

    // A leaf voxel of the tree's look.
    inline uint8_t leafOf(const Tree& t, uint32_t pick, float e) {
        if (t.fruit != SLOTS && e > 0.75f && unit(pick >> 8) < 0.05f) return t.fruit;
        if (t.kind == TreeKind::Pine) return uint8_t(Needle0 + pick % 2u);
        if (t.leaves == 1) return unit(pick >> 4) < 0.12f ? uint8_t(Leaves1) : uint8_t(LeafPink0 + pick % 2u);
        if (t.leaves == 2) {
            const uint32_t k = pick % 4u;
            return k == 0 ? uint8_t(LeafOrange) : k == 1 ? uint8_t(LeafYellow) : k == 2 ? uint8_t(LeafOrange) : uint8_t(Leaves2);
        }
        return uint8_t(Leaves0 + pick % 3u);
    }

    // What a tree puts at p, or SLOTS for nothing.
    inline uint8_t treeAt(const Tree& t, vec3 p) {
        const uint32_t h = hash(int(std::floor(p.x * 8.0f)), int(std::floor(p.y * 8.0f)), int(std::floor(p.z * 8.0f)), t.seed);
        if (t.kind == TreeKind::Pine) {
            const float up = p.y - t.base.y;
            const float dx = p.x - t.base.x, dz = p.z - t.base.z, r = std::sqrt(dx * dx + dz * dz);
            if (up >= 0.0f && up < t.height * 0.92f && r < t.trunk * (1.0f - 0.7f * up / t.height) + (up < 0.6f ? 0.12f * (0.6f - up) / 0.6f * 2.0f : 0.0f))
                return r < t.trunk * 0.5f ? uint8_t(Wood) : uint8_t(Bark);
            // Tiers: skirts that droop, each narrower than the one under it.
            const float start = 1.6f + 0.6f * unit(t.seed >> 8);
            if (up < start || up > t.height) return SLOTS;
            const float k = (up - start) / (t.height - start);              // 0 at the bottom, 1 at the tip
            const float tierHeight = 1.1f + 0.3f * (1.0f - k);
            const float frac = std::fmod((t.height - up) / tierHeight, 1.0f);   // 0 at a tier's top, 1 at its hem
            const float radius = t.crown * (1.0f - k) * (0.35f + 0.65f * frac) + 0.25f;
            const float lump = 0.18f * gradientNoise3(p.x * 1.4f, p.y * 1.4f, p.z * 1.4f, t.seed);
            const float e = r / radius + lump;
            if (e > 1.0f) return SLOTS;
            if (e > 0.8f && unit(h) < (e - 0.8f) * 3.0f) return SLOTS;
            return leafOf(t, hash(h + 1u), e);
        }
        // Trunk, flared at the roots, tapering, and the branches out to the clusters.
        const float along = (p.y - t.base.y) / t.height;
        if (along >= 0.0f && along <= 1.05f) {
            const vec3 axis = t.base + t.lean * along;
            const float dx = p.x - axis.x, dz = p.z - axis.z;
            const float r = std::sqrt(dx * dx + dz * dz);
            const float flare = along < 0.18f ? (0.18f - along) / 0.18f * t.trunk * 0.9f : 0.0f;
            const float radius = t.trunk * (1.0f - 0.4f * along) + flare;
            if (r < radius) return r < radius - 0.2f ? uint8_t(Wood) : uint8_t(Bark);
        }
        if (t.kind == TreeKind::Broad)
            for (int i = 0; i < t.clusters; i++) {
                float s;
                const float d = segmentDistance(p, t.branchFrom[i], t.cluster[i], s);
                if (d < t.trunk * (0.5f - 0.3f * s) + 0.03f && s < 0.85f) return Bark;
            }
        // Leaves: the clusters' lumpy union, flattened a little underneath.
        float e = 1e9f;
        for (int i = 0; i < t.clusters; i++) {
            vec3 d = p - t.cluster[i];
            d.y *= d.y < 0.0f ? 1.5f : 1.15f;
            e = std::min(e, glm::length(d) / t.clusterRadius[i]);
        }
        e += 0.16f * gradientNoise3(p.x * 0.9f, p.y * 0.9f, p.z * 0.9f, t.seed + 3u);
        if (e > 1.0f) return SLOTS;
        if (e > 0.82f && unit(h) < (e - 0.82f) * 3.5f) return SLOTS;   // ragged edge
        if (unit(hash(h)) < 0.06f) return SLOTS;                        // a few holes
        return leafOf(t, hash(h + 1u), e);
    }

    // Boulders: lumpy rounded stones sunk into the ground, many in rocky regions, a few elsewhere.
    struct Boulder {
        vec3  centre;
        float radius;        // sideways, m; 0.75 of it up and down
        uint32_t seed;
    };
    constexpr float BOULDER_SPACING = 3.0f;
    constexpr float BOULDER_REACH = 2.6f;

    inline std::vector<Boulder> bouldersNear(vec3 lo, vec3 hi, uint32_t seed) {
        std::vector<Boulder> out;
        const int x0 = int(std::floor((lo.x - BOULDER_REACH) / BOULDER_SPACING)), x1 = int(std::floor((hi.x + BOULDER_REACH) / BOULDER_SPACING));
        const int z0 = int(std::floor((lo.z - BOULDER_REACH) / BOULDER_SPACING)), z1 = int(std::floor((hi.z + BOULDER_REACH) / BOULDER_SPACING));
        for (int gz = z0; gz <= z1; gz++)
            for (int gx = x0; gx <= x1; gx++) {
                const uint32_t h = hash(gx, gz, seed + 1499u);
                const float x = (float(gx) + 0.1f + 0.8f * unit(h)) * BOULDER_SPACING;
                const float z = (float(gz) + 0.1f + 0.8f * unit(hash(h))) * BOULDER_SPACING;
                const float rocky = rockyAt(x, z, seed);
                if (unit(hash(h + 1u)) > 0.03f + 0.3f * rocky) continue;
                if (baseHeight(x, z, seed) < SEA) continue;
                const float surface = surfaceAt(x, z, seed);
                if (surface < SEA + 0.5f) continue;
                Boulder b;
                const float u = unit(hash(h + 2u));
                b.radius = 0.35f + (1.0f + 0.8f * rocky) * u * u;
                b.centre = vec3(x, surface + b.radius * 0.75f * (0.15f - 0.45f * unit(hash(h + 3u))), z);
                b.seed = h;
                out.push_back(b);
            }
        return out;
    }

    // Props: mushrooms (a cluster of small ones, or now and then a giant you can stand on), fallen logs and
    // stumps in the forest, driftwood on the beach. Small mushrooms go in the plants (nothing to trip on);
    // the rest are ground.
    enum class PropKind : uint8_t { Mushrooms, GiantMushroom, Log, Stump, Driftwood };
    struct Prop {
        PropKind kind;
        vec3  pos;           // on the ground
        float radius;        // trunk / stem / log radius, m
        float size;          // log length; giant's stem height
        float cap;           // giant's cap radius
        vec3  dir{1, 0, 0};  // a log's axis
        bool  red = true;    // a mushroom's cap colour
        vec3  small[6];      // a cluster's mushrooms: x, ground y, z (voxel centres)
        float smallCap[6];
        int   count = 0;
        uint32_t seed;
    };
    constexpr float PROP_SPACING = 2.5f;
    constexpr float PROP_REACH = 4.0f;

    inline bool propIsPlant(const Prop& p) { return p.kind == PropKind::Mushrooms; }

    inline std::vector<Prop> propsNear(vec3 lo, vec3 hi, uint32_t seed) {
        std::vector<Prop> out;
        const int x0 = int(std::floor((lo.x - PROP_REACH) / PROP_SPACING)), x1 = int(std::floor((hi.x + PROP_REACH) / PROP_SPACING));
        const int z0 = int(std::floor((lo.z - PROP_REACH) / PROP_SPACING)), z1 = int(std::floor((hi.z + PROP_REACH) / PROP_SPACING));
        auto snap = [](float w) { return (std::floor(w / VOXEL) + 0.5f) * VOXEL; };
        for (int gz = z0; gz <= z1; gz++)
            for (int gx = x0; gx <= x1; gx++) {
                const uint32_t h = hash(gx, gz, seed + 2203u);
                const float roll = unit(hash(h + 1u));
                if (roll > 0.2f) continue;   // cheap: most candidates are nothing
                const float x = (float(gx) + 0.1f + 0.8f * unit(h)) * PROP_SPACING;
                const float z = (float(gz) + 0.1f + 0.8f * unit(hash(h))) * PROP_SPACING;
                if (baseHeight(x, z, seed) < SEA - 1.0f) continue;
                const float surface = surfaceAt(x, z, seed);
                const float forest = forestAt(x, z, seed);
                Prop p;
                p.seed = h;
                p.pos = vec3(x, surface, z);
                const float a = unit(hash(h + 2u)) * 6.2831853f;
                p.dir = vec3(std::cos(a), 0.0f, std::sin(a));
                p.red = unit(hash(h + 3u)) < 0.6f;
                const bool beach = surface < SEA + 1.4f && surface > SEA - 0.1f;
                const bool land = surface > SEA + 2.0f && surface < ROCK_LINE - 3.0f;
                if (beach && roll < 0.03f) {
                    p.kind = PropKind::Driftwood;
                    p.radius = 0.12f + 0.1f * unit(hash(h + 4u));
                    p.size = 1.0f + 1.8f * unit(hash(h + 5u));
                } else if (land && roll < 0.008f + 0.004f * forest) {
                    p.kind = PropKind::GiantMushroom;
                    p.radius = 0.35f + 0.25f * unit(hash(h + 4u));
                    p.size = 2.2f + 2.2f * unit(hash(h + 5u));
                    p.cap = 1.4f + 1.0f * unit(hash(h + 6u));
                } else if (land && roll < 0.008f + forest * 0.16f && slopeAt(x, z, surface, seed) < 0.6f) {
                    const float k = unit(hash(h + 7u));
                    if (k < 0.62f) {
                        p.kind = PropKind::Mushrooms;
                        p.count = 3 + int(hash(h + 8u) % 4u);
                        for (int i = 0; i < p.count; i++) {
                            const float b = unit(hash(h + 20u + uint32_t(i))) * 6.2831853f, r = 0.15f + 0.5f * unit(hash(h + 30u + uint32_t(i)));
                            const float sx = snap(x + std::cos(b) * r), sz = snap(z + std::sin(b) * r);
                            p.small[i] = vec3(sx, surfaceAt(sx, sz, seed), sz);
                            p.smallCap[i] = 0.13f + 0.17f * unit(hash(h + 40u + uint32_t(i)));
                        }
                    } else if (k < 0.85f) {
                        p.kind = PropKind::Log;
                        p.radius = 0.25f + 0.17f * unit(hash(h + 4u));
                        p.size = 2.0f + 2.4f * unit(hash(h + 5u));
                    } else {
                        p.kind = PropKind::Stump;
                        p.radius = 0.28f + 0.2f * unit(hash(h + 4u));
                        p.size = 0.3f + 0.5f * unit(hash(h + 5u));
                    }
                } else continue;
                out.push_back(p);
            }
        return out;
    }

    // What a prop puts at q, or SLOTS.
    inline uint8_t propAt(const Prop& p, vec3 q) {
        const uint32_t h = hash(int(std::floor(q.x * 8.0f)), int(std::floor(q.y * 8.0f)), int(std::floor(q.z * 8.0f)), p.seed);
        auto spots = [&](vec3 at, float scale) {   // white spots on a red cap: blobs, not a grid
            return gradientNoise3(at.x * scale, at.y * scale, at.z * scale, p.seed + 9u) > 0.28f;
        };
        switch (p.kind) {
            case PropKind::Mushrooms:
                for (int i = 0; i < p.count; i++) {
                    const vec3 m = p.small[i];
                    const float capR = p.smallCap[i], stem = capR * 1.1f + 0.05f;
                    const float dx = q.x - m.x, dz = q.z - m.z, r = std::sqrt(dx * dx + dz * dz), up = q.y - m.y;
                    if (up < 0.0f || up > stem + capR * 0.7f) continue;
                    if (up < stem && r < 0.07f) return MushStem;
                    const float cy = up - stem;   // the cap: a dome over the stem
                    if (cy > -0.07f && r * r / (capR * capR) + (cy * cy) / (capR * capR * 0.45f) < 1.0f)
                        return p.red ? (unit(h) < 0.25f ? uint8_t(CapSpot) : uint8_t(CapRed)) : uint8_t(CapBrown);
                }
                return SLOTS;
            case PropKind::GiantMushroom: {
                const float dx = q.x - p.pos.x, dz = q.z - p.pos.z, r = std::sqrt(dx * dx + dz * dz);
                const float up = q.y - p.pos.y + 0.4f;
                const float sway = 0.25f * std::sin(up * 0.6f + float(p.seed % 7u));   // a little bend in the stem
                if (up >= 0.0f && up < p.size + 0.2f) {
                    const float sx = dx - sway, sr = std::sqrt(sx * sx + dz * dz);
                    if (sr < p.radius * (1.0f + 0.25f * std::max(0.0f, 1.0f - up))) return MushStem;
                }
                const float cy = up - p.size;
                if (cy < -0.1f) return SLOTS;
                const float e = r * r / (p.cap * p.cap) + cy * cy / (p.cap * p.cap * 0.3f);
                if (e > 1.0f) return SLOTS;
                if (cy < 0.15f) return Gills;   // the underside
                if (!p.red) return unit(h) < 0.1f ? uint8_t(CapSpot) : uint8_t(CapBrown);
                return spots(q, 1.6f) ? uint8_t(CapSpot) : uint8_t(CapRed);
            }
            case PropKind::Log:
            case PropKind::Driftwood: {
                const vec3 c = p.pos + vec3(0.0f, p.radius * 0.55f, 0.0f);
                const vec3 d = q - c;
                const float t = d.x * p.dir.x + d.z * p.dir.z;
                if (std::abs(t) > p.size * 0.5f) return SLOTS;
                const vec3 radial = d - p.dir * t;
                const float r = glm::length(radial);
                if (r > p.radius) return SLOTS;
                if (p.kind == PropKind::Driftwood) return Driftwood;
                if (std::abs(t) > p.size * 0.5f - 0.13f) return r > p.radius - 0.1f ? uint8_t(Bark) : uint8_t(Wood);   // the cut ends
                if (r > p.radius - 0.13f) {
                    if (radial.y > p.radius * 0.45f && gradientNoise3(q.x * 1.5f, q.y * 1.5f, q.z * 1.5f, p.seed) > -0.1f) return Moss;
                    return Bark;
                }
                return Wood;
            }
            case PropKind::Stump: {
                const float dx = q.x - p.pos.x, dz = q.z - p.pos.z, r = std::sqrt(dx * dx + dz * dz);
                const float up = q.y - p.pos.y + 0.3f;
                const float radius = p.radius * (1.0f + 0.35f * std::max(0.0f, 0.4f - up));   // flared roots
                if (up < 0.0f || up > p.size + 0.3f || r > radius) return SLOTS;
                if (up > p.size + 0.3f - 0.13f && r < radius - 0.1f) return Wood;   // the cut top
                return unit(h) < 0.15f && up > 0.3f ? uint8_t(Moss) : uint8_t(Bark);
            }
        }
        return SLOTS;
    }

    // ---- The voxels ---------------------------------------------------------------------------------

    // Stone: soft blotches of shade from a smooth 3D noise, speckled. Nothing ruled: no bands, no cells.
    inline uint8_t stoneAt(vec3 p, uint32_t h, uint32_t seed) {
        const float blot = fbm3(p.x * 0.09f, p.y * 0.09f, p.z * 0.09f, seed + 93u, 2);
        int shade = std::clamp(int((0.5f + blot * 1.0f) * float(STONE_SHADES)), 0, STONE_SHADES - 1);
        if (unit(h) < 0.2f) shade = std::clamp(shade + (int(h >> 24) % 3) - 1, 0, STONE_SHADES - 1);   // speckle
        return uint8_t(Stone0 + shade);
    }

    // What a boulder puts at p, or SLOTS: stone, lumpy, with moss on its top.
    inline uint8_t boulderAt(const Boulder& b, vec3 p, uint32_t seed) {
        const vec3 d = p - b.centre;
        const float lump = 0.14f * gradientNoise3(p.x * 1.3f, p.y * 1.3f, p.z * 1.3f, b.seed);
        const float e = std::sqrt(d.x * d.x + d.y * d.y / 0.5625f + d.z * d.z) / b.radius + lump;
        if (e > 1.0f) return SLOTS;
        const uint32_t h = hash(int(std::floor(p.x * 8.0f)), int(std::floor(p.y * 8.0f)), int(std::floor(p.z * 8.0f)), b.seed);
        if (d.y > 0.45f * b.radius && gradientNoise3(p.x * 0.9f, p.y * 0.9f, p.z * 0.9f, b.seed + 1u) > -0.05f)
            return uint8_t(Grass0 + h % 3u);
        if (unit(h >> 4) < 0.12f) return StoneDark;
        return stoneAt(p, h, seed);
    }

    // What the surface a voxel lies under is like.
    enum class Surface : uint8_t { Grass, Sand, Rock };

    // The density is sampled on a lattice this coarse, aligned to the world so neighbouring columns agree,
    // and interpolated: the 3D noise has nothing finer than about 2 m, and sampling it at every 0.125 m
    // voxel costs 64 times as much. A cheap fine noise is added per voxel near the surface instead.
    constexpr float LATTICE = 0.5f;
    constexpr float ROUGH = 0.35f;   // the fine noise's amplitude, m
    constexpr float BUMP = 0.22f;    // rock's bumps and recesses, m (one or two voxels)

    // One column of cells (cx, cz), all LAYERS of it: the ground into `blocks`, grass blades and flowers
    // into `plantBlocks` (bottom first); `filled` / `plantFilled` say which came out non-empty. A voxel's
    // material depends on its depth under the surface above it, which can be in a higher cell, so a column
    // is made whole. `trees` and `boulders` are every one on the island.
    inline void makeColumn(int cx, int cz, uint32_t seed, const std::vector<Tree>& trees, const std::vector<Boulder>& boulders,
                           const std::vector<Prop>& props, projv::DataBlock* blocks, char* filled, projv::DataBlock* plantBlocks, char* plantFilled) {
        constexpr int M = 2;              // the footprint's margin: one voxel for faces and normals, one more for the bumps' normals
        constexpr int F = RES + 2 * M;
        const float ox = float(cx * RES) * VOXEL, oz = float(cz * RES) * VOXEL;
        auto wx = [&](int fx) { return ox + (float(fx - M) + 0.5f) * VOXEL; };   // footprint index to world
        auto wz = [&](int fz) { return oz + (float(fz - M) + 0.5f) * VOXEL; };
        auto wy = [](int y) { return (float(y) + 0.5f) * VOXEL; };

        // The lattice over the footprint.
        const int i0 = int(std::floor(wx(0) / LATTICE)), i1 = int(std::floor(wx(F - 1) / LATTICE)) + 1;
        const int k0 = int(std::floor(wz(0) / LATTICE)), k1 = int(std::floor(wz(F - 1) / LATTICE)) + 1;
        const int NX = i1 - i0 + 1, NZ = k1 - k0 + 1, NY = int(HEIGHT / LATTICE) + 1;
        std::vector<Base> bases(size_t(NX * NZ));
        std::vector<float> lattice(size_t(NX * NZ * NY));
        for (int k = 0; k < NZ; k++)
            for (int i = 0; i < NX; i++) {
                const float x = float(i0 + i) * LATTICE, z = float(k0 + k) * LATTICE;
                const Base& b = bases[size_t(k * NX + i)] = baseAt(x, z, seed);
                float* column = &lattice[size_t(k * NX + i) * size_t(NY)];
                for (int j = 0; j < NY; j++) column[j] = density(b, x, float(j) * LATTICE, z, seed);
            }

        // The density at every voxel of the footprint.
        auto index = [](int fx, int y, int fz) { return size_t(fz * F + fx) * COLUMN + size_t(y); };
        std::vector<float> smooth(size_t(F * F * COLUMN));
        for (int fz = 0; fz < F; fz++)
            for (int fx = 0; fx < F; fx++) {
                const float gx = wx(fx) / LATTICE - float(i0), gz = wz(fz) / LATTICE - float(k0);
                const int i = int(std::floor(gx)), k = int(std::floor(gz));
                const float tx = gx - float(i), tz = gz - float(k);
                const float* c00 = &lattice[size_t(k * NX + i) * size_t(NY)];
                const float* c10 = &lattice[size_t(k * NX + i + 1) * size_t(NY)];
                const float* c01 = &lattice[size_t((k + 1) * NX + i) * size_t(NY)];
                const float* c11 = &lattice[size_t((k + 1) * NX + i + 1) * size_t(NY)];
                float lo = 1e9f, hi = -1e9f;
                for (int c : {k * NX + i, k * NX + i + 1, (k + 1) * NX + i, (k + 1) * NX + i + 1}) {
                    lo = std::min(lo, bandLow(bases[size_t(c)]) - LATTICE);
                    hi = std::max(hi, bandHigh(bases[size_t(c)]) + LATTICE);
                }
                for (int y = 0; y < COLUMN; y++) {
                    const float py = wy(y);
                    float d;
                    if (py < lo) d = lo - py + 1.0f;          // under every band: solid
                    else if (py > hi) d = hi - py - 1.0f;     // over: air
                    else {
                        const float gy = py / LATTICE;
                        const int j = std::min(int(gy), NY - 2);
                        const float ty = gy - float(j);
                        auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
                        auto col = [&](const float* c) { return lerp(c[j], c[j + 1], ty); };
                        d = lerp(lerp(col(c00), col(c10), tx), lerp(col(c01), col(c11), tx), tz);
                        if (std::abs(d) < ROUGH * 2.0f)
                            d += ROUGH * gradientNoise3(wx(fx) * 0.45f, py * 0.45f, wz(fz) * 0.45f, seed + 430u);
                    }
                    smooth[index(fx, y, fz)] = d;
                }
            }

        // Rock gets bumps and recesses a voxel or two deep: where the surface faces sideways, or is over the
        // rock line, a smooth fine noise is added. Read from `smooth`, written to `dens`, so every voxel's
        // normal comes from the unbumped field (and its neighbour column computes the same).
        std::vector<float> dens = smooth;
        for (int fz = 1; fz < F - 1; fz++)
            for (int fx = 1; fx < F - 1; fx++) {
                const float px = wx(fx), pz = wz(fz);
                const float rockLine = ROCK_LINE + 6.0f * fbm(px * 0.05f, pz * 0.05f, seed + 53u, 2);
                for (int y = 1; y < COLUMN - 1; y++) {
                    const float d = smooth[index(fx, y, fz)];
                    if (std::abs(d) > BUMP * 2.5f) continue;
                    const vec3 grad(smooth[index(fx + 1, y, fz)] - smooth[index(fx - 1, y, fz)], smooth[index(fx, y + 1, fz)] - smooth[index(fx, y - 1, fz)],
                                    smooth[index(fx, y, fz + 1)] - smooth[index(fx, y, fz - 1)]);
                    const float length = glm::length(grad);
                    const float up = length > 1e-6f ? -grad.y / length : 1.0f;
                    const float py = wy(y);
                    const float rockiness = std::max(smoothstep(0.78f, 0.5f, up), smoothstep(rockLine - 2.0f, rockLine + 1.0f, py));
                    if (rockiness <= 0.0f) continue;
                    const float n = 0.7f * gradientNoise3(px * 0.8f, py * 0.8f, pz * 0.8f, seed + 450u) +
                                    0.3f * gradientNoise3(px * 1.9f, py * 1.9f, pz * 1.9f, seed + 451u);
                    dens[index(fx, y, fz)] = d + rockiness * BUMP * n * 1.4f;
                }
            }

        // Solid or not.
        std::vector<uint8_t> solid(size_t(F * F * COLUMN), 0);
        auto at = [&](int fx, int y, int fz) -> uint8_t& { return solid[index(fx, y, fz)]; };
        auto densityAt = [&](int fx, int y, int fz) { return smooth[index(fx, y, fz)]; };
        for (int fz = 1; fz < F - 1; fz++)
            for (int fx = 1; fx < F - 1; fx++) {
                for (int y = 0; y < COLUMN; y++) at(fx, y, fz) = dens[index(fx, y, fz)] > 0.0f;
                // Specks: a run of under 0.5 m resting on air is noise, not a ledge.
                constexpr int SPECK = int(0.5f / VOXEL);
                for (int y = COLUMN - 1; y > 0;) {
                    if (!at(fx, y, fz)) { y--; continue; }
                    int bottom = y;
                    while (bottom > 0 && at(fx, bottom - 1, fz)) bottom--;
                    if (bottom > 0 && y - bottom + 1 < SPECK)
                        for (int k2 = bottom; k2 <= y; k2++) at(fx, k2, fz) = 0;
                    y = bottom - 1;
                }
            }

        // Materials, top down per column; plants on grass.
        std::vector<uint8_t> material(size_t(RES * RES * COLUMN), uint8_t(SLOTS));
        std::vector<uint8_t> plants(size_t(RES * RES * COLUMN), uint8_t(SLOTS));
        auto mat = [&](int x, int y, int z) -> uint8_t& { return material[(size_t(z * RES + x)) * COLUMN + size_t(y)]; };
        auto plant = [&](int x, int y, int z) -> uint8_t& { return plants[(size_t(z * RES + x)) * COLUMN + size_t(y)]; };
        for (int z = 0; z < RES; z++)
            for (int x = 0; x < RES; x++) {
                const int fx = x + M, fz = z + M;
                const float px = wx(fx), pz = wz(fz);
                const float jitter = fbm(px * 0.05f, pz * 0.05f, seed + 53u, 2);
                const float rocky = rockyAt(px, pz, seed);
                const float patches = fbm(px * 0.15f, pz * 0.15f, seed + 57u, 2);   // bare dirt and stony bits in the grass
                // Plants: how thick the grass is, how tall, how dry; where the flower fields are.
                const float grassy = std::clamp(0.3f + 0.6f * (0.5f + 0.5f * fbm(px * 0.07f, pz * 0.07f, seed + 600u, 2)) - 0.35f * rocky, 0.0f, 0.85f);
                const float tall = 0.5f + 0.5f * fbm(px * 0.05f, pz * 0.05f, seed + 601u, 2);
                const bool dry = fbm(px * 0.04f, pz * 0.04f, seed + 602u, 2) > 0.25f;
                const float flowers = flowerField(px, pz, seed);
                const float reeds = smoothstep(0.0f, 0.3f, fbm(px * 0.08f, pz * 0.08f, seed + 620u, 2));
                const float vines = 0.5f + 0.5f * fbm(px * 0.1f, pz * 0.1f, seed + 630u, 2);
                bool roofed = false;        // there is ground somewhere above: a cave floor, the ground under an overhang
                Surface surface = Surface::Grass;
                float top = 0.0f;           // the surface's height, m
                float dirtDepth = 2.5f, grassDepth = 0.3f, lip = 0.5f;
                bool bare = false, stony = false;
                bool above = false;         // the voxel above was solid
                for (int y = COLUMN - 1; y >= 0; y--) {
                    if (!at(fx, y, fz)) { if (above) roofed = true; above = false; continue; }
                    const uint32_t h = hash(cx * RES + x, y, cz * RES + z, seed + 5u);
                    // Under a run of ground with air below it: vines hang down.
                    if (y > 0 && !at(fx, y - 1, fz) && wy(y) > SEA + 1.0f && unit(hash(h + 91u)) < vines * vines * 0.3f) {
                        int gap = 0;
                        while (y - 1 - gap >= 0 && !at(fx, y - 1 - gap, fz) && gap < 40) gap++;
                        const uint32_t v = hash(h + 92u);
                        const int length = std::min(gap - 1, 3 + int(v % 14u));
                        for (int k = 1; k <= length; k++)
                            plant(x, y - k, z) = k == length && unit(v >> 8) < 0.1f ? uint8_t(FlowerPurple) : uint8_t(Vine0 + (hash(v + uint32_t(k)) % 2u));
                    }
                    bool fresh = false;
                    float up = 1.0f;
                    if (!above) {
                        // A new surface: its outward normal from the (unbumped) density's gradient.
                        const int yu = std::min(y + 1, COLUMN - 1), yd = std::max(y - 1, 0);
                        vec3 grad(densityAt(fx + 1, y, fz) - densityAt(fx - 1, y, fz),
                                  densityAt(fx, yu, fz) - densityAt(fx, yd, fz),
                                  densityAt(fx, y, fz + 1) - densityAt(fx, y, fz - 1));
                        const float length = glm::length(grad);
                        up = length > 1e-6f ? -grad.y / length : 1.0f;
                        top = wy(y);
                        // Every threshold wavers with a 3D noise, so no rule draws a clean line.
                        const float n = gradientNoise3(px * 0.12f, top * 0.12f, pz * 0.12f, seed + 500u);
                        const float rockLine = ROCK_LINE + 6.0f * jitter + 3.0f * n;
                        if (up < 0.62f + 0.15f * n || top > rockLine) {
                            surface = Surface::Rock;
                            // Grass clings to flat ledges in the rock, below the bare peaks.
                            if (up > 0.82f && top < rockLine + 4.0f && n > 0.15f) surface = Surface::Grass;
                        } else if (top < SEA + 1.2f + 1.5f * jitter + 0.6f * n && up > 0.5f) surface = Surface::Sand;
                        else surface = Surface::Grass;
                        dirtDepth = 1.2f + 2.0f * (0.5f + 0.5f * n) + 0.6f * unit(h >> 8);
                        grassDepth = unit(h >> 12) < 0.3f ? 0.375f : 0.25f;
                        lip = 0.25f + 0.5f * unit(h >> 16);
                        bare = patches + 0.3f * n > 0.3f - 0.15f * rocky;
                        stony = patches - 0.3f * n < -0.36f + 0.3f * rocky;
                        above = true;
                        fresh = true;
                    }
                    const float depth = top - wy(y);
                    // A voxel open to the side is a cliff's face: rock, past a lip of soil.
                    const bool face = !at(fx + 1, y, fz) || !at(fx - 1, y, fz) || !at(fx, y, fz + 1) || !at(fx, y, fz - 1);
                    const vec3 p(px, wy(y), pz);
                    uint8_t slot;
                    if (surface == Surface::Rock || (face && depth >= lip && surface == Surface::Grass) || depth > dirtDepth ||
                        (stony && surface == Surface::Grass && depth < 0.5f && unit(h >> 20) < 0.6f))
                        slot = stoneAt(p, h, seed);
                    else if (surface == Surface::Sand) slot = depth > 1.5f + jitter ? stoneAt(p, h, seed) : uint8_t(Sand0 + h % 2u);
                    else if (depth < grassDepth && !(bare && unit(h >> 20) < 0.75f)) slot = uint8_t(Grass0 + h % 3u);
                    else slot = uint8_t(Dirt0 + h % 2u);
                    mat(x, y, z) = slot;

                    // In a cave or under an overhang: a few glowing mushrooms.
                    if (fresh && roofed && up > 0.6f && y + 2 < COLUMN && unit(hash(h + 88u)) < 0.03f) {
                        plant(x, y + 1, z) = GlowCap;
                        if (unit(hash(h + 89u)) < 0.5f) plant(x, y + 2, z) = GlowCap;
                    }
                    // The shore: reeds and cattails at the water's edge; pebbles and shells on the sand.
                    else if (fresh && up > 0.6f && top > SEA - 0.9f && top < SEA + 1.0f && unit(hash(h + 81u)) < reeds * 0.3f) {
                        const uint32_t r = hash(h + 82u);
                        const int height = 4 + int(r % 6u) + (top < SEA ? 4 : 0);
                        const bool cattail = unit(r >> 8) < 0.35f;
                        for (int k = 1; k <= height && y + k < COLUMN; k++) plant(x, y + k, z) = cattail && k > height - 2 ? uint8_t(Cattail) : uint8_t(Reed);
                    } else if (fresh && surface == Surface::Sand && up > 0.7f && y + 1 < COLUMN) {
                        const float roll = unit(hash(h + 83u));
                        if (roll < 0.02f) plant(x, y + 1, z) = uint8_t(Pebble0 + (h >> 4) % 2u);
                        else if (roll < 0.026f) plant(x, y + 1, z) = uint8_t(Shell0 + (h >> 4) % 2u);
                    }
                    // On a fresh grass top facing up: a grass blade, or a flower.
                    if (fresh && slot <= Grass2 && up > 0.75f && y + 1 < COLUMN && plant(x, y + 1, z) == SLOTS) {
                        const uint32_t g = hash(h + 77u);
                        if (unit(g) < flowers * 0.05f) {
                            // Flowers in clusters of a colour, a few strays of another.
                            const uint32_t cluster = hash(int(std::floor(px / 2.5f)), int(std::floor(pz / 2.5f)), seed + 611u);
                            const uint8_t colour = uint8_t(FlowerRed + (unit(g >> 8) < 0.8f ? cluster : g >> 12) % 5u);
                            const int stem = 3 + int(g >> 20) % 3;
                            for (int k = 1; k <= stem + 1 && y + k < COLUMN; k++) plant(x, y + k, z) = k <= stem ? uint8_t(Blade1) : colour;
                        } else if (unit(g >> 4) < grassy) {
                            const int blade = 2 + int(g >> 16) % 2 + int(tall * 4.0f * unit(g >> 24));
                            const uint8_t colour = dry && unit(g >> 10) < 0.7f ? uint8_t(BladeDry) : uint8_t(Blade0 + (g >> 28) % 2u);
                            for (int k = 1; k <= blade && y + k < COLUMN; k++) plant(x, y + k, z) = colour;
                        }
                    }
                }
                // The sea: one voxel of it, at its surface, where there is no ground.
                const int sea = int(SEA / VOXEL) - 1;
                if (mat(x, sea, z) == SLOTS) mat(x, sea, z) = Water;
            }

        // Boulders and trees, each stamped over its own box: testing every one at every voxel costs far
        // more. Plants where they land are cleared.
        auto stamp = [&](vec3 boxLo, vec3 boxHi, auto&& what) {
            auto cell = [&](float w, float origin) { return int(std::floor((w - origin) / VOXEL)); };
            const int x0 = std::max(0, cell(boxLo.x, ox)), x1 = std::min(RES - 1, cell(boxHi.x, ox));
            const int y0 = std::max(0, cell(boxLo.y, 0.0f)), y1 = std::min(COLUMN - 1, cell(boxHi.y, 0.0f));
            const int z0 = std::max(0, cell(boxLo.z, oz)), z1 = std::min(RES - 1, cell(boxHi.z, oz));
            for (int z = z0; z <= z1; z++)
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) what(x, y, z, vec3(ox + (float(x) + 0.5f) * VOXEL, wy(y), oz + (float(z) + 0.5f) * VOXEL));
        };
        for (const Prop& pr : props) {
            if (pr.pos.x < ox - PROP_REACH || pr.pos.x > ox + RES * VOXEL + PROP_REACH ||
                pr.pos.z < oz - PROP_REACH || pr.pos.z > oz + RES * VOXEL + PROP_REACH) continue;
            const float r = pr.kind == PropKind::GiantMushroom ? pr.cap + 0.6f : pr.kind == PropKind::Mushrooms ? 1.0f : pr.size * 0.5f + pr.radius + 0.4f;
            const float tall = pr.kind == PropKind::GiantMushroom ? pr.size + pr.cap : 2.0f;
            stamp(pr.pos - vec3(r, 0.6f, r), pr.pos + vec3(r, tall, r), [&](int x, int y, int z, vec3 q) {
                const uint8_t slot = propAt(pr, q);
                if (slot == SLOTS) return;
                if (propIsPlant(pr)) {
                    if (mat(x, y, z) == SLOTS) plant(x, y, z) = slot;
                } else {
                    mat(x, y, z) = slot;
                    plant(x, y, z) = SLOTS;
                }
            });
        }
        for (const Boulder& b : boulders) {
            if (b.centre.x < ox - BOULDER_REACH || b.centre.x > ox + RES * VOXEL + BOULDER_REACH ||
                b.centre.z < oz - BOULDER_REACH || b.centre.z > oz + RES * VOXEL + BOULDER_REACH) continue;
            const float r = b.radius * 1.2f;
            stamp(b.centre - vec3(r), b.centre + vec3(r), [&](int x, int y, int z, vec3 p) {
                const uint8_t slot = boulderAt(b, p, seed);
                if (slot == SLOTS) return;
                mat(x, y, z) = slot;
                plant(x, y, z) = SLOTS;
                // A boulder's top can carry grass too, but nothing grows under it.
            });
        }
        for (const Tree& t : trees) {
            if (t.base.x < ox - TREE_REACH || t.base.x > ox + RES * VOXEL + TREE_REACH ||
                t.base.z < oz - TREE_REACH || t.base.z > oz + RES * VOXEL + TREE_REACH) continue;
            const vec3 boxLo(t.base.x - t.reach, t.base.y, t.base.z - t.reach);
            const vec3 boxHi(t.base.x + t.reach, t.top, t.base.z + t.reach);
            stamp(boxLo, boxHi, [&](int x, int y, int z, vec3 p) {
                const uint8_t slot = treeAt(t, p);
                if (slot == SLOTS) return;
                uint8_t& m = mat(x, y, z);
                if (slot >= Leaves0 && slot <= Leaves2 && m != SLOTS && m != Water) return;   // leaves never replace ground
                m = slot;
                plant(x, y, z) = SLOTS;
            });
        }

        // Into cells.
        auto bake = [&](const std::vector<uint8_t>& from, projv::DataBlock* out, char* outFilled) {
            for (int cy = 0; cy < LAYERS; cy++) {
                auto map = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(RES));
                bool any = false;
                for (int z = 0; z < RES; z++)
                    for (int x = 0; x < RES; x++)
                        for (int y = 0; y < RES; y++) {
                            const uint8_t slot = from[(size_t(z * RES + x)) * COLUMN + size_t(cy * RES + y)];
                            if (slot == SLOTS) continue;
                            projv::utils::brickMapSetVoxel(*map, x, y, z, slot);
                            any = true;
                        }
                outFilled[cy] = any ? 1 : 0;
                if (!any) continue;
                projv::DataBlock& block = out[cy];
                block.gridX = cx;
                block.gridY = cy;
                block.gridZ = cz;
                block.geometry = projv::utils::buildTree64FromBrickMap(*map, RES);
                projv::utils::bakeMaterialsFromBrickMap(block.geometry, block.materialIDs, *map);
            }
        };
        bake(material, blocks, filled);
        bake(plants, plantBlocks, plantFilled);
    }

    inline std::string paletteJson() {
        std::string out;
        for (int i = 0; i < SLOTS; i++) {
            const Shade& s = PALETTE[i];
            out += std::string(i ? ",\n" : "") + "        { \"name\": \"" + s.name + "\", \"color\": [" + std::to_string(s.r) + ", " +
                   std::to_string(s.g) + ", " + std::to_string(s.b) + "], \"physics\": \"" + s.physics + "\"" +
                   (i == Water ? ", \"glossiness\": 0.9" : "") +
                   (i == GlowCap ? ", \"emission\": [300, 900, 1023], \"emissiveStrength\": 2.5" : "") + " }";
        }
        return out;
    }

    // Makes the island for `seed` and writes it to `folder` (compose.json; island.data, the ground, which
    // collides; plants.data, grass blades and flowers, which do not), on every core but one. `progress`,
    // if given, counts cells done (of CELLS * LAYERS * CELLS).
    inline bool generate(const std::filesystem::path& folder, uint32_t seed, std::atomic<int>* progress = nullptr) {
        const int total = CELLS * LAYERS * CELLS;
        std::vector<projv::DataBlock> blocks(static_cast<size_t>(total)), plantBlocks(static_cast<size_t>(total));   // column by column, bottom first
        std::vector<char> filled(static_cast<size_t>(total), 0), plantFilled(static_cast<size_t>(total), 0);
        const std::vector<Tree> trees = treesNear(vec3(0.0f), vec3(SIZE, HEIGHT, SIZE), seed);
        const std::vector<Boulder> boulders = bouldersNear(vec3(0.0f), vec3(SIZE, HEIGHT, SIZE), seed);
        const std::vector<Prop> props = propsNear(vec3(0.0f), vec3(SIZE, HEIGHT, SIZE), seed);
        std::atomic<int> next{0};
        auto work = [&] {
            for (int i; (i = next.fetch_add(1)) < CELLS * CELLS;) {
                const size_t at = size_t(i * LAYERS);
                makeColumn(i % CELLS, i / CELLS, seed, trees, boulders, props, &blocks[at], &filled[at], &plantBlocks[at], &plantFilled[at]);
                if (progress) progress->fetch_add(LAYERS);
            }
        };
        const unsigned threads = std::max(1u, std::thread::hardware_concurrency() - 1u);
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; t++) pool.emplace_back(work);
        for (std::thread& t : pool) t.join();

        std::error_code error;
        std::filesystem::create_directories(folder, error);
        auto write = [&](const char* name, std::vector<projv::DataBlock>& from, const std::vector<char>& used) {
            projv::DataFile file;
            file.resolution = RES;
            file.voxelScale = VOXEL;
            for (int i = 0; i < total; i++)
                if (used[size_t(i)]) file.blocks.push_back(std::move(from[size_t(i)]));
            projv::core::info("island: {}: {} cells of {}", name, file.blocks.size(), total);
            return projv::utils::writeDataFile((folder / name).string(), file);
        };
        if (!write("island.data", blocks, filled) || !write("plants.data", plantBlocks, plantFilled)) return false;
        std::ofstream(folder / "compose.json") << "{\n    \"version\": 1,\n    \"name\": \"island\",\n    \"components\": [\n"
                                                  "      { \"type\": \"data\", \"source\": \"island.data\", \"id\": 1, \"name\": \"island\",\n"
                                                  "        \"palette\": [\n" << paletteJson() << "\n        ] },\n"
                                                  "      { \"type\": \"data\", \"source\": \"plants.data\", \"id\": 2, \"name\": \"plants\",\n"
                                                  "        \"palette\": [\n" << paletteJson() << "\n        ] }\n    ]\n}\n";
        projv::core::info("island: seed {}: {} trees, {} boulders, {} props, written to {}", seed, trees.size(), boulders.size(), props.size(), folder.string());
        return true;
    }

    // Where to start: gentle grass near the middle, fairly high, clear of trees above it.
    inline vec3 spawnPoint(uint32_t seed) {
        vec3 best(CENTRE, surfaceAt(CENTRE, CENTRE, seed) + 2.0f, CENTRE);
        float bestScore = -1e9f;
        for (int i = 0; i < 300; i++) {
            const float a = unit(hash(i, 1, seed)) * 6.2831853f, r = 45.0f * std::sqrt(unit(hash(i, 2, seed)));
            const float x = CENTRE + std::cos(a) * r, z = CENTRE + std::sin(a) * r;
            const float surface = surfaceAt(x, z, seed);
            if (surface < SEA + 2.0f || surface > ROCK_LINE - 4.0f) continue;
            if (slopeAt(x, z, surface, seed) > 0.35f) continue;
            bool covered = false;   // under a crown, or too near a trunk
            for (const Tree& t : treesNear(vec3(x - 1.0f, 0.0f, z - 1.0f), vec3(x + 1.0f, HEIGHT, z + 1.0f), seed)) {
                const float dx = t.base.x - x, dz = t.base.z - z;
                if (dx * dx + dz * dz < (t.reach + 1.0f) * (t.reach + 1.0f)) covered = true;
            }
            if (covered) continue;
            const float score = -r * 0.2f + surface * 0.08f;
            if (score > bestScore) {
                bestScore = score;
                best = vec3(x, surface + 1.5f, z);
            }
        }
        return best;
    }
}

#endif
