#ifndef PJV_RANDOM_SC
#define PJV_RANDOM_SC

// =============================================================================
// pjv_random.sc -- hashes, random streams and low-discrepancy sequences (docs/plans/shader-library.md)
// =============================================================================
//
// Mechanisms only: each takes its seed, index or pixel. Which stream a pass draws from, and how a
// frame number enters it, is the renderer's.

// --- Integer streams ---------------------------------------------------------

// One step of PCG (the RXS-M-XS output of Jarzynski & Olano 2020): advances state, returns 32 bits.
uint pjvPcg(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// A uniform number from the stream, all 32 bits scaled by 2^-32. In [0, 1], and 1.0 itself once
// float rounding is counted: for a stream that must stay below 1, pjvRandomUnit24.
float pjvRandomUnit(inout uint state) {
    return float(pjvPcg(state)) * (1.0 / 4294967296.0);
}

// A uniform number in [0, 1) exactly: the top 24 bits, which a float holds without rounding.
float pjvRandomUnit24(inout uint state) {
    return float(pjvPcg(state) >> 8u) * (1.0 / 16777216.0);
}

// A seed from three integers -- typically (pixel x, pixel y, frame) -- mixed so neighbouring seeds
// start uncorrelated streams.
uint pjvHashSeed(uvec3 v) {
    uint h = v.x * 1973u + v.y * 9277u + v.z * 26699u;
    h = (h ^ (h >> 15u)) * 2246822519u;
    h = (h ^ (h >> 13u)) * 3266489917u;
    return h ^ (h >> 16u);
}

// --- Float hashes and noise --------------------------------------------------

// Hashes of a float position (Hoskins, "hash without sine"). Cheap and stateless; for a stream, PCG.
// Transcendental-free on purpose: the sin()-based hash these replaced in the animated march measured as
// the dominant cost of the whole resolve. pjvHash23 gives two decorrelated values for the price of one.
float pjvHash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec2 pjvHash23(vec3 p) {
    vec3 q = fract(p * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yxz + 33.33);
    return fract(vec2((q.x + q.y) * q.z, (q.x + q.z) * q.y));
}

// Trilinear value noise over pjvHash13 at integer lattice points, smoothstepped (C1, so its gradient
// stays small). In [-1, 1].
float pjvValueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float c000 = pjvHash13(i + vec3(0.0, 0.0, 0.0));
    float c100 = pjvHash13(i + vec3(1.0, 0.0, 0.0));
    float c010 = pjvHash13(i + vec3(0.0, 1.0, 0.0));
    float c110 = pjvHash13(i + vec3(1.0, 1.0, 0.0));
    float c001 = pjvHash13(i + vec3(0.0, 0.0, 1.0));
    float c101 = pjvHash13(i + vec3(1.0, 0.0, 1.0));
    float c011 = pjvHash13(i + vec3(0.0, 1.0, 1.0));
    float c111 = pjvHash13(i + vec3(1.0, 1.0, 1.0));
    float x00 = mix(c000, c100, f.x);
    float x10 = mix(c010, c110, f.x);
    float x01 = mix(c001, c101, f.x);
    float x11 = mix(c011, c111, f.x);
    return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z) * 2.0 - 1.0;
}

// --- Per-pixel patterns ------------------------------------------------------

// Interleaved gradient noise (Jimenez 2014): a per-pixel offset in [0, 1) whose neighbours differ
// as much as they can, so a small kernel or a temporal mean averages it away.
float pjvInterleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// The 4x4 Bayer (ordered dither) index of a pixel: 0..15, each once per tile.
float pjvBayer4(vec2 pixel) {
    ivec2 p = ivec2(mod(pixel, 4.0));
    int fine = 2 * ((p.x ^ p.y) & 1) + (p.y & 1);
    int coarse = 2 * (((p.x ^ p.y) >> 1) & 1) + ((p.y >> 1) & 1);
    return float(4 * fine + coarse);
}

// --- Low-discrepancy sequences -----------------------------------------------

// The radical inverse of index in base (Halton): sixteen digits, enough for any index a frame counter
// reaches in base 2 or 3.
float pjvHalton(int index, int base) {
    float f = 1.0;
    float r = 0.0;
    for (int k = 0; k < 16; k++) {
        if (index <= 0) break;
        f /= float(base);
        r += f * float(index - (index / base) * base);
        index /= base;
    }
    return r;
}

#endif
