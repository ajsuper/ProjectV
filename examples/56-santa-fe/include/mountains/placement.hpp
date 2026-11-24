// What stands on the ground -- trees, shrubs, rocks, grass, deadfall -- as one library of shapes, placed
// axis-aligned on the voxel lattice (santa-fe-reference S5).
//
// Every shape is built at the ground's own voxel (0.1 m; the tallest trees at a whole multiple of it), so it
// reads at the terrain's scale, and it turns only by quarter turns about y, so its voxels stay on the
// terrain's lattice. Variety is variants: each kind at a few sizes, a few seeds each; an instance takes the
// variant nearest the size its placement asks for.
//
// A placement is used two ways, by distance (the user: near things stay objects for game mechanics, far
// ones are baked to draw fast):
//
//   near   rings of 0.25 m voxels or finer: a loose chunk sharing the shape's blob (a tile table in a
//          power-of-4 cube), its corner on the lattice
//   far    coarser rings: stamped into the ring's own voxels, the shape resampled at the ring's voxel (a
//          voxel is solid when enough of the shape's voxels inside it are)
//
// Where things stand is a pure function of world position (as 55's props and trees), so every ring and
// column agrees on every placement.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "mountains/props.hpp"
#include "mountains/trees.hpp"

namespace mountains {

constexpr double kLattice = 0.1;   // the finest ring's voxel; columns.hpp kBaseVoxel

// A shape: a dense box of palette slots (0 empty, else slot + 1), `voxel` m a voxel, its anchor -- where it
// meets the ground: a trunk's foot, a stone's buried centre -- at voxel (ax, ay, az) of the box.
struct Shape {
    PropKind kind = PropKind::Boulder;
    Material material = Material::Granite;
    TreeKind tree = TreeKind::Juniper;
    int family = -1;       // a formation's rock family
    double size = 1.0;     // what an instance's size is compared with: width (stones, shrubs), length (logs), height (trees)
    double voxel = kLattice;
    int nx = 0, ny = 0, nz = 0;
    int ax = 0, ay = 0, az = 0;
    std::vector<uint8_t> v;
    int res = 16;          // the loose chunk's cube: the box at its corner
    uint8_t at(int x, int y, int z) const { return v[(size_t(z) * size_t(ny) + size_t(y)) * size_t(nx) + size_t(x)]; }
    // Coarser levels for stamping into coarse rings (level k: 2^k voxels a side), level 0 the box itself.
    std::vector<std::vector<uint8_t>> mip;
    std::vector<std::array<int, 3>> mipDims;
    uint8_t atLevel(int k, int x, int y, int z) const {
        const auto& d = mipDims[size_t(k)];
        if (x < 0 || y < 0 || z < 0 || x >= d[0] || y >= d[1] || z >= d[2]) return 0;
        return mip[size_t(k)][(size_t(z) * size_t(d[1]) + size_t(y)) * size_t(d[0]) + size_t(x)];
    }
};

// The pyramid: a coarse voxel is solid when at least a fifth of the finest voxels under it are (less for a
// shape too sparse for that at a level: see below), with the slot
// of a solid one under it. The fraction is carried down from level 0 (a float a voxel), not re-decided from
// the level above: "two of eight" decided level by level wore a sparse crown's outer layer away at every
// level, and by the 1.6 m ring a tree was a few blocks (then the far field's full crowns past it: trees,
// specks, trees, as the distance grew).
inline void buildMips(Shape& s) {
    constexpr float kSolid = 0.2f;
    s.mip.clear();
    s.mipDims.clear();
    s.mip.push_back(s.v);
    s.mipDims.push_back({s.nx, s.ny, s.nz});
    std::vector<float> density(s.v.size());
    std::vector<uint8_t> slot = s.v;   // any slot under a voxel, solid or not
    for (size_t i = 0; i < s.v.size(); ++i) density[i] = s.v[i] ? 1.0f : 0.0f;
    for (int k = 1; k < 8; ++k) {
        const auto d = s.mipDims.back();
        if (d[0] <= 1 && d[1] <= 1 && d[2] <= 1) break;
        const std::array<int, 3> e{(d[0] + 1) / 2, (d[1] + 1) / 2, (d[2] + 1) / 2};
        const size_t n = size_t(e[0]) * size_t(e[1]) * size_t(e[2]);
        std::vector<uint8_t> out(n, 0), slotOut(n, 0);
        std::vector<float> densOut(n, 0.0f);
        for (int z = 0; z < e[2]; ++z)
            for (int y = 0; y < e[1]; ++y)
                for (int x = 0; x < e[0]; ++x) {
                    float sum = 0.0f;
                    uint8_t solidSlot = 0, anySlot = 0;
                    for (int c = 0; c < 8; ++c) {
                        const int cx = 2 * x + (c & 1), cy = 2 * y + ((c >> 1) & 1), cz = 2 * z + (c >> 2);
                        if (cx >= d[0] || cy >= d[1] || cz >= d[2]) continue;
                        const size_t ci = (size_t(cz) * size_t(d[1]) + size_t(cy)) * size_t(d[0]) + size_t(cx);
                        sum += density[ci];
                        if (!anySlot) anySlot = slot[ci];
                        if (!solidSlot && s.mip.back()[ci]) solidSlot = s.mip.back()[ci];
                    }
                    const size_t oi = (size_t(z) * size_t(e[1]) + size_t(y)) * size_t(e[0]) + size_t(x);
                    densOut[oi] = sum / 8.0f;
                    slotOut[oi] = solidSlot ? solidSlot : anySlot;
                }
        // Solid at a fifth filled -- or, when even the shape's densest voxel at this level is under that (a 4 m
        // juniper in 3.2 m voxels), at 60% of that densest: every tree keeps its core at every level (the 3.2 m
        // ring was a barren band between trees nearer and the far field's crowns beyond).
        float densest = 0.0f;
        for (float dv : densOut) densest = std::max(densest, dv);
        const float solidAt = std::min(kSolid, 0.6f * densest);
        for (size_t oi = 0; oi < n; ++oi)
            if (densOut[oi] > 0.0f && densOut[oi] >= solidAt) out[oi] = slotOut[oi];
        density = std::move(densOut);
        slot = std::move(slotOut);
        s.mip.push_back(std::move(out));
        s.mipDims.push_back(e);
    }
}

struct ShapeLibrary {
    std::vector<Shape> shapes;
    std::vector<projv::utils::TileTable> tiles;   // parallel; moved into blobs by main.cpp
    std::array<std::array<std::vector<int>, kMaterialCount>, kPropKinds> variants;   // [kind][material]
    std::array<std::vector<int>, 9> trees;                                           // [TreeKind]
    std::array<std::vector<int>, 5> formations;                                      // [family]
    // The variant of `list` nearest `size` (by ratio), among the near ones picked by `pick` (0..1).
    int nearest(const std::vector<int>& list, double size, double pick) const {
        if (list.empty()) return -1;
        double best = 1e30;
        for (int i : list) best = std::min(best, std::abs(std::log(shapes[size_t(i)].size / size)));
        std::vector<int> closest;
        for (int i : list)
            if (std::abs(std::log(shapes[size_t(i)].size / size)) <= best + 0.12) closest.push_back(i);
        return closest[size_t(pick * double(closest.size())) % closest.size()];
    }
};

// The occupied box of `g`, with its anchor at grid point (ax, ay, az).
inline Shape cropShape(const props::Grid& g, double voxel, double ax, double ay, double az) {
    Shape s;
    int lo[3] = {g.res, g.res, g.res}, hi[3] = {-1, -1, -1};
    for (int z = 0; z < g.res; ++z)
        for (int y = 0; y < g.res; ++y)
            for (int x = 0; x < g.res; ++x)
                if (g.get(x, y, z) >= 0) {
                    lo[0] = std::min(lo[0], x); lo[1] = std::min(lo[1], y); lo[2] = std::min(lo[2], z);
                    hi[0] = std::max(hi[0], x); hi[1] = std::max(hi[1], y); hi[2] = std::max(hi[2], z);
                }
    if (hi[0] < 0) { lo[0] = lo[1] = lo[2] = 0; hi[0] = hi[1] = hi[2] = 0; }
    s.voxel = voxel;
    s.nx = hi[0] - lo[0] + 1; s.ny = hi[1] - lo[1] + 1; s.nz = hi[2] - lo[2] + 1;
    s.ax = int(std::lround(ax)) - lo[0]; s.ay = int(std::lround(ay)) - lo[1]; s.az = int(std::lround(az)) - lo[2];
    s.v.assign(size_t(s.nx) * size_t(s.ny) * size_t(s.nz), 0);
    for (int z = 0; z < s.nz; ++z)
        for (int y = 0; y < s.ny; ++y)
            for (int x = 0; x < s.nx; ++x) {
                const int16_t k = g.get(x + lo[0], y + lo[1], z + lo[2]);
                if (k >= 0) s.v[(size_t(z) * size_t(s.ny) + size_t(y)) * size_t(s.nx) + size_t(x)] = uint8_t(k + 1);
            }
    return s;
}

// The shape in the smallest power-of-4 cube that holds it, at its corner, as a tile table.
inline projv::utils::TileTable shapeTiles(Shape& s) {
    const int m = std::max({s.nx, s.ny, s.nz});
    s.res = m <= 16 ? 16 : (m <= 64 ? 64 : 256);
    auto map = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(s.res));
    for (int z = 0; z < std::min(s.nz, s.res); ++z)
        for (int y = 0; y < std::min(s.ny, s.res); ++y)
            for (int x = 0; x < std::min(s.nx, s.res); ++x)
                if (uint8_t k = s.at(x, y, z)) projv::utils::brickMapSetVoxel(*map, x, y, z, uint8_t(k - 1));
    projv::ChunkHeader hdr;
    hdr.position = projv::core::vec3(0.0f);
    hdr.scale = float(s.res);
    hdr.voxelScale = 1.0f;
    hdr.resolution = uint32_t(s.res);
    hdr.rotation = projv::core::quat(1, 0, 0, 0);
    projv::Chunk chunk = projv::utils::createChunk(hdr);
    projv::utils::updateChunkFromBrickMap(chunk, *map);
    std::vector<uint8_t> materials;
    projv::utils::bakeMaterialsFromBrickMap(chunk.geometryData, materials, *map);
    return projv::utils::buildTileTable(chunk.geometryData, materials, s.res);
}

// Every shape: the props.hpp makers built at a grid sized so their voxel is the lattice's, at a few sizes
// (world units, game scale included) and seeds; and the grown trees (`trees`, their slots mapped to the
// palette by `treeSlots[i][brush material]`). A few seconds, on every core.
inline ShapeLibrary buildShapeLibrary(uint32_t seed, const Palette& pal, const std::vector<TreeVoxels>& trees,
                                      const std::vector<std::vector<uint8_t>>& treeSlots) {
    struct Job {
        PropKind kind; Material material; double size; uint32_t seed; int style;
        double fill;   // the share of the grid the maker's shape spans (width; a log's length)
    };
    std::vector<Job> jobs;
    uint32_t n = seed * 0x68E31DA4U + 11U;
    auto next = [&]() { return n = hash32(n + 0x9e3779b9U); };
    const double G = kGameScale;
    for (Material rock : {Material::Granite, Material::Gneiss, Material::Basalt, Material::Sediment}) {
        for (double size : {0.8, 1.3, 2.0, 3.0})
            for (int i = 0; i < 2; ++i) jobs.push_back({PropKind::Boulder, rock, size * G, next(), i, 0.8});
        for (double size : {0.2, 0.35, 0.55})
            for (int i = 0; i < 2; ++i) jobs.push_back({PropKind::Cobble, rock, size * G, next(), i + 2, 0.8});
    }
    for (double size : {0.7, 1.0, 1.4})
        for (int i = 0; i < 3; ++i) jobs.push_back({PropKind::Chamisa, Material::Chamisa, size * G, next(), 0, 0.88});
    for (double size : {0.35, 0.5, 0.65})
        for (int i = 0; i < 2; ++i) jobs.push_back({PropKind::Snakeweed, Material::Snakeweed, size * G, next(), 0, 0.76});
    for (double size : {1.2, 1.8, 2.3})
        for (int i = 0; i < 2; ++i) jobs.push_back({PropKind::Cholla, Material::Cholla, size * G, next(), 0, 0.7});
    for (double size : {0.65, 0.9, 1.1})
        for (int i = 0; i < 2; ++i) jobs.push_back({PropKind::Yucca, Material::Yucca, size * G, next(), 0, 0.8});
    for (double size : {0.25, 0.35, 0.45})
        for (int i = 0; i < 4; ++i) jobs.push_back({PropKind::Grass, Material::Grama, size * G, next(), 0, 0.85});
    for (Material wood : {Material::Deadwood, Material::PonderosaBark, Material::Bark})
        for (double size : {4.0, 6.0, 8.5})
            for (int i = 0; i < 2; ++i) jobs.push_back({PropKind::Log, wood, size * G, next(), 0, 0.86});
    for (Material wood : {Material::Deadwood, Material::Bark})   // sticks and fallen branches: small logs
        for (double size : {0.7, 1.1, 1.7})
            for (int i = 0; i < 2; ++i) jobs.push_back({PropKind::Log, wood, size * G, next(), 0, 0.86});

    ShapeLibrary lib;
    lib.shapes.resize(jobs.size() + trees.size());
    {
        std::atomic<size_t> nextJob{0};
        std::vector<std::thread> workers;
        for (unsigned w = 0; w < std::max(2u, std::thread::hardware_concurrency()); ++w)
            workers.emplace_back([&] {
                for (size_t j; (j = nextJob++) < jobs.size();) {
                    const Job& job = jobs[j];
                    const int R = std::clamp(int(std::ceil(job.size / kLattice / job.fill)) + 2, 4, 250);
                    props::Grid g(R);
                    switch (job.kind) {
                        case PropKind::Boulder:
                        case PropKind::Cobble: props::makeStone(g, job.seed, job.style, job.material, pal); break;
                        case PropKind::Chamisa: props::makeChamisa(g, job.seed, pal); break;
                        case PropKind::Snakeweed: props::makeSnakeweed(g, job.seed, pal); break;
                        case PropKind::Cholla: props::makeCholla(g, job.seed, pal); break;
                        case PropKind::Yucca: props::makeYucca(g, job.seed, pal); break;
                        case PropKind::Grass: props::makeGrass(g, job.seed, pal); break;
                        case PropKind::Log: props::makeLog(g, job.seed, job.material, pal); break;
                        default: break;
                    }
                    // The anchor: the footprint's centre at the shape's lowest voxel (stones are sunk by the
                    // placement); a log's middle.
                    double sx = 0, sz = 0, cnt = 0;
                    int low = R;
                    for (int z = 0; z < R; ++z)
                        for (int y = 0; y < R; ++y)
                            for (int x = 0; x < R; ++x)
                                if (g.get(x, y, z) >= 0) low = std::min(low, y);
                    for (int z = 0; z < R; ++z)
                        for (int x = 0; x < R; ++x)
                            for (int y = low; y <= std::min(R - 1, low + 2); ++y)
                                if (g.get(x, y, z) >= 0) { sx += x; sz += z; cnt += 1; }
                    Shape s = cropShape(g, kLattice, cnt > 0 ? sx / cnt : R * 0.5, low, cnt > 0 ? sz / cnt : R * 0.5);
                    s.kind = job.kind;
                    s.material = job.material;
                    s.size = job.kind == PropKind::Log ? std::max(s.nx, s.nz) * kLattice : std::max(s.nx, s.nz) * kLattice;
                    lib.shapes[j] = std::move(s);
                }
            });
        for (auto& w : workers) w.join();
    }
    for (size_t i = 0; i < trees.size(); ++i) {
        const TreeVoxels& t = trees[i];
        int reach = 0, below = 0;
        for (const auto& p : t.pos) {
            reach = std::max({reach, std::abs(int(p[0])), std::abs(int(p[2]))});
            below = std::max(below, -int(p[1]));
        }
        const int R = std::min(2 * reach + 3, 255);
        Shape s;
        s.kind = t.family >= 0 ? PropKind::Formation : PropKind::Tree;
        s.family = t.family;
        s.material = Material::Bark;
        s.tree = t.kind;
        s.voxel = t.voxel;
        s.size = double(std::max(t.height, 1)) * t.voxel;
        s.nx = s.nz = R;
        s.ny = std::min(t.height + below + 3, 255);
        s.ax = s.az = R / 2;
        s.ay = below;
        s.v.assign(size_t(s.nx) * size_t(s.ny) * size_t(s.nz), 0);
        for (size_t k = 0; k < t.pos.size(); ++k) {
            const int x = s.ax + t.pos[k][0], y = s.ay + t.pos[k][1], z = s.az + t.pos[k][2];
            if (x < 0 || y < 0 || z < 0 || x >= s.nx || y >= s.ny || z >= s.nz) continue;
            if (t.slot[k] >= treeSlots[i].size()) continue;
            s.v[(size_t(z) * size_t(s.ny) + size_t(y)) * size_t(s.nx) + size_t(x)] = uint8_t(treeSlots[i][t.slot[k]] + 1);
        }
        lib.shapes[jobs.size() + i] = std::move(s);
    }
    for (size_t i = 0; i < lib.shapes.size(); ++i) {
        Shape& s = lib.shapes[i];
        buildMips(s);
        lib.tiles.push_back(shapeTiles(s));
        if (s.kind == PropKind::Tree) lib.trees[size_t(s.tree)].push_back(int(i));
        else if (s.kind == PropKind::Formation) lib.formations[size_t(std::clamp(s.family, 0, 4))].push_back(int(i));
        else lib.variants[size_t(s.kind)][size_t(s.material)].push_back(int(i));
    }
    return lib;
}

// One placement: a shape, its anchor (world), a quarter turn about y.
struct Placement {
    int shape = 0;
    double x = 0, y = 0, z = 0;
    int turn = 0;
};

// The shape's box corner offsets in world units under a quarter turn: local (x, z) -> world.
inline void turnXZ(int turn, double x, double z, double& wx, double& wz) {
    switch (turn & 3) {
        case 0: wx = x; wz = z; break;
        case 1: wx = z; wz = -x; break;
        case 2: wx = -x; wz = -z; break;
        default: wx = -z; wz = x; break;
    }
}

// The rotation turnXZ applies, as a quaternion (props.hpp's: the loose chunk's).
inline props::Q turnQuat(int turn) { return props::axisAngle({0, 1, 0}, kPi * 0.5 * double(turn & 3)); }

// A placement as a loose chunk (near rings): its corner, rotation and edge.
inline PropInstance looseInstance(const ShapeLibrary& lib, const Placement& p) {
    const Shape& s = lib.shapes[size_t(p.shape)];
    const props::Q q = turnQuat(p.turn);
    const props::V3 anchor{double(s.ax) * s.voxel, double(s.ay) * s.voxel, double(s.az) * s.voxel};
    const props::V3 corner = props::V3{p.x, p.y, p.z} - props::rotate(q, anchor);
    PropInstance out;
    out.shape = p.shape;
    out.px = corner.x; out.py = corner.y; out.pz = corner.z;
    out.qw = q.w; out.qx = q.x; out.qy = q.y; out.qz = q.z;
    out.size = s.voxel * double(s.res);
    return out;
}

// Stamps `p` into the voxels of a ring of voxel `v` inside [x0, x0 + c) x [z0, z0 + c): `write(i, j, y, slot)`
// with i, j the column's voxel and y the ring's voxel row. A ring voxel takes the shape's slot when at least
// a quarter of the shape's voxels inside it are solid (the centre alone where the shape is the coarser).
template <typename Write>
inline void stampPlacement(const ShapeLibrary& lib, const Placement& p, double v, double x0, double z0, double c, Write&& write) {
    const Shape& s = lib.shapes[size_t(p.shape)];
    const props::Q qi = turnQuat(-p.turn);
    // The box's world extent.
    double lx[2] = {-double(s.ax) * s.voxel, double(s.nx - s.ax) * s.voxel};
    double lz[2] = {-double(s.az) * s.voxel, double(s.nz - s.az) * s.voxel};
    double minX = 1e30, maxX = -1e30, minZ = 1e30, maxZ = -1e30;
    for (double a : lx)
        for (double b : lz) {
            double wx, wz;
            turnXZ(p.turn, a, b, wx, wz);
            minX = std::min(minX, p.x + wx); maxX = std::max(maxX, p.x + wx);
            minZ = std::min(minZ, p.z + wz); maxZ = std::max(maxZ, p.z + wz);
        }
    const double minY = p.y - double(s.ay) * s.voxel, maxY = p.y + double(s.ny - s.ay) * s.voxel;
    const int i0 = std::max(0, int(std::floor((minX - x0) / v))), i1 = std::min(int(std::lround(c / v)) - 1, int(std::floor((maxX - x0) / v)));
    const int j0 = std::max(0, int(std::floor((minZ - z0) / v))), j1 = std::min(int(std::lround(c / v)) - 1, int(std::floor((maxZ - z0) / v)));
    const int64_t y0 = int64_t(std::floor(minY / v)), y1 = int64_t(std::floor(maxY / v));
    if (i0 > i1 || j0 > j1) return;
    // The pyramid level nearest the ring's voxel: one sample a ring voxel (sub-sampling the full shape cost a
    // coarse column 200 ms).
    const int level = std::clamp(int(std::floor(std::log2(v / s.voxel) + 0.25)), 0, int(s.mip.size()) - 1);
    const double sv = s.voxel * double(1 << level);
    const double ax = double(s.ax) / double(1 << level), ay = double(s.ay) / double(1 << level), az = double(s.az) / double(1 << level);
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            for (int64_t y = y0; y <= y1; ++y) {
                const props::V3 w{x0 + (double(i) + 0.5) * v - p.x, (double(y) + 0.5) * v - p.y, z0 + (double(j) + 0.5) * v - p.z};
                const props::V3 l = props::rotate(qi, w);
                const uint8_t k = s.atLevel(level, int(std::floor(l.x / sv + ax)), int(std::floor(l.y / sv + ay)), int(std::floor(l.z / sv + az)));
                if (k) write(i, j, y, uint8_t(k - 1));
            }
}

// Snaps a world coordinate to the lattice (a corner of the finest ring's voxels).
inline double snap(double x) { return std::floor(x / kLattice + 0.5) * kLattice; }
// The top of the voxel ground at height h, for a ring of voxel v.
inline double voxelGround(double h, double v) { return (std::floor(h / v - 0.5) + 1.0) * v; }

// The trees around a column as placements: each the variant of its kind nearest its height, turned at
// random, its foot on the ring's voxel ground. False where the library has no tree of a kind (the caller
// stamps those as 55 did).
inline void treePlacements(const ShapeLibrary& lib, double v, const std::vector<Tree>& trees, std::vector<Placement>& out,
                           std::vector<Tree>& unplaced) {
    for (const Tree& t : trees) {
        const int shape = lib.nearest(lib.trees[size_t(t.kind)], t.height, unit(hash32(t.seed + 0x51ed27U)));
        if (shape < 0) { unplaced.push_back(t); continue; }
        Placement p;
        p.shape = shape;
        p.x = snap(t.x);
        p.z = snap(t.z);
        p.y = voxelGround(t.base, kLattice) - 0.1;   // the trunk a little into the ground
        p.turn = int(hash32(t.seed + 7U) & 3U);
        out.push_back(p);
    }
}

// The small things (55's props, game-scaled) around [x0, x0 + c) x [z0, z0 + c) as placements: a jittered
// grid per kind, a density from the exact landform and surface mix at the anchor. All whose shape could
// reach the column (a margin of `margin` m); `anchoredOnly` keeps only those anchored inside it (a loose
// object belongs to one column).
template <typename Ground>
inline void propPlacements(uint32_t seed, const ShapeLibrary& lib, double v, double x0, double z0, double c, double margin,
                           bool anchoredOnly, Ground&& ground, const std::vector<Tree>& trees, std::vector<Placement>& out) {
    struct Rule { PropKind kind; double cell; uint32_t salt; };
    const double G = kGameScale;
    static const Rule rules[] = {
        {PropKind::Boulder, 5.0 * kGameScale, 101U}, {PropKind::Cobble, 1.6 * kGameScale, 102U},
        {PropKind::Chamisa, 3.0 * kGameScale, 103U}, {PropKind::Snakeweed, 2.0 * kGameScale, 104U},
        {PropKind::Cholla, 7.0 * kGameScale, 105U},  {PropKind::Yucca, 5.0 * kGameScale, 106U},
        {PropKind::Log, 11.0 * kGameScale, 107U},    {PropKind::Grass, 0.5 * kGameScale, 108U},
        {PropKind::Formation, 30.0 * kGameScale, 109U},
        {PropKind::Boulder, 1.6 * kGameScale, 110U},   // the rock faces, built of boulders
        {PropKind::Grass, 1.1 * kGameScale, 111U},     // plants in the rock's joints
        {PropKind::Log, 2.5 * kGameScale, 112U},       // sticks and fallen branches under the trees
    };
    // The voxel ground's height at (x, z): the landform and the rock blocks (surface.hpp rockBlocks).
    auto groundTop = [&](double x, double z, double fallback) {
        Sample s;
        if (!ground(x, z, s)) return fallback;
        return s.height + rockBlocks(x, z, rockWeight(s), kLattice).height;
    };
    // The lowest ground under a shape's footprint (nine points over its turned box): it rests on that, so its
    // downhill side does not float.
    auto lowestUnder = [&](const Shape& sh, double ax, double az, int turn, double centre) {
        double lowest = centre;
        const double hx = 0.45 * double(sh.nx) * sh.voxel, hz = 0.45 * double(sh.nz) * sh.voxel;
        for (int c = 0; c < 9; ++c) {
            if (c == 4) continue;
            double wx, wz;
            turnXZ(turn, double(c % 3 - 1) * hx, double(c / 3 - 1) * hz, wx, wz);
            lowest = std::min(lowest, groundTop(ax + wx, az + wz, centre));
        }
        return lowest;
    };
    for (const Rule& rule : rules) {
        // A kind whose largest is under two of this ring's voxels draws nothing here: skip its whole grid
        // (grass looped over 27,000 cells of a coarse column to place none).
        double largest = 1e9;
        switch (rule.kind) {
            case PropKind::Grass: largest = 0.47 * G; break;
            case PropKind::Cobble: largest = 0.58 * G; break;
            case PropKind::Snakeweed: largest = 0.65 * G; break;
            case PropKind::Yucca: largest = 1.1 * G; break;
            case PropKind::Chamisa: largest = 1.6 * G; break;
            case PropKind::Cholla: largest = 2.3 * G; break;
            default: break;
        }
        if (rule.salt == 112U) largest = 1.8 * G;   // sticks
        if (largest / v < 2.0) continue;
        const double lo = anchoredOnly ? 0.0 : margin;
        const int64_t i0 = int64_t(std::floor((x0 - lo) / rule.cell)), i1 = int64_t(std::floor((x0 + c + lo) / rule.cell));
        const int64_t j0 = int64_t(std::floor((z0 - lo) / rule.cell)), j1 = int64_t(std::floor((z0 + c + lo) / rule.cell));
        for (int64_t j = j0; j <= j1; ++j)
            for (int64_t i = i0; i <= i1; ++i) {
                const uint32_t h = hash2(i, j, seed * 0x2545F491U + rule.salt);
                auto u = [&](int k) { return unit(hash32(h + uint32_t(k) * 0x9e3779b9U)); };
                const double ax = (double(i) + u(1)) * rule.cell, az = (double(j) + u(2)) * rule.cell;
                if (anchoredOnly && (ax < x0 || ax >= x0 + c || az < z0 || az >= z0 + c)) continue;
                if (!anchoredOnly && (ax < x0 - margin || ax >= x0 + c + margin || az < z0 - margin || az >= z0 + c + margin)) continue;
                Sample s;
                if (!ground(ax, az, s)) continue;
                const SurfaceMix m = surfaceMix(s, ax, az, 100.0);
                if (m.water > 0.3) continue;
                const double slope = std::sqrt(s.gx * s.gx + s.gz * s.gz);
                const double e = s.height + rockBlocks(ax, az, rockWeight(s), kLattice).height;
                double density = 0.0, size = 1.0;
                Material material = Material::Granite;
                const bool stone = rule.kind == PropKind::Boulder || rule.kind == PropKind::Cobble;
                if (stone) {
                    material = s.basalt > 0.5 ? Material::Basalt
                             : (s.core > 0.5 ? (m.rockMix > u(4) ? m.rockB : m.rockA)
                                             : (s.badland > 0.4 ? Material::Sediment : (u(4) < 0.6 ? Material::Granite : Material::Gneiss)));
                    if (material != Material::Granite && material != Material::Gneiss && material != Material::Basalt)
                        material = Material::Sediment;
                }
                if (m.snow > 0.6 && !stone) continue;
                // Plants rooted in the rock's joints (santa-fe-reference S9): grass, snakeweed, chamisa, now and then
                // a small juniper or pinyon -- only where the anchor falls in a crack between bare blocks.
                if (rule.salt == 111U) {
                    const double rw = rockWeight(s);
                    if (rw < 0.2 || m.snow > 0.5) continue;
                    const RockBlock rb = rockBlocks(ax, az, rw, kLattice);
                    if (rb.crack < 0.35 || u(7) >= 0.75) continue;
                    const double pick = u(4);
                    int shape = -1;
                    if (pick < 0.5) shape = lib.nearest(lib.variants[size_t(PropKind::Grass)][size_t(Material::Grama)], (0.3 + 0.2 * u(5)) * G, u(8));
                    else if (pick < 0.72) shape = lib.nearest(lib.variants[size_t(PropKind::Snakeweed)][size_t(Material::Snakeweed)], 0.45 * G, u(8));
                    else if (pick < 0.9) shape = lib.nearest(lib.variants[size_t(PropKind::Chamisa)][size_t(Material::Chamisa)], 0.8 * G, u(8));
                    else shape = lib.nearest(lib.trees[size_t(u(5) < 0.6 ? TreeKind::Juniper : TreeKind::Pinyon)], 2.5 * G, u(8));
                    if (shape < 0) continue;
                    const Shape& sh = lib.shapes[size_t(shape)];
                    if (std::max(sh.nx, sh.nz) * sh.voxel / v < 2.0) continue;
                    Placement p;
                    p.shape = shape;
                    p.x = snap(ax);
                    p.z = snap(az);
                    p.turn = int(hash32(h + 11U) & 3U);
                    p.y = std::floor((voxelGround(e, kLattice) - 0.1) / kLattice + 0.5) * kLattice;
                    out.push_back(p);
                    continue;
                }
                // Exposed rock -- cliffs, steep faces, cirques, basalt scarps -- built of boulders (the user):
                // packed close, large, sunk deep into the slope, so the face reads as stacked blocks.
                if (rule.salt == 110U) {
                    const double face = std::max(smoothstep(0.3, 0.75, m.rock), s.basalt * smoothstep(0.25, 0.6, slope));
                    if (face <= 0.0 || u(7) >= 0.9 * face) continue;
                    if (s.basalt <= 0.5 && s.core <= 0.5 && material == Material::Sediment && u(9) < 0.3) material = Material::Granite;
                    const double sz = (1.1 + 2.4 * std::pow(u(5), 1.6)) * G;
                    if (sz / v < 2.5) continue;
                    const int shape = lib.nearest(lib.variants[size_t(PropKind::Boulder)][size_t(material)], sz, u(8));
                    if (shape < 0) continue;
                    const Shape& sh = lib.shapes[size_t(shape)];
                    const double height = double(sh.ny) * sh.voxel;
                    Placement p;
                    p.shape = shape;
                    p.x = snap(ax);
                    p.z = snap(az);
                    p.turn = int(hash32(h + 11U) & 3U);
                    p.y = std::floor((voxelGround(lowestUnder(sh, ax, az, p.turn, e), kLattice) - height * (0.15 + 0.2 * u(12))) / kLattice + 0.5) * kLattice;
                    out.push_back(p);
                    continue;
                }
                switch (rule.kind) {
                    case PropKind::Boulder:
                        density = 0.0025 * s.core + 0.035 * m.rock + 0.06 * s.basalt * smoothstep(0.15, 0.6, slope) +
                                  0.03 * s.cirque + 0.004 * smoothstep(0.3, 0.8, slope);
                        density /= G * G;
                        size = (0.7 + 2.8 * std::pow(u(5), 2.4)) * G;
                        break;
                    case PropKind::Cobble:
                        density = 0.05 + 0.3 * m.rock + 0.15 * smoothstep(0.2, 0.8, slope) + 0.35 * s.basalt * smoothstep(0.1, 0.5, slope);
                        if (s.core < 0.5 && s.basalt < 0.5) density *= 0.6;
                        density /= G * G;
                        size = (0.18 + 0.4 * std::pow(u(5), 2.0)) * G;
                        break;
                    case PropKind::Chamisa: {
                        if (m.shrub != Material::Chamisa) continue;
                        size = (0.6 + 0.8 * u(5) * u(5) + 0.2 * u(6)) * G;
                        density = 1.5 * m.understory / (kPi * 0.25 * size * size * 0.8);
                        material = Material::Chamisa;
                        break;
                    }
                    case PropKind::Snakeweed:
                        density = 0.14 * (1.0 - smoothstep(2200.0, 2400.0, e)) * (1.0 - m.rock) * (1.0 - m.cover) *
                                  clamp01(0.5 + 1.5 * fbm(ax / 90.0, az / 90.0, 2, 141)) / (G * G);
                        size = (0.3 + 0.35 * u(5)) * G;
                        material = Material::Snakeweed;
                        break;
                    case PropKind::Cholla:
                        density = 0.003 * (1.0 - smoothstep(2080.0, 2220.0, e)) * (1.0 - m.rock) *
                                  clamp01(0.4 + 1.5 * fbm(ax / 300.0, az / 300.0, 2, 142)) / (G * G);
                        size = (1.1 + 1.2 * u(5)) * G;
                        material = Material::Cholla;
                        break;
                    case PropKind::Yucca:
                        density = 0.006 * (1.0 - smoothstep(2250.0, 2450.0, e)) * (1.0 - m.rock) *
                                  clamp01(0.4 + 1.5 * fbm(ax / 200.0, az / 200.0, 2, 143)) / (G * G);
                        size = (0.6 + 0.5 * u(5)) * G;
                        material = Material::Yucca;
                        break;
                    case PropKind::Log: {
                        const Material tree = m.treeMix > 0.5 ? m.treeB : m.treeA;
                        const bool sticks = rule.salt == 112U;
                        if (sticks) {
                            // Sticks and fallen branches wherever there are trees to drop them, thick under them.
                            if (m.cover < 0.03) continue;
                            material = u(4) < 0.6 ? Material::Deadwood : Material::Bark;
                            density = 0.045 * clamp01(m.cover / 0.4) * (1.0 - smoothstep(0.8, 1.1, slope)) / (G * G);
                            size = (0.6 + 1.2 * u(5) * u(5)) * G;
                            break;
                        }
                        if (tree == Material::Ponderosa) material = u(4) < 0.5 ? Material::PonderosaBark : Material::Deadwood;
                        else if (tree == Material::Fir || tree == Material::Spruce || tree == Material::AspenGold || tree == Material::AspenBare)
                            material = u(4) < 0.4 ? Material::Bark : Material::Deadwood;
                        else if (tree == Material::Juniper || tree == Material::Pinyon) material = Material::Deadwood;   // silver juniper deadwood
                        else continue;
                        density = 0.004 * clamp01(m.cover / 0.4) * (1.0 - smoothstep(0.7, 1.0, slope)) / (G * G);   // was 0.0022, conifers only
                        size = (tree == Material::Juniper || tree == Material::Pinyon ? 2.0 + 2.5 * u(5) : 3.5 + 5.5 * u(5)) * G;
                        break;
                    }
                    case PropKind::Grass: {
                        const double grassy = m.groundB == Material::Grama || m.groundB == Material::Tundra ? m.groundMix : 0.0;
                        density = (0.35 + 0.9 * grassy) * (1.0 - m.rock) * (1.0 - 0.6 * m.cover) * (1.0 - smoothstep(0.6, 1.0, slope)) *
                                  clamp01(0.45 + 1.4 * fbm(ax / 9.0, az / 9.0, 2, 144)) / (G * G);
                        size = (0.22 + 0.25 * u(5)) * G;
                        material = Material::Grama;
                        break;
                    }
                    case PropKind::Formation: {
                        // Boulder formations by the rock: tors and splits on the granite cores (on slopes and
                        // ridges more), basalt on the mesa scarps, sandstone in the badlands; an erratic now and
                        // then anywhere (santa-fe-reference S6).
                        const int family = s.basalt > 0.5 ? 2 : (s.badland > 0.4 ? 3 : (s.core > 0.5 ? (u(13) < 0.25 ? 1 : 0) : 4));
                        const double p = family == 2 ? 0.55 * smoothstep(0.1, 0.5, slope) + 0.1
                                       : family == 3 ? 0.3
                                       : family == 4 ? 0.06
                                       : 0.18 + 0.4 * smoothstep(0.25, 0.8, slope) + 0.3 * m.rock;
                        if (u(7) >= p) continue;
                        const std::vector<int>& pick = lib.formations[size_t(family)].empty() ? lib.formations[4] : lib.formations[size_t(family)];
                        if (pick.empty()) continue;
                        Placement f;
                        f.shape = pick[size_t(u(8) * double(pick.size())) % pick.size()];
                        f.x = snap(ax);
                        f.z = snap(az);
                        f.turn = int(hash32(h + 11U) & 3U);
                        f.y = voxelGround(lowestUnder(lib.shapes[size_t(f.shape)], ax, az, f.turn, e), kLattice);
                        out.push_back(f);
                        continue;
                    }
                    default: continue;
                }
                // The density is per (game-scaled) square metre of the player's.
                if (u(7) >= density * rule.cell * rule.cell) continue;
                // Too small to show in this ring: under ~2.5 of its voxels.
                if (size / v < (rule.kind == PropKind::Grass ? 2.0 : 2.5)) continue;
                const int shape = lib.nearest(lib.variants[size_t(rule.kind)][size_t(material)], size, u(8));
                if (shape < 0) continue;
                bool blocked = false;
                const double reach = rule.kind == PropKind::Log ? 0.15 * size : 0.4 * size;
                for (const Tree& t : trees) {
                    if (t.trunkR <= 0.0 || t.kind == TreeKind::Oak || t.kind == TreeKind::Chamisa) continue;
                    const double dx = t.x - ax, dz = t.z - az;
                    const double lim = t.trunkR + reach + (rule.kind == PropKind::Log ? 0.5 * size : 0.0);
                    if (dx * dx + dz * dz < lim * lim) { blocked = true; break; }
                }
                if (blocked) continue;
                const Shape& sh = lib.shapes[size_t(shape)];
                const double height = double(sh.ny) * sh.voxel;
                Placement p;
                p.shape = shape;
                p.x = snap(ax);
                p.z = snap(az);
                p.turn = int(hash32(h + 11U) & 3U);
                // Stones and logs rest on the lowest ground under them, a little into it; small plants on the
                // ground at their centre.
                const bool rests = stone || rule.kind == PropKind::Log;
                const double base = rests && size > 0.5 * kGameScale ? lowestUnder(sh, ax, az, p.turn, e) : e;
                double sink;
                if (stone) sink = height * (0.08 + 0.15 * u(12));
                else if (rule.kind == PropKind::Log) sink = height * 0.2;
                else sink = slope * size * 0.2;
                p.y = std::floor((voxelGround(base, kLattice) - sink) / kLattice + 0.5) * kLattice;
                out.push_back(p);
            }
    }
}

}  // namespace mountains
