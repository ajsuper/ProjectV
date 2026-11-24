// One column of a clipmap ring: the ground under a 64 x 64 voxel footprint, as the 64^3 chunks it
// fills, each a tile table ready for a blob. Runs on a worker; touches no Scene.
//
// Ring k's voxels are 0.1 * 2^k m (santa-fe-reference S3: 55's 0.25 m ring 0 was too coarse up close). A column samples the landform once per voxel
// column (with a border, for its neighbours), then fills each voxel column from its surface down to
// a little below its lowest neighbour: deep enough that no gap shows on a slope or against the next
// ring, and no deeper (what is below is never seen, and a tile table stores solid bricks for almost
// nothing anyway).
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

#include "mountains/landform.hpp"
#include "mountains/placement.hpp"
#include "mountains/props.hpp"
#include "mountains/surface.hpp"
#include "mountains/trees.hpp"
#include "utils/material.h"
#include "utils/tile_table.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

namespace mountains {

constexpr int kChunkRes = 64;

constexpr double kBaseVoxel = 0.1;   // the user: 0.125 still a little coarse; trees and props grown to match
inline double voxelSize(int ring) { return kBaseVoxel * double(1u << ring); }
inline double cellSize(int ring) { return voxelSize(ring) * kChunkRes; }

struct ColumnChunk {
    int64_t iy = 0;   // cell, in the ring's cells (world y = iy * cellSize)
    projv::utils::TileTable tiles;
};

struct ColumnResult {
    int ring = 0;
    int64_t ix = 0, iz = 0;
    std::vector<ColumnChunk> chunks;   // a voxel ring's
    std::vector<float> farHeights;     // a far ring's: 64 x 64 heights, x fastest
    std::vector<uint32_t> farColours;  // ... the ground's colours, RGBA8 (sRGB; a: the canopy cover), x fastest
    std::vector<uint32_t> farCanopy;   // ... the canopy's colour (sRGB; a: bit 7 conifer, bits 0-6 height / 0.35 m)
    float farMax = -1e9f;              // ... the highest the ground and its trees stand (the blocky march skips by it)
    std::vector<PropInstance> props;   // the props this column draws (rings 0-2)
    double sampleMs = 0, fillMs = 0;
};

// One column of a far ring (plan D14-D16): no voxels, a 64 x 64 tile of the ground at the ring's
// resolution -- its height, raised by the canopy where there is forest, and its colour, the surface mix
// blended rather than dithered -- for the renderer to march as a smooth surface.
// `samples` a side (64, or fewer for blocky distance: one a block, replicated over its texels -- the far
// columns are a third of the generation, and at 16 a side a sixteenth of it).
inline ColumnResult generateFarColumn(const Landform& land, int ring, int64_t ix, int64_t iz, int samples_ = kChunkRes) {
    using clock = std::chrono::steady_clock;
    ColumnResult out;
    out.ring = ring;
    out.ix = ix;
    out.iz = iz;
    const double v = voxelSize(ring), c = cellSize(ring);
    const double x0 = double(ix) * c, z0 = double(iz) * c;
    const auto t0 = clock::now();
    std::vector<Sample> samples;
    const int ns = std::clamp(samples_, 1, kChunkRes);
    const int rep = kChunkRes / ns;
    if (rep <= 1) {
        land.sampleGrid(x0 + 0.5 * v, z0 + 0.5 * v, v, kChunkRes, kChunkRes, v, samples);
    } else {
        // One sample a block of rep x rep texels, at its centre and its footprint, copied to every texel of it.
        std::vector<Sample> coarse;
        const double b = v * double(rep);
        land.sampleGrid(x0 + 0.5 * b, z0 + 0.5 * b, b, ns, ns, b, coarse);
        samples.resize(size_t(kChunkRes) * size_t(kChunkRes));
        for (int j = 0; j < kChunkRes; ++j)
            for (int i = 0; i < kChunkRes; ++i) samples[size_t(j) * kChunkRes + size_t(i)] = coarse[size_t(j / rep) * size_t(ns) + size_t(i / rep)];
    }
    const auto t1 = clock::now();
    out.farHeights.resize(samples.size());
    out.farColours.resize(samples.size());
    out.farCanopy.resize(samples.size());
    auto byte = [](double v8) { return uint32_t(std::clamp(int(std::lround(v8 * 255.0)), 0, 255)); };
    for (int j = 0; j < kChunkRes; ++j)
        for (int i = 0; i < kChunkRes; ++i) {
            const size_t k = size_t(j) * kChunkRes + size_t(i);
            if (rep > 1 && (i % rep != 0 || j % rep != 0)) {   // a block's texels are its first's (copied below)
                const size_t k0 = size_t(j - j % rep) * kChunkRes + size_t(i - i % rep);
                out.farHeights[k] = out.farHeights[k0];
                out.farColours[k] = out.farColours[k0];
                out.farCanopy[k] = out.farCanopy[k0];
                continue;
            }
            const Sample& s = samples[k];
            const double x = x0 + (i + 0.5) * v, z = z0 + (j + 0.5) * v;
            const SurfaceMix m = surfaceMix(s, x, z, v);
            // The ground alone: the renderer stands the trees on it (santa-fe-reference R7).
            out.farHeights[k] = float(s.height + rockBlocks(x, z, rockWeight(s), v).height);   // the blocks' mean, at this footprint
            SurfaceMix ground = m;
            ground.cover = 0.0;
            // The voxels' shades average 0.84 of their class colours: the same here, so the two meet.
            const Rgb col = mixColour(ground) * 0.84;
            const double cover = std::clamp(m.cover, 0.0, 1.0);
            out.farColours[k] = byte(col.r) | (byte(col.g) << 8) | (byte(col.b) << 16) | (byte(cover) << 24);
            Rgb tree = lerp(materialColour(m.treeA), materialColour(m.treeB), m.treeMix) * 0.84;
            // Snow caught in the crowns: from the snowline itself (as trace.frag's on voxel trees), not the ground's
            // snow, which the cover thins.
            const double crownSnow = std::max(m.snow, smoothstep(snowlineAt() - 320.0, snowlineAt() - 80.0, double(out.farHeights[k])));
            tree = lerp(tree, materialColour(Material::Snow) * 0.8, crownSnow * 0.55);
            const Material lead = m.treeMix > 0.5 ? m.treeB : m.treeA;
            const bool conifer = lead == Material::Fir || lead == Material::Spruce;
            const double height = mix(canopyHeight(m.treeA), canopyHeight(m.treeB), m.treeMix);
            const uint32_t code = uint32_t(std::clamp(int(std::lround(height / 0.35)), 0, 127)) | (conifer ? 128u : 0u);
            out.farCanopy[k] = byte(tree.r) | (byte(tree.g) << 8) | (byte(tree.b) << 16) | (code << 24);
            out.farMax = std::max(out.farMax, out.farHeights[k] + float(cover * height));
        }
    out.sampleMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    out.fillMs = std::chrono::duration<double, std::milli>(clock::now() - t1).count();
    return out;
}

// How many voxels below its lowest neighbour a column is filled.
constexpr int kSkirt = 4;

inline ColumnResult generateColumn(const Landform& land, const Palette& palette, const ShapeLibrary* library, int ring,
                                   int64_t ix, int64_t iz) {
    using clock = std::chrono::steady_clock;
    ColumnResult out;
    out.ring = ring;
    out.ix = ix;
    out.iz = iz;
    const double v = voxelSize(ring);
    const double c = cellSize(ring);
    const double x0 = double(ix) * c, z0 = double(iz) * c;
    const int n = kChunkRes + 2;   // a voxel of border on each side

    const auto t0 = clock::now();
    std::vector<Sample> samples;
    if (v <= 0.45) {
        // The fine rings sample the landform at twice their voxel -- the next ring's own lattice -- and
        // interpolate (Catmull-Rom): what is finer than that is a few centimetres of relief, and a quarter of
        // the samples is three quarters of a column's sampling time saved.
        const double s2 = 2.0 * v;
        const double gx0 = std::floor((x0 - 0.5 * v) / s2) * s2 - s2, gz0 = std::floor((z0 - 0.5 * v) / s2) * s2 - s2;
        const int m = int(std::ceil((double(n) * v + 3.0 * s2) / s2)) + 2;
        std::vector<Sample> coarse;
        land.sampleGrid(gx0, gz0, s2, m, m, s2, coarse);
        auto cr = [](double p0, double p1, double p2, double p3, double t) {
            return p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
        };
        samples.resize(size_t(n) * size_t(n));
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const double fx = (x0 + (i - 0.5) * v - gx0) / s2, fz = (z0 + (j - 0.5) * v - gz0) / s2;
                const int ci = std::clamp(int(std::floor(fx)), 1, m - 3), cj = std::clamp(int(std::floor(fz)), 1, m - 3);
                const double tx = fx - ci, tz = fz - cj;
                auto at = [&](int a, int b) -> const Sample& { return coarse[size_t(b) * size_t(m) + size_t(a)]; };
                auto field = [&](double Sample::*f) {
                    double row[4];
                    for (int k = 0; k < 4; ++k)
                        row[k] = cr(at(ci - 1, cj - 1 + k).*f, at(ci, cj - 1 + k).*f, at(ci + 1, cj - 1 + k).*f, at(ci + 2, cj - 1 + k).*f, tx);
                    return cr(row[0], row[1], row[2], row[3], tz);
                };
                Sample& o = samples[size_t(j) * size_t(n) + size_t(i)];
                o = at(int(std::lround(fx)), int(std::lround(fz)));
                o.height = field(&Sample::height);
                o.gx = field(&Sample::gx);
                o.gz = field(&Sample::gz);
            }
    } else {
        land.sampleGrid(x0 - 0.5 * v, z0 - 0.5 * v, v, n, n, v, samples);
    }
    const auto t1 = clock::now();

    // Trees, in the rings fine enough to draw them: their trunks stand on a coarser grid of the ground
    // (aligned to the world, so neighbouring columns agree on every trunk) around the column.
    // Every voxel ring, the 3.2 m one too: without it the trees went from silhouettes (1.6 m) to blocks of
    // canopy colour and back to the far field's crowns past it.
    const bool stampTrees = v <= 4.0;
    const bool near = v <= 0.25;   // loose objects within the two finest rings; baked beyond
    std::vector<Tree> trees;
    std::vector<Placement> placements;   // to stamp (far rings)
    if (stampTrees) {
        const double g = std::max(v, 2.0);   // the ground placements stand on: 2 m is plenty (a 1 m grid with its margin was most of a fine column's time)
        const double gx0 = std::floor((x0 - kTreeReach - kTreeCell) / g) * g, gz0 = std::floor((z0 - kTreeReach - kTreeCell) / g) * g;
        const int gn = int(std::ceil((c + 2.0 * (kTreeReach + kTreeCell)) / g)) + 2;
        std::vector<Sample> coarse;
        land.sampleGrid(gx0, gz0, g, gn, gn, g, coarse);
        auto groundAt = [&](double x, double z, Sample& out) {
            const double fi = (x - gx0) / g, fj = (z - gz0) / g;
            const int i = int(std::floor(fi)), j = int(std::floor(fj));
            if (i < 0 || j < 0 || i >= gn - 1 || j >= gn - 1) return false;
            const double u = fi - i, w = fj - j;
            auto at = [&](int a, int b) -> const Sample& { return coarse[size_t(b) * size_t(gn) + size_t(a)]; };
            out = at(int(std::lround(fi)), int(std::lround(fj)));
            out.height = mix(mix(at(i, j).height, at(i + 1, j).height, u), mix(at(i, j + 1).height, at(i + 1, j + 1).height, u), w);
            return true;
        };
        // Trees and shrubs from the stand grids (chamisa is a placement of its own, below).
        collectTrees(x0, z0, x0 + c, z0 + c, land.settings().seed, v <= 1.0, false, groundAt, trees);
        if (library && !library->shapes.empty()) {
            // Everything that stands on the ground as placements (placement.hpp): near rings keep them as loose
            // objects, each in the column its anchor is in; coarser rings stamp every one that reaches them.
            auto propGround = [&](double x, double z, Sample& out) {
                const double fi = (x - gx0) / g, fj = (z - gz0) / g;
                const int i = int(std::floor(fi)), j = int(std::floor(fj));
                if (i < 0 || j < 0 || i >= gn - 1 || j >= gn - 1) return false;
                const double u = fi - i, w = fj - j;
                auto at = [&](int a, int b) -> const Sample& { return coarse[size_t(b) * size_t(gn) + size_t(a)]; };
                auto bil = [&](double Sample::*f) {
                    return mix(mix(at(i, j).*f, at(i + 1, j).*f, u), mix(at(i, j + 1).*f, at(i + 1, j + 1).*f, u), w);
                };
                out = at(int(std::lround(fi)), int(std::lround(fj)));
                out.height = bil(&Sample::height);
                out.gx = bil(&Sample::gx);
                out.gz = bil(&Sample::gz);
                return true;
            };
            std::vector<Tree> mine, unplaced;
            for (const Tree& t : trees)
                if (!near || (t.x >= x0 && t.x < x0 + c && t.z >= z0 && t.z < z0 + c)) mine.push_back(t);
            if (!near) mine = trees;
            for (Tree& t : mine) {   // on the relief (surface.hpp rockBlocks), not the smooth landform
                Sample ts;
                if (groundAt(t.x, t.z, ts)) t.base += rockBlocks(t.x, t.z, rockWeight(ts), kLattice).height;
            }
            treePlacements(*library, v, mine, placements, unplaced);
            propPlacements(land.settings().seed, *library, v, x0, z0, c, 16.0 * kGameScale, near, propGround, trees, placements);
            trees = std::move(unplaced);   // a kind the library has no tree of is stamped as 55 did
            if (near) {
                for (const Placement& p : placements) out.props.push_back(looseInstance(*library, p));
                placements.clear();
            }
        }
    }

    // Exposed rock raised into blocks (surface.hpp rockBlocks): the heights first, the tones for the fill.
    std::vector<RockBlock> blocks(samples.size());
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const size_t k = size_t(j) * size_t(n) + size_t(i);
            blocks[k] = rockBlocks(x0 + (i - 0.5) * v, z0 + (j - 0.5) * v, rockWeight(samples[k]), v);
            samples[k].height += blocks[k].height;
        }
    // The top solid voxel of each column: a voxel is solid when its centre is under the ground.
    std::vector<int64_t> top(size_t(n) * size_t(n));
    for (size_t i = 0; i < top.size(); ++i) top[i] = int64_t(std::floor(samples[i].height / v - 0.5));
    auto topAt = [&](int i, int j) { return top[size_t(j) * size_t(n) + size_t(i)]; };

    struct ColumnFill { int64_t top, bottom; uint8_t surface, under; bool canopy; uint8_t ground; int blades = 0; uint8_t blade[3] = {0, 0, 0}; };
    std::vector<ColumnFill> fills(size_t(kChunkRes) * size_t(kChunkRes));
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (int j = 0; j < kChunkRes; ++j) {
        for (int i = 0; i < kChunkRes; ++i) {
            const Sample& s = samples[size_t(j + 1) * size_t(n) + size_t(i + 1)];
            const int64_t t = topAt(i + 1, j + 1);
            const int64_t lowest = std::min({t, topAt(i, j + 1), topAt(i + 2, j + 1), topAt(i + 1, j), topAt(i + 1, j + 2)});
            ColumnFill& f = fills[size_t(j) * kChunkRes + size_t(i)];
            f.top = t;
            f.bottom = lowest - kSkirt;
            const double x = x0 + (i + 0.5) * v, z = z0 + (j + 0.5) * v;
            const SurfaceMix mixAt = surfaceMix(s, x, z, v);
            // Oak is drawn from 1 m voxels in, chamisa from 0.5 m; trees from 2 m.
            const bool shrubsDrawn = mixAt.shrub == Material::Chamisa ? v <= 0.5 : v <= 1.0;
            Material m;
            int shade;
            // Patches a few metres across up close; per-voxel dither from 2 m voxels out.
            // Spread to roughly uniform on 0..1 (fBm piles up near its mean), so a weight picks the same
            // share of voxels here as the dither does in the next ring out: no line where they meet.
            const double coherent = clamp01(0.5 + 1.7 * fbm(x / 3.0, z / 3.0, 2, 101));
            pickMaterial(mixAt, hash2(ix * kChunkRes + i, iz * kChunkRes + j, uint32_t(ring) * 0x51ed27U + 9U),
                         coherent, smoothstep(0.5, 2.0, v), stampTrees, shrubsDrawn, m, shade);
            // A rock block's own tone, darker in the joints between blocks.
            // Bare blocks are rock, whatever the mix picked; buried ones are ground (soil, grass), the rock the mix
            // picked there notwithstanding: the blocks decide what is bare.
            const RockBlock& rb = blocks[size_t(j + 1) * size_t(n) + size_t(i + 1)];
            const bool rocky = m == Material::Granite || m == Material::Gneiss || m == Material::Basalt || m == Material::Lichen ||
                               ((m == Material::Sediment || m == Material::RedSoil) && (m == mixAt.rockA || m == mixAt.rockB));
            if (rb.exposed && m != Material::Snow && m != Material::Water) {
                if (!rocky) m = unit(hash32(uint32_t(i * 7919 + j * 104729) ^ uint32_t(ix * 31 + iz))) < mixAt.rockMix ? mixAt.rockB : mixAt.rockA;
                // Lichen in patches; moss in the joints and on the shaded side (pinyon's dark green, or dry grama).
                const uint32_t rh = hash32(uint32_t(i * 131 + j * 977 + ring) ^ uint32_t(ix * 7 + iz * 13));
                const double lichenPatch = fbm(x / (1.5 * kGameScale), z / (1.5 * kGameScale), 2, 861);
                const double north = smoothstep(0.0, 0.6, -samples[size_t(j + 1) * size_t(n) + size_t(i + 1)].gz);
                shade = std::clamp(int((0.25 + 0.6 * rb.tone - 0.5 * rb.crack) * kShades), 0, kShades - 1);
                if ((m == Material::Granite || m == Material::Gneiss) && unit(rh) < 0.05 + 0.5 * smoothstep(0.15, 0.45, lichenPatch)) m = Material::Lichen;
                if (rb.crack > 0.45 && unit(hash32(rh + 1U)) < 0.35 + 0.35 * north) {
                    m = unit(hash32(rh + 2U)) < 0.7 ? Material::Pinyon : Material::Grama;
                    shade = std::clamp(int(unit(hash32(rh + 3U)) * kShades), 0, kShades - 1);
                    if (v <= 0.25 && unit(hash32(rh + 4U)) < 0.5) {   // a cushion of moss standing in the joint
                        f.blades = 1;
                        f.blade[0] = palette.at(m, std::min(kShades - 1, shade + 1));
                    }
                }
            } else if (!rb.exposed && rocky && mixAt.rock < 0.9) {
                m = mixAt.groundMix > 0.5 ? mixAt.groundB : mixAt.groundA;
            }
            f.surface = palette.at(m, shade);
            // A carpet of grass blades on grassy ground in the finest rings (santa-fe-reference G2: the ground
            // is mostly grass, not bare earth): one to three voxels tall at 0.1 m, one at 0.2 m; a few on bare
            // earth too, and none on rock, sand, snow or water.
            if (v <= 0.25) {
                const bool grass = m == Material::Grama || m == Material::Tundra;
                const bool earth = m == Material::Soil || m == Material::RedSoil;
                double density = grass ? 0.38 + 0.3 * mixAt.meadow : (earth ? 0.1 : 0.0);   // thick in a meadow
                density *= (1.0 - mixAt.rock) * (1.0 - mixAt.snow) * clamp01(0.55 + 1.2 * fbm(x / 2.2, z / 2.2, 2, 131));
                if (v > 0.15) density *= 0.45;
                const uint32_t hb = hash2(ix * kChunkRes + i, iz * kChunkRes + j, 0x9e37U + uint32_t(ring));
                if (unit(hb) < density) {
                    const double r = unit(hash32(hb + 1U));
                    f.blades = v > 0.15 ? 1 : (r < 0.8 ? 1 : 2);   // turf: mostly one voxel, a few taller
                    const double c = unit(hash32(hb + 2U));
                    // Straw, tawny and sage-green blades, in patches.
                    const double green = clamp01(0.35 + 0.9 * fbm(x / 6.0, z / 6.0, 2, 133));
                    const Material bm = c < green * 0.55 ? Material::Chamisa : (c < 0.75 ? Material::Grama : Material::Tundra);
                    for (int k = 0; k < f.blades; ++k)
                        f.blade[k] = palette.at(bm, std::clamp(3 + k * 2 + int(unit(hash32(hb + 3U + uint32_t(k))) * 2.0), 0, kShades - 1));
                    // A flower head on top of its stem, in a flower field (one of a few kinds in its drift).
                    if (grass && unit(hash32(hb + 9U)) < 0.1 * mixAt.flowers) {   // a scatter, not a carpet (0.55 was)
                        const int stem = v > 0.15 ? 0 : 1 + int(unit(hash32(hb + 10U)) < 0.4);
                        f.blades = std::min(3, stem + 1);
                        for (int k = 0; k < f.blades - 1; ++k) f.blade[k] = palette.at(Material::Chamisa, 3);
                        const Material fm = unit(hash32(hb + 11U)) < 0.8 ? mixAt.flower
                                          : (unit(hash32(hb + 12U)) < 0.5 ? Material::Daisy : Material::ChamisaBloom);
                        f.blade[f.blades - 1] = palette.at(fm, 2);
                    }
                }
            } else if (v <= 1.0 && (m == Material::Grama || m == Material::Tundra) && mixAt.flowers > 0.0) {
                // Coarser rings: no blades, so a flower field shows as flower voxels in the grass.
                const uint32_t hf = hash2(ix * kChunkRes + i, iz * kChunkRes + j, 0x5f1eU + uint32_t(ring));
                if (unit(hf) < 0.1 * mixAt.flowers) f.surface = palette.at(mixAt.flower, 2);
            }
            f.under = palette.at(subsurface(mixAt), std::clamp(int(mixAt.shade * kShades), 0, kShades - 1));
            // A canopy voxel where trees are not drawn stands one voxel up off the ground, when its trees
            // are tall enough for this ring: woodland keeps a silhouette and a shadow out to where a
            // voxel is taller than a fir.
            f.canopy = false;
            // Up to the 4 m ring: beyond, a whole voxel of forest is a block, and it casts a square shadow.
            if (v <= 4.0 && canopyHeight(m) >= 0.6 * v) {   // only a plant not drawn as voxels comes back as its class
                f.canopy = true;
                f.ground = palette.at(mixAt.groundMix > 0.5 ? mixAt.groundB : mixAt.groundA, shade);
                f.top += 1;
            }
            lo = std::min(lo, f.bottom);
            hi = std::max(hi, f.top + f.blades);
            hi = std::max(hi, f.top);
        }
    }

    // The trees' voxels, in this column only.
    struct TreeVoxel { int i, j; int64_t y; uint8_t slot; };
    std::vector<TreeVoxel> treeVoxels;
    for (const Placement& p : placements)
        stampPlacement(*library, p, v, x0, z0, c, [&](int i, int j, int64_t y, uint8_t slot) {
            treeVoxels.push_back({i, j, y, slot});
            lo = std::min(lo, y);
            hi = std::max(hi, y);
        });
    if (!trees.empty()) {
        const double lo3[3] = {x0, -1e9, z0}, hi3[3] = {x0 + c, 1e9, z0 + c};
        const int64_t bx = ix * kChunkRes, bz = iz * kChunkRes;
        for (const Tree& t : trees)
            stampTree(t, v, lo3, hi3, [&](int64_t vx, int64_t vy, int64_t vz, Material m, int shade) {
                const int i = int(vx - bx), j = int(vz - bz);
                if (i < 0 || j < 0 || i >= kChunkRes || j >= kChunkRes) return;
                treeVoxels.push_back({i, j, vy, palette.at(m, shade)});
                lo = std::min(lo, vy);
                hi = std::max(hi, vy);
            });
    }

    // One chunk per vertical cell the column touches.
    const int64_t cy0 = int64_t(std::floor(double(lo) / kChunkRes)), cy1 = int64_t(std::floor(double(hi) / kChunkRes));
    for (int64_t cy = cy0; cy <= cy1; ++cy) {
        const int64_t base = cy * kChunkRes;
        auto map = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(kChunkRes));
        bool any = false;
        for (int j = 0; j < kChunkRes; ++j)
            for (int i = 0; i < kChunkRes; ++i) {
                const ColumnFill& f = fills[size_t(j) * kChunkRes + size_t(i)];
                const int64_t y0 = std::max(f.bottom, base), y1 = std::min(f.top, base + kChunkRes - 1);
                for (int64_t y = y0; y <= y1; ++y) {
                    const uint8_t slot = y == f.top ? f.surface : (f.canopy && y == f.top - 1 ? f.ground : f.under);
                    projv::utils::brickMapSetVoxel(*map, i, int(y - base), j, slot);
                    any = true;
                }
                for (int k = 0; k < f.blades; ++k) {
                    const int64_t y = f.top + 1 + k;
                    if (y < base || y >= base + kChunkRes) continue;
                    projv::utils::brickMapSetVoxel(*map, i, int(y - base), j, f.blade[k]);
                    any = true;
                }
            }
        for (const TreeVoxel& tv : treeVoxels)
            if (tv.y >= base && tv.y < base + kChunkRes) {
                projv::utils::brickMapSetVoxel(*map, tv.i, int(tv.y - base), tv.j, tv.slot);
                any = true;
            }
        if (!any) continue;
        projv::ChunkHeader hdr;
        hdr.position = projv::core::vec3(float(x0), float(double(base) * v), float(z0));
        hdr.scale = float(c);
        hdr.voxelScale = float(v);
        hdr.resolution = kChunkRes;
        hdr.rotation = projv::core::quat(1, 0, 0, 0);
        projv::Chunk chunk = projv::utils::createChunk(hdr);
        projv::utils::updateChunkFromBrickMap(chunk, *map);
        std::vector<uint8_t> materials;
        projv::utils::bakeMaterialsFromBrickMap(chunk.geometryData, materials, *map);
        ColumnChunk cc;
        cc.iy = cy;
        cc.tiles = projv::utils::buildTileTable(chunk.geometryData, materials, kChunkRes);
        out.chunks.push_back(std::move(cc));
    }
    const auto t2 = clock::now();
    out.sampleMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    out.fillMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    return out;
}

}  // namespace mountains
