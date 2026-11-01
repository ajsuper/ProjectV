// Fluid prototype -- quick and scrappy, to see what grid-aligned water looks like in a voxel world.
//
// A FLIP liquid (particles carrying velocity, a MAC grid for the pressure solve) on a 64^3 grid that is
// the voxel chunk's own grid: cell (x, y, z) is voxel (x, y, z), so solids are just the terrain's voxels.
// Every frame the cells holding water are written into a second chunk laid over the terrain, through
// the engine's ordinary edit queue, and drawn by the basic renderer: deep water dark, the surface
// light, fast water white.
//
//   E      break the dam              G      drop a block of water
//   X      dig a trench across        T      start over
//   Space  pause                      WASD/RF, mouse: fly
//
// Not engine code, not a plan: CPU, single-threaded, no determinism, nothing networked.

#include <algorithm>
#include <barrier>
#include <thread>
#include <chrono>
#include <cmath>
#include <random>
#include <cstdlib>
#include <string>
#include <vector>

#include "basic_renderer.h"
#include "core/application.h"
#include "core/log.h"
#include "graphics/input.h"
#include "graphics/render_instance.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/voxel_management.h"
#include "utils/blob_voxels.h"
#include "core/paths.h"

namespace {

constexpr int N = 64;                       // the chunk's and the grid's cells per side
constexpr float VOXEL = 0.25f;              // world units (metres) per voxel
constexpr float GRAVITY = 9.8f / VOXEL;     // in voxels / s^2
constexpr float FLIP = 0.95f;               // FLIP / PIC blend
constexpr int PRESSURE_ITERATIONS = 60;
constexpr uint8_t EMPTY = projv::INVALID_MATERIAL;   // "no voxel" in a slot array: slot 0 is a real material

inline int cellIndex(int x, int y, int z) { return x + N * (y + N * z); }

const int THREADS = std::max(1, int(std::thread::hardware_concurrency()));

// fn(lo, hi) over [begin, end), split across the cores.
template <typename Fn>
void parallelFor(int begin, int end, Fn fn) {
    const int n = end - begin;
    if (n <= 0) return;
    const int chunks = std::min(THREADS, n);
    std::vector<std::thread> pool;
    for (int t = 0; t < chunks; t++) pool.emplace_back([=, &fn] { fn(begin + n * t / chunks, begin + n * (t + 1) / chunks); });
    for (std::thread& t : pool) t.join();
}

// ---------------------------------------------------------------------------------------------
// The terrain: a valley floor sloping away from a walled reservoir, a dam, and some rocks.
// ---------------------------------------------------------------------------------------------

struct Terrain {
    std::vector<uint8_t> solid = std::vector<uint8_t>(N * N * N, 0);
    std::vector<uint8_t> colour = std::vector<uint8_t>(N * N * N, 0);   // 1 rock, 2 grass, 3 dam
    bool at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= N || y >= N || z >= N) return true;   // the box's walls
        return solid[cellIndex(x, y, z)] != 0;
    }
};

constexpr int DAM_X0 = 22, DAM_X1 = 24;     // the dam's thickness, x in [DAM_X0, DAM_X1)

Terrain buildTerrain() {
    Terrain t;
    auto set = [&](int x, int y, int z, uint8_t c) {
        if (x < 0 || y < 0 || z < 0 || x >= N || y >= N || z >= N) return;
        t.solid[cellIndex(x, y, z)] = 1;
        t.colour[cellIndex(x, y, z)] = c;
    };
    for (int z = 0; z < N; z++) {
        for (int x = 0; x < N; x++) {
            // The floor falls away from the reservoir, with a gentle channel down the middle.
            float h = 10.0f - 0.10f * float(std::max(0, x - DAM_X1)) +
                      2.5f * std::abs(float(z - N / 2)) / float(N / 2) +
                      1.2f * std::sin(x * 0.31f) * std::cos(z * 0.27f);
            int top = std::max(2, int(h));
            for (int y = 0; y < top; y++) set(x, y, z, y == top - 1 ? 2 : 1);
        }
    }
    // The reservoir's walls (x < DAM_X0, the box's edges close the rest), and the dam.
    for (int y = 0; y < 32; y++) {   // just over the water's surface (y 30)
        for (int z = 0; z < N; z++) {
            for (int x = DAM_X0; x < DAM_X1; x++) set(x, y, z, 3);
        }
    }
    // Rocks in the valley for the flood to break around.
    std::mt19937 rng(7);
    for (int i = 0; i < 9; i++) {
        int cx = 30 + int(rng() % 30), cz = 6 + int(rng() % 52), r = 2 + int(rng() % 3);
        for (int z = cz - r; z <= cz + r; z++)
            for (int x = cx - r; x <= cx + r; x++)
                for (int y = 0; y < 13 + r; y++)
                    if ((x - cx) * (x - cx) + (z - cz) * (z - cz) + (y - 9) * (y - 9) / 2 <= r * r) set(x, y, z, 1);
    }
    return t;
}

// ---------------------------------------------------------------------------------------------
// FLIP on a MAC grid. Velocities in voxels / s. u lives on x faces, v on y faces, w on z faces.
// ---------------------------------------------------------------------------------------------

struct Particle { float px, py, pz, vx, vy, vz; };

struct Fluid {
    std::vector<Particle> particles;
    std::vector<float> u, v, w, uw, vw, ww, u0, v0, w0;    // face velocities, their weights, and the pre-solve copy
    std::vector<float> pressure, divergence;
    std::vector<uint8_t> fluid;                            // cell holds liquid this step
    std::vector<uint16_t> count;                           // particles per cell
    std::vector<float> speed;                              // summed |v| per cell, for foam

    Fluid() {
        size_t faces = size_t(N + 1) * N * N;
        for (auto* f : {&u, &v, &w, &uw, &vw, &ww, &u0, &v0, &w0}) f->assign(faces, 0.0f);
        pressure.assign(N * N * N, 0.0f);
        divergence.assign(N * N * N, 0.0f);
        fluid.assign(N * N * N, 0);
        count.assign(N * N * N, 0);
        speed.assign(N * N * N, 0.0f);
    }

    // Face indices. u face (i, j, k) sits at x = i, between cells i-1 and i; i in [0, N].
    static int uIndex(int i, int j, int k) { return i + (N + 1) * (j + N * k); }
    static int vIndex(int i, int j, int k) { return j + (N + 1) * (i + N * k); }   // j in [0, N]
    static int wIndex(int i, int j, int k) { return k + (N + 1) * (i + N * j); }   // k in [0, N]

    void addBlock(int x0, int y0, int z0, int x1, int y1, int z1, const Terrain& t, std::mt19937& rng) {
                for (int z = z0; z < z1; z++)
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    if (t.at(x, y, z)) continue;
                    for (int s = 0; s < 4; s++) {   // four per cell, jittered over it
                        std::uniform_real_distribution<float> any(0.05f, 0.95f);
                        particles.push_back({x + any(rng), y + any(rng), z + any(rng), 0, 0, 0});
                    }
                }
    }

    // Trilinear splat / gather on one staggered component. (ox, oy, oz) is that component's offset
    // from the cell corner: u faces are at (0, .5, .5), v at (.5, 0, .5), w at (.5, .5, 0).
    template <typename Index, typename Fn>
    static void stencil(float x, float y, float z, float ox, float oy, float oz, int nx, int ny, int nz,
                        Index index, Fn fn) {
        float gx = x - ox, gy = y - oy, gz = z - oz;
        int i = int(std::floor(gx)), j = int(std::floor(gy)), k = int(std::floor(gz));
        float fx = gx - i, fy = gy - j, fz = gz - k;
        for (int c = 0; c < 8; c++) {
            int ii = i + (c & 1), jj = j + ((c >> 1) & 1), kk = k + ((c >> 2) & 1);
            if (ii < 0 || jj < 0 || kk < 0 || ii >= nx || jj >= ny || kk >= nz) continue;
            float wgt = ((c & 1) ? fx : 1 - fx) * (((c >> 1) & 1) ? fy : 1 - fy) * (((c >> 2) & 1) ? fz : 1 - fz);
            fn(index(ii, jj, kk), wgt);
        }
    }

    float sampleU(const std::vector<float>& f, float x, float y, float z) const {
        float s = 0; stencil(x, y, z, 0, .5f, .5f, N + 1, N, N, uIndex, [&](int i, float wt) { s += f[i] * wt; }); return s;
    }
    float sampleV(const std::vector<float>& f, float x, float y, float z) const {
        float s = 0; stencil(x, y, z, .5f, 0, .5f, N, N + 1, N, [](int i, int j, int k) { return vIndex(i, j, k); },
                             [&](int i, float wt) { s += f[i] * wt; }); return s;
    }
    float sampleW(const std::vector<float>& f, float x, float y, float z) const {
        float s = 0; stencil(x, y, z, .5f, .5f, 0, N, N, N + 1, [](int i, int j, int k) { return wIndex(i, j, k); },
                             [&](int i, float wt) { s += f[i] * wt; }); return s;
    }

    void step(float dt, const Terrain& t) {
        // 1. Particles to grid.
        for (auto* f : {&u, &v, &w, &uw, &vw, &ww}) std::fill(f->begin(), f->end(), 0.0f);
        std::fill(count.begin(), count.end(), 0);
        std::fill(speed.begin(), speed.end(), 0.0f);
        // Bucketed by their cell's z, then splatted slab by slab: a slab of 4 cells writes faces within
        // one cell of itself, so every other slab can run at once without two threads on one face.
        constexpr int SLAB = 4, SLABS = N / SLAB;
        std::vector<int> start(N + 1, 0);
        for (const Particle& p : particles) start[std::clamp(int(p.pz), 0, N - 1) + 1]++;
        for (int z = 0; z < N; z++) start[z + 1] += start[z];
        std::vector<int> order(particles.size());
        {
            std::vector<int> next(start.begin(), start.end() - 1);
            for (int i = 0; i < int(particles.size()); i++) order[next[std::clamp(int(particles[i].pz), 0, N - 1)]++] = i;
        }
        auto splatSlab = [&](int slab) {
            for (int o = start[slab * SLAB]; o < start[(slab + 1) * SLAB]; o++) {
                const Particle& p = particles[order[o]];
                stencil(p.px, p.py, p.pz, 0, .5f, .5f, N + 1, N, N, uIndex, [&](int i, float wt) { u[i] += p.vx * wt; uw[i] += wt; });
                stencil(p.px, p.py, p.pz, .5f, 0, .5f, N, N + 1, N, [](int i, int j, int k) { return vIndex(i, j, k); },
                        [&](int i, float wt) { v[i] += p.vy * wt; vw[i] += wt; });
                stencil(p.px, p.py, p.pz, .5f, .5f, 0, N, N, N + 1, [](int i, int j, int k) { return wIndex(i, j, k); },
                        [&](int i, float wt) { w[i] += p.vz * wt; ww[i] += wt; });
                int cx = int(p.px), cy = int(p.py), cz = int(p.pz);
                if (cx >= 0 && cy >= 0 && cz >= 0 && cx < N && cy < N && cz < N) {
                    int c = cellIndex(cx, cy, cz);
                    count[c]++;
                    speed[c] += std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz);
                }
            }
        };
        for (int phase = 0; phase < 2; phase++)
            parallelFor(0, SLABS / 2, [&](int lo, int hi) { for (int s = lo; s < hi; s++) splatSlab(2 * s + phase); });
        for (size_t i = 0; i < u.size(); i++) {
            u[i] = uw[i] > 0 ? u[i] / uw[i] : 0;
            v[i] = vw[i] > 0 ? v[i] / vw[i] : 0;
            w[i] = ww[i] > 0 ? w[i] / ww[i] : 0;
        }
        for (int c = 0; c < N * N * N; c++) fluid[c] = count[c] > 0 && !t.solid[c];

        // 2. Gravity, then no flow through solids or the box.
        for (size_t i = 0; i < v.size(); i++) if (vw[i] > 0) v[i] -= GRAVITY * dt;
        applySolids(t);
        u0 = u; v0 = v; w0 = w;

        // 3. Pressure: make the fluid cells divergence-free. Air is p = 0, solids are walls.
        for (int z = 0; z < N; z++)
            for (int y = 0; y < N; y++)
                for (int x = 0; x < N; x++) {
                    int c = cellIndex(x, y, z);
                    if (!fluid[c]) { pressure[c] = 0; continue; }
                    divergence[c] = (u[uIndex(x + 1, y, z)] - u[uIndex(x, y, z)]) + (v[vIndex(x, y + 1, z)] - v[vIndex(x, y, z)]) +
                                    (w[wIndex(x, y, z + 1)] - w[wIndex(x, y, z)]);
                }
        // The fluid cells, by colour, so the solve visits only them.
        std::vector<int> cells[2];
        for (int z = 0; z < N; z++)
            for (int y = 0; y < N; y++)
                for (int x = 0; x < N; x++)
                    if (fluid[cellIndex(x, y, z)]) cells[(x + y + z) & 1].push_back(cellIndex(x, y, z));
        auto relax = [&](int c) {
            int x = c % N, y = (c / N) % N, z = c / (N * N);
            float sum = 0; int open = 0;
            const int dx[6] = {1, -1, 0, 0, 0, 0}, dy[6] = {0, 0, 1, -1, 0, 0}, dz[6] = {0, 0, 0, 0, 1, -1};
            for (int n = 0; n < 6; n++) {
                int ax = x + dx[n], ay = y + dy[n], az = z + dz[n];
                if (t.at(ax, ay, az)) continue;
                open++;
                int a = cellIndex(ax, ay, az);
                if (fluid[a]) sum += pressure[a];
            }
            if (open == 0) return;
            float target = (sum - divergence[c]) / float(open);
            pressure[c] += 1.7f * (target - pressure[c]);   // over-relaxed
        };
        // Red-black Gauss-Seidel: a colour's cells only read the other colour, so each colour splits
        // across threads, with a barrier between colours. The threads start once per solve.
        {
            std::barrier sync(THREADS);
            std::vector<std::thread> pool;
            for (int th = 0; th < THREADS; th++)
                pool.emplace_back([&, th] {
                    for (int it = 0; it < PRESSURE_ITERATIONS; it++)
                        for (int colour = 0; colour < 2; colour++) {
                            const std::vector<int>& list = cells[colour];
                            size_t lo = list.size() * th / THREADS, hi = list.size() * (th + 1) / THREADS;
                            for (size_t i = lo; i < hi; i++) relax(list[i]);
                            sync.arrive_and_wait();
                        }
                });
            for (std::thread& th : pool) th.join();
        }
        // Subtract the gradient on every face with fluid on either side and no solid.
        for (int z = 0; z < N; z++)
            for (int y = 0; y < N; y++)
                for (int x = 0; x < N; x++) {
                    int c = cellIndex(x, y, z);
                    auto grad = [&](int ax, int ay, int az, float& face) {
                        if (t.at(ax, ay, az) || t.at(x, y, z)) return;
                        int a = cellIndex(ax, ay, az);
                        if (!fluid[c] && !fluid[a]) return;
                        face -= pressure[c] - pressure[a];
                    };
                    if (x > 0) grad(x - 1, y, z, u[uIndex(x, y, z)]);
                    if (y > 0) grad(x, y - 1, z, v[vIndex(x, y, z)]);
                    if (z > 0) grad(x, y, z - 1, w[wIndex(x, y, z)]);
                }
        applySolids(t);

        // 4. Grid to particles (FLIP blended with PIC), then move them.
        parallelFor(0, int(particles.size()), [&](int lo, int hi) { for (int pi = lo; pi < hi; pi++) {
            Particle& p = particles[pi];
            float nu = sampleU(u, p.px, p.py, p.pz), nv = sampleV(v, p.px, p.py, p.pz), nw = sampleW(w, p.px, p.py, p.pz);
            float du = nu - sampleU(u0, p.px, p.py, p.pz), dv = nv - sampleV(v0, p.px, p.py, p.pz), dw = nw - sampleW(w0, p.px, p.py, p.pz);
            p.vx = FLIP * (p.vx + du) + (1 - FLIP) * nu;
            p.vy = FLIP * (p.vy + dv) + (1 - FLIP) * nv;
            p.vz = FLIP * (p.vz + dw) + (1 - FLIP) * nw;
            // Advect with the grid's velocity at the midpoint (RK2).
            float mx = p.px + 0.5f * dt * nu, my = p.py + 0.5f * dt * nv, mz = p.pz + 0.5f * dt * nw;
            float ox = p.px, oy = p.py, oz = p.pz;
            p.px += dt * sampleU(u, mx, my, mz);
            p.py += dt * sampleV(v, mx, my, mz);
            p.pz += dt * sampleW(w, mx, my, mz);
            // Out of the walls: back to where it was, axis by axis, losing that axis's velocity.
            p.px = std::clamp(p.px, 0.01f, N - 0.01f); p.py = std::clamp(p.py, 0.01f, N - 0.01f); p.pz = std::clamp(p.pz, 0.01f, N - 0.01f);
            if (t.at(int(p.px), int(oy), int(oz))) { p.px = ox; p.vx = 0; }
            if (t.at(int(p.px), int(p.py), int(oz))) { p.py = oy; p.vy = 0; }
            if (t.at(int(p.px), int(p.py), int(p.pz))) { p.pz = oz; p.vz = 0; }
        } });
    }

    void applySolids(const Terrain& t) {
        for (int z = 0; z < N; z++)
            for (int y = 0; y < N; y++)
                for (int x = 0; x <= N; x++) {
                    if (t.at(x - 1, y, z) || t.at(x, y, z)) u[uIndex(x, y, z)] = 0;
                }
        for (int z = 0; z < N; z++)
            for (int x = 0; x < N; x++)
                for (int y = 0; y <= N; y++)
                    if (t.at(x, y - 1, z) || t.at(x, y, z)) v[vIndex(x, y, z)] = 0;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
                for (int z = 0; z <= N; z++)
                    if (t.at(x, y, z - 1) || t.at(x, y, z)) w[wIndex(x, y, z)] = 0;
    }
};

// ---------------------------------------------------------------------------------------------
// The program
// ---------------------------------------------------------------------------------------------

struct Camera {
    projv::core::vec3 position{21.0f, 10.0f, -5.0f};   // the chunk spans 0..16 m; looking back over the valley at the dam
    float yaw = 2.36f, pitch = -0.42f;
};

struct Prototype {
    Terrain terrain;
    Fluid fluid;
    std::mt19937 rng{1};
    bool paused = false;
    std::vector<uint8_t> drawn = std::vector<uint8_t>(N * N * N, EMPTY);   // the water chunk's slot per voxel
    uint8_t slots[4] = {};                                               // deep, water, surface, foam
    uint8_t terrainSlots[4] = {};
    double simMs = 0;
};

constexpr projv::ComponentHandle TERRAIN = 0, WATER = 1;

void addChunkComponent(projv::Scene& scene, projv::ComponentHandle handle, const char* name,
                       const std::vector<uint8_t>& slotPerVoxel) {
    projv::ChunkHeader header;
    header.chunkID = handle + 1;
    header.position = projv::core::vec3(0.0f);
    header.scale = N * VOXEL;
    header.voxelScale = VOXEL;
    header.resolution = N;
    header.rotation = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);
    projv::Chunk chunk;
    chunk.header = header;
    chunk.requestedLOD = 0;
    chunk.alive = true;
    chunk.componentHandle = handle;
    auto brickMap = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(N));
    for (int z = 0; z < N; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
                if (uint8_t s = slotPerVoxel[cellIndex(x, y, z)]; s != EMPTY) projv::utils::brickMapSetVoxel(*brickMap, x, y, z, s);
    projv::utils::updateChunkFromBrickMap(chunk, *brickMap);
    std::vector<uint8_t> materials;
    projv::utils::bakeMaterialsFromBrickMap(chunk.geometryData, materials, *brickMap);
    const int32_t blob = projv::internChunkGeometry(scene, chunk, std::move(brickMap));
    if (blob >= 0) projv::utils::setBlobTreeMaterials(scene.geometryPool[blob], std::move(materials));
    scene.chunks.push_back(std::move(chunk));
    scene.looseChunks.push_back(handle);
    scene.looseChunkCount = uint32_t(scene.looseChunks.size());
    (void)name;
}

// The water cells and their look, from the particle counts: dark below, light at the surface, white where fast.
std::vector<uint8_t> waterSlots(const Prototype& p) {
    // Particles per cell, smoothed with the six neighbours: a cell is water when the water around it
    // is about half full or more, so a sparse spray of particles does not draw as scattered voxels.
    std::vector<float> density(N * N * N, 0.0f);
    for (int z = 0; z < N; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                int c = cellIndex(x, y, z);
                float sum = 2.0f * p.fluid.count[c], weight = 2.0f;
                const int dx[6] = {1, -1, 0, 0, 0, 0}, dy[6] = {0, 0, 1, -1, 0, 0}, dz[6] = {0, 0, 0, 0, 1, -1};
                for (int n = 0; n < 6; n++) {
                    int ax = x + dx[n], ay = y + dy[n], az = z + dz[n];
                    if (p.terrain.at(ax, ay, az)) continue;   // walls are not empty air
                    sum += p.fluid.count[cellIndex(ax, ay, az)];
                    weight += 1.0f;
                }
                density[c] = sum / weight;
            }
    auto wet = [&](int x, int y, int z) { return y < N && density[cellIndex(x, y, z)] >= 1.8f; };
    std::vector<uint8_t> out(N * N * N, EMPTY);
    for (int z = 0; z < N; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                int c = cellIndex(x, y, z);
                if (!wet(x, y, z) || p.terrain.solid[c]) continue;
                float speed = p.fluid.count[c] ? p.fluid.speed[c] / float(p.fluid.count[c]) : 0.0f;
                bool surface = !wet(x, y + 1, z);
                bool deep = wet(x, y + 1, z) && wet(x, y + 2, z) && wet(x, y + 3, z);
                out[c] = speed > 24.0f ? p.slots[3] : surface ? p.slots[2] : deep ? p.slots[0] : p.slots[1];
            }
    return out;
}

void reset(projv::Application& app) {
    auto& scene = app.world.ctx().get<projv::Scene>();
    auto& p = app.world.ctx().get<Prototype>();
    p.terrain = buildTerrain();
    p.fluid = Fluid();
    p.fluid.addBlock(1, 8, 1, DAM_X0, 30, N - 1, p.terrain, p.rng);   // the reservoir, full to the dam's top

    // Terrain: rewrite every voxel's slot (dug or broken voxels come back).
    std::vector<projv::PendingVoxelOp> add;
    for (int z = 0; z < N; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
                if (uint8_t c = p.terrain.colour[cellIndex(x, y, z)]) add.push_back({true, {x, y, z}, 0, p.terrainSlots[c]});
    projv::utils::queueVoxelAdd(scene, TERRAIN, add);
    projv::core::info("fluid: {} particles", p.fluid.particles.size());
}

void startup(projv::Application& app) {
    auto& renderInstance = app.world.ctx().emplace<projv::graphics::RenderInstance>();
    renderInstance.resizable = std::getenv("FLUID_CAPTURE") == nullptr;
    renderInstance.initialize(1600, 900, "ProjectV fluid prototype");
    projv::graphics::installPlatform(app, renderInstance);
    projv::graphics::setCursorCaptured(app, renderInstance, true);

    auto& scene = app.world.ctx().emplace<projv::Scene>();
    auto& gpu = app.world.ctx().emplace<projv::GPUData>();
    auto& p = app.world.ctx().emplace<Prototype>();
    app.world.ctx().emplace<Camera>();

    for (projv::ComponentHandle h : {TERRAIN, WATER})
        scene.components.push_back(projv::ComponentRecord{projv::ComponentKind::Chunk, h, -1,
                                                          h == TERRAIN ? "internal/terrain" : "internal/water", false, {}, -1});
    using projv::packColor;
    p.terrainSlots[1] = projv::utils::internMaterial(scene, scene.components[TERRAIN], "rock", packColor({118, 110, 100}));
    p.terrainSlots[2] = projv::utils::internMaterial(scene, scene.components[TERRAIN], "grass", packColor({92, 140, 62}));
    p.terrainSlots[3] = projv::utils::internMaterial(scene, scene.components[TERRAIN], "dam", packColor({168, 150, 120}));
    p.slots[0] = projv::utils::internMaterial(scene, scene.components[WATER], "deep", packColor({16, 52, 96}));
    p.slots[1] = projv::utils::internMaterial(scene, scene.components[WATER], "water", packColor({30, 92, 150}));
    p.slots[2] = projv::utils::internMaterial(scene, scene.components[WATER], "surface", packColor({80, 160, 205}));
    p.slots[3] = projv::utils::internMaterial(scene, scene.components[WATER], "foam", packColor({225, 238, 245}));

    p.terrain = buildTerrain();
    std::vector<uint8_t> terrainVoxels(N * N * N, EMPTY);
    for (int c = 0; c < N * N * N; c++) if (p.terrain.colour[c]) terrainVoxels[c] = p.terrainSlots[p.terrain.colour[c]];
    addChunkComponent(scene, TERRAIN, "terrain", terrainVoxels);
    p.fluid.addBlock(1, 8, 1, DAM_X0, 30, N - 1, p.terrain, p.rng);
    p.fluid.step(0.0f, p.terrain);   // counts, for the first drawing
    p.drawn = waterSlots(p);
    addChunkComponent(scene, WATER, "water", p.drawn);

    basic_renderer::install(renderInstance, projv::core::executableDirectory());
    gpu = projv::graphics::createTexturesForScene(scene);
    projv::core::info("fluid: {} particles. E breaks the dam, X digs a trench, G drops water, T resets.",
                      p.fluid.particles.size());
}

void controls(projv::Application& app) {
    auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
    const auto& input = app.world.ctx().get<projv::Input>();
    auto& camera = app.world.ctx().get<Camera>();
    auto& p = app.world.ctx().get<Prototype>();
    auto& scene = app.world.ctx().get<projv::Scene>();

    if (input.cursorCaptured && input.pressed(projv::Key::Escape)) projv::graphics::setCursorCaptured(app, renderInstance, false);
    else if (!input.cursorCaptured && input.pressed(projv::MouseButton::Left)) projv::graphics::setCursorCaptured(app, renderInstance, true);
    if (input.cursorCaptured) {
        camera.yaw += input.cursorDelta.x * 0.0025f;
        camera.pitch = std::clamp(camera.pitch - input.cursorDelta.y * 0.0025f, -1.55f, 1.55f);
    }
    const projv::core::vec3 forward{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
    const projv::core::vec3 right{-std::sin(camera.yaw), 0.0f, std::cos(camera.yaw)};
    const float speed = 8.0f * app.time().delta;
    if (input.down(projv::Key::W)) camera.position += forward * speed;
    if (input.down(projv::Key::S)) camera.position -= forward * speed;
    if (input.down(projv::Key::D)) camera.position += right * speed;
    if (input.down(projv::Key::A)) camera.position -= right * speed;
    if (input.down(projv::Key::R)) camera.position.y += speed;
    if (input.down(projv::Key::F)) camera.position.y -= speed;

    auto removeSolid = [&](int x0, int y0, int z0, int x1, int y1, int z1) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = z0; z < z1; z++)
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    int c = cellIndex(x, y, z);
                    if (!p.terrain.solid[c]) continue;
                    p.terrain.solid[c] = 0;
                    p.terrain.colour[c] = 0;
                    ops.push_back({false, {x, y, z}, 0});
                }
        projv::utils::queueVoxelRemove(scene, TERRAIN, ops);
    };
    if (input.pressed(projv::Key::E)) removeSolid(DAM_X0, 6, N / 2 - 8, DAM_X1, 40, N / 2 + 8);   // a breach in the middle
    if (input.pressed(projv::Key::X)) removeSolid(36, 4, 0, 40, 14, N);                         // a trench across the valley
    if (input.pressed(projv::Key::G)) p.fluid.addBlock(40, 34, 26, 52, 46, 38, p.terrain, p.rng);
    if (input.pressed(projv::Key::T)) reset(app);
    if (input.pressed(projv::Key::Space)) p.paused = !p.paused;
}

void simulate(projv::Application& app) {
    auto& p = app.world.ctx().get<Prototype>();
    auto& scene = app.world.ctx().get<projv::Scene>();
    if (!p.paused) {
        auto t0 = std::chrono::steady_clock::now();
        p.fluid.step(1.0f / 60.0f, p.terrain);
        p.simMs = 0.9 * p.simMs + 0.1 * std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // Only the cells whose look changed go through the edit queue.
    std::vector<uint8_t> now = waterSlots(p);
    std::vector<projv::PendingVoxelOp> add, remove;
    for (int z = 0; z < N; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                int c = cellIndex(x, y, z);
                if (now[c] == p.drawn[c]) continue;
                if (now[c] != EMPTY) add.push_back({true, {x, y, z}, 0, now[c]});
                else remove.push_back({false, {x, y, z}, 0});
            }
    if (!remove.empty()) projv::utils::queueVoxelRemove(scene, WATER, remove);
    if (!add.empty()) projv::utils::queueVoxelAdd(scene, WATER, add);
    p.drawn = now;
    projv::utils::updateScene(scene);

    // FLUID_CAPTURE=<prefix>: break the dam at frame 30, screenshot frames 30, 90, 180 and 360, exit.
    static int frame = 0;
    if (const char* prefix = std::getenv("FLUID_CAPTURE")) {
        if (frame == 30) {
            auto& t = p.terrain;
            std::vector<projv::PendingVoxelOp> ops;
            for (int z = N / 2 - 8; z < N / 2 + 8; z++)
                for (int y = 6; y < 40; y++)
                    for (int x = DAM_X0; x < DAM_X1; x++)
                        if (t.solid[cellIndex(x, y, z)]) { t.solid[cellIndex(x, y, z)] = 0; t.colour[cellIndex(x, y, z)] = 0; ops.push_back({false, {x, y, z}, 0}); }
            projv::utils::queueVoxelRemove(scene, TERRAIN, ops);
            projv::utils::updateScene(scene);
        }
        if (frame == 30 || frame == 90 || frame == 180 || frame == 360)
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, (std::string(prefix) + "-" + std::to_string(frame)).c_str());
        if (frame > 365) app.closeRequested = true;
    }
    if (++frame % 60 == 0) projv::core::info("fluid: {} particles, {:.1f} ms a frame simulating", p.fluid.particles.size(), p.simMs);
}

void render(projv::Application& app) {
    auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
    auto& camera = app.world.ctx().get<Camera>();
    projv::core::vec3 direction{std::cos(camera.pitch) * std::cos(camera.yaw), std::sin(camera.pitch),
                                std::cos(camera.pitch) * std::sin(camera.yaw)};
    basic_renderer::draw(renderInstance, app.world.ctx().get<projv::GPUData>(), app.world.ctx().get<projv::Scene>(),
                         camera.position, direction);
}

void shutdown(projv::Application& app) {
    projv::graphics::destroyGPUData(app.world.ctx().get<projv::GPUData>());
}

}   // namespace

int main() {
    projv::Application app;
    app.addSystem(projv::Stage::Startup, "startup", startup);
    app.addSystem(projv::Stage::Update, "controls", controls);
    app.addSystem(projv::Stage::Update, "simulate", simulate);
    app.addSystem(projv::Stage::Render, "render", render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    return 0;
}
