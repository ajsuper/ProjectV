// ProjectV Santa Fe: the Sangre de Cristo at Santa Fe as a quality reference -- 55-mountains' world, drawn by
// a slow progressive renderer (renderer/: soft shadows, traced sky light and a bounce, volumetric clouds and
// haze, smooth rock, the far field as terrain with trees), with every feature a switch in the dev panel (F1).
//
// Plans: docs/plans/santa-fe-reference.md (this fork), docs/plans/mountains-santa-fe.md (the world). The ground is a pure function of position
// (include/mountains/landform.hpp); this program streams it as a clipmap of voxel rings around the
// camera:
//
//   ring k   voxels 0.125 * 2^k m (55: 0.25), 64^3 chunks (cells of 16 * 2^k m), 2R cells across, one SceneGrid,
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
//   ./mountains --selftest         headless: generate, fly 5 km and back, check for holes, stray cells and leaks
//
//   MOUNTAINS_SEED, MOUNTAINS_RINGS (11), MOUNTAINS_N (voxels from the centre to a ring's edge: 512)
//   MOUNTAINS_CAMERA=plaza|atalaya|baldy|aerial, or "x,z,above,yaw,pitch" (m, radians; yaw 0 = east)
//   MOUNTAINS_CAPTURE=<file>       save a screenshot once everything has generated, then exit
//   SANTA_FE_CAMERA_ABS=1          MOUNTAINS_CAMERA's third number is the camera's height, not its height above the ground
//   SANTA_FE_SHADOW_VOXELS=<m>, SANTA_FE_SHADOW_LOD=<voxels>, SANTA_FE_SHADOW_PROPS=<m>, SANTA_FE_SHADOW_DEBUG=1|2 (voxels|far only),
//   SANTA_FE_NO_SSAO               the shadow rays' parts and the AO's, for measuring
//
//   MOUNTAINS_RENDERER=basic       the engine's basic renderer instead of renderer/ (M3)
//
// Controls: WASD move, R/F up/down, mouse look (Esc releases, click captures), Shift x20, Ctrl x0.1,
// G walk / fly (the character: Space jumps, Shift runs, Ctrl creeps), [ ] lower / raise the sun, , . turn it.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
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
#include "core/paths.h"
#include "graphics/basic_renderer.h"
#include "graphics/camera.h"
#include "graphics/disk_io.h"
#include "graphics/engine_shaders.h"
#include "graphics/gpu_interface.h"
#include "graphics/input.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "mountains/brush_trees.hpp"
#include "mountains/columns.hpp"
#include "mountains/landform.hpp"
#include "mountains/surface.hpp"
#include "utils/blob_voxels.h"
#include "utils/material.h"
#include "utils/picking.h"
#include "utils/scene_query.h"
#ifdef PROJV_UI
#include "ui/dev.h"
#include "ui/ui.h"
#endif

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

// Samples a side of a far column: 64, or 64 / the block factor in blocky distance (the far rings regenerate
// when it changes).
std::atomic<int> gFarSamples{64};

struct Job {
    double priority = 0;   // lower first
    int ring = 0;
    int64_t ix = 0, iz = 0;
    std::shared_ptr<std::atomic<bool>> cancelled;
    bool operator<(const Job& o) const { return priority > o.priority; }   // a min-heap
};

class Pool {
public:
    Pool(const Landform& land, const Palette& palette, const ShapeLibrary* props, unsigned threads, int firstFar)
        : land_(land), palette_(palette), props_(props), firstFar_(firstFar) {
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
            ColumnResult r = job.ring >= firstFar_ ? generateFarColumn(land_, job.ring, job.ix, job.iz, gFarSamples.load())
                                                   : generateColumn(land_, palette_, props_, job.ring, job.ix, job.iz);
            std::lock_guard<std::mutex> lock(mutex_);
            results_.push_back(std::move(r));
        }
    }
    const Landform& land_;
    const Palette& palette_;
    const ShapeLibrary* props_ = nullptr;
    int firstFar_ = 1 << 30;
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
    std::vector<projv::ChunkHandle> props;                         // loose chunks (props.hpp)
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

// The far rings' tiles on their way to the renderer (plan D15): each far ring has a square of the
// height and colour atlases, a column's 64 x 64 tile at (column mod span) in it, and a slot in the state
// texture saying which column a tile holds (the low bytes of its ix, iz; b = 255 once it holds one).
struct FarField {
    static constexpr int kAtlasCols = 3, kAtlasRows = 2;   // ring squares across and down: six far rings
    int width = 0, height = 0;                             // the atlases, texels
    int stateWidth = 0, stateHeight = 0;
    std::vector<uint8_t> state;
    std::vector<float> maxHeight;   // per state texel: the column's highest (the blocky march's skip)
    bool stateDirty = true;
    struct Upload { int x, y; std::vector<float> heights; std::vector<uint32_t> colours, canopy; };
    std::deque<Upload> uploads;
    bool upload = false;   // a renderer will take them (not in --bench, --selftest)
};

struct Clipmap {
    int rings = 11;
    int voxelRings = 5;   // rings [0, voxelRings) are voxels; the rest heightfields (D14)
    int64_t radius = 8;   // cells from the centre to the edge (N / 64)
    std::vector<Ring> ring;
    FarField far;
    // Props (plan D18): the library's blob and resolution per shape, and the component they draw as.
    std::vector<int32_t> propBlob;
    std::vector<int> propRes;
    projv::ComponentHandle propComponent = projv::INVALID_COMPONENT_HANDLE;
    projv::ComponentHandle treeComponent = projv::INVALID_COMPONENT_HANDLE;   // the tree brush's palette
    std::vector<bool> propOwnPalette;
    double culledAt[3] = {1e30, 1e30, 1e30};   // where the camera was when the props were last culled

    bool isFar(int k) const { return k >= voxelRings; }
    // The state texel and the atlas texel of a far ring's column.
    void farSlot(int k, int64_t ix, int64_t iz, int& sx, int& sz, int& ax, int& az) const {
        const int f = k - voxelRings;
        const int64_t span = 2 * radius;
        sx = int(((ix % span) + span) % span);
        sz = int(((iz % span) + span) % span);
        ax = (f % FarField::kAtlasCols) * int(span) * kChunkRes + sx * kChunkRes;
        az = (f / FarField::kAtlasCols) * int(span) * kChunkRes + sz * kChunkRes;
        sx += f * int(span);
    }
    void setFarState(int k, int64_t ix, int64_t iz, bool held) {
        int sx, sz, ax, az;
        farSlot(k, ix, iz, sx, sz, ax, az);
        uint8_t* p = &far.state[(size_t(sz) * size_t(far.stateWidth) + size_t(sx)) * 4];
        if (held) {
            p[0] = uint8_t(ix & 255); p[1] = uint8_t(iz & 255); p[2] = 255; p[3] = 255;
        } else if (p[0] == uint8_t(ix & 255) && p[1] == uint8_t(iz & 255)) {
            p[2] = 0;   // only if it still holds this column, not the one that took its slot
        }
        far.stateDirty = true;
    }

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

// Frees a column's chunks and props, and its far tile's slot.
void releaseColumn(Clipmap& cm, projv::Scene& scene, int k, Column& col) {
    col.cancelled->store(true);
    for (auto& [iy, h] : col.chunks) releaseChunk(scene, h);
    for (projv::ChunkHandle h : col.props) releaseChunk(scene, h);
    col.chunks.clear();
    col.props.clear();
    if (cm.isFar(k) && col.done) cm.setFarState(k, col.ix, col.iz, false);
}

// One prop as a loose chunk sharing its shape's blob.
projv::ChunkHandle installProp(projv::Scene& scene, const Clipmap& cm, const PropInstance& p) {
    const int res = cm.propRes[size_t(p.shape)];
    projv::Chunk chunk;
    chunk.header.position = vec3(float(p.px), float(p.py), float(p.pz));
    chunk.header.rotation = projv::core::quat(float(p.qw), float(p.qx), float(p.qy), float(p.qz));
    chunk.header.scale = float(p.size);
    chunk.header.voxelScale = float(p.size / double(res));
    chunk.header.resolution = uint32_t(res);
    chunk.geometryPoolIndex = cm.propBlob[size_t(p.shape)];
    scene.geometryPool[size_t(chunk.geometryPoolIndex)].refCount++;
    chunk.componentHandle = cm.propComponent;
    chunk.gridIndex = -1;
    chunk.cellIndex = -1;
    chunk.alive = true;
    return projv::utils::allocateChunkSlot(scene, std::move(chunk));
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
    for (auto& [key, col] : r.columns) releaseColumn(cm, scene, k, col);
    r.columns.clear();
    r.pending = 0;
    r.placed = false;
    r.dirty = true;
    if (k + 1 < cm.rings) cm.ring[size_t(k + 1)].dirty = true;
}

// Moves each ring to the camera, queueing the columns it moved onto and dropping the ones it left.
// A ring less than half as wide as the camera is high would cover a few pixels under it: it is
// dropped, and the next ring shows its hole instead.
// `camSpeed` (m/s): a ring that would cross its own half-width in under two seconds is not kept while the
// camera moves that fast -- it could never finish, and its work starves the rings that can.
void placeRings(Clipmap& cm, projv::Scene& scene, Pool* pool, double camX, double camZ, double camAbove, double camSpeed = 0.0) {
    for (int k = 0; k < cm.rings; ++k) {
        Ring& r = cm.ring[size_t(k)];
        const double c = cellSize(k);
        const bool active = k == cm.rings - 1 || (c * double(cm.radius) * 2.0 >= camAbove && c * double(cm.radius) >= camSpeed * 2.0);
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
            if (!col.done) --r.pending;
            releaseColumn(cm, scene, k, col);
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
    if (r.gridIndex < 0) { r.dirty = false; return; }   // a far ring: no grid
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

// How far out a prop is drawn, in multiples of its chunk's edge: about 4 pixels tall at 1080p (a 30 cm
// cobble to 60 m, a chamisa to 200 m). Every ray near the ground walks the props it passes, so the
// ones too small to see are not worth their cost.
constexpr double kPropReach = 200.0;
// How far the camera moves before the props are culled again.
constexpr double kPropRecull = 6.0;

// The loose list: every prop of every column that shows (a hidden column's props hide with its cells)
// that is near enough to be seen.
void rebuildProps(Clipmap& cm, projv::Scene& scene, double camX, double camY, double camZ) {
    static const double propReach = std::getenv("MOUNTAINS_PROP_REACH") ? std::atof(std::getenv("MOUNTAINS_PROP_REACH")) : kPropReach;
    scene.looseChunks.clear();
    for (int k = 0; k < cm.voxelRings; ++k) {
        const Ring& r = cm.ring[size_t(k)];
        for (const auto& [key, col] : r.columns) {
            if (!col.done || col.props.empty() || cm.covered(k, col.ix, col.iz)) continue;
            for (projv::ChunkHandle h : col.props) {
                const projv::ChunkHeader& hd = scene.chunks[h].header;
                const double dx = hd.position.x - camX, dy = hd.position.y - camY, dz = hd.position.z - camZ;
                const double reach = propReach * hd.scale + kPropRecull;
                if (dx * dx + dy * dy + dz * dz < reach * reach) scene.looseChunks.push_back(h);
            }
        }
    }
    scene.looseChunkCount = uint32_t(scene.looseChunks.size());
    cm.culledAt[0] = camX;
    cm.culledAt[1] = camY;
    cm.culledAt[2] = camZ;
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
            if (cm.isFar(res.ring)) {
                int sx, sz, ax, az;
                cm.farSlot(res.ring, res.ix, res.iz, sx, sz, ax, az);
                if (cm.far.upload)
                    cm.far.uploads.push_back({ax, az, std::move(res.farHeights), std::move(res.farColours), std::move(res.farCanopy)});
                cm.setFarState(res.ring, res.ix, res.iz, true);
                cm.far.maxHeight[size_t(sz) * size_t(cm.far.stateWidth) + size_t(sx)] = res.farMax;
            }
            for (ColumnChunk& cc : res.chunks) {
                const int64_t iy = cc.iy;
                col.chunks.emplace_back(iy, installChunk(scene, r, res.ix, res.iz, std::move(cc)));
            }
            if (cm.propComponent != projv::INVALID_COMPONENT_HANDLE)
                for (const PropInstance& p : res.props) col.props.push_back(installProp(scene, cm, p));
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

// The tree brush's materials (and three ramps of ours), interned into every component after the terrain's,
// in the same order, so a tree's slots are the same whether it stands loose or is baked into a ring.
struct ExtraMaterial { std::string name; uint32_t packed; uint32_t transmission; };
std::vector<ExtraMaterial> gExtraMaterials;
std::vector<uint8_t> gExtraSlots;   // the slot each took (the same in every component)

// The terrain's palette slots: five shades a material, one for a one-shade material (water, flowers).
const int kTerrainSlots = [] {
    int n = 0;
    for (int m = 0; m < kMaterialCount; ++m) n += oneShade(Material(m)) ? 1 : kShades;
    return n;
}();

Palette internPalette(projv::Scene& scene, projv::ComponentRecord& comp) {
    Palette p;
    static const char* names[] = {"soil", "redsoil", "grama", "sediment", "sand", "tundra",
                                  "juniper", "pinyon", "ponderosa", "fir", "spruce", "aspengold", "aspenbare", "oak",
                                  "chamisa", "chamisabloom", "cholla", "yucca", "snakeweed", "bark", "ponderosabark", "aspentrunk", "deadwood",
                                  "granite", "gneiss", "lichen", "basalt", "snow", "water",
                                  "aster", "lupine", "daisy", "paintbrush"};
    static_assert(sizeof(names) / sizeof(names[0]) == kMaterialCount, "a name for every material");
    for (int m = 0; m < kMaterialCount; ++m)
        for (int s = 0; s < kShades; ++s) {
            const Rgb c = shadeColour(Material(m), s);
            p.slot[size_t(m)][size_t(s)] = projv::utils::internMaterial(
                scene, comp, std::string("mountains.") + names[m] + "." + std::to_string(oneShade(Material(m)) ? 0 : s),   // one slot for a one-shade material
                projv::packRGB10(float(c.r), float(c.g), float(c.b)));
        }
    // What the reference renderer reads besides the colour (renderer/shaders/trace.frag): the `metallic`
    // byte is how smooth a surface is drawn (R6: rock, stone, snow), glossiness water's mirror and snow's
    // sheen, transmission the light through leaves. Nothing else draws this scene.
    for (int m = 0; m < kMaterialCount; ++m) {
        const Material mat = Material(m);
        uint32_t smooth = 0, gloss = 0, trans = 0;
        switch (mat) {
            case Material::Granite: case Material::Gneiss: case Material::Lichen: case Material::Basalt: smooth = 255; break;
            case Material::Sediment: smooth = 150; break;
            case Material::Snow: smooth = 200; gloss = 60; break;
            case Material::Water: gloss = 240; break;
            // Thin: grass casts soft shadows (trace.frag), and below the leaf light's 0.2 -- the grass blades are
            // Grama, Tundra and Chamisa, so Chamisa shrubs go without it too.
            case Material::Grama: case Material::Tundra: case Material::Chamisa: case Material::ChamisaBloom: trans = 40; break;
            case Material::Juniper: case Material::Pinyon: case Material::Ponderosa: case Material::Fir:
            case Material::Spruce: case Material::Oak:
            case Material::Cholla: case Material::Yucca: case Material::Snakeweed: trans = 80; break;
            case Material::AspenGold: trans = 150; break;
            default: break;
        }
        for (int sh = 0; sh < kShades; ++sh) {
            projv::Material& e = comp.materialPalette[p.slot[size_t(m)][size_t(sh)]];
            e.packedSurface = (gloss << 24) | (smooth << 16) | (e.packedSurface & 0xFFFFu);
            e.packedExtra = (e.packedExtra & 0xFF00FFFFu) | (trans << 16);
        }
    }
    // The tree brush's materials, after the terrain's (prepareTrees).
    std::vector<uint8_t> extra;
    for (const ExtraMaterial& e : gExtraMaterials) {
        const uint8_t slot = projv::utils::internMaterial(scene, comp, e.name, e.packed);
        comp.materialPalette[slot].packedExtra = (comp.materialPalette[slot].packedExtra & 0xFF00FFFFu) | (e.transmission << 16);
        extra.push_back(slot);
    }
    if (gExtraSlots.empty()) gExtraSlots = extra;
    else if (extra != gExtraSlots) projv::core::warn("[SANTA FE] a component interned the tree materials into different slots");
    comp.paletteVersion++;
    return p;
}

// ---- Camera --------------------------------------------------------------------------------------

// The player at game scale (surface.hpp kGameScale): 1.8 m tall, eyes at 1.65 m, in the player's metres.
constexpr double kPlayerHeight = 1.8 * kGameScale;
constexpr double kEyeHeight = 1.65 * kGameScale;

struct Camera {
    double x = 0, y = 2200, z = 0;   // the eye
    float yaw = 0.0f, pitch = 0.05f;
    bool walk = true;                // the character (G: fly)
    double vy = 0.0;                 // walking: vertical speed (m/s)
    bool grounded = false;
};

void placeCamera(Camera& cam, const Landform& land) {
    std::string spec = std::getenv("MOUNTAINS_CAMERA") ? std::getenv("MOUNTAINS_CAMERA") : "plaza";
    double x = 0, z = 0, above = kEyeHeight;
    float yaw = 0.0f, pitch = 0.05f;   // east, a little up
    if (spec == "plaza") { above = kEyeHeight; }
    else if (spec == "atalaya") { x = 6900; z = 300; above = 3; yaw = 3.14159f; pitch = -0.05f; }
    else if (spec == "baldy") { x = 15700; z = 15400; above = 40; yaw = 3.3f; pitch = -0.08f; }
    else if (spec == "aerial") { x = -12000; z = -9000; above = 2500; yaw = 0.65f; pitch = -0.18f; }
    else std::sscanf(spec.c_str(), "%lf,%lf,%lf,%f,%f", &x, &z, &above, &yaw, &pitch);
    cam.x = x;
    cam.z = z;
    cam.y = std::getenv("SANTA_FE_CAMERA_ABS") ? above : land.height(x, z, 0.25) + above;   // ABS: `above` is the height
    cam.yaw = yaw;
    cam.pitch = pitch;
    cam.walk = above <= kEyeHeight * 1.5;   // in the air: flying (G walks)
}

// ---- The program ---------------------------------------------------------------------------------

struct State {
    std::unique_ptr<Landform> land;
    Palette palette;
    ShapeLibrary props;   // shapes; their voxels are blobs in the scene too (the near rings' loose objects)
    std::vector<TreeVoxels> grown;           // the tree brush's trees (prepareTrees)
    std::vector<std::vector<int>> grownMap;  // each tree's brush materials, as gExtraMaterials indices
    Clipmap clipmap;
    std::unique_ptr<Pool> pool;
    Camera camera;
    std::shared_ptr<projv::ConstructedRenderer> renderer;   // null: the basic renderer
    projv::CameraView lastView;
    bool profiling = false;   // during a capture's settled seconds: GPU time by pass
    double passMs[32] = {};
    double gpuFrameMs = 0;
    int profiled = 0;
    int renderFrame = 0;
    double cameraAbove = 0;   // over the ground, refreshed a few times a second
    double camSpeed = 0, lastCamX = 0, lastCamZ = 0;   // horizontal, m/s (smoothed)
    Clock::time_point groundCheck;
    Clock::time_point started = Clock::now();
    Clock::time_point lastReport = Clock::now();
    double frameMs = 0;
    int frames = 0;
    std::string capturePath;
    int captureStage = 0;
    // The reference renderer (santa-fe-reference): its switches and knobs, what they were last frame (a
    // change starts the accumulation over), and how many frames are accumulated.
    struct Look {
        uint32_t flags = 0;
        uint32_t flags2 = 0;   // the second word (mountains_far.sc SF2_*: a float holds 24 bits)
        float exposure = 1.7f, sunDisc = 1.0f;
        float cloudCoverage = -0.12f, cloudBase = 4300.0f, cloudTop = 9800.0f, cloudExtinction = 0.06f;
        float cloudScale = 11000.0f, cloudWindX = 0.0f, cloudWindZ = 0.0f;
        int cloudSteps = 56;
        float hazeDensity = 2.5e-5f, hazeBase = 1700.0f, hazeHeight = 450.0f, hazeG = 0.55f;
        float treeCell = float(kTreeCell), crownScale = 1.0f, farDetail = 1.0f, patches = 1.0f;
        float smoothGround = 0.0f;
        float glowThreshold = 3.0f, glowKnee = 1.0f, glowSpread = 2.5f;
        float glow = 0.12f, vignette = 0.35f, contrast = 1.2f, saturation = 1.25f, vibrance = 0.4f;
        // Quality (renderer/include/mountains_far.sc sfQuality): smoothing radius, cloud light steps, far
        // shadow reach (m), far detail octaves (share).
        int smoothRadius = 2, cloudLightSteps = 6;
        float renderScale = 0.5f;   // the trace pass's share of the window, each way
        bool reconstruct = true, reconDebug = false;
        int blockFactor = 4;   // blocky distance: a block, in far texels (1, 2, 4, 8)
        float shadowSoftness = 0.025f;   // shadow blur: penumbra per metre of blocker distance
        float glintBright = 30.0f, glintSharp = 0.9985f;   // sun glints on snow and rock
        float cloudBright = 0.75f;       // the clouds' light (santa_fe_clouds.sc), against the sky's
        float shadowVoxels = 120.0f;     // sun-shadow rays through voxels this far (m), the far field's heightfield past it (0: all the way)
        float shadowLODVoxels = 48.0f;   // and a coarser march past this many voxels along them
        float bevelWidth = 0.15f, bevelStrength = 0.8f, smoothMore = 0.5f;   // Normals
        float sssStrength = 0.6f, sssDepth = 1.2f, sssWrap = 0.3f, leafSheen = 1.0f;
        float leafSpread = 1.5f, leafSpreadGain = 0.5f;   // the glow's light spread through a crown (shadow_soft)
        float aoReach = 6.0f, aoStrength = 0.85f, aoBlur = 4.0f;
        int aoSteps = 32;            // the traced AO's ray (ao_trace.frag, half the trace's resolution)
        float aoRange = 300.0f;      // ... out to this far (m)
        bool ssao = true;            // and a screen-space AO at the trace's resolution (shadow_soft)
        float ssaoRadius = 0.4f, ssaoStrength = 0.8f;
        float moteDensity = 0.25f, moteSize = 0.012f, moteBright = 1.0f, moteDrift = 0.25f;
        float rayStrength = 0.35f, rayDecay = 0.96f, rayLength = 0.9f;
        float farShadow = 40000.0f, detailOctaves = 1.0f;
        float air = 1.0f, airHaze = 0.15f;
        float sunAzimuth = 250.0f, sunElevation = 22.0f;
        bool operator==(const Look& o) const { return std::memcmp(this, &o, sizeof(Look)) == 0; }
    } look, lastLook;
    int accumulated = 0;
    int maxSamples = 4096;
    bool sceneChanged = true;
    Clock::time_point lastMove;
    int captureSamples = 256;
    // SANTA_FE_BENCH: once settled, every feature switched off in turn (and the presets), GPU ms by pass for
    // each, one window, one batch -- what each part of the image costs.
    struct BenchCase { std::string name; uint32_t flags; };
    std::vector<BenchCase> bench;
    int benchCase = -1, benchFrame = 0;
    double benchTrace = 0, benchTotal = 0;
    int benchSamples = 0;
    std::vector<std::string> benchLines;
    Clock::time_point settledAt;
    int settledFrames = 0;
};

void setUpRings(State& st, projv::Scene& scene) {
    Clipmap& cm = st.clipmap;
    cm.voxelRings = std::clamp(envInt("MOUNTAINS_VOXEL_RINGS", 6), 1, 14);
    cm.rings = std::clamp(envInt("MOUNTAINS_RINGS", 12), cm.voxelRings, cm.voxelRings + FarField::kAtlasCols * FarField::kAtlasRows);
    cm.radius = std::max<int64_t>(2, (envInt("MOUNTAINS_N", 512) / kChunkRes) & ~int64_t(1));
    if (cm.rings > cm.voxelRings && cm.radius != 8) {
        projv::core::warn("[MOUNTAINS] the far field's atlases are sized for MOUNTAINS_N=512; using it");
        cm.radius = 8;
    }
    {
        FarField& f = cm.far;
        const int square = int(cm.span()) * kChunkRes;
        f.width = FarField::kAtlasCols * square;
        f.height = FarField::kAtlasRows * square;
        f.stateWidth = int(cm.span()) * FarField::kAtlasCols * FarField::kAtlasRows;
        f.stateHeight = int(cm.span());
        f.state.assign(size_t(f.stateWidth) * size_t(f.stateHeight) * 4, 0);
        f.maxHeight.assign(size_t(f.stateWidth) * size_t(f.stateHeight), 1e9f);
    }
    cm.ring.resize(size_t(cm.rings));
    for (int k = 0; k < cm.rings; ++k) {
        Ring& r = cm.ring[size_t(k)];
        r.k = k;
        if (cm.isFar(k)) continue;   // a heightfield: no grid, no component
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


// Grows the trees with the scene editor's tree brush (brush_trees.hpp), before the rings intern their
// palettes: the brush's materials, and for each tree which of them each of its brush materials is (an aspen's
// autumn leaves gold, a ponderosa's bark cinnamon, an aspen's bark white).
void prepareTrees(State& st) {
#ifdef SANTA_FE_BRUSH_TREES
    if (std::getenv("MOUNTAINS_BRUSH_TREES") && std::string(std::getenv("MOUNTAINS_BRUSH_TREES")) == "0") return;
    TreeGrower grower;
    std::string error;
    if (!grower.load(error)) {
        projv::core::warn("[SANTA FE] the tree brush did not load ({}): trees.hpp's trees instead", error);
        return;
    }
    const auto& slots = grower.slots();
    std::vector<std::string> ramp(slots.size());
    std::vector<int> step(slots.size()), rampLength(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) {
        const std::string& n = slots[i].declaredName;
        const size_t dot = n.find_last_of('.');
        const bool numbered = dot != std::string::npos && dot + 1 < n.size() && std::isdigit(uint8_t(n[dot + 1]));
        ramp[i] = numbered ? n.substr(0, dot) : n;
        step[i] = (i > 0 && ramp[i] == ramp[i - 1]) ? step[i - 1] + 1 : 0;
        const std::string& r = ramp[i];
        const bool leafy = r == "tree.leaf" || r == "tree.needle" || r == "tree.autumn" || r == "tree.juniper" ||
                           r == "tree.pinyon" || r == "tree.sage" || r == "tree.blossom";
        gExtraMaterials.push_back({slots[i].name, projv::packRGB10(slots[i].color[0], slots[i].color[1], slots[i].color[2]), leafy ? 90u : 0u});
    }
    for (size_t i = slots.size(); i-- > 0;) rampLength[i] = (i + 1 < slots.size() && ramp[i + 1] == ramp[i]) ? rampLength[i + 1] : step[i] + 1;
    auto ourRamp = [&](const char* name, std::initializer_list<std::array<float, 3>> colours, uint32_t trans) {
        std::vector<int> out;
        int k = 0;
        for (const auto& c : colours) {
            out.push_back(int(gExtraMaterials.size()));
            gExtraMaterials.push_back({std::string("santafe.") + name + "." + std::to_string(k++), projv::packRGB10(c[0], c[1], c[2]), trans});
        }
        return out;
    };
    const auto gold = ourRamp("aspengold", {{0.30, 0.20, 0.05}, {0.55, 0.38, 0.06}, {0.80, 0.58, 0.10}, {0.95, 0.75, 0.18}, {1.00, 0.88, 0.40}}, 150u);
    const auto cinnamon = ourRamp("ponderosabark", {{0.22, 0.12, 0.08}, {0.42, 0.23, 0.13}, {0.62, 0.36, 0.20}, {0.76, 0.50, 0.32}}, 0u);
    const auto white = ourRamp("aspenbark", {{0.30, 0.29, 0.27}, {0.62, 0.61, 0.57}, {0.82, 0.81, 0.76}, {0.92, 0.91, 0.86}}, 0u);
    auto remap = [&](size_t i, const std::vector<int>& to) {
        return to[size_t(std::clamp(int(std::lround(double(step[i]) / std::max(1, rampLength[i] - 1) * double(to.size() - 1))), 0, int(to.size()) - 1))];
    };
    const auto t0 = Clock::now();
    int fresh = 0;
    for (const BrushDesign& base : santaFeDesigns()) {
        // Each design at two heights, so an instance finds a variant near its own (they are not scaled: their
        // voxels stay the lattice's).
        for (double k : {0.8, 1.2}) {
            BrushDesign d = base;
            d.heightM *= k;
            d.name += k < 1.0 ? "_s" : "_l";
            TreeVoxels t;
            const auto g0 = Clock::now();
            if (!grower.grow(d, t, error)) {
                projv::core::warn("[SANTA FE] tree {}: {}", d.name, error);
                continue;
            }
            if (std::chrono::duration<double>(Clock::now() - g0).count() > 0.05) ++fresh;
            std::vector<int> map(slots.size());
            for (size_t i = 0; i < slots.size(); ++i) {
                map[i] = int(i);
                if (d.kind == TreeKind::Aspen && ramp[i] == "tree.autumn") map[i] = remap(i, gold);
                if (d.kind == TreeKind::Aspen && (ramp[i] == "tree.bark" || ramp[i] == "tree.branch")) map[i] = remap(i, white);
                if (d.kind == TreeKind::Ponderosa && ramp[i] == "tree.bark") map[i] = remap(i, cinnamon);
            }
            st.grown.push_back(std::move(t));
            st.grownMap.push_back(std::move(map));
        }
    }
    projv::core::info("[SANTA FE] {} trees from the tree brush ({} grown, the rest cached) in {:.1f} s", st.grown.size(), fresh,
                      std::chrono::duration<double>(Clock::now() - t0).count());

    // Boulder formations from the Boulders brush (brush_trees.hpp santaFeBoulders).
    TreeGrower rocks("boulders");
    if (rocks.load(error)) {
        const auto t1 = Clock::now();
        const size_t offset = gExtraMaterials.size();
        for (const auto& slot : rocks.slots())
            gExtraMaterials.push_back({slot.name, projv::packRGB10(slot.color[0], slot.color[1], slot.color[2]), 0u});
        int count = 0;
        for (const BoulderDesign& b : santaFeBoulders()) {
            TreeVoxels t;
            if (!rocks.grow(b.design, t, error, b.voxel)) {
                projv::core::warn("[SANTA FE] boulders {}: {}", b.design.name, error);
                continue;
            }
            t.family = b.family;
            std::vector<int> map(rocks.slots().size());
            for (size_t i = 0; i < map.size(); ++i) map[i] = int(offset + i);
            st.grown.push_back(std::move(t));
            st.grownMap.push_back(std::move(map));
            ++count;
        }
        projv::core::info("[SANTA FE] {} boulder formations from the Boulders brush in {:.1f} s", count,
                          std::chrono::duration<double>(Clock::now() - t1).count());
    } else {
        projv::core::warn("[SANTA FE] the Boulders brush did not load ({})", error);
    }

    // Only the materials something uses go into the palettes (255 slots, the terrain's 145 first).
    std::vector<int> used(gExtraMaterials.size(), -1);
    std::vector<ExtraMaterial> compact;
    for (size_t i = 0; i < st.grown.size(); ++i)
        for (uint8_t m : st.grown[i].slot) {
            if (m >= st.grownMap[i].size()) continue;
            const int e = st.grownMap[i][m];
            if (used[size_t(e)] < 0) { used[size_t(e)] = int(compact.size()); compact.push_back(gExtraMaterials[size_t(e)]); }
        }
    for (auto& map : st.grownMap)
        for (int& e : map) e = std::max(0, used[size_t(e)]);
    projv::core::info("[SANTA FE] {} of the brushes' {} materials in use ({} palette slots in all)", compact.size(),
                      gExtraMaterials.size(), compact.size() + size_t(kTerrainSlots));
    gExtraMaterials = std::move(compact);
#else
    (void)st;
#endif
}

// The shape library (placement.hpp): everything that stands on the ground, at the lattice's voxel; every
// shape a blob held by the library (one reference) for the near rings' loose objects, and a component for
// them with the rings' palette.
void setUpProps(State& st, projv::Scene& scene) {
    if (std::getenv("MOUNTAINS_PROPS") && std::string(std::getenv("MOUNTAINS_PROPS")) == "0") return;
    const auto t0 = Clock::now();
    Clipmap& cm = st.clipmap;
    projv::ComponentRecord rec;
    rec.kind = projv::ComponentKind::Chunk;
    rec.name = "props";
    rec.sourcePath = "internal/mountains";
    cm.propComponent = projv::utils::reserveComponentSlot(scene);
    projv::utils::placeComponent(scene, cm.propComponent, std::move(rec));
    const Palette p = internPalette(scene, scene.components[cm.propComponent]);
    if (std::memcmp(&p, &st.palette, sizeof(Palette)) != 0)
        projv::core::warn("[MOUNTAINS] the props interned their palette into different slots");
    std::vector<std::vector<uint8_t>> treeSlots;
    for (const std::vector<int>& map : st.grownMap) {
        std::vector<uint8_t> s(map.size());
        for (size_t i = 0; i < map.size(); ++i) s[i] = gExtraSlots[size_t(map[i])];
        treeSlots.push_back(std::move(s));
    }
    st.props = buildShapeLibrary(uint32_t(envInt("MOUNTAINS_SEED", 1)), st.palette, st.grown, treeSlots);
    size_t bytes = 0;
    for (size_t i = 0; i < st.props.shapes.size(); ++i) {
        projv::GeometryBlob blob;
        projv::utils::setBlobTiles(blob, std::move(st.props.tiles[i]));
        blob.refCount = 1;
        blob.dirty = true;
        bytes += projv::utils::blobVoxelBytes(blob);
        cm.propBlob.push_back(projv::poolInsertBlob(scene, std::move(blob)));
        cm.propRes.push_back(st.props.shapes[i].res);
    }
    st.props.tiles.clear();
    projv::core::info("[MOUNTAINS] {} shapes, {:.1f} MB, {:.0f} ms", st.props.shapes.size(), double(bytes) / 1048576.0,
                      std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
}

projv::GPUData* app_gpu = nullptr;
void report(State& st, projv::Scene& scene) {
    const Clipmap& cm = st.clipmap;
    size_t chunks = 0, bytes = 0;
    size_t props = 0;
    for (const projv::Chunk& c : scene.chunks)
        if (c.alive) {
            if (c.componentHandle == cm.propComponent) { ++props; continue; }   // shared blobs: not counted per prop
            ++chunks;
            if (c.geometryPoolIndex >= 0) bytes += projv::utils::blobVoxelBytes(scene.geometryPool[size_t(c.geometryPoolIndex)]);
        }
    std::string rings;
    for (const Ring& r : cm.ring) rings += " " + std::to_string(r.pending);
    if (std::getenv("MOUNTAINS_DEBUG_PROPS") && app_gpu) {
        static bool once = false;
        if (!once) {
            once = true;
            for (size_t i = 0; i < cm.propBlob.size(); i += 4) {
                const int32_t b = cm.propBlob[i];
                const projv::GPUBlobRange& r = app_gpu->blobRanges[size_t(b)];
                projv::core::info("[SANTA FE] shape {} kind {} res {} blob {} ref {} uploaded {} geom {}+{} mat {}+{} bytes {}", i,
                                  int(st.props.shapes[i].kind), st.props.shapes[i].res, b, scene.geometryPool[size_t(b)].refCount,
                                  int(r.uploaded), r.geomTexelOffset, r.geomTexelLen, r.matTexelOffset, r.matTexelLen,
                                  projv::utils::blobVoxelBytes(scene.geometryPool[size_t(b)]));
            }
        }
    }
    if (std::getenv("MOUNTAINS_DEBUG_PROPS")) {
        int byKind[kPropKinds] = {};
        for (projv::ChunkHandle h : scene.looseChunks)
            for (size_t i = 0; i < cm.propBlob.size(); ++i)
                if (cm.propBlob[i] == scene.chunks[h].geometryPoolIndex) { ++byKind[int(st.props.shapes[i].kind)]; break; }
        std::string k;
        for (int i = 0; i < kPropKinds; ++i) k += " " + std::to_string(byKind[i]);
        projv::core::info("[SANTA FE] shown by kind:{}", k);
    }
    projv::core::info("[MOUNTAINS] {:.1f} ms/frame, camera ({:.0f}, {:.0f}, {:.0f}); pending by ring:{}; {} chunks, {:.0f} MB voxels; "
                      "{} props ({} shown)",
                      st.frames ? st.frameMs / st.frames : 0.0, st.camera.x, st.camera.y, st.camera.z, rings, chunks,
                      double(bytes) / 1048576.0, props, scene.looseChunks.size());
}

// ---- The renderer (plan D11; renderer/, forked from the gallery's fast) ----------------------------

// The air, as mountains_sky.sc models it: sea-level-relative air density, haze, the haze's forward
// scattering, and multiple scattering's share. High, dry air: the haze is light.
projv::core::vec4 kAtmosphere{1.0f, 0.15f, 0.6f, 0.8f};
constexpr double kSunIrradiance = 6.0;   // above the air, in the renderer's units
float kExposure = 1.7f;

// MOUNTAINS_AIR="density,haze,g,multiple" and MOUNTAINS_EXPOSURE override the above, for tuning.
void readLookEnvironment() {
    if (const char* air = std::getenv("MOUNTAINS_AIR"))
        std::sscanf(air, "%f,%f,%f,%f", &kAtmosphere.x, &kAtmosphere.y, &kAtmosphere.z, &kAtmosphere.w);
    if (const char* e = std::getenv("MOUNTAINS_EXPOSURE")) kExposure = float(std::atof(e));
}

vec3 sunDirection(const State& st) {
    const double az = st.look.sunAzimuth * kPi / 180.0, el = st.look.sunElevation * kPi / 180.0;
    return vec3(float(std::sin(az) * std::cos(el)), float(std::sin(el)), float(std::cos(az) * std::cos(el)));
}

// The sun's light at height y: what is left after the air and the ozone above it along the sun's
// path (Kasten-Young airmass), so a low sun is warm and dim, and nothing once it has set. The ozone
// (Chappuis band) takes out green on a long path, which is why a twilight sky is blue and pink and not
// green.
vec3 sunIrradianceAt(const State& st, double y) {
    const double el = st.look.sunElevation;
    const double airmass = 1.0 / (std::sin(std::max(el, 0.0) * kPi / 180.0) + 0.50572 * std::pow(std::max(el, 0.0) + 6.07995, -1.6364));
    const double beta[3] = {5.8e-6, 13.5e-6, 33.1e-6};
    const double ozone[3] = {0.65e-6, 1.881e-6, 0.085e-6};   // per metre, at the layer's peak
    const double ozoneThickness = 15000.0;                   // its column, as metres at peak density
    const double white[3] = {1.0, 0.985, 0.955};
    const double haze = 3.0e-6 * kAtmosphere.y * 1.11 * 1200.0 * std::exp(-y / 1200.0);
    const double up = smoothstep(-1.0, 2.0, el);
    float c[3];
    for (int i = 0; i < 3; ++i) {
        const double tau = (beta[i] * kAtmosphere.x * 8000.0 * std::exp(-y / 8000.0) + haze + ozone[i] * ozoneThickness) * airmass;
        c[i] = float(kSunIrradiance * white[i] * std::exp(-tau) * up);
    }
    return vec3(c[0], c[1], c[2]);
}
vec3 sunIrradiance(const State& st) { return sunIrradianceAt(st, st.camera.y); }

constexpr uint32_t kCloudNoiseTexture = 1;

// The renderer's switches: renderer/include/mountains_far.sc's SF_* bits, in that order.
enum Feature : uint32_t {
    kShadows = 1u << 0, kSoftShadows = 1u << 1, kDiffuse = 1u << 2, kBounceSun = 1u << 3, kSmooth = 1u << 4,
    kClouds = 1u << 5, kCloudShadow = 1u << 6, kHaze = 1u << 7, kShafts = 1u << 8, kFarTrees = 1u << 9,
    kFarDetail = 1u << 10, kFarPatches = 1u << 11, kAerial = 1u << 12, kGloss = 1u << 13, kTranslucent = 1u << 14,
    kProps = 1u << 15, kCloudDetail = 1u << 16, kSkip = 1u << 17,
    kAllFeatures = (1u << 18) - 1u,
    kFarBlocky = 1u << 18,  // a style, not a feature: never in kAllFeatures
    kFaceShading = 1u << 19, // voxels lit once per face (shadow, cloud shadow, ground normal at the face's centre)
    kShadowBlur = 1u << 20   // soft shadows from one hard ray: a screen-space blur by blocker distance (optimized mode)
};
enum Feature2 : uint32_t {   // the second word: mountains_far.sc SF2_*
    kCloudBlocky = 1u, kSmoothGround = 2u, kSmoothMore = 4u, kBevel = 8u, kSSS = 16u, kMotes = 32u, kGodRays = 64u,
    kAO = 128u, kFarShadowLow = 256u, kGlints = 512u,
    kNormalBlend = kSmoothGround | kSmoothMore | kBevel   // with kSmooth (the first word): the panel's "normal blend"
};
struct FeatureName { Feature bit; const char* name; const char* help; };
constexpr FeatureName kFeatureNames[] = {
    {kShadows, "sun shadows", "a shadow ray toward the sun from every hit"},
    {kSoftShadows, "soft shadows", "the shadow ray aims at a random point of the sun's disc"},
    {kDiffuse, "sky light + bounce (traced)", "a cosine-weighted ray from every hit: sky occlusion and one bounce"},
    {kBounceSun, "bounce lit by the sun", "a shadow ray from the bounce's hit (else the bounce is sky-lit only)"},
    {kSmooth, "smooth rock and snow", "normals from the voxel occupancy round the hit, for materials flagged smooth (also in Normals)"},
    {kClouds, "clouds", "the volumetric cumulus layer"},
    {kCloudDetail, "cloud detail", "the Worley erosion octaves on camera rays"},
    {kCloudShadow, "cloud shadows", "the sun through the clouds, on the ground, the bounce and the haze"},
    {kHaze, "valley haze", "height fog in the basin"},
    {kShafts, "haze shafts (traced)", "one shadowed sample of the haze per pixel per frame"},
    {kFarTrees, "far trees as geometry", "crowns marched on the far heightfield (off: the cover raises it, as in 55)"},
    {kFarDetail, "far detail normals", "55's gully and grain normals below the far field's texels"},
    {kFarPatches, "far ground patches", "bare earth and grass in patches between the far trees"},
    {kAerial, "aerial perspective", "the air between the camera and what it sees"},
    {kGloss, "gloss (water, snow)", "water mirrors the sky; snow's sheen"},
    {kTranslucent, "leaf translucency", "light through backlit crowns"},
    {kProps, "props (rocks, shrubs)", "the loose chunks on every ray"},
    {kSkip, "depth pre-pass skip", "camera rays start where the 1/8 pre-pass says the ground can begin"},
    {kFaceShading, "per-face shading", "shadow, cloud shadow and the ground's normal taken at each voxel face's centre: every texel of a face agrees, so the reconstruction is exact from the first frame; off: per hit (shadow edges fall across faces)"},
    {kShadowBlur, "shadow blur (by distance)", "soft shadows from the hard shadow ray: blurred in screen space, wider the farther the blocker (optimized mode; reference traces the sun's disc)"},
    {kFarBlocky, "blocky far ground", "stylized: the far field as big flat-topped blocks, larger with distance (faster to draw and to generate)"},
};

// ---- The clouds' noise (renderer/include/santa_fe_clouds.sc) -------------------------------------
//
// A tileable 128^3 noise as 128 slices of 128^2 in a 2080 x 1040 RGBA8 atlas (16 x 8 tiles of 130^2, a
// texel of wrapped gutter round each): r Perlin-Worley, g b a Worley at 8, 16 and 32 cells a period.
namespace cloudnoise {
constexpr int N = 128, Tile = 130, W = 2080, H = 1040;

double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
uint32_t h3(int x, int y, int z, uint32_t seed) {
    return hash32(uint32_t(x) * 0x8da6b343U ^ uint32_t(y) * 0xd8163841U ^ uint32_t(z) * 0xcb1ab31fU ^ seed);
}
// Perlin noise with `period` cells a period, -1..1.
double perlin(double x, double y, double z, int period, uint32_t seed) {
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y)), z0 = int(std::floor(z));
    const double fx = x - x0, fy = y - y0, fz = z - z0;
    auto grad = [&](int i, int j, int k, double dx, double dy, double dz) {
        const uint32_t h = h3(((i % period) + period) % period, ((j % period) + period) % period, ((k % period) + period) % period, seed);
        static const double g[12][3] = {{1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},{1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},{0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1}};
        const double* v = g[h % 12];
        return v[0] * dx + v[1] * dy + v[2] * dz;
    };
    const double u = fade(fx), v = fade(fy), w = fade(fz);
    double r = 0;
    for (int c = 0; c < 8; ++c) {
        const int i = c & 1, j = (c >> 1) & 1, k = (c >> 2) & 1;
        r += (i ? u : 1 - u) * (j ? v : 1 - v) * (k ? w : 1 - w) * grad(x0 + i, y0 + j, z0 + k, fx - i, fy - j, fz - k);
    }
    return r;
}
// Inverted Worley (1 at a feature point, 0 a cell away), `cells` a period.
double worley(double x, double y, double z, int cells, uint32_t seed) {
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y)), z0 = int(std::floor(z));
    double best = 1e9;
    for (int k = -1; k <= 1; ++k)
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                const int cx = x0 + i, cy = y0 + j, cz = z0 + k;
                const uint32_t h = h3(((cx % cells) + cells) % cells, ((cy % cells) + cells) % cells, ((cz % cells) + cells) % cells, seed);
                const double px = cx + unit(h), py = cy + unit(hash32(h + 1)), pz = cz + unit(hash32(h + 2));
                const double d = (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z);
                best = std::min(best, d);
            }
    return 1.0 - std::min(std::sqrt(best), 1.0);
}

std::vector<uint8_t> bake() {
    const auto t0 = Clock::now();
    std::vector<uint8_t> volume(size_t(N) * N * N * 4);
    const unsigned threads = std::max(2u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    std::atomic<int> next{0};
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int z; (z = next++) < N;)
                for (int y = 0; y < N; ++y)
                    for (int x = 0; x < N; ++x) {
                        const double u = double(x) / N, v = double(y) / N, w = double(z) / N;
                        double p = 0, amp = 1, norm = 0;
                        for (int o = 0; o < 4; ++o) {
                            const int f = 4 << o;
                            p += perlin(u * f, v * f, w * f, f, 11u + uint32_t(o)) * amp;
                            norm += amp;
                            amp *= 0.5;
                        }
                        p = clamp01(0.5 + 0.75 * p / norm);
                        auto wf = [&](int f, uint32_t sd) {
                            return worley(u * f, v * f, w * f, f, sd) * 0.625 + worley(u * 2 * f, v * 2 * f, w * 2 * f, 2 * f, sd + 1) * 0.25 +
                                   worley(u * 4 * f, v * 4 * f, w * 4 * f, 4 * f, sd + 2) * 0.125;
                        };
                        const double w4 = wf(4, 101u);
                        // Perlin-Worley: the Perlin billows, dented by the Worley cells.
                        const double pw = clamp01((p - (1.0 - w4)) / std::max(w4, 1e-3) * 0.5 + 0.5 * p + 0.25 * w4);
                        uint8_t* out = &volume[((size_t(z) * N + size_t(y)) * N + size_t(x)) * 4];
                        out[0] = uint8_t(std::lround(pw * 255.0));
                        out[1] = uint8_t(std::lround(clamp01(wf(8, 201u)) * 255.0));
                        out[2] = uint8_t(std::lround(clamp01(wf(16, 301u)) * 255.0));
                        out[3] = uint8_t(std::lround(clamp01(wf(32, 401u)) * 255.0));
                    }
        });
    for (auto& t : pool) t.join();
    std::vector<uint8_t> atlas(size_t(W) * H * 4);
    for (int sl = 0; sl < N; ++sl) {
        const int tx = (sl % 16) * Tile, ty = (sl / 16) * Tile;
        for (int gy = 0; gy < Tile; ++gy)
            for (int gx = 0; gx < Tile; ++gx) {
                const int x = (gx - 1 + N) % N, y = (gy - 1 + N) % N;
                std::memcpy(&atlas[(size_t(ty + gy) * W + size_t(tx + gx)) * 4], &volume[((size_t(sl) * N + size_t(y)) * N + size_t(x)) * 4], 4);
            }
    }
    projv::core::info("[SANTA FE] cloud noise baked in {:.0f} ms",
                      std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    return atlas;
}
}  // namespace cloudnoise

std::shared_ptr<projv::ConstructedRenderer> buildRenderer(projv::graphics::RenderInstance& ri) {
    readLookEnvironment();
    projv::RendererSpecification spec = projv::graphics::loadRendererSpecification("./renderer/");
    ri.addRendererSpecification(1, spec);
    static const bgfx::ShaderHandle vsh = projv::graphics::loadEngineShader("vs_quad");
    auto renderer = projv::graphics::constructRendererSpecification(ri.getRendererSpecification(1), vsh);
    const std::vector<uint8_t> clouds = cloudnoise::bake();
    projv::graphics::setTextureToData(renderer, kCloudNoiseTexture, const_cast<uint8_t*>(clouds.data()), cloudnoise::W, cloudnoise::H);
    ri.setActiveRenderer(renderer);
    return renderer;
}

// Sends the far columns finished since last frame to the atlases, a tile at a time, and the state
// texture when it changed (texture ids in renderer/resources.json).
constexpr uint32_t kFarHeightTexture = 16, kFarColourTexture = 17, kFarStateTexture = 18, kFarCanopyTexture = 19, kFarMaxTexture = 20;
void uploadFarField(State& st) {
    FarField& f = st.clipmap.far;
    if (!st.renderer) return;
    auto& handles = st.renderer->resources.textures.textureHandles;
    const bgfx::TextureHandle heights = handles[kFarHeightTexture], colours = handles[kFarColourTexture];
    const bgfx::TextureHandle canopy = handles[kFarCanopyTexture];
    for (FarField::Upload& u : f.uploads) {
        const uint32_t bytes = uint32_t(kChunkRes * kChunkRes * 4);
        bgfx::updateTexture2D(heights, 0, 0, uint16_t(u.x), uint16_t(u.y), kChunkRes, kChunkRes, bgfx::copy(u.heights.data(), bytes));
        bgfx::updateTexture2D(colours, 0, 0, uint16_t(u.x), uint16_t(u.y), kChunkRes, kChunkRes, bgfx::copy(u.colours.data(), bytes));
        bgfx::updateTexture2D(canopy, 0, 0, uint16_t(u.x), uint16_t(u.y), kChunkRes, kChunkRes, bgfx::copy(u.canopy.data(), bytes));
    }
    if (!f.uploads.empty()) st.sceneChanged = true;
    f.uploads.clear();
    if (f.stateDirty) {
        projv::graphics::setTextureToData(st.renderer, kFarStateTexture, f.state.data(), uint32_t(f.stateWidth), uint32_t(f.stateHeight));
        projv::graphics::setTextureToData(st.renderer, kFarMaxTexture, reinterpret_cast<unsigned char*>(f.maxHeight.data()), uint32_t(f.stateWidth), uint32_t(f.stateHeight));
        f.stateDirty = false;
    }
}

constexpr uint32_t kOptimizedFlags = kShadowBlur | kShadows | kSmooth | kClouds | kCloudShadow | kHaze | kFarTrees | kFarDetail | kFarPatches |
                                    kAerial | kGloss | kTranslucent | kProps | kSkip;

// The two presets. Reference: everything, at full quality. Optimized: what mattered most to the image
// (shadows, the far trees, clouds and their shadows, the air, the props) at a fraction of the cost -- no traced
// sky light or bounce (the most expensive part, and the least missed), hard shadows, cheaper clouds,
// smoothing and far detail (santa-fe-reference S4).
void applyMode(State::Look& L, bool optimized) {
    if (optimized) {
        L.flags = kOptimizedFlags;
        L.flags2 = (L.flags2 & kCloudBlocky) | kSmoothGround | kBevel | kSSS | kMotes | kGodRays | kAO | kFarShadowLow | kGlints;
        L.renderScale = 0.5f;
        L.cloudSteps = 28;
        L.cloudLightSteps = 4;
        L.smoothRadius = 1;
        L.farShadow = 20000.0f;
        L.detailOctaves = 0.6f;
        L.shadowVoxels = 120.0f;
        L.shadowLODVoxels = 48.0f;
    } else {
        L.flags = kAllFeatures;
        L.flags2 = (L.flags2 & kCloudBlocky) | kSmoothGround | kBevel | kSSS | kMotes | kGodRays | kGlints;   // AO: the diffuse rays do it
        L.renderScale = 1.0f;
        L.cloudSteps = 56;
        L.cloudLightSteps = 6;
        L.smoothRadius = 2;
        L.farShadow = 40000.0f;
        L.detailOctaves = 1.0f;
        L.shadowVoxels = 0.0f;
        L.shadowLODVoxels = 512.0f;
    }
}

void runBench(projv::Application& app, State& st) {
    int pending = 0;
    for (const Ring& r : st.clipmap.ring) pending += r.pending;
    if (st.benchCase < 0) {
        if (pending > 0 || std::chrono::duration<double>(Clock::now() - st.started).count() < 5.0) return;
        st.benchCase = 0;
        st.benchFrame = 0;
        bgfx::setDebug(BGFX_DEBUG_PROFILER);
    }
    if (st.benchCase >= int(st.bench.size())) return;
    State::BenchCase& c = st.bench[size_t(st.benchCase)];
    if (st.benchFrame == 0) applyMode(st.look, c.name.rfind("optimized", 0) == 0);
    st.look.flags = c.flags;
    ++st.benchFrame;
    const bgfx::Stats* gs = bgfx::getStats();
    if (st.benchFrame > 4 && gs && gs->gpuTimerFreq > 0) {   // three frames for the switch to reach the GPU
        const double toMs = 1000.0 / double(gs->gpuTimerFreq);
        st.benchTotal += double(gs->gpuTimeEnd - gs->gpuTimeBegin) * toMs;
        for (uint16_t v = 0; v < gs->numViews; ++v)
            if (gs->viewStats[v].view == 1)
                st.benchTrace += double(gs->viewStats[v].gpuTimeEnd - gs->viewStats[v].gpuTimeBegin) * toMs;
        ++st.benchSamples;
    }
    if (st.benchFrame >= 12) {
        char line[160];
        std::snprintf(line, sizeof line, "%-26s trace %7.1f ms   frame %7.1f ms", c.name.c_str(), st.benchTrace / std::max(1, st.benchSamples),
                      st.benchTotal / std::max(1, st.benchSamples));
        st.benchLines.push_back(line);
        projv::core::info("[SANTA FE] bench {}", line);
        st.benchTrace = st.benchTotal = 0;
        st.benchSamples = 0;
        st.benchFrame = 0;
        if (++st.benchCase >= int(st.bench.size())) {
            std::printf("\n%s\n", "santa_fe bench (GPU ms, this window):");
            for (const std::string& l : st.benchLines) std::printf("  %s\n", l.c_str());
            app.closeRequested = true;
        }
    }
}

void render(projv::Application& app) {
    State& st = app.world.ctx().get<State>();
    if (!st.renderer) return;
    auto& ri = app.world.ctx().get<projv::graphics::RenderInstance>();
    auto& scene = app.world.ctx().get<projv::Scene>();
    auto& gpu = app.world.ctx().get<projv::GPUData>();
    projv::graphics::flushSceneUpdates(scene, gpu);
    uploadFarField(st);
    const projv::CameraView& view = app.world.ctx().get<projv::CameraView>();
    projv::graphics::setCamera(*st.renderer, view);
    const bool moved = view.position != st.lastView.position || view.forward != st.lastView.forward;
    st.lastView = view;
    if (moved) st.lastMove = Clock::now();
    // A knob changed: the history is another image (reset). The camera moved or the scene changed (a column
    // installed, a far tile uploaded): the resolve reprojects and clamps it instead.
    const bool reset = !(st.look == st.lastLook) || st.renderFrame == 0;
    const bool shifted = moved || st.sceneChanged;
    const bool changed = reset || shifted;
    st.lastLook = st.look;
    st.sceneChanged = false;
    if (changed) st.accumulated = 0;
    // The render scale (santa-fe-reference S6): the trace pass and its G-buffer at a share of the window each
    // way, the depth pre-pass at an eighth of that; the reconstruction and the resolve rebuild the window.
    {
        auto& scales = st.renderer->resources.textures.relativeTextureScales;
        const float s = std::clamp(st.look.renderScale, 0.2f, 1.0f);
        scales[2] = s / 8.0f;
        for (uint32_t id : {3u, 5u, 6u, 7u, 8u, 9u, 11u, 12u, 21u, 22u, 23u}) scales[id] = s;
        scales[13] = scales[14] = s * 0.5f;   // clouds.frag: a quarter of the trace's texels
        scales[25] = s * 0.5f;                // ao_trace.frag: likewise
    }
    const bool preview = std::chrono::duration<double>(Clock::now() - st.lastMove).count() < 0.3;
    const auto& r = st.renderer;
    const bgfx::Stats* stats = bgfx::getStats();
    const projv::core::vec2 res(float(std::max<uint16_t>(stats->width, 1)), float(std::max<uint16_t>(stats->height, 1)));
    projv::core::vec2 window = res;
    const State::Look& L = st.look;
    kAtmosphere.x = L.air;
    kAtmosphere.y = L.airHaze;
    {
        auto halton = [](int index, int base) {
            float f = 1.0f, v = 0.0f;
            for (; index > 0; index /= base) { f /= float(base); v += f * float(index % base); }
            return v;
        };
        const int phase = st.renderFrame % 16 + 1;
        projv::core::vec4 jitter(halton(phase, 2) - 0.5f, halton(phase, 3) - 0.5f, 0.0f, 0.0f);
        projv::graphics::setUniformToValue(st.renderer, "jitterParams", jitter);
        projv::core::vec4 temporal(reset ? 1.0f : 0.0f, float(st.maxSamples), 12.0f, 0.0f);
        projv::graphics::setUniformToValue(st.renderer, "sfTemporal", temporal);
        projv::core::vec4 recon(0.0f, 0.0f, st.look.reconstruct ? 0.0f : 1.0f, st.look.reconDebug ? 1.0f : 0.0f);
        projv::graphics::setUniformToValue(st.renderer, "sfRecon", recon);
    }
    projv::core::vec4 frame(float(st.renderFrame), shifted ? 1.0f : 0.0f,
                            float(std::min(st.accumulated, st.maxSamples)), st.accumulated >= st.maxSamples ? 1e6f : 0.0f);
    const vec3 sd = sunDirection(st);
    projv::core::vec4 sun(sd.x, sd.y, sd.z, 0.0f);
    projv::core::vec4 params(L.sunDisc, float(L.cloudSteps), preview ? 1.0f : 0.0f, L.smoothGround);
    const vec3 irr = sunIrradiance(st);
    projv::core::vec4 mtnSun(irr.x, irr.y, irr.z, L.exposure);
    projv::core::vec4 atmos = kAtmosphere;
    projv::graphics::setUniformToValue(r, "windowRes", window);
    projv::graphics::setUniformToValue(r, "frameCount", frame);
    projv::graphics::setUniformToValue(r, "sunDir", sun);
    projv::graphics::setUniformToValue(r, "renderParams", params);
    projv::graphics::setUniformToValue(r, "mtnSun", mtnSun);
    projv::graphics::setUniformToValue(r, "mtnAtmos", atmos);
    { projv::core::vec4 u_(float(L.flags), float(L.flags2), 0.0f, 0.0f); projv::graphics::setUniformToValue(r, "sfFlags", u_); }
    { projv::core::vec4 u_(L.bevelWidth, L.bevelStrength, L.smoothMore, 0.0f); projv::graphics::setUniformToValue(r, "sfBevel", u_); }
    { projv::core::vec4 u_(L.sssStrength, L.sssDepth * float(kGameScale), L.sssWrap, L.leafSheen); projv::graphics::setUniformToValue(r, "sfSSS", u_); }
    { projv::core::vec4 u_(L.leafSpread * float(kGameScale), L.leafSpreadGain, 32.0f, 0.0f); projv::graphics::setUniformToValue(r, "sfLeafSpread", u_); }
    { projv::core::vec4 u_(L.aoReach * float(kGameScale), L.aoStrength, float(L.aoSteps), L.aoRange); projv::graphics::setUniformToValue(r, "sfAO", u_); }
    { projv::core::vec4 u_(L.ssaoRadius * float(kGameScale), L.ssaoStrength, 24.0f, L.ssao ? 1.0f : 0.0f); projv::graphics::setUniformToValue(r, "sfSSAO", u_); }
    { projv::core::vec4 u_(L.aoBlur, 0.0f, 0.0f, 0.0f); projv::graphics::setUniformToValue(r, "sfAOBlur", u_); }
    { projv::core::vec4 u_(L.moteDensity, L.moteSize * float(kGameScale), L.moteBright, L.moteDrift * float(kGameScale)); projv::graphics::setUniformToValue(r, "sfMotes", u_); }
    { projv::core::vec4 u_(L.rayStrength, L.rayDecay, L.rayLength, 0.0f); projv::graphics::setUniformToValue(r, "sfRays", u_); }
    { projv::core::vec4 u_(float(std::chrono::duration<double>(Clock::now() - st.started).count()), 0.0f, 0.0f, 0.0f); projv::graphics::setUniformToValue(r, "sfTime", u_); }
    { projv::core::vec4 u_(L.treeCell, L.crownScale, L.farDetail, L.patches); projv::graphics::setUniformToValue(r, "mtnCanopy", u_); }
    { projv::core::vec4 u_(L.cloudCoverage, L.cloudBase, L.cloudTop, L.cloudExtinction); projv::graphics::setUniformToValue(r, "mtnClouds", u_); }
    { projv::core::vec4 u_(L.cloudWindX, L.cloudWindZ, L.cloudScale, L.cloudBright); projv::graphics::setUniformToValue(r, "mtnWind", u_); }
    { projv::core::vec4 u_(L.hazeDensity, L.hazeBase, L.hazeHeight, L.hazeG); projv::graphics::setUniformToValue(r, "mtnFog", u_); }
    { projv::core::vec4 u_(L.glow, L.vignette, L.contrast, L.saturation); projv::graphics::setUniformToValue(r, "sfDisplay", u_); }
    { projv::core::vec4 u_(L.vibrance, 0.0f, 0.0f, 0.0f); projv::graphics::setUniformToValue(r, "sfDisplay2", u_); }
    { projv::core::vec4 u_(float(L.smoothRadius), float(L.cloudLightSteps), L.farShadow, L.detailOctaves); projv::graphics::setUniformToValue(r, "sfQuality", u_); }
    { projv::core::vec4 u_(float(L.blockFactor), 0.0f, 0.0f, 0.0f); projv::graphics::setUniformToValue(r, "sfFar", u_); }
    { projv::core::vec4 u_(L.shadowSoftness, 12.0f, 0.0f, 0.0f); projv::graphics::setUniformToValue(r, "sfShadow", u_); }
    { projv::core::vec4 u_(L.glowThreshold, L.glowKnee, L.glowSpread, 0.0f); projv::graphics::setUniformToValue(r, "sfGlow", u_); }
    { projv::core::vec4 u_(float(snowlineAt()), 0.0f, 0.0f, 0.0f); projv::graphics::setUniformToValue(r, "sfSnow", u_); }
    { projv::core::vec4 u_(L.glintBright, L.glintSharp, 0.0f, 0.0f); projv::graphics::setUniformToValue(r, "sfGlint", u_); }
    // The cloud-shadow map (cloud_shadow.frag): 32 km round the camera, its corner snapped to its texels.
    {
        const double size = 32000.0, texel = size / 256.0;
        const double cx = std::floor(st.camera.x / texel) * texel - size * 0.5, cz = std::floor(st.camera.z / texel) * texel - size * 0.5;
        static const bool noMap = std::getenv("SANTA_FE_NO_CLOUDMAP") != nullptr;
        projv::core::vec4 u_(float(cx), float(cz), float(size), noMap ? 0.0f : 1.0f);
        projv::graphics::setUniformToValue(r, "sfCloudMap", u_);
        // A sixteenth of it refreshed a frame; all of it on the first frames and when the look changes.
        static double pcx = cx, pcz = cz;
        static int frame = 0, fresh = 2;
        if (reset) fresh = 2;
        projv::core::vec4 p_(float(pcx), float(pcz), float(frame++ & 15), fresh > 0 ? 1.0f : 0.0f);
        projv::graphics::setUniformToValue(r, "sfCloudMapPrev", p_);
        if (fresh > 0) --fresh;
        pcx = cx; pcz = cz;
    }
    // Sun-shadow rays: voxels for x metres, then the far field; a coarser march past y voxels.
    { static const float pr = std::getenv("SANTA_FE_SHADOW_PROPS") ? float(std::atof(std::getenv("SANTA_FE_SHADOW_PROPS"))) : 400.0f;
      static const float dbg = std::getenv("SANTA_FE_SHADOW_DEBUG") ? float(std::atof(std::getenv("SANTA_FE_SHADOW_DEBUG"))) : 0.0f;
      projv::core::vec4 u_(L.shadowVoxels, L.shadowLODVoxels, pr, dbg); projv::graphics::setUniformToValue(r, "sfShadowLOD", u_); }
    // Blocky distance's far columns: one sample a block. A change regenerates the far rings.
    {
        const int want = (L.flags & kFarBlocky) ? std::max(1, kChunkRes / std::max(1, L.blockFactor)) : kChunkRes;
        if (want != gFarSamples.load()) {
            gFarSamples.store(want);
            auto& sc = app.world.ctx().get<projv::Scene>();
            for (int k = st.clipmap.voxelRings; k < st.clipmap.rings; ++k) dropRing(st.clipmap, sc, k);
        }
    }
    // The far rings (D15): where each one is, and the box the voxel rings cover, where the heightfield
    // is not marched.
    const Clipmap& cm = st.clipmap;
    for (int f = 0; f < FarField::kAtlasCols * FarField::kAtlasRows; ++f) {
        projv::core::vec4 ringInfo(0.0f);
        const int k = cm.voxelRings + f;
        if (k < cm.rings && cm.ring[size_t(k)].placed) {
            const Ring& fr = cm.ring[size_t(k)];
            const double c = cellSize(k);
            ringInfo = projv::core::vec4(float(double(fr.ox) * c), float(double(fr.oz) * c), float(c), 1.0f);
        }
        projv::graphics::setUniformToValue(r, "mtnFar" + std::to_string(f), ringInfo);
    }
    projv::core::vec4 voxelBox(1.0f, 1.0f, -1.0f, -1.0f);   // empty
    for (int k = cm.voxelRings - 1; k >= 0; --k)
        if (cm.ring[size_t(k)].placed) {
            const Ring& vr = cm.ring[size_t(k)];
            const double c = cellSize(k), span = double(cm.span());
            voxelBox = projv::core::vec4(float(double(vr.ox) * c), float(double(vr.oz) * c),
                                         float((double(vr.ox) + span) * c), float((double(vr.oz) + span) * c));
            break;
        }
    projv::graphics::setUniformToValue(r, "mtnVoxelBox", voxelBox);
    const vec3 air = sunIrradianceAt(st, st.camera.y + 6000.0);
    projv::core::vec4 mtnSunAir(air.x, air.y, air.z, 0.0f);
    projv::graphics::setUniformToValue(r, "mtnSunAir", mtnSunAir);
    projv::graphics::renderConstructedRenderer(ri, r, &gpu);
    ++st.renderFrame;
    ++st.accumulated;
    if (!st.bench.empty()) runBench(app, st);
    if (st.profiling) {
        const bgfx::Stats* gs = bgfx::getStats();
        if (gs && gs->gpuTimerFreq > 0) {
            const double toMs = 1000.0 / double(gs->gpuTimerFreq);
            st.gpuFrameMs += double(gs->gpuTimeEnd - gs->gpuTimeBegin) * toMs;
            for (uint16_t v = 0; v < gs->numViews; ++v)
                if (gs->viewStats[v].view < 32)
                    st.passMs[gs->viewStats[v].view] += double(gs->viewStats[v].gpuTimeEnd - gs->viewStats[v].gpuTimeBegin) * toMs;
            ++st.profiled;
        }
    }
}

void reportPasses(State& st, projv::graphics::RenderInstance& ri) {
    if (!st.profiled) return;
    const projv::RendererSpecification& spec = ri.getRendererSpecification(1);
    std::string line = "[MOUNTAINS] GPU " + std::to_string(st.gpuFrameMs / st.profiled).substr(0, 5) + " ms:";
    const auto& passes = spec.dependencyGraph.renderPasses;
    for (size_t v = 0; v < passes.size() && v < 32; ++v) {
        std::string name = std::to_string(passes[v].shaderID);
        for (const projv::Shader& shader : spec.resources.shaders)
            if (shader.shaderID == passes[v].shaderID) name = std::filesystem::path(shader.filePath).stem().string();
        line += " " + name + " " + std::to_string(st.passMs[v] / st.profiled).substr(0, 5);
    }
    projv::core::info("{}", line);
}

#ifdef PROJV_UI
// The dev panel (F1): every feature a switch and every knob a slider, so the image can be leaned down live
// to see what each part is worth. Any change starts the accumulation over.
void lookPanel(projv::Application& app) {
    State& st = app.world.ctx().get<State>();
    State::Look& L = st.look;
    int pending = 0;
    for (const Ring& r : st.clipmap.ring) pending += r.pending;
    ImGui::Text("samples %d / %d   %.1f ms/frame   pending columns %d", std::min(st.accumulated, st.maxSamples), st.maxSamples,
                st.frames ? st.frameMs / st.frames : 0.0, pending);
    ImGui::Text("camera %.0f, %.0f, %.0f  yaw %.3f pitch %.3f", st.camera.x, st.camera.y, st.camera.z, st.camera.yaw, st.camera.pitch);
    if (ImGui::Button("print camera")) {
        projv::core::info("[SANTA FE] MOUNTAINS_CAMERA={:.1f},{:.1f},{:.1f},{:.4f},{:.4f}  MOUNTAINS_SUN={:.1f},{:.1f}", st.camera.x,
                          st.camera.z, st.camera.y - st.land->height(st.camera.x, st.camera.z, 0.25), st.camera.yaw,
                          st.camera.pitch, L.sunAzimuth, L.sunElevation);
    }
    ImGui::SameLine();
    if (ImGui::Button("save capture")) {
        static int n = 0;
        const std::string path = "santa_fe_" + std::to_string(n++) + ".png";
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, path.c_str());
        projv::core::info("[SANTA FE] saved {}", path);
    }
    if (ImGui::Button("Optimized mode")) applyMode(L, true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shadows, far trees, clouds, the air and the props; no traced sky light; cheaper clouds and detail");
    ImGui::SameLine();
    if (ImGui::Button("Reference mode")) applyMode(L, false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Everything on, at full quality: slow, converges over many frames");
    ImGui::SliderFloat("render scale", &L.renderScale, 0.25f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The trace pass's share of the window each way; the voxel reconstruction and the resolve rebuild the rest");
    ImGui::Checkbox("voxel reconstruction", &L.reconstruct);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: the low-resolution frame point-sampled, to see what the reconstruction does");
    ImGui::SameLine();
    ImGui::Checkbox("show where", &L.reconDebug);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Green: voxels rebuilt the pixel; red: it fell back to filtering; blue: sky");
    ImGui::SliderInt("max samples", &st.maxSamples, 1, 8192);

    // One switch for each feature, in the section it belongs to (each label once: ImGui IDs).
    auto bit = [&](const char* label, uint32_t b, const char* tip) {    // the first word (SF_*)
        bool on = (L.flags & b) != 0;
        if (ImGui::Checkbox(label, &on)) L.flags = on ? (L.flags | b) : (L.flags & ~b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    auto bit2 = [&](const char* label, uint32_t b, const char* tip) {   // the second word (SF2_*)
        bool on = (L.flags2 & b) != 0;
        if (ImGui::Checkbox(label, &on)) L.flags2 = on ? (L.flags2 | b) : (L.flags2 & ~b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    auto tip = [](const char* t) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t); };

    if (ImGui::CollapsingHeader("Sun and exposure", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("sun azimuth", &L.sunAzimuth, 0.0f, 360.0f);
        ImGui::SliderFloat("sun elevation", &L.sunElevation, -4.0f, 89.0f);
        ImGui::SliderFloat("exposure", &L.exposure, 0.2f, 6.0f);
    }
    if (ImGui::CollapsingHeader("Shadows")) {
        bit("sun shadows", kShadows, "a shadow ray toward the sun from every hit");
        bit("shadow blur (by distance)", kShadowBlur, "soft shadows from one hard ray, blurred in screen space, wider the farther the blocker (optimized mode)");
        ImGui::SliderFloat("blur softness", &L.shadowSoftness, 0.0f, 0.1f, "%.3f");
        bit("sun's disc (traced)", kSoftShadows, "the shadow ray aims at a random point of the sun's disc: soft shadows over many frames (reference mode)");
        ImGui::SliderFloat("sun disc (x real)", &L.sunDisc, 0.0f, 8.0f);
        bit2("far shadows at quarter resolution", kFarShadowLow, "the hills' and far crowns' part of the sun's shadow marched once per cloud texel (clouds pass); needs shadow blur");
        ImGui::SliderFloat("voxels for (m)", &L.shadowVoxels, 0.0f, 2000.0f, "%.0f");
        tip("the shadow ray marches voxels this far; past it the far field's heightfield and crowns; 0: voxels all the way");
        ImGui::SliderFloat("coarse past (voxels)", &L.shadowLODVoxels, 4.0f, 512.0f, "%.0f");
        ImGui::SliderFloat("far shadow reach (m)", &L.farShadow, 2000.0f, 60000.0f);
        bit("per-face shading", kFaceShading, "shadow, cloud shadow and the ground's normal at each voxel face's centre (off: per hit)");
    }
    if (ImGui::CollapsingHeader("Leaves", ImGuiTreeNodeFlags_DefaultOpen)) {
        bit2("leaf light", kSSS, "light through leaves (each leaf voxel asks whether the sun reaches it), wrap, sheen; grass and leaves let some sun through, so their shadows are soft");
        ImGui::SliderFloat("glow", &L.sssStrength, 0.0f, 2.0f);
        ImGui::SliderFloat("glow spread (m)", &L.leafSpread, 0.0f, 5.0f);
        tip("how far the light the sunlit leaves find spreads through a crown (screen space; needs shadow blur)");
        ImGui::SliderFloat("glow spread amount", &L.leafSpreadGain, 0.0f, 2.0f);
        ImGui::SliderFloat("light through a crown (m)", &L.sssDepth, 0.1f, 5.0f);
        tip("how far into foliage the sun still partly gets: a leaf behind other leaves, the ground under a crown");
        ImGui::SliderFloat("wrap", &L.sssWrap, 0.0f, 1.0f);
        ImGui::SliderFloat("sheen", &L.leafSheen, 0.0f, 3.0f);
        bit("far crowns' translucency", kTranslucent, "light through backlit crowns in the far field");
    }
    if (ImGui::CollapsingHeader("Sky light and AO")) {
        bit("sky light + bounce (traced)", kDiffuse, "a cosine-weighted ray from every hit: sky occlusion and one bounce (reference mode)");
        bit("bounce lit by the sun", kBounceSun, "a shadow ray from the bounce's hit (else the bounce is sky-lit only)");
        bit2("AO (traced)", kAO, "one short traced ray per 2 x 2 texels (its own pass, accumulated over frames) for the sky light's occlusion (optimized mode)");
        ImGui::SliderFloat("AO reach (m)", &L.aoReach, 1.0f, 20.0f);
        ImGui::SliderFloat("AO strength", &L.aoStrength, 0.0f, 1.0f);
        ImGui::SliderInt("AO steps", &L.aoSteps, 8, 128);
        ImGui::SliderFloat("AO blur (texels)", &L.aoBlur, 0.0f, 8.0f);
        ImGui::SliderFloat("AO out to (m)", &L.aoRange, 10.0f, 2000.0f, "%.0f");
        ImGui::Checkbox("screen-space AO", &L.ssao);
        tip("the creases under a texel of the traced AO, at full resolution");
        ImGui::SliderFloat("SSAO radius (m)", &L.ssaoRadius, 0.05f, 2.0f);
        ImGui::SliderFloat("SSAO strength", &L.ssaoStrength, 0.0f, 2.0f);
    }
    if (ImGui::CollapsingHeader("Surfaces and normals")) {
        bool all = (L.flags & kSmooth) && (L.flags2 & kNormalBlend) == kNormalBlend;
        if (ImGui::Checkbox("normal blend (all of these)", &all)) {
            L.flags = all ? (L.flags | kSmooth) : (L.flags & ~uint32_t(kSmooth));
            L.flags2 = all ? (L.flags2 | kNormalBlend) : (L.flags2 & ~uint32_t(kNormalBlend));
        }
        ImGui::Indent();
        bit("smooth rock and snow", kSmooth, "normals from the voxel occupancy round the hit, for materials flagged smooth");
        bit2("smooth coarse ground", kSmoothGround, "the steps of a slope shaded smooth, and by the landform's own normal from 1 m voxels");
        bit2("smooth everything else", kSmoothMore, "leaves, wood, shrubs, props: normals from the voxels round them");
        bit2("round outside edges (bevel)", kBevel, "near a voxel's outside edge the shading turns toward the next face, as restir-pt's normal blur");
        ImGui::Unindent();
        ImGui::SliderInt("smoothing radius", &L.smoothRadius, 1, 2);
        ImGui::SliderFloat("ground smoothing", &L.smoothGround, 0.0f, 1.0f);
        ImGui::SliderFloat("everything else: amount", &L.smoothMore, 0.0f, 1.0f);
        ImGui::SliderFloat("bevel width", &L.bevelWidth, 0.02f, 0.5f, "%.2f voxel");
        ImGui::SliderFloat("bevel strength", &L.bevelStrength, 0.0f, 2.0f);
        bit("gloss (water, snow)", kGloss, "water mirrors the sky; snow's sheen");
        bit2("sun glints", kGlints, "snow's crystals and the granite's mica flash the sun: a tiny mirror in each pixel-sized cell, tilted at random");
        ImGui::SliderFloat("glint brightness", &L.glintBright, 0.0f, 100.0f);
        ImGui::SliderFloat("glint sharpness", &L.glintSharp, 0.99f, 0.9999f, "%.4f");
        tip("how exactly a facet must lie between the sun and the eye: higher, fewer and finer");
        bit("props (rocks, shrubs, trees)", kProps, "the loose objects on every ray");
    }
    if (ImGui::CollapsingHeader("Clouds")) {
        bit("clouds", kClouds, "the volumetric cumulus layer");
        bit("cloud detail", kCloudDetail, "the Worley erosion octaves on camera rays");
        bit("cloud shadows", kCloudShadow, "the sun through the clouds, on the ground, the bounce and the haze");
        bit2("box clouds (stylized)", kCloudBlocky, "clouds of boxes: big ones for their bodies, small ones for their detail, rebuilt with the voxels' edges");
        ImGui::SliderFloat("brightness", &L.cloudBright, 0.2f, 1.5f);
        ImGui::SliderFloat("coverage", &L.cloudCoverage, -1.0f, 1.0f);
        ImGui::SliderFloat("base (m)", &L.cloudBase, 2500.0f, 8000.0f);
        ImGui::SliderFloat("top (m)", &L.cloudTop, 3000.0f, 12000.0f);
        ImGui::SliderFloat("extinction (/m)", &L.cloudExtinction, 0.002f, 0.15f, "%.4f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("shape scale (m)", &L.cloudScale, 2000.0f, 30000.0f);
        ImGui::DragFloat("wind x (m)", &L.cloudWindX, 50.0f);
        ImGui::DragFloat("wind z (m)", &L.cloudWindZ, 50.0f);
        ImGui::SliderInt("cloud steps", &L.cloudSteps, 8, 128);
        ImGui::SliderInt("cloud light steps", &L.cloudLightSteps, 1, 6);
    }
    if (ImGui::CollapsingHeader("Air and haze")) {
        bit("aerial perspective", kAerial, "the air between the camera and what it sees");
        ImGui::SliderFloat("air density", &L.air, 0.2f, 3.0f);
        ImGui::SliderFloat("air haze", &L.airHaze, 0.0f, 2.0f);
        bit("valley haze", kHaze, "height fog in the basin");
        ImGui::SliderFloat("haze density (/m)", &L.hazeDensity, 0.0f, 3e-4f, "%.6f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("haze base (m)", &L.hazeBase, 1000.0f, 3500.0f);
        ImGui::SliderFloat("haze scale height (m)", &L.hazeHeight, 50.0f, 3000.0f);
        ImGui::SliderFloat("haze forward scattering", &L.hazeG, 0.0f, 0.95f);
        bit("haze shafts (traced)", kShafts, "one shadowed sample of the haze per pixel per frame");
        bit2("light shafts (screen space)", kGodRays, "rays from the sun past ridges, crowns and clouds");
        ImGui::SliderFloat("shaft strength", &L.rayStrength, 0.0f, 2.0f);
        ImGui::SliderFloat("shaft decay", &L.rayDecay, 0.85f, 1.0f, "%.3f");
        ImGui::SliderFloat("shaft length", &L.rayLength, 0.2f, 1.0f);
        bit2("dust and pollen", kMotes, "motes drifting in the air near you, bright in the sun's direction");
        ImGui::SliderFloat("mote density", &L.moteDensity, 0.0f, 1.0f);
        ImGui::SliderFloat("mote size (m)", &L.moteSize, 0.002f, 0.06f, "%.3f");
        ImGui::SliderFloat("mote brightness", &L.moteBright, 0.0f, 4.0f);
        ImGui::SliderFloat("mote drift (m/s)", &L.moteDrift, 0.0f, 2.0f);
    }
    if (ImGui::CollapsingHeader("Distance (far field)")) {
        bit("far trees as geometry", kFarTrees, "crowns marched on the far heightfield (off: the cover raises it)");
        ImGui::SliderFloat("tree cell (m)", &L.treeCell, 4.0f, 20.0f);
        ImGui::SliderFloat("crown scale", &L.crownScale, 0.3f, 2.5f);
        bit("far ground patches", kFarPatches, "bare earth and grass in patches between the far trees");
        ImGui::SliderFloat("patches", &L.patches, 0.0f, 2.0f);
        bit("far detail normals", kFarDetail, "gully and grain normals below the far field's texels");
        ImGui::SliderFloat("detail normals", &L.farDetail, 0.0f, 2.0f);
        ImGui::SliderFloat("detail octaves", &L.detailOctaves, 0.0f, 1.0f);
        bit("blocky far ground (stylized)", kFarBlocky, "the far field as big flat-topped blocks, larger with distance");
        int idx = L.blockFactor >= 8 ? 3 : (L.blockFactor >= 4 ? 2 : (L.blockFactor >= 2 ? 1 : 0));
        const char* names[] = {"1 texel", "2 texels", "4 texels", "8 texels"};
        if (ImGui::Combo("block", &idx, names, 4)) L.blockFactor = 1 << idx;
    }
    if (ImGui::CollapsingHeader("Display")) {
        ImGui::SliderFloat("glow strength", &L.glow, 0.0f, 1.0f);
        ImGui::SliderFloat("glow threshold", &L.glowThreshold, 0.5f, 10.0f);
        tip("how bright (after exposure) a thing must be to glow");
        ImGui::SliderFloat("glow knee", &L.glowKnee, 0.05f, 4.0f);
        tip("how gradually the glow comes in round the threshold");
        ImGui::SliderFloat("glow spread", &L.glowSpread, 0.5f, 8.0f);
        ImGui::SliderFloat("vignette", &L.vignette, 0.0f, 1.0f);
        ImGui::SliderFloat("contrast", &L.contrast, 0.7f, 1.5f);
        ImGui::SliderFloat("saturation", &L.saturation, 0.0f, 2.0f);
        ImGui::SliderFloat("vibrance", &L.vibrance, 0.0f, 1.5f);
    }
    if (ImGui::CollapsingHeader("Every feature switch")) {
        if (ImGui::Button("all on")) L.flags = kAllFeatures;
        ImGui::SameLine();
        if (ImGui::Button("all off")) L.flags = kSkip;
        for (const FeatureName& f : kFeatureNames) {
            bool on = (L.flags & f.bit) != 0;
            // Its own ID: each also has a switch in its section.
            if (ImGui::Checkbox((std::string(f.name) + "##all").c_str(), &on)) L.flags = on ? (L.flags | f.bit) : (L.flags & ~uint32_t(f.bit));
            tip(f.help);
        }
    }
}
#endif

void startup(projv::Application& app) {
    auto& ri = app.world.ctx().emplace<projv::graphics::RenderInstance>();
    // A capture asks for exactly its size, so a tiling window manager floats it rather than fitting it.
    if (std::getenv("MOUNTAINS_CAPTURE")) ri.resizable = false;
    ri.initialize(1600, 900, "ProjectV Santa Fe (reference)");
    projv::graphics::installPlatform(app, ri);
    auto& scene = app.world.ctx().emplace<projv::Scene>();
    auto& st = app.world.ctx().emplace<State>();
    Settings settings;
    settings.seed = uint32_t(envInt("MOUNTAINS_SEED", 1));
    st.land = std::make_unique<Landform>(settings);
    prepareTrees(st);
    setUpRings(st, scene);
    setUpProps(st, scene);
    placeCamera(st.camera, *st.land);
    st.cameraAbove = std::max(0.0, st.camera.y - st.land->height(st.camera.x, st.camera.z, 8.0));
    // MOUNTAINS_SUN="azimuth,elevation" in degrees (azimuth clockwise from north).
    if (const char* sun = std::getenv("MOUNTAINS_SUN")) std::sscanf(sun, "%f,%f", &st.look.sunAzimuth, &st.look.sunElevation);
    st.pool = std::make_unique<Pool>(*st.land, st.palette, st.props.shapes.empty() ? nullptr : &st.props, std::max(2u, std::thread::hardware_concurrency() - 2), st.clipmap.voxelRings);
    placeRings(st.clipmap, scene, st.pool.get(), st.camera.x, st.camera.z, st.cameraAbove);
    readLookEnvironment();
    projv::core::info("[MOUNTAINS] {} rings of {} cells, {} workers; the outer ring reaches {:.0f} km",
                      st.clipmap.rings, st.clipmap.span(), st.pool->threads(),
                      cellSize(st.clipmap.rings - 1) * double(st.clipmap.radius) / 1000.0);

    // SANTA_FE_MODE=reference for everything at full quality; optimized by default.
    applyMode(st.look, !(std::getenv("SANTA_FE_MODE") && std::string(std::getenv("SANTA_FE_MODE")) == "reference"));
    if (const char* f = std::getenv("SANTA_FE_SCALE")) st.look.renderScale = float(std::atof(f));
    if (std::getenv("SANTA_FE_RECON_DEBUG")) st.look.reconDebug = true;
    if (const char* f2 = std::getenv("SANTA_FE_FLAGS2")) st.look.flags2 = uint32_t(std::strtoul(f2, nullptr, 0));
    if (std::getenv("SANTA_FE_BLOCKY")) { st.look.flags |= kFarBlocky; st.look.flags2 |= kCloudBlocky; }
    if (const char* b = std::getenv("SANTA_FE_CLOUD_BASE")) st.look.cloudBase = float(std::atof(b));
    if (std::getenv("SANTA_FE_DEBUG_CLOUDMAP")) st.look.flags |= (1u << 21);
    if (std::getenv("SANTA_FE_DEBUG_RINGS")) st.look.flags |= (1u << 22);   // each hit tinted by its ring
    if (std::getenv("SANTA_FE_NO_RECON")) st.look.reconstruct = false;
    if (const char* v = std::getenv("SANTA_FE_SHADOW_VOXELS")) st.look.shadowVoxels = float(std::atof(v));
    if (const char* v = std::getenv("SANTA_FE_SHADOW_LOD")) st.look.shadowLODVoxels = float(std::atof(v));
    if (std::getenv("SANTA_FE_NO_SSAO")) st.look.ssao = false;
    if (const char* g = std::getenv("SANTA_FE_GLINT")) std::sscanf(g, "%f,%f", &st.look.glintBright, &st.look.glintSharp);
    if (std::getenv("SANTA_FE_FACE")) st.look.flags = std::atoi(std::getenv("SANTA_FE_FACE")) ? (st.look.flags | kFaceShading) : (st.look.flags & ~uint32_t(kFaceShading));
    if (const char* f = std::getenv("SANTA_FE_FLAGS")) st.look.flags = uint32_t(std::strtoul(f, nullptr, 0));
    if (const char* f = std::getenv("SANTA_FE_OFF")) st.look.flags &= ~uint32_t(std::strtoul(f, nullptr, 0));
    st.look.exposure = kExposure;
    st.captureSamples = std::max(1, envInt("SANTA_FE_SAMPLES", 256));
    if (std::getenv("SANTA_FE_BENCH")) {
        st.bench.push_back({"all on", kAllFeatures});
        st.bench.push_back({"optimized preset", kOptimizedFlags});
        for (const FeatureName& f : kFeatureNames)
            if (kOptimizedFlags & f.bit) st.bench.push_back({std::string("optimized - ") + f.name, kOptimizedFlags & ~uint32_t(f.bit)});
        st.bench.push_back({"nothing (skip only)", kSkip});
        for (const FeatureName& f : kFeatureNames) st.bench.push_back({std::string("- ") + f.name, kAllFeatures & ~uint32_t(f.bit)});
        st.bench.push_back({"all on (again)", kAllFeatures});
    }
    std::filesystem::current_path(projv::core::executableDirectory());   // renderer/ names its shaders from here
    const char* which = std::getenv("MOUNTAINS_RENDERER");
    if (!which || std::string(which) != "basic") {
        st.renderer = buildRenderer(ri);
        st.clipmap.far.upload = true;
        app.world.ctx().emplace<projv::CameraView>();
        app.world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));
        app_gpu = &app.world.ctx().get<projv::GPUData>();
        if (const char* cap = std::getenv("MOUNTAINS_CAPTURE")) st.capturePath = cap;
        else projv::graphics::setCursorCaptured(app, ri, true);
#ifdef PROJV_UI
        projv::ui::dev::addPanel(app, "Santa Fe", lookPanel);
        projv::ui::installUi(app, ri, {.devShown = st.capturePath.empty() ? std::optional<bool>(true) : std::optional<bool>(false)});
#endif
        return;
    }
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


// ---- The character: a box the player's size, on the voxels ----------------------------------------
//
// Solid is ring 0's voxels (the finest ring; it is centred on the camera, so the player is always in it)
// and the near loose objects that are solid to walk into: trees, stones, logs, cactus (grass, chamisa and
// snakeweed are walked through). Where ring 0 has not arrived yet, the landform is the ground.
//
// Collision samples the box's cells at the lattice (0.1 m world), axis by axis, in steps of at most a cell;
// a horizontal move blocked by a ledge up to 0.3 m (the player's) steps up onto it.
struct Character {
    State& st;
    projv::Scene& scene;
    static constexpr double kHalfWidth = 0.3 * kGameScale;
    static constexpr double kStep = 0.3 * kGameScale;
    std::vector<projv::ChunkHandle> nearby;   // solid loose objects near the player
    Character(State& s, projv::Scene& sc) : st(s), scene(sc) {}

    void gather(double x, double y, double z) {
        nearby.clear();
        static std::vector<int8_t> solidBlob;   // by blob: 1 solid, 0 not, -1 unknown
        if (solidBlob.size() < scene.geometryPool.size()) solidBlob.resize(scene.geometryPool.size(), -1);
        for (size_t i = 0; i < st.clipmap.propBlob.size(); ++i) {
            const PropKind k = st.props.shapes[i].kind;
            const bool solid = k == PropKind::Tree || k == PropKind::Boulder || k == PropKind::Cobble || k == PropKind::Log ||
                               k == PropKind::Cholla || k == PropKind::Yucca || k == PropKind::Formation;
            solidBlob[size_t(st.clipmap.propBlob[i])] = solid ? 1 : 0;
        }
        for (projv::ChunkHandle h : scene.looseChunks) {
            const projv::Chunk& c = scene.chunks[h];
            if (!c.alive || c.geometryPoolIndex < 0 || solidBlob[size_t(c.geometryPoolIndex)] != 1) continue;
            const double e = c.header.scale;   // the cube's edge: whatever it holds is within ~1.5 edges
            const double dx = c.header.position.x - x, dy = c.header.position.y - y, dz = c.header.position.z - z;
            if (dx * dx + dy * dy + dz * dz < (e * 1.8 + 6.0) * (e * 1.8 + 6.0)) nearby.push_back(h);
        }
    }

    bool solid(double x, double y, double z) const {
        // Ring 0.
        const Ring& r = st.clipmap.ring[0];
        bool known = false;
        if (r.placed && r.gridIndex >= 0) {
            const projv::SceneGrid& g = scene.grids[size_t(r.gridIndex)];
            const int cx = int(std::floor((x - g.origin.x) / g.cellSize)), cy = int(std::floor((y - g.origin.y) / g.cellSize)),
                      cz = int(std::floor((z - g.origin.z) / g.cellSize));
            if (cx >= 0 && cz >= 0 && cx < g.dims.x && cz < g.dims.z) {
                const Column* col = nullptr;
                auto it = r.columns.find(columnKey(r.ox + cx, r.oz + cz));
                if (it != r.columns.end() && it->second.done) col = &it->second;
                if (col) {
                    known = true;
                    if (cy >= 0 && cy < g.dims.y) {
                        const int32_t h = g.cellToChunk[size_t(cx + g.dims.x * (cy + g.dims.y * cz))];
                        if (h >= 0) {
                            const projv::Chunk& c = scene.chunks[size_t(h)];
                            const double v = c.header.voxelScale;
                            const projv::core::ivec3 q(int(std::floor((x - c.header.position.x) / v)), int(std::floor((y - c.header.position.y) / v)),
                                                       int(std::floor((z - c.header.position.z) / v)));
                            uint8_t slot;
                            if (c.geometryPoolIndex >= 0 && projv::utils::queryVoxelMaterial(scene.geometryPool[size_t(c.geometryPoolIndex)], c.header.resolution, q, slot))
                                return true;
                        }
                    } else if (cy < 0) {
                        return true;   // below everything the ring holds
                    }
                }
            }
        }
        if (!known && y < st.land->height(x, z, 0.25)) return true;
        // The solid loose objects.
        for (projv::ChunkHandle h : nearby) {
            const projv::Chunk& c = scene.chunks[h];
            const projv::core::quat qi = glm::conjugate(c.header.rotation);
            const projv::core::vec3 l = qi * projv::core::vec3(float(x - c.header.position.x), float(y - c.header.position.y), float(z - c.header.position.z));
            const double v = c.header.voxelScale;
            const projv::core::ivec3 q(int(std::floor(l.x / v)), int(std::floor(l.y / v)), int(std::floor(l.z / v)));
            const int res = int(c.header.resolution);
            if (q.x < 0 || q.y < 0 || q.z < 0 || q.x >= res || q.y >= res || q.z >= res) continue;
            uint8_t slot;
            if (projv::utils::queryVoxelMaterial(scene.geometryPool[size_t(c.geometryPoolIndex)], c.header.resolution, q, slot)) return true;
        }
        return false;
    }

    // Whether the player's box with its feet at (x, feet, z) overlaps anything solid.
    bool blocked(double x, double feet, double z) {
        if (nearby.empty()) gather(x, feet, z);
        // The box's surface only: every move is under a cell, so nothing reaches the inside without crossing it.
        const double c = kLattice;
        const int n = int(std::lround(2.0 * kHalfWidth / c)), m = int(std::lround(kPlayerHeight / c));
        for (int k = 0; k < m; ++k) {
            const double y = feet + (k + 0.5) * c;
            const bool cap = k == 0 || k == m - 1;
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    if (!cap && i != 0 && i != n - 1 && j != 0 && j != n - 1) continue;
                    if (solid(x - kHalfWidth + (i + 0.5) * c, y, z - kHalfWidth + (j + 0.5) * c)) return true;
                }
        }
        return false;
    }

    void moveHorizontal(double& x, double& feet, double& z, double dx, double dz, bool grounded) {
        const double total = std::max(std::abs(dx), std::abs(dz));
        const int steps = std::max(1, int(std::ceil(total / (kLattice * 0.9))));
        for (int s = 0; s < steps; ++s) {
            const double sx = dx / steps, sz = dz / steps;
            for (int axis = 0; axis < 2; ++axis) {
                const double nx = axis == 0 ? x + sx : x, nz = axis == 1 ? z + sz : z;
                if ((axis == 0 ? sx : sz) == 0.0) continue;
                if (!blocked(nx, feet, nz)) { x = nx; z = nz; continue; }
                // A ledge: step up onto it, if there is room.
                if (!grounded) continue;
                for (double up = kLattice; up <= kStep + 1e-6; up += kLattice)
                    if (!blocked(nx, feet + up, nz)) { x = nx; z = nz; feet += up; break; }
            }
        }
    }

    // Falls or rises by dy; true when it lands on something. Stops vy against a floor or a ceiling.
    bool moveVertical(double x, double& feet, double z, double dy, double& vy) {
        const int steps = std::max(1, int(std::ceil(std::abs(dy) / (kLattice * 0.9))));
        for (int s = 0; s < steps; ++s) {
            const double ny = feet + dy / steps;
            if (blocked(x, ny, z)) {
                const bool landing = dy < 0.0;
                vy = 0.0;
                return landing;
            }
            feet = ny;
        }
        // Grounded when the cell just under the feet is solid.
        return blocked(x, feet - kLattice * 0.5, z);
    }
};

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
    const float sunStep = 15.0f * app.time().delta;
    if (input.down(projv::Key::LeftBracket)) st.look.sunElevation = std::max(-5.0f, st.look.sunElevation - sunStep);
    if (input.down(projv::Key::RightBracket)) st.look.sunElevation = std::min(89.0f, st.look.sunElevation + sunStep);
    if (input.down(projv::Key::Comma)) st.look.sunAzimuth -= sunStep * 3.0f;
    if (input.down(projv::Key::Period)) st.look.sunAzimuth += sunStep * 3.0f;
    const double dt = std::min(double(app.time().delta), 0.1);
    const double fx = std::cos(cam.yaw), fz = std::sin(cam.yaw);
    double ix = 0, iz = 0;
    if (input.down(projv::Key::W)) { ix += fx; iz += fz; }
    if (input.down(projv::Key::S)) { ix -= fx; iz -= fz; }
    if (input.down(projv::Key::D)) { ix -= fz; iz += fx; }
    if (input.down(projv::Key::A)) { ix += fz; iz -= fx; }
    const double len = std::hypot(ix, iz);
    if (len > 0.0) { ix /= len; iz /= len; }
    if (!cam.walk) {
        double speed = 40.0;
        if (input.down(projv::Key::LeftShift)) speed *= 20.0;
        if (input.down(projv::Key::LeftControl)) speed *= 0.1;
        cam.x += ix * speed * dt;
        cam.z += iz * speed * dt;
        if (input.down(projv::Key::R)) cam.y += speed * dt;
        if (input.down(projv::Key::F)) cam.y -= speed * dt;
        return;
    }
    // The character: walk 4.5, run 8.5 (Shift), creep 1.5 (Ctrl) of the player's m/s; jump (Space) 1.1 m.
    auto& scene = app.world.ctx().get<projv::Scene>();
    double speed = 4.5;
    if (input.down(projv::Key::LeftShift)) speed = 8.5;
    if (input.down(projv::Key::LeftControl)) speed = 1.5;
    speed *= kGameScale;
    const double gravity = 9.81 * 1.6 * kGameScale;   // a little stronger than life: games feel floaty otherwise
    if (cam.grounded && input.pressed(projv::Key::Space)) cam.vy = std::sqrt(2.0 * gravity * 1.1 * kGameScale);
    cam.vy -= gravity * dt;
    Character ch{st, scene};
    double feet = cam.y - kEyeHeight;
    // Unstick: spawned or streamed into something (or under ground before ring 0 arrived), climb out.
    for (int k = 0; k < 60 && ch.blocked(cam.x, feet, cam.z); ++k) feet += kLattice;
    ch.moveHorizontal(cam.x, feet, cam.z, ix * speed * dt, iz * speed * dt, cam.grounded);
    cam.grounded = ch.moveVertical(cam.x, feet, cam.z, cam.vy * dt, cam.vy);
    cam.y = feet + kEyeHeight;
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
    {
        const double dt = std::max(double(app.time().delta), 1e-3);
        const double moved = std::hypot(st.camera.x - st.lastCamX, st.camera.z - st.lastCamZ) / dt;
        st.lastCamX = st.camera.x;
        st.lastCamZ = st.camera.z;
        // Fast to rise (drop the fine rings at once), slow to fall (refill them only once settled).
        st.camSpeed = moved > st.camSpeed ? moved : mix(st.camSpeed, moved, std::min(1.0, dt * 1.5));
        if (moved > 5000.0) st.camSpeed = 0.0;   // a teleport (placed, or the first frame), not a flight
    }
    placeRings(st.clipmap, scene, st.pool.get(), st.camera.x, st.camera.z, st.cameraAbove, st.camSpeed);
    if (installResults(st.clipmap, scene, *st.pool, 10.0) > 0) st.sceneChanged = true;
    bool changed = false;
    for (int k = 0; k < st.clipmap.rings; ++k)
        if (st.clipmap.ring[size_t(k)].dirty) {
            rebuildGrid(st.clipmap, scene, k);
            changed = true;
        }
    const double* at = st.clipmap.culledAt;
    const double moved = std::hypot(st.camera.x - at[0], st.camera.y - at[1], st.camera.z - at[2]);
    if (changed || moved > kPropRecull) rebuildProps(st.clipmap, scene, st.camera.x, st.camera.y, st.camera.z);

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
                if (st.renderer) {
                    bgfx::setDebug(BGFX_DEBUG_PROFILER);
                    st.profiling = true;
                }
            }
        } else if (st.captureStage == 1) {
            ++st.settledFrames;
            const double s = std::chrono::duration<double>(Clock::now() - st.settledAt).count();
            // The reference renderer converges: wait for its samples (a scene change starts them over).
            const bool done = st.renderer ? st.accumulated >= st.captureSamples : s >= 3.0;
            if (done) {
                projv::core::info("[SANTA FE] {} samples", st.accumulated);
                const auto& ri = app.world.ctx().get<projv::graphics::RenderInstance>();
                (void)ri;
                projv::core::info("[MOUNTAINS] settled: {:.2f} ms/frame over {} frames", s * 1000.0 / st.settledFrames,
                                  st.settledFrames);
                st.profiling = false;
                reportPasses(st, app.world.ctx().get<projv::graphics::RenderInstance>());
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
    if (const char* b = std::getenv("SANTA_FE_BLOCK")) gFarSamples.store(std::max(1, kChunkRes / std::max(1, std::atoi(b))));
    projv::Scene scene;
    State st;
    Settings settings;
    settings.seed = uint32_t(envInt("MOUNTAINS_SEED", 1));
    st.land = std::make_unique<Landform>(settings);
    prepareTrees(st);
    setUpRings(st, scene);
    setUpProps(st, scene);
    placeCamera(st.camera, *st.land);
    st.cameraAbove = std::max(0.0, st.camera.y - st.land->height(st.camera.x, st.camera.z, 8.0));
    const unsigned threads = std::max(2u, std::thread::hardware_concurrency());
    st.pool = std::make_unique<Pool>(*st.land, st.palette, st.props.shapes.empty() ? nullptr : &st.props, threads, st.clipmap.voxelRings);
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
    if (std::getenv("MOUNTAINS_FIND_ROCK")) {
        // The rockiest ground near the range front, and a camera 60 m west of it looking east.
        double best = 0, bx = 0, bz = 0;
        for (double z = -6000; z <= 16000; z += 100)
            for (double x = 3000; x <= 16000; x += 100) {
                Sample sm;
                std::vector<Sample> one;
                st.land->sampleGrid(x, z, 1, 1, 1, 8.0, one);
                const double r = rockWeight(one[0]);
                if (r > best + 1e-3) { best = r; bx = x; bz = z; }
            }
        std::printf("rockiest: %.0f at %.0f, %.0f -> MOUNTAINS_CAMERA=%.0f,%.0f,25,0,-0.15\n", best, bx, bz, bx - 70.0, bz);
    }
    if (std::getenv("MOUNTAINS_DEBUG_PROPS")) {
        int shown = 0;
        for (auto& [key, col] : st.clipmap.ring[0].columns)
            for (projv::ChunkHandle h : col.props) {
                const projv::Chunk& c = scene.chunks[h];
                const double dx = c.header.position.x - st.camera.x, dz = c.header.position.z - st.camera.z;
                if (dx * dx + dz * dz > 15.0 * 15.0 || shown > 30) continue;
                ++shown;
                int si = -1;
                for (size_t i = 0; i < st.clipmap.propBlob.size(); ++i) if (st.clipmap.propBlob[i] == c.geometryPoolIndex) si = int(i);
                std::printf("prop kind %d at %.2f %.2f %.2f scale %.3f res %u ground %.2f alive %d\n", si >= 0 ? int(st.props.shapes[size_t(si)].kind) : -1,
                            c.header.position.x, c.header.position.y, c.header.position.z, c.header.scale, c.header.resolution,
                            st.land->height(c.header.position.x, c.header.position.z, 0.25), int(c.alive));
            }
    }
    if (!st.props.shapes.empty()) {
        static const char* kinds[] = {"boulder", "cobble", "chamisa", "snakeweed", "cholla", "yucca", "log", "grass", "tree", "formation"};
        std::printf("props by ring (shown):");
        for (const char* k : kinds) std::printf(" %9s", k);
        std::printf("\n");
        for (const Ring& r : st.clipmap.ring) {
            if (r.k > 2) break;
            int count[kPropKinds] = {};
            for (auto& [key, col] : r.columns) {
                if (st.clipmap.covered(r.k, col.ix, col.iz)) continue;
                for (projv::ChunkHandle h : col.props) {
                    const int32_t blob = scene.chunks[h].geometryPoolIndex;
                    for (size_t i = 0; i < st.clipmap.propBlob.size(); ++i)
                        if (st.clipmap.propBlob[i] == blob) { ++count[int(st.props.shapes[i].kind)]; break; }
                }
            }
            std::printf("  ring %d             ", r.k);
            for (int k = 0; k < kPropKinds; ++k) std::printf(" %9d", count[k]);
            std::printf("\n");
        }
    }
    st.pool.reset();
    return 0;
}

// ---- Headless: check the clipmap ------------------------------------------------------------------
//
// Generates every ring around the Plaza, then flies 5 km east in 250 m steps and back, letting
// everything generate at each. Checks, every step: every column the rings hold produced ground (no holes); every
// grid cell names a live chunk of its own ring; and the scene's rows and blobs stay bounded (columns
// dropped behind are freed and their rows reused, not leaked). Exit code 0 on success.
int selftest() {
    projv::Scene scene;
    State st;
    Settings settings;
    settings.seed = uint32_t(envInt("MOUNTAINS_SEED", 1));
    st.land = std::make_unique<Landform>(settings);
    prepareTrees(st);
    setUpRings(st, scene);
    setUpProps(st, scene);
    st.camera.x = 0.0;
    st.camera.z = 0.0;
    st.camera.y = st.land->height(0.0, 0.0, 0.25) + 1.7;
    st.cameraAbove = 1.7;
    st.pool = std::make_unique<Pool>(*st.land, st.palette, st.props.shapes.empty() ? nullptr : &st.props, std::max(2u, std::thread::hardware_concurrency()), st.clipmap.voxelRings);
    int failures = 0;
    auto fail = [&](const std::string& what) {
        if (failures < 20) std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    };
    size_t firstRows = 0, firstBlobs = 0, peakChunks = 0;
    const auto t0 = Clock::now();
    // Out 5 km and back: back at the start, the same columns are held, so the same chunks and blobs must
    // be live as at the first step -- more would be a leak.
    for (int step = 0; step <= 21; ++step) {
        st.camera.x = step == 21 ? 0.0 : 250.0 * step;
        st.camera.y = st.land->height(st.camera.x, 0.0, 0.25) + 1.7;
        placeRings(st.clipmap, scene, st.pool.get(), st.camera.x, st.camera.z, st.cameraAbove);
        for (;;) {
            installResults(st.clipmap, scene, *st.pool, 50.0);
            int pending = 0;
            for (const Ring& r : st.clipmap.ring) pending += r.pending;
            if (pending == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (int k = 0; k < st.clipmap.rings; ++k) rebuildGrid(st.clipmap, scene, k);
        rebuildProps(st.clipmap, scene, st.camera.x, st.camera.y, st.camera.z);
        for (projv::ChunkHandle h : scene.looseChunks)
            if (h >= scene.chunks.size() || !scene.chunks[h].alive || scene.chunks[h].geometryPoolIndex < 0)
                fail("a prop on the loose list is not a live chunk with a shape");
        size_t liveChunks = 0, liveBlobs = 0;
        for (const projv::Chunk& c : scene.chunks) liveChunks += c.alive ? 1 : 0;
        for (const projv::GeometryBlob& b : scene.geometryPool) liveBlobs += b.refCount > 0 ? 1 : 0;
        for (const Ring& r : st.clipmap.ring) {
            if (int64_t(r.columns.size()) != st.clipmap.span() * st.clipmap.span())
                fail("ring " + std::to_string(r.k) + " holds " + std::to_string(r.columns.size()) + " columns");
            for (const auto& [key, col] : r.columns) {
                if (!col.done) fail("ring " + std::to_string(r.k) + ": a column never finished");
                else if (col.chunks.empty() && !st.clipmap.isFar(r.k))
                    fail("ring " + std::to_string(r.k) + ": column " + std::to_string(col.ix) + "," + std::to_string(col.iz) + " has no ground");
                for (const auto& [iy, h] : col.chunks)
                    if (h >= scene.chunks.size() || !scene.chunks[h].alive) fail("ring " + std::to_string(r.k) + ": a dead chunk");
            }
            if (r.gridIndex < 0) continue;
            const projv::SceneGrid& g = scene.grids[size_t(r.gridIndex)];
            for (int32_t h : g.cellToChunk)
                if (h >= 0 && (size_t(h) >= scene.chunks.size() || !scene.chunks[size_t(h)].alive ||
                               scene.chunks[size_t(h)].gridIndex != r.gridIndex))
                    fail("ring " + std::to_string(r.k) + ": a grid cell names a chunk that is not its own");
        }
        if (step == 0) {
            firstRows = liveChunks;
            firstBlobs = liveBlobs;
        }
        if (step == 21) {
            if (liveChunks != firstRows) fail("back at the start, " + std::to_string(liveChunks) + " chunks live, not " + std::to_string(firstRows));
            if (liveBlobs != firstBlobs) fail("back at the start, " + std::to_string(liveBlobs) + " blobs live, not " + std::to_string(firstBlobs));
        }
        peakChunks = std::max(peakChunks, liveChunks);   // props vary with the ground: rows track the peak
        if (scene.chunks.size() > peakChunks + peakChunks / 4 + 64) fail("chunk rows " + std::to_string(scene.chunks.size()) + " for " + std::to_string(liveChunks) + " live: not reused");
        std::printf("step %2d  x %5.0f m  %zu chunks live, %zu rows, %zu blobs\n", step, st.camera.x, liveChunks,
                    scene.chunks.size(), liveBlobs);
    }
    st.pool.reset();
    std::printf("%s: %d failure(s), %.1f s\n", failures ? "FAILED" : "passed", failures,
                std::chrono::duration<double>(Clock::now() - t0).count());
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--bench") return bench();
        if (std::string(argv[i]) == "--selftest") return selftest();
    }
    projv::Application app;
    app.addSystem(projv::Stage::Startup, "startup", startup);
    app.addSystem(projv::Stage::Update, "move camera", moveCamera);
    app.addSystem(projv::Stage::Update, "stream", stream, {.after = "move camera"});
    app.addSystem(projv::Stage::Render, "render", render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    return 0;
}
