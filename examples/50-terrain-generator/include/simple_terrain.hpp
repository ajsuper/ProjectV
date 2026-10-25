#ifndef SIMPLE_TERRAIN_HPP
#define SIMPLE_TERRAIN_HPP

// The terrain, from scratch: one Perlin heightfield, grass where it is gentle and rock where it is
// steep, conifers, and blades of grass.
//
//   height    fBm Perlin (noise.hpp), five octaves, reshaped so valleys are broad and hills rise
//             out of them rather than the whole map being one even swell.
//   rock      3D: a density, not a height, wherever the ground is steep -- shelves, undercuts and
//             overhangs along the rock's beds (see "Rock" below) -- coloured as rock where the slope
//             passes kStoneSlope0..kStoneSlope1, dithered so an outcrop's edge is broken. Boulders
//             merged in at the foot of cliffs.
//   colour    named entries (terrain.grass.*, terrain.rock.*, terrain.dirt, ...): grass is tinted
//             in broad patches by a low-frequency noise; rock as described below.
//
// Everything here is a pure function of world position, so two worker threads generating
// neighbouring chunks agree on every column on their shared seam.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "noise.hpp"

namespace simple_terrain {

// --- The heightfield, in world units ------------------------------------------------------------
//
// A terrain voxel is 2.8 units at the nearest ring and the player's eye is 40 units up, so a unit is
// about 4 cm: kRelief is a hillside of ~45 m and kFeatureSize puts hills ~130 m apart.
constexpr float  kFeatureSize = 3200.0f;   // wavelength of the base octave
constexpr int    kOctaves     = 5;
constexpr float  kMinHeight   = 40.0f;     // floor of the deepest valley
constexpr float  kRelief      = 1150.0f;   // valley floor to the highest summit
constexpr float  CEILING      = kMinHeight + kRelief;

// --- Rock ---------------------------------------------------------------------------------------
//
// **Rock is a 3D shape, not a height.** Each voxel is solid where a density is positive:
//
//     density = (h - y) + reach * rockShape(x, y, z) + grain
//
// On meadow `reach` is 0 and this is exactly the heightfield (solid below h). On steep ground `reach`
// grows with how rocky the slope is (up to kOverhang), and rockShape -- in [-1, 1] -- pushes the face
// in and out *as a function of height as well as position*. Where it changes faster with height than
// the slope does, a column has rock above air: an overhang, an alcove, a ledge standing out from the
// face. A heightfield cannot say any of that, which is why the cliffs used to be stepped slopes.
//
// rockShape is mostly the beds: every bed of rock stands out or sits back by its own amount, varied
// along the face, so a cliff is shelves of harder rock over undercut softer ones -- the shape of real
// sedimentary cliffs. Then broad warped masses, so the face is not a stack of identical plates, and a
// little fine grain (white noise) for roughness. Boulders are separate shapes, merged in (see below).
//
// Colour is unchanged in spirit: structure at three scales and low contrast at the smallest one --
//   * layers: each bed its own tone, matching the shelves the geometry makes;
//   * breaks: joints along the boundaries of large Worley blocks;
//   * regions: broad warm (brown-grey) and cool (blue-grey) stretches;
//   * and only then a little per-voxel variation, a fraction of one ramp step.
constexpr float kStoneSlope0 = 0.70f;      // rise/run where rock starts showing through (~35 deg)
constexpr float kStoneSlope1 = 1.10f;      // ... and where it is all rock (~48 deg)
constexpr float kBedHeight   = 14.0f;      // world units per bed (5 nearest-ring voxels), on average
// World units the 3D term can move a face: ~15 voxels. At 6 voxels the shelves were a texture on a
// slope rather than a shape -- the 3D barely showed on a cliff.
constexpr float kOverhang    = 42.0f;
// The 3D term starts on ground gentler than the rock does (from ~20 degrees, at half strength), so a
// steep grassy hillside already has outcrops and shelves under its turf, and it is at full strength
// wherever the ground is rock.
constexpr float kShapeSlope0 = 0.36f;
constexpr float kShapeSlope1 = 0.70f;
constexpr float kRockGrain   = 0.6f;       // world units of fine roughness on a rock face
// The most the rock's surface moves off the smooth height, for bounds tests.
constexpr float kRockReach   = kOverhang + kRockGrain;

// --- Boulders -----------------------------------------------------------------------------------
//
// Lumpy ellipsoids, a third to half buried, scattered on a jittered lattice (one candidate per
// kBoulderCell): gathered on the gentle ground at the foot of cliffs, rare out in the meadows. A pure function of
// position like everything else, so every chunk a boulder overlaps builds the same boulder.
constexpr float kBoulderCell   = 130.0f;
// Of cells: a rare boulder out in a meadow, and up to kBoulderTalus of them on the gentle ground at
// the foot of a cliff -- where rock that falls ends up. Never on a slope itself, where a boulder reads
// as stuck to the hill rather than resting on it.
constexpr float kBoulderChance = 0.025f;
constexpr float kBoulderTalus  = 0.30f;
constexpr float kBoulderMaxSlope = 0.45f;  // rise/run: ~24 degrees
constexpr float kBoulderRMin   = 6.0f;     // world units
constexpr float kBoulderRMax   = 24.0f;
// How far a boulder's lumps can reach past its radius, and how high its top can stand over the ground.
constexpr float kBoulderLump   = 1.35f;
constexpr float kBoulderAbove  = kBoulderRMax * 1.8f;

// --- Grass blades (nearest ring only) -----------------------------------------------------------
constexpr float kBladeChance = 0.32f;      // of grass columns, at the densest patches
constexpr int   kBladeMaxVoxels = 3;

inline uint32_t hashColumn(int x, int z, uint32_t salt) {
    uint32_t h = uint32_t(x) * 0x8DA6B343u ^ uint32_t(z) * 0xD8163841u ^ salt * 0xCB1AB31Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
inline float hashUnit(int x, int z, uint32_t salt) {
    return float(hashColumn(x, z, salt) >> 8) * (1.0f / 16777216.0f);
}
inline float smoothstep(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// Worley (cellular) noise: the distances to the nearest and second-nearest of one jittered feature
// point per lattice cell, in cell units, and the nearest one's cell as an id. This is what rock
// generators are built on (Blender's and Houdini's cliff setups, Cinema 4D's Rock Surface): each cell
// is a block of rock, and f2 - f1 is near zero exactly on the boundary between two blocks -- a
// continuous network of lines, which is what a joint or a crack is. Thresholding a smooth noise for
// cracks instead only ever crosses a jagged voxel surface at scattered points.
struct Cells { float f1, f2; uint32_t id; };
inline uint32_t hashCell3(int x, int y, int z, uint32_t seed) {
    return hashColumn(x * 73856093 ^ y * 19349663, z, seed);
}
inline Cells worley3(float x, float y, float z, uint32_t seed) {
    int cx = int(std::floor(x)), cy = int(std::floor(y)), cz = int(std::floor(z));
    Cells c{1e9f, 1e9f, 0u};
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                int gx = cx + dx, gy = cy + dy, gz = cz + dz;
                uint32_t h = hashCell3(gx, gy, gz, seed);
                float px = float(gx) + float(h & 0xFFu) / 255.0f - x;
                float py = float(gy) + float((h >> 8) & 0xFFu) / 255.0f - y;
                float pz = float(gz) + float((h >> 16) & 0xFFu) / 255.0f - z;
                float d = std::sqrt(px * px + py * py + pz * pz);
                if (d < c.f1) { c.f2 = c.f1; c.f1 = d; c.id = h; }
                else if (d < c.f2) { c.f2 = d; }
            }
    return c;
}
inline Cells worley2(float x, float z, uint32_t seed) {
    int cx = int(std::floor(x)), cz = int(std::floor(z));
    Cells c{1e9f, 1e9f, 0u};
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            int gx = cx + dx, gz = cz + dz;
            uint32_t h = hashColumn(gx, gz, seed);
            float px = float(gx) + float(h & 0xFFu) / 255.0f - x;
            float pz = float(gz) + float((h >> 8) & 0xFFu) / 255.0f - z;
            float d = std::sqrt(px * px + pz * pz);
            if (d < c.f1) { c.f2 = c.f1; c.f1 = d; c.id = h; }
            else if (d < c.f2) { c.f2 = d; }
        }
    return c;
}

struct Generator {
    siv::PerlinNoise height{1u};
    siv::PerlinNoise patches{2u};   // grass tint and blade density
    siv::PerlinNoise forest{3u};    // where the trees are thick, and which conifer grows there
    siv::PerlinNoise rock{4u};      // the rock's beds, breaks and regions
    uint32_t seed = 1;

    void reseed(uint32_t s) {
        seed = s;
        height.reseed(s);
        patches.reseed(s ^ 0x9E3779B9u);
        forest.reseed(s ^ 0x85EBCA6Bu);
        rock.reseed(s ^ 0xC2B2AE35u);
    }

    float rock3(float x, float y, float z) const {
        return float(rock.noise3D_01(double(x), double(y), double(z)));
    }

    // Where the beds are at this XZ: a gentle warp, so the layers dip and roll across a cliff
    // instead of lying dead level.
    float bedWarp(float x, float z) const {
        return (rock3(x / 160.0f, 0.37f, z / 160.0f) - 0.5f) * 3.0f * kBedHeight;
    }
    // How thick the beds are here: thin-bedded in one outcrop, massive in the next. One thickness
    // everywhere stripes every cliff in the world alike, like plywood.
    float bedHeight(float x, float z) const {
        return kBedHeight * (0.6f + 1.4f * rock3(x / 420.0f, 7.7f, z / 420.0f));   // 8..28 units
    }
    // Which bed a height is in, as a continuous coordinate: the integer part is the bed.
    float bedCoord(float x, float y, float z) const {
        return (y + bedWarp(x, z)) / bedHeight(x, z);
    }

    // The smooth ground: what trees stand on and what the slope is measured from.
    float heightAt(float x, float z) const {
        double n = height.octave2D_01(double(x) / kFeatureSize, double(z) / kFeatureSize, kOctaves);
        // fBm sits mostly in 0.25..0.75; spread that over the full range, then bias low with a
        // power so most ground is valley and meadow and the hills stand up out of it.
        float t = float((n - 0.22) / 0.56);
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return kMinHeight + kRelief * std::pow(t, 1.45f);
    }

    // 0..1 broad patches, for grass tint and blade density.
    float patchAt(float x, float z) const {
        return float(patches.octave2D_01(double(x) / 900.0, double(z) / 900.0, 2));
    }

    // 0..1 broad regions, for forest density and which conifer.
    float forestAt(float x, float z, float scale) const {
        return float(forest.octave2D_01(double(x) / scale, double(z) / scale, 2));
    }
};

// How much of a column is rock, from its slope.
inline float stoneness(float slope) { return smoothstep(kStoneSlope0, kStoneSlope1, slope); }
// How far a column's ground may move in 3D: all of kOverhang wherever there is rock, half of it on
// steep grass.
inline float rockReach(float slope) {
    return kOverhang * std::max(stoneness(slope), 0.5f * smoothstep(kShapeSlope0, kShapeSlope1, slope));
}

// The rock's 3D displacement, in [-1, 1]: beds standing out or sitting back, broad masses, all varying
// along the face.
inline float rockShape(const Generator& g, float x, float y, float z) {
    float b = g.bedCoord(x, y, z);
    float bed = std::floor(b);
    // Each bed's own offset, eased into from the one below over the bottom fifth of the bed, so a shelf
    // has an underside that slopes back into the face rather than a knife edge one voxel thick.
    float here = hashUnit(int(bed), 7, g.seed + 911u) * 2.0f - 1.0f;
    float below = hashUnit(int(bed) - 1, 7, g.seed + 911u) * 2.0f - 1.0f;
    float bedTerm = below + (here - below) * smoothstep(0.0f, 0.22f, b - bed);
    // ...and how far it does so varies along the face: a shelf runs out, another starts.
    float along = g.rock3(x / 70.0f, bed * 0.37f, z / 70.0f) * 2.0f - 1.0f;
    // Masses large against a voxel. A finer octave (~15 units) made the face hairy: single-voxel
    // spikes and pits everywhere, which read as noise rather than as rock breaking in pieces.
    float mass = (g.rock3(x / 44.0f, y / 30.0f, z / 44.0f) * 0.8f +
                  g.rock3(x / 24.0f, y / 18.0f, z / 24.0f) * 0.2f) * 2.0f - 1.0f;
    // The beds carry most of it: shelves of rock standing out over undercuts are what a cliff's 3D
    // shape mostly is.
    float v = 0.65f * bedTerm * (0.55f + 0.45f * along) + 0.2f * along + 0.35f * mass;
    return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

// The terrain's density at a point, given the column's smooth height and its rock reach. Grain is
// keyed on the nearest ring's voxel so every ring sees the same roughness at the same place.
inline float terrainDensity(const Generator& g, float x, float y, float z, float h, float reach,
                            float nearVoxel) {
    float d = h - y;
    if (reach <= 0.0f) return d;
    int ix = int(std::floor(x / nearVoxel)), iy = int(std::floor(y / nearVoxel)), iz = int(std::floor(z / nearVoxel));
    float grain = float(hashColumn(ix * 7919 + iy, iz - iy * 131, g.seed + 101u) >> 8) * (2.0f / 16777216.0f) - 1.0f;
    return d + reach * rockShape(g, x, y, z) + grain * kRockGrain * (reach / kOverhang);
}

struct Boulder {
    float x, y, z;        // centre, world units
    float r;              // radius
    float squash;         // vertical radius as a fraction of r
    uint32_t id;
};

// Every boulder whose centre falls in the given world XZ rectangle (callers widen it by the largest
// reach). Sits each on the smooth ground, a third to half buried.
inline void collectBoulders(const Generator& g, float minX, float maxX, float minZ, float maxZ,
                            std::vector<Boulder>& out) {
    int x0 = int(std::floor(minX / kBoulderCell)), x1 = int(std::floor(maxX / kBoulderCell));
    int z0 = int(std::floor(minZ / kBoulderCell)), z1 = int(std::floor(maxZ / kBoulderCell));
    for (int cz = z0; cz <= z1; ++cz) {
        for (int cx = x0; cx <= x1; ++cx) {
            float roll = hashUnit(cx, cz, g.seed + 1201u);
            if (roll > kBoulderChance + kBoulderTalus) continue;   // the most any cell can have
            float x = (float(cx) + 0.15f + 0.7f * hashUnit(cx, cz, g.seed + 1203u)) * kBoulderCell;
            float z = (float(cz) + 0.15f + 0.7f * hashUnit(cx, cz, g.seed + 1205u)) * kBoulderCell;
            if (x < minX || x > maxX || z < minZ || z > maxZ) continue;
            float h = g.heightAt(x, z);
            auto slopeAt = [&](float px, float pz) {
                float sx = g.heightAt(px + 6.0f, pz) - g.heightAt(px - 6.0f, pz);
                float sz = g.heightAt(px, pz + 6.0f) - g.heightAt(px, pz - 6.0f);
                return std::sqrt(sx * sx + sz * sz) / 12.0f;
            };
            // Resting on gentle ground...
            if (slopeAt(x, z) > kBoulderMaxSlope) continue;
            // ...and most likely at the foot of rock: the rockiest ground a short way off, in any of
            // four directions.
            float talus = 0.0f;
            const float probe = 45.0f;
            talus = std::max(talus, stoneness(slopeAt(x + probe, z)));
            talus = std::max(talus, stoneness(slopeAt(x - probe, z)));
            talus = std::max(talus, stoneness(slopeAt(x, z + probe)));
            talus = std::max(talus, stoneness(slopeAt(x, z - probe)));
            if (roll > kBoulderChance + kBoulderTalus * talus) continue;
            uint32_t id = hashColumn(cx, cz, g.seed + 1207u);
            float size = float(id & 0xFFu) / 255.0f;
            float r = kBoulderRMin + (kBoulderRMax - kBoulderRMin) * size * size;   // mostly small
            float squash = 0.6f + 0.35f * float((id >> 8) & 0xFFu) / 255.0f;
            float buried = 0.3f + 0.25f * float((id >> 16) & 0xFFu) / 255.0f;
            out.push_back({x, h + r * squash * (1.0f - 2.0f * buried), z, r, squash, id});
        }
    }
}

// A boulder's density at a point, in world units: positive inside. Lumpy -- the radius is warped by a
// noise at about half the boulder's size, so no two are the same ellipsoid -- and faceted a little by
// a coarse noise, which reads as the planes a weathered stone breaks along.
inline float boulderDensity(const Generator& g, const Boulder& b, float x, float y, float z) {
    float dx = (x - b.x) / b.r, dy = (y - b.y) / (b.r * b.squash), dz = (z - b.z) / b.r;
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > kBoulderLump * kBoulderLump) return -1.0f;
    float d = std::sqrt(d2);
    float off = float(b.id & 0xFFFFu) * 0.013f;
    float lump = (g.rock3(dx * 1.6f + off, dy * 1.6f, dz * 1.6f - off) - 0.5f) * 0.55f;
    float facet = (g.rock3(dx * 0.7f - off, dy * 0.7f + off, dz * 0.7f) - 0.5f) * 0.3f;
    return (1.0f - d + lump + facet) * b.r;
}

// --- Colours ------------------------------------------------------------------------------------
//
// Small ramps, hue-shifted like the trees' (cool shadow, warm light), so the ground sits with them.
struct RGB { float r, g, b; };
constexpr int kGrassShades = 4;
// Darker and less yellow than a "grass green" picked on its own: under the sun the light steps read
// lime, and next to the conifers' blue-greens a meadow should sit back, not shout.
constexpr RGB kGrass[kGrassShades] = {
    {0.12f, 0.24f, 0.10f}, {0.17f, 0.31f, 0.12f}, {0.23f, 0.37f, 0.13f}, {0.31f, 0.43f, 0.16f}};
// Rock: two hue-shifted ramps -- cool blue-grey and warm brown-grey, each shadow-cool to light-warm
// -- so a face can drift between them by region, plus the breaks and the growth on it.
//
// **Seven steps, close together.** Five steps a tenth of the range apart made every neighbouring
// voxel a visible jump, and a face read as a mosaic of grey cubes. Real rock -- and Teardown's,
// whose palettes are sampled from photographs -- varies a great deal over a face and very little
// from one grain to the next; finer steps, with the variation carried by the coherent noises below
// rather than by the dither, is that.
constexpr int kRockShades = 7;
constexpr RGB rampStep(RGB a, RGB b, int i) {
    float t = float(i) / float(kRockShades - 1);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
constexpr RGB kCoolDark = {0.20f, 0.21f, 0.24f}, kCoolLight = {0.58f, 0.57f, 0.55f};
constexpr RGB kWarmDark = {0.24f, 0.21f, 0.19f}, kWarmLight = {0.63f, 0.59f, 0.52f};
constexpr RGB kRockCool[kRockShades] = {
    rampStep(kCoolDark, kCoolLight, 0), rampStep(kCoolDark, kCoolLight, 1), rampStep(kCoolDark, kCoolLight, 2),
    rampStep(kCoolDark, kCoolLight, 3), rampStep(kCoolDark, kCoolLight, 4), rampStep(kCoolDark, kCoolLight, 5),
    rampStep(kCoolDark, kCoolLight, 6)};
constexpr RGB kRockWarm[kRockShades] = {
    rampStep(kWarmDark, kWarmLight, 0), rampStep(kWarmDark, kWarmLight, 1), rampStep(kWarmDark, kWarmLight, 2),
    rampStep(kWarmDark, kWarmLight, 3), rampStep(kWarmDark, kWarmLight, 4), rampStep(kWarmDark, kWarmLight, 5),
    rampStep(kWarmDark, kWarmLight, 6)};
// Crusts are muted, the way they are on real stone: they tint a patch rather than paint it.
constexpr RGB kLichenY = {0.47f, 0.47f, 0.33f};   // pale grey-green crust
constexpr RGB kLichenR = {0.49f, 0.38f, 0.27f};   // rust crust
constexpr RGB kMoss    = {0.20f, 0.26f, 0.14f};

// Which rock entry a voxel is. Indices into a table the caller interns: the cool ramp, the warm ramp,
// then the two lichens and moss.
enum RockSlot : int { RockCool = 0, RockWarm = kRockShades, RockLichenY = 2 * kRockShades,
                      RockLichenR, RockMoss, RockSlotCount };

// The colour of the rock at a world position. `top` is true for the voxel a stone column's surface
// is (the one facing up); `voxel` is the size of the voxel being coloured, so a coarse ring does not
// draw cracks thinner than one of its own voxels as noise.
inline int rockSlot(const Generator& g, float x, float y, float z, bool top, float voxel) {
    int ix = int(std::floor(x / 2.8f)), iy = int(std::floor(y / 2.8f)), iz = int(std::floor(z / 2.8f));
    float grain = hashUnit(ix * 7919 + iy, iz - iy * 131, g.seed + 307u);

    float b = g.bedCoord(x, y, z);
    float inBed = b - std::floor(b);

    // Growth on the faces that catch the light: lichen crusts in a few broad patches, moss in the
    // low damp ones. Patches, not speckle: a crust is a colony, and colonies are tens of voxels.
    if (top) {
        float crust = g.rock3(x / 24.0f, 9.1f, z / 24.0f);
        if (crust > 0.74f && grain < 0.8f) return g.rock3(x / 60.0f, 2.2f, z / 60.0f) < 0.5f ? RockLichenY : RockLichenR;
        float damp = g.rock3(x / 36.0f, 4.3f, z / 36.0f);
        if (damp > 0.73f && y < kMinHeight + kRelief * 0.45f) return RockMoss;
    }

    // Tone, built from coherent structure, largest first -- layered Voronoi and noise into a ramp,
    // the way procedural rock is built in Blender, Houdini and Cinema 4D:
    //   blocks: 3D Worley cells, wider than tall (rock joints along its beds), each its own tone;
    //   joints: the boundaries between blocks, a continuous dark line; the rim just inside a
    //   block's edge a touch lighter (weathered, catching light);
    //   each bed its own tone, and a broad mottle across all of it;
    //   the bed's top edge lighter (a ledge's lip catches the sun), and tops a little bleached;
    //   on side faces, vertical streaks -- water running down a cliff stains it in long dark
    //   streaks, and nothing says "cliff" faster;
    //   then a dither of well under one step, which only breaks the boundaries between steps.
    Cells blocks = worley3(x / 38.0f, y / 20.0f, z / 38.0f, g.seed + 503u);
    float blockTone = float(blocks.id >> 24) / 255.0f - 0.5f;
    float edge = blocks.f2 - blocks.f1;
    float bedTone = hashUnit(int(std::floor(b)), 0, g.seed + 401u) - 0.5f;
    float mottle = g.rock3(x / 48.0f, y / 48.0f, z / 48.0f) - 0.5f;
    float v = 0.50f + 0.24f * blockTone + 0.12f * bedTone + 0.45f * mottle
            + 0.05f * smoothstep(0.6f, 1.0f, inBed) + (top ? 0.06f : 0.0f)
            + (grain - 0.5f) * 0.08f;
    if (voxel < 4.0f) {
        if (edge < 0.03f) v -= 0.30f;                    // the joint itself
        else if (edge < 0.08f) v += 0.06f;               // the weathered rim
    }
    if (!top) {
        float streak = g.rock3(x / 5.5f, y / 46.0f, z / 5.5f);
        v -= 0.20f * smoothstep(0.58f, 0.80f, streak);
        // Bedding planes: the base of some beds is a recessed line in shadow -- darker steps of the
        // rock's own ramp, not a separate black, which on a stepped face only ever showed as
        // scattered single voxels and read as holes. Side faces only, and only where the voxel is
        // fine enough to draw a line.
        if (voxel < 4.0f && inBed < 0.12f &&
            g.rock3(x / 46.0f, std::floor(b) * 0.71f, z / 46.0f) > 0.55f) {
            v -= 0.26f;
        }
    }
    int shade = int(v * float(kRockShades));
    shade = shade < 0 ? 0 : (shade >= kRockShades ? kRockShades - 1 : shade);

    // Region: warm or cool rock, with a dithered border between them.
    float warmth = g.rock3(x / 220.0f, y / 220.0f, z / 220.0f) + (grain - 0.5f) * 0.12f;
    return (warmth > 0.5f ? RockWarm : RockCool) + shade;
}
constexpr RGB kDirt = {0.30f, 0.22f, 0.15f};
constexpr int kBladeShades = 3;
constexpr RGB kBlade[kBladeShades] = {
    {0.15f, 0.30f, 0.11f}, {0.23f, 0.40f, 0.13f}, {0.34f, 0.49f, 0.17f}};

}  // namespace simple_terrain

#endif
