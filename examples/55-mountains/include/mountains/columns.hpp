// One column of a clipmap ring: the ground under a 64 x 64 voxel footprint, as the 64^3 chunks it
// fills, each a tile table ready for a blob. Runs on a worker; touches no Scene.
//
// Ring k's voxels are 0.25 * 2^k m (plan D1, D2). A column samples the landform once per voxel
// column (with a border, for its neighbours), then fills each voxel column from its surface down to
// a little below its lowest neighbour: deep enough that no gap shows on a slope or against the next
// ring, and no deeper (what is below is never seen, and a tile table stores solid bricks for almost
// nothing anyway).
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

#include "mountains/landform.hpp"
#include "mountains/props.hpp"
#include "mountains/surface.hpp"
#include "mountains/trees.hpp"
#include "utils/material.h"
#include "utils/tile_table.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

namespace mountains {

constexpr int kChunkRes = 64;

inline double voxelSize(int ring) { return 0.25 * double(1u << ring); }
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
    std::vector<uint32_t> farColours;  // ... and colours, RGBA8 (sRGB), x fastest
    std::vector<PropInstance> props;   // the props this column draws (rings 0-2)
    double sampleMs = 0, fillMs = 0;
};

// One column of a far ring (plan D14-D16): no voxels, a 64 x 64 tile of the ground at the ring's
// resolution -- its height, raised by the canopy where there is forest, and its colour, the surface mix
// blended rather than dithered -- for the renderer to march as a smooth surface.
inline ColumnResult generateFarColumn(const Landform& land, int ring, int64_t ix, int64_t iz) {
    using clock = std::chrono::steady_clock;
    ColumnResult out;
    out.ring = ring;
    out.ix = ix;
    out.iz = iz;
    const double v = voxelSize(ring), c = cellSize(ring);
    const double x0 = double(ix) * c, z0 = double(iz) * c;
    const auto t0 = clock::now();
    std::vector<Sample> samples;
    land.sampleGrid(x0 + 0.5 * v, z0 + 0.5 * v, v, kChunkRes, kChunkRes, v, samples);
    const auto t1 = clock::now();
    out.farHeights.resize(samples.size());
    out.farColours.resize(samples.size());
    for (int j = 0; j < kChunkRes; ++j)
        for (int i = 0; i < kChunkRes; ++i) {
            const size_t k = size_t(j) * kChunkRes + size_t(i);
            const Sample& s = samples[k];
            const double x = x0 + (i + 0.5) * v, z = z0 + (j + 0.5) * v;
            const SurfaceMix m = surfaceMix(s, x, z, v);
            const double canopy = mix(canopyHeight(m.treeA), canopyHeight(m.treeB), m.treeMix);
            out.farHeights[k] = float(s.height + m.cover * canopy * 0.7);
            // Seen from the side, a canopy hides the ground between its crowns, and shades itself: as the
            // drawn trees do. So the far colour takes more of the canopy than its cover, and darker.
            SurfaceMix seen = m;
            seen.cover = std::min(1.0, m.cover * 1.7);
            Rgb col = mixColour(seen);
            col = lerp(col, col * 0.75, seen.cover);
            // The voxels' shades average 0.84 of their class colours: the same here, so the two meet.
            col = col * 0.84;
            auto byte = [](double v8) { return uint32_t(std::clamp(int(std::lround(v8 * 255.0)), 0, 255)); };
            out.farColours[k] = byte(col.r) | (byte(col.g) << 8) | (byte(col.b) << 16) | (255u << 24);
        }
    out.sampleMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    out.fillMs = std::chrono::duration<double, std::milli>(clock::now() - t1).count();
    return out;
}

// How many voxels below its lowest neighbour a column is filled.
constexpr int kSkirt = 4;

inline ColumnResult generateColumn(const Landform& land, const Palette& palette, const PropLibrary* library, int ring,
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
    land.sampleGrid(x0 - 0.5 * v, z0 - 0.5 * v, v, n, n, v, samples);
    const auto t1 = clock::now();

    // Trees, in the rings fine enough to draw them: their trunks stand on a coarser grid of the ground
    // (aligned to the world, so neighbouring columns agree on every trunk) around the column.
    const bool stampTrees = v <= 2.0;
    std::vector<Tree> trees;
    if (stampTrees) {
        const double g = std::max(v, 1.0);
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
        // Chamisa is a prop in ring 0 (props.hpp), stamped in ring 1 (0.5 m voxels) as before.
        collectTrees(x0, z0, x0 + c, z0 + c, land.settings().seed, v <= 1.0, ring == 1, groundAt, trees);
        // Props read the same 1 m grid, interpolated (height and slope): it is the same grid in rings 0-2, so
        // they agree on every prop.
        if (library && ring <= 2) {
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
            collectProps(land.settings().seed, *library, ring, x0, z0, c, propGround, trees, out.props);
        }
    }

    // The top solid voxel of each column: a voxel is solid when its centre is under the ground.
    std::vector<int64_t> top(size_t(n) * size_t(n));
    for (size_t i = 0; i < top.size(); ++i) top[i] = int64_t(std::floor(samples[i].height / v - 0.5));
    auto topAt = [&](int i, int j) { return top[size_t(j) * size_t(n) + size_t(i)]; };

    struct ColumnFill { int64_t top, bottom; uint8_t surface, under; bool canopy; uint8_t ground; };
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
            f.surface = palette.at(m, shade);
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
            hi = std::max(hi, f.top);
        }
    }

    // The trees' voxels, in this column only.
    struct TreeVoxel { int i, j; int64_t y; uint8_t slot; };
    std::vector<TreeVoxel> treeVoxels;
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
