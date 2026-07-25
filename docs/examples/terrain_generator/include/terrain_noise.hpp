// ProjectV Terrain Generator - Advanced Noise System
// Ported from ~/Development/Development_Synced/TerrainTest/terrain.cpp
// Streaming layered terrain generation with:
//   - Perlin fBm, smooth ridged multifractal, and layered domain warping
//   - 7 terrain maps (mountains / plains / badlands / dunes / plateau / downs / jagged)
//     blended by low-frequency weights
//   - Large-scale relief field varies amplitude across the world
//   - Climate maps (temperature / humidity / slope) drive per-vertex color
//   - Erosion filter (Phacelle noise) for drainage networks
//   - Worley cell noise for gullies and rock texture

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
    ARCH_DUNES, ARCH_PLATEAU, ARCH_DOWNS, ARCH_JAGGED, ARCH_COUNT
};

static const float ARCH_AMP[ARCH_COUNT] = {
    2000.0f, 200.0f, 650.0f, 150.0f, 1500.0f, 650.0f, 1600.0f};

static const float ARCH_SEL_FREQ[ARCH_COUNT] = {
    0.000038f, 0.000065f, 0.000092f, 0.000110f, 0.000050f, 0.000078f, 0.000052f};

static const float ARCH_ROUGH[ARCH_COUNT] = {
    0.32f, 0.10f, 0.34f, 0.14f, 0.30f, 0.28f, 0.26f};

static const float ARCH_CLIMATE[ARCH_COUNT][4] = {
    {0.30f, 0.35f, 0.48f, 0.45f},
    {0.55f, 0.30f, 0.50f, 0.30f},
    {0.85f, 0.20f, 0.18f, 0.22f},
    {0.90f, 0.16f, 0.10f, 0.18f},
    {0.55f, 0.25f, 0.32f, 0.28f},
    {0.30f, 0.30f, 0.72f, 0.28f},
    {0.35f, 0.38f, 0.42f, 0.48f},
};

// Plains and downs used to be the only archetypes with NO erosion pass at all (flat {} == strength
// 0 == archEroded() false), so those regions never got the gully/drainage texture that
// mountains/badlands/plateau/jagged already have. Plains gets a very light touch (it should still
// read as mostly smooth), downs a bit more (rolling hills read well with soft drainage lines).
// Dunes stays erosion-free: wind-formed ridges, not water drainage, are the whole point of its own
// shape function.
static const ErosionParams ARCH_EROSION[ARCH_COUNT] = {
    {0.15f, 0.52f, 0.70f, 1.4f, {0.22f,0.10f,0.10f,2.0f}, {0.58f,1.10f,2.80f,1.5f}, {0.72f,1.0f}, 0.85f, 0.42f, 2.0f, 0.52f},
    {0.22f, 0.16f, 0.28f, 1.1f, {0.24f,0.12f,0.10f,2.0f}, {0.48f,1.05f,2.80f,1.2f}, {0.80f,1.0f}, 0.92f, 0.42f, 2.0f, 0.52f},
    {0.09f, 0.40f, 0.45f, 2.2f, {0.10f,0.04f,0.10f,2.0f}, {0.55f,1.40f,2.80f,1.7f}, {0.78f,1.0f}, 0.75f, 0.45f, 2.0f, 0.52f},
    {},
    {0.12f, 0.55f, 0.90f, 1.2f, {0.18f,0.09f,0.10f,2.0f}, {0.38f,1.15f,2.80f,1.4f}, {0.66f,1.0f}, 0.80f, 0.40f, 2.0f, 0.52f},
    {0.16f, 0.28f, 0.42f, 1.3f, {0.18f,0.08f,0.10f,2.0f}, {0.42f,1.10f,2.80f,1.3f}, {0.72f,1.0f}, 0.85f, 0.42f, 2.0f, 0.52f},
    {0.15f, 0.35f, 0.35f, 1.2f, {0.18f,0.06f,0.10f,2.0f}, {0.35f,1.10f,2.80f,1.3f}, {0.70f,1.0f}, 0.85f, 0.42f, 2.0f, 0.52f},
};
static bool archEroded(int a) { return ARCH_EROSION[a].strength > 0.0f; }

static const float MTN_FREQ  = 0.00016f;
static const float PLN_FREQ  = 0.00060f;
static const float BAD_FREQ  = 0.00040f;
static const float PLAT_FREQ = 0.00018f;
static const float PLAT_TILT_FREQ = 0.000012f;
static const float DOWN_FREQ = 0.00030f;
static const float JAG_FREQ = 0.00035f;
static const float RELIEF_FREQ = 0.000040f;
static const float HUMID_FREQ = 0.000015f;
static const float SEA_LEVEL = 0.0f;
static const float CONT_FREQ = 0.000022f;
static const float BLEND_MOD_FREQ = 0.000030f;
static const float TEMP_FREQ = 0.000008f;
static const float DETAIL_FREQ = 0.0022f;
static const float DETAIL_GULLY_FREQ = 0.0035f;
static const float MAX_HEIGHT = ARCH_AMP[ARCH_MOUNTAIN];

struct Generator {
    Perlin shapeN[ARCH_COUNT] = {Perlin(0), Perlin(0), Perlin(0),
                                 Perlin(0), Perlin(0), Perlin(0), Perlin(0)};
    Perlin selN[ARCH_COUNT]   = {Perlin(0), Perlin(0), Perlin(0),
                                 Perlin(0), Perlin(0), Perlin(0), Perlin(0)};
    Perlin warpN[ARCH_COUNT]  = {Perlin(0), Perlin(0), Perlin(0),
                                 Perlin(0), Perlin(0), Perlin(0), Perlin(0)};
    Perlin relief{0}, cont{0}, tempField{0}, humid{0};
    Perlin warpClimate{0}, patchA{0}, patchB{0}, detailN{0}, blendModN{0}, microN{0};
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
        gully.reseed(seed * 3266489917u + 77u);
        rockNoise.reseed(seed * 3266489917u + 12345u);
    }

    float reliefFactor(float x, float z) const {
        float n = fbm(relief, x * RELIEF_FREQ, z * RELIEF_FREQ, 3, 2.0f, 0.5f) * 0.5f + 0.5f;
        return lerpf(0.15f, 2.20f, smoothstep(0.15f, 0.85f, n));
    }

    float baseElevation(float x, float z) const {
        float n = fbm(cont, x * CONT_FREQ, z * CONT_FREQ, 4, 2.0f, 0.5f) * 0.5f + 0.5f;
        return lerpf(-700.0f, 260.0f, smoothstep(0.20f, 0.72f, n));
    }

    float baseTemperature(float x, float z) const {
        float f = TEMP_FREQ;
        float nx = x * f, nz = z * f;
        domainWarp(warpClimate, nx, nz, 0.55f);
        float n = fbm(tempField, nx, nz, 5, 2.1f, 0.5f) * 0.5f + 0.5f;
        return lerpf(0.00f, 1.05f, smoothstep(0.06f, 0.90f, n));
    }

    float humidityNoise(float x, float z) const {
        float f = HUMID_FREQ;
        float nx = x * f + 31.4f, nz = z * f + 17.9f;
        domainWarp(warpClimate, nx, nz, 0.60f);
        return clampf(fbm(humid, nx, nz, 5, 2.1f, 0.5f) * 0.5f + 0.5f, 0.0f, 1.0f);
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
        float nx = x * MTN_FREQ * fm, nz = z * MTN_FREQ * fm;
        domainWarp(warpN[ARCH_MOUNTAIN], nx, nz, 0.25f);
        float n = fbm(shapeN[ARCH_MOUNTAIN], nx, nz, 3, 2.0f, 0.5f) * 0.5f + 0.5f;
        float t = smoothstep(0.08f, 0.85f, core);
        return clampf(n * lerpf(0.40f, 1.20f, t), 0.0f, 1.0f);
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
        float nx = x * JAG_FREQ, nz = z * JAG_FREQ;
        domainWarp(warpN[ARCH_JAGGED], nx, nz, 0.20f);
        float n = shapeN[ARCH_JAGGED].noise(nx, nz) * 0.5f + 0.5f;
        float pillar = clampf((n - 0.40f) * 2.0f, 0.0f, 1.0f);
        pillar = std::pow(pillar, 0.30f);
        float t = smoothstep(0.08f, 0.85f, core);
        return clampf(pillar * lerpf(0.15f, 1.20f, t), 0.0f, 1.0f);
    }

    float baseShape(int a, float x, float z, float core) const {
        switch (a) {
            case ARCH_MOUNTAIN: return baseMountain(x, z, core);
            case ARCH_BADLANDS: return baseBadlands(x, z);
            case ARCH_PLATEAU:  return basePlateau(x, z);
            case ARCH_JAGGED:   return baseJagged(x, z, core);
            case ARCH_PLAINS:   return basePlains(x, z);
            case ARCH_DOWNS:    return baseDowns(x, z);
            default:            return 0.0f;
        }
    }

    float erodedShapeAt(int a, float x, float z, float core, int oct,
                        float* ridgeMapOut, float eroMul = 1.0f) const {
        const float bf = (a == ARCH_MOUNTAIN) ? MTN_FREQ
                       : (a == ARCH_BADLANDS) ? BAD_FREQ
                       : (a == ARCH_JAGGED)   ? JAG_FREQ
                       : (a == ARCH_PLAINS)   ? PLN_FREQ
                       : (a == ARCH_DOWNS)    ? DOWN_FREQ : PLAT_FREQ;
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
                float temp = -1.0f, float humid = -1.0f) const {
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

        if (a == ARCH_MOUNTAIN || a == ARCH_JAGGED)
            return smoothstep(0.42f, 0.84f, n);
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
    float amount[ARCH_COUNT] = {1.0f, 1.0f, 1.0f, 1.0f, 1.5f, 1.0f, 0.0f};
    float regionSize = 1.0f;
    float climateScale[2] = {2.0f, 2.0f};
};

struct TerrainSample {
    float height = 0;
    float w[ARCH_COUNT] = {};
    float temp = 0.5f;
    float humid = 0.5f;
};

// detailOct/erosionOct let the caller trade detail for speed per LOD ring: full octaves for
// near/LOD0 chunks (where the player actually sees the texture), fewer for the far LOD1/LOD2 rings
// that make up the bulk of resident chunks by count but are viewed from a distance where the extra
// octaves are imperceptible. Defaults match the original hardcoded oct=2/eoct=4 so any other caller
// is unaffected.
static TerrainSample sampleTerrain(const Generator& g, const BlendParams& bp,
                                   float x, float z, int detailOct = 2, int erosionOct = 4) {
    TerrainSample out;
    float claim[ARCH_COUNT];
    float total = 0.0f;
    out.temp = g.baseTemperature(x, z);
    out.humid = g.humidityNoise(x, z);

    for (int a = 0; a < ARCH_COUNT; ++a) {
        claim[a] = g.claim(a, x, z, bp.regionSize, out.temp, out.humid);
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

    const float DOMINANCE = 2.0f;
    float sharp = 0.0f;
    for (int a = 0; a < ARCH_COUNT; ++a) {
        out.w[a] = std::pow(out.w[a], DOMINANCE);
        sharp += out.w[a];
    }
    for (int a = 0; a < ARCH_COUNT; ++a) out.w[a] /= sharp;

    const int oct = detailOct;
    const int eoct = erosionOct;

    float eroMul = 1.0f;
    {
        float dry = 1.0f - clampf(out.humid, 0.0f, 1.0f);
        float cold = 1.0f - clampf(out.temp, 0.0f, 1.0f);
        eroMul = lerpf(0.65f, 1.35f, dry) * lerpf(0.80f, 1.20f, cold);
    }

    float shapeSum = 0.0f, usedW = 0.0f, amp = 0.0f, rough = 0.0f;
    for (int a = 0; a < ARCH_COUNT; ++a) {
        amp += out.w[a] * ARCH_AMP[a];
        rough += out.w[a] * ARCH_ROUGH[a];
        if (out.w[a] < WEIGHT_GATE) continue;
        shapeSum += out.w[a] * g.shape(a, x, z, claim[a], eoct, nullptr, eroMul);
        usedW += out.w[a];
    }
    float shape01 = usedW > 1e-6f ? shapeSum / usedW : 0.0f;

    float micro = g.microFactor(x, z);
    out.height = shape01 * amp * g.reliefFactor(x, z)
               + g.detailFn(x, z, oct) * (8.0f + 0.020f * amp) * rough * micro
               + g.baseElevation(x, z);
    return out;
}

} // namespace terrain_noise
