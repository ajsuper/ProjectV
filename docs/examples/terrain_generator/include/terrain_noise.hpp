// ProjectV Terrain Generator - Advanced Noise System
// Ported from ~/Development/Development_Synced/TerrainTest/terrain.cpp
// Streaming layered terrain generation. Three scales, largest first:
//
//   Continents  - continentField decides land from sea, the shelf, and the interior swell that all
//                 elevations are measured against; upliftBelt decides which of that land is a
//                 mountain range, in long sinuous belts rather than scattered massifs.
//   Landforms   - 8 archetypes (mountains / plains / badlands / dunes / plateau / downs / jagged /
//                 mesa) blended by low-frequency weights, their amplitude scaled by the continent
//                 and their relief multiplied up inside an uplift belt.
//   Surface     - Phacelle-noise erosion for drainage texture, Worley gullies, per-voxel detail.
//
// Cutting across all three: climate (temperature with an altitude lapse rate, plus humidity), which
// drives landform selection, surface colour, rock type and tree cover; and rivers, which are level
// sets of a smooth field carved into the land, carrying their own local water surface so water can
// exist above sea level.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/math.h"

namespace terrain_noise {

using projv::core::vec2;
using projv::core::vec3;

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float smoothstep(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Ken Perlin's improved noise, 2D, with a seeded permutation table.
class Perlin {
public:
    explicit Perlin(uint32_t seed) { reseed(seed); }

    void reseed(uint32_t seed) {
        for (int i = 0; i < 256; ++i) perm_[i] = i;
        uint32_t s = seed ? seed : 1u;
        for (int i = 255; i > 0; --i) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            int j = int(s % uint32_t(i + 1));
            std::swap(perm_[i], perm_[j]);
        }
        for (int i = 0; i < 256; ++i) perm_[256 + i] = perm_[i];
    }

    float noise(float x, float y) const {
        int xi = int(std::floor(x)) & 255;
        int yi = int(std::floor(y)) & 255;
        float xf = x - std::floor(x);
        float yf = y - std::floor(y);
        float u = fade(xf), v = fade(yf);

        int aa = perm_[perm_[xi] + yi];
        int ab = perm_[perm_[xi] + yi + 1];
        int ba = perm_[perm_[xi + 1] + yi];
        int bb = perm_[perm_[xi + 1] + yi + 1];

        float x1 = lerpf(grad(aa, xf, yf), grad(ba, xf - 1, yf), u);
        float x2 = lerpf(grad(ab, xf, yf - 1), grad(bb, xf - 1, yf - 1), u);
        return lerpf(x1, x2, v);
    }

private:
    static float fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
    static float grad(int hash, float x, float y) {
        switch (hash & 7) {
            case 0: return  x + y;
            case 1: return  x - y;
            case 2: return -x + y;
            case 3: return -x - y;
            case 4: return  x;
            case 5: return -x;
            case 6: return  y;
            default: return -y;
        }
    }
    int perm_[512];
};

static float fbm(const Perlin& p, float x, float y, int octaves, float lacunarity, float gain) {
    float value = 0, amplitude = 1, frequency = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        value += amplitude * p.noise(x * frequency, y * frequency);
        norm += amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }
    return value / norm;
}

// Worley (cellular) noise
class Worley {
public:
    explicit Worley(uint32_t seed) { reseed(seed); }
    void reseed(uint32_t s) { seed_ = s ? s : 1u; }

    float edge(float x, float y) const {
        int xi = int(std::floor(x)), yi = int(std::floor(y));
        float fx = x - xi, fy = y - yi;
        float f1 = 1e9f, f2 = 1e9f;
        for (int dj = -1; dj <= 1; ++dj) {
            for (int di = -1; di <= 1; ++di) {
                uint32_t h = hash(xi + di, yi + dj);
                float px = float(di) + unitFloat(h);
                float py = float(dj) + unitFloat(h * 1664525u + 1013904223u);
                float dx = px - fx, dy = py - fy;
                float d = dx * dx + dy * dy;
                if (d < f1)      { f2 = f1; f1 = d; }
                else if (d < f2) { f2 = d; }
            }
        }
        return std::sqrt(f2) - std::sqrt(f1);
    }

    float cell(float x, float y, float* cellHashOut = nullptr) const {
        int xi = int(std::floor(x)), yi = int(std::floor(y));
        float fx = x - xi, fy = y - yi;
        float f1 = 1e9f, f2 = 1e9f;
        uint32_t nearest = 0;
        for (int dj = -1; dj <= 1; ++dj) {
            for (int di = -1; di <= 1; ++di) {
                uint32_t h = hash(xi + di, yi + dj);
                float px = float(di) + unitFloat(h);
                float py = float(dj) + unitFloat(h * 1664525u + 1013904223u);
                float dx = px - fx, dy = py - fy;
                float d = dx * dx + dy * dy;
                if (d < f1)      { f2 = f1; f1 = d; nearest = h; }
                else if (d < f2) { f2 = d; }
            }
        }
        float d1 = std::sqrt(f1);
        if (cellHashOut) *cellHashOut = float(nearest & 0xFF) * (1.0f / 255.0f);
        return clampf(d1 * 1.8f, 0.0f, 1.0f);
    }

private:
    uint32_t hash(int x, int y) const {
        uint32_t h = seed_ ^ (uint32_t(x) * 374761393u) ^ (uint32_t(y) * 668265263u);
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }
    static float unitFloat(uint32_t h) {
        return float(h & 0xFFFFFFu) * (1.0f / float(0x1000000));
    }
    uint32_t seed_;
};

static float worleyGullies(const Worley& w, float x, float y, int octaves,
                           float lacunarity, float gain) {
    float sum = 0.0f, freq = 1.0f, amp = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        float e = w.edge(x * freq, y * freq);
        sum += amp * clampf(1.0f - e * 2.6f, 0.0f, 1.0f);
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

// ---------------------------------------------------------------- erosion ----

static const float kTau = 6.28318530717959f;

static vec2 phacelleHash(int gx, int gy, uint32_t seed) {
    uint32_t h = seed ^ (uint32_t(gx) * 374761393u) ^ (uint32_t(gy) * 668265263u);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    uint32_t h2 = h * 2246822519u + 2654435761u;
    return {float(h & 0xFFFFu) * (2.0f / 65535.0f) - 1.0f,
            float(h2 & 0xFFFFu) * (2.0f / 65535.0f) - 1.0f};
}

static void phacelleNoise(vec2 p, vec2 normDir, float freq, float offset,
                          float normalization, uint32_t seed, float out[4]) {
    vec2 sideDir = vec2{-normDir.y, normDir.x} * (freq * kTau);
    offset *= kTau;

    int pIntX = int(std::floor(p.x)), pIntY = int(std::floor(p.y));
    vec2 pFrac{p.x - pIntX, p.y - pIntY};
    vec2 phaseDir{0, 0};
    float weightSum = 0.0f;

    for (int j = -1; j <= 2; ++j) {
        for (int i = -1; i <= 2; ++i) {
            vec2 gridOffset{float(i), float(j)};
            vec2 randomOffset = phacelleHash(pIntX + i, pIntY + j, seed) * 0.5f;
            vec2 v = pFrac - gridOffset - randomOffset;

            float sqrDist = v.x * v.x + v.y * v.y;
            float weight = std::exp(-sqrDist * 2.0f);
            weight = std::max(0.0f, weight - 0.01111f);
            weightSum += weight;

            float waveInput = v.x * sideDir.x + v.y * sideDir.y + offset;
            phaseDir += vec2{std::cos(waveInput), std::sin(waveInput)} * weight;
        }
    }

    vec2 interp = phaseDir * (weightSum > 1e-20f ? 1.0f / weightSum : 0.0f);
    float magnitude = std::max(1.0f - normalization, std::sqrt(interp.x * interp.x + interp.y * interp.y));
    float inv = magnitude > 1e-20f ? 1.0f / magnitude : 0.0f;
    out[0] = interp.x * inv;
    out[1] = interp.y * inv;
    out[2] = sideDir.x;
    out[3] = sideDir.y;
}

struct ErosionParams {
    float scale;
    float strength;
    float gullyWeight;
    float detail;
    float rounding[4];
    float onset[4];
    float assumedSlope[2];
    float cellScale;
    float normalization;
    float lacunarity;
    float gain;
};

static float eroPowInv(float t, float power) {
    return 1.0f - std::pow(1.0f - clampf(t, 0.0f, 1.0f), power);
}
static float eroEaseOut(float t) {
    float v = 1.0f - clampf(t, 0.0f, 1.0f);
    return 1.0f - v * v;
}
static float eroSmoothStart(float t, float smoothing) {
    if (t >= smoothing) return t - 0.5f * smoothing;
    return smoothing > 1e-20f ? 0.5f * t * t / smoothing : 0.0f;
}
static vec2 eroSafeNormalize(vec2 n) {
    float l = std::sqrt(n.x * n.x + n.y * n.y);
    return std::fabs(l) > 1e-10f ? n * (1.0f / l) : n;
}

static float erosionFilter(vec2 p, float height, vec2 slope, float fadeTarget,
                           const ErosionParams& ep, int octaves, uint32_t seed,
                           float& ridgeMap) {
    float strength = ep.strength * ep.scale;
    fadeTarget = clampf(fadeTarget, -1.0f, 1.0f);

    float inputHeight = height;
    float freq = 1.0f / (ep.scale * ep.cellScale);
    float slopeLength = std::max(std::sqrt(slope.x * slope.x + slope.y * slope.y), 1e-10f);
    float roundingMult = 1.0f;

    float roundingForInput =
        lerpf(ep.rounding[1], ep.rounding[0], clampf(fadeTarget + 0.5f, 0.0f, 1.0f)) * ep.rounding[2];
    float combiMask = eroEaseOut(eroSmoothStart(slopeLength * ep.onset[0],
                                                roundingForInput * ep.onset[0]));

    float ridgeMapCombiMask = eroEaseOut(slopeLength * ep.onset[2]);
    float ridgeMapFadeTarget = fadeTarget;

    vec2 gullySlope = slope * (1.0f - ep.assumedSlope[1])
                    + (slope * (1.0f / slopeLength) * ep.assumedSlope[0]) * ep.assumedSlope[1];

    for (int i = 0; i < octaves; ++i) {
        float ph[4];
        phacelleNoise(p * freq, eroSafeNormalize(gullySlope), ep.cellScale, 0.25f,
                      ep.normalization, seed, ph);
        vec2 phDeriv{ph[2] * -freq, ph[3] * -freq};
        float sloping = std::fabs(ph[1]);

        gullySlope += phDeriv * (std::copysign(1.0f, ph[1]) * strength * ep.gullyWeight);

        float gulliesH = ph[0];
        vec2 gulliesD = phDeriv * ph[1];
        float fadedH = lerpf(fadeTarget, gulliesH * ep.gullyWeight, combiMask);
        height += fadedH * strength;

        fadeTarget = fadedH;

        float roundingForOctave =
            lerpf(ep.rounding[1], ep.rounding[0], clampf(ph[0] + 0.5f, 0.0f, 1.0f)) * roundingMult;
        float newMask = eroEaseOut(eroSmoothStart(sloping * ep.onset[1],
                                                  roundingForOctave * ep.onset[1]));
        combiMask = eroPowInv(combiMask, ep.detail) * newMask;

        ridgeMapFadeTarget = lerpf(ridgeMapFadeTarget, gulliesH, ridgeMapCombiMask);
        ridgeMapCombiMask *= eroEaseOut(sloping * ep.onset[3]);
        (void)gulliesD;

        strength *= ep.gain;
        freq *= ep.lacunarity;
        roundingMult *= ep.rounding[3];
    }

    ridgeMap = ridgeMapFadeTarget * (1.0f - ridgeMapCombiMask);
    return height - inputHeight;
}

const float kWarpReach = 2.0f;

static void domainWarp(const Perlin& w, float& x, float& y, float strength) {
    if (strength <= 0.0f) return;
    float wx = fbm(w, x + 1.7f, y + 9.2f, 4, 2.0f, 0.5f);
    float wy = fbm(w, x + 5.2f, y + 1.3f, 4, 2.0f, 0.5f);
    x += wx * strength * kWarpReach;
    y += wy * strength * kWarpReach;
}

// ------------------------------------------------------------ constants ----

enum Archetype {
    ARCH_MOUNTAIN = 0, ARCH_PLAINS, ARCH_BADLANDS,
    ARCH_DUNES, ARCH_PLATEAU, ARCH_DOWNS, ARCH_JAGGED, ARCH_MESA, ARCH_COUNT
};

// Mountain and jagged amplitudes are the ones the uplift belt multiplies up (see upliftBelt and
// reliefFactor); the value here is what a range reaches OUTSIDE a belt core, not its peak.
static const float ARCH_AMP[ARCH_COUNT] = {
    4000.0f, 200.0f, 1200.0f, 0.0f, 2200.0f, 650.0f, 3200.0f, 1500.0f};

static const float ARCH_SEL_FREQ[ARCH_COUNT] = {
    0.000038f, 0.000065f, 0.000092f, 0.000110f, 0.000050f, 0.000078f, 0.000052f, 0.000095f};

// Mesa is the smoothest archetype in this table on purpose: its tables are dead flat and its cliffs
// come from the tier risers, so adding fbm detail on top only frosts the flats with noise.
static const float ARCH_ROUGH[ARCH_COUNT] = {
    0.32f, 0.10f, 0.34f, 0.14f, 0.30f, 0.28f, 0.26f, 0.12f};

static const float ARCH_CLIMATE[ARCH_COUNT][4] = {
    {0.30f, 0.35f, 0.48f, 0.45f},
    {0.55f, 0.30f, 0.50f, 0.30f},
    {0.85f, 0.20f, 0.18f, 0.22f},
    {0.90f, 0.16f, 0.10f, 0.18f},
    {0.55f, 0.25f, 0.32f, 0.28f},
    {0.30f, 0.30f, 0.72f, 0.28f},
    {0.35f, 0.38f, 0.42f, 0.48f},
    // Mesa: hot and properly arid, widened so mesa country covers more of the hot dry world
    // and creates larger stretches of canyon / terracotta terrain. This is the band
    // rock_detail's clay/terracotta colours key off as well.
    {0.70f, 0.20f, 0.19f, 0.22f},
};

// Plains and downs used to be the only archetypes with NO erosion pass at all (flat {} == strength
// 0 == archEroded() false), so those regions never got the gully/drainage texture that
// mountains/badlands/plateau/jagged already have. Plains gets a very light touch (it should still
// read as mostly smooth), downs a bit more (rolling hills read well with soft drainage lines).
// Dunes stays erosion-free: wind-formed ridges, not water drainage, are the whole point of its own
// shape function.
static const ErosionParams ARCH_EROSION[ARCH_COUNT] = {
    {0.15f, 0.38f, 0.70f, 1.4f, {0.22f,0.10f,0.10f,2.0f}, {0.58f,1.10f,2.80f,1.5f}, {0.72f,1.0f}, 0.85f, 0.42f, 2.0f, 0.52f},
    {0.22f, 0.16f, 0.28f, 1.1f, {0.24f,0.12f,0.10f,2.0f}, {0.48f,1.05f,2.80f,1.2f}, {0.80f,1.0f}, 0.92f, 0.42f, 2.0f, 0.52f},
    {0.09f, 0.40f, 0.45f, 2.2f, {0.10f,0.04f,0.10f,2.0f}, {0.55f,1.40f,2.80f,1.7f}, {0.78f,1.0f}, 0.75f, 0.45f, 2.0f, 0.52f},
    {},
    {0.12f, 0.55f, 0.90f, 1.2f, {0.18f,0.09f,0.10f,2.0f}, {0.38f,1.15f,2.80f,1.4f}, {0.66f,1.0f}, 0.80f, 0.40f, 2.0f, 0.52f},
    {0.16f, 0.28f, 0.42f, 1.3f, {0.18f,0.08f,0.10f,2.0f}, {0.42f,1.10f,2.80f,1.3f}, {0.72f,1.0f}, 0.85f, 0.42f, 2.0f, 0.52f},
    {0.15f, 0.28f, 0.35f, 1.2f, {0.18f,0.06f,0.10f,2.0f}, {0.35f,1.10f,2.80f,1.3f}, {0.70f,1.0f}, 0.85f, 0.42f, 2.0f, 0.52f},
    // Mesa. Weak and fine-scaled compared to badlands: the erosion pass here is not meant to shape
    // the landform (the tier function already did that) but to ravel the cliff faces and build a
    // talus apron at their feet. Its onset values are high so the filter engages only where the
    // ground is already steep -- i.e. on the risers -- and leaves the tables untouched.
    {0.07f, 0.30f, 0.55f, 1.8f, {0.12f,0.05f,0.10f,2.0f}, {0.95f,1.60f,2.80f,1.9f}, {0.80f,1.0f}, 0.70f, 0.44f, 2.0f, 0.52f},
};
static bool archEroded(int a) { return ARCH_EROSION[a].strength > 0.0f; }

static const float MTN_FREQ  = 0.00016f;
static const float PLN_FREQ  = 0.00060f;
static const float BAD_FREQ  = 0.00040f;
static const float PLAT_FREQ = 0.00018f;
static const float PLAT_TILT_FREQ = 0.000012f;
static const float DOWN_FREQ = 0.00030f;
static const float JAG_FREQ = 0.00035f;
static const float MESA_FREQ = 0.00033f;
static const float RELIEF_FREQ = 0.000040f;
static const float HUMID_FREQ = 0.000015f;
static const float BLEND_MOD_FREQ = 0.000030f;
static const float TEMP_FREQ = 0.000008f;
static const float DETAIL_FREQ = 0.0022f;
static const float DETAIL_GULLY_FREQ = 0.0035f;

// ------------------------------------------------------- continents and tectonics ----
//
// Everything above this line describes terrain at the scale of a landform -- a range, a dune field,
// a run of badlands. None of it decides whether there is land here at all, which is why the world
// used to read as one endless mid-altitude surface: the old CONT_FREQ field was a +/-500 unit bias
// on that surface, not a coastline.
//
// The two fields below are the layer above landforms:
//
//   continentField -- where the crust is thick. Drives sea/land, the shelf, and the broad interior
//                     swell, and scales every archetype's amplitude so the sea floor stays a sea
//                     floor instead of growing its own mountains under the water.
//   upliftBelt     -- where two of them met. A ridged, heavily warped field whose crests are long
//                     sinuous lines rather than blobs, which is what makes a RANGE instead of the
//                     scattered massifs the archetype selection noise produces on its own.
//
// Frequencies are in the terrain's own (pre-kTerrainScale) coordinates. At the 2x world stretch
// main.cpp applies, CONTINENT_FREQ puts a landmass at roughly 180k world units across -- several
// times the view radius, so a coast is somewhere you travel to.
static const float CONTINENT_FREQ = 0.0000115f;
static const float UPLIFT_FREQ    = 0.0000190f;

// World height the water plane sits at. Land elevations below are all expressed relative to this so
// the coastline lands where the water actually is; main.cpp mirrors it in TerrainState::kWaterLevel.
static constexpr float SEA_LEVEL = 400.0f;

// Normalization reference for "how high up am I" (altN) queries: heights above SEA_LEVEL are
// divided by this to get the 0..1 altitude that drives snow line, rock exposure and river gating.
// It is deliberately a little BELOW the true maximum, so the handful of summits that exceed it
// saturate at altN 1 -- which is what you want, since everything altN feeds is already at its
// endpoint by then.
static constexpr float MAX_HEIGHT = 11000.0f;

// Hard ceiling on generated terrain, and what main.cpp sizes its vertical chunk range from. Terrain
// above this simply has no chunks to live in, so it must clear the tallest peak a belt core can
// produce (roughly mountain amplitude x the relief ceiling upliftBelt pushes reliefFactor to, plus
// the continental swell underneath) with room to spare. Sampling six seeds over a 1.8M-unit square
// tops out near 6100, so this leaves better than 15%.
//
// sampleTerrain clamps to it as well. Sampling can only ever bound the maximum of an unbounded
// noise field from below, and the failure mode if a summit does exceed it is not a slightly flat
// peak -- it is a peak whose top chunks were never enumerated, i.e. a hole in the mountain.
static constexpr float TERRAIN_CEILING = 14000.0f;

// ------------------------------------------------------------------ rivers ----
//
// Frequency of the river trunk network, again in pre-stretch coordinates. About a 50k-world-unit
// spacing between major valleys, with two finer generations of tributaries between them.
//
// Worth knowing when retuning: for a level-set network, the fraction of the map a channel covers is
// roughly 2 x half-width / spacing, and it is the same for every generation of a self-similar
// network -- halving the spacing and halving the width leaves coverage unchanged. So tributaries do
// not come free, and the way to keep the map from turning into marsh is fewer generations and wider
// spacing, not thinner channels.
static const float RIVER_FREQ = 0.000040f;

struct Generator {
    Perlin shapeN[ARCH_COUNT] = {Perlin(0), Perlin(0), Perlin(0), Perlin(0),
                                 Perlin(0), Perlin(0), Perlin(0), Perlin(0)};
    Perlin selN[ARCH_COUNT]   = {Perlin(0), Perlin(0), Perlin(0), Perlin(0),
                                 Perlin(0), Perlin(0), Perlin(0), Perlin(0)};
    Perlin warpN[ARCH_COUNT]  = {Perlin(0), Perlin(0), Perlin(0), Perlin(0),
                                 Perlin(0), Perlin(0), Perlin(0), Perlin(0)};
    Perlin relief{0}, cont{0}, tempField{0}, humid{0};
    Perlin warpClimate{0}, patchA{0}, patchB{0}, detailN{0}, blendModN{0}, microN{0};
    Perlin contWarp{0}, upliftN{0}, upliftWarp{0}, riverN{0};
    Worley gully{0}, rockNoise{0};
    uint32_t erosionSeed = 0;

    void reseed(uint32_t seed) {
        erosionSeed = seed * 2891336453u + 12653u;
        for (int a = 0; a < ARCH_COUNT; ++a) {
            shapeN[a].reseed(seed * 2654435761u + uint32_t(a) * 7919u + 11u);
            selN[a].reseed(seed * 40503u + uint32_t(a) * 6151u + 101u);
            warpN[a].reseed(seed * 1103515245u + uint32_t(a) * 3607u + 12345u);
        }
        relief.reseed(seed * 69069u + 907u);
        cont.reseed(seed * 1664525u + 1013u);
        tempField.reseed(seed * 1664525u + 6553u);
        humid.reseed(seed * 22695477u + 7u);
        warpClimate.reseed(seed * 22695477u + 4409u);
        patchA.reseed(seed * 987654321u + 17u);
        patchB.reseed(seed * 987654321u + 8191u);
        detailN.reseed(seed * 2246822519u + 331u);
        blendModN.reseed(seed * 2246822519u + 1337u);
        microN.reseed(seed * 2246822519u + 5555u);
        contWarp.reseed(seed * 1664525u + 40961u);
        upliftN.reseed(seed * 3266489917u + 2749u);
        upliftWarp.reseed(seed * 3266489917u + 6151u);
        riverN.reseed(seed * 374761393u + 811u);
        gully.reseed(seed * 3266489917u + 77u);
        rockNoise.reseed(seed * 3266489917u + 12345u);
    }

    // ---------------------------------------------------------- continents ----

    // 0 = abyssal ocean, ~0.45 = coastline, 1 = deep continental interior.
    //
    // Five octaves rather than the old four, and warped: the extra octave is what gives a coast
    // bays and headlands at a scale you can see from the ground, and the warp keeps the landmass
    // from reading as a circular blob.
    //
    // The gain-and-offset on the way out is not cosmetic. A normalized fbm does not fill [0,1] --
    // measured over this one, the middle 98% of the world lands between 0.31 and 0.67. Mapped
    // straight through, every threshold below near either end is unreachable: the abyssal branch of
    // continentBase never fires, so "ocean" comes out as one uniform shallow shelf at roughly the
    // same depth everywhere, and the deep-interior branch never fires either, so continents have no
    // high dry heart. Stretching the measured spread across the full range is what turns the field
    // from a gentle undulation into a map with basins and interiors on it.
    float continentField(float x, float z) const {
        float nx = x * CONTINENT_FREQ, nz = z * CONTINENT_FREQ;
        domainWarp(contWarp, nx, nz, 0.60f);
        float n = fbm(cont, nx, nz, 5, 2.05f, 0.5f);
        return clampf(n * 2.2f + 0.617f, 0.0f, 1.0f);
    }

    // How much of a landmass this is: 0 well offshore, 1 well inland. Also the gate on archetype
    // amplitude, so relief is a property of continents and the sea floor stays quiet.
    static float landMask(float cf) {
        return smoothstep(0.380f, 0.550f, cf);
    }

    // Elevation of the crust itself, before any landform sits on it. Three regimes joined at the
    // shelf break: abyssal plain, continental shelf, and the interior swell that puts inland
    // ground several hundred units above the coast before a single mountain is added.
    static float continentBase(float cf) {
        float ocean = lerpf(SEA_LEVEL - 950.0f, SEA_LEVEL - 105.0f, smoothstep(0.02f, 0.38f, cf));
        float land  = lerpf(SEA_LEVEL + 18.0f,  SEA_LEVEL + 640.0f, smoothstep(0.44f, 0.92f, cf));
        return lerpf(ocean, land, smoothstep(0.380f, 0.520f, cf));
    }

    // Orogenic belts. `1 - |noise|` turns a smooth field's zero SET -- a curve, not a region --
    // into a crest, so the result is inherently linear where fbm alone is blobby. Three octaves
    // give the belt a spine plus foothills; the heavy warp bends the whole system into arcs.
    //
    // Returned already gated to the strong cores: a belt is a narrow thing with a lot of ordinary
    // land between belts, which is what makes the ranges read as towering rather than as the
    // world's average altitude.
    float upliftBelt(float x, float z) const {
        float nx = x * UPLIFT_FREQ, nz = z * UPLIFT_FREQ;
        domainWarp(upliftWarp, nx, nz, 0.95f);
        float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
        for (int i = 0; i < 3; ++i) {
            float r = 1.0f - std::fabs(upliftN.noise(nx * freq, nz * freq)) * 2.6f;
            sum += amp * clampf(r, 0.0f, 1.0f);
            norm += amp;
            freq *= 2.1f;
            amp *= 0.48f;
        }
        // Two competing requirements, and the shape here is the compromise between them.
        //
        // Gated LATE, because a ridged field's crest is broad: opening at the middle of its measured
        // range puts half the world inside a "belt", which makes uplift the world's average
        // condition and defeats the point of having belts at all.
        //
        // But gated WIDE, because everything downstream sharpens this again -- the mountain claim
        // multiplies by it and then smoothsteps, weightFromClaim squares, the dominance pass squares
        // again -- so height ends up something like the fourth power of what comes out of here. With
        // a tight gate the compounding put 98% of a range's entire rise into the last fifth of the
        // approach: 2,000 units of mountain arriving over one sample, a wall standing on a plain
        // with no foothills in front of it.
        //
        // The pow is the part that actually buys foothills. Widening the smoothstep alone moves the
        // whole wall outward without softening it, because a smoothstep is still flat at its foot;
        // an exponent below one lifts the low end off zero, so the ground starts climbing early and
        // keeps climbing, which is what an approach to a range looks like.
        //
        // Lowered from 0.62. Note what this knob does NOT do: pow(0)=0 and pow(1)=1, so the belt
        // covers exactly the same ground as before and the interiors stay as clear as ever -- only
        // the PROFILE across the approach changes. That is why it is the safe place to buy
        // gradualness, and why the fix for "mountains arrive too suddenly" is here and in the
        // mountain claim rather than in the smoothstep bounds, which would widen the belts
        // themselves and make uplift the world's default condition.
        return std::pow(smoothstep(0.30f, 0.95f, sum / norm), 0.50f);
    }

    // Amplitude multiplier for the landform layer. Was a standalone noise field; it now carries the
    // uplift belt as well, which is the single term that makes a range tower over its own foothills
    // instead of every mountain region topping out at the same altitude.
    float reliefFactor(float x, float z, float uplift) const {
        float n = fbm(relief, x * RELIEF_FREQ, z * RELIEF_FREQ, 3, 2.0f, 0.5f) * 0.5f + 0.5f;
        float base = lerpf(0.15f, 1.35f, smoothstep(0.15f, 0.85f, n));
        return base * (1.0f + 1.60f * uplift);
    }

    // ------------------------------------------------------------- rivers ----

    struct River {
        float valley  = 0.0f;   // 0 outside .. 1 on the axis of the carved valley
        float channel = 0.0f;   // 0 outside .. 1 in the wetted channel itself
        float order   = 0.0f;   // 0.26 headwater .. 1 trunk; scales width and depth
        vec2  center{0.0f, 0.0f};  // nearest point on the channel centreline
        vec2  normal{0.0f, 0.0f};  // unit vector across the channel, pointing at the banks
        float flare = 0.0f;        // distance from the axis to the top of the valley
        float bankiness = 0.0f;    // 1 on the bank strip either side of the channel, 0 elsewhere
    };

    // Half-widths of the three generations, in pre-stretch units -- double them for world size, so
    // the trunk is about 320 world units bank to bank. They fall off faster than the spacing between
    // generations grows, which is what makes a headwater a thread and a trunk a river rather than
    // all three covering the same share of the map (see the note on RIVER_FREQ).
    static constexpr float kRiverHalfWidth[3] = {160.0f, 52.0f, 17.0f};
    // How far past the wetted channel the carved valley reaches, as a multiple of half-width.
    static constexpr float kValleyFlare = 2.8f;
    static constexpr float kRiverScale[3] = {1.0f, 2.7f, 6.8f};
    static constexpr float kRiverOrder[3] = {1.0f, 0.52f, 0.26f};
    static constexpr float kRiverOfsX[3]  = {0.0f, 21.7f, 53.1f};
    static constexpr float kRiverOfsZ[3]  = {0.0f, 9.3f, 37.7f};

    // One generation's signed field, in world coordinates.
    float riverValue(float x, float z, int i) const {
        float f = RIVER_FREQ * kRiverScale[i];
        return fbm(riverN, x * f + kRiverOfsX[i], z * f + kRiverOfsZ[i], 4, 2.0f, 0.5f);
    }

    // A river network is the ZERO SET of a smooth field, not its peaks.
    //
    // That choice is what makes this work at all. The level set of a 2D field is a family of curves
    // that never terminate and never cross, which is the topology a drainage network has, and it
    // comes out sinuous and branching for free -- where ridged noise (used above for uplift) gives
    // crests that are lines but whose WIDTH varies with the field's gradient, a level set gives a
    // line whose distance function we can actually recover, and distance from the axis is the one
    // thing a river needs: it decides bank, channel and valley.
    //
    // |v| alone is not that distance. It is the field value, which shrinks near a flat spot and
    // steepens near a fold, so a fixed |v| threshold gives a river that balloons and pinches for no
    // reason. |v| / |grad v| is the first-order distance to the zero set, and measuring the gradient
    // rather than assuming it is what keeps the channel a consistent width along its length.
    //
    // There is deliberately NO domain warp on these fields, unlike every other field in this file.
    // A warp of the strength used elsewhere has a Jacobian around five, so a gradient measured in
    // warped coordinates converts to world distance five times wrong -- which does not show up as a
    // subtly mis-sized river, it shows up as every channel in the world collapsing to about a fifth
    // of its intended width, a thread one voxel across. Differencing through the warp instead would
    // fix it at the cost of four more warp evaluations per terrain sample. It is not worth it: a
    // four-octave fbm's zero set already meanders, and the warp was only adding meander.
    River riverField(float x, float z) const {
        River r;
        r.center = vec2{x, z};

        // Rank the generations on |v| scaled by each one's frequency. That is the true distance
        // times the field's own gradient, which is drawn from the same distribution for all three,
        // so the ranking is fair even though the number is not yet a distance.
        int bi = -1;
        float bestQ = 1e9f, v[3];
        for (int i = 0; i < 3; ++i) {
            v[i] = riverValue(x, z, i);
            float q = std::fabs(v[i]) / (kRiverHalfWidth[i] * RIVER_FREQ * kRiverScale[i]);
            if (q < bestQ) { bestQ = q; bi = i; }
        }
        // Generously outside every generation -- no gradient can bring it back, since |grad| of a
        // normalized fbm does not reach 8. Skips the differencing over most of the map, which is
        // what keeps rivers cheap.
        if (bi < 0 || bestQ > kValleyFlare * 8.0f) return r;

        // Central differences in world units, at half the channel's own half-width so the finest
        // generation is not smeared by its own probe.
        const float e = kRiverHalfWidth[bi] * 0.5f;
        float gx = (riverValue(x + e, z, bi) - riverValue(x - e, z, bi)) / (2.0f * e);
        float gz = (riverValue(x, z + e, bi) - riverValue(x, z - e, bi)) / (2.0f * e);
        float gl2 = gx * gx + gz * gz;
        if (gl2 < 1e-20f) return r;

        float q = std::fabs(v[bi]) / std::sqrt(gl2) / kRiverHalfWidth[bi];
        if (q > kValleyFlare) return r;

        // One Newton step along the gradient lands on the zero set. The datum a valley is cut
        // relative to is sampled THERE rather than here, which is what keeps a river's water surface
        // level across its own width: sampled per-column, the datum tilts with the hillside and one
        // bank ends up dry while the other is metres deep.
        float k = v[bi] / gl2;
        r.center = vec2{x - gx * k, z - gz * k};

        // The field's gradient is perpendicular to its own level set, so normalizing it gives the
        // across-channel direction for free. The caller probes the banks along it.
        float invG = 1.0f / std::sqrt(gl2);
        r.normal = vec2{gx * invG, gz * invG};
        r.flare  = kValleyFlare * kRiverHalfWidth[bi];
        r.order   = kRiverOrder[bi];
        r.channel = 1.0f - smoothstep(0.55f, 1.05f, q);
        r.valley  = 1.0f - smoothstep(0.15f, kValleyFlare, q);
        // Peaks just outside the wetted channel and falls off through the valley: rockfall collects
        // on the banks and in the shallows, not out on the floodplain.
        r.bankiness = smoothstep(0.30f, 0.85f, q) * (1.0f - smoothstep(1.10f, 2.10f, q));
        return r;
    }

    // Boulders strewn along a river.
    //
    // A channel cut by a level set is a smooth ribbon, and a smooth ribbon reads as a canal however
    // well the water in it behaves. Real rivers are edged with rockfall and are visibly ragged
    // because of it, so this scatters rounded blocks along the banks: they break up the bank line,
    // they interrupt the water's edge, and being spheres they are the one thing in this whole
    // generator that is not a smooth field.
    //
    // Returned as a HEIGHT for the caller to max into the terrain, not as a displacement to add to
    // it. That is what makes them read as objects resting on the ground rather than as bumps in it:
    // a sphere's top is flat in the middle and falls away steeply at the rim, whereas anything
    // ADDED to the surface inherits the surface's own slope and just looks like a swelling of the
    // same material. Resting them on the bed datum rather than on the local ground also means a
    // boulder standing in the channel stands in the water instead of lifting the water with it.
    //
    // One jittered site per cell, scanned over 3x3 so a boulder near a cell edge still reaches into
    // its neighbour.
    float boulderTop(float x, float z, float bedLevel, float amount, uint32_t salt) const {
        if (amount <= 0.01f) return -1e30f;
        const float cell = 34.0f;               // pre-stretch units between candidate sites
        float fx = x / cell, fz = z / cell;
        int ix = int(std::floor(fx)), iz = int(std::floor(fz));
        float best = -1e30f;
        for (int dj = -1; dj <= 1; ++dj) {
            for (int di = -1; di <= 1; ++di) {
                int cx = ix + di, cz = iz + dj;
                uint32_t h = (uint32_t(cx) * 374761393u) ^ (uint32_t(cz) * 668265263u) ^ salt;
                h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
                uint32_t h2 = h * 2246822519u + 2654435761u;
                float r0 = float(h & 0xFFFFu) * (1.0f / 65535.0f);
                float r1 = float((h >> 16) & 0xFFFFu) * (1.0f / 65535.0f);
                float r2 = float(h2 & 0xFFFFu) * (1.0f / 65535.0f);
                float r3 = float((h2 >> 16) & 0xFFFFu) * (1.0f / 65535.0f);
                // Only a fraction of sites carry a boulder, so they clump instead of tiling.
                if (r3 > amount * 0.60f) continue;
                float px = (float(cx) + 0.15f + 0.70f * r0) * cell;
                float pz = (float(cz) + 0.15f + 0.70f * r1) * cell;
                float radius = lerpf(4.5f, 14.0f, r2 * r2);   // squared: many small, few large
                float dx = x - px, dz = z - pz;
                float d2 = dx * dx + dz * dz;
                if (d2 >= radius * radius) continue;
                // Sunk by a third, so they sit IN the ground rather than balancing on it.
                float top = bedLevel - radius * 0.34f + std::sqrt(radius * radius - d2);
                if (top > best) best = top;
            }
        }
        return best;
    }

    float baseTemperature(float x, float z) const {
        float f = TEMP_FREQ;
        float nx = x * f, nz = z * f;
        domainWarp(warpClimate, nx, nz, 0.55f);
        float n = fbm(tempField, nx, nz, 5, 2.1f, 0.5f) * 0.5f + 0.5f;
        float bias = fbm(tempField, x * 0.000002f, z * 0.000002f, 1, 2.0f, 0.5f) * 0.20f;
        return lerpf(0.00f, 1.05f, smoothstep(0.06f, 0.70f, n)) + bias;
    }

    float humidityNoise(float x, float z) const {
        float f = HUMID_FREQ;
        float nx = x * f + 31.4f, nz = z * f + 17.9f;
        domainWarp(warpClimate, nx, nz, 0.60f);
        float n = fbm(humid, nx, nz, 5, 2.1f, 0.5f) * 0.5f + 0.5f;
        return clampf(lerpf(0.0f, 1.0f, smoothstep(0.55f, 0.45f, n)), 0.0f, 1.0f);
    }

    // Takes the already-sampled temp/humid (sampleTerrain computes both once per point already;
    // this used to redundantly recompute both -- 2 extra domain-warped 5-octave fbm's per call --
    // for a value the caller already had).
    float duneGate(float temp, float humid, float x, float z) const {
        float hot = smoothstep(0.60f, 0.86f, temp);
        float dry = 1.0f - smoothstep(0.16f, 0.40f, humid);
        float field = fbm(selN[ARCH_DUNES], x * 0.00012f + 41.0f, z * 0.00012f + 17.0f,
                          2, 2.0f, 0.5f) * 0.5f + 0.5f;
        return hot * dry * smoothstep(0.50f, 0.72f, field);
    }

    float baseMountain(float x, float z, float core) const {
        float fm = lerpf(0.60f, 1.60f,
                         blendModN.noise(x * 0.000020f, z * 0.000020f) * 0.5f + 0.5f);
        float nx = x * MTN_FREQ * fm * 0.55f, nz = z * MTN_FREQ * fm * 0.55f;
        domainWarp(warpN[ARCH_MOUNTAIN], nx, nz, 0.25f);
        float n = fbm(shapeN[ARCH_MOUNTAIN], nx, nz, 3, 2.0f, 0.5f) * 0.5f + 0.5f;
        float t = smoothstep(0.08f, 0.85f, core);
        // Low end raised from 0.40: at the margin of a belt the amplitude term is already small, so
        // collapsing the SHAPE as well squared the fade and left the approach as a featureless
        // apron that then jumped straight into the range. Keeping more shape out here gives the
        // foothills their own ridges and valleys to climb through.
        return clampf(n * lerpf(0.62f, 1.20f, t), 0.0f, 1.0f);
    }

    float baseBadlands(float x, float z) const {
        float nx = x * BAD_FREQ, nz = z * BAD_FREQ;
        domainWarp(warpN[ARCH_BADLANDS], nx, nz, 0.30f);
        float n = fbm(shapeN[ARCH_BADLANDS], nx, nz, 2, 2.1f, 0.5f) * 0.5f + 0.5f;
        return n * n * (3.0f - 2.0f * n);
    }

    // Formerly inline in shape()'s non-eroded switch; pulled out so erodedShapeAt (via baseShape)
    // can reach them now that plains/downs get their own (gentle) erosion pass too.
    float basePlains(float x, float z) const {
        float nx = x * PLN_FREQ, nz = z * PLN_FREQ;
        domainWarp(warpN[ARCH_PLAINS], nx, nz, 0.08f);
        float n = fbm(shapeN[ARCH_PLAINS], nx, nz, 4, 2.1f, 0.45f) * 0.5f + 0.5f;
        return n * n * (3.0f - 2.0f * n);
    }

    float baseDowns(float x, float z) const {
        float nx = x * DOWN_FREQ, nz = z * DOWN_FREQ;
        domainWarp(warpN[ARCH_DOWNS], nx, nz, 0.12f);
        float n = fbm(shapeN[ARCH_DOWNS], nx, nz, 3, 2.0f, 0.5f) * 0.5f + 0.5f;
        return n * n * (3.0f - 2.0f * n);
    }

    float basePlateau(float x, float z) const {
        float nx = x * PLAT_FREQ, nz = z * PLAT_FREQ;
        domainWarp(warpN[ARCH_PLATEAU], nx, nz, 0.25f);
        float base = fbm(shapeN[ARCH_PLATEAU], nx, nz, 3, 2.0f, 0.5f) * 0.5f + 0.5f;
        float tilt = blendModN.noise(x * PLAT_TILT_FREQ, z * PLAT_TILT_FREQ)
                   + blendModN.noise(x * PLAT_TILT_FREQ + 5.1f, z * PLAT_TILT_FREQ + 7.3f);
        tilt *= 0.035f;
        base = base * base * (3.0f - 2.0f * base);
        const float tiers = 4.0f;
        float t = (base + tilt) * tiers;
        float lvl = std::floor(t);
        float riser = smoothstep(0.55f, 0.85f, t - lvl);
        return clampf((lvl + riser) / tiers, 0.0f, 1.0f);
    }

    float baseJagged(float x, float z, float core) const {
        float nx = x * JAG_FREQ * 0.55f, nz = z * JAG_FREQ * 0.55f;
        domainWarp(warpN[ARCH_JAGGED], nx, nz, 0.20f);
        float n = shapeN[ARCH_JAGGED].noise(nx, nz) * 0.5f + 0.5f;
        float pillar = clampf((n - 0.40f) * 2.0f, 0.0f, 1.0f);
        pillar = std::pow(pillar, 0.30f);
        float t = smoothstep(0.08f, 0.85f, core);
        return clampf(pillar * lerpf(0.15f, 1.20f, t), 0.0f, 1.0f);
    }

    // Mesa and butte country: flat tables standing over a flat pan, separated by cliffs.
    //
    // The difference from basePlateau, which also terraces, is entirely in the riser width. Plateau
    // ramps between tiers over 30% of a tier (smoothstep 0.55..0.85), which at its amplitude is a
    // steep hillside. Here the riser spans 4% of a tier, so the rise between two treads happens over
    // a few tens of world units and the face is genuinely vertical at voxel resolution -- that step
    // is the whole landform, and softening it at all turns mesa country back into rolling plateau.
    //
    // Two further departures from plateau:
    //
    //   A `stand` mask, at a third of the terracing frequency, decides where a table exists at all.
    //   Without it every tier boundary is drawn everywhere and the result is a continuous staircase
    //   landscape; with it, tiers only rise inside the stands and the ground between them stays a
    //   flat pan, which is what makes a butte an isolated object.
    //
    //   The tread is pushed toward the top of its tier (the pow below), so a table's surface is
    //   dead flat rather than gently domed. Real cap rock is a resistant layer: it is flat because
    //   it is the layer, not because erosion happened to level it.
    float baseMesa(float x, float z) const {
        float nx = x * MESA_FREQ, nz = z * MESA_FREQ;
        domainWarp(warpN[ARCH_MESA], nx, nz, 0.38f);

        float stand = fbm(shapeN[ARCH_MESA], nx * 0.34f + 7.1f, nz * 0.34f + 2.9f, 3, 2.0f, 0.5f)
                    * 0.5f + 0.5f;
        stand = smoothstep(0.40f, 0.62f, stand);

        float n = fbm(shapeN[ARCH_MESA], nx, nz, 3, 2.15f, 0.48f) * 0.5f + 0.5f;

        const float tiers = 6.0f;
        float t = clampf(n, 0.0f, 1.0f) * tiers;
        float lvl = std::floor(t);
        float f = t - lvl;
        float riser = smoothstep(0.478f, 0.522f, f);
        float table = clampf((lvl + riser) / tiers, 0.0f, 1.0f);

        // Flatten the treads: pow > 1 on the fractional part within the stepped field would undo the
        // step, so instead the whole terraced field is pulled toward its own tier tops.
        float pan = n * 0.22f;
        return clampf(lerpf(pan, table, stand), 0.0f, 1.0f);
    }

    float baseShape(int a, float x, float z, float core) const {
        switch (a) {
            case ARCH_MOUNTAIN: return baseMountain(x, z, core);
            case ARCH_BADLANDS: return baseBadlands(x, z);
            case ARCH_PLATEAU:  return basePlateau(x, z);
            case ARCH_JAGGED:   return baseJagged(x, z, core);
            case ARCH_PLAINS:   return basePlains(x, z);
            case ARCH_DOWNS:    return baseDowns(x, z);
            case ARCH_MESA:     return baseMesa(x, z);
            default:            return 0.0f;
        }
    }

    float erodedShapeAt(int a, float x, float z, float core, int oct,
                        float* ridgeMapOut, float eroMul = 1.0f) const {
        const float bf = (a == ARCH_MOUNTAIN) ? MTN_FREQ
                       : (a == ARCH_BADLANDS) ? BAD_FREQ
                       : (a == ARCH_JAGGED)   ? JAG_FREQ
                       : (a == ARCH_PLAINS)   ? PLN_FREQ
                       : (a == ARCH_DOWNS)    ? DOWN_FREQ
                       : (a == ARCH_MESA)     ? MESA_FREQ : PLAT_FREQ;
        float h0 = baseShape(a, x, z, core);
        if (oct <= 0) { if (ridgeMapOut) *ridgeMapOut = 1.0f; return h0; }

        const float eW = 1.5f;
        float hL = baseShape(a, x - eW, z, core), hR = baseShape(a, x + eW, z, core);
        float hD = baseShape(a, x, z - eW, core), hU = baseShape(a, x, z + eW, core);
        vec2 slope{(hR - hL) / (2.0f * eW * bf), (hU - hD) / (2.0f * eW * bf)};

        float fadeTarget = clampf((h0 - 0.42f) / 0.34f, -1.0f, 1.0f);
        float rm = 0.0f;
        float delta = erosionFilter(vec2{x * bf, z * bf}, h0, slope, fadeTarget,
                                    ARCH_EROSION[a], oct, erosionSeed + uint32_t(a) * 101u, rm);
        if (ridgeMapOut) *ridgeMapOut = rm;
        return clampf(h0 + delta * eroMul, 0.0f, 1.2f);
    }

    float shape(int a, float x, float z, float core, int erosionOct,
                float* ridgeMapOut = nullptr, float eroMul = 1.0f) const {
        if (archEroded(a))
            return erodedShapeAt(a, x, z, core, erosionOct, ridgeMapOut, eroMul);

        switch (a) {
            case ARCH_PLAINS: {
                float nx = x * PLN_FREQ, nz = z * PLN_FREQ;
                domainWarp(warpN[a], nx, nz, 0.08f);
                float n = fbm(shapeN[a], nx, nz, 4, 2.1f, 0.45f) * 0.5f + 0.5f;
                return n * n * (3.0f - 2.0f * n);
            }
            case ARCH_DUNES: {
                float wa = fbm(warpN[a], x * 0.000035f, z * 0.000035f, 2, 2.0f, 0.5f) * 3.14159265f;
                float cw = std::cos(wa), sw = std::sin(wa);
                float u = x * cw + z * sw;
                float v = -x * sw + z * cw;
                u += fbm(shapeN[a], x * 0.0006f, z * 0.0006f, 2, 2.0f, 0.5f) * 55.0f;
                float ridge = 0.5f - 0.5f * std::cos(u * (kTau / 210.0f));
                ridge = std::pow(clampf(ridge, 0.0f, 1.0f), 1.3f);
                float seg = fbm(shapeN[a], u * 0.004f + 3.1f, v * 0.011f, 2, 2.0f, 0.5f) * 0.5f + 0.5f;
                seg = smoothstep(0.18f, 0.72f, seg);
                return clampf(ridge * seg, 0.0f, 1.0f);
            }
            default: {
                float nx = x * DOWN_FREQ, nz = z * DOWN_FREQ;
                domainWarp(warpN[a], nx, nz, 0.12f);
                float n = fbm(shapeN[a], nx, nz, 3, 2.0f, 0.5f) * 0.5f + 0.5f;
                return n * n * (3.0f - 2.0f * n);
            }
        }
    }

    float blendFreqMod(int a, float x, float z) const {
        float n = blendModN.noise(x * BLEND_MOD_FREQ + a * 1.73f,
                                  z * BLEND_MOD_FREQ + a * 3.14f);
        return lerpf(0.55f, 3.00f, n * 0.5f + 0.5f);
    }

    float claim(int a, float x, float z, float regionSize,
                float temp = -1.0f, float humid = -1.0f, float uplift = 0.0f) const {
        float mod = blendFreqMod(a, x, z);
        float f = ARCH_SEL_FREQ[a] * mod / std::max(regionSize, 0.01f);
        float n = selN[a].noise(x * f, z * f) * 0.5f + 0.5f;

        float bm = blendModN.noise(x * BLEND_MOD_FREQ + a * 5.93f,
                                   z * BLEND_MOD_FREQ + a * 7.31f);
        n = clampf(n + bm * 0.12f, 0.0f, 1.0f);

        if (a != ARCH_MOUNTAIN && a != ARCH_JAGGED && temp >= 0.0f && humid >= 0.0f) {
            const float* c = ARCH_CLIMATE[a];
            float dt = (temp - c[0]) / c[1];
            float dh = (humid - c[2]) / c[3];
            float aff = std::exp(-2.0f * (dt * dt + dh * dh));
            n *= lerpf(0.55f, 1.35f, aff);
        }

        if (a == ARCH_MOUNTAIN || a == ARCH_JAGGED) {
            // Tectonics decides where mountains are; the selection noise only decides which parts
            // of a belt are massif and which are pass. Multiplying rather than adding is what
            // clears the interiors: away from a belt the claim goes to zero however high the noise
            // happens to be there, so mountains stop turning up as isolated lumps in the middle of
            // plains and start forming a continuous cordillera you can follow.
            n = clampf(n * lerpf(0.28f, 1.0f, uplift) + uplift * 0.34f, 0.0f, 1.0f);
            // Widened slightly from the original 0.28-0.94 to soften the edge of the mountain
            // belt: the four squarings (pow^2 in weightFromClaim, DOMINANCE=2, smoothstep itself)
            // still compound, but a wider smoothstep range means the ramp rises over more distance
            // and the wall does not hit all at once at the belt boundary.
            //
            // The fractional power on top is what actually produces an approach you can walk up.
            // Widening the smoothstep was not enough on its own because of what this claim feeds:
            // weightFromClaim squares it, the dominance pass raises that to 1.15, and the resulting
            // weight then selects between archetype amplitudes that differ by twenty times (plains
            // 200, mountain 4000). So terrain height rises as roughly claim^2.3, and a smoothstep --
            // flat at both ends by construction -- puts nearly all of that rise into a narrow band:
            // a 3,800-unit amplitude change arriving over a few hundred metres is the "harsh cliff".
            //
            // An exponent near 1/2.3 cancels most of that compounding, so amplitude climbs closer to
            // linearly with distance into the belt and the range gains real foothills. As with
            // upliftBelt's pow, the endpoints are fixed, so this widens no belt and moves no
            // mountain -- it only changes how the ground gets there.
            return std::pow(smoothstep(0.20f, 0.90f, n), 0.55f);
        }
        return smoothstep(0.18f, 0.82f, n);
    }

    static float weightFromClaim(float c, float amount) {
        return (std::pow(c, 2.0f) + 0.004f) * amount;
    }

    float detailFn(float x, float z, int octaves) const {
        if (octaves <= 0) return 0.0f;
        float f = fbm(detailN, x * DETAIL_FREQ, z * DETAIL_FREQ, octaves, 2.1f, 0.5f);
        if (octaves < 3) return f;
        float g = worleyGullies(gully, x * DETAIL_GULLY_FREQ, z * DETAIL_GULLY_FREQ,
                                2, 2.1f, 0.5f);
        return f - g * 1.20f;
    }

    float microFactor(float x, float z) const {
        float n = fbm(microN, x * 0.0005f, z * 0.0005f, 2, 2.0f, 0.5f);
        return lerpf(0.65f, 1.60f, n * 0.5f + 0.5f);
    }
};

// Reduced LOD-level structure  blends the terrain at a single (x,z) point
// and returns the final height. Parameters match terrain.cpp's blendTerrain
// but with erosion always enabled (highest detail), since we generate once.

static const float WEIGHT_GATE = 0.010f;

struct BlendParams {
    // Mesa outweighs its neighbours because its own climate window is narrow (see ARCH_CLIMATE): it
    // only competes at all in hot arid country, and there it should win outright rather than average
    // with badlands into a rounded compromise that has neither's silhouette.
    float amount[ARCH_COUNT] = {1.0f, 1.0f, 1.0f, 1.0f, 1.5f, 1.0f, 0.0f, 1.35f};
    float regionSize = 1.0f;
    float climateScale[2] = {2.0f, 2.0f};
};

struct TerrainSample {
    float height = 0;
    float w[ARCH_COUNT] = {};
    float temp = 0.5f;
    float humid = 0.5f;

    // 0 offshore .. 1 continental interior.
    float land = 0.0f;
    // 0 stable craton .. 1 the axis of a mountain belt.
    float uplift = 0.0f;
    // World height the water surface sits at for this column. SEA_LEVEL everywhere except in a
    // river valley, where it is that reach's pool level -- which is what lets water exist above sea
    // level at all instead of only in one global plane.
    float waterTop = SEA_LEVEL;
    // 0 outside .. 1 on the river axis. Drives bank material, and suppresses trees in the channel.
    float river = 0.0f;
};

// detailOct/erosionOct let the caller trade detail for speed per LOD ring: full octaves for
// near/LOD0 chunks (where the player actually sees the texture), fewer for the far LOD1/LOD2 rings
// that make up the bulk of resident chunks by count but are viewed from a distance where the extra
// octaves are imperceptible. Defaults match the original hardcoded oct=2/eoct=4 so any other caller
// is unaffected.
// The land surface at LANDFORM scale: the archetype blend and the continental crust under it, with
// no per-voxel detail and with the erosion pass under the caller's control.
//
// Split out of sampleTerrain so the river code can evaluate the same surface a second time, at the
// channel centreline, with erosion turned off -- see riverDatum. It is one function rather than two
// on purpose: a river's pool level is only sane if the datum it is cut relative to is the terrain,
// and a separately-written approximation of "roughly the terrain" is exactly what drifts.
struct MacroSample {
    float height = 0.0f;   // land surface before detail
    float w[ARCH_COUNT] = {};
    float temp = 0.5f, humid = 0.5f;
    float land = 0.0f, uplift = 0.0f;
    float amp = 0.0f, rough = 0.0f;
};

static MacroSample sampleMacro(const Generator& g, const BlendParams& bp,
                               float x, float z, int erosionOct) {
    MacroSample out;
    float claim[ARCH_COUNT];
    float total = 0.0f;
    out.temp = g.baseTemperature(x, z);
    out.humid = g.humidityNoise(x, z);

    // The two fields above landform scale, resolved first because everything below depends on them:
    // continents decide whether there is land here and how high its crust sits, uplift decides
    // whether that land is a mountain belt.
    float cf = g.continentField(x, z);
    out.land = Generator::landMask(cf);
    float crust = Generator::continentBase(cf);
    out.uplift = g.upliftBelt(x, z) * out.land;

    for (int a = 0; a < ARCH_COUNT; ++a) {
        claim[a] = g.claim(a, x, z, bp.regionSize, out.temp, out.humid, out.uplift);
        out.w[a] = Generator::weightFromClaim(claim[a], bp.amount[a]);
        if (a == ARCH_DUNES && out.w[a] > 1e-4f)
            out.w[a] *= g.duneGate(out.temp, out.humid, x, z);
        total += out.w[a];
    }
    if (total < 1e-6f) {
        for (int a = 0; a < ARCH_COUNT; ++a) out.w[a] = (a == ARCH_PLAINS) ? 1.0f : 0.0f;
    } else {
        for (int a = 0; a < ARCH_COUNT; ++a) out.w[a] /= total;
    }

    const float DOMINANCE = 1.15f;
    float sharp = 0.0f;
    for (int a = 0; a < ARCH_COUNT; ++a) {
        out.w[a] = std::pow(out.w[a], DOMINANCE);
        sharp += out.w[a];
    }
    for (int a = 0; a < ARCH_COUNT; ++a) out.w[a] /= sharp;

    float eroMul = 1.0f;
    {
        float dry = 1.0f - clampf(out.humid, 0.0f, 1.0f);
        float cold = 1.0f - clampf(out.temp, 0.0f, 1.0f);
        eroMul = lerpf(0.65f, 1.35f, dry) * lerpf(0.80f, 1.20f, cold);
    }

    float shapeSum = 0.0f, usedW = 0.0f;
    for (int a = 0; a < ARCH_COUNT; ++a) {
        out.amp += out.w[a] * ARCH_AMP[a];
        out.rough += out.w[a] * ARCH_ROUGH[a];
        if (out.w[a] < WEIGHT_GATE) continue;
        shapeSum += out.w[a] * g.shape(a, x, z, claim[a], erosionOct, nullptr, eroMul);
        usedW += out.w[a];
    }
    float shape01 = usedW > 1e-6f ? shapeSum / usedW : 0.0f;

    // Landform relief is a property of continents: offshore it collapses almost to nothing so the
    // sea floor stays a sea floor instead of growing its own submarine ranges that then poke back
    // through the water as spurious islands. The onset is a little earlier than landMask's so
    // coastal ranges still reach the shore.
    float ampScale = lerpf(0.15f, 1.0f, smoothstep(0.380f, 0.620f, cf));

    out.height = shape01 * out.amp * g.reliefFactor(x, z, out.uplift) * ampScale + crust;
    return out;
}

// The datum a river valley is cut relative to, and the surface its pools descend along.
//
// This is the real terrain, evaluated at the channel centreline with the erosion pass and the
// per-voxel detail switched off -- a genuine low-pass of the ground, not a stand-in for it.
//
// It started as a stand-in: continental base plus one low-frequency swell, on the reasoning that a
// datum only has to be smooth, fall toward the sea, and stay in the neighbourhood of the ground.
// The first two held; the third did not, and it is the one that matters. Because the landform layer
// was missing from it, the datum could sit a couple of hundred units off the real surface, and a
// pool placed relative to it then stood above ground that was never carved. Measured over three
// thousand river cells, 31% had their water surface standing more than eight units above dry ground
// a hundred units away -- a third of every river in the world was a perched ribbon with a wall of
// water down one side. Paying for a second archetype blend is worth not having that.
//
// It is affordable because it runs only inside a valley (a few percent of columns) and because
// erosion, which is by far the most expensive part of a terrain sample, is exactly what is left
// out: what remains is the noise-and-blend layer.
static float riverDatum(const Generator& g, const BlendParams& bp, float x, float z) {
    return sampleMacro(g, bp, x, z, /*erosionOct=*/0).height;
}

static TerrainSample sampleTerrain(const Generator& g, const BlendParams& bp,
                                   float x, float z, int detailOct = 2, int erosionOct = 4) {
    MacroSample m = sampleMacro(g, bp, x, z, erosionOct);

    TerrainSample out;
    for (int a = 0; a < ARCH_COUNT; ++a) out.w[a] = m.w[a];
    out.temp = m.temp;
    out.humid = m.humid;
    out.land = m.land;
    out.uplift = m.uplift;

    const int oct = detailOct;
    const float amp = m.amp;
    const float macro = m.height;
    float micro = g.microFactor(x, z);

    out.height = macro
               + g.detailFn(x, z, oct) * (8.0f + 0.020f * amp) * m.rough * micro
                 * lerpf(0.25f, 1.0f, out.land);

    // Lapse rate: it gets colder as you go up.
    //
    // Temperature was a purely horizontal field, which was survivable when the tallest thing in the
    // world was 2000 units but is not now. Without this, a 5000-unit peak in a temperate belt is the
    // same temperature as the valley floor beside it, so it gets no snow cap, no bare frost-shattered
    // rock and no tree line -- and a mountain range with meadow on top does not read as towering
    // however tall it actually is. Snow and the tree line are most of what communicates the scale.
    //
    // Applied AFTER the archetype blend, deliberately: `claim` consults temperature to decide which
    // landforms belong here, so feeding it a height-dependent temperature would make selection
    // depend on the height that selection produces. Everything downstream of the blend -- surface
    // colour, rock type, tree stocking -- gets the lapsed value; only the choice of landform sees
    // the sea-level one.
    float altT = clampf((out.height - SEA_LEVEL) / MAX_HEIGHT, 0.0f, 1.0f);
    out.temp = clampf(out.temp - 0.68f * altT, 0.0f, 1.05f);

    // ------------------------------------------------------------------ rivers ----
    Generator::River rv = g.riverField(x, z);
    if (rv.valley > 0.001f) {
        float datum = riverDatum(g, bp, rv.center.x, rv.center.y);
        // Measured at the CENTRELINE, like the datum itself, and for the same reason. Everything
        // this feeds -- the valley depth, the channel depth, and so the pool level -- has to be a
        // property of the reach rather than of the column, or the pool tilts across its own width:
        // taking it from the local height made a 500-unit-wide channel's water surface vary by 22
        // units, as much as the river was deep.
        float altN = clampf((datum - SEA_LEVEL) / MAX_HEIGHT, 0.0f, 1.0f);

        // A river grows downstream. Altitude is the only proxy for "downstream" available to a
        // purely local sample, but it is a good one: low ground is where the catchment has already
        // collected, so channels widen and deepen as they approach the sea.
        //
        // Depth scales with generation, but nothing like proportionally. A trunk pool sitting a
        // full 4x deeper than the tributary joining it puts a hundred-unit step in the water
        // surface at every confluence, because the two are independent level sets and a column
        // belongs to exactly one of them -- the step is real, not a seam, and it is much too big.
        float maturity = 1.0f - smoothstep(0.015f, 0.30f, altN);
        float byOrder = lerpf(0.45f, 1.0f, rv.order);
        float valleyDepth = lerpf(32.0f, 115.0f, maturity) * byOrder;
        float channelDepth = lerpf(7.0f, 27.0f, maturity) * byOrder;

        // The banks. A channel placed by a noise field takes no notice of the ground it crosses, so
        // it will happily run along a shoulder or over the lip of a cliff, and a pool set from the
        // centreline datum then stands in mid-air with a wall of water down the low side. Both banks
        // are therefore sampled, at the top of the flare where the carve has faded out and the
        // ground is untouched, and the water is not allowed above the lower of them.
        //
        // Clamping rather than merely testing is the important part. Every gate here is a smooth
        // fade, and a fade cannot decide a shoreline: whichever threshold the water switches off at,
        // the ground goes on descending past it, so the water ends in a step wherever the GATE ran
        // out rather than wherever the ground came up to meet it. Clamping the pool under the banks
        // makes containment structural -- the water then ends where the terrain rises through it,
        // which is what a shoreline is -- and leaves the gates free to do the job they are actually
        // suited to, which is deciding whether a reach exists at all.
        vec2 bankA{rv.center.x + rv.normal.x * rv.flare, rv.center.y + rv.normal.y * rv.flare};
        vec2 bankB{rv.center.x - rv.normal.x * rv.flare, rv.center.y - rv.normal.y * rv.flare};
        float lowBank = std::min(riverDatum(g, bp, bankA.x, bankA.y),
                                 riverDatum(g, bp, bankB.x, bankB.y));

        float wanted = datum - valleyDepth + channelDepth;   // pool the reach's own datum asks for
        float pool = std::min(wanted, lowBank - 10.0f);
        float bed  = pool - channelDepth;

        // Where a river can be at all. All four are properties of the REACH, computed from
        // centreline quantities, so they vary along a river and not across it -- see the note above
        // on why a gate must not be what ends the water.
        //   land/sea  -- no channels offshore, and none cut below the waterline where the sea
        //                already is; the coast is where a river ends.
        //   humidity  -- true desert gets dry washes (the carve) but no water in them.
        //   alpine    -- channels thin out and stop near the summits.
        //   banks     -- how far the bank clamp above had to drag the pool below what the reach
        //                wanted, which is a direct measure of the ground's cross-fall: on level
        //                ground both banks stand a full valley-depth above the pool and this is
        //                zero, while on a hillside the downhill bank drops toward and then below
        //                it. Only extreme cross-falls kill the reach outright now -- see
        //                `contained` below, which is the same measurement used for a much better
        //                purpose.
        //   gorge     -- an upper bound on how deep a valley may cut. A canyon is welcome; a
        //                kilometre-deep slot through a summit ridge is not.
        float banks = 1.0f - smoothstep(60.0f, 220.0f, wanted - pool);
        float gorge = 1.0f - smoothstep(420.0f, 1100.0f, macro - datum);
        float aboveSea = smoothstep(SEA_LEVEL - 10.0f, SEA_LEVEL + 55.0f, datum);
        float alpine   = 1.0f - smoothstep(0.50f, 0.88f, altN);

        float carveFlow = out.land * aboveSea * alpine * banks * gorge;
        // The finest generation carves but never fills. Its channel is about 19 voxels across and
        // only a handful deep, which is not enough trough to hold a level surface -- any bump in the
        // bed leaves a step of water standing over dry ground, and measured at voxel scale that
        // happened on 4% of its cells against well under 1% for the two coarser generations. A dry
        // headwater gully feeding a river is both the cheap answer and the right-looking one.
        float waterFlow = carveFlow * smoothstep(0.17f, 0.42f, out.humid)
                                    * smoothstep(0.30f, 0.48f, rv.order);

        float t  = clampf(rv.valley * carveFlow, 0.0f, 1.0f);
        float tc = clampf(rv.channel * carveFlow, 0.0f, 1.0f);
        // Two blends, not one: the outer valley eases the ground down toward the bed, and the
        // channel then pins it flat AT the bed. Without the second the detail term keeps roughing
        // up the river floor, and a channel with hillocks in it does not hold a level water surface.
        float target = lerpf(out.height, bed, t);
        target = lerpf(target, bed, tc);
        out.height = std::min(out.height, target);

        // Boulders, added after the carve so the channel does not flatten them away again. Only at
        // the near LOD: at 45 world units per voxel the outer ring cannot resolve a 10-unit rock,
        // and letting it try would turn them into single-voxel speckle along every river on the
        // horizon. detailFn is already octave-gated the same way, so the rings already disagree
        // slightly about height by design.
        if (detailOct >= 3 && rv.bankiness > 0.02f) {
            float amount = rv.bankiness * carveFlow;
            float top = g.boulderTop(x, z, bed + channelDepth * 0.35f, amount,
                                     g.erosionSeed ^ 0x5EEDB01Fu);
            if (top > out.height) out.height = top;
        }

        out.river = t;

        // A river has two regimes, and treating it as only the first is what broke it.
        //
        // A level pool needs banks that can hold one. Where the channel runs down a slope they
        // cannot: the pool gets clamped under the falling bank, the ground the carve leaves behind
        // rises through it, and the reach simply has no water. Walking a centreline downstream, that
        // showed up as a river in pieces -- 23 wet samples, 38 dry, gaps of 2,300 units -- water
        // stopping at a drop and reappearing on the flat below it. Which is exactly right about the
        // hydrology and exactly wrong about the rendering, because what belongs in the gap is a
        // waterfall.
        //
        // So the steep case gets its own regime: a thin sheet riding the carved bed instead of a
        // level surface. It is continuous by construction, because it is defined relative to the
        // ground rather than to a datum the ground has climbed away from, and it cannot perch --
        // it is never more than `sheet` above the ground it sits on, which makes it strictly safer
        // than the pool it replaces. Where the bed then plunges, the fill runs from the bed up to
        // the surface and the sheet becomes a genuinely tall column of water: the fall itself.
        //
        // `contained` is the strict bank test that used to kill these reaches outright. It now
        // chooses between the two regimes instead, which is a far better use of the same number.
        //
        // Water is offered over the CHANNEL, not over the whole valley, and the difference between
        // those two is most of whether rivers work at all.
        //
        // A trunk valley is about 1800 units across but only a hundred deep. Terrain varies by far
        // more than a hundred units over eighteen hundred, so a level pool spanning the whole valley
        // cannot be contained by it: wherever the valley floor happens to run low, the pool either
        // floods it into a lake or -- if that is gated off -- ends in a wall of water standing over
        // dry ground. Measured across a range of gate settings this stayed stubbornly at a third of
        // all river cells, and no amount of gating moved it, because the gates were not the problem:
        // a shallow wide trough is simply the wrong shape to hold still water.
        //
        // The channel is a tenth as wide and is the part the carve pins flat AT the bed, so the
        // water sits in a trough it actually fits, and the valley reverts to being what a valley is
        // -- the shaped ground the river runs through, not part of the river.
        if (waterFlow > 0.30f && tc > 0.05f) {
            float sheet = lerpf(2.5f, 7.0f, maturity) * byOrder;
            float surface = out.height + sheet;
            float contained = 1.0f - smoothstep(0.0f, 40.0f, wanted - pool);
            if (contained > 0.5f && pool > surface) surface = pool;
            out.waterTop = std::max(out.waterTop, surface);
        }
    }

    // Never emit a column the chunk grid has no room for -- see TERRAIN_CEILING.
    out.height = std::min(out.height, TERRAIN_CEILING - 60.0f);
    return out;
}

} // namespace terrain_noise
