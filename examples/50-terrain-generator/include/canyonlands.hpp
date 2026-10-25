// Canyonlands: the Colorado Plateau's rock faces, as one 3D model (docs/plans/biome-canyonlands.md).
//
// **The model.** Every column has `e`, how far it stands into the plateau from the drainage below it.
// The rock at height y stands back from the drainage by a setback w(y); a voxel is solid where
//
//     e > w(x, y)
//
// w is built by walking up the strata, each unit adding setback at its own slope, so a cliff-former
// adds almost none (a wall) and a slope-former a lot. Detail is added to w per face type -- always as
// setback, so a face can be pushed back but the stratum's outline never moves forward of its base --
// and where w shrinks going up, the rock overhangs: an undercut base, a spall scar's ceiling, a hard
// Kayenta bed over a soft one. A heightfield cannot say any of that.
//
// Bottom to top (heights above the wash, world units; the nearest voxel is 2.8):
//
//   F4 Chinle    0..100     a slope, concave (gentle toe, ~40 deg under the wall), cut by gullies
//                           that start at the wall's pour-off chutes and by rills between them;
//                           banded purple / red / grey-green, mostly buried in red debris off the wall
//   F1 Wingate   100..340   a wall: jointed slabs, each set back as a unit and stacked in spall scars
//                           with overhanging ceilings, joint cracks, an undercut base, chutes where
//                           water pours over the rim; red-orange, varnish streaks under the chutes
//   F2 Kayenta   340..400   ledges: hard beds standing out with flat tops over recessed soft ones;
//                           maroon to lavender; then a bench
//   F3 Navajo    400+       domes of cross-bedded sandstone on the bench, cracked in a checkerboard by
//                           joints and laminae; salmon at the base, cream above
//
// Chinle and Navajo are heightfields of `e` (nothing there overhangs); Wingate and Kayenta use the
// setback test. Everything is a pure function of world position, so chunks agree at their seams.
//
// The lab (TERRAIN_LAB=faces) gives `e` as a straight escarpment along X with slow embayments; the
// terrain (C3) will give it from a drainage network.
#ifndef CANYONLANDS_HPP
#define CANYONLANDS_HPP

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>
#include <vector>

#include "simple_terrain.hpp"

namespace canyonlands {

using simple_terrain::Cells;
using simple_terrain::Generator;
using simple_terrain::RGB;
using simple_terrain::hashUnit;
using simple_terrain::smoothstep;
using simple_terrain::worley2;

// --- Strata ---------------------------------------------------------------------------------------
constexpr float kChinleTop  = 100.0f;   // heights above the wash
constexpr float kWingateTop = 340.0f;
constexpr float kKayentaTop = 400.0f;

// Setbacks, in e.
constexpr float kWallFoot     = 170.0f;  // where the Chinle slope meets the wall
constexpr float kWallBatter   = 0.06f;   // the wall leans back this much per unit up: ~87 deg
constexpr float kUndercut     = 8.0f;    // how far the wall's base is cut back over the Chinle
constexpr float kUndercutRise = 16.0f;
constexpr float kKayentaStep  = 3.0f;    // the Kayenta starts this far back from the Wingate rim
constexpr float kKayentaRun   = 0.45f;   // and leans back at ~65 deg overall
constexpr float kBenchMin     = 6.0f;    // bare Kayenta bench before the Navajo starts: from almost
constexpr float kBenchMax     = 70.0f;   // none (the Navajo rises straight off the rim) to a wide shelf

// Detail sizes.
constexpr float kJoint        = 46.0f;   // Wingate joint spacing along the wall (~16 voxels)
constexpr float kSlabSetback  = 10.0f;   // most a slab or spall scar is set back
constexpr float kCrack        = 4.0f;    // joint crack depth
constexpr float kChuteSpacing = 125.0f;  // pour-offs along the rim
constexpr float kChuteDepth   = 13.0f;
constexpr float kGullyDepth   = 15.0f;
constexpr float kBulge        = 22.0f;   // most a stack of slabs stands out or sits back
constexpr float kAlcoveSpacing = 170.0f;
constexpr float kAlcove       = 24.0f;   // alcove depth

inline float wallRim() { return kWallFoot + kWallBatter * (kWingateTop - kChinleTop); }
inline float kayentaBase(float y) { return wallRim() + kKayentaStep + kKayentaRun * (y - kWingateTop); }
inline float navajoStart(const Generator& g, float x) {
    float t = g.rock3(x / 230.0f, 2.6f, 7.7f);
    return kayentaBase(kKayentaTop) + kBenchMin + (kBenchMax - kBenchMin) * smoothstep(0.25f, 0.75f, t);
}

// --- Palette -------------------------------------------------------------------------------------
enum Slot : uint8_t {
    Sand = 0,                      // 2: wash sand, and sand drifted onto ledges
    Chinle = 2,                    // 5: the bands
    Wingate = 7,                   // 6: dark .. light
    WingateFresh = 13,             // 2: a recent spall scar, pale and unvarnished
    Varnish = 15,                  // 3: black .. brown
    Kayenta = 18,                  // 6: maroon, lavender (soft) .. red-brown (hard)
    Navajo = 24,                   // 6: tan .. cream
    NavajoBand = 30,               // 3: salmon
    Lichen = 33,
    SandRed = 34,                  // 1: the open floor's deeper red
    Crust = 35,                    // 2: cryptobiotic soil, dark and lumpy between the grass
    GrassLive = 37,                // 3: base .. tip
    GrassDead = 40,                // 3: base .. tip, cured straw
    SlotCount = 43
};

inline const RGB* slotColours() {
    static const RGB colours[SlotCount] = {
        {0.66f, 0.45f, 0.32f}, {0.72f, 0.54f, 0.40f},
        // Chinle: warm, muted. Grey-blue bands read as blue under a blue sky, so even the greys lean warm.
        {0.50f, 0.34f, 0.36f}, {0.62f, 0.40f, 0.31f}, {0.68f, 0.62f, 0.54f}, {0.56f, 0.53f, 0.44f}, {0.58f, 0.45f, 0.47f},
        {0.36f, 0.15f, 0.09f}, {0.48f, 0.21f, 0.12f}, {0.60f, 0.29f, 0.16f}, {0.70f, 0.37f, 0.21f}, {0.78f, 0.47f, 0.29f}, {0.85f, 0.58f, 0.40f},
        {0.86f, 0.55f, 0.36f}, {0.92f, 0.66f, 0.47f},
        {0.20f, 0.11f, 0.08f}, {0.28f, 0.16f, 0.11f}, {0.36f, 0.21f, 0.14f},
        {0.30f, 0.16f, 0.15f}, {0.42f, 0.27f, 0.29f}, {0.50f, 0.36f, 0.38f}, {0.46f, 0.24f, 0.19f}, {0.56f, 0.32f, 0.24f}, {0.64f, 0.42f, 0.32f},
        {0.60f, 0.42f, 0.30f}, {0.71f, 0.52f, 0.37f}, {0.80f, 0.62f, 0.46f}, {0.86f, 0.71f, 0.55f}, {0.90f, 0.79f, 0.64f}, {0.93f, 0.86f, 0.73f},
        {0.74f, 0.40f, 0.25f}, {0.82f, 0.49f, 0.31f}, {0.88f, 0.58f, 0.39f},
        {0.80f, 0.50f, 0.15f},
        {0.60f, 0.36f, 0.24f},
        {0.22f, 0.18f, 0.15f}, {0.30f, 0.25f, 0.20f},
        {0.30f, 0.36f, 0.16f}, {0.42f, 0.48f, 0.22f}, {0.56f, 0.60f, 0.30f},
        {0.55f, 0.47f, 0.32f}, {0.68f, 0.60f, 0.42f}, {0.80f, 0.73f, 0.55f},
    };
    return colours;
}

// --- Joints and chutes ---------------------------------------------------------------------------
// Joint boundaries along X: b_j = (j + jitter) * kJoint. The wall is a row of slabs between them.
inline float jointBoundary(int j) { return (float(j) + 0.35f * (hashUnit(j, 3, 401u) - 0.5f)) * kJoint; }

// Which slab x is on, how far to its nearest joint, and both its boundaries.
struct Joint { int segment; float distance; float left, right; };
inline Joint jointAt(float x) {
    int j = int(std::floor(x / kJoint));
    float b0 = jointBoundary(j), b1 = jointBoundary(j + 1);
    if (x < b0) { b1 = b0; b0 = jointBoundary(j - 1); j -= 1; }
    else if (x >= b1) { b0 = b1; b1 = jointBoundary(j + 2); j += 1; }
    return {j, std::min(x - b0, b1 - x), b0, b1};
}

// **Joints wander.** A joint is a fracture through rock that is never quite uniform, so it bends a
// little as it goes down the wall: x is warped by two octaves of noise along the height before any
// joint, slab edge or chute is looked up. Ruled-straight cracks read as drawn on.
inline float wallWarp(const Generator& g, float x, float y) {
    return x + (g.rock3(x / 60.0f, y / 45.0f, 1.7f) - 0.5f) * 13.0f
             + (g.rock3(x / 17.0f, y / 11.0f, 2.9f) - 0.5f) * 4.5f;
}

// **And they split.** Water and frost work a crack open and it forks: off each joint, one to three
// branches leave at a slant and run down the face for a slab's height or so, thinning out, and about
// half of them fork again, the other way, smaller still. Subtle -- a few units deep -- so the joints
// stay the structure and the branches are texture. Returns the extra setback at (xw, y), xw warped.
inline float crackBranches(const Joint& jt, float xw, float y) {
    float depth = 0.0f;
    const float sides[2] = {jt.left, jt.right};
    for (int side = 0; side < 2; ++side) {
        const float b = sides[side];
        const int jb = int(std::lround(b / kJoint));
        const int count = 1 + int(hashUnit(jb, 31, 481u) * 2.99f);
        for (int m = 0; m < count; ++m) {
            const float y0 = kChinleTop + 10.0f + hashUnit(jb * 7 + m, 32, 481u) * (kWingateTop - kChinleTop - 20.0f);
            const float length = 30.0f + 60.0f * hashUnit(jb * 7 + m, 33, 481u);
            if (y > y0 || y < y0 - length) continue;
            // Into the slab beside this joint: away from it on the side xw is.
            const float dir = side == 0 ? 1.0f : -1.0f;
            const float slant = 0.25f + 0.35f * hashUnit(jb * 7 + m, 34, 481u);
            const float down = y0 - y;
            const float s = down / length;
            float d = std::abs(xw - (b + dir * down * slant));
            if (d < 1.3f) depth = std::max(depth, 3.0f * (1.0f - s) * (1.0f - d / 1.3f));
            // The fork: from halfway down, back the other way, half as long and shallower.
            if (hashUnit(jb * 7 + m, 35, 481u) < 0.55f && s > 0.45f) {
                const float fy = y0 - length * 0.45f;
                const float fx = b + dir * length * 0.45f * slant;
                const float fdown = fy - y;
                const float fs = fdown / (length * 0.5f);
                if (fs <= 1.0f) {
                    float fd = std::abs(xw - (fx - dir * fdown * slant * 0.9f));
                    if (fd < 1.1f) depth = std::max(depth, 2.2f * (1.0f - fs) * (1.0f - fd / 1.1f));
                }
            }
        }
    }
    return depth;
}

// Pour-offs: every so often along the rim water leaves the plateau, always down a joint (it is the
// weakness it finds). Returns the chute's x, snapped to a joint; false if cell k has none.
inline bool chuteX(int k, float& x) {
    if (hashUnit(k, 1, 409u) > 0.8f) return false;
    float raw = (float(k) + 0.2f + 0.6f * hashUnit(k, 2, 409u)) * kChuteSpacing;
    x = jointBoundary(int(std::lround(raw / kJoint)));
    return true;
}

// --- F4: Chinle ----------------------------------------------------------------------------------
// The slope's height above the wash at (x, e), gullies and rills cut in. `incision` reports how deep
// the gullies are here (for colour: their floors are sand, their walls show the bands).
inline float chinleHeight(const Generator& g, float x, float e, float* incision = nullptr) {
    if (e <= 0.0f) { if (incision) *incision = 0.0f; return 0.0f; }
    const float t = std::min(1.0f, e / kWallFoot);
    // Concave: a gentle toe steepening toward the wall, where the fresh debris lies.
    float h = kChinleTop * (0.55f * t + 0.45f * t * t);
    // Gullies from the chutes, meandering more the further they run from the wall, widening and
    // fanning out at the toe. Anchored at the chute, so the cut in the wall continues down the slope.
    float cut = 0.0f;
    int k0 = int(std::floor(x / kChuteSpacing));
    for (int k = k0 - 1; k <= k0 + 1; ++k) {
        float cx;
        if (!chuteX(k, cx)) continue;
        float meander = (g.rock3(cx * 0.013f, e / 70.0f, 5.5f) - 0.5f) * 2.0f * 26.0f * (1.0f - t);
        float width = 11.0f + 24.0f * (1.0f - t);
        float dx = (x - cx - meander) / width;
        if (std::abs(dx) >= 1.0f) continue;
        float depth = kGullyDepth * smoothstep(0.0f, 0.35f, t) * (0.65f + 0.35f * t);
        cut = std::max(cut, depth * (1.0f - dx * dx));
    }
    // Rills between: furrows down the fall line, a few units deep, fading out at the toe.
    float r = g.rock3(x / 9.0f, e / 90.0f, 2.0f);
    float rill = (1.0f - std::abs(2.0f * r - 1.0f)) * 3.5f * smoothstep(0.1f, 0.5f, t);
    // The badland surface: mudstone weathers to a crumbly, popcorn crust.
    float crust = (g.rock3(x / 3.5f, e / 3.5f, 7.1f) - 0.5f) * 1.6f;
    if (incision) *incision = cut;
    return std::max(0.0f, h - cut - rill + crust * t);
}

// --- F1: Wingate ---------------------------------------------------------------------------------
// The wall's setback at (x, y), detail included. `fresh` says the voxel is in a deep spall scar.
inline float wingateSetback(const Generator& g, float x, float y, bool* fresh = nullptr, float* chute = nullptr) {
    const float above = y - kChinleTop;
    float w = kWallFoot + kWallBatter * above;
    // The undercut: the soft Chinle weathers back under the wall's base.
    w += kUndercut * (1.0f - smoothstep(0.0f, kUndercutRise, above)) * (0.55f + 0.45f * g.rock3(x / 70.0f, 3.3f, 1.9f));
    // Slabs: each joint segment is a column of stacked slabs, rows of its own height, each set back
    // as a unit. A slab standing proud above one set back makes a flat ceiling: a spall scar.
    const float xw = wallWarp(g, x, y);
    Joint jt = jointAt(xw);
    const int j = jt.segment;
    float rowHeight = 40.0f + 55.0f * hashUnit(j, 8, 419u);
    int row = int(std::floor((above + hashUnit(j, 7, 419u) * 80.0f) / rowHeight));
    float s = hashUnit(j * 37 + row, 9, 421u);
    float slab = kSlabSetback * s * s;
    // **Bulges and recesses, built of slabs.** A coherent field over the wall, sampled at each slab's
    // centre so it moves whole slabs: stacks of them stand out as buttresses, others sit back, and
    // where the field changes with height a slab overhangs the one under it. Sampled per slab, not
    // per voxel, so the faces stay planes -- a smooth bulge would read as clay, not jointed rock.
    // Nothing stands forward near the base, where it would hang over the slope with nothing under it.
    const float slabX = 0.5f * (jt.left + jt.right);
    const float slabY = (float(row) + 0.5f) * rowHeight - hashUnit(j, 7, 419u) * 80.0f;
    float bulge = (g.rock3(slabX / 140.0f, slabY / 95.0f, 6.1f) - 0.5f) * 2.0f * kBulge * 1.3f
                + (g.rock3(slabX / 60.0f, slabY / 50.0f, 6.9f) - 0.5f) * 2.0f * kBulge * 0.4f;
    bulge = std::clamp(bulge, -kBulge, kBulge);
    if (bulge < 0.0f) bulge *= smoothstep(10.0f, 60.0f, above);
    slab += bulge;
    w += slab;
    if (fresh) *fresh = s * s > 0.45f;
    // Joint cracks, and the cracks branching off them.
    if (jt.distance < 1.6f) w += kCrack * (1.0f - jt.distance / 1.6f);
    w += crackBranches(jt, xw, y);
    // Chutes: a groove down the whole wall where water pours over.
    float chuteCut = 0.0f;
    int k0 = int(std::floor(x / kChuteSpacing));
    for (int k = k0 - 1; k <= k0 + 1; ++k) {
        float cx;
        if (!chuteX(k, cx)) continue;
        float dx = (xw - cx) / 11.0f;
        if (std::abs(dx) < 1.0f) chuteCut = std::max(chuteCut, kChuteDepth * (1.0f - dx * dx));
    }
    w += chuteCut;
    if (chute) *chute = chuteCut / kChuteDepth;
    // Alcoves: arched hollows where a slab sheet has fallen out of the wall -- a round-topped recess
    // with a flat floor, its ceiling overhanging. One every few hundred units.
    {
        int k = int(std::floor(xw / kAlcoveSpacing));
        for (int a = k - 1; a <= k + 1; ++a) {
            if (hashUnit(a, 41, 487u) > 0.5f) continue;
            float xa = (float(a) + 0.5f + 0.4f * (hashUnit(a, 42, 487u) - 0.5f)) * kAlcoveSpacing;
            float width = 28.0f + 32.0f * hashUnit(a, 43, 487u);
            float springLine = kChinleTop + 35.0f + 150.0f * hashUnit(a, 44, 487u);
            float rise = 25.0f + 40.0f * hashUnit(a, 45, 487u);
            float drop = 12.0f + 18.0f * hashUnit(a, 46, 487u);
            float dx = (xw - xa) / width;
            if (std::abs(dx) >= 1.0f || y < springLine - drop) continue;
            float dy = y > springLine ? (y - springLine) / rise : 0.0f;
            float r = dx * dx + dy * dy;
            if (r < 1.0f) {
                float cut = kAlcove * (1.0f - r * r) * smoothstep(0.0f, 6.0f, y - (springLine - drop));
                w += cut;
                if (fresh && cut > kAlcove * 0.3f) *fresh = true;
            }
        }
    }
    // Grain.
    w += (g.rock3(x / 5.0f, y / 5.0f, 3.7f) - 0.5f) * 1.4f;
    return w;
}
constexpr float kWingateDetailMax = kUndercut + kSlabSetback + kBulge + kCrack + 3.0f + kChuteDepth + kAlcove + 1.0f;
constexpr float kWingateDetailMin = -kBulge - 1.0f;

// --- F2: Kayenta ---------------------------------------------------------------------------------
// Beds ~7-13 units thick. A hard bed stands out (negative setback) with a flat top, its edge broken
// back in blocks along the cliff; a soft bed is recessed and slopes back as it rises, so the hard bed
// above overhangs it. `hard` and `bed` report which, for colour.
inline float kayentaSetback(const Generator& g, float x, float y, bool* hard = nullptr, int* bedOut = nullptr) {
    float b = (y - kWingateTop) / 10.0f + (g.rock3(x / 260.0f, 5.1f, 0.3f) - 0.5f) * 0.6f;
    int bed = int(std::floor(b));
    float f = b - float(bed);
    bool isHard = hashUnit(bed, 11, 431u) < 0.55f;
    float w = kayentaBase(y);
    if (isHard) {
        int block = int(std::floor((x + 20.0f * float(bed)) / (30.0f + 30.0f * hashUnit(bed, 12, 431u))));
        float broken = hashUnit(bed * 53 + block, 13, 433u);
        w += -5.0f + (broken > 0.7f ? 7.0f : broken * 2.0f);
    } else {
        w += 2.0f + 6.0f * f;
    }
    // The chutes cut the rim too.
    int k0 = int(std::floor(x / kChuteSpacing));
    for (int k = k0 - 1; k <= k0 + 1; ++k) {
        float cx;
        if (!chuteX(k, cx)) continue;
        float dx = (wallWarp(g, x, y) - cx) / 9.0f;
        if (std::abs(dx) < 1.0f) w += 8.0f * (1.0f - dx * dx);
    }
    w += (g.rock3(x / 4.0f, y / 4.0f, 8.3f) - 0.5f) * 1.2f;
    if (hard) *hard = isHard;
    if (bedOut) *bedOut = bed;
    return w;
}
constexpr float kKayentaDetailMin = -6.0f, kKayentaDetailMax = 18.0f;

// --- F3: Navajo ---------------------------------------------------------------------------------
// One mass, not a row of domes: it rises off the bench in a steep, rounding front -- straight off the
// Kayenta rim where the bench pinches out, so the cliff carries on up into it -- and is broken into
// domes by clefts along warped joint lines. Each dome is its own size and height, rounded on top,
// lumpy, and the clefts between them are deep but floored, so neighbours merge at their bases.
// Its height above the Kayenta top; -1 off the Navajo.
constexpr float kNavajoFront = 110.0f;   // how far back the front takes to round over

// `along` is the wall's own coordinate (see the network below); the domes are laid out on the
// world's (x, z), so they do not care which way the wall runs.
inline float navajoHeight(const Generator& g, float along, float x, float z, float e, float* joint = nullptr) {
    const float start = navajoStart(g, along);
    if (e < start) { if (joint) *joint = 1.0f; return -1.0f; }
    // The front: steep (~70 deg) off the bench, rounding over to the mass's own height.
    const float t = std::min(1.0f, (e - start) / kNavajoFront);
    const float front = 1.0f - std::pow(1.0f - t, 2.5f);
    const float mass = 115.0f + 70.0f * g.rock3(x / 320.0f, 4.4f, z / 320.0f);
    // Domes: Worley cells on warped coordinates, so they come out irregular, of uneven size.
    const float wx = x + (g.rock3(x / 170.0f, z / 170.0f, 3.3f) - 0.5f) * 90.0f;
    const float we = z + (g.rock3(x / 170.0f, z / 170.0f, 8.8f) - 0.5f) * 90.0f;
    // Two scales: big massifs, and smaller knobs on and between them. Only some cell edges are cut
    // into clefts -- elsewhere neighbours merge into one ridge -- or it is a tray of buns.
    auto domes = [&](float size, uint32_t seed) {
        Cells c = worley2(wx / size, we / (size * 0.85f), seed);
        const float scale = 0.55f + 0.55f * float(c.id >> 24) / 255.0f;
        const float dome = std::pow(std::max(0.0f, 1.0f - c.f1 * c.f1 * 1.2f), 0.6f);
        const float edge = smoothstep(0.0f, 0.25f, c.f2 - c.f1);
        const float cutHere = smoothstep(0.42f, 0.62f, g.rock3(wx / (size * 0.6f), we / (size * 0.6f), float(seed % 97u)));
        const float cleft = 1.0f - cutHere * (1.0f - std::pow(edge, 0.7f));
        return dome * cleft * scale;
    };
    const float shape = std::max(domes(330.0f, 453u), 0.6f * domes(120.0f, 457u));
    float top = mass * front * (0.25f + 0.75f * shape);
    // Lumps: the domes are weathered, not turned.
    top += (g.rock3(x / 38.0f, z / 38.0f, 5.5f) - 0.5f) * 18.0f * front
         + (g.rock3(x / 11.0f, z / 11.0f, 6.2f) - 0.5f) * 4.0f * front;
    // Joints: a polygonal crack network -- with the laminae, the checkerboard of Navajo slickrock.
    // Fine lines, a voxel deep: deeper or closer and the domes became a field of paving tiles.
    Cells j = worley2(x / 75.0f, z / 75.0f, 449u);
    float crack = j.f2 - j.f1;
    if (joint) *joint = crack;
    if (crack < 0.04f) top -= 1.6f * (1.0f - crack / 0.04f);
    // Pits where water stands, few and small, on the flattest tops.
    Cells p = worley2(x / 16.0f, z / 16.0f, 451u);
    if (p.f1 < 0.12f && top > 70.0f && (p.id & 3u) == 0u) top -= (0.12f - p.f1) * 14.0f;
    return std::max(0.0f, top);
}

// --- The drainage network (C3) ------------------------------------------------------------------
//
// `e` for the real terrain: how far a column stands into the plateau from the nearest canyon.
//
// **Canyons follow joints.** Real canyon country is cut along its joint sets: walls run in a couple of
// directions, meet at corners, and buttes are blocky rather than round. So the network is the edges of
// Worley cells measured with a *squarish* metric (mostly Chebyshev, a little Euclidean) in a frame turned
// to the region's joint direction -- edges come out straight and at a few angles -- with only a gentle
// warp to keep them from being ruled, and a small jagged one on top. (f2 - f1) * cell / 2 is near
// enough the distance to the nearest edge. Three scales, each cut to its own depth:
//
//   main canyons   down to the wash: Chinle slopes, the full wall, floors ~140 units wide
//   box canyons    into the Wingate only, floored on the Chinle's top -- side canyons that hang above
//                  the main ones; only where a noise mask allows
//   clefts         into the Navajo only, down to the Kayenta bench
//
// **And most of the desert is not plateau.** Over a large field the plateau has been stripped back
// entirely: open plains, ending at long escarpments, with isolated buttes standing on them (Monument
// Valley). A butte's size picks what caps it -- a small one is bare Wingate, a big one carries Kayenta
// and a Navajo dome.

// The joint frame's angle, turning slowly across the region.
inline float jointAngle(const Generator& g, float x, float z) {
    return 0.6f + 1.2f * (g.rock3(x / 9000.0f, 2.0f, z / 9000.0f) - 0.5f);
}

// Worley in the joint frame with a squarish metric. (u, v) in cell units.
inline Cells worleyJointed(float u, float v, uint32_t seed) {
    int cx = int(std::floor(u)), cz = int(std::floor(v));
    Cells c{1e9f, 1e9f, 0u};
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            int gx = cx + dx, gz = cz + dz;
            uint32_t h = simple_terrain::hashColumn(gx, gz, seed);
            float px = float(gx) + 0.15f + 0.7f * float(h & 0xFFu) / 255.0f - u;
            float pz = float(gz) + 0.15f + 0.7f * float((h >> 8) & 0xFFu) / 255.0f - v;
            float d = 0.7f * std::max(std::abs(px), std::abs(pz)) + 0.3f * std::sqrt(px * px + pz * pz);
            if (d < c.f1) { c.f2 = c.f1; c.f1 = d; c.id = h; }
            else if (d < c.f2) { c.f2 = d; }
        }
    return c;
}

struct Frame { float u, v; };
inline Frame jointFrame(const Generator& g, float x, float z, float warp, float scale, float k) {
    const float wx = x + (g.rock3(x / scale, z / scale, k) - 0.5f) * 2.0f * warp
                       + (g.rock3(x / 45.0f, z / 45.0f, k + 7.0f) - 0.5f) * 2.0f * warp * 0.05f;
    const float wz = z + (g.rock3(x / scale, z / scale, k + 31.0f) - 0.5f) * 2.0f * warp
                       + (g.rock3(x / 45.0f, z / 45.0f, k + 13.0f) - 0.5f) * 2.0f * warp * 0.05f;
    const float a = jointAngle(g, x, z);
    const float c = std::cos(a), s = std::sin(a);
    return {wx * c + wz * s, -wx * s + wz * c};
}

inline float networkDistance(const Generator& g, float x, float z, float cell, float warp, uint32_t seed) {
    Frame f = jointFrame(g, x, z, warp, cell * 0.6f, float(seed % 113u));
    Cells c = worleyJointed(f.u / cell, f.v / cell, seed);
    return (c.f2 - c.f1) * 0.5f * cell;
}

// --- The canyon network: a local, Dendry-style hierarchy ------------------------------------------
//
// Canyons are a drainage network -- long continuous trunks with tributaries coming into them, and
// smaller ones into those -- but built the way Dendry builds dendritic patterns (Gaillard et al.,
// "Dendry: A Procedural Model for Dendritic Patterns", I3D 2019): **as a pure function of position**,
// from jittered points at a few levels, each looked up in a bounded neighbourhood. No flow simulation,
// no global state: any chunk evaluates alone, the world can be infinite, and the result is one more
// field a biome blend can weight.
//
//   level 0  trunks       points every 2400 units; each joins the lowest of its eight neighbours on
//                         the control field (a low-frequency elevation), so trunks run long and
//                         continuous downhill, toward the basins; cut to the wash, as narrow as a
//                         young gorge or as wide as a mature valley, by region (`maturity`)
//   level 1  tributaries  points every 900; each joins the nearest point on a trunk -- about half cut
//                         deep and narrow, half box canyons hanging on the Chinle
//   level 2  side canyons points every 330; each joins the nearest level 0 or 1 segment -- mostly
//                         clefts into the Navajo, some boxes
//
// Segments bend once or twice (midpoint displacement), so a canyon wanders between straight reaches.
// Where the control field is lowest the plateau is gone altogether: basins, old lake beds and inland
// flats, with isolated buttes standing in them. The trunks run down into them, following the same field.

inline float controlField(const Generator& g, float x, float z) {
    return g.rock3(x / 14000.0f, 1.1f, z / 14000.0f) + 0.35f * g.rock3(x / 4500.0f, 2.2f, z / 4500.0f)
         + 0.12f * g.rock3(x / 1500.0f, 3.3f, z / 1500.0f);
}
constexpr float kBasinLevel = 0.62f;     // control below this: basin floor
inline float maturity(const Generator& g, float x, float z) { return g.rock3(x / 12000.0f, 4.4f, z / 12000.0f); }

struct Segment { float ax, az, bx, bz; float halfWidth, offset; };

inline float segmentDistance(const Segment& s, float x, float z) {
    float dx = s.bx - s.ax, dz = s.bz - s.az;
    float t = ((x - s.ax) * dx + (z - s.az) * dz) / std::max(1e-6f, dx * dx + dz * dz);
    t = std::clamp(t, 0.0f, 1.0f);
    float px = s.ax + dx * t - x, pz = s.az + dz * t - z;
    return std::sqrt(px * px + pz * pz);
}

namespace detail {
constexpr float kSpacing[3] = {2400.0f, 900.0f, 330.0f};
inline void point(const Generator& g, int level, int i, int k, float& x, float& z) {
    const uint32_t salt = 701u + uint32_t(level) * 17u + g.seed * 131u;
    x = (float(i) + 0.15f + 0.7f * hashUnit(i, k, salt)) * kSpacing[level];
    z = (float(k) + 0.15f + 0.7f * hashUnit(i, k, salt + 3u)) * kSpacing[level];
}
// Adds a bent segment: split at a displaced midpoint, `depth` times.
inline void addBent(std::vector<Segment>& out, float ax, float az, float bx, float bz, float hw, float off,
                    uint32_t key, int depth, float amount) {
    if (depth == 0) { out.push_back({ax, az, bx, bz, hw, off}); return; }
    float dx = bx - ax, dz = bz - az;
    float shift = (hashUnit(int(key), depth, 733u) - 0.5f) * amount;
    float mx = 0.5f * (ax + bx) - dz * shift, mz = 0.5f * (az + bz) + dx * shift;
    addBent(out, ax, az, mx, mz, hw, off, key * 2654435761u + 1u, depth - 1, amount * 0.6f);
    addBent(out, mx, mz, bx, bz, hw, off, key * 2654435761u + 2u, depth - 1, amount * 0.6f);
}
inline void nearestOn(const std::vector<Segment>& segs, float x, float z, float& px, float& pz, float& best) {
    best = 1e30f;
    for (const Segment& s : segs) {
        float dx = s.bx - s.ax, dz = s.bz - s.az;
        float t = std::clamp(((x - s.ax) * dx + (z - s.az) * dz) / std::max(1e-6f, dx * dx + dz * dz), 0.0f, 1.0f);
        float qx = s.ax + dx * t, qz = s.az + dz * t;
        float d = (qx - x) * (qx - x) + (qz - z) * (qz - z);
        if (d < best) { best = d; px = qx; pz = qz; }
    }
    best = std::sqrt(best);
}
}  // namespace detail

// Every segment that can matter inside the rectangle (callers widen it by the reach they need).
inline void collectSegments(const Generator& g, float x0, float x1, float z0, float z1, std::vector<Segment>& out) {
    using detail::kSpacing;
    // Level 0, looked up wider than the rectangle: a trunk runs up to ~1.5 spacings, and the finer
    // levels need the trunks near their own points.
    std::vector<Segment> trunks, tribs;
    const float m0 = kSpacing[0] * 2.5f;
    for (int k = int(std::floor((z0 - m0) / kSpacing[0])); k <= int(std::floor((z1 + m0) / kSpacing[0])); ++k)
        for (int i = int(std::floor((x0 - m0) / kSpacing[0])); i <= int(std::floor((x1 + m0) / kSpacing[0])); ++i) {
            float px, pz; detail::point(g, 0, i, k, px, pz);
            float c = controlField(g, px, pz), best = c;
            float qx = 0, qz = 0;
            for (int dk = -1; dk <= 1; ++dk)
                for (int di = -1; di <= 1; ++di) {
                    if (!di && !dk) continue;
                    float nx, nz; detail::point(g, 0, i + di, k + dk, nx, nz);
                    float cn = controlField(g, nx, nz);
                    if (cn < best) { best = cn; qx = nx; qz = nz; }
                }
            if (best >= c) continue;   // a sink: the trunk ends (a basin, usually)
            float m = maturity(g, 0.5f * (px + qx), 0.5f * (pz + qz));
            float hw = 55.0f + 420.0f * m * m;
            detail::addBent(trunks, px, pz, qx, qz, hw, 0.0f, uint32_t(i * 92821 + k * 7919), 3, 0.35f);
        }
    // Level 1: to the nearest point on a trunk.
    const float m1 = kSpacing[1] * 2.0f;
    for (int k = int(std::floor((z0 - m1) / kSpacing[1])); k <= int(std::floor((z1 + m1) / kSpacing[1])); ++k)
        for (int i = int(std::floor((x0 - m1) / kSpacing[1])); i <= int(std::floor((x1 + m1) / kSpacing[1])); ++i) {
            float px, pz; detail::point(g, 1, i, k, px, pz);
            float qx = px, qz = pz, d;
            detail::nearestOn(trunks, px, pz, qx, qz, d);
            if (d > kSpacing[0] * 1.2f || d < 1.0f) continue;
            bool deep = hashUnit(i, k, 739u) < 0.5f;
            detail::addBent(tribs, px, pz, qx, qz, deep ? 22.0f : 14.0f, deep ? 0.0f : kWallFoot + 8.0f,
                            uint32_t(i * 15485863 + k * 104729), 2, 0.4f);
        }
    // Level 2: to the nearest level 0 or 1 segment.
    std::vector<Segment> coarse = trunks;
    coarse.insert(coarse.end(), tribs.begin(), tribs.end());
    const float m2 = kSpacing[2] * 2.0f;
    for (int k = int(std::floor((z0 - m2) / kSpacing[2])); k <= int(std::floor((z1 + m2) / kSpacing[2])); ++k)
        for (int i = int(std::floor((x0 - m2) / kSpacing[2])); i <= int(std::floor((x1 + m2) / kSpacing[2])); ++i) {
            float px, pz; detail::point(g, 2, i, k, px, pz);
            float qx = px, qz = pz, d;
            detail::nearestOn(coarse, px, pz, qx, qz, d);
            // Only some of them, and short: every one cuts the Navajo down to the bench, and all of
            // them together left none of it standing.
            if (d > kSpacing[1] * 0.9f || d < 1.0f || hashUnit(i, k, 747u) > 0.45f) continue;
            bool box = hashUnit(i, k, 743u) < 0.3f;
            detail::addBent(out, px, pz, qx, qz, box ? 12.0f : 5.0f,
                            box ? kWallFoot + 8.0f : kayentaBase(kKayentaTop) + 4.0f,
                            uint32_t(i * 49979687 + k * 3571), 1, 0.4f);
        }
    out.insert(out.end(), coarse.begin(), coarse.end());
}

// e is only needed this far: past it every stratum's profile has levelled out (the Navajo's front
// has rounded over), so any larger value draws the same. Segments are binned by how far they can
// reach, and a column looks only in its own bin.
constexpr float kNeedE = 480.0f;
constexpr float kFarE = 900.0f;
constexpr float kWarp = 35.0f;      // drainageDistance's wander on the query point
constexpr float kTile = 1024.0f;
constexpr int   kBins = 16;         // per tile side: 64-unit bins

// The segments that can bring e under kNeedE at (x, z): a per-thread memo of the last few tiles, each
// collected once and binned. Only a cache -- the answer is the same pure function of position.
inline const std::vector<uint16_t>& segmentBin(const Generator& g, float x, float z, const std::vector<Segment>*& segs) {
    struct Tile {
        int tx = INT_MIN, tz = INT_MIN; uint32_t seed = 0;
        std::vector<Segment> segs;
        std::vector<uint16_t> bins[kBins * kBins];
    };
    thread_local Tile tiles[4];
    const int tx = int(std::floor(x / kTile)), tz = int(std::floor(z / kTile));
    Tile& t = tiles[(tx & 1) + 2 * (tz & 1)];
    if (t.tx != tx || t.tz != tz || t.seed != g.seed) {
        t.tx = tx; t.tz = tz; t.seed = g.seed;
        t.segs.clear();
        for (auto& b : t.bins) b.clear();
        const float x0 = float(tx) * kTile, z0 = float(tz) * kTile;
        // A segment reaches as far as e can still be under kNeedE: its half-width plus kNeedE, less
        // its offset (a cleft's e starts high), plus the query warp.
        const float maxReach = kNeedE + 500.0f + kWarp;
        collectSegments(g, x0 - maxReach, x0 + kTile + maxReach, z0 - maxReach, z0 + kTile + maxReach, t.segs);
        const float bin = kTile / float(kBins);
        for (size_t n = 0; n < t.segs.size() && n < 65535; ++n) {
            const Segment& sg = t.segs[n];
            const float reach = kNeedE + sg.halfWidth - sg.offset + kWarp;
            if (reach <= 0.0f) continue;
            int i0 = std::max(0, int(std::floor((std::min(sg.ax, sg.bx) - reach - x0) / bin)));
            int i1 = std::min(kBins - 1, int(std::floor((std::max(sg.ax, sg.bx) + reach - x0) / bin)));
            int k0 = std::max(0, int(std::floor((std::min(sg.az, sg.bz) - reach - z0) / bin)));
            int k1 = std::min(kBins - 1, int(std::floor((std::max(sg.az, sg.bz) + reach - z0) / bin)));
            for (int k = k0; k <= k1; ++k)
                for (int i = i0; i <= i1; ++i) {
                    // Only bins the segment really comes within reach of (the bin's centre, padded
                    // by the bin's half-diagonal).
                    float cx = x0 + (float(i) + 0.5f) * bin, cz = z0 + (float(k) + 0.5f) * bin;
                    if (segmentDistance(sg, cx, cz) <= reach + bin * 0.71f) t.bins[k * kBins + i].push_back(uint16_t(n));
                }
        }
    }
    segs = &t.segs;
    const int bi = std::clamp(int(std::floor((x - float(tx) * kTile) / (kTile / kBins))), 0, kBins - 1);
    const int bk = std::clamp(int(std::floor((z - float(tz) * kTile) / (kTile / kBins))), 0, kBins - 1);
    return t.bins[bk * kBins + bi];
}

inline float drainageDistance(const Generator& g, float x, float z) {
    // A little wander on the query point, so walls are not ruled along each straight reach.
    const float wx = x + (g.rock3(x / 300.0f, z / 300.0f, 3.7f) - 0.5f) * 2.0f * 35.0f;
    const float wz = z + (g.rock3(x / 300.0f, z / 300.0f, 5.9f) - 0.5f) * 2.0f * 35.0f;
    float e = kFarE;
    const std::vector<Segment>* segs = nullptr;
    for (uint16_t n : segmentBin(g, x, z, segs)) {
        const Segment& s = (*segs)[n];
        float d = segmentDistance(s, wx, wz) - s.halfWidth + s.offset;
        if (d < e) e = d;
    }
    if (e > kNeedE) e = kFarE;   // past the reach every answer draws alike; make them one
    // Basins: the plateau gone where the control field is lowest. Its gradient is about 1/10000 per
    // unit, so scaled up it is near enough a distance -- the basin's rim is its contour.
    const float basin = (controlField(g, x, z) - kBasinLevel) * 9000.0f;
    e = std::min(e, basin);
    // Buttes standing in the basins, blocky (joint frame).
    const float open = smoothstep(80.0f, -200.0f, basin);
    if (open > 0.0f) {
        Frame f = jointFrame(g, x, z, 120.0f, 700.0f, 41.0f);
        Cells b = worleyJointed(f.u / 1100.0f, f.v / 1100.0f, 621u);
        if (float(b.id >> 24) / 255.0f < 0.5f) {
            const float radius = 110.0f + 300.0f * float((b.id >> 16) & 0xFFu) / 255.0f;
            e = std::max(e, radius - b.f1 * 1100.0f - (1.0f - open) * 1000.0f);
        }
    }
    return e;
}

// Where the strata sit: a broad swell, and a **monocline** -- a band where the whole column bends up
// steeply, like Capitol Reef's Waterpocket Fold -- so rims and tops tilt, and one side of the fold
// towers over the other.
inline float strataBase(const Generator& g, float x, float z) {
    float base = 300.0f + 110.0f * (g.rock3(x / 3600.0f, 1.7f, z / 3600.0f) - 0.5f) * 2.0f;
    float s = x * 0.8f + z * 0.6f + 600.0f * (g.rock3(x / 3000.0f, 4.0f, z / 3000.0f) - 0.5f) * 2.0f;
    base += 380.0f * smoothstep(-450.0f, 450.0f, s - 1400.0f);
    return base;
}
// How thick the strata are here: 0.85x to 1.35x, so some walls tower and some are modest.
inline float strataScale(const Generator& g, float x, float z) {
    return 0.85f + 0.5f * g.rock3(x / 4200.0f, 5.5f, z / 4200.0f);
}

// The wall's own coordinate: joints in two sets at right angles, aligned with the network's frame. A
// wall along one set's direction takes the other set; `ex`, `ez` are e's gradient (the wall's normal).
inline float wallAlong(const Generator& g, float x, float z, float ex, float ez, float dither) {
    const float a = jointAngle(g, x, z);
    const float c = std::cos(a), s = std::sin(a);
    const float u = x * c + z * s, v = -x * s + z * c;
    const float nu = ex * c + ez * s, nv = -ex * s + ez * c;
    return std::abs(nu) + dither * 0.3f < std::abs(nv) ? u : v + 7919.0f;
}

// Pebble-scale relief: a fine two-voxel noise and the odd one-voxel stone, keyed on the nearest ring's
// voxels. Without it the ground is the smooth surface of its own function -- triangles, not dirt.
inline float grit(const Generator& g, float x, float z) {
    const int ix = int(std::floor(x / 2.8f)), iz = int(std::floor(z / 2.8f));
    float h = (g.rock3(x / 5.5f, 8.8f, z / 5.5f) - 0.5f) * 2.4f;
    if (hashUnit(ix, iz, 677u) < 0.05f) h += 2.8f;
    return h;
}
// A pebble's colour, or 255 for none: varnished dark, red, pale.
inline uint8_t pebble(float x, float z) {
    const int ix = int(std::floor(x / 2.8f)), iz = int(std::floor(z / 2.8f));
    const float r = hashUnit(ix, iz, 683u);
    if (r > 0.07f) return 255;
    return r < 0.03f ? uint8_t(Varnish + 1) : r < 0.05f ? uint8_t(Wingate + 3) : uint8_t(Navajo + 4);
}

// --- The desert floor ----------------------------------------------------------------------------
//
// **A desert floor is not one surface.** It is a patchwork of substrates, and plants live in only one
// of them: dune fields (loose sand, rippled ridges, nearly bare), rock pavement (slickrock humps, bare
// but for what roots in the cracks), and soil between them, where the vegetation gathers -- and even
// there in clumps, with bare ground between. A uniform cover of grass and brush reads as a meadow.

// 0 = rock pavement .. 1 = dune sand; soil around the middle.
inline float sandiness(const Generator& g, float x, float z) {
    const float wx = x + (g.rock3(x / 400.0f, z / 400.0f, 2.3f) - 0.5f) * 300.0f;
    return g.rock3(wx / 700.0f, 6.1f, z / 700.0f);
}
inline float soilness(float sand) { return smoothstep(0.26f, 0.38f, sand) * smoothstep(0.72f, 0.6f, sand); }

// Where plants are, 0..1: clumps a few tens of units across, mostly on soil.
inline float vegetation(const Generator& g, float x, float z) {
    const float clump = g.rock3(x / 70.0f, 2.7f, z / 70.0f) * 0.75f + g.rock3(x / 22.0f, 3.9f, z / 22.0f) * 0.25f;
    const float patch = smoothstep(0.6f, 0.68f, clump);
    return patch * (0.12f + 0.88f * soilness(sandiness(g, x, z)));
}

// Height of the open ground (e < 0) above the strata's base: broad swells; dune ridges in the sand,
// slickrock humps in the pavement; shallow arroyos (a fine network of washes). All fading out toward
// the slope's toe (e = 0).
inline float floorHeight(const Generator& g, float x, float z, float e, float* incision = nullptr) {
    const float out = smoothstep(0.0f, -200.0f, e);
    const float sand = sandiness(g, x, z);
    float h = 22.0f * (g.rock3(x / 800.0f, 2.0f, z / 800.0f) - 0.5f) * 2.0f * out;
    // Dunes: transverse ridges across the wind (along x here), asymmetric, rippled.
    const float duneField = smoothstep(0.6f, 0.75f, sand) * out;
    if (duneField > 0.0f) {
        const float w = z / 70.0f + (g.rock3(x / 160.0f, 5.2f, z / 160.0f) - 0.5f) * 3.0f;
        const float f = w - std::floor(w);
        const float ridge = f < 0.7f ? f / 0.7f : (1.0f - f) / 0.3f;   // long windward, short slip face
        h += duneField * (16.0f * ridge * ridge * (0.6f + 0.4f * g.rock3(x / 300.0f, 1.4f, z / 300.0f))
                          + 0.8f * std::sin(z * 0.9f + x * 0.15f));
    }
    // Pavement: slickrock humps.
    const float rockField = smoothstep(0.32f, 0.2f, sand) * out;
    if (rockField > 0.0f) h += rockField * 10.0f * g.rock3(x / 55.0f, 8.1f, z / 55.0f);
    // Arroyos: only along some of the network's edges -- an even mesh of them read as cracked mud.
    float cut = 0.0f;
    const float washes = smoothstep(0.5f, 0.62f, g.rock3(x / 700.0f, 4.7f, z / 700.0f));
    if (washes > 0.0f) {
        const float d = networkDistance(g, x, z, 520.0f, 160.0f, 631u);
        if (d < 11.0f) cut = 6.0f * washes * (1.0f - (d / 11.0f) * (d / 11.0f)) * smoothstep(0.0f, -60.0f, e);
    }
    if (incision) *incision = cut;
    return h - cut + grit(g, x, z) * out;
}

// --- Grass ---------------------------------------------------------------------------------------
// Bunchgrass: tufts on a jittered 9-unit grid, each a little dome of blades 2-5 voxels tall at its
// centre, alive (green) or cured (straw) by patches -- after a rain one hollow greens while the next
// stays dry. Returns the blade height at (x, z) in voxels, 0 for none.
inline int grassAt(const Generator& g, float x, float z, float density, bool& dead) {
    const float cell = 9.0f;
    int cx = int(std::floor(x / cell)), cz = int(std::floor(z / cell));
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            int gx = cx + dx, gz = cz + dz;
            if (hashUnit(gx, gz, 641u) > density) continue;
            float tx = (float(gx) + hashUnit(gx, gz, 643u)) * cell, tz = (float(gz) + hashUnit(gx, gz, 647u)) * cell;
            float r = 3.0f + 3.5f * hashUnit(gx, gz, 653u);
            float d2 = ((x - tx) * (x - tx) + (z - tz) * (z - tz)) / (r * r);
            if (d2 >= 1.0f) continue;
            dead = hashUnit(gx, gz, 659u) > 0.25f + 0.55f * g.rock3(tx / 900.0f, 3.9f, tz / 900.0f);
            float tall = (dead ? 3.0f : 2.0f) + 2.0f * hashUnit(gx, gz, 661u);
            int ix = int(std::floor(x / 2.8f)), iz = int(std::floor(z / 2.8f));
            return std::max(1, int(tall * std::sqrt(1.0f - d2) * (0.6f + 0.5f * hashUnit(ix, iz, 667u)) + 0.5f));
        }
    return 0;
}

// --- Colour --------------------------------------------------------------------------------------
inline uint8_t ramp(uint8_t first, int steps, float v, float dither) {
    return uint8_t(first + std::clamp(int((v + dither * 0.35f) * float(steps)), 0, steps - 1));
}

inline uint8_t chinleColour(const Generator& g, float x, float y, float e, bool top, float incision, float dither) {
    const float t = std::min(1.0f, std::max(0.0f, e) / kWallFoot);
    if (top) { uint8_t p = pebble(x * 1.0f, e); if (p != 255) return p; }
    if (top) {
        // Gully floors: washed sand. The toe: sand blown up onto it.
        if (incision > kGullyDepth * 0.55f || t < 0.12f) return dither > -0.2f ? Sand : Sand + 1;
        // Debris: red fragments off the wall, thickest under it, in patches. Each fragment is a
        // Worley cell a couple of voxels across, one shade throughout -- chosen per voxel, the debris
        // was salt and pepper.
        float cover = 0.75f * smoothstep(0.2f, 0.9f, t) * (0.6f + 0.8f * (g.rock3(x / 30.0f, 4.4f, e / 30.0f) - 0.5f));
        Cells frag = worley2(x / 6.5f, e / 6.5f, 463u);
        float pick = float(frag.id & 0xFFFFu) / 65535.0f;
        if (incision < 2.0f && pick < cover) return uint8_t(Wingate + 1 + int(float(frag.id >> 24) / 256.0f * 4.0f));
        if (incision < 2.0f && frag.f2 - frag.f1 < 0.08f && pick < cover + 0.15f) return Wingate;   // the gaps between, in shade
    }
    // The bands: soft, slightly wavy, each a colour of its own.
    float b = y / 11.0f + (g.rock3(x / 140.0f, y / 60.0f, 2.2f) - 0.5f) * 0.9f;
    int band = int(std::floor(b));
    return uint8_t(Chinle + int(hashUnit(band, 21, 461u) * 4.999f));
}

// The open floor: red sand, paler drifts, washed sand in the arroyos, and dark crusts of
// cryptobiotic soil in the stable ground between the grass.
inline uint8_t floorColour(const Generator& g, float x, float z, bool top, float incision, float dither) {
    if (top) { uint8_t p = pebble(x, z); if (p != 255) return p; }
    if (incision > 1.5f) return dither > 0.0f ? Sand + 1 : Sand;
    const float sand = sandiness(g, x, z) + dither * 0.04f;
    if (sand < 0.27f) {
        // Pavement: slickrock, salmon to cream in broad patches.
        float v = g.rock3(x / 60.0f, 3.1f, z / 60.0f);
        return v < 0.45f ? uint8_t(NavajoBand + 1) : uint8_t(Navajo + 2 + int(v * 2.0f));
    }
    if (sand > 0.66f) return g.rock3(x / 200.0f, 5.0f, z / 200.0f) > 0.5f ? SandRed : Sand;   // dune sand
    // Soil: dark crust under and around the plants, red sand between.
    if (top && vegetation(g, x, z) > 0.15f && g.rock3(x / 6.0f, 1.3f, z / 6.0f) > 0.45f) return dither > 0.0f ? Crust : Crust + 1;
    return g.rock3(x / 140.0f, 7.0f, z / 140.0f) < 0.4f ? Sand + 1 : Sand;
}

inline uint8_t wingateColour(const Generator& g, float x, float y, float e, bool top, bool fresh, float chute, float dither) {
    (void)e;
    const float above = y - kChinleTop;
    const float fromRim = (kWingateTop - y) / (kWingateTop - kChinleTop);
    Joint jt = jointAt(wallWarp(g, x, y));
    if (top) {
        // A ledge or a scar's floor: dust and debris on it.
        return dither > 0.0f ? Sand + 1 : ramp(Wingate, 6, 0.6f, dither);
    }
    // Varnish: down the chutes, and in streaks from the rim elsewhere; thinning downward.
    float streak = g.rock3(x / 6.0f, y / 170.0f, 9.5f);
    float strength = std::max(chute * 1.3f, (streak - 0.56f) / 0.22f) * (0.35f + 0.85f * (1.0f - fromRim * 0.8f));
    if (fresh) strength *= 0.25f;   // a fresh scar has not had time to varnish
    if (strength > 0.0f && dither + 0.5f < strength) return ramp(Varnish, 3, 1.0f - strength, dither);
    if (fresh) return dither > 0.1f ? WingateFresh : WingateFresh + 1;
    float v = 0.45f + (hashUnit(jt.segment, 5, 467u) - 0.5f) * 0.3f
            + std::sin(above * 0.33f + g.rock3(x / 60.0f, y / 60.0f, 4.0f) * 6.0f) * 0.05f
            + (g.rock3(x / 25.0f, y / 25.0f, 6.6f) - 0.5f) * 0.35f;
    // The base, wet from seeps and in shade, darker; the rim's edge bleached.
    v -= 0.15f * (1.0f - smoothstep(0.0f, 30.0f, above));
    return ramp(Wingate, 6, v, dither);
}

inline uint8_t kayentaColour(const Generator& g, float x, float y, bool top, bool hard, int bed, float dither) {
    if (top && dither > -0.1f) return Sand + 1;
    float v = hashUnit(bed, 23, 471u) * 0.5f + (g.rock3(x / 20.0f, y / 6.0f, 1.1f) - 0.5f) * 0.3f;
    return hard ? ramp(Kayenta + 3, 3, v + 0.25f, dither) : ramp(Kayenta, 3, v + 0.2f, dither);
}

inline uint8_t navajoColour(const Generator& g, float x, float above, float e, bool top, float joint, float dither) {
    if (joint < 0.04f) return ramp(Navajo, 6, 0.15f, dither);   // the cracks hold dark grit
    // Cross-bed laminae: dipping, gently curved planes cut through the dome.
    // Steeply dipping, so the flat tops of the domes are striped too rather than one broad band.
    float c = above * 0.7f + (x * 0.5f + e * 0.3f) * 0.7f + (g.rock3(x / 70.0f, above / 70.0f, e / 70.0f) - 0.5f) * 20.0f;
    float f = c / 7.0f - std::floor(c / 7.0f);
    // Salmon low on the domes, cream high: the iron was leached from the top.
    float bleach = smoothstep(25.0f, 90.0f, above + (g.rock3(x / 90.0f, 2.0f, e / 90.0f) - 0.5f) * 40.0f);
    if (top && g.rock3(x / 18.0f, 6.0f, e / 18.0f) > 0.78f) return Lichen;
    if (f < 0.25f) return bleach < 0.5f ? ramp(NavajoBand, 3, 0.4f, dither) : ramp(Navajo, 6, 0.55f, dither);
    if (bleach < 0.4f) return ramp(NavajoBand, 3, 0.75f + f * 0.3f, dither);
    return ramp(Navajo, 6, 0.55f + 0.4f * bleach + (f - 0.6f) * 0.2f, dither);
}

}  // namespace canyonlands

#endif
