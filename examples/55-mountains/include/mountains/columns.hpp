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
#include "mountains/surface.hpp"
#include "utils/material.h"
#include "utils/tile_table.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

namespace mountains {

constexpr int kChunkRes = 64;

inline double voxelSize(int ring) { return 0.25 * double(1u << ring); }
inline double cellSize(int ring) { return voxelSize(ring) * kChunkRes; }

// Palette slots for every material and shade, the same in every ring's component.
struct Palette {
    std::array<std::array<uint8_t, kShades>, kMaterialCount> slot{};
    uint8_t at(Material m, int shade) const { return slot[size_t(m)][size_t(shade)]; }
};

struct ColumnChunk {
    int64_t iy = 0;   // cell, in the ring's cells (world y = iy * cellSize)
    projv::utils::TileTable tiles;
};

struct ColumnResult {
    int ring = 0;
    int64_t ix = 0, iz = 0;
    std::vector<ColumnChunk> chunks;
    double sampleMs = 0, fillMs = 0;
};

// How many voxels below its lowest neighbour a column is filled.
constexpr int kSkirt = 4;

inline ColumnResult generateColumn(const Landform& land, const Palette& palette, int ring, int64_t ix, int64_t iz) {
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

    // The top solid voxel of each column: a voxel is solid when its centre is under the ground.
    std::vector<int64_t> top(size_t(n) * size_t(n));
    for (size_t i = 0; i < top.size(); ++i) top[i] = int64_t(std::floor(samples[i].height / v - 0.5));
    auto topAt = [&](int i, int j) { return top[size_t(j) * size_t(n) + size_t(i)]; };

    struct ColumnFill { int64_t top, bottom; uint8_t surface, under; };
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
            Material m;
            int shade;
            // Patches a few metres across up close; per-voxel dither from 2 m voxels out.
            // Spread to roughly uniform on 0..1 (fBm piles up near its mean), so a weight picks the same
            // share of voxels here as the dither does in the next ring out: no line where they meet.
            const double coherent = clamp01(0.5 + 1.7 * fbm(x / 3.0, z / 3.0, 2, 101));
            pickMaterial(mixAt, hash2(ix * kChunkRes + i, iz * kChunkRes + j, uint32_t(ring) * 0x51ed27U + 9U),
                         coherent, smoothstep(0.5, 2.0, v), m, shade);
            f.surface = palette.at(m, shade);
            f.under = palette.at(subsurface(mixAt), std::clamp(int(mixAt.shade * kShades), 0, kShades - 1));
            lo = std::min(lo, f.bottom);
            hi = std::max(hi, f.top);
        }
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
                    projv::utils::brickMapSetVoxel(*map, i, int(y - base), j, y == f.top ? f.surface : f.under);
                    any = true;
                }
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
