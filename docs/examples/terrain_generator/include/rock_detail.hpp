#ifndef ROCK_DETAIL_HPP
#define ROCK_DETAIL_HPP

// Procedural rock appearance for the terrain surface and the shell beneath it.
//
// A voxel carries an 8-bit index into its component's material palette. There is no UV anywhere in
// the voxel path, so "rock texture" here cannot mean an image: it has to be driven by position.
// That turns out to be the right medium anyway -- at the terrain's LOD0 voxel size (~0.8 m if a
// tree reads as ~25 m) grain and pitting are far below one voxel, and what actually makes rock look
// like rock at that scale is larger structure: bedding, joints, and where soil can and cannot sit.
//
// So this module produces four things, in rough order of how much they matter:
//
//   1. Exposure  -- soil and vegetation cannot cling to a steep face, so slope alone decides where
//                   bare rock shows. This is what turns a uniformly-green hillside into cliffs.
//   2. Strata    -- horizontal bedding, folded by a low-frequency field so the beds undulate rather
//                   than lying dead flat. Beds differ in COLOUR (strata) and in HARDNESS
//                   (bedSoftness); the second is what makes a cliff a stack of ledges and recesses
//                   instead of a flat wall with stripes painted on it, since at any distance what
//                   makes real bedding legible is the shadow under each resistant ledge.
//   3. Joints    -- Worley cell edges darkened into cracks, plus a per-cell tint so the rock reads
//                   as jointed blocks rather than a continuous mass; they cut geometry too.
//   4. Cover     -- lichen on damp shallow rock, scree lightening on mid slopes.
//
// Both geometry terms are combined in surfaceRecess and applied to the column HEIGHT before
// voxelization -- see the note there for why that is the only safe place to cut a heightfield.
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

// Bedding. Beds are ~22 world units thick and undulate by ~38 units over a ~1600-unit wavelength,
// which reads as gently folded sediment rather than a layer cake.
//
// Sized against the VOXEL, not against geology: at the old 14 units a bed was 8 voxels while
// kVoxelScale was 1.75, but only 4 once that doubled, and a 4-voxel band is too fine to read as
// bedding either in colour or in the relief that bedSoftness now cuts. These scale with
// TerrainState::kVoxelScale and should be revisited if it moves again.
//
// Beds are DISCRETE -- floored to an index and hashed -- rather than a smooth noise gradient. Two
// reasons. Real sedimentary contacts are sharp, so a smooth ramp reads as dirty rock rather than
// layered rock. And the terrain quantizes to a 6-level lattice, where one step is 20% lightness: a
// smooth band that swings less than that simply rounds away to nothing, which is exactly what
// happened here with a continuous fbm swinging +/-9%.
static constexpr float kBedThickness = 22.0f;
static constexpr float kFoldAmp      = 38.0f;
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
// little (thin soils high up), so does cold, since frost shatters and strips a slope, and so does
// drought.
//
// The drought term is what makes badlands and mesa country read as rock. Steepness alone cannot do
// it: a mesa's defining surface is its table, which is dead flat, so bySlope is zero exactly where
// the landform most needs to look like stone, and the climate blend in surfaceColor happily paints
// it as dry grassland. The gate opens well below the savanna band on purpose -- brown grass with
// scattered trees is a correct reading of semi-arid ground and is left alone; this only takes over
// once the ground is drier than anything can root in.
inline float exposure(float slope, float altitude01, float temp, float humid) {
    float bySlope = rsmooth(kSoilSlope, kBareSlope, slope);
    float byAltitude = rsmooth(0.70f, 0.98f, altitude01) * 0.55f;
    float byCold = rsmooth(0.30f, 0.08f, temp) * 0.30f;
    float byDrought = rsmooth(0.18f, 0.07f, humid) * 0.85f;
    return rclamp(bySlope + (1.0f - bySlope) * (byAltitude + byCold + byDrought), 0.0f, 1.0f);
}

// How vertical the surface is, 0 flat to 1 cliff. Selects which plane the joint pattern is read on:
// a cliff shows its joints in the vertical plane, a bench shows them from above.
inline float verticality(float slope) {
    return rsmooth(0.35f, 1.30f, slope);
}

// The rock type of a region, before any per-voxel detail. Five end members selected by climate, on
// the reasoning that climate correlates with the rock it weathers out of and the crust it leaves
// exposed. This is what makes an arid plateau read as red desert stone while a wet coast reads as
// dark blue-grey basalt, off the same code.
//
// The colours sit ON the 6-level lattice they will be quantized onto rather than merely near it,
// because a value midway between two levels rounds one way or the other per channel and comes out
// as an unintended hue. With six levels the available values per channel are 0, 0.2, 0.4, 0.6, 0.8
// and 1.0, and every end member below is built only from those.
//
// The arid half of the range is now the widest of the five rather than the narrowest, split into
// terracotta and buff sandstone. It was previously a thin accent band on the reasoning that a
// saturated red reads as loud at lattice resolution -- true, but the consequence was that hot dry
// country came out as tan rock under dry-grass colouring and never as the clay-red desert it should
// be. It reads loud because red desert IS loud; the thing that keeps it from being garish is that
// it is banded against buff and oxide by strata() rather than laid down as one flat sheet.
//
// `clayOut`, if given, receives how strongly this region is clay, for strata() to key its hue
// banding off. It is the post-normalization weight, so it is directly comparable against a
// threshold regardless of what else the blend contains.
inline vec3 baseColor(float temp, float humid, float* clayOut = nullptr) {
    float dry  = 1.0f - rsmooth(0.30f, 0.56f, humid);       // 1 in true desert, 0 by temperate
    float hot  = rsmooth(0.44f, 0.70f, temp);
    float arid = dry * hot;

    // Iron staining is really a property of the formation, which nothing here knows about; the
    // depth of the drought stands in for it, so the driest ground is the reddest and the margins of
    // the desert grade out through sandstone rather than ending at a line.
    float clay = arid * rsmooth(0.40f, 0.16f, humid);
    float buff = arid - clay;
    float rest = 1.0f - arid;
    // Cold claims a region before wet does. A glaciated slope is pale limestone and granite whether
    // or not it is damp, and letting the two split it 50/50 blended pale grey with blue basalt into
    // a cyan that is not a rock colour -- another blend landing between lattice levels.
    float pale = rsmooth(0.42f, 0.16f, temp) * rest;
    float dark = rsmooth(0.44f, 0.70f, humid) * (rest - pale);
    float mid  = rclamp(rest - pale - dark, 0.0f, 1.0f);
    float sum  = clay + buff + pale + dark + mid;
    if (sum < 1e-5f) { if (clayOut) *clayOut = 0.0f; return vec3{0.6f, 0.6f, 0.6f}; }
    if (clayOut) *clayOut = clay / sum;

    return (vec3{0.80f, 0.40f, 0.20f} * clay     // terracotta / iron-red clay
          + vec3{0.80f, 0.60f, 0.40f} * buff     // buff sandstone
          // Wet basalt. Deliberately pushed clear of the lattice mid-points: at (0.22,0.25,0.36)
          // its blend with `mid` put red at exactly 1.50 steps, so a hair of climate variation
          // flipped that one channel and half the biome came out teal instead of blue-grey. An end
          // member has to be chosen so its BLENDS land inside a level, not just itself.
          + vec3{0.20f, 0.20f, 0.40f} * dark
          + vec3{0.80f, 0.80f, 0.80f} * pale     // cold limestone / granite
          + vec3{0.60f, 0.60f, 0.60f} * mid)     // neutral temperate stone
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
    float clay = 0.0f;            // how strongly this is clay country; selects the strata mode
};

inline Column prepare(const terrain_noise::Perlin& n, float wx, float wz,
                      float temp, float humid, float altitude01, float slope) {
    Column c;
    c.base = baseColor(temp, humid, &c.clay);
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
// Two modes, switched on rather than blended between (same reasoning as the cover switches below).
//
// Everywhere else: lightness only, applied equally to all three channels. Tilting hue between beds
// was tried and removed there, because on a 6-level lattice the smallest possible hue offset is a
// whole step, which turns grey stone into saturated blue or orange stripes. Real bedding reads as
// value contrast at any distance anyway.
//
// In clay country: hue, and this is the point of the whole module there. Banded red rock is not
// red rock with light and dark stripes, it is terracotta against buff against oxide against pale
// marl -- the mineral genuinely changes bed to bed, and rendering that as one hue at four
// brightnesses is what makes procedural badlands look like painted cardboard.
//
// What makes this safe here when it was not safe on grey stone is that the four members are picked
// to be exactly one, two or three whole lattice steps apart, so the offsets below are EXACT on the
// lattice: terracotta (0.8,0.4,0.2) + (0,+1,+1) steps is buff (0.8,0.6,0.4), + (-1,-1,0) is oxide
// (0.6,0.2,0.2), + (+1,+2,+2) is pale marl (1.0,0.8,0.6). Nothing rounds, so no bed can come out a
// colour that was not chosen. The old objection was never to hue banding as such, it was to hue
// offsets that land between levels.
inline vec3 strata(const Column& col, float wy) {
    float u = (wy + col.bedDatum) / col.bedThickness;
    int bed = int(std::floor(u));
    float h1 = bedHash(bed, 1u);

    if (col.clay > 0.5f) {
        // Weighted so terracotta stays the dominant rock and the other three read as beds within
        // it; an even split makes the sequence too busy to read as a formation.
        if (h1 < 0.44f) return vec3{0.0f, 0.0f, 0.0f};                       // terracotta
        if (h1 < 0.72f) return vec3{0.0f, kStep, kStep};                     // buff sandstone
        if (h1 < 0.90f) return vec3{-kStep, -kStep, 0.0f};                   // deep oxide red
        return vec3{kStep, 2.0f * kStep, 2.0f * kStep};                      // pale marl
    }

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

// Deepest a joint cuts back into the rock, in WORLD units.
//
// This used to be expressed in voxels, which quietly made the landform itself depend on which LOD
// ring a chunk happened to be generated at: the caller multiplies by the ring's voxel size, so the
// same joint recessed 8.75 units at LOD0 and 280 at LOD2. Two rings disagreeing about the shape of
// the ground is a popping artefact on every refine, and it gets worse the coarser the far ring is.
// In world units every ring cuts the same groove, and at a coarse ring it correctly rounds away to
// nothing rather than being magnified sixteen-fold.
static constexpr float kJointCarveWorld = 14.0f;
// How far a soft bed weathers back from the resistant one above it, in world units. Deliberately
// half the bed thickness, so a recessed bed reads as a groove with rock above and below it rather
// than as a general roughening.
static constexpr float kBedReliefWorld = 9.0f;
// Exposure below which nothing is carved: joints only open up where there is bare rock. Soil and
// turf bridge over a joint rather than following it down.
static constexpr float kCarveMinExposure = 0.35f;

// How readily the bed at this altitude weathers: 0 for a resistant caprock that stands proud, 1 for
// a soft bed that retreats behind it.
//
// Discrete, and read off the SAME bed index that strata() colours from, so a recessed groove and a
// colour band are the same bed rather than two independent patterns that happen to share a
// wavelength. Hardness and colour are hashed separately though -- a soft bed is not reliably a
// particular colour in real rock, and forcing the correlation makes the sequence look printed on.
inline float bedSoftness(const Column& col, float wy) {
    float u = (wy + col.bedDatum) / col.bedThickness;
    int bed = int(std::floor(u));
    float h = bedHash(bed, 23u);
    if (h < 0.34f) return 1.0f;    // shale/marl: retreats
    if (h < 0.68f) return 0.45f;   // ordinary
    return 0.0f;                   // resistant: forms the ledge
}

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
//
// The bedding term is the same idea applied to the other pattern this module draws. Bedding was
// colour-only, which left a cliff face geometrically smooth and painted with stripes -- and a flat
// wall reads as flat however it is coloured, because the thing that makes real strata legible at
// distance is the shadow line under each resistant ledge, not the change of hue. Beds differ in
// hardness, the soft ones retreat, and the face becomes a stack of ledges and recesses.
//
// It works here for exactly the reason the joint carve does: applied to the column HEIGHT before
// voxelization, so a heightfield column keeps its full thickness and simply starts lower, and a
// cliff face IS the tops of a run of columns stepping down, so recessing them cuts a groove ACROSS
// the face. Neighbouring columns on a face sit in different beds and so recess by different
// amounts, which is what turns the wall into a profile; columns on a flat top all share one bed and
// recess together, which does nothing visible, correctly -- a bench, not a groove.
//
// Returned in world units. Both terms scale with exposure, so this is a property of bare rock: turf
// bridges over a joint and soil buries a bedding plane.
inline float surfaceRecess(const Column& col, const terrain_noise::Worley& w,
                           float wx, float wy, float wz, float exposureAmount) {
    if (exposureAmount < kCarveMinExposure) return 0.0f;

    float crack = crackField(w, wx, wy, wz, col.vert);
    float joint = rsmooth(0.25f, 1.0f, crack) * kJointCarveWorld * exposureAmount;

    // Weighted toward steep ground. On a face, differential weathering cuts back into the wall; on
    // a near-flat top there is no wall to cut into and the same beds form a stepped bench instead,
    // which needs much less displacement to read.
    float bed = bedSoftness(col, wy) * kBedReliefWorld * exposureAmount
              * (0.30f + 0.70f * col.vert);

    return joint + bed;
}

// ---- platforms ---------------------------------------------------------------------------------

// How much of a bare cliff face is taken up by protruding shelves, 0..1. This is a coverage, not a
// probability per column: the mask is thresholded, so the face comes out as distinct shelves with
// edges rather than as a uniformly bumpy wall.
static constexpr float kPlatformCover = 0.34f;
// Feature size of the shelf patches. ~90 world units across at this frequency, so a shelf is a
// feature of a cliff rather than a texture on it.
static constexpr float kPlatformFreq = 0.011f;
// Steepness below which no shelf forms. A shelf sticking out of gently sloping ground is just a
// lump; the whole effect depends on there being a wall for it to stand out from.
static constexpr float kPlatformMinVert = 0.55f;
// Ceiling on the protrusion, world units. Also bounds the extra downward fill it costs (see the
// caller): a shelf must be solid from its top down to the rock it grows out of, or it floats.
static constexpr float kPlatformMaxOutWorld = 26.0f;

// How far this column's top is pushed OUT of a cliff face, in world units.
//
// The counterpart to surfaceRecess, and it exists because that function only ever subtracts: joints
// and soft beds cut INTO a wall, so a face built from them is a flat plane with grooves in it. What
// it never gets is anything standing proud of the plane, which is most of what makes a real cliff
// read as three-dimensional -- buttresses, ledges, blocks left behind as the face around them
// weathered back.
//
// Applied to the column HEIGHT like every other geometry term here, for the same reason (see
// surfaceRecess): a heightfield column keeps its full thickness and simply starts higher, so a
// shelf can never be a floating slab or punch a hole in the shell. And because a cliff face IS the
// tops of a run of columns stepping down, raising a patch of them pushes a shelf out of the face.
//
// The lift SNAPS to the bedding planes rather than following the mask continuously, which is what
// makes these read as platforms instead of blisters. Every column in a patch whose top falls in the
// same bed is lifted to that bed's ceiling, so they all arrive at exactly the same altitude and the
// shelf has a genuinely flat top; columns a bed lower snap a bed lower, so a wide patch comes out
// as a flight of steps. It also ties the geometry to the same bed indices strata() colours from, so
// a shelf edge and a colour band are the same feature rather than two patterns at similar scales.
//
// Pure in world position (the bedding datum comes from Column, itself a pure function of world XZ),
// so neighbouring chunks agree on every shelf and none of this shows a seam. In world units, so
// every LOD ring cuts the same shape and a coarse ring correctly rounds it away to nothing.
inline float platformLift(const Column& col, const terrain_noise::Perlin& p,
                          float wx, float wy, float wz, float exposureAmount) {
    if (exposureAmount < kCarveMinExposure) return 0.0f;
    float steep = rsmooth(kPlatformMinVert, 0.88f, col.vert);
    if (steep <= 0.001f) return 0.0f;

    // The noise library is 2D, so a third axis is synthesized the way crackField does it: a second
    // lookup taken on a vertical section. Averaging the two (rather than sampling one plane) is what
    // makes the mask vary with HEIGHT as well as position -- without the wy term a shelf would be a
    // stripe running around the whole mountain at one altitude, and with only the wy term it would
    // be a stripe running along the face at every altitude.
    float planar  = p.noise(wx * kPlatformFreq + 53.1f, wz * kPlatformFreq + 11.7f);
    float section = p.noise((wx - wz) * 0.7071f * kPlatformFreq + 5.3f, wy * kPlatformFreq);
    float mask = (planar + section) * 0.25f + 0.5f;

    // Thresholded hard on purpose. A soft mask would scale the snap below by a fraction and destroy
    // the flat top that makes this a platform; the narrow ramp is only there to keep the shelf edge
    // from aliasing into a staircase of single voxels.
    float shelf = rsmooth(1.0f - kPlatformCover - 0.04f, 1.0f - kPlatformCover + 0.04f, mask);
    if (shelf <= 0.001f) return 0.0f;

    // Resistant beds stand proud, soft ones do not -- the same hardness the recess term uses to
    // decide what retreats, read here to decide what is left behind.
    float hard = 1.0f - bedSoftness(col, wy);
    float amount = shelf * steep * exposureAmount * (0.35f + 0.65f * hard);
    if (amount <= 0.001f) return 0.0f;

    // Snap up to the ceiling of the bed this column's top sits in.
    float u = (wy + col.bedDatum) / col.bedThickness;
    float lift = (std::ceil(u) - u) * col.bedThickness;
    // A top already sitting on a bed plane would otherwise lift by zero and punch a hole in an
    // otherwise continuous shelf; give it the whole bed.
    if (lift < col.bedThickness * 0.02f) lift = col.bedThickness;

    return rclamp(lift * amount, 0.0f, kPlatformMaxOutWorld);
}

// How much of a voxel's colour comes from rock rather than from the surface cover above it. Soil is
// a thin skin: a few voxels down it is gone and you are looking at bedrock, which is exactly what
// makes a cut face show bands while the top of the same hill stays green.
inline float subsurfaceBlend(float depthBelowSurface, float voxelSize) {
    return rsmooth(voxelSize * 0.5f, voxelSize * 4.0f, depthBelowSurface);
}

}  // namespace rock

#endif
