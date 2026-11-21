// ProjectV Mountains: the Sangre de Cristo at Santa Fe, in voxels, to a horizon ~100 km out.
//
// Plan: docs/plans/mountains-santa-fe.md. The ground is a pure function of position
// (include/mountains/landform.hpp); this program streams it as a clipmap of voxel rings around the
// camera:
//
//   ring k   voxels 0.25 * 2^k m, 64^3 chunks (cells of 16 * 2^k m), 2R cells across, one SceneGrid,
//            centred on the camera and snapped to every second cell, so ring k-1 covers whole cells
//            of ring k. Where it does, and once it has generated them, those cells of ring k are
//            hidden: the hole.
//
// A ring moves a cell at a time, generating the strip it moves onto and dropping the one it leaves,
// so each ring changes only as fast as the camera crosses its own cells and the far rings almost
// never do. A ring generates its hole too, last, and keeps it: when the finer ring moves on, the
// coarse cells are already there to show.
//
//   ./mountains                    fly from the Plaza
//   ./mountains --bench            headless: generate every ring around the camera, report, exit
//
//   MOUNTAINS_SEED, MOUNTAINS_RINGS (11), MOUNTAINS_N (voxels from the centre to a ring's edge: 512)
//   MOUNTAINS_CAMERA=plaza|atalaya|baldy|aerial, or "x,z,above,yaw,pitch" (m, radians; yaw 0 = east)
//   MOUNTAINS_CAPTURE=<file>       save a screenshot once everything has generated, then exit
//
// Controls: WASD move, R/F up/down, mouse look (Esc releases, click captures), Shift x20, Ctrl x0.1,
// G walk (stay 1.7 m over the ground).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <bgfx/bgfx.h>

#include "core/application.h"
#include "core/log.h"
#include "core/math.h"
#include "graphics/basic_renderer.h"
#include "graphics/gpu_interface.h"
#include "graphics/input.h"
#include "graphics/render_instance.h"
#include "mountains/columns.hpp"
#include "mountains/landform.hpp"
#include "mountains/surface.hpp"
#include "utils/blob_voxels.h"
#include "utils/material.h"
#include "utils/scene_query.h"

using namespace mountains;
using projv::core::vec3;

namespace {

using Clock = std::chrono::steady_clock;

int envInt(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return v ? std::atoi(v) : fallback;
}

int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// ---- Work: columns generated on a pool -----------------------------------------------------------

struct Job {
    double priority = 0;   // lower first
    int ring = 0;
    int64_t ix = 0, iz = 0;
    std::shared_ptr<std::atomic<bool>> cancelled;
    bool operator<(const Job& o) const { return priority > o.priority; }   // a min-heap
};

class Pool {
public:
    Pool(const Landform& land, const Palette& palette, unsigned threads) : land_(land), palette_(palette) {
        for (unsigned t = 0; t < threads; ++t) threads_.emplace_back([this] { run(); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }
    void push(Job job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
    }
    // Finished columns, oldest first, at most `max`.
    std::vector<ColumnResult> take(size_t max) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ColumnResult> out;
        while (!results_.empty() && out.size() < max) {
            out.push_back(std::move(results_.front()));
            results_.pop_front();
        }
        return out;
    }
    size_t queued() {
        std::lock_guard<std::mutex> lock(mutex_);
        return jobs_.size();
    }
    unsigned threads() const { return unsigned(threads_.size()); }

private:
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = jobs_.top();
                jobs_.pop();
            }
            if (job.cancelled && job.cancelled->load()) continue;
            ColumnResult r = generateColumn(land_, palette_, job.ring, job.ix, job.iz);
            std::lock_guard<std::mutex> lock(mutex_);
            results_.push_back(std::move(r));
        }
    }
    const Landform& land_;
    const Palette& palette_;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<Job> jobs_;
    std::deque<ColumnResult> results_;
    bool stop_ = false;
};

// ---- The clipmap ---------------------------------------------------------------------------------

uint64_t columnKey(int64_t ix, int64_t iz) {
    return (uint64_t(uint32_t(int32_t(ix))) << 32) | uint64_t(uint32_t(int32_t(iz)));
}

struct Column {
    int64_t ix = 0, iz = 0;
    bool done = false;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    std::vector<std::pair<int64_t, projv::ChunkHandle>> chunks;   // (iy, handle)
};

struct Ring {
    int k = 0;
    int32_t gridIndex = -1;
    projv::ComponentHandle component = projv::INVALID_COMPONENT_HANDLE;
    int64_t ox = 0, oz = 0;   // extent [ox, ox + 2R) x [oz, oz + 2R), in this ring's cells
    bool placed = false;
    std::unordered_map<uint64_t, Column> columns;
    int pending = 0;
    bool dirty = true;
    // Stats
    double sampleMs = 0, fillMs = 0;
    int generated = 0;
};

struct Clipmap {
    int rings = 11;
    int64_t radius = 8;   // cells from the centre to the edge (N / 64)
    std::vector<Ring> ring;

    int64_t span() const { return 2 * radius; }
    bool inExtent(const Ring& r, int64_t ix, int64_t iz) const {
        return ix >= r.ox && ix < r.ox + span() && iz >= r.oz && iz < r.oz + span();
    }
    // Whether ring k's cell (ix, iz) lies under ring k-1.
    bool inHole(int k, int64_t ix, int64_t iz) const {
        if (k == 0 || !ring[size_t(k - 1)].placed) return false;
        const Ring& f = ring[size_t(k - 1)];
        const int64_t hx = f.ox / 2, hz = f.oz / 2;
        return ix >= hx && ix < hx + radius && iz >= hz && iz < hz + radius;
    }
    // Whether ring k-1 has finished the four columns over ring k's cell, so ring k may hide it.
    bool covered(int k, int64_t ix, int64_t iz) const {
        if (!inHole(k, ix, iz)) return false;
        const Ring& f = ring[size_t(k - 1)];
        for (int64_t j = 0; j < 2; ++j)
            for (int64_t i = 0; i < 2; ++i) {
                auto it = f.columns.find(columnKey(ix * 2 + i, iz * 2 + j));
                if (it == f.columns.end() || !it->second.done) return false;
            }
        return true;
    }
};

void releaseChunk(projv::Scene& scene, projv::ChunkHandle h) {
    if (h >= scene.chunks.size()) return;
    projv::Chunk& c = scene.chunks[h];
    if (!c.alive) return;
    const int32_t pool = c.geometryPoolIndex;
    if (pool >= 0 && size_t(pool) < scene.geometryPool.size()) {
        projv::GeometryBlob& blob = scene.geometryPool[size_t(pool)];
        const bool last = blob.refCount == 1;
        projv::releaseBlob(scene, pool);
        if (last) {
            projv::utils::setBlobTree(blob, {}, {});   // frees the voxels now, not at reuse
            blob.dirty = false;
        }
    }
    c.alive = false;
    c.geometryPoolIndex = -1;
    c.gridIndex = -1;
    c.cellIndex = -1;
    scene.slots.chunks.push_back(h);
}

projv::ChunkHandle installChunk(projv::Scene& scene, const Ring& r, int64_t ix, int64_t iz, ColumnChunk&& cc) {
    const double c = cellSize(r.k);
    projv::ChunkHeader hdr;
    hdr.position = vec3(float(double(ix) * c), float(double(cc.iy) * c), float(double(iz) * c));
    hdr.scale = float(c);
    hdr.voxelScale = float(voxelSize(r.k));
    hdr.resolution = kChunkRes;
    hdr.rotation = projv::core::quat(1, 0, 0, 0);
    projv::Chunk chunk = projv::utils::createChunk(hdr);
    chunk.gridIndex = r.gridIndex;
    chunk.componentHandle = r.component;
    chunk.alive = true;
    projv::internChunkGeometry(scene, chunk);
    if (chunk.geometryPoolIndex >= 0) {
        projv::GeometryBlob& blob = scene.geometryPool[size_t(chunk.geometryPoolIndex)];
        projv::utils::setBlobTiles(blob, std::move(cc.tiles));
        blob.dirty = true;
    }
    return projv::utils::allocateChunkSlot(scene, std::move(chunk));
}

void dropRing(Clipmap& cm, projv::Scene& scene, int k) {
    Ring& r = cm.ring[size_t(k)];
    for (auto& [key, col] : r.columns) {
        col.cancelled->store(true);
        for (auto& [iy, h] : col.chunks) releaseChunk(scene, h);
    }
    r.columns.clear();
    r.pending = 0;
    r.placed = false;
    r.dirty = true;
    if (k + 1 < cm.rings) cm.ring[size_t(k + 1)].dirty = true;
}

// Moves each ring to the camera, queueing the columns it moved onto and dropping the ones it left.
// A ring less than half as wide as the camera is high would cover a few pixels under it: it is
// dropped, and the next ring shows its hole instead.
void placeRings(Clipmap& cm, projv::Scene& scene, Pool* pool, double camX, double camZ, double camAbove) {
    for (int k = 0; k < cm.rings; ++k) {
        Ring& r = cm.ring[size_t(k)];
        const double c = cellSize(k);
        const bool active = k == cm.rings - 1 || c * double(cm.radius) * 2.0 >= camAbove;
        if (!active) {
            if (r.placed || !r.columns.empty()) dropRing(cm, scene, k);
            continue;
        }
        const int64_t cx = int64_t(std::floor(camX / c)), cz = int64_t(std::floor(camZ / c));
        const int64_t ox = 2 * floorDiv(cx, 2) - cm.radius, oz = 2 * floorDiv(cz, 2) - cm.radius;
        if (r.placed && ox == r.ox && oz == r.oz) continue;
        r.ox = ox;
        r.oz = oz;
        r.placed = true;
        r.dirty = true;
        if (k + 1 < cm.rings) cm.ring[size_t(k + 1)].dirty = true;
        for (auto it = r.columns.begin(); it != r.columns.end();) {
            Column& col = it->second;
            if (cm.inExtent(r, col.ix, col.iz)) { ++it; continue; }
            col.cancelled->store(true);
            if (!col.done) --r.pending;
            for (auto& [iy, h] : col.chunks) releaseChunk(scene, h);
            it = r.columns.erase(it);
        }
    }
    // Queue after every ring is placed, so the hole test sees where the finer ring now is.
    for (int k = 0; k < cm.rings; ++k) {
        Ring& r = cm.ring[size_t(k)];
        if (!r.placed) continue;
        const double c = cellSize(k);
        for (int64_t iz = r.oz; iz < r.oz + cm.span(); ++iz)
            for (int64_t ix = r.ox; ix < r.ox + cm.span(); ++ix) {
                const uint64_t key = columnKey(ix, iz);
                if (r.columns.count(key)) continue;
                Column col;
                col.ix = ix;
                col.iz = iz;
                Job job;
                job.ring = k;
                job.ix = ix;
                job.iz = iz;
                job.cancelled = col.cancelled;
                const double dx = (double(ix) + 0.5) * c - camX, dz = (double(iz) + 0.5) * c - camZ;
                const double d = std::sqrt(dx * dx + dz * dz) / (c * double(cm.radius));   // 0 .. ~1.4
                // Every ring from its centre out, together, so a fast camera cannot starve the far
                // rings; finer rings a little sooner; the hole after everything that shows.
                job.priority = (cm.inHole(k, ix, iz) ? 1000.0 : 0.0) + d + 0.05 * double(k);
                r.columns.emplace(key, std::move(col));
                ++r.pending;
                if (pool) pool->push(std::move(job));
            }
    }
}

// Rebuilds a ring's SceneGrid: its extent, the vertical span of what it holds, and which cells show.
void rebuildGrid(Clipmap& cm, projv::Scene& scene, int k) {
    Ring& r = cm.ring[size_t(k)];
    projv::SceneGrid& g = scene.grids[size_t(r.gridIndex)];
    const double c = cellSize(k);
    int64_t y0 = INT64_MAX, y1 = INT64_MIN;
    for (auto& [key, col] : r.columns)
        for (auto& [iy, h] : col.chunks) {
            y0 = std::min(y0, iy);
            y1 = std::max(y1, iy);
        }
    if (y0 > y1) y0 = y1 = 0;
    const int64_t span = cm.span();
    g.origin = vec3(float(double(r.ox) * c), float(double(y0) * c), float(double(r.oz) * c));
    g.cellSize = float(c);
    g.nativeCellSize = float(c);
    g.dims = projv::core::ivec3(int(span), int(y1 - y0 + 1), int(span));
    g.originCellCoord = projv::core::ivec3(int(r.ox), int(y0), int(r.oz));
    g.cellToChunk.assign(size_t(span) * size_t(y1 - y0 + 1) * size_t(span), -1);
    for (auto& [key, col] : r.columns) {
        if (!col.done) continue;
        const bool hidden = cm.covered(k, col.ix, col.iz);
        for (auto& [iy, h] : col.chunks) {
            const int64_t lx = col.ix - r.ox, ly = iy - y0, lz = col.iz - r.oz;
            const int lin = int(lx + span * (ly + (y1 - y0 + 1) * lz));
            scene.chunks[h].cellIndex = hidden ? -1 : lin;
            if (!hidden) g.cellToChunk[size_t(lin)] = int32_t(h);
        }
    }
    r.dirty = false;
}

// Installs finished columns, within a budget. Returns how many it installed.
int installResults(Clipmap& cm, projv::Scene& scene, Pool& pool, double budgetMs) {
    const auto start = Clock::now();
    int installed = 0;
    while (std::chrono::duration<double, std::milli>(Clock::now() - start).count() < budgetMs) {
        std::vector<ColumnResult> batch = pool.take(8);
        if (batch.empty()) break;
        for (ColumnResult& res : batch) {
            Ring& r = cm.ring[size_t(res.ring)];
            auto it = r.columns.find(columnKey(res.ix, res.iz));
            if (it == r.columns.end() || it->second.done) continue;   // dropped while it was generating
            Column& col = it->second;
            for (ColumnChunk& cc : res.chunks) {
                const int64_t iy = cc.iy;
                col.chunks.emplace_back(iy, installChunk(scene, r, res.ix, res.iz, std::move(cc)));
            }
            col.done = true;
            --r.pending;
            r.dirty = true;
            if (res.ring + 1 < cm.rings) cm.ring[size_t(res.ring + 1)].dirty = true;
            r.sampleMs += res.sampleMs;
            r.fillMs += res.fillMs;
            ++r.generated;
            ++installed;
        }
    }
    return installed;
}

Palette internPalette(projv::Scene& scene, projv::ComponentRecord& comp) {
    Palette p;
    static const char* names[] = {"soil", "grama", "sediment", "pinyon", "ponderosa", "fir", "spruce", "aspen",
                                  "tundra", "granite", "gneiss", "basalt", "snow", "water"};
    for (int m = 0; m < kMaterialCount; ++m)
        for (int s = 0; s < kShades; ++s) {
            const Rgb c = shadeColour(Material(m), s);
            p.slot[size_t(m)][size_t(s)] = projv::utils::internMaterial(
                scene, comp, std::string("mountains.") + names[m] + "." + std::to_string(s),
                projv::packRGB10(float(c.r), float(c.g), float(c.b)));
        }
    return p;
}

// ---- Camera --------------------------------------------------------------------------------------

struct Camera {
    double x = 0, y = 2200, z = 0;
    float yaw = 0.0f, pitch = 0.05f;
    bool walk = false;
};

void placeCamera(Camera& cam, const Landform& land) {
    std::string spec = std::getenv("MOUNTAINS_CAMERA") ? std::getenv("MOUNTAINS_CAMERA") : "plaza";
    double x = 0, z = 0, above = 1.7;
    float yaw = 0.0f, pitch = 0.05f;   // east, a little up
    if (spec == "plaza") { above = 1.7; }
    else if (spec == "atalaya") { x = 6900; z = 300; above = 3; yaw = 3.14159f; pitch = -0.05f; }
    else if (spec == "baldy") { x = 14800; z = 15700; above = 3; yaw = 3.4f; pitch = -0.06f; }
    else if (spec == "aerial") { x = -12000; z = -9000; above = 2500; yaw = 0.65f; pitch = -0.18f; }
    else std::sscanf(spec.c_str(), "%lf,%lf,%lf,%f,%f", &x, &z, &above, &yaw, &pitch);
    cam.x = x;
    cam.z = z;
    cam.y = land.height(x, z, 0.25) + above;
    cam.yaw = yaw;
    cam.pitch = pitch;
}

// ---- The program ---------------------------------------------------------------------------------

struct State {
    std::unique_ptr<Landform> land;
    Palette palette;
    Clipmap clipmap;
    std::unique_ptr<Pool> pool;
    Camera camera;
    double cameraAbove = 0;   // over the ground, refreshed a few times a second
    Clock::time_point groundCheck;
    Clock::time_point started = Clock::now();
    Clock::time_point lastReport = Clock::now();
    double frameMs = 0;
    int frames = 0;
    std::string capturePath;
    int captureStage = 0;
    Clock::time_point settledAt;
    int settledFrames = 0;
};

void setUpRings(State& st, projv::Scene& scene) {
    Clipmap& cm = st.clipmap;
    cm.rings = std::clamp(envInt("MOUNTAINS_RINGS", 11), 1, 14);
    cm.radius = std::max<int64_t>(2, (envInt("MOUNTAINS_N", 512) / kChunkRes) & ~int64_t(1));
    cm.ring.resize(size_t(cm.rings));
    for (int k = 0; k < cm.rings; ++k) {
        Ring& r = cm.ring[size_t(k)];
        r.k = k;
        r.gridIndex = int32_t(scene.grids.size());
        projv::SceneGrid g;
        g.cellSize = float(cellSize(k));
        g.nativeCellSize = g.cellSize;
        g.dims = projv::core::ivec3(1, 1, 1);
        g.rotation = projv::core::quat(1, 0, 0, 0);
        g.cellToChunk.assign(1, -1);
        scene.grids.push_back(std::move(g));
        projv::ComponentRecord rec;
        rec.kind = projv::ComponentKind::Grid;
        rec.gridIndex = r.gridIndex;
        rec.name = "ring " + std::to_string(k);
        rec.sourcePath = "internal/mountains";
        r.component = projv::utils::reserveComponentSlot(scene);
        projv::utils::placeComponent(scene, r.component, std::move(rec));
        scene.grids[size_t(r.gridIndex)].componentHandle = r.component;
        const Palette p = internPalette(scene, scene.components[r.component]);
        if (k == 0) st.palette = p;
        else if (std::memcmp(&p, &st.palette, sizeof(Palette)) != 0)
            projv::core::warn("[MOUNTAINS] ring {} interned its palette into different slots", k);
    }
}

void report(State& st, projv::Scene& scene) {
    const Clipmap& cm = st.clipmap;
    size_t chunks = 0, bytes = 0;
    for (const projv::Chunk& c : scene.chunks)
        if (c.alive) {
            ++chunks;
            if (c.geometryPoolIndex >= 0) bytes += projv::utils::blobVoxelBytes(scene.geometryPool[size_t(c.geometryPoolIndex)]);
        }
    std::string rings;
    for (const Ring& r : cm.ring) rings += " " + std::to_string(r.pending);
    projv::core::info("[MOUNTAINS] {:.1f} ms/frame, camera ({:.0f}, {:.0f}, {:.0f}); pending by ring:{}; {} chunks, {:.0f} MB voxels",
                      st.frames ? st.frameMs / st.frames : 0.0, st.camera.x, st.camera.y, st.camera.z, rings, chunks,
                      double(bytes) / 1048576.0);
}

void startup(projv::Application& app) {
    auto& ri = app.world.ctx().emplace<projv::graphics::RenderInstance>();
    ri.initialize(1600, 900, "ProjectV Mountains");
    projv::graphics::installPlatform(app, ri);
    auto& scene = app.world.ctx().emplace<projv::Scene>();
    auto& st = app.world.ctx().emplace<State>();
    Settings settings;
    settings.seed = uint32_t(envInt("MOUNTAINS_SEED", 1));
    st.land = std::make_unique<Landform>(settings);
    setUpRings(st, scene);
    placeCamera(st.camera, *st.land);
    st.cameraAbove = std::max(0.0, st.camera.y - st.land->height(st.camera.x, st.camera.z, 8.0));
    st.pool = std::make_unique<Pool>(*st.land, st.palette, std::max(2u, std::thread::hardware_concurrency() - 2));
    placeRings(st.clipmap, scene, st.pool.get(), st.camera.x, st.camera.z, st.cameraAbove);
    projv::core::info("[MOUNTAINS] {} rings of {} cells, {} workers; the outer ring reaches {:.0f} km",
                      st.clipmap.rings, st.clipmap.span(), st.pool->threads(),
                      cellSize(st.clipmap.rings - 1) * double(st.clipmap.radius) / 1000.0);

    projv::graphics::BasicRendererOptions look;
    look.sunDirection = vec3(-0.80f, 0.42f, -0.30f);   // late afternoon, west-southwest
    look.sunColour = vec3(1.05f, 0.98f, 0.90f);
    look.ambientAbove = vec3(0.32f, 0.38f, 0.48f);
    look.ambientBelow = vec3(0.16f, 0.14f, 0.12f);
    look.skyAbove = vec3(0.24f, 0.43f, 0.78f);
    look.skyBelow = vec3(0.70f, 0.78f, 0.88f);
    projv::graphics::installBasicRenderer(app, ri, look);
    app.world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));

    if (const char* cap = std::getenv("MOUNTAINS_CAPTURE")) st.capturePath = cap;
    else projv::graphics::setCursorCaptured(app, ri, true);
}

void moveCamera(projv::Application& app) {
    auto& ri = app.world.ctx().get<projv::graphics::RenderInstance>();
    const auto& input = app.world.ctx().get<projv::Input>();
    State& st = app.world.ctx().get<State>();
    Camera& cam = st.camera;
    if (input.cursorCaptured && input.pressed(projv::Key::Escape)) projv::graphics::setCursorCaptured(app, ri, false);
    else if (!input.cursorCaptured && input.pressed(projv::MouseButton::Left) && st.capturePath.empty())
        projv::graphics::setCursorCaptured(app, ri, true);
    if (input.cursorCaptured) {
        cam.yaw += input.cursorDelta.x * 0.0025f;
        cam.pitch = std::clamp(cam.pitch - input.cursorDelta.y * 0.0025f, -1.55f, 1.55f);
    }
    if (input.pressed(projv::Key::G)) cam.walk = !cam.walk;
    double speed = cam.walk ? 4.0 : 40.0;
    if (input.down(projv::Key::LeftShift)) speed *= 20.0;
    if (input.down(projv::Key::LeftControl)) speed *= 0.1;
    const double d = speed * app.time().delta;
    const double fx = std::cos(cam.yaw), fz = std::sin(cam.yaw);
    if (input.down(projv::Key::W)) { cam.x += fx * d; cam.z += fz * d; }
    if (input.down(projv::Key::S)) { cam.x -= fx * d; cam.z -= fz * d; }
    if (input.down(projv::Key::D)) { cam.x -= fz * d; cam.z += fx * d; }
    if (input.down(projv::Key::A)) { cam.x += fz * d; cam.z -= fx * d; }
    if (input.down(projv::Key::R)) cam.y += d;
    if (input.down(projv::Key::F)) cam.y -= d;
    if (cam.walk) cam.y = st.land->height(cam.x, cam.z, 0.25) + 1.7;
}

void stream(projv::Application& app) {
    State& st = app.world.ctx().get<State>();
    auto& scene = app.world.ctx().get<projv::Scene>();
    // MOUNTAINS_FLY=<m/s>: fly straight ahead, for testing whether streaming keeps up.
    static const double fly = std::getenv("MOUNTAINS_FLY") ? std::atof(std::getenv("MOUNTAINS_FLY")) : 0.0;
    if (fly > 0.0) {
        st.camera.x += std::cos(st.camera.yaw) * fly * app.time().delta;
        st.camera.z += std::sin(st.camera.yaw) * fly * app.time().delta;
    }
    if (Clock::now() - st.groundCheck > std::chrono::milliseconds(250)) {
        st.cameraAbove = std::max(0.0, st.camera.y - st.land->height(st.camera.x, st.camera.z, 8.0));
        st.groundCheck = Clock::now();
    }
    placeRings(st.clipmap, scene, st.pool.get(), st.camera.x, st.camera.z, st.cameraAbove);
    installResults(st.clipmap, scene, *st.pool, 6.0);
    for (int k = 0; k < st.clipmap.rings; ++k)
        if (st.clipmap.ring[size_t(k)].dirty) rebuildGrid(st.clipmap, scene, k);

    projv::CameraView& view = app.world.ctx().get<projv::CameraView>();
    view.position = vec3(float(st.camera.x), float(st.camera.y), float(st.camera.z));
    view.forward = vec3(std::cos(st.camera.pitch) * std::cos(st.camera.yaw), std::sin(st.camera.pitch),
                        std::cos(st.camera.pitch) * std::sin(st.camera.yaw));
    view.verticalFovDegrees = 50.0f;

    st.frameMs += app.time().unscaledDelta * 1000.0;
    ++st.frames;
    if (Clock::now() - st.lastReport > std::chrono::seconds(2)) {
        report(st, scene);
        st.lastReport = Clock::now();
        st.frameMs = 0;
        st.frames = 0;
    }

    // MOUNTAINS_CAPTURE: once nothing is pending (or two minutes), three seconds more to measure the
    // settled frame time, then a screenshot, then exit.
    if (!st.capturePath.empty()) {
        int pending = 0;
        for (const Ring& r : st.clipmap.ring) pending += r.pending;
        const double elapsed = std::chrono::duration<double>(Clock::now() - st.started).count();
        if (st.captureStage == 0) {
            const double flySeconds = fly > 0.0 ? 20.0 : 0.0;
            if ((fly > 0.0 && elapsed > flySeconds) || (fly <= 0.0 && pending == 0 && elapsed > 2.0) || elapsed > 120.0) {
                st.captureStage = 1;
                st.settledAt = Clock::now();
                st.settledFrames = 0;
            }
        } else if (st.captureStage == 1) {
            ++st.settledFrames;
            const double s = std::chrono::duration<double>(Clock::now() - st.settledAt).count();
            if (s >= 3.0) {
                const auto& ri = app.world.ctx().get<projv::graphics::RenderInstance>();
                (void)ri;
                projv::core::info("[MOUNTAINS] settled: {:.2f} ms/frame over {} frames", s * 1000.0 / st.settledFrames,
                                  st.settledFrames);
                bgfx::requestScreenShot(BGFX_INVALID_HANDLE, st.capturePath.c_str());
                st.captureStage = 2;
            }
        } else if (++st.captureStage > 12) {
            app.closeRequested = true;
        }
    }
}

void shutdown(projv::Application& app) {
    State& st = app.world.ctx().get<State>();
    st.pool.reset();
    projv::graphics::destroyGPUData(app.world.ctx().get<projv::GPUData>());
}

// ---- Headless: generate every ring once, report ----------------------------------------------------

int bench() {
    projv::Scene scene;
    State st;
    Settings settings;
    settings.seed = uint32_t(envInt("MOUNTAINS_SEED", 1));
    st.land = std::make_unique<Landform>(settings);
    setUpRings(st, scene);
    placeCamera(st.camera, *st.land);
    st.cameraAbove = std::max(0.0, st.camera.y - st.land->height(st.camera.x, st.camera.z, 8.0));
    const unsigned threads = std::max(2u, std::thread::hardware_concurrency());
    st.pool = std::make_unique<Pool>(*st.land, st.palette, threads);
    const auto t0 = Clock::now();
    placeRings(st.clipmap, scene, st.pool.get(), st.camera.x, st.camera.z, st.cameraAbove);
    for (;;) {
        installResults(st.clipmap, scene, *st.pool, 50.0);
        int pending = 0;
        for (const Ring& r : st.clipmap.ring) pending += r.pending;
        if (pending == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("%d rings of %lld x %lld columns, %u threads, %.1f s wall\n", st.clipmap.rings,
                (long long)st.clipmap.span(), (long long)st.clipmap.span(), threads, wall);
    std::printf("%4s %8s %8s %8s %10s %10s %10s\n", "ring", "voxel m", "reach km", "chunks", "MB", "sample ms", "fill ms");
    size_t totalBytes = 0, totalChunks = 0;
    for (const Ring& r : st.clipmap.ring) {
        size_t chunks = 0, bytes = 0;
        for (auto& [key, col] : r.columns)
            for (auto& [iy, h] : col.chunks) {
                ++chunks;
                bytes += projv::utils::blobVoxelBytes(scene.geometryPool[size_t(scene.chunks[h].geometryPoolIndex)]);
            }
        totalBytes += bytes;
        totalChunks += chunks;
        std::printf("%4d %8.2f %8.2f %8zu %10.1f %10.2f %10.2f\n", r.k, voxelSize(r.k),
                    cellSize(r.k) * double(st.clipmap.radius) / 1000.0, chunks, double(bytes) / 1048576.0,
                    r.generated ? r.sampleMs / r.generated : 0.0, r.generated ? r.fillMs / r.generated : 0.0);
    }
    std::printf("total: %zu chunks, %.1f MB of voxels\n", totalChunks, double(totalBytes) / 1048576.0);
    st.pool.reset();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--bench") return bench();
    projv::Application app;
    app.addSystem(projv::Stage::Startup, "startup", startup);
    app.addSystem(projv::Stage::Update, "move camera", moveCamera);
    app.addSystem(projv::Stage::Update, "stream", stream, {.after = "move camera"});
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    return 0;
}
