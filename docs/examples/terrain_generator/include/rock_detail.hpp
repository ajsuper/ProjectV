#ifndef ROCK_DETAIL_HPP
#define ROCK_DETAIL_HPP

// Procedural rock appearance for the terrain surface and the shell beneath it.
//
// A voxel carries an 8-bit index into its component's material palette. There is no UV anywhere in
// the voxel path, so "rock texture" here cannot mean an image: it has to be per-voxel colour driven
// by position. That turns out to be the right medium anyway -- at the terrain's LOD0 voxel size
// (~0.4 m if a tree reads as ~25 m) grain and pitting are far below one voxel, and what actually
// makes rock look like rock at that scale is larger structure: bedding, joints, and where soil can
// and cannot sit.
//
// So this module produces four things, in rough order of how much they matter:
//
//   1. Exposure  -- soil and vegetation cannot cling to a steep face, so slope alone decides where
//                   bare rock shows. This is what turns a uniformly-green hillside into cliffs.
//   2. Strata    -- horizontal bedding, folded by a low-frequency field so the beds undulate rather
//                   than lying dead flat. Only visible because the shell below the surface is
//                   coloured per voxel by its own altitude (see subsurfaceBlend), which is what
//                   lets a cliff face show bands instead of one smeared-down surface colour.
//   3. Joints    -- Worley cell edges darkened into cracks, plus a per-cell tint so the rock reads
//                   as jointed blocks rather than a continuous mass.
//   4. Cover     -- lichen on damp shallow rock, scree lightening on mid slopes.
//
// Everything lands on the same kSurfaceLevels^3 lattice the rest of the terrain quantizes to, so
// none of this widens the material palette: it can only make fuller use of the lattice already
// budgeted for.

#include <cmath>
#include <cstdint>

#include "core/math.h"
#include "terrain_noise.hpp"

namespace rock {

using projv::core::vec3;

// ---- tuning ------------------------------------------------------------------------------------

// Slope, as rise/run, where soil starts to fail and where the ground is fully bare. ~29 deg to
// ~49 deg: gentle enough that rolling hills stay green, steep enough that only real faces go bare.
static constexpr float kSoilSlope = 0.55f;
static constexpr float kBareSlope = 1.15f;

// Bedding. Beds are ~14 world units thick (8 voxels) and undulate by ~26 units over a ~1600-unit
// wavelength, which reads as gently folded sediment rather than a layer cake.
//
// Beds are DISCRETE -- floored to an index and hashed -- rather than a smooth noise gradient. Two
// reasons. Real sedimentary contacts are sharp, so a smooth ramp reads as dirty rock rather than
// layered rock. And the terrain quantizes to a 6-level lattice, where one step is 20% lightness: a
// smooth band that swings less than that simply rounds away to nothing, which is exactly what
// happened here with a continuous fbm swinging +/-9%.
static constexpr float kBedThickness = 14.0f;
static constexpr float kFoldAmp      = 26.0f;
static constexpr float kFoldFreq     = 0.00062f;

// Every variation below is expressed as a whole number of LATTICE STEPS, added equally to all three
// channels, rather than as a multiplier.
//
// This is forced by the 6-level quantization, and getting it wrong is loud. A multiplicative tweak
// scales the gaps between channels as well as their magnitude, so on near-grey rock the brightest
// channel crosses a lattice boundary before the others and the voxel rounds to a saturated colour:
// a +30% "lightness" band on grey stone came out solid pink, another cyan. An equal additive offset
// preserves the channel differences exactly, so grey stays grey and a bed reads as lighter or
// darker stone instead of a different mineral. Deliberate hue changes (a red bed, lichen) are then
// applied explicitly, as their own whole-step shifts, rather than emerging by accident.
static constexpr float kLatticeLevels = 6.0f;
static constexpr float kStep          = 1.0f / (kLatticeLevels - 1.0f);  // 0.2

// Joint spacing, in world units, for the horizontal and vertical crack planes.
static constexpr float kJointFreq    = 0.030f;
static constexpr float kCrackWidth   = 0.16f;   // Worley edge distance counted as "in a crack"
static constexpr float kBlockTintOdds = 0.30f;  // fraction of joint blocks offset a step

// Lichen: damp, shallow, low-altitude rock only.
static constexpr float kLichenFreq   = 0.0042f;

// ---- helpers -----------------------------------------------------------------------------------

// Smoothstep that also accepts a DESCENDING range (e1 < e0), meaning "1 at e1, falling to 0 at e0".
// GLSL's smoothstep leaves that case undefined and the usual guard collapses it to a hard step in
// the wrong direction, which silently turns every inverted call site into a constant. Several of the
// terms below are naturally written descending ("colder means more bare rock", "closer to a cell
// edge means more crack"), so this handles it rather than making each caller flip its own arguments.
static inline float rsmooth(float e0, float e1, float x) {
    if (e0 == e1) return x < e0 ? 0.0f : 1.0f;
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}
static inline float rclamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline vec3 rmix(vec3 a, vec3 b, float t) { return a + (b - a) * t; }

// Fraction of the ground that is bare rock rather than soil. Steepness dominates; altitude helps a
// little (thin soils high up) and so does cold, since frost shatters and strips a slope.
inline float exposure(float slope, float altitude01, float temp) {
    float bySlope = rsmooth(kSoilSlope, kBareSlope, slope);
    float byAltitude = rsmooth(0.55f, 0.95f, altitude01) * 0.55f;
    float byCold = rsmooth(0.30f, 0.08f, temp) * 0.30f;
    return rclamp(bySlope + (1.0f - bySlope) * (byAltitude + byCold), 0.0f, 1.0f);
}

// How vertical the surface is, 0 flat to 1 cliff. Selects which plane the joint pattern is read on:
// a cliff shows its joints in the vertical plane, a bench shows them from above.
inline float verticality(float slope) {
    return rsmooth(0.35f, 1.30f, slope);
}

// The rock type of a region, before any per-voxel detail. Five end members selected by climate, on
// the reasoning that climate correlates with the rock it weathers out of and the crust it leaves
// exposed. This is what makes an arid plateau read as red-and-tan desert stone while a wet coast
// reads as dark blue-grey basalt, off the same code.
//
// The colours are chosen to sit close to the 6-level lattice they will be quantized onto, because a
// value that lands midway between two levels rounds one way or the other per channel and comes out
// as an unintended hue. Targets: tan (0.8,0.6,0.4), red (0.6,0.4,0.2), pale (0.8,0.8,0.8), dark
// (0.2,0.2,0.4), mid (0.6,0.6,0.6).
//
// `red` is deliberately the narrowest band of the five. It is an accent -- iron-stained beds and
// desert varnish streaking otherwise tan rock -- and reads badly when it covers whole hillsides:
// at lattice resolution a saturated red is a very loud colour.
inline vec3 baseColor(float temp, float humid) {
    float dry = 1.0f - rsmooth(0.28f, 0.52f, humid);        // 1 in true desert, 0 by temperate
    float hot = rsmooth(0.52f, 0.78f, temp);

    float red  = dry * hot * rsmooth(0.30f, 0.10f, humid);  // hot AND properly arid only
    float tan_ = dry * hot * (1.0f - red);                  // the rest of the warm dry range
    // Cold claims a region before wet does. A glaciated slope is pale limestone and granite whether
    // or not it is damp, and letting the two split it 50/50 blended pale grey with blue basalt into
    // a cyan that is not a rock colour -- another blend landing between lattice levels.
    float pale = rsmooth(0.42f, 0.16f, temp) * (1.0f - red - tan_);
    float dark = rsmooth(0.44f, 0.70f, humid) * (1.0f - red - tan_ - pale);
    float mid  = rclamp(1.0f - red - tan_ - dark - pale, 0.0f, 1.0f);
    float sum  = red + tan_ + dark + pale + mid;
    if (sum < 1e-5f) return vec3{0.6f, 0.6f, 0.6f};

    return (vec3{0.61f, 0.41f, 0.24f} * red      // iron-red / brick, muted
          + vec3{0.78f, 0.61f, 0.41f} * tan_     // sandstone tan
          // Wet basalt. Deliberately pushed clear of the lattice mid-points: at (0.22,0.25,0.36)
          // its blend with `mid` put red at exactly 1.50 steps, so a hair of climate variation
          // flipped that one channel and half the biome came out teal instead of blue-grey. An end
          // member has to be chosen so its BLENDS land inside a level, not just itself.
          + vec3{0.18f, 0.20f, 0.34f} * dark
          + vec3{0.80f, 0.81f, 0.83f} * pale     // cold limestone / granite
          + vec3{0.57f, 0.56f, 0.53f} * mid)     // neutral temperate stone
         * (1.0f / sum);
}

static inline float bedHash(int bed, uint32_t salt) {
    uint32_t h = uint32_t(bed) * 2654435761u ^ salt * 2246822519u;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return float(h >> 8) * (1.0f / 16777216.0f);
}

// Everything about a rock column that does not vary with altitude, evaluated once per column.
//
// This split is the difference between the rock detail being affordable and not. Every term here is
// a function of XZ alone -- the fold, the bed thickness, the lichen and scree masks -- but they are
// consumed inside the per-voxel loop, which runs ~30 times per column. Recomputing them per voxel
// costs three Perlin evaluations on all ~2M fill voxels of a chunk and measured at 45 ms per chunk;
// hoisting them leaves the per-voxel path with no noise evaluations at all, just a hash.
struct Column {
    vec3  base{0.5f, 0.5f, 0.5f};
    float bedDatum = 0.0f;        // folded altitude offset, world units
    float bedThickness = kBedThickness;
    float lichen = 0.0f;          // already combined with wetness/slope/altitude
    float scree = 0.0f;
    float vert = 0.0f;
};

inline Column prepare(const terrain_noise::Perlin& n, float wx, float wz,
                      float temp, float humid, float altitude01, float slope) {
    Column c;
    c.base = baseColor(temp, humid);
    c.vert = verticality(slope);

    float fold = terrain_noise::fbm(n, wx * kFoldFreq, wz * kFoldFreq, 2, 2.0f, 0.5f);
    c.bedDatum = fold * kFoldAmp;
    // Bed thickness varies across the world rather than along a single column, which is both
    // cheaper and geologically sensible -- a formation has a characteristic bedding, and it changes
    // as you walk into the next one.
    c.bedThickness = kBedThickness * (1.0f + fold * 0.45f);

    float lichenMask = terrain_noise::fbm(n, wx * kLichenFreq + 31.4f, wz * kLichenFreq + 2.7f,
                                          3, 2.0f, 0.5f) * 0.5f + 0.5f;
    c.lichen = rsmooth(0.52f, 0.78f, lichenMask)
             * rsmooth(0.42f, 0.68f, humid)
             * (1.0f - c.vert)
             * rsmooth(0.85f, 0.55f, altitude01);

    float screeBand = rsmooth(0.34f, 0.52f, c.vert) * rsmooth(0.82f, 0.62f, c.vert);
    float screePatch = terrain_noise::fbm(n, wx * 0.006f + 77.0f, wz * 0.006f + 13.0f,
                                          2, 2.0f, 0.5f) * 0.5f + 0.5f;
    c.scree = screeBand * rsmooth(0.45f, 0.72f, screePatch);
    return c;
}

// Bedding. Returns a per-channel ADDITIVE offset in whole lattice steps: one bed sits a step
// lighter or darker than the next.
//
// The bed index is read off a *folded* altitude -- offsetting Y by a slow 2D field tilts and waves
// the beds, so they are not stacked pancakes. The offset is bounded rather than a true dip angle,
// so it cannot run away with distance from the origin.
//
// Lightness only, applied equally to all three channels. Tilting hue between beds was tried and
// removed: on a 6-level lattice the smallest possible hue offset is a whole step, which turns grey
// stone into saturated blue or orange stripes. Real bedding reads as value contrast at any distance
// anyway -- and the base rock colour already shifts with climate across the world, so the sequence
// is not monochrome, it just does not change mineral every 14 units.
inline vec3 strata(const Column& col, float wy) {
    float u = (wy + col.bedDatum) / col.bedThickness;
    int bed = int(std::floor(u));
    float h1 = bedHash(bed, 1u);
    float light = (h1 < 0.30f ? -1.0f : (h1 > 0.70f ? 1.0f : 0.0f)) * kStep;
    return vec3{light, light, light};
}

// Joint pattern. Returns {crack darkening 0..1, per-block lightness offset}. `vert` blends between
// reading the cells in plan (flat ground) and on a vertical section (cliff faces), so joints always
// run across the surface you are actually looking at.
// Just the crack term, without the per-block tint's extra cell lookup. Split out because the
// geometry carve below needs it once per column and does not care about block tinting.
inline float crackField(const terrain_noise::Worley& w, float wx, float wy, float wz, float vert) {
    float planar = w.edge(wx * kJointFreq, wz * kJointFreq);
    // Vertical section taken on the diagonal so it is not axis-aligned with the planar set.
    float section = w.edge((wx + wz) * 0.7071f * kJointFreq, wy * kJointFreq);
    float e = planar + (section - planar) * vert;
    return rsmooth(kCrackWidth, 0.0f, e);
}

inline void joints(const terrain_noise::Worley& w, float wx, float wy, float wz, float vert,
                   float& crackOut, float& blockTintOut) {
    crackOut = crackField(w, wx, wy, wz, vert);

    // Per-block offset, in whole steps: most blocks sit at the bed's own level, a minority a step
    // off it, so the rock reads as jointed masses rather than one continuous slab.
    float cellHash = 0.0f;
    w.cell(wx * kJointFreq, (wz + wy * vert) * kJointFreq, &cellHash);
    float half = kBlockTintOdds * 0.5f;
    blockTintOut = cellHash < half ? -1.0f : (cellHash > 1.0f - half ? 1.0f : 0.0f);
}

// Full per-voxel rock colour. `vert` from verticality(), `wetness` is the local humidity, and
// `altitude01` the normalized height, both only used for cover (lichen/scree).
//
// `detail` is the switch that keeps this affordable: passing false skips the Worley work and keeps
// only the cheap bedding term, for voxels buried inside the terrain shell where nothing can see the
// joints anyway. See the exposure test in generateChunkVoxels.
inline vec3 color(const Column& col, const terrain_noise::Worley& w,
                  float wx, float wy, float wz, bool detail) {
    // All offsets are additive whole lattice steps -- see the note on kStep above.
    vec3 offset = strata(col, wy);

    if (detail) {
        float crack = 0.0f, blockTint = 0.0f;
        joints(w, wx, wy, wz, col.vert, crack, blockTint);
        // A joint is a dark line, not a gradient: one step down wherever the cell edge passes.
        float shade = -kStep * rsmooth(0.35f, 0.65f, crack) - kStep * blockTint;
        offset = offset + vec3{shade, shade, shade};
    }

    vec3 c = col.base + offset;

    // Lichen mottles damp shallow rock; scree lightens the broken slopes below faces. Both masks
    // were resolved per column, so only the (trivial) selection is per voxel.
    //
    // Both SWITCH rather than blend. Partially mixing toward a different colour is the one thing
    // that reliably breaks on this lattice: a half-strength blend leaves the three channels at
    // unrelated fractions of a step, they round independently, and the result is a colour that was
    // never in either input -- half-strength scree turned desert tan into pink, and lichen turned
    // wet basalt teal. Thresholding a smooth spatial mask gives organic patch edges anyway, and
    // every voxel lands on a colour that was deliberately chosen.
    if (col.lichen > 0.55f) c = vec3{0.42f, 0.44f, 0.24f};   // olive lichen crust, lattice-aligned
    if (col.scree > 0.50f) c = c + vec3{kStep, kStep, kStep}; // broken rock reads lighter

    return vec3{rclamp(c.x, 0.0f, 1.0f), rclamp(c.y, 0.0f, 1.0f), rclamp(c.z, 0.0f, 1.0f)};
}

// ---- geometry ----------------------------------------------------------------------------------

// Deepest a joint cuts back into the rock, in voxels.
static constexpr float kCarveDepth = 5.0f;
// Exposure below which nothing is carved: joints only open up where there is bare rock. Soil and
// turf bridge over a joint rather than following it down.
static constexpr float kCarveMinExposure = 0.35f;

// How far to recess a column, in voxels, because a joint surfaces on it.
//
// This is the crack pattern doing geometry rather than just colour. It is applied to the column's
// HEIGHT, before the height is turned into voxels, which is what makes it safe in a heightfield
// shell: the column keeps its full thickness and simply starts lower, so a joint can never punch a
// hole through the shell into the hollow interior below it. Carving voxels out of the middle of the
// fill instead would do exactly that, since a joint is a surface in 3D and would slice the whole
// 30-voxel shell open along its plane.
//
// It still cuts into cliff faces, because a cliff face IS the tops of a run of columns stepping
// down: recessing each of them cuts a groove across the face. And because the joint field is 3D and
// continuous, neighbouring columns recess at correlated depths, so the groove follows the joint
// plane instead of coming out as per-column noise.
//
// Depth follows crack strength rather than being fixed, which gives the groove a V-profile --
// deepest along the joint's centre line, feathering out at its edges.
inline int carveDepth(const terrain_noise::Worley& w, float wx, float wy, float wz,
                      float vert, float exposureAmount) {
    if (exposureAmount < kCarveMinExposure) return 0;
    float crack = crackField(w, wx, wy, wz, vert);
    float d = rsmooth(0.25f, 1.0f, crack) * kCarveDepth * exposureAmount;
    return int(d + 0.5f);
}

// How much of a voxel's colour comes from rock rather than from the surface cover above it. Soil is
// a thin skin: a few voxels down it is gone and you are looking at bedrock, which is exactly what
// makes a cut face show bands while the top of the same hill stays green.
inline float subsurfaceBlend(float depthBelowSurface, float voxelSize) {
    return rsmooth(voxelSize * 0.5f, voxelSize * 4.0f, depthBelowSurface);
}

}  // namespace rock

#endif
