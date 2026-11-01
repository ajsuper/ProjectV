#ifndef PJV_MOTION_SC
#define PJV_MOTION_SC

// The motion table: what makes animated voxels move. The sets the program configures (pjvMotionSets,
// engine-owned), the fields they describe -- sway displaces, advection flows -- and the clock.
// The traversal's animated march resolves them (pjv_scene.sc, PJV_SCENE_ANIMATION).

#include <pjv_engine.sc>  // pjvAnimTime, pjvMotionSets
#include <pjv_random.sc>  // pjvHash23, pjvValueNoise

// =============================================================================
// The motion table
// =============================================================================
//
// Engine-owned, exactly as passTargetRes is, and declared with it in pjv_engine.sc. Laid out per
// motion set:
//
//   [2i+0] = (kind, amplitude in VOXELS, spatial frequency, speed)
//   [2i+1] = (direction.xyz, turbulence)
//
// A material's flags byte names a row (see MATERIAL_FLAG_ANIMATED in scene.h). The indirection is
// what makes this a mechanism rather than a menu: grass sway, leaf flutter, a rising flame and a
// water surface are one traversal and four rows, not four code paths. An all-zero table -- what
// every renderer that never configures one gets -- means nothing animates.

#define PJV_MOTION_NONE   0u
#define PJV_MOTION_SWAY   1u
#define PJV_MOTION_ADVECT 2u

struct PjvMotionSet {
    uint  kind;
    float amplitude;    // voxels
    float frequency;    // cycles per voxel
    float speed;
    vec3  direction;    // already normalised on upload
    float turbulence;
    // ---- ADVECTION ONLY; a sway set leaves these zero and never reads them ----------------
    float dissolve;         // 0..1, how readily a risen parcel burns out
    float turbulenceGrowth; // how much the swirl grows with age
    float travel;           // how far a parcel may rise from its source, in voxels
};

PjvMotionSet pjvMotionSet(uint index) {
    uint base = 3u * min(index, 15u);
    vec4 a = pjvMotionSets[base];
    vec4 b = pjvMotionSets[base + 1u];
    vec4 c = pjvMotionSets[base + 2u];
    PjvMotionSet m;
    m.kind = uint(a.x + 0.5);
    m.amplitude = a.y;
    m.frequency = a.z;
    m.speed = a.w;
    m.direction = b.xyz;
    m.turbulence = b.w;
    // Advection's row. Zero for a sway set, which never reads it.
    m.dissolve = c.x;
    m.turbulenceGrowth = c.y;
    m.travel = c.z;
    return m;
}

float pjvAnimSeconds() { return pjvAnimTime.x; }

// ---- The field -------------------------------------------------------------------------------
//
// Value noise, advected along the motion set's direction. It replaced a travelling plane wave, and
// the reason is worth keeping: a plane wave's level sets are straight lines, and a quantised
// displacement DRAWS its level set -- so the grass moved in visible straight bands across the field.
//
// What it must be is LOW FREQUENCY, and that is a correctness requirement rather than a taste one.
// Neighbouring voxels of one blade get their displacement from their own positions, so if the field
// varies quickly in space those displacements diverge and the blade separates into floating
// segments. That does not read as motion blur; it reads as a bug. Hence a field smooth over tens of
// voxels, and hence NOT a per-voxel position hash -- a hash has no scale, so neighbours are
// independent, and independent neighbours under a quantiser is torn geometry by construction.
// The field is pjvValueNoise (pjv_random.sc), over a transcendental-free lattice hash: the sin()-based
// one it replaced ran four sines per corner set and the field is evaluated up to nine times per
// candidate cell, which measured as the dominant cost of the whole resolve.

// TWO independent noise values from ONE lattice, and it costs what one costs.
//
// Advection needs a pair per step -- the two components of its swirl -- and calling pjvValueNoise
// twice pays for two sets of eight corner hashes and two trilinear blends. Nearly all of that is
// shared: the lattice cell, the smoothstep weights and the mix tree are identical, and only the hash
// differs. A hash that emits two decorrelated floats instead of one therefore halves the whole
// function, which is the single hottest thing in the advection trace. The hash is pjvHash23.
vec2 pjvValueNoise2(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    vec2 c000 = pjvHash23(i + vec3(0.0, 0.0, 0.0));
    vec2 c100 = pjvHash23(i + vec3(1.0, 0.0, 0.0));
    vec2 c010 = pjvHash23(i + vec3(0.0, 1.0, 0.0));
    vec2 c110 = pjvHash23(i + vec3(1.0, 1.0, 0.0));
    vec2 c001 = pjvHash23(i + vec3(0.0, 0.0, 1.0));
    vec2 c101 = pjvHash23(i + vec3(1.0, 0.0, 1.0));
    vec2 c011 = pjvHash23(i + vec3(0.0, 1.0, 1.0));
    vec2 c111 = pjvHash23(i + vec3(1.0, 1.0, 1.0));
    vec2 x00 = mix(c000, c100, f.x);
    vec2 x10 = mix(c010, c110, f.x);
    vec2 x01 = mix(c001, c101, f.x);
    vec2 x11 = mix(c011, c111, f.x);
    return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z) * 2.0 - 1.0;   // -1..1
}

// Displacement of a SWAY set at a world position, in voxels.
//
// A function of the POSITION and the time, and of nothing else -- not of the ray, and not of which
// voxel is asking. That is the property the whole resolve depends on: two rays crossing one cell
// must compute the same answer for it, or the voxel is drawn in two places at once and appears
// sliced in half along the silhouette where the two rays disagree.
vec3 pjvMotionDisplacement(PjvMotionSet m, vec3 worldPos, float voxelSize, float t) {
    if (m.kind != PJV_MOTION_SWAY || m.amplitude <= 0.0) return vec3(0.0);
    // Sampled in VOXEL units so a set's frequency means the same thing whatever a chunk's scale is.
    vec3 p = worldPos / max(voxelSize, 1e-6);
    vec3 flow = m.direction * (m.speed * t);
    float signal = pjvValueNoise(p * m.frequency - flow * m.frequency);
    // Turbulence adds a second, finer octave, kept well below the base frequency on purpose: it stops
    // the field reading as one smooth swell rather than adding detail the blades could hold. Skipped
    // outright at zero -- it doubles the cost of the single hottest function in the resolve, and
    // paying that to add nothing is not a trade worth making by default.
    if (m.turbulence > 1e-3) {
        signal += m.turbulence * pjvValueNoise(p * (m.frequency * 2.7) - flow * m.frequency * 1.7);
    }
    signal = clamp(signal, -1.0, 1.0);
    // Perpendicular-to-vertical: wind pushes along its direction, and a horizontal set has no y.
    return m.direction * (signal * m.amplitude);
}

// The flow at a point, in VOXELS PER STEP, in WORLD orientation (the caller rotates it into the
// chunk's frame). Rise along the set's direction, plus a swirl that grows with age.
//
// The pattern SCROLLS ALONG THE RISE rather than merely varying with time, and that distinction is
// what makes a parcel appear to travel: the backward trace follows a path that itself moves, so one
// cell resolves to a different part of the flame from frame to frame. A field that only oscillated
// in place would shimmer without rising.
vec3 pjvAdvectFlow(PjvMotionSet m, vec3 worldPos, float voxelSize, float t, float ageFrac) {
    // Voxel units, so a set's frequency means the same thing whatever a chunk's scale is -- the same
    // convention pjvMotionDisplacement uses.
    vec3 p = worldPos / max(voxelSize, 1e-6);
    vec3 q = p * m.frequency - m.direction * (m.speed * t * m.frequency);

    vec2 ab = pjvValueNoise2(q);
    float a = ab.x;
    float b = ab.y;

    // TURBULENCE GROWS WITH AGE, which is the difference between a glow and a flame. A real fire is
    // laminar where it leaves the fuel and ragged by the time it is a few voxels up; uniform
    // turbulence gives a body that wanders as a whole instead of one that breaks into tongues.
    float turb = m.turbulence * (1.0 + m.turbulenceGrowth * ageFrac);

    // The swirl is built in the plane PERPENDICULAR to the rise, with only a quarter of it along the
    // rise itself. Displacing mostly across the flow is what spreads a plume; displacing along it
    // only speeds parcels up and down, which reads as flicker rather than as motion.
    vec3 up = m.direction;
    vec3 seed = abs(up.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 e1 = normalize(cross(up, seed));
    vec3 e2 = cross(up, e1);
    vec3 swirl = (e1 * a + e2 * b + up * ((a + b) * 0.25)) * turb;

    return up + swirl;
}

#endif
