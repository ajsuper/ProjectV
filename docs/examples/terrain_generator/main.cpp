// ProjectV Terrain Generator (grid-based)
// Generates a Perlin-noise heightfield terrain as voxel chunks in a SceneGrid,
// streamed around the camera. The grid allows the shader's marchGrid DDA path
// to handle broadphase instead of brute-forcing every loose chunk per pixel.
//
// Controls: WASD/R/F + mouse to fly (mode 1). Press 2 for player mode with gravity/collision.
//   Key 1 — fly camera (default)
//   Key 2 — player camera with gravity and terrain collision
//   In player mode: WASD moves on XZ plane, Space jumps, gravity pulls you to the ground.
//   Key E — place a 256-voxel-wide sphere of material 500 voxels ahead of the camera
//   Key Q — carve the same sphere out of the terrain

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/ecs.h"
#include "core/math.h"
#include "core/log.h"
#include "graphics/render_instance.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/type_mapping.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/voxel_management.h"
#include "utils/material.h"

#include "terrain_noise.hpp"
#include "rock_detail.hpp"
#include "tree_placement.hpp"

#include <dlfcn.h>
#include "renderdoc_app.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace fs = std::filesystem;

static inline float lerpf(float a, float b, float t) { return a + t * (b - a); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Cheap integer hash -> [0,1), used for the per-voxel color speckle/dither in surfaceColor(). Not
// used for shape/height (that's all terrain_noise's Perlin/Worley) -- purely a rendering-detail
// trick to break up the flat quantized-lattice look with stable, seed-independent per-voxel noise.
static inline float hash2f(int x, int z) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0xFFFFFFu) * (1.0f / float(0x1000000));
}

struct ivec3_hash { size_t operator()(const projv::core::ivec3& v) const { return size_t(v.x) * 73856093 ^ size_t(v.y) * 19349663 ^ size_t(v.z) * 83492791; } };

struct FrameContext {
    projv::core::vec3 cameraPosition, cameraDirection;
    projv::core::vec3 prevCameraPosition, prevCameraDirection;
    projv::core::vec3 sunDirection;
    int frameCount;
    bool cameraMoved;
    int frameCameraLastMovedOn;
    projv::core::vec2 windowResolution;
};

// Sun elevation angle (radians), driven by the mouse scroll wheel: scrolling up raises the
// sun, scrolling down lowers it (through sunset and on below the horizon into night). Plain global (not ECS state)
// because it's written from a GLFW callback on the same thread the render loop reads it from.
static float g_sunElevation = 0.26f;

void sunScrollCallback(GLFWwindow* /*window*/, double /*xoffset*/, double yoffset) {
    const float sensitivity = 0.03f;
    g_sunElevation += float(yoffset) * sensitivity;
}

struct CameraState {
    projv::core::vec3 position       = projv::core::vec3(0, 1000, 0);
    projv::core::vec3 prevPosition   = projv::core::vec3(0, 1000, 0);
    projv::core::vec3 prevDirection  = projv::core::vec3(0, 1, 0);
    float pitch = -0.3f;
    float phi   = 1.57f;
    bool  prevInitialized = false;
    bool  playerMode = false;
    float playerVelocityY = 0.0f;
};

static void uploadCommonUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    projv::core::vec3 cp = ctx.cameraPosition, cd = ctx.cameraDirection;
    projv::core::vec3 pp = ctx.prevCameraPosition, pd = ctx.prevCameraDirection;
    projv::core::vec2 wr = ctx.windowResolution;
    projv::core::vec2 texelSize = { 1.0f / wr.x, 1.0f / wr.y };
    projv::core::vec4 frameCount = { (float)ctx.frameCount, ctx.cameraMoved ? 1.0f : 0.0f, (float)ctx.frameCameraLastMovedOn, 0.0f };
    projv::core::vec4 sunDir = { ctx.sunDirection.x, ctx.sunDirection.y, ctx.sunDirection.z, 0.0f };

    projv::graphics::setUniformToValue(renderer, "cameraPos",  cp);
    projv::graphics::setUniformToValue(renderer, "cameraDir",  cd);
    projv::graphics::setUniformToValue(renderer, "windowRes",  wr);
    projv::graphics::setUniformToValue(renderer, "prevCameraPos", pp);
    projv::graphics::setUniformToValue(renderer, "prevCameraDir", pd);
    projv::graphics::setUniformToValue(renderer, "frameCount", frameCount);
    projv::graphics::setUniformToValue(renderer, "texelSize",  texelSize);
    projv::graphics::setUniformToValue(renderer, "sunDir",     sunDir);
}

// =============================================================================
// Terrain state & chunk streaming
// =============================================================================
// Sentinel ChunkHandle meaning "not a refine" / "no chunk" -- reused for both ChunkWorkItem's
// refineHandle and the activeChunks "known empty" marker.
static constexpr projv::ChunkHandle kInvalidChunkHandle = static_cast<projv::ChunkHandle>(-1);

struct ChunkWorkItem {
    projv::core::ivec3 coord;
    projv::core::vec3 worldPos;
    // Storage LOD ring this chunk should be generated at (0=256^3, 1=64^3, 2=16^3), decided at
    // enqueue time. See TerrainState::resolutionForLOD/voxelScaleForLOD.
    uint32_t lod = 0;
    // != kInvalidChunkHandle for a refine request: this coord already has a live chunk (the camera
    // moved close enough to want finer detail than what's resident), and the result should be
    // installed in place via replaceChunkGeometry -- not admitted as if brand new. Refines jump the
    // work queue and get their own completion path (see terrainWorkerFunc/generatePendingChunks) so
    // a chunk doesn't have to wait behind the ordinary streaming backlog, which can be tens of
    // thousands of chunks deep during a big flight. The chunk is never evicted for this, so it
    // stays visible at its current resolution right up until the new data replaces it.
    projv::ChunkHandle refineHandle = kInvalidChunkHandle;
    // Skip the readyChunks backpressure cap and get picked up ahead of ordinary streaming. Set for
    // both kinds of camera-driven catch-up work (refines and empty re-checks): both are bounded at
    // a few per frame at request time, so they cannot flood the result backlog the cap protects,
    // and both describe something wrong on screen right now rather than terrain the camera has not
    // reached yet. Without it, one of these sitting at the head of the queue while readyChunks is
    // full stalls the worker instead of jumping the line -- blocking the very work behind it.
    bool jumpQueue = false;
    // Sphere-edit work: goes into WorkerState::editQueue rather than workQueue, which is drained
    // ahead of everything else by every worker and has its own dedicated threads waiting on it.
    // An edit is the one kind of work with a human waiting on it in real time.
    bool isEdit = false;
    // When the edit that caused this was placed. Carried through onto the result purely so the
    // install can report end-to-end latency; zero for non-edit work.
    std::chrono::steady_clock::time_point requestedAt{};
    // Which sphere placement this chunk belongs to. Every chunk of one sphere carries the same ID
    // and none of them is installed until all of them have arrived, so the sphere appears whole
    // rather than filling in a chunk at a time. 0 for non-edit work.
    uint64_t editBatchID = 0;
};

struct ProcessedChunk {
    projv::core::ivec3 coord;
    projv::Chunk chunk;
    std::vector<uint8_t> materialIDs;   // already in final global-palette slots (interned during
                                        // generation via the thread-safe internMaterial)
    bool empty = false;  // true if the batch was empty (no geometry to intern)
    // Storage LOD this result was generated at. An `empty` result is only authoritative for THIS
    // LOD -- see TerrainState::emptyChunks.
    uint32_t lod = 0;
    // True if the terrain surface passed within a coarse voxel of this chunk's vertical slab. Only
    // consulted when `empty`: it separates "empty because the surface grazed the slab and this
    // ring's sparse sampling missed it" (worth re-checking at a finer ring) from "empty because
    // this is open sky or deep bedrock" (empty at every ring, never worth re-checking).
    bool marginal = false;
    // Solid featureless underground -- see TerrainState::buriedCoords.
    bool buried = false;
    projv::ChunkHandle refineHandle = kInvalidChunkHandle;
    // Version of this coord's sphere-edit bucket that was replayed into the result (see
    // applySphereEdits). Compared against the bucket's current version when the result is consumed;
    // a mismatch means an edit landed mid-generation and this result predates it.
    uint32_t editVersion = 0;
    // Mirrors ChunkWorkItem::isEdit, plus the timestamps needed to break the observed latency down
    // into queue wait / generation / install wait. Only meaningful when isEdit.
    bool isEdit = false;
    std::chrono::steady_clock::time_point requestedAt{}, startedAt{}, finishedAt{};
    uint64_t editBatchID = 0;
};

struct WorkerState {
    std::mutex workMutex;
    std::condition_variable workCv;
    // FIFO: consumed front-to-back so nearest-first ordering (the pending sweep walks coords
    // nearest-first before pushing them here) is preserved. A LIFO pop would generate the
    // farthest-just-pushed chunks first and starve the near ones the camera actually needs.
    // Refines are pushed to the front (see applyLODRing) so they're picked up next regardless of
    // how deep the ordinary streaming backlog is.
    //
    // Kept SHALLOW on purpose -- see kMaxWorkQueueDepth. This used to be allowed to grow to
    // whatever the pending sweep could feed it (measured at 534,000 items, ~20MB) which was wrong
    // twice over: the whole depth was re-scanned every frame to build a dedup set, and the items
    // were addressed to a camera position minutes in the past, so a queue that deep meant the
    // workers spent minutes generating terrain for somewhere the camera had already left.
    std::deque<ChunkWorkItem> workQueue;
    // Sphere-edit work, drained ahead of workQueue by every worker and exempt from the readyChunks
    // backpressure cap. Guarded by workMutex, same as workQueue.
    //
    // A separate queue rather than a priority flag on workQueue because the two need different
    // *pickup* behaviour, not just different ordering. push_front already put edits at the head of
    // the line, and it was not enough: the pool is saturated with 20-80ms LOD0 chunks, so being next
    // in line still means waiting for whichever worker happens to finish first, and an 8-chunk batch
    // is admitted at the rate workers free up rather than in parallel. Measured that way, the eight
    // chunks of one sphere landed 100ms, 230ms and 338ms after the key press. kEditWorkers threads
    // below wait on this queue and nothing else, so a batch starts generating immediately and in
    // parallel however busy the ordinary pool is.
    std::deque<ChunkWorkItem> editQueue;
    // Woken only for editQueue, so the dedicated edit threads don't wake on ordinary streaming.
    std::condition_variable editCv;
    // Coords that are either sitting in workQueue or already generated but not yet consumed out of
    // readyChunks/readyRefines -- i.e. every coord the pending sweep must NOT queue a second time.
    //
    // Maintained incrementally (insert at push, erase when the result is consumed) rather than
    // rebuilt per frame. The rebuild it replaces walked the entire workQueue and readyChunks into a
    // fresh unordered_set EVERY FRAME while holding workMutex, which is both O(queue depth) of
    // hashing on the main thread and -- because the lock is held for all of it -- a hard stall for
    // all N worker threads at the same time. That is what pinned one core at 100% while the other
    // thirty sat idle.
    //
    // Guarded by workMutex. The erase side therefore cannot run inside a resultMutex critical
    // section: the established lock order is workMutex -> resultMutex (see terrainWorkerFunc), so
    // generatePendingChunks collects consumed coords while draining results and erases them after
    // dropping resultMutex.
    std::unordered_set<projv::core::ivec3, ivec3_hash> queuedCoords;
    std::mutex resultMutex;
    std::vector<ProcessedChunk> readyChunks;
    // Refine results, kept separate from readyChunks so they can be admitted every frame
    // regardless of kMaxNewPerFrame -- refines are rare (capped by kMaxRefinesPerFrame at the
    // point they're requested) and time-sensitive (the chunk is already popped out of the scene),
    // so they shouldn't have to wait behind whatever ordinary streaming happened to finish first.
    std::vector<ProcessedChunk> readyRefines;
    // Edit results that have to be admitted as brand-new chunks (a sphere placed where nothing was
    // resident). Kept out of readyChunks so they skip its kMaxNewPerFrame/kMaxConsumedPerFrame
    // budgets: a result sitting behind a 512-deep ready backlog drained 24 a frame waits up to
    // twenty frames, which is most of a third of a second added on the frame side of an edit that
    // had already finished generating. Edits that land on a resident chunk need no equivalent --
    // readyRefines is already drained uncapped.
    std::vector<ProcessedChunk> readyEdits;
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;
    // Threads that serve editQueue and nothing else. Idle (blocked on editCv) whenever no edit is in
    // flight, which is nearly always, so the only standing cost is their stacks. Sized to cover a
    // whole sphere in one wave: a sphere kEditSphereRadiusVoxels across spans at most 2 chunks per
    // axis, so a batch is at most 8 chunks and none of them ever queues behind another.
    std::vector<std::thread> editThreads;
    static constexpr int kEditWorkers = 8;
    int numThreads = 1;
    // Chunks finished by the pool, split by the ring they were generated at. Generation cost per
    // chunk differs by orders of magnitude across rings (a LOD0 chunk runs a 256x256 column
    // prepass; a LOD2 chunk runs 16x16), so a single throughput number says nothing on its own --
    // "the pool got slower" and "the pool is working on more expensive chunks" look identical
    // until these are separated.
    std::atomic<uint64_t> completed[3] = {};
};

static WorkerState g_worker;

struct TerrainState {
    // Native generation resolution per storage LOD ring. Chunks are generated directly at the
    // resolution their assigned ring needs (see resolutionForLOD/voxelScaleForLOD below) instead
    // of always at full resolution -- generating a far LOD2 chunk at 256^3 just to immediately
    // truncate 99.97% of it at GPU-upload time is what previously drove unbounded CPU RAM growth.
    static constexpr int kChunkRes = 256;
    static_assert(kChunkRes / 4 == 64 && 64 / 4 == 16,
                  "LOD rings must be consecutive powers of 4 -- the renderLOD drop-count "
                  "arithmetic in generatePendingChunks/applyLODRing assumes exactly one "
                  "downsampleTree64 level (4x) per ring step");
    static constexpr int resolutionForLOD(uint32_t lod) {
        return lod == 0 ? kChunkRes : (lod == 1 ? kChunkRes / 4 : kChunkRes / 16);
    }
    // World size of one voxel edge at LOD0, and so (times kChunkRes) the world footprint of a
    // chunk.
    //
    // Up from 1.75, which is the cheapest possible way to see more world: every chunk covers more
    // ground for exactly the same voxel count, the vertical stack gets shorter rather than taller
    // (13 levels instead of 21, since chunks are proportionally taller too), and the view radius in
    // world units grows without a single extra chunk. The world is built at a scale -- 174k-unit
    // continents, 105k-unit mountain belts -- that the old 22k-unit view radius could not show; you
    // were always inside one landform, never looking at one.
    //
    // What it costs is near-field detail, which is why this is 2.8 rather than the 3.5 first tried:
    // 3.5 put rock bedding at only 4 voxels per bed and was visibly coarse up close. At 2.8 a bed
    // is about 8 voxels again, matching what 1.75 gave. Trees keep their WORLD size regardless --
    // treeVoxelWorldSize is pinned at 1.75 rather than derived from this, or they would have grown
    // along with the grid.
    static constexpr float kVoxelScale = 2.8f;
    static constexpr float kChunkSize  = kChunkRes * kVoxelScale;
    // Voxel edge length for a chunk generated at the given ring's native resolution, keeping the
    // world-space chunk footprint (kChunkSize) identical across all three rings.
    static constexpr float voxelScaleForLOD(uint32_t lod) {
        return kChunkSize / float(resolutionForLOD(lod));
    }

    // Vertical extent of chunk generation, in chunks. Derived from kChunkSize rather than hardcoded
    // so changing kVoxelScale does not leave us generating (and discarding) columns of empty sky:
    // taller chunks need proportionally fewer levels to cover the same world height.
    //
    // The ceiling is terrain_noise's, which sampleTerrain also clamps to, so the two cannot drift
    // apart -- a peak taller than the topmost chunk level does not get a flat top, it gets no
    // chunks and therefore a hole. It went up by a factor of nearly three when mountains gained
    // uplift belts, which is most of why the column prepass is now cached across a chunk stack
    // (see columnsForChunk): a vertical stack is seventeen levels deep now rather than eight, and
    // without the cache every one of them re-ran the full terrain prepass for the same columns.
    static constexpr float kTerrainCeiling = terrain_noise::TERRAIN_CEILING;
    // Deep enough for the abyssal plain (about 550 units below sea level), which the continent
    // field now actually reaches -- it previously bottomed out just under the waterline. One level
    // suffices now that a chunk is 896 units tall.
    static constexpr int   kChunkYMin = -1;
    static constexpr int   kChunkYMax = int((kTerrainCeiling + kChunkSize - 1.0f) / kChunkSize);

    // Radius of the outermost ring, in chunks. 110 x 717 puts the horizon 79,000 world units out,
    // against 22,400 originally -- around half a continent wavelength, so a mountain belt is now
    // something you look AT rather than something you are inside.
    //
    // Cost is chunk COUNT, which is what this was measured against rather than guessed: sweeping
    // the occupancy rule (terrain shell or water shell intersecting a slab) over the noise field
    // gives about 124k resident chunks. That is affordable because the outer ring is stored
    // sparsely -- a 16^3 heightfield shell is a few kilobytes of tree64, and the per-chunk fixed
    // overhead (header texel, grid cell, map entry) is under 200 bytes, so the header texture's
    // ~53M slot ceiling is nowhere near binding.
    //
    // Shrinking kVoxelScale costs chunks quadratically at fixed view distance -- going 3.5 -> 2.8
    // alone would have needed radius 120 and 148k chunks to hold 86k units. This trades a tenth of
    // the view distance to keep that closer to 124k. It is the single knob to turn down if the
    // outer ring proves too heavy; nothing else depends on it.
    static constexpr int   kViewRadius = 150;
    // Storage LOD rings. Three levels:
    //   LOD 0 (256^3) — chunks within kLodRadiusFine chunks of camera
    //   LOD 1 (64^3)  — chunks within kLodRadius chunks of camera
    //   LOD 2 (16^3)  — everything inside the view radius
    //
    // Both radii grew, but by less than the chunk size did, so in WORLD units the fine ring roughly
    // doubles (2,688 -> 3,584) and the middle ring more than triples (5,376 -> 17,920). The middle
    // ring is the one that had to move: LOD2 voxels are 56 world units now rather than 28, and
    // pushing the coarsest representation to the horizon without widening the band above it would
    // have traded the extra view distance straight back for blockiness at mid range.
    static constexpr int   kLodRadiusFine = 5;
    static constexpr int   kLodRadius     = 22;
    // Cap on refines (regenerate-at-finer-resolution) triggered per frame. Unlike an ordinary LOD
    // change (a cheap flag flip -- setBlobRenderLOD), a refine re-runs the full worker generation
    // pipeline at higher resolution, so it gets a budget while ordinary transitions do not.
    static constexpr int   kMaxRefinesPerFrame = 3;

    // Fill every column from its surface down to the floor of the world instead of stopping
    // kFillDepth voxels under it, so the world is solid rock you can dig into rather than a shell
    // over emptiness. Flip to false to get the old hollow world back.
    static constexpr bool kSolidFillToWorldBottom = true;
    // Coarsest ring a fully-buried chunk is allowed to sit at, and the ring it is promoted to once
    // an edit cuts into it.
    //
    // Buried chunks are uniform stone, so at LOD2 (16^3, 44.8 world units a voxel) they cost about
    // 900 bytes each and look identical to a finer version -- right up until you dig, at which point
    // the hole would be made of 45-unit blocks.
    //
    // So an edited one goes to full resolution. That is affordable for exactly the reason the shell
    // exists: a carved 256^3 chunk does not store its solid interior, it stores the wall of the
    // carve (see applySphereEdits), which is a surface and costs ~100k voxels rather than the 16.7M
    // a real solid fill would. The world reads as full-resolution stone everywhere you cut into it
    // while only the cut is paid for.
    static constexpr uint32_t kBuriedLOD       = 2;
    static constexpr uint32_t kBuriedEditedLOD = 0;
    // Chunks are evicted past kViewRadius + this. The gap keeps a camera sitting on a chunk
    // boundary from evicting and regenerating the same ring every time it steps back and forth.
    static constexpr int   kEvictHysteresis = 2;
    // Was 2, tuned back when every chunk (near or far) cost a full 256^3 admission. Now that
    // generation and admission are LOD-appropriately cheap for the vast majority of chunks (only
    // the innermost ring is full-res), a much higher per-frame admission rate is affordable and
    // meaningfully cuts how long newly-visible/refined chunks stay popped-out during fast travel.
    //
    // This is now a SAFETY CEILING rather than the operating point: the drain loop stops on
    // kAdmitBudgetMs of main-thread time (see generatePendingChunks), so the machine sets the rate
    // and this only bounds the worst case. It was the binding constraint on how long a view takes
    // to fill, and by a wide margin -- the arithmetic that motivated the change:
    //
    //   A radius-110 view is ~124k resident chunks. At 24/frame that is 5,166 frames, i.e. 86
    //   seconds at a sustained 60fps and worse at any lower frame rate. The worker pool that
    //   PRODUCES those chunks finishes the whole view in about 4.4 seconds on a 30-worker machine
    //   (measured: 7.7/5.6/4.9 us per terrain sample at LOD0/1/2, ~133s of single-threaded column
    //   prepass). So admission was running 19x slower than generation, and every second of the wait
    //   was this constant rather than the terrain noise. Doubling kViewRadius makes it 33x.
    //
    // The old note here read "the limit is GPU upload bandwidth per frame rather than anything
    // CPU-side". That was an assumption, never a measurement. Measured ([ADMIT], 235k chunks over
    // 21.6k frames), the main-thread cost of one more admitted chunk is:
    //
    //   admit() bookkeeping   0.0052 ms/chunk
    //   flushSceneUpdates     0.0083 ms/chunk  + 2.66 ms/frame FIXED
    //   marginal              0.0135 ms/chunk
    //
    // Which is to say a chunk costs almost nothing to admit, and essentially the entire per-frame
    // cost of streaming is a FIXED 2.66ms flush that does not care whether one chunk was admitted or
    // two hundred. That inverts the old reasoning completely: the way to stream faster is to put more
    // chunks through each flush, not fewer. 128 leaves the budget (kAdmitBudgetMs, ~148 chunks at the
    // measured rate) as the real governor while capping a pathological frame at about 4.4ms.
    //
    // The 2.66ms fixed flush is now the largest single per-frame cost here and is worth attacking on
    // its own -- it is paid on every streaming frame regardless of throughput.
    //
    // Note this is NOT currently the binding constraint: [ADMIT] reports "stopped on cap 0%" over
    // 21.6k frames, because generation cannot keep up -- 65% of the worker pool's time is blocked in
    // columnsForChunk waiting on other workers building the same column (see [COLCACHE], 79.6%
    // wait). Raising this only pays off once that convoy is fixed; it is raised now so it does not
    // become the next ceiling the moment it is.
    static constexpr int   kMaxNewPerFrame = 128;
    // Main-thread time the admission drain may spend per frame. This, not kMaxNewPerFrame, is the
    // real operating limit: it is what the cap was always a proxy for, and expressing it directly
    // means the rate self-tunes to the machine instead of being a constant that has to be re-guessed
    // every time chunk costs or the view radius change.
    //
    // Charges the MEASURED marginal cost of an admitted chunk, not just the admit() call: the flush
    // is a single batched call after the drain, so its per-chunk share cannot be observed from inside
    // the loop and has to be carried as a constant. [ADMIT] fits that constant against real frames
    // (least squares, flush ms vs chunks admitted) so it can be checked rather than trusted -- if the
    // reported ms/chunk drifts from kMarginalAdmitMs, update it here.
    //
    // Budgeting on admit() time alone would have been useless: at 0.0041ms a chunk, 2ms of admit()
    // is ~490 chunks, which would let a frame commit to ~25ms of flush it never accounted for.
    static constexpr double kAdmitBudgetMs   = 2.0;
    // Measured at 0.0052ms admit + 0.0083ms flush. The first fit of this put the flush share at
    // 0.0508ms, which was badly conditioned: the drain was only reaching ~11 chunks a frame, so the
    // regression had almost no spread in x to separate the per-chunk slope from the 2.7ms fixed
    // intercept. Letting the budget open up first, then re-fitting at ~25 chunks a frame, gives the
    // figure below. Trust a re-measurement only when [ADMIT] shows a decent chunks/frame spread.
    static constexpr double kMarginalAdmitMs = 0.014;
    static constexpr int   kBootstrapBatch = 5;
    // Backpressure cap on unadmitted worker results (see terrainWorkerFunc). Bounds how far ahead
    // of kMaxNewPerFrame's admission rate the worker pool is allowed to race.
    static constexpr size_t kMaxReadyBacklog = 512;
    // Cap on WorkerState::workQueue depth. The pending sweep tops the queue up to this and then
    // stops for the frame, leaving the rest of the ring for later frames -- pendingIndex is just a
    // cursor, so nothing is lost by deferring.
    //
    // The queue only has to be deep enough that no worker ever finds it empty; past that, depth is
    // pure harm. Terrain throughput is set by kMaxNewPerFrame (admissions/frame), not by how much
    // work is queued, so extra depth buys no fill rate -- it only ages the queue. A deep queue is
    // full of coords chosen for a camera position the camera has since left, and every one of them
    // is generated, admitted, and then immediately evicted. Sized at roughly 30x the worker count
    // so a full pool stays fed across a frame even if generation times are uneven.
    static constexpr size_t kMaxWorkQueueDepth = 1024;

    // The global sea level. Rivers and lakes carry their own, higher, per-column water surface
    // (TerrainSample::waterTop), so this is the floor of that value rather than the only water in
    // the world. Owned by terrain_noise because that is where continental elevations are measured
    // against it -- two copies of this number that disagree puts the coastline in the wrong place.
    static constexpr float kWaterLevel = terrain_noise::SEA_LEVEL;

    // Horizontal world-space stretch applied to the terrain noise: continents, mountain ranges and
    // climate regions all come out this much wider, while heights are left alone.
    //
    // Horizontal-only is deliberate. Height was never what made the scattered trees read as
    // oversized -- a 112-unit tree against a 2000-unit mountain is already negligible vertically.
    // What made them look big was their *footprint* against the width of the landforms, and that is
    // exactly what this fixes. It also happens to be free: the vertical chunk range is untouched, so
    // the number of chunks generated per column (and the cost of probing the empty ones) does not
    // move. Stretching the terrain vertically instead would roughly double both.
    //
    // A side effect worth knowing about: wider landforms are gentler, so noticeably more ground
    // passes the tree slope test. Widening the terrain thickens the forests on its own.
    static constexpr float kTerrainScale = 2.0f;
    // Raise this to grow the peaks along with the width. It widens the chunk Y range
    // (kChunkYMax scales with kTerrainCeiling), so it is not free.
    static constexpr float kTerrainHeightScale = 1.0f;

    terrain_noise::Generator noiseGen;
    terrain_noise::BlendParams blendParams;
    // Random per run, unless TERRAIN_SEED pins it. Pin it for any before/after measurement: the
    // world's geometry density -- and with it both generation cost and GPU traversal cost -- varies
    // enough between seeds to swamp the change being measured. Two runs of the same binary on
    // different seeds differed by 3x here, which is larger than most of the wins in this file.
    int seed = [] {
        if (const char* e = std::getenv("TERRAIN_SEED")) return int(std::strtol(e, nullptr, 10));
        return int(std::mt19937(std::random_device{}())());
    }();

    trees::TreeLibrary treeLib;
    trees::Params treeParams;

    // Grid
    int gridIndex = -1;
    projv::ComponentHandle gridCompHandle = projv::INVALID_COMPONENT_HANDLE;

    std::unordered_map<projv::core::ivec3, projv::ChunkHandle, ivec3_hash> activeChunks;
    // Handles with a refine (replaceChunkGeometry) request currently in flight on a worker thread.
    // A refine no longer evicts the chunk -- it stays in activeChunks at its old resolution the
    // whole time -- so without this, applyLODRing would see the same handle fail requestChunkLOD
    // every single frame until the async result lands, and queue a duplicate work item each time.
    // Entries are removed once the result is consumed (applied or discarded as stale) in
    // generatePendingChunks, and defensively in evictDistantChunks so a recycled handle can never
    // inherit a stale in-flight marker that isn't actually about it.
    std::unordered_set<projv::ChunkHandle> refiningHandles;

    // Coords that generated no geometry, and the ring they were generated at when that was decided.
    //
    // "Empty" is NOT a property of the coord, it is a property of the coord AT A GIVEN RING, and
    // that distinction is what this map exists to record. A chunk the terrain surface merely grazes
    // resolves to nothing at LOD2 -- 16 columns spanning 448 world units, with detail and erosion
    // octaves dropped (terrainOctavesForLOD) -- while the same chunk at LOD0 has 256 columns of
    // full-octave terrain and a solid corner of ground in it. Treating the coarse answer as final
    // is what made distant terrain that was first seen from far away stay permanently missing as
    // the camera flew into it; `marginal` (see ProcessedChunk) bounds the re-check to the chunks
    // that can actually change, so open sky is still decided once and never revisited.
    struct EmptyRecord { uint32_t lod; bool marginal; };
    std::unordered_map<projv::core::ivec3, EmptyRecord, ivec3_hash> emptyChunks;
    // Coords whose chunk generated as solid featureless underground (see generateChunkVoxels's
    // outBuried). Held at kBuriedLOD by the LOD sweep regardless of camera distance, so the newly
    // solid underground costs a few hundred bytes a chunk rather than the tens of kilobytes a
    // surface chunk does. Promoted to kBuriedEditedLOD once an edit touches them.
    std::unordered_set<projv::core::ivec3, ivec3_hash> buriedCoords;
    // Coords from emptyChunks currently being re-generated at a finer ring. Same job as
    // refiningHandles, but keyed by coord because these have no chunk handle to key on.
    std::unordered_set<projv::core::ivec3, ivec3_hash> revisitingEmpty;

    projv::core::ivec3 lastCameraChunk{-9999, -9999, -9999};

    // The streaming ring, as a camera-relative TEMPLATE rather than a materialized coord list.
    //
    // xzOffsets holds every (dx,dz) inside kViewRadius, sorted nearest-first, and is built exactly
    // once at startup. pendingIndex is a cursor into the implied sequence
    //
    //     for each xzOffsets[i]:  for y in [kChunkYMin, kChunkYMax]:  camChunk + (dx, y, dz)
    //
    // so "restart streaming around the new camera chunk" is `pendingIndex = 0` and costs nothing.
    // What this replaces: every time the camera crossed a chunk boundary, the old code cleared a
    // vector, pushed ~684,000 freshly-computed ivec3 into it, and std::sorted the lot by distance.
    // That is a six-figure allocation-and-sort on the main thread, on a frame the player is by
    // definition moving through, and it recomputed a result that differs from the previous frame's
    // only by a translation.
    //
    // Ordering is XZ-major, but LEVEL-major inside a block of kColumnInterleave columns -- see
    // pendingCoordAt. It was fully column-contiguous, on the reasoning that whole columns near the
    // camera stream better than a shell of scattered slabs. They do, but the cost was almost the
    // whole worker pool:
    //
    //   A vertical stack is kYLevels (22) chunks that share one XZ column, so 22 CONSECUTIVE queue
    //   entries all need the same column prepass. The pool pulls consecutive entries, so ~30 workers
    //   would land on one or two columns; the first to arrive builds the prepass and the rest block
    //   on g_columnCacheCv until it finishes. Measured, that was 79% of all columnsForChunk calls
    //   waiting, 2,475 worker-seconds blocked out of ~3,340 total -- the pool was effectively
    //   processing one column at a time instead of thirty, and a full radius-110 view took 116s to
    //   fill against a ~33s floor. The cache was doing its job (only 4.6% misses, so almost no
    //   duplicated work); it was the queue ORDER that serialized everything.
    //
    // Interleaving inside a block keeps the near-first property at block granularity -- a patch of
    // kColumnInterleave columns still completes before the sweep moves outward -- while guaranteeing
    // that any kColumnInterleave consecutive entries are all DIFFERENT columns, so the pool spreads.
    // It also bounds the cache working set to one block of columns rather than a whole ring, which is
    // what previously made kColumnCacheMaxBytes scale with kViewRadius squared.
    std::vector<projv::core::ivec2> xzOffsets;
    size_t pendingIndex = 0;
    // Chunk coord the cursor's offsets are relative to. Tracks lastCameraChunk, but kept separate
    // so a coord decoded from a stale cursor can never be attributed to the wrong origin.
    projv::core::ivec3 pendingOrigin{0, 0, 0};

    static constexpr int kYLevels = kChunkYMax - kChunkYMin + 1;
    // Columns per interleave block. Must comfortably exceed the worker count so no two workers ever
    // contend for one column's prepass; 128 covers any plausible core count (the pool is
    // hardware_concurrency-2) with room over. Larger costs only cache working set -- one block of
    // fine-ring columns is 128 * 1.33MB = 170MB worst case, against a 512MB budget -- and coarsens
    // the granularity at which streaming is near-first.
    static constexpr size_t kColumnInterleave = 128;

    size_t pendingTotal() const { return xzOffsets.size() * size_t(kYLevels); }

    // Index -> coord, level-major within a block of kColumnInterleave columns.
    //
    // A bijection over [0, pendingTotal()), including the short final block: every block before the
    // last holds exactly kColumnInterleave columns, so block b's entries always begin at
    // b * kColumnInterleave * kYLevels, and the last block simply contributes fewer than the slot it
    // was allotted. That keeps pendingTotal() exactly xzOffsets.size() * kYLevels, so the cursor
    // arithmetic and every pendingIndex comparison elsewhere are unaffected.
    projv::core::ivec3 pendingCoordAt(size_t i) const {
        const size_t nCols = xzOffsets.size();
        const size_t perBlock = kColumnInterleave * size_t(kYLevels);
        const size_t block = i / perBlock;
        const size_t r = i - block * perBlock;
        const size_t blockStart = block * kColumnInterleave;
        // Short final block: iterate over the columns it actually has, not the full stride.
        const size_t blockCols = std::min(kColumnInterleave, nCols - blockStart);
        const projv::core::ivec2& o = xzOffsets[blockStart + (r % blockCols)];
        return projv::core::ivec3(pendingOrigin.x + o.x,
                                  kChunkYMin + int(r / blockCols),
                                  pendingOrigin.z + o.y);
    }

    // Set when the desired-LOD of resident chunks may have gone stale (the camera changed chunk, a
    // refine landed, or the previous sweep ran out of per-frame budget). applyLODRing walks the
    // whole activeChunks map, so it must not run on frames where nothing can have changed --
    // desiredLODForChunk is a pure function of (coord, camChunk), so with a stationary camera every
    // answer is identical to last frame's.
    bool lodSweepNeeded = true;
    // Bumped by every event that can invalidate the part of the disc a sweep has ALREADY scanned:
    // the camera changing chunk, a refine landing, a chunk being admitted below the LOD it wants.
    // lodPassStamp is the value the pass currently in progress started at.
    //
    // A plain `lodSweepNeeded = true` is not enough on its own, because applyLODRing runs after
    // generatePendingChunks and used to *assign* this flag at the end of the frame -- so any wake-up
    // raised earlier in the same frame was silently overwritten by a sweep that had already walked
    // past the coord in question. Comparing stamps instead of trusting a bool makes that lost
    // wake-up impossible: the pass that was running when the event landed cannot close the gate.
    uint64_t lodDirtyStamp = 1;
    uint64_t lodPassStamp = 0;
    // Refine candidates found but dropped for want of per-frame budget, accumulated over the WHOLE
    // pass. Counting only the frame that happens to finish the pass loses every candidate the
    // earlier frames of that same pass had to skip.
    int lodPassDeferred = 0;
    // Arm the sweep. Always use this rather than setting lodSweepNeeded directly -- the stamp is
    // what protects an in-progress pass from closing the gate over the event just raised.
    void armLodSweep() { lodSweepNeeded = true; lodDirtyStamp++; }
    // Camera chunk as of the last LOD sweep, so a sweep can widen its scan radius to cover whatever
    // left the LOD<2 disc while it was not looking. Distinct from lastCameraChunk, which tracks
    // streaming and is updated on a different schedule.
    projv::core::ivec3 lastSweepChunk{-9999, -9999, -9999};
    // Position within the LOD sweep's disc scan, so one sweep can be spread over several frames.
    // Reset whenever the camera changes chunk (the whole disc has moved and every answer in it is
    // potentially stale).
    size_t lodCursor = 0;
    // Set while evictDistantChunks still has chunks outside the radius it has not reached yet.
    bool evictPending = false;

    // Chunk slots freed by eviction, reused before appending to Scene.chunks. Scene.chunks is
    // "slot-indexed with holes" and never shrinks, and its length drives the GPU header texture
    // (capped at maxTextureSize/4 slots), so without reuse a long flight would grow it forever.
    std::vector<projv::ChunkHandle> freeChunkSlots;
    // Monotonic, so an evicted chunk's ID is never handed to a later chunk.
    uint32_t nextChunkID = 0;

    // Sphere placements whose chunks are still arriving.
    //
    // An edit spans up to eight chunks generated in parallel by independent workers, and they finish
    // at different times -- measured at 20ms for the chunk holding a sliver of the sphere against
    // 140ms for the one holding its middle. Installing each as it lands makes the sphere assemble
    // itself on screen over an eighth of a second, one block at a time. Holding the whole batch
    // until its last chunk is ready and installing them in a single frame (and so a single
    // flushSceneUpdates) costs the difference between the fastest and slowest chunk -- which is
    // latency nobody perceives, because the edit was not finished until the slow one landed anyway --
    // and buys an edit that appears all at once.
    struct EditBatch {
        uint32_t expected = 0;
        std::vector<ProcessedChunk> staged;
        std::chrono::steady_clock::time_point requestedAt;
    };
    std::unordered_map<uint64_t, EditBatch> pendingEditBatches;
    uint64_t nextEditBatchID = 1;   // 0 means "not an edit"
    // Backstop against a batch that can never complete (a chunk evicted between request and result,
    // say). Whatever has arrived by then is installed rather than held forever.
    static constexpr int kEditBatchTimeoutMs = 3000;
};

// Every terrain query goes through here, so the world-scale knobs above apply uniformly: sampling
// the noise anywhere else in world coordinates would put that caller on a different landscape.
static inline terrain_noise::TerrainSample sampleWorld(const TerrainState& ts, float wx, float wz,
                                                       int detailOct, int erosionOct) {
    terrain_noise::TerrainSample sample = terrain_noise::sampleTerrain(
        ts.noiseGen, ts.blendParams,
        wx / TerrainState::kTerrainScale, wz / TerrainState::kTerrainScale,
        detailOct, erosionOct);
    sample.height *= TerrainState::kTerrainHeightScale;
    return sample;
}

static projv::core::ivec3 worldToChunkCoord(projv::core::vec3 p) {
    return projv::core::ivec3(int(std::floor(p.x / TerrainState::kChunkSize)),
                              int(std::floor(p.y / TerrainState::kChunkSize)),
                              int(std::floor(p.z / TerrainState::kChunkSize)));
}

static int chunkCoordToLin(projv::core::ivec3 coord, const projv::SceneGrid& grid) {
    projv::core::ivec3 cell = coord - grid.originCellCoord;
    return cell.x + grid.dims.x * (cell.y + grid.dims.y * cell.z);
}

static constexpr int kFillDepth = 30;

// Palette budget. Material IDs are uint8_t everywhere (GeometryBlob::materialIDs, internMaterial,
// the remap tables), and a component palette holds at most MAX_MATERIALS_PER_COMPONENT - 1 entries
// (indices 0..254; 255 is INVALID_MATERIAL). That cap is *global* across every chunk we ever
// generate, not per-chunk -- the component palette accumulates every distinct color the generator
// emits. Overrun it and internMaterial's uint8_t return wraps, so voxels silently alias whatever
// palette slot sits at (index & 255).
//
// Surface colors come out of a continuous 13-way climate blend, which is unbounded, so they are
// snapped to a uniform kSurfaceLevels^3 RGB lattice. The lattice is cubic -- the same level set on
// every channel -- so near-grey rock/snow/ice quantize to neutral greys instead of picking up a
// color cast, and the levels span [0,255] inclusive so pure white snow stays exactly white.
//
// Budget: 216 lattice + 1 deep water + <=3 shallow-water shades + 1 water fill = <=221 of 255.
// kSurfaceLevels = 7 would need 343 lattice entries on its own and blow the cap.
static constexpr int kSurfaceLevels = 6;
static_assert(kSurfaceLevels * kSurfaceLevels * kSurfaceLevels + 5
                  <= int(projv::MAX_MATERIALS_PER_COMPONENT) - 1,
              "quantized surface lattice plus the fixed water colors must fit the palette cap");

// Snap one normalized channel to the nearest lattice level.
static inline uint8_t quantizeSurfaceChannel(float v01) {
    constexpr float kMaxLevel = float(kSurfaceLevels - 1);
    float level = std::round(clampf(v01, 0.0f, 1.0f) * kMaxLevel);
    return uint8_t(level * (255.0f / kMaxLevel) + 0.5f);
}

// `slope` is the terrain gradient (rise/run) at this column, from the height prepass in
// generateChunkVoxels. It is what decides bare rock versus soil: without it every hillside gets the
// same climate colour regardless of whether it is a meadow or a cliff.
// `waterTop` is this column's own water surface -- sea level almost everywhere, but a river reach's
// pool level inside a valley. Submerged ground is coloured relative to it rather than to sea level,
// so a river bed reads as a river bed and not as ordinary hillside that happens to be wet.
//
// `river` is 0 outside a valley and 1 on its axis, used only for the bank strip.
//
// `bare` and `rc` come from the caller rather than being worked out here. Both are needed by the
// column's geometry (the joint and bedding recess) before any colour is chosen, and both are needed
// again by the subsurface fill below the surface voxel, so computing them here would be the second
// of three identical evaluations of the same three Perlin fields per column.
static projv::Color surfaceColor(float height, float t, float h, int worldY,
                                  float wx, float wz, float slope, float waterTop, float river,
                                  float bare, const rock::Column& rc,
                                  const TerrainState& ts) {
    int wl = int(waterTop);

    // Submerged ground. Quantized onto the same lattice as everything else, which the palette
    // budget above depends on: this used to return an ungated ramp keyed on absolute depth, and a
    // ramp is an unbounded number of distinct colours. It cost nothing while the sea bed sat a few
    // tens of units under the waterline, but the continent field now digs 950 units down and rivers
    // put a second waterline anywhere up to the tree line, so the ramp would have spread over
    // dozens of palette slots on top of the 216 the lattice already claims -- and overrunning the
    // cap does not fail loudly, it wraps internMaterial's uint8_t and silently aliases voxels onto
    // whatever colour sits at (index & 255).
    if (worldY < wl) {
        float df = clampf((float(wl) - float(worldY)) * (1.0f / 260.0f), 0.0f, 1.0f);
        return projv::Color{quantizeSurfaceChannel(lerpf(0.28f, 0.06f, df)),
                            quantizeSurfaceChannel(lerpf(0.36f, 0.10f, df)),
                            quantizeSurfaceChannel(lerpf(0.52f, 0.30f, df))};
    }

    // Altitude is measured from SEA level, not from this column's water surface: a tarn near a
    // summit is still alpine, and normalizing against its own pool would tell the snow line and the
    // rock exposure that it is at sea level.
    float altN = clampf((height - TerrainState::kWaterLevel) / terrain_noise::MAX_HEIGHT,
                        0.0f, 1.0f);

    // The rock end members now live in rock_detail.hpp so the surface and the shell beneath it are
    // guaranteed to be reading the same stone.
    using vec3 = projv::core::vec3;
    vec3 rockColor = rock::baseColor(t, h);

    // Simplified material affinities
    struct Mat { projv::core::vec3 color; float tC, tW, hC, hW, aC, aW, rockBias, bias; };
    const Mat mats[] = {
        {{1.00f,0.84f,0.32f},  0.92f,0.18f, 0.05f,0.18f, 0.30f,4.0f, -0.5f,  0.08f}, // hot sand
        {{0.92f,0.60f,0.16f},  0.74f,0.20f, 0.25f,0.20f, 0.30f,4.0f, -0.3f,  0.00f}, // savanna
        {{0.74f,0.46f,0.18f},  0.62f,0.18f, 0.15f,0.18f, 0.30f,4.0f, -0.1f,  0.00f}, // scrub
        {{0.30f,0.65f,0.20f},  0.50f,0.24f, 0.55f,0.24f, 0.30f,4.0f, -0.4f,  0.05f}, // grassland
        {{0.10f,0.40f,0.08f},  0.44f,0.18f, 0.82f,0.20f, 0.30f,4.0f, -0.6f,  0.05f}, // forest
        {{0.04f,0.28f,0.05f},  0.90f,0.16f, 0.93f,0.16f, 0.30f,4.0f, -0.6f,  0.08f}, // jungle
        {{0.12f,0.34f,0.24f},  0.28f,0.18f, 0.75f,0.20f, 0.30f,4.0f, -0.4f,  0.05f}, // taiga
        {{0.96f,0.98f,1.00f},  0.10f,0.22f, 0.40f,0.22f, 0.30f,4.0f, -0.1f,  0.00f}, // tundra
        {{0.93f,0.96f,1.00f},  0.12f,0.22f, 0.07f,0.20f, 0.30f,4.0f,  0.0f,  0.00f}, // cold desert
        {{1.00f,1.00f,1.00f},  0.50f,1.40f, 0.50f,1.40f, 0.30f,4.0f,  3.2f, -2.9f}, // rock
        {{1.00f,1.00f,1.00f},  0.02f,0.28f, 0.72f,0.28f, 0.30f,4.0f, -0.7f,  0.50f}, // snow
        {{0.82f,0.94f,1.00f},  0.00f,0.10f, 0.93f,0.14f, 0.30f,4.0f, -0.5f,  0.20f}, // glacial ice
        {{0.88f,0.82f,0.60f},  0.55f,1.40f, 0.50f,1.40f, 0.004f,0.018f,-0.8f, -0.4f}, // beach
    };
    constexpr int kRockMat = 9;
    constexpr float kSharpness = 2.6f;

    vec3 acc{0, 0, 0};
    float total = 0.0f;
    for (int i = 0; i < 13; ++i) {
        const Mat& m = mats[i];
        float dt = (t - m.tC) / m.tW;
        float dh = (h - m.hC) / m.hW;
        float da = (altN - m.aC) / m.aW;
        float aff = std::exp(m.bias - kSharpness * (dt * dt + dh * dh + da * da));
        acc += (i == kRockMat ? rockColor : m.color) * aff;
        total += aff;
    }
    vec3 col = total > 1e-6f ? acc * (1.0f / total) : vec3{0.5f, 0.5f, 0.5f};

    // Bare rock. The climate blend above answers "what grows here"; this answers "can anything stay
    // here at all", and on a steep face the answer is no. Overriding after the blend rather than
    // adding another affinity term keeps it absolute: a cliff is rock in every biome, which is the
    // single change that turns uniformly-coloured hillsides into terrain with cliffs in it.
    // Bare rock, chosen per voxel by a dither rather than blended in.
    //
    // Cross-fading vegetation into stone sounds right and is wrong here: the two are far apart in
    // hue, so a partial mix leaves all three channels mid-step, they round independently, and the
    // transition band comes out a colour that is in neither -- green-into-blue-grey basalt produced
    // a wide teal fringe. Dithering picks one or the other, so every voxel is a colour that was
    // actually chosen, and the boundary reads as patchy soil clinging to rock, which is what a real
    // slope looks like anyway.
    if (bare > 0.001f && hash2f(int(wx * 3.0f), int(wz * 3.0f) + 4096) < bare) {
        col = rock::color(rc, ts.noiseGen.rockNoise, wx, float(worldY), wz, /*detail=*/true);
    }

    // River banks. The strip between the water line and the top of the cut bank is washed gravel
    // and silt: the current strips it every time the river is in spate, so whatever the climate
    // grows on the slope above, it does not grow here.
    //
    // Dithered against the surrounding ground for the same reason bare rock is -- the bank colour
    // and a green floodplain are far enough apart in hue that a blended fringe rounds to a colour
    // that is in neither.
    if (river > 0.20f) {
        float aboveWater = height - waterTop;
        float bank = rock::rsmooth(0.34f, 0.72f, river)
                   * (1.0f - rock::rsmooth(9.0f, 30.0f, aboveWater));
        if (bank > 0.001f && hash2f(int(wx * 3.0f) + 1531, int(wz * 3.0f) + 8623) < bank)
            col = projv::core::vec3{0.78f, 0.74f, 0.62f};
    }

    float grey = col.x * 0.299f + col.y * 0.587f + col.z * 0.114f;
    col = vec3{clampf(grey + (col.x - grey) * 1.0f, 0.0f, 1.0f),
               clampf(grey + (col.y - grey) * 1.0f, 0.0f, 1.0f),
               clampf(grey + (col.z - grey) * 1.0f, 0.0f, 1.0f)};

    // Per-voxel micro-detail, purely cosmetic (stable hash of world xz, not a new noise field feeding
    // shape/height): a small brightness speckle to read as rock/dirt grain instead of a flat fill,
    // plus an ordered dither of about half a lattice step so the kSurfaceLevels quantization below
    // reads as a smooth gradient instead of visible hard-edged color bands.
    int hx = int(wx * 4.0f), hz = int(wz * 4.0f);
    float speckle = (hash2f(hx, hz) - 0.5f) * 0.10f;               // +/-5% brightness
    float ditherStep = 1.0f / float(kSurfaceLevels - 1);
    float dither = (hash2f(hx + 7919, hz + 104729) - 0.5f) * ditherStep;
    col = vec3{clampf(col.x * (1.0f + speckle) + dither, 0.0f, 1.0f),
               clampf(col.y * (1.0f + speckle) + dither, 0.0f, 1.0f),
               clampf(col.z * (1.0f + speckle) + dither, 0.0f, 1.0f)};

    // Quantized to the lattice so the set of distinct surface colors stays inside the palette cap.
    // The water returns above are left exact -- they are already only four colors in total.
    return projv::Color{quantizeSurfaceChannel(col.x),
                        quantizeSurfaceChannel(col.y),
                        quantizeSurfaceChannel(col.z)};
}

// Depth-graded water color (world units below the water surface). Previously every water voxel --
// shallow lake edge or open-ocean floor alike -- used one flat hardcoded blue, which read as a flat
// color slab on any large body of water. Four shades (matching the palette budget comment above:
// "1 deep water + <=3 shallow-water shades") gives lakes/coastlines a visible depth gradient instead.
static projv::Color waterColorForDepth(float depthBelowSurface) {
    // Pushed bluer/darker in G than a "natural" lake photo would suggest: this renderer's GI bounces
    // ambient light off whatever's nearby (often green terrain/foliage) onto the water's diffuse
    // term, which pulls the rendered result toward green/cyan. Biasing the albedo itself toward blue
    // compensates so the final composited water reads blue instead of algae-green.
    if (depthBelowSurface < 8.0f)  return projv::Color{45, 130, 195};  // shallow: lit, blue-cyan
    if (depthBelowSurface < 30.0f) return projv::Color{25, 85, 175};   // mid
    if (depthBelowSurface < 80.0f) return projv::Color{14, 50, 135};   // deep
    return projv::Color{7, 22, 80};                                    // abyssal
}

// Detail/erosion octave counts to sample terrain at, keyed by storage LOD ring. The near ring
// (LOD0) is the only one ever seen up close, so it gets more octaves than the original hardcoded
// oct=2/eoct=4 -- including detailOct=3, which turns on the Worley-gully term in
// terrain_noise::Generator::detailFn. The far rings (LOD1/LOD2) make up the large majority of
// resident chunks by count (the view radius rings grow with area) but are only ever seen from a
// distance where the extra octaves are invisible, so they drop below the original defaults instead
// -- this is what actually pays for the LOD0 increase and nets out chunk generation faster overall.
static void terrainOctavesForLOD(uint32_t lod, int& detailOct, int& erosionOct) {
    switch (lod) {
        case 0:  detailOct = 3; erosionOct = 6; break;
        case 1:  detailOct = 2; erosionOct = 4; break;
        default: detailOct = 1; erosionOct = 2; break;
    }
}

// Plants trees into a chunk's brick map, after the heightfield has filled it.
//
// Runs on the worker threads and touches no shared mutable state: the library is read-only by now
// (materials were resolved single-threaded at startup) and placement is a pure function of world
// position, so two threads generating adjacent chunks independently agree on every tree that
// crosses between them. See tree_placement.hpp.
//
// The ground is sampled with LOD0's octave counts no matter which ring this chunk belongs to. A
// coarser chunk's own surface is generated with fewer octaves and so sits a little differently, but
// letting the trunk height follow that would plant the same tree at two heights either side of a
// ring boundary. Params::embedVoxels sinks the trunk far enough to swallow the difference.
// Trees are stamped at every LOD ring. A coarse ring reads a coarse mip of the same asset, so a
// distant tree is the same object at the resolution that ring can represent -- 64 voxels tall in
// the near ring, 16 in the middle, 4 in the far one -- rather than blinking into existence at a
// ring boundary. Cost at the far rings is negligible: the coarse mip is ~60 voxels, and most
// candidates are rejected by the patch field before they ever touch the terrain noise.
// `maxGroundH` is the highest terrain column in this chunk's footprint, from the shared prepass.
static void stampTreesIntoChunk(projv::core::ivec3 coord, projv::VoxelBrickMap& map,
                                TerrainState& ts, int res, float vs, uint32_t lod,
                                float maxGroundH) {
    (void)lod;
    if (ts.treeLib.empty()) return;

    // Open sky: no tree rooted anywhere in the footprint reaches this slab. Worth testing before
    // collectTrees rather than relying on its per-instance vertical reject, because collecting is
    // the expensive half -- every candidate that survives the patch field costs a full terrain
    // sample to find its ground. A vertical chunk stack is seventeen levels deep and only two or
    // three of them hold any ground, so without this the tree pass runs, in full, for a dozen
    // slabs of empty air above every column in the world.
    if (float(coord.y) * TerrainState::kChunkSize
            > maxGroundH + trees::canopyHeight(ts.treeLib, ts.treeParams)) return;

    int placeDetailOct, placeErosionOct;
    terrainOctavesForLOD(0, placeDetailOct, placeErosionOct);
    auto sampleGround = [&](float x, float z) {
        terrain_noise::TerrainSample s = sampleWorld(ts, x, z, placeDetailOct, placeErosionOct);
        return trees::GroundSample{s.height, s.temp, s.humid, s.waterTop};
    };

    float ox = float(coord.x) * TerrainState::kChunkSize;
    float oy = float(coord.y) * TerrainState::kChunkSize;
    float oz = float(coord.z) * TerrainState::kChunkSize;
    float reach = trees::canopyReach(ts.treeLib, ts.treeParams);

    // Look one canopy beyond the chunk in XZ: a tree rooted in a neighbour still drops voxels here.
    std::vector<trees::TreeInstance> planted;
    trees::collectTrees(ts.treeLib, ts.treeParams,
                        ox - reach, ox + TerrainState::kChunkSize + reach,
                        oz - reach, oz + TerrainState::kChunkSize + reach,
                        sampleGround, planted);

    for (const trees::TreeInstance& inst : planted) {
        projv::core::vec3 lo, hi;
        trees::treeWorldBounds(inst, ts.treeParams, lo, hi);
        // Vertical reject: most candidates are rooted far below or above this chunk's slab.
        if (hi.y < oy || lo.y > oy + TerrainState::kChunkSize) continue;
        trees::stampTree(inst, ts.treeParams, map, coord, res, vs);
    }
}

// One terrain column: everything the voxel loop needs about a world XZ position, independent of
// which chunk in the vertical stack is being filled.
struct ColumnSample {
    float height;
    float temp;
    float humid;
    float waterTop;   // this column's own water surface: sea level, or a river reach's pool
    float river;      // 0 outside a river valley .. 1 on its axis
};

// The prepass for one (chunkX, chunkZ) at one LOD ring: a (res+2)^2 grid of columns, plus the
// bounds the marginal test needs.
struct ColumnBlock {
    int stride = 0;
    float minH = 0.0f, maxH = 0.0f;
    float minWaterTop = 0.0f, maxWaterTop = 0.0f;
    std::vector<ColumnSample> cells;
};

// Cache of column blocks, keyed by chunk column and ring.
//
// A chunk stack is seventeen levels tall (kChunkYMin..kChunkYMax) and every level used to run the
// full prepass over the same (res+2)^2 world positions -- 66k full terrain samples per chunk at
// LOD0 -- including for the dozen levels of open sky above the ground and open water below it,
// which then produced nothing. The columns do not depend on Y at all, so this computes them once
// per stack. That is a 17x cut in the dominant cost of chunk generation, and it is what makes the
// much taller world affordable: raising the ceiling for towering mountains adds levels to the
// stack, and with the cache in place adding a level is nearly free.
//
// Bounded by count rather than by residency: entries are cheap to rebuild, so when the cache is
// over budget the coldest half is simply dropped. Keyed including the LOD ring because a refine
// regenerates the same column at a different resolution, and the two grids are not interchangeable.
namespace {
struct ColumnCacheKey {
    int cx, cz; uint32_t lod;
    bool operator==(const ColumnCacheKey& o) const { return cx == o.cx && cz == o.cz && lod == o.lod; }
};
struct ColumnCacheKeyHash {
    size_t operator()(const ColumnCacheKey& k) const {
        size_t h = size_t(uint32_t(k.cx)) * 0x9E3779B97F4A7C15ull;
        h ^= size_t(uint32_t(k.cz)) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
        h ^= size_t(k.lod) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
        return h;
    }
};
struct ColumnCacheEntry {
    std::shared_ptr<const ColumnBlock> block;
    uint64_t lastUse = 0;
};

// Budgeted in BYTES, not entries. The three rings differ in size by three orders of magnitude -- a
// LOD0 block is 258^2 columns at 20 bytes, about 1.3 MB, while a LOD2 block is 18^2, about 6 KB --
// so any entry count generous enough to hold the far rings is also a licence to hold thousands of
// near ones, and a flight that leaves a trail of cold LOD0 columns behind it would reach gigabytes
// before the count ever tripped.
// Raised from 192MB after [GENPHASE] showed the column prepass is 86% of a LOD0 chunk's generation
// time (186ms of 218ms). At 192MB the cache held ~144 LOD0 blocks (1.33MB each) against 121 XZ
// columns in the fine ring alone, sharing the budget with every LOD1/LOD2 block -- so ordinary
// streaming traffic evicted fine-ring columns that were about to be needed again, and an edit that
// regenerated a chunk paid a full 190ms prepass instead of a cache hit. 512MB holds ~385, which
// clears the fine ring with room to spare.
constexpr size_t kColumnCacheMaxBytes = 512u << 20;   // 512 MB

std::mutex g_columnCacheMutex;
std::unordered_map<ColumnCacheKey, ColumnCacheEntry, ColumnCacheKeyHash> g_columnCache;
uint64_t g_columnCacheClock = 0;
size_t g_columnCacheBytes = 0;

// Columns a worker is building RIGHT NOW, and a CV to wait on one.
//
// Without this the cache only ever helps a chunk whose column was finished by an earlier chunk --
// and the streaming order makes that the uncommon case, not the common one. Coords are queued XZ
// column by XZ column, so the whole vertical stack for one column becomes queue-adjacent and is
// picked up by the entire worker pool within microseconds of each other. Every one of them misses
// the (still empty) cache and runs the identical 258x258 prepass, so a stack that should cost one
// prepass costs one per level. The cache's entire 22x saving was being lost to that race exactly
// when it mattered most: the fine ring, where a prepass is at its most expensive.
//
// Waiting rather than racing is right here because the duplicate work is total: the loser of the
// race throws away a full LOD0 prepass. Only workers targeting the SAME column ever wait, so the
// pool never serializes on distinct work.
std::unordered_map<ColumnCacheKey, int, ColumnCacheKeyHash> g_columnBuilding;
std::condition_variable g_columnCacheCv;

// Is the column cache actually amortizing a prepass over its vertical stack?
//
// It should: a stack is ~20 chunks deep and they share one XZ column, so 19 of every 20 calls ought
// to be `hit`. [GENPHASE] says otherwise -- LOD2 chunks average 1.74ms in `columns` against a full
// prepass of about 1.6ms, i.e. roughly the whole cost, roughly every time. These counters say which
// of the two possible causes it is, because the phase timer cannot tell them apart:
//
//   miss  the entry was gone and this thread rebuilt it. Duplicate WORK -- the cache is too small,
//         or the stack's chunks are spread far enough apart in time that entries age out between
//         them, and the fix is capacity or ordering.
//   wait  the entry was mid-build by another thread and this one blocked on the CV. No duplicate
//         work, but the pool is serialized: the streaming order queues a whole vertical stack
//         contiguously (see the note above), so ~20 workers can pile onto one column and 19 of them
//         block for the length of one prepass. The fix is to interleave columns in the queue so
//         concurrent workers land on DIFFERENT ones.
//
// waitNs separates the two definitively: if `columns` time is mostly waitNs, it is the second.
struct ColumnCacheStats {
    std::atomic<uint64_t> hit{0}, miss{0}, wait{0}, waitNs{0};
};
ColumnCacheStats g_colStats;

size_t columnBlockBytes(const ColumnBlock& b) {
    return b.cells.size() * sizeof(ColumnSample) + sizeof(ColumnBlock);
}
}  // namespace

// Build (or fetch) the column block for a chunk's XZ footprint at a given ring.
//
// The one-voxel skirt on every side is what makes the terrain gradient continuous ACROSS chunk
// boundaries: with one-sided differences at the edges, two neighbouring chunks would disagree about
// the slope on their shared seam and draw a visible line of mismatched rock down it. It costs
// (res+2)^2/res^2, about 1.6% more samples at LOD0.
static std::shared_ptr<const ColumnBlock> columnsForChunk(const TerrainState& ts, int cx, int cz,
                                                          uint32_t lod, int res, float vs) {
    ColumnCacheKey key{cx, cz, lod};
    {
        std::unique_lock<std::mutex> lock(g_columnCacheMutex);
        bool blocked = false;
        auto waitT0 = std::chrono::steady_clock::now();
        // Loop rather than test once: waking from the CV means SOME column finished, not
        // necessarily this one, and the entry can also have been evicted between the notify and
        // this thread reacquiring the lock.
        for (;;) {
            auto it = g_columnCache.find(key);
            if (it != g_columnCache.end()) {
                it->second.lastUse = ++g_columnCacheClock;
                if (blocked) {
                    g_colStats.wait.fetch_add(1, std::memory_order_relaxed);
                    g_colStats.waitNs.fetch_add(
                        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - waitT0).count()),
                        std::memory_order_relaxed);
                } else {
                    g_colStats.hit.fetch_add(1, std::memory_order_relaxed);
                }
                return it->second.block;
            }
            if (!g_columnBuilding.count(key)) break;   // nobody on it: claim it below
            if (!blocked) { blocked = true; waitT0 = std::chrono::steady_clock::now(); }
            g_columnCacheCv.wait(lock);
        }
        // Falling out of the loop means this thread builds it. If it got here after blocking, the
        // column it waited for was evicted before it could be read -- charge both.
        if (blocked) {
            g_colStats.wait.fetch_add(1, std::memory_order_relaxed);
            g_colStats.waitNs.fetch_add(
                uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - waitT0).count()),
                std::memory_order_relaxed);
        }
        g_colStats.miss.fetch_add(1, std::memory_order_relaxed);
        g_columnBuilding[key] = 1;
    }

    // Built outside the lock: holding it across the build would serialize every worker in the pool
    // behind whichever one is doing terrain noise. g_columnBuilding is what keeps other workers off
    // THIS column meanwhile, without blocking them on any other.
    auto block = std::make_shared<ColumnBlock>();
    const int stride = res + 2;
    block->stride = stride;
    block->cells.resize(size_t(stride) * size_t(stride));

    int detailOct, erosionOct;
    terrainOctavesForLOD(lod, detailOct, erosionOct);

    float ox = float(cx) * TerrainState::kChunkSize;
    float oz = float(cz) * TerrainState::kChunkSize;

    float minH = 1e30f, maxH = -1e30f, minWT = 1e30f, maxWT = -1e30f;
    for (int j = 0; j < stride; ++j) {
        float wz = oz + float(j - 1) * vs;
        for (int i = 0; i < stride; ++i) {
            float wx = ox + float(i - 1) * vs;
            terrain_noise::TerrainSample s = sampleWorld(ts, wx, wz, detailOct, erosionOct);
            block->cells[size_t(j) * stride + i] = {s.height, s.temp, s.humid, s.waterTop, s.river};
            minH = std::min(minH, s.height);   maxH = std::max(maxH, s.height);
            minWT = std::min(minWT, s.waterTop); maxWT = std::max(maxWT, s.waterTop);
        }
    }
    block->minH = minH; block->maxH = maxH;
    block->minWaterTop = minWT; block->maxWaterTop = maxWT;

    std::shared_ptr<const ColumnBlock> result = block;
    {
        std::lock_guard<std::mutex> lock(g_columnCacheMutex);
        if (g_columnCacheBytes >= kColumnCacheMaxBytes) {
            // Evict coldest-first down to half the budget, in one pass. A single sort over the keys
            // is cheaper than keeping an intrusive LRU list correct under contention, and it only
            // happens once per 96 MB admitted.
            std::vector<std::pair<uint64_t, ColumnCacheKey>> ages;
            ages.reserve(g_columnCache.size());
            for (const auto& kv : g_columnCache) ages.emplace_back(kv.second.lastUse, kv.first);
            std::sort(ages.begin(), ages.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& a : ages) {
                if (g_columnCacheBytes <= kColumnCacheMaxBytes / 2) break;
                auto it = g_columnCache.find(a.second);
                if (it == g_columnCache.end()) continue;
                g_columnCacheBytes -= columnBlockBytes(*it->second.block);
                g_columnCache.erase(it);
            }
        }
        auto& e = g_columnCache[key];
        if (e.block) {
            result = e.block;   // evicted and rebuilt by someone else meanwhile; use the live copy
        } else {
            e.block = result;
            g_columnCacheBytes += columnBlockBytes(*result);
        }
        e.lastUse = ++g_columnCacheClock;
        // Release the claim and wake anyone queued behind this column. Must happen under the same
        // lock as the publish, so a waiter can never observe "not building" and "not cached" for a
        // column that is in fact finished.
        g_columnBuilding.erase(key);
    }
    g_columnCacheCv.notify_all();
    return result;
}

// How deep below its surface water is actually filled, in voxels.
//
// Water is opaque here -- it is a coloured voxel, not a transparent medium -- so only the topmost
// voxel of a body of water is ever visible, exactly as with ground (see kFillDepth, which does the
// same job for rock). Filling to the sea bed mattered little when the sea bed sat just below the
// waterline, but the continent field now digs abyssal plains 950 units down, and filling those
// solid would spend the overwhelming majority of every ocean chunk's voxels on water nothing can
// see.
static constexpr int kWaterFillDepth = 40;

// =============================================================================
// Sphere edits
// =============================================================================
//
// An edit is stored as the SHAPE that produced it (a world-space sphere), not as the voxels it
// touched, and it is replayed every time a chunk it overlaps is generated. That is what makes an
// edit survive the things this generator routinely does to a chunk: an LOD refine, an eviction and
// re-stream once the camera flies away and back, a coarse->fine regeneration. All three throw the
// chunk's voxels away and rebuild them from the noise field, so an edit written only into the voxels
// would last until the next one of those and no longer. Replaying the shape costs a clipped
// bounding-box sweep per overlapping chunk per generation and is resolution-independent -- the same
// sphere cuts correctly at 256^3, 64^3 and 16^3, because it is defined in world units rather than
// in voxels.
//
// Written from the main thread (placeSphereEdit) and read from the worker pool (applySphereEdits),
// so everything here is under g_editMutex.
struct SphereEdit {
    projv::core::vec3 center;      // world space
    float             radius;      // world units
    bool              isAdd;
    uint32_t          packedColor; // ignored when !isAdd
};

// Edits bucketed by the chunk coords they overlap, so a chunk generation looks up only its own.
// `version` bumps on every edit added to the bucket; a worker stamps the version it replayed onto
// its result and generatePendingChunks re-queues anything that comes back stale. That closes the
// window where an edit lands *while* a worker is midway through generating the very chunk being
// edited -- that result would otherwise be installed without the edit and nothing would ever come
// along to correct it.
struct ChunkEditBucket {
    uint32_t version = 0;
    std::vector<SphereEdit> edits;
};

static std::mutex g_editMutex;
static std::unordered_map<projv::core::ivec3, ChunkEditBucket, ivec3_hash> g_editsByChunk;
// Lets every hot path below early-out without touching g_editMutex, so the whole feature costs one
// relaxed atomic load per chunk generation until the first edit is actually placed.
static std::atomic<uint32_t> g_editCount{0};

// Per-axis Z-order bit contributions inside a brick, tabulated once.
//
// A Z-order index is a bit interleave, so each axis contributes an independent set of bits and
// zorder(x,y,z) == tx[x] | ty[y] | tz[z]. The tables are derived by asking the engine's own
// computeLocalZOrder for the one-axis-at-a-time cases rather than by reimplementing its bit layout,
// so this cannot drift from it, and the constructor cross-checks the identity on a spread of
// combinations before anything relies on it. What this buys: the innermost fill loop varies only y,
// so the x and z terms hoist out of it entirely and the per-voxel cost drops to a table lookup and
// an OR.
namespace {
struct BrickZOrderTables {
    uint32_t x[projv::BRICK_SIZE], y[projv::BRICK_SIZE], z[projv::BRICK_SIZE];
    BrickZOrderTables() {
        for (uint32_t i = 0; i < projv::BRICK_SIZE; ++i) {
            x[i] = projv::utils::computeLocalZOrder(projv::core::ivec3(int(i), 0, 0));
            y[i] = projv::utils::computeLocalZOrder(projv::core::ivec3(0, int(i), 0));
            z[i] = projv::utils::computeLocalZOrder(projv::core::ivec3(0, 0, int(i)));
        }
        for (uint32_t a = 0; a < projv::BRICK_SIZE; a += 7) {
            for (uint32_t b = 1; b < projv::BRICK_SIZE; b += 11) {
                for (uint32_t c = 2; c < projv::BRICK_SIZE; c += 13) {
                    uint32_t expect = projv::utils::computeLocalZOrder(
                        projv::core::ivec3(int(a), int(b), int(c)));
                    if ((x[a] | y[b] | z[c]) != expect) {
                        projv::core::error("BrickZOrderTables: Z-order is not a plain bit interleave "
                                           "at ({},{},{}) -- the fast sphere fill would corrupt "
                                           "geometry; falling back is required.", a, b, c);
                        valid = false;
                        return;
                    }
                }
            }
        }
    }
    bool valid = true;
};
const BrickZOrderTables g_zTables;
}  // namespace

// Bucket version currently recorded for a coord; 0 if it has never been edited.
static uint32_t editVersionForChunk(projv::core::ivec3 coord) {
    if (g_editCount.load(std::memory_order_relaxed) == 0) return 0u;
    std::lock_guard<std::mutex> lock(g_editMutex);
    auto it = g_editsByChunk.find(coord);
    return it == g_editsByChunk.end() ? 0u : it->second.version;
}

// Replay every edit overlapping this chunk into its brick map, after the terrain itself has been
// generated into it. Returns the bucket version applied, which the caller stamps onto the result.
// Worker-thread side.
static uint32_t applySphereEdits(projv::Scene& scene, projv::ComponentRecord& comp,
                                 projv::core::ivec3 coord, projv::VoxelBrickMap& map,
                                 int res, float vs,
                                 const std::function<float(int, int)>& surfaceHeightAt) {
    if (g_editCount.load(std::memory_order_relaxed) == 0) return 0u;

    std::vector<SphereEdit> edits;
    uint32_t version = 0;
    {
        std::lock_guard<std::mutex> lock(g_editMutex);
        auto it = g_editsByChunk.find(coord);
        if (it == g_editsByChunk.end()) return 0u;
        version = it->second.version;
        edits = it->second.edits;
    }

    const projv::core::vec3 origin(float(coord.x) * TerrainState::kChunkSize,
                                   float(coord.y) * TerrainState::kChunkSize,
                                   float(coord.z) * TerrainState::kChunkSize);

    // Is this voxel rock in the FINAL state of the chunk -- after every edit in the bucket, in the
    // order they were made? Later edits win, so this walks backwards and stops at the first sphere
    // containing the point; below all of them it falls through to the terrain itself.
    auto solidAt = [&](int lx, int ly, int lz) {
        const projv::core::vec3 p(origin.x + float(lx) * vs,
                                  origin.y + float(ly) * vs,
                                  origin.z + float(lz) * vs);
        for (size_t i = edits.size(); i-- > 0; ) {
            const projv::core::vec3 d = p - edits[i].center;
            if (d.x * d.x + d.y * d.y + d.z * d.z <= edits[i].radius * edits[i].radius)
                return edits[i].isAdd;
        }
        return p.y <= surfaceHeightAt(lx, lz);
    };

    // Line every carve with rock.
    //
    // The terrain fill only materialises kFillDepth voxels below the surface, because that is all
    // that can ever be seen -- which stops being true the moment a sphere cuts deeper than the
    // shell, and that is what made digging break through into emptiness. What a carve needs is not
    // the whole volume filled (a solid 256^3 is 16.7M brick-map entries and about 690MB, measured at
    // 4.9 SECONDS a chunk) but rock on the wall it just exposed. That is a surface, not a volume:
    // the shell below is ~100k voxels for a chunk's share of a 128-voxel sphere against 16.7M for
    // the fill, and it renders identically because everything it leaves out is behind it.
    //
    // Thickness is in voxels, so it costs the same at every ring and stays thick enough to survive
    // the renderer coarsening the chunk by dropping tree levels.
    constexpr int kCarveWallThickness = 4;
    const uint8_t rockID = projv::utils::internMaterial(
        scene, comp, "",
        projv::packColor(projv::Color{quantizeSurfaceChannel(0.42f),
                                      quantizeSurfaceChannel(0.40f),
                                      quantizeSurfaceChannel(0.38f)}));
    if (rockID != projv::INVALID_MATERIAL) {
        for (const SphereEdit& e : edits) {
            if (e.isAdd) continue;   // an add brings its own material; only carves expose new wall
            const float outer = e.radius + float(kCarveWallThickness) * vs;
            const float r2 = e.radius * e.radius, o2 = outer * outer;
            auto loIdx = [&](float w, float o) { return std::max(0,       int(std::ceil ((w - o) / vs))); };
            auto hiIdx = [&](float w, float o) { return std::min(res - 1, int(std::floor((w - o) / vs))); };
            const int x0 = loIdx(e.center.x - outer, origin.x), x1 = hiIdx(e.center.x + outer, origin.x);
            const int y0 = loIdx(e.center.y - outer, origin.y), y1 = hiIdx(e.center.y + outer, origin.y);
            const int z0 = loIdx(e.center.z - outer, origin.z), z1 = hiIdx(e.center.z + outer, origin.z);
            for (int lz = z0; lz <= z1; ++lz) {
                const float dz = origin.z + float(lz) * vs - e.center.z;
                for (int lx = x0; lx <= x1; ++lx) {
                    const float dx = origin.x + float(lx) * vs - e.center.x;
                    const float d2xz = dx * dx + dz * dz;
                    if (d2xz > o2) continue;
                    // The shell is two thin y-bands per column (above and below the sphere), or one
                    // solid band where the column misses the sphere entirely. Solving for them beats
                    // testing the AABB: a 4-voxel shell on a 128-voxel sphere is ~823k voxels inside
                    // a 260^3 box, so a per-voxel distance test throws away 95% of its work.
                    const int yOuter = int(std::sqrt(o2 - d2xz) / vs);
                    const int yCenter = int(std::floor((e.center.y - origin.y) / vs));
                    const int yInner = d2xz < r2 ? int(std::sqrt(r2 - d2xz) / vs) : -1;
                    auto band = [&](int lo, int hi) {
                        lo = std::max(lo, y0); hi = std::min(hi, y1);
                        for (int ly = lo; ly <= hi; ++ly) {
                            const float dy = origin.y + float(ly) * vs - e.center.y;
                            const float d2 = d2xz + dy * dy;
                            if (d2 <= r2 || d2 > o2) continue;   // exact edges of the two bands
                            if (!solidAt(lx, ly, lz)) continue;
                            projv::utils::brickMapSetVoxel(map, lx, ly, lz, rockID);
                        }
                    };
                    if (yInner < 0) {
                        band(yCenter - yOuter, yCenter + yOuter);
                    } else {
                        band(yCenter - yOuter - 1, yCenter - yInner + 1);
                        band(yCenter + yInner - 1, yCenter + yOuter + 1);
                    }
                }
            }
        }
    }

    for (const SphereEdit& e : edits) {
        // One intern per edit, not one per voxel: internMaterial takes the shared palette lock and
        // linear-scans the palette, and the loop below runs up to res^3 times.
        uint8_t matID = e.isAdd ? projv::utils::internMaterial(scene, comp, "", e.packedColor)
                                : projv::INVALID_MATERIAL;
        if (e.isAdd && matID == projv::INVALID_MATERIAL) continue;  // palette full; nothing to write

        // Voxel index bounds of the sphere's AABB, clipped to this chunk. Voxel (lx,ly,lz) sits at
        // world `origin + (lx,ly,lz)*vs` -- the same corner convention generateChunkVoxels uses, so
        // an edit lines up exactly with the terrain it is cutting into.
        auto loIdx = [&](float w, float o) { return std::max(0,       int(std::ceil ((w - o) / vs))); };
        auto hiIdx = [&](float w, float o) { return std::min(res - 1, int(std::floor((w - o) / vs))); };
        const int x0 = loIdx(e.center.x - e.radius, origin.x), x1 = hiIdx(e.center.x + e.radius, origin.x);
        const int y0 = loIdx(e.center.y - e.radius, origin.y), y1 = hiIdx(e.center.y + e.radius, origin.y);
        const int z0 = loIdx(e.center.z - e.radius, origin.z), z1 = hiIdx(e.center.z + e.radius, origin.z);

        const float r2 = e.radius * e.radius;

        // Fallback for the case the table constructor rejected: correct, just slower. Checked once
        // per edit rather than per voxel, so it costs nothing when the fast path is live.
        if (!g_zTables.valid) {
            for (int lz = z0; lz <= z1; ++lz) {
                const float dz = origin.z + float(lz) * vs - e.center.z;
                for (int lx = x0; lx <= x1; ++lx) {
                    const float dx = origin.x + float(lx) * vs - e.center.x;
                    const float d2xz = dx * dx + dz * dz;
                    if (d2xz > r2) continue;
                    const float dy = std::sqrt(r2 - d2xz);
                    const int ya = std::max(y0, int(std::ceil ((e.center.y - dy - origin.y) / vs)));
                    const int yb = std::min(y1, int(std::floor((e.center.y + dy - origin.y) / vs)));
                    for (int ly = ya; ly <= yb; ++ly) {
                        if (e.isAdd) projv::utils::brickMapSetVoxel(map, lx, ly, lz, matID);
                        else         projv::utils::brickMapClearVoxel(map, lx, ly, lz);
                    }
                }
            }
            continue;
        }

        // Brick-major, so everything brickMapSetVoxel recomputes per voxel -- brick coord, brick
        // Z-order, bounds check, unique_ptr deref -- is done once per brick instead. A radius-128
        // sphere is ~8.5M voxels at LOD0 and that per-voxel overhead was the single largest term in
        // an edit's latency, ahead of generating the chunk's terrain: measured across a batch, cost
        // tracked each chunk's share of the sphere's VOLUME almost exactly, not its terrain content.
        const int B = int(projv::BRICK_SIZE);
        const int bx0 = x0 / B, bx1 = x1 / B;
        const int by0 = y0 / B, by1 = y1 / B;
        const int bz0 = z0 / B, bz1 = z1 / B;

        for (int bz = bz0; bz <= bz1; ++bz) {
            for (int by = by0; by <= by1; ++by) {
                for (int bx = bx0; bx <= bx1; ++bx) {
                    const projv::core::ivec3 brickCoord(bx, by, bz);
                    if (bx < 0 || by < 0 || bz < 0 ||
                        bx >= map.brickDims.x || by >= map.brickDims.y || bz >= map.brickDims.z)
                        continue;
                    const uint32_t brickIdx = projv::utils::computeBrickZOrder(brickCoord, map.brickDims);

                    // A carve has nothing to do in a brick that holds no voxels; an add has to
                    // create one. This is also what keeps a remove cheap -- it never allocates.
                    if (!map.bricks[brickIdx]) {
                        if (!e.isAdd) continue;
                        map.bricks[brickIdx] = std::make_unique<projv::BrickData>();
                        map.brickMask[brickIdx / 64] |= (1ull << (brickIdx % 64));
                    }
                    projv::BrickData& brick = *map.bricks[brickIdx];

                    const int ox = bx * B, oy = by * B, oz = bz * B;
                    const int vx0 = std::max(x0, ox), vx1 = std::min(x1, ox + B - 1);
                    const int vy0 = std::max(y0, oy), vy1 = std::min(y1, oy + B - 1);
                    const int vz0 = std::max(z0, oz), vz1 = std::min(z1, oz + B - 1);

                    for (int lz = vz0; lz <= vz1; ++lz) {
                        const float dz = origin.z + float(lz) * vs - e.center.z;
                        const uint32_t zBits = g_zTables.z[lz - oz];
                        for (int lx = vx0; lx <= vx1; ++lx) {
                            const float dx = origin.x + float(lx) * vs - e.center.x;
                            // Solve for the Y span inside the sphere rather than testing every voxel
                            // of the AABB: the AABB of a chunk-wide sphere is the whole chunk, and
                            // the ~47% of it outside the sphere would be pure distance-test waste.
                            const float d2xz = dx * dx + dz * dz;
                            if (d2xz > r2) continue;
                            const float dy = std::sqrt(r2 - d2xz);
                            const int ya = std::max(vy0, int(std::ceil ((e.center.y - dy - origin.y) / vs)));
                            const int yb = std::min(vy1, int(std::floor((e.center.y + dy - origin.y) / vs)));
                            const uint32_t xzBits = zBits | g_zTables.x[lx - ox];
                            for (int ly = ya; ly <= yb; ++ly) {
                                const uint32_t lzo = xzBits | g_zTables.y[ly - oy];
                                const uint64_t bit = 1ull << (63 - (lzo % 64));
                                uint64_t& row = brick.mask[lzo / 64];
                                if (e.isAdd) {
                                    row |= bit;
                                    brick.materials[lzo] = matID;
                                } else if (row & bit) {
                                    row &= ~bit;
                                    brick.materials.erase(lzo);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return version;
}

// Where a chunk generation's wall time goes, accumulated per storage ring. Only useful in
// aggregate -- individual chunks vary by two orders of magnitude with how much surface they hold --
// so these are summed into atomics and reported periodically rather than logged per chunk. Answers
// "what is `gen` actually doing", which is not guessable from the code: the column prepass is
// cached and usually free, and the tree64 build is a fixed cost that dominates the cheap chunks.
struct GenPhases {
    double columns = 0, voxels = 0, trees = 0, edits = 0, tree64 = 0, materials = 0;
};
static std::atomic<uint64_t> g_genPhaseNs[3][6] = {};   // [lod][phase], nanoseconds
static std::atomic<uint64_t> g_genPhaseCount[3] = {};
static void recordGenPhases(uint32_t lod, const GenPhases& p) {
    const uint32_t l = lod < 3 ? lod : 2;
    const double v[6] = {p.columns, p.voxels, p.trees, p.edits, p.tree64, p.materials};
    for (int i = 0; i < 6; ++i)
        g_genPhaseNs[l][i].fetch_add(uint64_t(v[i] * 1e6), std::memory_order_relaxed);
    g_genPhaseCount[l].fetch_add(1, std::memory_order_relaxed);
}

// outMarginal (optional) reports whether the terrain surface came within a coarse voxel of this
// chunk's vertical slab. It is only meaningful when the chunk comes back empty; see
// ProcessedChunk::marginal.
static void generateChunkVoxels(projv::Scene& scene, projv::ComponentRecord& comp,
                                projv::core::ivec3 coord, projv::VoxelBrickMap& map,
                                TerrainState& ts, int res, float vs, uint32_t lod,
                                bool* outMarginal = nullptr, GenPhases* outPhases = nullptr,
                                bool* outBuried = nullptr, uint32_t* outEditVersion = nullptr) {
    auto phaseClock = std::chrono::steady_clock::now();
    auto phaseSplit = [&]() {
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - phaseClock).count();
        phaseClock = now;
        return ms;
    };
    float ox = float(coord.x) * TerrainState::kChunkSize;
    float oz = float(coord.z) * TerrainState::kChunkSize;
    float oy = float(coord.y) * TerrainState::kChunkSize;

    int detailOct, erosionOct;
    terrainOctavesForLOD(lod, detailOct, erosionOct);

    // internMaterial is thread-safe but not free -- it locks scene.materialPaletteMutex and
    // linear-scans the shared palette on every call. Terrain columns repeat a small, bounded set
    // of colors (kSurfaceLevels^3 lattice + water) up to res^2 times per chunk, so this per-call
    // cache collapses that down to one real intern (one lock) per *distinct* color actually seen
    // in this chunk, instead of one per column.
    std::unordered_map<uint32_t, uint8_t> colorCache;
    auto internColor = [&](const std::string& name, uint32_t packedColor) -> uint8_t {
        auto it = colorCache.find(packedColor);
        if (it != colorCache.end()) return it->second;
        uint8_t id = projv::utils::internMaterial(scene, comp, name, packedColor);
        colorCache.emplace(packedColor, id);
        return id;
    };

    // Column prepass, shared with every other chunk in this vertical stack. Rock exposure needs the
    // terrain gradient, and a gradient needs neighbouring heights; the grid supplies both.
    std::shared_ptr<const ColumnBlock> columnBlock =
        columnsForChunk(ts, coord.x, coord.z, lod, res, vs);
    if (outPhases) outPhases->columns = phaseSplit();
    const ColumnBlock& cols = *columnBlock;

    // Is this slab entirely underground -- solid rock, no surface anywhere in its footprint? Such a
    // chunk has no detail to show at any resolution (it is one uniform block of stone until somebody
    // digs into it), so it is held at a coarse ring no matter how close the camera gets. That clamp
    // is what makes filling the world to its floor affordable at all; see TerrainState::buriedCoords.
    // minH, not maxH: the test has to hold for EVERY column in the footprint before the chunk can be
    // called featureless, and one column dipping into the slab is enough to make it a surface chunk.
    if (outBuried) *outBuried = (cols.minH >= oy + TerrainState::kChunkSize + vs);
    const int stride = cols.stride;
    auto columnAt = [&](int i, int j) -> const ColumnSample& {
        return cols.cells[size_t(j + 1) * stride + (i + 1)];
    };

    // Does the terrain surface, or any water, come near this chunk's vertical slab at all? The
    // prepass already holds the bounds, so this is a handful of comparisons.
    //
    // The margin is one coarse voxel on each side. That is the scale at which this ring's sampling
    // can be wrong: a finer ring samples more columns AND more detail/erosion octaves
    // (terrainOctavesForLOD), so a surface sitting just outside the slab here can cross into it
    // there. Well clear of the slab on both sides, no amount of extra detail brings it back, so the
    // chunk is empty at every ring and never needs re-checking.
    if (outMarginal) {
        const float lo = oy - vs;
        const float hi = oy + TerrainState::kChunkSize + vs;
        // Water is bounded now rather than reaching down to the sea bed (see kWaterFillDepth), so
        // "below the water line" is no longer enough to make a slab marginal -- it also has to be
        // within the fill depth of some column's water surface. Without that bound every chunk in
        // the 950-unit-deep abyss would be re-checked at every ring forever.
        // The lower bound carries the subsurface shell, not just the surface: a column whose top
        // stands above this slab still writes kFillDepth voxels downward, and that shell can reach
        // into the slab (see the fill band in the column loop). Testing the bare surface here would
        // mark such a slab "not near ground", and an empty verdict for it would then never be
        // re-checked at a finer ring.
        // With the fill running to the chunk floor there is no lower bound any more: every slab at
        // or below the surface is solid, so "the surface reaches this slab's top" is the whole test.
        bool nearGround = (TerrainState::kSolidFillToWorldBottom &&
                           res <= TerrainState::resolutionForLOD(1))
                              ? (cols.maxH >= lo)
                              : (cols.maxH >= lo && cols.minH - float(kFillDepth) * vs < hi);
        bool nearWater = (cols.maxWaterTop >= lo)
                      && (cols.minWaterTop - float(kWaterFillDepth) * vs < hi)
                      && (cols.minH < cols.maxWaterTop);
        *outMarginal = nearGround || nearWater;
    }

    for (int lz = 0; lz < res; ++lz) {
        float wz = oz + float(lz) * vs;
        for (int lx = 0; lx < res; ++lx) {
            float wx = ox + float(lx) * vs;
            const ColumnSample& sample = columnAt(lx, lz);
            float worldH = sample.height;

            // This column's own water surface. Everywhere but a river valley `waterTop` is sea
            // level and this is exactly what it always was.
            const float wt = sample.waterTop;

            // Waterfalls.
            //
            // A cliff, in a heightfield, is not a surface -- it is the GAP between two columns. So a
            // river going over one gets water on the lip and water in the plunge pool and nothing
            // whatever on the face between them, which is precisely the "water stops and reappears
            // lower down" artefact. No amount of work in the per-point terrain sampler can fix it,
            // because the sampler sees one column and the fall exists only in the relationship
            // between two.
            //
            // Here that relationship is available: the prepass grid holds the neighbours. A channel
            // column whose channel neighbour stands higher carries that neighbour's surface down
            // its own face, so the water sheet follows the drop instead of stopping at the lip. It
            // chains -- each column down a cascade lifts from the one above it -- so a long rapid
            // comes out as continuous falling water rather than a row of disconnected puddles.
            //
            // Restricted to columns that are actually in a channel on BOTH sides, so it can only
            // ever extend water along a river, never spill it onto a bank.
            float surfaceTop = wt;
            if (sample.river > 0.25f) {
                const ColumnSample* nb[4] = {&columnAt(lx + 1, lz), &columnAt(lx - 1, lz),
                                             &columnAt(lx, lz + 1), &columnAt(lx, lz - 1)};
                for (const ColumnSample* n : nb)
                    if (n->river > 0.25f && n->waterTop > surfaceTop) surfaceTop = n->waterTop;
            }
            const bool isFall = surfaceTop > wt + 0.5f;

            const int localWL = int(std::floor((surfaceTop - oy) / vs));
            // A fall fills from the ground up, ignoring the still-water shell depth: the whole
            // point is the vertical ribbon, and clipping it to 40 voxels would reinstate the gap
            // this exists to close. Safe to leave uncapped because a channel is a few columns wide.
            const int waterFloorLocal = isFall ? INT_MIN / 2 : localWL - kWaterFillDepth;

            // Central differences in world units: rise over run, so 1.0 is a 45-degree slope.
            float slope;
            {
                float dhdx = (columnAt(lx + 1, lz).height - columnAt(lx - 1, lz).height) / (2.0f * vs);
                float dhdz = (columnAt(lx, lz + 1).height - columnAt(lx, lz - 1).height) / (2.0f * vs);
                slope = std::sqrt(dhdx * dhdx + dhdz * dhdz);
            }

            float altN0 = clampf((worldH - TerrainState::kWaterLevel) / terrain_noise::MAX_HEIGHT,
                                 0.0f, 1.0f);
            float bare = rock::exposure(slope, altN0, sample.temp, sample.humid);

            // Resolved once per column, here, because all three consumers below need it: the
            // geometry recess, the surface colour, and the subsurface fill.
            rock::Column rc = rock::prepare(ts.noiseGen.microN, wx, wz,
                                            sample.temp, sample.humid, altN0, slope);

            // Joints and bedding cut real geometry, not just colour: where a joint surfaces on bare
            // rock, or a soft bed outcrops, the whole column is recessed. Applied to the HEIGHT
            // before it is voxelized, and only above water. Chunk seams stay consistent because
            // this is a pure function of world XZ, and because both chunks voxelize the carved
            // height rather than the raw one.
            //
            // The recess is in world units, so every LOD ring cuts the same shape -- it used to be
            // in voxels, which made a coarse ring gouge sixteen times deeper than a fine one at the
            // same spot.
            // Shelves standing PROUD of a bare face, the counterpart to the recess above (see
            // rock::platformLift). Evaluated at the recessed height so the two agree about where the
            // wall actually is, and kept as its own value because the shell below this column has to
            // be deepened by the same amount: a shelf is only a shelf if it is solid all the way
            // down to the rock it grows out of, and a lift with the standard fill depth would hang
            // the top of a tall one in the air.
            float platform = 0.0f;
            if (worldH > wt + 4.0f) {
                worldH -= rock::surfaceRecess(rc, ts.noiseGen.rockNoise, wx, worldH, wz, bare);
                platform = rock::platformLift(rc, ts.noiseGen.microN, wx, worldH, wz, bare);
                worldH += platform;
            }

            int topLocalY = int(std::floor((worldH - oy) / vs));
            // Extra shell voxels this column needs purely to carry its shelf back down to the
            // unlifted surface.
            const int platformFill = int(std::ceil(platform / vs));

            // Water occupies [max(ground, waterFloor) .. waterSurface], clipped to the slab.
            auto fillWater = [&](int fromLocal) {
                int top = std::min(localWL, res - 1);
                int bottom = std::max(std::max(fromLocal, waterFloorLocal), 0);
                for (int ly = bottom; ly <= top; ++ly) {
                    float voxelWorldY = oy + float(ly) * vs;
                    // Falling water is shallow water however tall the column is: it is a sheet in
                    // free air, not a body with depth. Grading it by distance below the lip would
                    // darken the bottom of every waterfall to abyssal blue.
                    float depth = isFall ? 0.0f : (surfaceTop - voxelWorldY);
                    // name="" (not "water"): internMaterial matches by NAME first when one is
                    // given, so a shared name would collapse every depth shade below back onto
                    // whichever color got interned first. "" falls through to the color-based
                    // lookup, same as the plain surface-color path below.
                    uint8_t wmatID = internColor("", projv::packColor(waterColorForDepth(depth)));
                    projv::utils::brickMapSetVoxel(map, lx, ly, lz, wmatID);
                }
            };

            if (topLocalY < 0) {
                // Ground is below this chunk; the slab can still hold the water above it.
                if (worldH < surfaceTop) fillWater(0);
                continue;
            }

            // A column whose surface is above this slab is NOT finished with this slab: the shell
            // below that surface is kFillDepth voxels deep, and if the surface sits near the bottom
            // of the chunk above, the rest of that shell belongs down here.
            //
            // Returning early on `topLocalY >= res` is what put horizontal gaps along chunk seams.
            // The chunk above filled its shell down to its own local 0 and stopped; this chunk
            // decided the column was none of its business and wrote nothing; and the voxels between
            // the two -- the remainder of a shell that is 84 world units deep against a 717-unit
            // chunk -- existed in neither. Looking into such a seam from the side, you see straight
            // through the terrain, which is exactly the reported artefact. It shows up worst at
            // corners only because that is where two seams meet and the odds of the surface landing
            // near a boundary are highest.
            //
            // The fill band is therefore computed in this chunk's local coordinates from the
            // column's true (possibly out-of-slab) top, then clipped to the slab. Every chunk in
            // the stack runs the same arithmetic against the same world-space band, so the pieces
            // meet exactly and no chunk needs to know anything about its neighbours.
            const bool topInSlab = (topLocalY < res);

            projv::Color col = surfaceColor(sample.height, sample.temp, sample.humid,
                                            int(worldH), wx, wz, slope, wt, sample.river,
                                            bare, rc, ts);
            if (topInSlab) {
                uint32_t packed = projv::packColor(col);
                uint8_t matID = internColor("", packed);
                projv::utils::brickMapSetVoxel(map, lx, topLocalY, lz, matID);
            }

            // Fill below the surface. Previously this smeared the surface colour straight down,
            // which meant a cliff face was a vertical stripe of whatever grew on top of it. Each
            // voxel is now coloured from its own altitude instead, so bedding reads across a cut
            // face -- which is the entire point of having strata at all.
            // Solid all the way down. Previously this stopped kFillDepth voxels below the surface,
            // which made the world a shell: dig into a hillside and you broke through into nothing,
            // and every chunk more than ~84 units under the surface generated empty and was never
            // resident at all. Filling to the chunk floor instead means a column's rock continues
            // into the chunk below it, and that one into the one below that, down to kChunkYMin.
            //
            // This is only affordable because of the LOD clamp on buried chunks (see
            // TerrainState::buriedCoords): a solid chunk is cheap in tree64 terms -- uniform leaves
            // collapse to one material byte apiece -- but the brick map it is built through is not,
            // and materialising a solid 256^3 would put 16.7M entries in the per-brick material
            // hash maps, on the order of half a gigabyte for one chunk.
            // Only at the coarse rings. Measured: filling a 256^3 chunk solid takes 4,349ms in this
            // loop alone against 25ms for the shell, because it is 16.7M brickMapSetVoxel calls and
            // 16.7M entries across the per-brick material hash maps. At 64^3 the same fill is 53ms
            // and at 16^3 it is under a millisecond, which is the whole reason buried chunks are
            // clamped away from LOD0 (see TerrainState::kBuriedLOD) -- the coarse rings are where
            // the underground actually lives, and they can afford to be solid.
            //
            // A LOD0 chunk therefore still carries only its visible shell. That is correct for
            // terrain, since nothing below kFillDepth is ever on screen; what it does not yet cover
            // is a carve at LOD0 deep enough to cut through the shell, which needs rock kept around
            // the sphere wall specifically rather than the whole volume filled.
            const bool solidFill = TerrainState::kSolidFillToWorldBottom &&
                                   res <= TerrainState::resolutionForLOD(1);
            int bottomFill = solidFill ? 0
                                       : std::max(topLocalY - (kFillDepth + platformFill), 0);
            // Exclusive upper bound: the surface voxel itself is written above (when it is in this
            // slab), and everything strictly below it down to bottomFill is shell.
            const int fillTop = std::min(topLocalY, res);
            if (bottomFill < fillTop) {
                // `rc` was resolved once at the top of this column -- everything constant down the
                // column, so the per-voxel loop below costs a hash rather than three Perlin lookups.

                // Only voxels that something can actually see get the joint/lichen/scree work. A
                // column's side is exposed exactly down to the lowest of its four neighbours' tops;
                // everything below that is buried inside the shell. On flat ground that is one or
                // two voxels per column, on a cliff it is the whole face -- so the cost lands
                // precisely where the detail is visible. Without this the Worley lookups would run
                // on all ~30 fill voxels of all 65k columns and dominate chunk generation.
                float lowestNeighbour = std::min(
                    std::min(columnAt(lx + 1, lz).height, columnAt(lx - 1, lz).height),
                    std::min(columnAt(lx, lz + 1).height, columnAt(lx, lz - 1).height));
                int exposedDownTo = int(std::floor((lowestNeighbour - oy) / vs)) - 1;

                for (int ly = bottomFill; ly < fillTop; ++ly) {
                    float voxelWorldY = oy + float(ly) * vs;
                    // Soil gives way to bedrock with depth, dithered for the same reason as the
                    // surface above: an interpolated soil/rock colour is a colour neither one is.
                    float blend = rock::subsurfaceBlend(worldH - voxelWorldY, vs);
                    projv::Color voxelCol = col;
                    if (blend > 0.001f &&
                        hash2f(int(wx * 3.0f) + ly * 131, int(wz * 3.0f) + 977) < blend) {
                        projv::core::vec3 c = rock::color(rc, ts.noiseGen.rockNoise,
                                                          wx, voxelWorldY, wz,
                                                          /*detail=*/ly >= exposedDownTo);
                        voxelCol = projv::Color{quantizeSurfaceChannel(c.x),
                                                quantizeSurfaceChannel(c.y),
                                                quantizeSurfaceChannel(c.z)};
                    }
                    projv::utils::brickMapSetVoxel(map, lx, ly, lz,
                                                   internColor("", projv::packColor(voxelCol)));
                }
            }

            // Water: fill from just above the ground up to this column's water surface.
            if (worldH < surfaceTop && localWL >= 0) fillWater(topLocalY + 1);
        }
    }

    if (outPhases) outPhases->voxels = phaseSplit();
    stampTreesIntoChunk(coord, map, ts, res, vs, lod, cols.maxH);
    if (outPhases) outPhases->trees = phaseSplit();

    // Edits go on last, so they override the terrain rather than the other way round, and from in
    // here rather than from the caller because lining a carve with rock needs to know where the
    // ground is -- see applySphereEdits. columnAt is the same skirted prepass grid the fill above
    // used, so a wall voxel and the terrain it is cut into agree about the surface exactly.
    uint32_t editVersion = applySphereEdits(
        scene, comp, coord, map, res, vs,
        [&](int lx, int lz) { return columnAt(lx, lz).height; });
    if (outEditVersion) *outEditVersion = editVersion;
    if (outPhases) outPhases->edits = phaseSplit();
}

// Generate one work item and publish the result. Shared by the ordinary worker pool and the
// dedicated edit threads -- the two differ only in which queue they take from, not in what they do
// with what they take.
static void runChunkWorkItem(projv::Scene& scene, TerrainState& ts, const ChunkWorkItem& item) {
    int res = TerrainState::resolutionForLOD(item.lod);
    float vs = TerrainState::voxelScaleForLOD(item.lod);
    auto brickMap = projv::utils::createVoxelBrickMap(
        projv::utils::computeBrickDims(res));
    // scene.components never resizes after startup (the terrain grid's one component is
    // pushed once), so this reference stays valid for the worker's whole lifetime -- the
    // only shared-mutable field on it (materialPalette/paletteVersion) is guarded by
    // scene.materialPaletteMutex inside internMaterial itself.
    projv::ComponentRecord& comp = scene.components[ts.gridCompHandle];
    bool marginal = false;
    bool buried = false;
    ProcessedChunk result;
    result.isEdit = item.isEdit;
    result.requestedAt = item.requestedAt;
    result.editBatchID = item.editBatchID;
    if (item.isEdit) result.startedAt = std::chrono::steady_clock::now();

    GenPhases phases;
    auto phaseClock = std::chrono::steady_clock::now();
    auto phaseSplit = [&]() {
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - phaseClock).count();
        phaseClock = now;
        return ms;
    };
    generateChunkVoxels(scene, comp, item.coord, *brickMap, ts, res, vs, item.lod,
                        &marginal, &phases, &buried, &result.editVersion);
    phaseClock = std::chrono::steady_clock::now();
    result.coord = item.coord;
    result.lod = item.lod;
    result.marginal = marginal;
    result.buried = buried;
    result.refineHandle = item.refineHandle;
    // Edits were applied inside generateChunkVoxels (it owns the column data a carve wall needs);
    // result.editVersion came back through its out-param. The hasVoxels test below therefore still
    // sees them, so a sphere placed in open sky makes the chunk non-empty and a sphere that carves a
    // chunk hollow makes it empty.
    bool hasVoxels = false;
    for (auto& maskWord : brickMap->brickMask) {
        if (maskWord != 0) { hasVoxels = true; break; }
    }
    if (!hasVoxels) {
        result.empty = true;
    } else {
        result.empty = false;
        projv::ChunkHeader hdr;
        hdr.chunkID    = 0;
        hdr.position   = item.worldPos;
        hdr.scale      = TerrainState::kChunkSize;
        hdr.voxelScale = vs;
        hdr.resolution = res;
        hdr.rotation   = projv::core::quat(1, 0, 0, 0);
        result.chunk = projv::utils::createChunk(hdr);
        result.chunk.gridIndex = ts.gridIndex;
        projv::utils::updateChunkFromBrickMap(result.chunk, *brickMap);
        phases.tree64 = phaseSplit();
        // Material IDs are already in final global-palette slots (interned directly above).
        projv::utils::bakeMaterialsFromBrickMap(result.chunk.geometryData,
                                                  result.materialIDs, *brickMap);
        phases.materials = phaseSplit();
    }
    recordGenPhases(item.lod, phases);
    if (item.isEdit) result.finishedAt = std::chrono::steady_clock::now();
    g_worker.completed[item.lod < 3 ? item.lod : 2].fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_worker.resultMutex);
        // Three destinations, by how the result has to be installed and how urgently: in place on an
        // existing chunk (readyRefines), as a brand-new chunk ahead of the frame's admission budget
        // (readyEdits), or as ordinary streaming (readyChunks).
        if (result.refineHandle != kInvalidChunkHandle) g_worker.readyRefines.push_back(std::move(result));
        else if (item.isEdit)                           g_worker.readyEdits.push_back(std::move(result));
        else                                            g_worker.readyChunks.push_back(std::move(result));
    }
}

// Dedicated edit thread: waits on editQueue and nothing else, so a sphere starts generating the
// moment it is placed no matter how backed up the ordinary pool is. See WorkerState::editQueue.
static void terrainEditWorkerFunc(projv::Scene& scene, TerrainState& ts) {
    while (g_worker.running.load(std::memory_order_relaxed)) {
        ChunkWorkItem item;
        bool gotWork = false;
        {
            std::unique_lock<std::mutex> lock(g_worker.workMutex);
            g_worker.editCv.wait_for(lock, std::chrono::milliseconds(100), [&]{
                return !g_worker.running.load(std::memory_order_relaxed) ||
                       !g_worker.editQueue.empty();
            });
            if (!g_worker.running.load(std::memory_order_relaxed)) return;
            if (!g_worker.editQueue.empty()) {
                item = std::move(g_worker.editQueue.front());
                g_worker.editQueue.pop_front();
                gotWork = true;
            }
        }
        if (gotWork) runChunkWorkItem(scene, ts, item);
    }
}

static void terrainWorkerFunc(projv::Scene& scene, TerrainState& ts) {
    while (g_worker.running.load(std::memory_order_relaxed)) {
        ChunkWorkItem item;
        bool gotWork = false;
        {
            std::unique_lock<std::mutex> lock(g_worker.workMutex);
            // Backpressure: don't generate further ahead of what generatePendingChunks can admit
            // each frame (kMaxNewPerFrame) for ORDINARY streaming work. Now that LOD-aware
            // generation makes far chunks cheap, workers can race through the entire pending queue
            // in seconds -- without this cap, tens of thousands of completed-but-unadmitted
            // ProcessedChunks pile up in readyChunks, reproducing the same unbounded RAM growth
            // this file was fixed to avoid, just moved one stage later in the pipeline.
            //
            // Refines (front().refineHandle set) bypass this cap entirely: they're rare (bounded
            // by kMaxRefinesPerFrame at request time, so they can never flood readyRefines) and
            // time-sensitive -- the chunk is already visible at a coarser resolution, so making it
            // wait behind a possibly-huge ordinary backlog is exactly the "takes a long time to
            // refine on approach" symptom this exists to avoid.
            g_worker.workCv.wait_for(lock, std::chrono::milliseconds(100), [&]{
                if (!g_worker.running.load(std::memory_order_relaxed)) return true;
                // Edits are never held back by the readyChunks backpressure cap: they publish to
                // readyEdits/readyRefines, neither of which that cap protects, and they are the one
                // kind of work whose latency is directly visible to whoever pressed the key.
                if (!g_worker.editQueue.empty()) return true;
                if (g_worker.workQueue.empty()) return false;
                if (g_worker.workQueue.front().jumpQueue) return true;
                std::lock_guard<std::mutex> rlock(g_worker.resultMutex);
                return g_worker.readyChunks.size() < TerrainState::kMaxReadyBacklog;
            });
            if (!g_worker.running.load(std::memory_order_relaxed)) return;
            // Edit work ahead of everything, so a batch bigger than kEditWorkers spills onto the
            // ordinary pool rather than queueing behind it.
            if (!g_worker.editQueue.empty()) {
                item = std::move(g_worker.editQueue.front());
                g_worker.editQueue.pop_front();
                gotWork = true;
            } else if (!g_worker.workQueue.empty()) {
                if (g_worker.workQueue.front().jumpQueue) {
                    item = g_worker.workQueue.front();
                    g_worker.workQueue.pop_front();
                    gotWork = true;
                } else {
                    std::lock_guard<std::mutex> rlock(g_worker.resultMutex);
                    if (g_worker.readyChunks.size() < TerrainState::kMaxReadyBacklog) {
                        item = g_worker.workQueue.front();
                        g_worker.workQueue.pop_front();
                        gotWork = true;
                    }
                }
            }
        }
        if (gotWork) runChunkWorkItem(scene, ts, item);
        // else: no work, loop back to wait on cv
    }
}

// Tear down one streamed chunk and hand its resources back.
//
// Order matters: clearing the grid cell is what actually hides the chunk, because syncSceneTables
// rebuilds the cellMap from cellToChunk each flush and a -1 cell becomes 0xFFFFFFFF, which the
// shader skips. The stale GPU header row for this slot is therefore unreachable, and it gets
// rewritten anyway if the slot is later reused (headerDirty below).
static void releaseChunk(projv::Scene& scene, TerrainState& ts, projv::ChunkHandle h) {
    if (h == kInvalidChunkHandle || h >= scene.chunks.size()) return;
    projv::Chunk& c = scene.chunks[h];
    if (!c.alive) return;

    if (c.gridIndex >= 0 && static_cast<size_t>(c.gridIndex) < scene.grids.size()) {
        projv::SceneGrid& grid = scene.grids[c.gridIndex];
        if (c.cellIndex >= 0 && static_cast<size_t>(c.cellIndex) < grid.cellToChunk.size() &&
            grid.cellToChunk[c.cellIndex] == static_cast<int32_t>(h)) {
            grid.cellToChunk[c.cellIndex] = -1;
        }
    }

    // Drop this chunk's reference to its geometry via the engine's releaseBlob (refcount decrement
    // + slot recycling into blobFreeList). At zero, additionally clear the CPU-side vectors
    // proactively here rather than waiting for the slot to be reused -- releaseBlob only manages
    // the refcount/free-list bookkeeping, not memory, and bounding peak RAM sooner rather than at
    // next reuse is the whole point of evicting in the first place. GPU texel ranges are freed by
    // uploadDirtyBlobs, which looks for exactly this (refCount == 0 with an uploaded range) on the
    // next flush.
    int32_t pool = c.geometryPoolIndex;
    if (pool >= 0 && static_cast<size_t>(pool) < scene.geometryPool.size()) {
        projv::GeometryBlob& blob = scene.geometryPool[pool];
        bool wasLastRef = (blob.refCount == 1);
        projv::releaseBlob(scene, pool);
        if (wasLastRef) {
            blob.geometry.clear();      blob.geometry.shrink_to_fit();
            blob.materialIDs.clear();   blob.materialIDs.shrink_to_fit();
            blob.brickMap.reset();
            blob.dirty = false;
        }
    }

    c.alive = false;
    c.geometryPoolIndex = -1;
    c.gridIndex = -1;
    c.cellIndex = -1;
    ts.freeChunkSlots.push_back(h);
}

// Storage LOD a chunk should be resident at, from its XZ distance to the camera chunk.
// Three rings: LOD 0 (256³ finest) for the innermost ring, LOD 1 (64³) for the mid ring,
// LOD 2 (16³) for the outer ring. Squared distance to match the view-radius tests.
static uint32_t desiredLODForChunk(projv::core::ivec3 coord, projv::core::ivec3 camChunk) {
    int dx = coord.x - camChunk.x;
    int dz = coord.z - camChunk.z;
    int d2 = dx * dx + dz * dz;
    if (d2 <= TerrainState::kLodRadiusFine * TerrainState::kLodRadiusFine)
        return 0u;
    if (d2 <= TerrainState::kLodRadius * TerrainState::kLodRadius)
        return 1u;
    return 2u;
}

// Distance-based LOD, then held back for solid underground. A buried chunk is one uniform block of
// stone, so the fine rings have nothing to resolve in it; the only reason to spend resolution there
// is a carve, and then only on the chunk actually carved. Applied at every point that asks what
// resolution a chunk should be -- admission, the LOD sweep, and the edit path -- because a clamp
// only one of them knows about would have the sweep and the worker disagreeing forever, each
// re-requesting a regeneration the other undoes.
static uint32_t desiredLODClamped(const TerrainState& ts, projv::core::ivec3 coord,
                                  projv::core::ivec3 camChunk) {
    uint32_t desired = desiredLODForChunk(coord, camChunk);
    if (!ts.buriedCoords.count(coord)) return desired;
    const uint32_t floorLOD = editVersionForChunk(coord) != 0 ? TerrainState::kBuriedEditedLOD
                                                             : TerrainState::kBuriedLOD;
    return std::max(desired, floorLOD);
}

// Chunks are now generated directly at the resolution their assigned ring needs (see
// terrainWorkerFunc), so a chunk's actual on-CPU detail is whatever ring it happened to be
// generated at -- recovered here from the resolution stamped on its header at generation time.
static uint32_t nativeLODFromResolution(uint32_t res) {
    if (res >= uint32_t(TerrainState::resolutionForLOD(0))) return 0u;
    if (res >= uint32_t(TerrainState::resolutionForLOD(1))) return 1u;
    return 2u;
}

// Size and standoff of an edit sphere, in LOD0 voxels. Converted to world units at the call site via
// kVoxelScale, so the sphere keeps its intended size in voxels-you-can-see terms rather than
// silently changing scale with the world grid.
static constexpr float kEditSphereRadiusVoxels   = 128.0f;  // 256 voxels across
static constexpr float kEditSphereDistanceVoxels = 500.0f;

// Record a sphere edit and get the chunks it touches back on screen with it applied.
//
// Nothing is written into any resident chunk here. The edit goes into the store and the affected
// coords are pushed back through the ordinary generation pipeline, which is what makes this one code
// path rather than two: the same replay that services this key press also services the refine, the
// re-stream and the LOD change that will rebuild these chunks later.
static void placeSphereEdit(projv::Scene& scene, TerrainState& ts,
                            projv::core::vec3 center, float radius,
                            bool isAdd, uint32_t packedColor) {
    // Chunk coords the sphere's AABB overlaps. Y is clipped to the generated range -- a sphere above
    // the terrain ceiling or below the sea floor simply touches nothing.
    auto chunkOf = [](float w) { return int(std::floor(w / TerrainState::kChunkSize)); };
    const int x0 = chunkOf(center.x - radius), x1 = chunkOf(center.x + radius);
    const int y0 = std::max(TerrainState::kChunkYMin, chunkOf(center.y - radius));
    const int y1 = std::min(TerrainState::kChunkYMax, chunkOf(center.y + radius));
    const int z0 = chunkOf(center.z - radius), z1 = chunkOf(center.z + radius);

    // Exact sphere-vs-box, not just the AABB overlap the loop bounds give: the sphere is inscribed
    // in a 2x2x2-chunk box, so the corner chunks of that box are usually only clipped by the AABB
    // and not touched by the sphere at all. Regenerating one is a full chunk of wasted work on the
    // critical path of a key press, and the test that avoids it is four lines.
    auto sphereHitsChunk = [&](int cx, int cy, int cz) {
        const projv::core::vec3 lo(float(cx) * TerrainState::kChunkSize,
                                   float(cy) * TerrainState::kChunkSize,
                                   float(cz) * TerrainState::kChunkSize);
        auto axis = [](float c, float l) {
            const float h = std::max(l, std::min(c, l + TerrainState::kChunkSize)) - c;
            return h * h;
        };
        return axis(center.x, lo.x) + axis(center.y, lo.y) + axis(center.z, lo.z) <= radius * radius;
    };

    std::vector<projv::core::ivec3> touched;
    {
        std::lock_guard<std::mutex> lock(g_editMutex);
        for (int cz = z0; cz <= z1; ++cz) {
            for (int cy = y0; cy <= y1; ++cy) {
                for (int cx = x0; cx <= x1; ++cx) {
                    if (!sphereHitsChunk(cx, cy, cz)) continue;
                    projv::core::ivec3 coord(cx, cy, cz);
                    ChunkEditBucket& bucket = g_editsByChunk[coord];
                    // Drop any earlier edit this one completely contains. Whatever that edit did is
                    // entirely overwritten inside the new sphere, and it reached nothing outside it,
                    // so it can never affect the result again -- keeping it would just add a full
                    // sphere sweep to every future regeneration of this chunk. Exact, not a
                    // heuristic. It is what stops repeated edits in one spot (digging out a room)
                    // from making each press slower than the last.
                    bucket.edits.erase(
                        std::remove_if(bucket.edits.begin(), bucket.edits.end(),
                                       [&](const SphereEdit& old) {
                                           const projv::core::vec3 d = old.center - center;
                                           const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                                           return dist + old.radius <= radius;
                                       }),
                        bucket.edits.end());
                    bucket.edits.push_back(SphereEdit{center, radius, isAdd, packedColor});
                    bucket.version++;
                    touched.push_back(coord);
                }
            }
        }
        // Published last, and only once the store is consistent: a worker that sees a non-zero count
        // must find the buckets it implies already in place.
        g_editCount.fetch_add(1, std::memory_order_release);
    }

    if (touched.empty()) return;

    const auto requestedAt = std::chrono::steady_clock::now();
    const uint64_t batchID = ts.nextEditBatchID++;
    {
        TerrainState::EditBatch& batch = ts.pendingEditBatches[batchID];
        batch.expected = uint32_t(touched.size());
        batch.requestedAt = requestedAt;
    }
    projv::SceneGrid& grid = scene.grids[ts.gridIndex];
    {
        std::lock_guard<std::mutex> lock(g_worker.workMutex);
        for (const projv::core::ivec3& coord : touched) {
            projv::core::vec3 wp = grid.origin +
                (projv::core::vec3(coord - grid.originCellCoord) * TerrainState::kChunkSize);
            auto it = ts.activeChunks.find(coord);
            const bool resident = it != ts.activeChunks.end() &&
                                  it->second != kInvalidChunkHandle &&
                                  it->second < scene.chunks.size() &&
                                  scene.chunks[it->second].alive;
            if (resident) {
                // Refine path: replaceChunkGeometry installs the result in place, so the chunk stays
                // visible at its current detail until the edited version is ready -- no pop-out.
                // Queued unconditionally rather than skipped when refiningHandles already holds this
                // chunk: an in-flight refine may have started before the edit was recorded, and
                // discarding it in favour of this one is exactly what the stamp check in
                // generatePendingChunks does when it arrives.
                const projv::ChunkHandle h = it->second;
                // The finer of what the chunk holds now and what its distance wants, so an edit
                // never costs a chunk detail it already had.
                uint32_t lod = std::min(nativeLODFromResolution(scene.chunks[h].header.resolution),
                                        desiredLODClamped(ts, coord, ts.lastCameraChunk));
                ts.refiningHandles.insert(h);
                g_worker.queuedCoords.insert(coord);
                g_worker.editQueue.push_back(ChunkWorkItem{coord, wp, lod, h, /*jumpQueue=*/true,
                                                          /*isEdit=*/true, requestedAt, batchID});
            } else {
                // No chunk here: open sky an add-sphere is being built in, or terrain that has not
                // streamed in yet. Ordinary admission, with any "known empty" verdict cleared first
                // -- the edit is precisely what makes the coord non-empty, and the sentinel would
                // otherwise make admit() and the pending sweep both skip right past it.
                ts.activeChunks.erase(coord);
                ts.emptyChunks.erase(coord);
                ts.revisitingEmpty.erase(coord);
                g_worker.queuedCoords.insert(coord);
                g_worker.editQueue.push_back(ChunkWorkItem{coord, wp,
                                                          desiredLODClamped(ts, coord, ts.lastCameraChunk),
                                                          kInvalidChunkHandle, /*jumpQueue=*/true,
                                                          /*isEdit=*/true, requestedAt, batchID});
            }
        }
    }
    // Both pools: the dedicated threads take the batch immediately, and any ordinary worker that
    // finishes its current chunk picks up the remainder ahead of its own queue.
    g_worker.editCv.notify_all();
    g_worker.workCv.notify_all();

    projv::core::info("[EDIT] {} sphere r={:.0f} at ({:.0f}, {:.0f}, {:.0f}) -- {} chunks requeued",
                      isAdd ? "add" : "remove", radius,
                      center.x, center.y, center.z, touched.size());
}

// Retune resident chunks toward their distance-appropriate LOD, at most kMaxRefinesPerFrame refine
// triggers per call. Walks the whole activeChunks map, but only on frames where the answer can have
// changed -- see the lodSweepNeeded gate at the top of the body. Chunks whose LOD is already right
// cost only the map iteration itself: requestChunkLOD returns Unchanged and touches nothing.
// Re-walking from the start each sweep rather than holding a cursor keeps this safe against the map
// mutating under generation and eviction, and still makes forward progress since applied chunks
// stop matching.
//
// (The map is NOT "a few thousand" entries as an earlier version of this comment assumed: it holds
// one entry per resolved coord including the empty-sky sentinels, which is the full ring -- 835,582
// at the shipped view radius. That is why the gate exists.)
//
// Returns true if anything was changed, i.e. if a flushSceneUpdates is needed to push it to the
// GPU. This return value is NOT optional bookkeeping: requestChunkLOD records the new level in
// Chunk::requestedLOD as it applies it, so every later call for that chunk returns Unchanged. A
// change that is applied CPU-side but never flushed is therefore never retried and the chunk is
// stuck at its old detail for good. The caller must flush in the same frame this returns true.
//
// requestChunkLOD handles the coarsening direction entirely (GPU-side truncation of data that's
// already resident) and never touches activeChunks, so it's safe to call directly from this
// single range-for loop. Refining past a chunk's native resolution has no data to reveal --
// requestChunkLOD reports that back as NeedsRegeneration without changing anything -- so those
// candidates are collected into a second pass that kicks off an async regenerate via
// replaceChunkGeometry (generatePendingChunks). Unlike the old design, this does NOT evict the
// chunk: it stays resident and visible at its current resolution the whole time, so there's no
// pop-out. ts.refiningHandles suppresses re-triggering the same handle every frame while that
// regeneration is in flight (requestChunkLOD's NeedsRegeneration path doesn't record anything
// itself, so without this guard the same chunk would get queued again on every subsequent frame
// until the first request completes).
static bool applyLODRing(projv::Scene& scene, TerrainState& ts, projv::core::ivec3 camChunk) {
    // Only sweep when an answer can have changed -- see TerrainState::lodSweepNeeded --
    // and, when sweeping, look only where a change is POSSIBLE rather than at every resolved coord.
    //
    // Both halves are needed. Gating alone is not enough: `deferred` (more LOD candidates than the
    // per-frame refine budget) is continuously true while the camera is moving, which is exactly
    // when the sweep is most expensive, so the gate opens every frame during flight. Measured at
    // 52ms per sweep over 700k activeChunks entries, running every frame -- 21.8 of a 22-second
    // flight was spent in here, and it alone accounted for the 15fps and the 125ms spikes.
    //
    // What makes the narrow scan correct: desiredLODForChunk is a step function of XZ distance with
    // its outermost step at kLodRadius, so every chunk beyond that radius wants LOD2 and cannot
    // change while it stays there. Only coords inside the disc need examining, plus a margin wide
    // enough to cover chunks that left the disc since the last sweep and are still owed their
    // coarsening. xzOffsets is distance-sorted, so the disc is a prefix of it -- no search needed.
    if (!ts.lodSweepNeeded) return false;
    auto sweepT0 = std::chrono::high_resolution_clock::now();

    // Margin: how far the camera has travelled (in chunks) since the last sweep, so anything that
    // crossed out of the LOD<2 disc in that time is still covered. The +2 floor absorbs the normal
    // one-chunk step; the clamp makes a teleport degrade to the old full-ring behaviour rather than
    // silently skip chunks.
    int moved = std::max(std::abs(camChunk.x - ts.lastSweepChunk.x),
                         std::abs(camChunk.z - ts.lastSweepChunk.z));
    int sweepR = std::min(TerrainState::kViewRadius, TerrainState::kLodRadius + std::max(2, moved));
    if (moved > 2) ts.lodCursor = 0;   // only rewind on large jumps; margin handles small drift
    ts.lastSweepChunk = camChunk;
    const int sweepR2 = sweepR * sweepR;

    // How many disc coords one frame may examine.
    //
    // Even narrowed to the disc this is ~40,000 lookups into a 400,000-entry map, which measured
    // 4.45ms -- and it ran every frame, because `deferred` stays true for as long as there are more
    // LOD candidates than the 3-per-frame refine budget, which is the entire time the camera is
    // moving. Bounding the scan and resuming from a cursor turns "4.45ms every frame" into "0.9ms
    // every frame, whole disc covered every five", which is well inside the refine budget's own
    // latency anyway: at 3 refines a frame, scanning faster than this cannot dispatch more work.
    static constexpr size_t kMaxLodScanPerFrame = 8192;
    size_t discCoords = 0;
    for (const projv::core::ivec2& o : ts.xzOffsets) {
        if (o.x * o.x + o.y * o.y > sweepR2) break;
        discCoords++;
    }
    discCoords *= size_t(TerrainState::kYLevels);
    if (ts.lodCursor >= discCoords) ts.lodCursor = 0;

    // Starting a fresh pass over the disc: latch the stamp it is being run against, and reset the
    // pass-wide deferred count. Everything below judges "did this pass finish the job" against
    // these, not against the current frame alone.
    if (ts.lodCursor == 0) {
        ts.lodPassStamp = ts.lodDirtyStamp;
        ts.lodPassDeferred = 0;
    }

    int applied = 0;
    // Candidates this sweep found but had no per-frame budget left for. Non-zero means the sweep
    // did not finish the job, so it must run again next frame rather than wait for the camera to
    // move -- otherwise a chunk that lost the budget race stays at the wrong LOD indefinitely.
    int deferred = 0;
    std::vector<std::pair<projv::core::ivec3, projv::ChunkHandle>> toRefine;
    std::vector<projv::core::ivec3> toRecheck;
    const size_t scanEnd = std::min(discCoords, ts.lodCursor + kMaxLodScanPerFrame);
    {
        for (size_t idx = ts.lodCursor; idx < scanEnd; ++idx) {
            const projv::core::ivec2& off = ts.xzOffsets[idx / size_t(TerrainState::kYLevels)];
            const int y = TerrainState::kChunkYMin + int(idx % size_t(TerrainState::kYLevels));
            projv::core::ivec3 coord(camChunk.x + off.x, y, camChunk.z + off.y);
            auto itA = ts.activeChunks.find(coord);
            if (itA == ts.activeChunks.end()) continue;   // not resolved yet; streaming will admit it
            const projv::ChunkHandle h = itA->second;
            uint32_t desired = desiredLODClamped(ts, coord, camChunk);

            // "Known empty" coords are not chunks, but they are not necessarily empty either -- see
            // TerrainState::emptyChunks. Re-check one at a finer ring than the one that decided it,
            // but only if the surface actually grazed it (marginal); open sky and deep bedrock are
            // decided once and stay decided, which is what keeps this from re-running a full 256³
            // generation over the ~200 empty sky cells that sit inside the fine ring at any moment.
            if (h == kInvalidChunkHandle) {
                auto itE = ts.emptyChunks.find(coord);
                if (itE == ts.emptyChunks.end()) continue;
                if (!itE->second.marginal || desired >= itE->second.lod) continue;
                if (ts.revisitingEmpty.count(coord)) continue;
                deferred += (toRecheck.size() >= size_t(TerrainState::kMaxRefinesPerFrame));
                if (toRecheck.size() < size_t(TerrainState::kMaxRefinesPerFrame))
                    toRecheck.push_back(coord);
                continue;
            }
            if (h >= scene.chunks.size()) continue;
            const projv::Chunk& c = scene.chunks[h];
            if (!c.alive || c.geometryPoolIndex < 0) continue;
            if (ts.refiningHandles.count(h)) continue; // already regenerating; don't re-trigger

            uint32_t requestedRes = TerrainState::resolutionForLOD(desired);
            projv::ChunkLODResult result = projv::requestChunkLOD(scene, h, requestedRes);
            if (result == projv::ChunkLODResult::Coarsened) {
                applied++;
            } else if (result == projv::ChunkLODResult::NeedsRegeneration) {
                deferred += (toRefine.size() >= size_t(TerrainState::kMaxRefinesPerFrame));
                if (toRefine.size() < size_t(TerrainState::kMaxRefinesPerFrame))
                    toRefine.push_back({coord, h});
            }
        }
    }

    // Come back next frame unless this pass is genuinely finished. "Finished" means all three of:
    // it reached the end of the disc, it acted on every candidate it found rather than dropping some
    // for want of refine budget, and nothing invalidated its already-scanned prefix while it ran.
    //
    // That last condition is the one a bool cannot express. The disc is scanned nearest-first in
    // kMaxLodScanPerFrame slices spread over several frames, so when the camera changes chunk (or a
    // refine lands) partway through, the coords the pass already walked were judged against inputs
    // that no longer hold -- and the nearest chunks, which need the finest LOD, sit in exactly that
    // already-walked prefix. Closing the gate there left them coarse with nothing scheduled to look
    // at them again until the camera next crossed a chunk boundary, which is why walking onto a
    // chunk fixed it and standing still did not.
    ts.lodCursor = scanEnd;
    ts.lodPassDeferred += deferred;
    const bool passComplete = (scanEnd >= discCoords);
    if (passComplete) {
        ts.lodSweepNeeded = (ts.lodPassDeferred > 0) || (ts.lodPassStamp != ts.lodDirtyStamp);
        ts.lodCursor = 0;
    } else {
        ts.lodSweepNeeded = true;
    }

    // Both queues below are pushed under ONE lock acquisition with ONE notify at the end. Taking
    // and dropping workMutex per item, then notify_all-ing per item, woke all N workers up to six
    // times per sweep just to have most of them find the queue's single new item already taken.
    if (!toRefine.empty() || !toRecheck.empty()) {
        projv::SceneGrid& grid = scene.grids[ts.gridIndex];
        std::lock_guard<std::mutex> lock(g_worker.workMutex);

        for (const auto& [coord, h] : toRefine) {
            projv::core::vec3 wp = grid.origin + (projv::core::vec3(coord - grid.originCellCoord) * TerrainState::kChunkSize);
            ts.refiningHandles.insert(h);
            // push_front (not push_back): the chunk is already visible at a coarser resolution, so
            // this should jump straight to the head of the line rather than wait behind whatever
            // ordinary streaming backlog happens to be queued.
            g_worker.workQueue.push_front(ChunkWorkItem{coord, wp, desiredLODClamped(ts, coord, camChunk),
                                                       /*refineHandle=*/h, /*jumpQueue=*/true});
            g_worker.queuedCoords.insert(coord);
        }

        // Empty re-checks go through the ORDINARY path (no refineHandle): there is no chunk to
        // replace, so if this ring does find geometry the result has to be admitted as a brand-new
        // chunk. The sentinel stays in activeChunks meanwhile, which keeps the pending sweep from
        // queueing the same coord a second time; admit() knows to look past a sentinel.
        for (const projv::core::ivec3& coord : toRecheck) {
            projv::core::vec3 wp = grid.origin + (projv::core::vec3(coord - grid.originCellCoord) * TerrainState::kChunkSize);
            ts.revisitingEmpty.insert(coord);
            g_worker.workQueue.push_front(ChunkWorkItem{coord, wp, desiredLODClamped(ts, coord, camChunk),
                                                       /*refineHandle=*/kInvalidChunkHandle,
                                                       /*jumpQueue=*/true});
            g_worker.queuedCoords.insert(coord);
        }
    }
    if (!toRefine.empty() || !toRecheck.empty()) g_worker.workCv.notify_all();

    // How often this runs, and what it costs when it does, is the thing to watch if frame times
    // regress: the gate above is what keeps an O(activeChunks) walk off the average frame, so a
    // sweep rate approaching one per frame means some trigger is stuck on.
    {
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::high_resolution_clock::now() - sweepT0).count();
        static double sweepMs = 0; static int sweepCount = 0;
        sweepMs += ms; sweepCount++;
        core_perf_every(60, "[LODSWEEP] {} sweeps, {:.2f}ms total, {:.2f}ms each, {} chunks",
                        sweepCount, sweepMs, sweepMs / sweepCount, ts.activeChunks.size());
    }

    // Refines don't dirty anything yet (the geometry lands frames later, via readyRefines), so
    // only the coarsen/flip path needs a flush now.
    return applied > 0;
}

// Evict streamed chunks that have left the view radius, up to a per-call budget.
//
// Budgeted because a single chunk-boundary crossing puts a whole crescent of the ring out of range
// at once -- measured at 3,900 chunks, 20ms in one frame, against a 13ms average. That is a visible
// hitch roughly once per crossing, and it was the largest remaining spike after the LOD sweep was
// bounded. Both halves of the cost are budgeted by the same early exit: the ~300,000-entry map walk
// stops at the cap alongside the releases, so a call costs a fraction of a pass rather than all of
// it. Stopping early is safe -- kEvictHysteresis leaves two chunks of slack outside the view radius,
// so a chunk waiting a few frames for its turn is still nowhere near visible -- and the caller
// re-runs this while ts.evictPending is set, so the backlog drains within a few frames.
static void evictDistantChunks(projv::Scene& scene, TerrainState& ts, projv::core::ivec3 camChunk) {
    auto t0 = std::chrono::high_resolution_clock::now();
    static constexpr int kMaxEvictPerCall = 512;
    const int evictR = TerrainState::kViewRadius + TerrainState::kEvictHysteresis;
    const int evictR2 = evictR * evictR;
    int evicted = 0;
    bool hitCap = false;
    for (auto it = ts.activeChunks.begin(); it != ts.activeChunks.end(); ) {
        int dx = it->first.x - camChunk.x;
        int dz = it->first.z - camChunk.z;
        if (dx * dx + dz * dz <= evictR2) { ++it; continue; }
        if (evicted >= kMaxEvictPerCall) { hitCap = true; break; }
        // Defensive: if a refine happened to be in flight for this handle, drop the marker so a
        // later slot-recycled chunk (freeChunkSlots reuses handles) can never inherit a stale
        // "already regenerating" state that isn't actually about it.
        ts.refiningHandles.erase(it->second);
        ts.emptyChunks.erase(it->first);
        ts.revisitingEmpty.erase(it->first);
        releaseChunk(scene, ts, it->second);   // no-op for the -1 "known empty" sentinel
        it = ts.activeChunks.erase(it);
        evicted++;
    }
    // More to do next frame if the budget ran out before the pass finished.
    ts.evictPending = hitCap;
    if (evicted > 0) {
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::high_resolution_clock::now() - t0).count();
        core_info_every(8, "[EVICT] {} chunks outside radius {} in {:.2f}ms — more={} freeChunkSlots={} blobFreeList={}",
                        evicted, evictR, ms, hitCap,
                        ts.freeChunkSlots.size(), scene.blobFreeList.size());
    }
}

// Returns true if anything was admitted or refined, i.e. if a flushSceneUpdates is needed. The
// flush itself belongs to the caller so that one flush per frame can cover this and applyLODRing
// together -- see the call site.
// Main-thread cost of admitting a generated chunk, and of the GPU flush that follows it.
//
// This is the measurement kMaxNewPerFrame / kAdmitBudgetMs should be set from, and it exists because
// the previous setting rested on an assumption rather than a number: admission was 19x slower than
// the generation feeding it, which made it -- not the terrain noise -- the reason a full view takes
// a minute and a half to appear.
//
// Three costs, separated because they scale differently and only one of them is per-chunk:
//   admit       per chunk. Grid/map writes plus requestChunkLOD. Scales linearly with the cap.
//   drain       per frame. The rest of generatePendingChunks: locks, queue churn, result scanning.
//   flush       per frame. flushSceneUpdates batches every dirty blob into one call, so this is
//               mostly bytes uploaded. Split busy/idle: `idle` is frames that flushed without
//               admitting anything (a pure LOD flip), which gives the fixed baseline, so the
//               difference is the part genuinely attributable to the chunks admitted.
//
// The number to read off is `marginal` -- admit plus the flush's per-chunk share -- because that is
// what one more chunk per frame actually costs.
struct AdmitCost {
    std::atomic<uint64_t> admitNs{0};
    std::atomic<uint64_t> drainNs{0};
    std::atomic<uint64_t> chunks{0};
    std::atomic<uint64_t> busyFrames{0};
    std::atomic<uint64_t> budgetFrames{0};   // frames the drain stopped on kAdmitBudgetMs
    std::atomic<uint64_t> capFrames{0};      // frames the drain stopped on kMaxNewPerFrame
    // Chunks admitted by the most recent call, read at the flush site to classify that frame.
    // Main-thread only, so it needs no synchronization.
    int lastFrameChunks = 0;

    // Least-squares fit of flush time against the number of chunks admitted that frame, over every
    // frame that flushed. Splitting the flush into a fixed per-frame cost and a marginal per-chunk
    // cost is the whole question -- "how many more chunks can a frame afford" is the slope, and
    // dividing total flush time by total chunks answers it wrongly by charging the fixed cost to the
    // chunks. Main-thread only, hence plain doubles.
    double fn = 0, fx = 0, fy = 0, fxx = 0, fxy = 0;   // n, sum x, sum y(ms), sum x^2, sum xy
    void addFlush(int nChunks, double ms) {
        const double x = double(nChunks);
        fn += 1; fx += x; fy += ms; fxx += x * x; fxy += x * ms;
    }
    // Returns false when the fit is degenerate (every frame admitted the same count), which is the
    // case during a pure-LOD-flip idle period and would otherwise divide by zero.
    bool flushFit(double& perChunkMs, double& fixedMs) const {
        const double den = fn * fxx - fx * fx;
        if (fn < 8.0 || std::fabs(den) < 1e-9) return false;
        perChunkMs = (fn * fxy - fx * fy) / den;
        fixedMs = (fy - perChunkMs * fx) / fn;
        return true;
    }
};
static AdmitCost g_admit;

static bool generatePendingChunks(projv::Scene& scene, TerrainState& ts) {
    int generated = 0;
    int consumed = 0;
    // Raised alongside kMaxNewPerFrame: this bounds how many results are EXAMINED, so leaving it at
    // 192 would have quietly become the new binding constraint the moment the admission cap passed
    // it -- a frame could admit at most 192 chunks however much budget it had left.
    static constexpr int kMaxConsumedPerFrame = 512;
    const auto drainT0 = std::chrono::steady_clock::now();
    uint64_t admitNsAcc = 0;
    // What this frame has committed to: measured admit() time plus the flush share the chunks it
    // admitted will cost later. See kAdmitBudgetMs for why admit() time alone is not enough.
    double committedMs = 0.0;
    std::vector<projv::core::ivec3> admittedRefines;
    std::vector<projv::ChunkHandle> admittedRefineHandles;
    // Edit results thrown away because a later sphere landed on the same coord mid-generation. These
    // carry their batch with them: the batch is still waiting on this chunk, so the re-queued item
    // has to report back under the same ID or the sphere would never complete.
    struct EditRequeue {
        projv::core::ivec3 coord;
        projv::ChunkHandle handle;
        uint64_t batchID;
        std::chrono::steady_clock::time_point requestedAt;
    };
    std::vector<EditRequeue> requeueEdits;
    // Buried chunks that came back empty from a fine ring and need re-generating at kBuriedLOD,
    // where the fill actually runs to the world floor. See admit().
    std::vector<projv::core::ivec3> buriedRegenCoords;

    // Admission logic for brand-new streaming results (refines are handled separately below,
    // since the chunk already exists and is never evicted). Returns true if the chunk was newly
    // admitted into the scene (counts toward the "generated" budget this function reports back).
    auto admit = [&](ProcessedChunk&& proc) -> bool {
        // A "known empty" sentinel does NOT block admission: this result may be a finer-ring
        // re-check of exactly that sentinel (see applyLODRing), and finding geometry this time is
        // the whole point. Only a real resident chunk blocks.
        auto itActive = ts.activeChunks.find(proc.coord);
        if (itActive != ts.activeChunks.end() && itActive->second != kInvalidChunkHandle)
            return false;

        // The camera can have moved since this was queued. Eviction removed the coord from
        // activeChunks, so without this it would be admitted right back and then evicted again
        // on the next camera step.
        int ddx = proc.coord.x - ts.lastCameraChunk.x;
        int ddz = proc.coord.z - ts.lastCameraChunk.z;
        const int evictR = TerrainState::kViewRadius + TerrainState::kEvictHysteresis;
        if (ddx * ddx + ddz * ddz > evictR * evictR) return false;

        projv::SceneGrid& grid = scene.grids[ts.gridIndex];
        int lin = chunkCoordToLin(proc.coord, grid);
        if (lin < 0 || lin >= static_cast<int>(grid.cellToChunk.size())) {
            projv::core::ivec3 cell = proc.coord - grid.originCellCoord;
            projv::utils::expandGridToInclude(grid, cell, scene, ts.gridIndex);
            lin = chunkCoordToLin(proc.coord, grid);
        }
        if (grid.cellToChunk[lin] >= 0) return false;

        if (proc.empty) {
            // A buried chunk that came back empty was generated at LOD0, where the fill is still
            // only the visible shell -- so "empty" here means "solid rock too far under the surface
            // for the shell to reach", which is the opposite of nothing. Sentinelling it would leave
            // a hole in the underground that nothing revisits, because the empty re-check only fires
            // for coords a FINER ring might rescue and this one needs a coarser one. Mark it buried
            // and let the LOD sweep re-request it at kBuriedLOD, where the fill runs to the floor.
            if (proc.buried && TerrainState::kSolidFillToWorldBottom &&
                proc.lod < TerrainState::kBuriedLOD) {
                ts.buriedCoords.insert(proc.coord);
                ts.activeChunks.erase(proc.coord);
                ts.emptyChunks.erase(proc.coord);
                buriedRegenCoords.push_back(proc.coord);
                return false;
            }
            // Record WHICH ring decided this was empty, so a later, finer ring can overrule it.
            ts.activeChunks[proc.coord] = kInvalidChunkHandle;
            ts.emptyChunks[proc.coord] = TerrainState::EmptyRecord{proc.lod, proc.marginal};
            if (proc.buried) ts.buriedCoords.insert(proc.coord);
            return false;
        }
        // Non-empty: any previous empty verdict for this coord is superseded.
        ts.emptyChunks.erase(proc.coord);
        // Record before requestChunkLOD below, which asks desiredLODClamped what this chunk wants
        // and would get the unclamped answer for a buried chunk it does not yet know is buried --
        // queueing a refine to LOD0 that the next sweep would immediately undo.
        if (proc.buried) ts.buriedCoords.insert(proc.coord);
        else             ts.buriedCoords.erase(proc.coord);

        // Monotonic: activeChunks.size() repeats once eviction starts removing entries.
        proc.chunk.header.chunkID = int(ts.nextChunkID++);
        proc.chunk.gridIndex       = ts.gridIndex;
        proc.chunk.cellIndex       = lin;
        proc.chunk.componentHandle = ts.gridCompHandle;
        proc.chunk.alive           = true;

        projv::internChunkGeometry(scene, proc.chunk);

        // Material IDs are already in final global-palette slots (interned during generation via
        // the thread-safe internMaterial) -- just transfer them to the blob.
        if (proc.chunk.geometryPoolIndex >= 0 &&
            static_cast<size_t>(proc.chunk.geometryPoolIndex) < scene.geometryPool.size()) {
            projv::GeometryBlob& blob = scene.geometryPool[proc.chunk.geometryPoolIndex];
            blob.materialIDs = std::move(proc.materialIDs);
            blob.dirty = true;
        }

        // Reuse a slot freed by eviction when one is available, so Scene.chunks (and with it the
        // GPU header texture) stays bounded over a long flight. A recycled slot sits below
        // gpuData.uploadedChunkCount, so updateDirtyHeaders would not consider it new —
        // headerDirty is what makes it rewrite the row.
        proc.chunk.headerDirty = true;
        int32_t poolIdx = proc.chunk.geometryPoolIndex;   // read before the move
        projv::core::ivec3 coord = proc.coord;
        projv::ChunkHandle h;
        if (!ts.freeChunkSlots.empty()) {
            h = ts.freeChunkSlots.back();
            ts.freeChunkSlots.pop_back();
            scene.chunks[h] = std::move(proc.chunk);
        } else {
            h = projv::ChunkHandle(scene.chunks.size());
            scene.chunks.push_back(std::move(proc.chunk));
        }
        grid.cellToChunk[lin] = static_cast<int32_t>(h);
        ts.activeChunks[coord] = h;

        // Pick the LOD now that the chunk has a handle and is resident in scene.chunks. The
        // camera can have drifted since this chunk was enqueued, so desired may be coarser than
        // what was actually generated (native) -- requestChunkLOD truncates for that via the
        // normal GPU-upload path. It can also want something finer than native if the camera got
        // closer during generation; that comes back as NeedsRegeneration and is left for
        // applyLODRing to pick up as a refine candidate.
        //
        // That hand-off is why NeedsRegeneration must arm lodSweepNeeded. applyLODRing no longer
        // sweeps unconditionally every frame, so "a later frame will notice" is only true if
        // something says so -- otherwise a chunk admitted coarser than the camera now wants would
        // sit at the wrong resolution until the camera happened to cross a chunk boundary.
        if (projv::requestChunkLOD(scene, h,
                TerrainState::resolutionForLOD(desiredLODClamped(ts, coord, ts.lastCameraChunk)))
            == projv::ChunkLODResult::NeedsRegeneration) {
            // Queue the refine directly — the sweep's cursor may have passed this coord's
            // position, and the sweep's own pass-complete gate could close before the cursor
            // wraps around, leaving this chunk stuck at coarse resolution forever.
            admittedRefines.push_back(coord);
            admittedRefineHandles.push_back(h);
        }

        // Every 256th, not every one: this fires up to kMaxNewPerFrame times per frame, and each
        // call is a synchronous formatted write to the console sink from the render thread.
        // Unthrottled it was ~10,000 lines a minute of terminal I/O on the critical path.
        core_info_every(256, "[GEN] coord=({},{},{}) h={} pool={} paletteSize={}",
                        coord.x, coord.y, coord.z, h, poolIdx,
                        scene.components[ts.gridCompHandle].materialPalette.size());
        return true;
    };

    // Coords whose result was consumed this frame, so they are no longer "in the pipeline" and the
    // pending sweep is free to consider them again. Collected here rather than erased inline
    // because g_worker.queuedCoords is guarded by workMutex and this block holds resultMutex --
    // taking workMutex here would invert the workMutex -> resultMutex order terrainWorkerFunc uses
    // and deadlock. The erase happens in the workMutex block below.
    // What an edit's end-to-end latency actually decomposed into, logged on install. Kept because
    // the three terms move independently and only one of them is worth optimising at a time: `wait`
    // is worker-pool contention, `gen` is the chunk generation itself, and `frame` is how long the
    // finished result sat waiting for this function to run.
    auto reportEditLatency = [](const ProcessedChunk& proc, const char* how) {
        if (!proc.isEdit) return;
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        const auto now = std::chrono::steady_clock::now();
        projv::core::info("[EDIT-LAT] {} coord=({},{},{}) wait={:.1f}ms gen={:.1f}ms frame={:.1f}ms total={:.1f}ms",
                          how, proc.coord.x, proc.coord.y, proc.coord.z,
                          ms(proc.requestedAt, proc.startedAt),
                          ms(proc.startedAt, proc.finishedAt),
                          ms(proc.finishedAt, now),
                          ms(proc.requestedAt, now));
    };

    // Install one refine result onto the handle it was generated for. Shared by the immediate path
    // (ordinary LOD refines) and the batched path (edits, held until the whole sphere is ready), so
    // the checks that have to happen at INSTALL time rather than at drain time -- the chunk can be
    // evicted while its siblings are still generating -- live here and are applied to both.
    // Returns true if anything reached the scene and so needs flushing.
    auto installRefine = [&](ProcessedChunk&& proc) -> bool {
        // Stale if the camera moved far enough away that evictDistantChunks already reclaimed this
        // coord/handle in the meantime (possibly recycling the slot for a different chunk entirely
        // via freeChunkSlots) -- discard rather than clobber whatever's there now.
        auto it = ts.activeChunks.find(proc.coord);
        if (it == ts.activeChunks.end() || it->second != proc.refineHandle) return false;

        // A refine CAN legitimately come back empty, and when it does the finer ring's answer is
        // the authoritative one -- so record it instead of dropping it on the floor.
        //
        // Why a solid chunk can refine to nothing: the subsurface shell is kFillDepth *voxels*
        // deep, not a fixed world depth, so it spans 84 world units at LOD0 and 1,344 at LOD2
        // (16x the voxel size). A chunk buried a few hundred units under the surface is
        // therefore genuinely solid at the coarse ring and genuinely empty at the fine one.
        // Neither ring is wrong; the fine one wins because it is the one the camera is close
        // enough to see. This is the same "empty is a property of the coord AT A GIVEN RING"
        // rule TerrainState::emptyChunks already encodes, just arriving from the opposite
        // direction, so it is recorded in exactly the same place.
        //
        // Recording it is also what stops this from looping. Discarding the result and leaving
        // the chunk resident at its coarse resolution (what this line used to do, on the
        // assumption the case "shouldn't happen") left the coord in applyLODRing's candidate set
        // unchanged, so the very next sweep queued the identical refine again -- forever.
        // Measured on a two-minute flight: 922 distinct coords regenerated up to several hundred
        // times each, saturating the whole kMaxRefinesPerFrame budget indefinitely and starving
        // every chunk that genuinely could have been refined. That is the "voxels stop refining
        // after a few minutes" symptom -- the budget was never idle, it was spent entirely on
        // work whose result was thrown away.
        if (proc.empty) {
            releaseChunk(scene, ts, proc.refineHandle);
            it->second = kInvalidChunkHandle;
            ts.emptyChunks[proc.coord] = TerrainState::EmptyRecord{proc.lod, proc.marginal};
            core_info_every(64, "[REFINE-EMPTY] coord=({},{},{}) h={} empty at lod={} marginal={}",
                            proc.coord.x, proc.coord.y, proc.coord.z,
                            proc.refineHandle, proc.lod, proc.marginal);
            return true;   // grid cell + blob release still have to reach the GPU this frame
        }

        if (proc.buried) ts.buriedCoords.insert(proc.coord);
        else             ts.buriedCoords.erase(proc.coord);
        projv::replaceChunkGeometry(scene, proc.refineHandle,
                                    std::move(proc.chunk.geometryData),
                                    std::move(proc.materialIDs),
                                    proc.chunk.header.resolution);
        core_info_every(64, "[REFINE] coord=({},{},{}) h={} resolution={}",
                        proc.coord.x, proc.coord.y, proc.coord.z,
                        proc.refineHandle, proc.chunk.header.resolution);
        return true;
    };

    // Park a finished edit chunk until the rest of its sphere catches up. The batch may have been
    // retired already (a late duplicate from a stale re-queue), in which case there is nothing left
    // waiting on it and it installs immediately.
    auto stageEdit = [&](ProcessedChunk&& proc) -> bool {
        auto it = ts.pendingEditBatches.find(proc.editBatchID);
        if (it == ts.pendingEditBatches.end()) return false;
        it->second.staged.push_back(std::move(proc));
        return true;
    };

    std::vector<projv::core::ivec3> consumedCoords;
    {
        std::lock_guard<std::mutex> lock(g_worker.resultMutex);
        // Edit results that have to be admitted as new chunks, drained first and without the
        // budgets the ordinary loop below is subject to. See WorkerState::readyEdits.
        while (!g_worker.readyEdits.empty()) {
            ProcessedChunk proc = std::move(g_worker.readyEdits.back());
            g_worker.readyEdits.pop_back();
            ts.revisitingEmpty.erase(proc.coord);
            consumedCoords.push_back(proc.coord);
            if (editVersionForChunk(proc.coord) != proc.editVersion) {
                requeueEdits.push_back({proc.coord, kInvalidChunkHandle,
                                        proc.editBatchID, proc.requestedAt});
                continue;
            }
            if (stageEdit(std::move(proc))) continue;
            reportEditLatency(proc, "admit");
            if (admit(std::move(proc))) generated++;
        }
        // Refines: install directly onto the existing handle via replaceChunkGeometry -- the chunk
        // was never evicted, so this is not "admission." Uncapped: rare (bounded by
        // kMaxRefinesPerFrame at request time in applyLODRing), so it can't flood this loop.
        while (!g_worker.readyRefines.empty()) {
            ProcessedChunk proc = std::move(g_worker.readyRefines.back());
            g_worker.readyRefines.pop_back();
            ts.refiningHandles.erase(proc.refineHandle);
            consumedCoords.push_back(proc.coord);
            // The chunk's resolution just changed under applyLODRing's feet, so its recorded
            // requestedLOD needs re-deriving against the new native resolution -- including when the
            // sweep in progress has already scanned past this coord, hence the stamped arm.
            ts.armLodSweep();

            // An edit landed on this coord while it was being generated, so this result predates it.
            // Installing it would put un-edited terrain back on screen with nothing scheduled to
            // correct it -- placeSphereEdit already queued its own refine for this handle, but that
            // one may have been the request this result answers. Re-queue rather than assume.
            if (editVersionForChunk(proc.coord) != proc.editVersion) {
                if (proc.isEdit) {
                    requeueEdits.push_back({proc.coord, proc.refineHandle,
                                            proc.editBatchID, proc.requestedAt});
                } else {
                    admittedRefines.push_back(proc.coord);
                    admittedRefineHandles.push_back(proc.refineHandle);
                }
                continue;
            }

            if (stageEdit(std::move(proc))) continue;
            reportEditLatency(proc, "refine");
            if (installRefine(std::move(proc))) generated++;
        }
        while (!g_worker.readyChunks.empty() && generated < TerrainState::kMaxNewPerFrame &&
               consumed < kMaxConsumedPerFrame && committedMs < TerrainState::kAdmitBudgetMs) {
            ProcessedChunk proc = std::move(g_worker.readyChunks.back());
            g_worker.readyChunks.pop_back();
            consumed++;
            // Clear the in-flight marker whatever the verdict is (admitted, still empty, or
            // rejected as stale) -- admit() records the new empty ring on its own, and leaving the
            // marker set would freeze this coord out of every future re-check.
            ts.revisitingEmpty.erase(proc.coord);
            consumedCoords.push_back(proc.coord);
            // Same mid-generation edit race as the refine loop above, on the admission path.
            if (editVersionForChunk(proc.coord) != proc.editVersion) {
                requeueEdits.push_back({proc.coord, kInvalidChunkHandle,
                                        proc.editBatchID, proc.requestedAt});
                continue;
            }
            // Timed individually rather than as one span around the loop: the loop also does the
            // stale-edit and empty-sentinel work above, and only what admit() itself costs scales
            // with the admission cap.
            const auto admitT0 = std::chrono::steady_clock::now();
            const bool admitted = admit(std::move(proc));
            const uint64_t admitNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - admitT0).count());
            admitNsAcc += admitNs;
            committedMs += double(admitNs) / 1e6;
            // Only an ADMITTED chunk dirties a blob, so only it costs flush time. Results rejected
            // as stale or recorded as empty cost admit() time and nothing more.
            if (admitted) { generated++; committedMs += TerrainState::kMarginalAdmitMs; }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_worker.workMutex);

        for (const projv::core::ivec3& c : consumedCoords) g_worker.queuedCoords.erase(c);

        // Refines queued by admission (chunks generated at coarser native resolution than the
        // camera now wants). Same path as applyLODRing's toRefine: push_front with jumpQueue,
        // insert refiningHandles so applyLODRing doesn't re-trigger, insert queuedCoords so
        // the pending sweep doesn't re-queue the coord as ordinary work.
        {
            projv::SceneGrid& grid = scene.grids[ts.gridIndex];
            for (size_t i = 0; i < admittedRefines.size(); i++) {
                projv::core::ivec3 coord = admittedRefines[i];
                projv::ChunkHandle h = admittedRefineHandles[i];
                g_worker.queuedCoords.insert(coord);
                ts.refiningHandles.insert(h);
                projv::core::vec3 wp = grid.origin + (projv::core::vec3(coord - grid.originCellCoord) * TerrainState::kChunkSize);
                g_worker.workQueue.push_front(ChunkWorkItem{coord, wp, desiredLODClamped(ts, coord, ts.lastCameraChunk),
                                                           /*refineHandle=*/h, /*jumpQueue=*/true});
            }
            admittedRefines.clear();
            admittedRefineHandles.clear();

            // Edit results dropped for a mid-generation edit, straight back onto the edit lane and
            // still tagged with their batch, which is what lets the sphere they belong to complete.
            for (const EditRequeue& r : requeueEdits) {
                g_worker.queuedCoords.insert(r.coord);
                if (r.handle != kInvalidChunkHandle) ts.refiningHandles.insert(r.handle);
                projv::core::vec3 wp = grid.origin + (projv::core::vec3(r.coord - grid.originCellCoord) * TerrainState::kChunkSize);
                uint32_t lod = desiredLODClamped(ts, r.coord, ts.lastCameraChunk);
                if (r.handle != kInvalidChunkHandle && r.handle < scene.chunks.size())
                    lod = std::min(lod, nativeLODFromResolution(scene.chunks[r.handle].header.resolution));
                g_worker.editQueue.push_back(ChunkWorkItem{r.coord, wp, lod, r.handle,
                                                          /*jumpQueue=*/true, /*isEdit=*/true,
                                                          r.requestedAt, r.batchID});
            }
            if (!requeueEdits.empty()) g_worker.editCv.notify_all();
            requeueEdits.clear();

            // Buried chunks re-requested at the coarse ring that can actually fill them. Ordinary
            // queue, not the edit lane -- this is streaming, nobody is waiting on it.
            for (const projv::core::ivec3& coord : buriedRegenCoords) {
                if (g_worker.queuedCoords.count(coord)) continue;
                g_worker.queuedCoords.insert(coord);
                projv::core::vec3 wp = grid.origin + (projv::core::vec3(coord - grid.originCellCoord) * TerrainState::kChunkSize);
                g_worker.workQueue.push_back(ChunkWorkItem{coord, wp,
                                                          desiredLODClamped(ts, coord, ts.lastCameraChunk)});
            }
            buriedRegenCoords.clear();
        }


        // Three caps, doing three different jobs:
        //
        //   kMaxScanPerFrame  bounds how many ring coords are EXAMINED. This is the one that
        //                     actually bounds the frame, and the one that was missing: the vast
        //                     majority of coords in a populated ring are skipped (already resident,
        //                     or already queued) without ever counting against a push budget, so a
        //                     push-only cap does not bound anything. With the cursor rewound to 0
        //                     on every chunk-boundary crossing, one frame could walk all ~684,000
        //                     coords looking for the handful that are new.
        //   kMaxNewWorkPerFrame  bounds how many items one frame may enqueue.
        //   kMaxWorkQueueDepth   bounds how far ahead of the camera the queue may run.
        //
        // Not finishing a pass is fine and expected while moving fast. xzOffsets is nearest-first,
        // so a truncated pass is exactly the near terrain that matters, and the outer rings are
        // picked up on later frames or once the camera slows.
        static constexpr size_t kMaxScanPerFrame = 16384;
        static constexpr size_t kMaxNewWorkPerFrame = 512;
        size_t newCount = 0, scanned = 0;
        while (ts.pendingIndex < ts.pendingTotal() && scanned < kMaxScanPerFrame &&
               newCount < kMaxNewWorkPerFrame &&
               g_worker.workQueue.size() < TerrainState::kMaxWorkQueueDepth) {
            projv::core::ivec3 c = ts.pendingCoordAt(ts.pendingIndex++);
            scanned++;
            // Cheap rejects first. Both of these hit for nearly every coord in a populated ring,
            // and neither needs the grid, so doing the grid lookup (and possibly a grid
            // reallocation, via expandGridToInclude) ahead of them was paying the expensive part of
            // the iteration for coords that were about to be thrown away.
            if (ts.activeChunks.count(c)) continue;
            if (g_worker.queuedCoords.count(c)) continue;

            projv::SceneGrid& grid = scene.grids[ts.gridIndex];
            int lin = chunkCoordToLin(c, grid);
            if (lin < 0 || lin >= static_cast<int>(grid.cellToChunk.size())) {
                projv::core::ivec3 cell = c - grid.originCellCoord;
                projv::utils::expandGridToInclude(grid, cell, scene, ts.gridIndex);
            }
            projv::core::vec3 wp = grid.origin + (projv::core::vec3(c - grid.originCellCoord) * TerrainState::kChunkSize);
            uint32_t lod = desiredLODClamped(ts, c, ts.lastCameraChunk);
            g_worker.workQueue.push_back(ChunkWorkItem{c, wp, lod});
            g_worker.queuedCoords.insert(c);
            newCount++;
        }
        if (newCount > 0) g_worker.workCv.notify_all();
    }

    // Install completed sphere edits, whole. A batch goes in only once every chunk it spans has
    // reported back, so all of them reach the scene in this frame and ride out on the single
    // flushSceneUpdates the caller does -- which is what makes the sphere appear as one object
    // instead of assembling itself chunk by chunk over the spread between its fastest and slowest
    // chunk. The timeout is a backstop, not a normal path: every queued item produces a result, so
    // a batch only fails to complete if something dropped one on the floor.
    if (!ts.pendingEditBatches.empty()) {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = ts.pendingEditBatches.begin(); it != ts.pendingEditBatches.end(); ) {
            TerrainState::EditBatch& batch = it->second;
            const bool complete = batch.staged.size() >= batch.expected;
            const bool timedOut =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - batch.requestedAt).count()
                    > TerrainState::kEditBatchTimeoutMs;
            if (!complete && !timedOut) { ++it; continue; }
            if (timedOut && !complete) {
                projv::core::warn("[EDIT] batch {} timed out with {}/{} chunks -- installing partial",
                                  it->first, batch.staged.size(), batch.expected);
            }
            double worstMs = 0.0;
            for (ProcessedChunk& proc : batch.staged) {
                worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(
                                                proc.finishedAt - proc.requestedAt).count());
                const bool isRefine = proc.refineHandle != kInvalidChunkHandle;
                reportEditLatency(proc, isRefine ? "refine" : "admit");
                if (isRefine) { if (installRefine(std::move(proc))) generated++; }
                else          { if (admit(std::move(proc)))         generated++; }
            }
            projv::core::info("[EDIT] batch {} installed {} chunks in one frame "
                              "(slowest chunk {:.1f}ms, total {:.1f}ms)",
                              it->first, batch.staged.size(), worstMs,
                              std::chrono::duration<double, std::milli>(now - batch.requestedAt).count());
            it = ts.pendingEditBatches.erase(it);
        }
    }

    // Only frames that actually admitted something are averaged: a frame that drained nothing tells
    // us nothing about per-chunk cost, and folding those in would divide the totals by mostly-idle
    // frames and understate it.
    g_admit.lastFrameChunks = generated;
    if (generated > 0) {
        const uint64_t drainNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - drainT0).count());
        g_admit.admitNs.fetch_add(admitNsAcc, std::memory_order_relaxed);
        g_admit.drainNs.fetch_add(drainNs, std::memory_order_relaxed);
        g_admit.chunks.fetch_add(uint64_t(generated), std::memory_order_relaxed);
        g_admit.busyFrames.fetch_add(1, std::memory_order_relaxed);
        // Which limit actually stopped the drain. If neither fires the pool simply had nothing more
        // ready, meaning admission is no longer the constraint -- which is the goal state.
        if (committedMs >= TerrainState::kAdmitBudgetMs)
            g_admit.budgetFrames.fetch_add(1, std::memory_order_relaxed);
        if (generated >= TerrainState::kMaxNewPerFrame)
            g_admit.capFrames.fetch_add(1, std::memory_order_relaxed);
    }
    return generated > 0;
}

// =============================================================================
// Application stages
// =============================================================================

void startup(projv::Application& app) {
    using namespace projv::core;

    std::string& exeDir = projv::core::getGlobalResource<std::string>(app.world);
    fs::current_path(exeDir);

    projv::graphics::RenderInstance& renderInstance =
        projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(1920, 1080, "ProjectV Terrain Generator");
    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetScrollCallback(renderInstance.window, sunScrollCallback);

    projv::Scene& scene    = projv::core::createGlobalResource<projv::Scene>(app.world);
    projv::GPUData& gpuData = projv::core::createGlobalResource<projv::GPUData>(app.world);
    TerrainState& ts        = projv::core::createGlobalResource<TerrainState>(app.world);
    ts.noiseGen.reseed(uint32_t(ts.seed));

    // --- Create terrain grid (centered on world origin, expands dynamically via expandGridToInclude) ---
    int r = ts.kViewRadius;
    int yMin = TerrainState::kChunkYMin, yMax = TerrainState::kChunkYMax;

    projv::SceneGrid grid;
    grid.origin = vec3(float(-r) * TerrainState::kChunkSize,
                       float(yMin) * TerrainState::kChunkSize,
                       float(-r) * TerrainState::kChunkSize);
    grid.cellSize = TerrainState::kChunkSize;
    grid.dims = ivec3(2 * r + 1, yMax - yMin + 1, 2 * r + 1);
    grid.rotation = quat(1, 0, 0, 0);
    grid.cellToChunk.assign(static_cast<size_t>(grid.dims.x) * grid.dims.y * grid.dims.z, -1);
    grid.originCellCoord = ivec3(-r, yMin, -r);

    ts.gridIndex = static_cast<int32_t>(scene.grids.size());
    scene.grids.push_back(std::move(grid));

    // --- Create grid component ---
    projv::ComponentRecord gridComp;
    gridComp.kind = projv::ComponentKind::Grid;
    gridComp.gridIndex = ts.gridIndex;
    gridComp.name = "terrain";
    gridComp.dataRefID = -1;
    int32_t compIdx = static_cast<int32_t>(scene.components.size());
    scene.components.push_back(std::move(gridComp));
    ts.gridCompHandle = projv::ComponentHandle(compIdx);
    scene.grids[ts.gridIndex].componentHandle = ts.gridCompHandle;

    // --- Load the voxelized tree assets and fold their colors into the terrain palette ---
    //
    // Trees become terrain voxels, so they draw from the terrain component's single palette, which
    // is already carrying the kSurfaceLevels^3 surface lattice plus the water shades. Rather than
    // spend the ~34 slots that leaves on a second, tree-only palette, tree colors are snapped onto
    // the *same* lattice the ground uses. That costs no new budget beyond what the static_assert
    // above already guarantees fits (a tree can at worst light up lattice entries the terrain had
    // not used yet), and it keeps foliage at exactly the color fidelity the rest of the world has.
    //
    // This runs before the workers spawn, so every material ID a worker reads off a tree voxel is
    // already resolved and no worker ever interns for a tree.
    {
        ts.treeParams.waterLevel = TerrainState::kWaterLevel;
        // A tree's world size, NOT the terrain grid's voxel size. These were the same number while
        // kVoxelScale was 1.75 and it was tempting to derive one from the other; doubling
        // kVoxelScale showed why that was wrong, because it silently doubled every tree in the
        // world along with it. stampTree already handles a mismatch properly -- it divides the two
        // to pick a mip, so a coarser terrain grid just draws the same tree at lower resolution
        // (see the decim/residual arithmetic there) -- which is exactly the intended behaviour and
        // the reason the parameter exists separately at all.
        ts.treeParams.treeVoxelWorldSize = 1.75f;
        ts.treeParams.seed = uint32_t(ts.seed);

        // Which climate each species claims. The names are the source OBJ's material names, which
        // is all the asset pack gives us, so the bands below are assigned from what each one
        // actually looks like once voxelized: narrow dark conifers to the cold end, broad light
        // canopies to the temperate middle, the flat olive canopy and the leafy shrub to the warm
        // wet end, and the orange autumn broadleaf to a drier cool-temperate band where it reads as
        // seasonal rather than dead.
        //
        // The centres are spread across the range this terrain actually produces on plantable
        // ground -- temperature ~0.1-0.8, humidity ~0.2-0.7 -- not across a nominal 0..1, or the
        // species at the ends would never come up. Widths are wider than the ~0.08 spacing between
        // centres so neighbouring bands interleave at their edges: a stand picks up strays from the
        // next zone over instead of the map banding into eight hard stripes.
        const std::string treeFolder = "../MeshVoxelizer/trees";
        const std::vector<trees::TreeSpecies> treeSpecies = {
            // name        temp c/w      humid c/w     abundance
            {"Bark___1",   0.20f, 0.13f, 0.45f, 0.18f, 1.20f},  // thin spire conifer — coldest
            {"Bark___0",   0.30f, 0.13f, 0.46f, 0.18f, 1.10f},  // narrow green spire — subalpine
            {"Bottom_T",   0.39f, 0.13f, 0.50f, 0.18f, 1.00f},  // tall dark conifer — cool
            {"Walnut_L",   0.47f, 0.11f, 0.42f, 0.15f, 0.85f},  // orange autumn broadleaf — drier
            {"Bark___S",   0.54f, 0.13f, 0.50f, 0.18f, 1.00f},  // spiky green — temperate
            {"Mossy_Tr",   0.60f, 0.13f, 0.60f, 0.17f, 1.15f},  // broad light canopy — temperate wet
            {"Sonnerat",   0.69f, 0.13f, 0.56f, 0.17f, 1.10f},  // flat olive canopy — warm
            {"Oak_Leav",   0.75f, 0.13f, 0.62f, 0.17f, 0.90f},  // leafy shrub — warmest, wettest
        };
        projv::ComponentRecord& terrainComp = scene.components[ts.gridCompHandle];
        if (trees::loadTreeLibrary(ts.treeLib, treeFolder, treeSpecies)) {
            std::unordered_map<uint32_t, uint8_t> treeColorCache;
            trees::resolveMaterials(ts.treeLib, [&](uint32_t packedColor) -> uint8_t {
                projv::Color c = projv::unpackColor(packedColor);
                projv::Color snapped{quantizeSurfaceChannel(float(c.r) / 255.0f),
                                     quantizeSurfaceChannel(float(c.g) / 255.0f),
                                     quantizeSurfaceChannel(float(c.b) / 255.0f)};
                uint32_t key = projv::packColor(snapped);
                auto it = treeColorCache.find(key);
                if (it != treeColorCache.end()) return it->second;
                uint8_t id = projv::utils::internMaterial(scene, terrainComp, "", key);
                treeColorCache.emplace(key, id);
                return id;
            });
            projv::core::info("[TREES] {} species loaded; terrain palette now {} entries",
                              ts.treeLib.assets.size(), terrainComp.materialPalette.size());
        } else {
            projv::core::warn("[TREES] no tree assets found under '{}' - terrain will be bare. "
                              "Generate them with: cd ../MeshVoxelizer && make && "
                              "./mesh_voxelizer -f <model> -o trees/<name> -r 64",
                              treeFolder);
        }
    }

    // Create GPU textures (scene is empty — grid exists but no chunks yet)
    gpuData = projv::graphics::createTexturesForScene(scene);

    // --- Set up renderer ---
    projv::RendererSpecification spec = projv::graphics::loadRendererSpecification("./worldCascadeRenderer/");
    renderInstance.addRendererSpecification(1, spec);
    bgfx::ShaderHandle vsh = projv::graphics::loadShader("./worldCascadeRenderer/pathTracerShaders/vs_quad.bin");
    auto cr = projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vsh);
    renderInstance.setActiveRenderer(cr);

    // Load blue-noise LUT (used by the path tracer for sample decorrelation).
    {
        int width, height, channels;
        unsigned char* img = stbi_load("LDR_RGBA_7.png", &width, &height, &channels, 4);
        projv::core::info("Blue-noise texture: {}x{}", width, height);
        projv::graphics::setTextureToData(cr, 1, img, width, height);
    }

    // Start worker threads — leave 2 cores for the render thread and main thread
    int numWorkers = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 2);
    projv::core::info("[WORKER] spawning {} terrain generation threads", numWorkers);
    g_worker.numThreads = numWorkers;
    for (int i = 0; i < numWorkers; ++i) {
        g_worker.threads.emplace_back(terrainWorkerFunc, std::ref(scene), std::ref(ts));
    }
    // Deliberately NOT subtracted from numWorkers: these are blocked on editCv except in the
    // fraction of a second after a key press, so counting them against the streaming pool would
    // give up a core permanently to buy latency that is only ever needed momentarily.
    projv::core::info("[WORKER] spawning {} dedicated edit threads", WorkerState::kEditWorkers);
    for (int i = 0; i < WorkerState::kEditWorkers; ++i) {
        g_worker.editThreads.emplace_back(terrainEditWorkerFunc, std::ref(scene), std::ref(ts));
    }

    // Build the camera-relative streaming template ONCE. Every later "re-seed the ring around the
    // new camera chunk" is just pendingIndex = 0 -- see TerrainState::xzOffsets.
    ts.xzOffsets.reserve(size_t(3.15 * r * r) + 4 * r);
    for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx)
            if (dx * dx + dz * dz <= r * r)
                ts.xzOffsets.push_back(ivec2(dx, dz));
    std::sort(ts.xzOffsets.begin(), ts.xzOffsets.end(),
        [](const ivec2& a, const ivec2& b) {
            return a.x * a.x + a.y * a.y < b.x * b.x + b.y * b.y;
        });
    projv::core::info("[STREAM] ring template: {} XZ offsets x {} Y levels = {} coords",
                      ts.xzOffsets.size(), TerrainState::kYLevels, ts.pendingTotal());

    ts.lastCameraChunk = ivec3(-9999, -9999, -9999); // force first-frame reseed
    ts.pendingOrigin = ivec3(0, 0, 0);
    ts.pendingIndex = 0;
}

void update(projv::Application& app) {
    using namespace projv::core;

    projv::Scene& scene    = projv::core::getGlobalResource<projv::Scene>(app.world);
    projv::GPUData& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    TerrainState& ts        = projv::core::getGlobalResource<TerrainState>(app.world);
    CameraState& cam        = projv::core::getGlobalResource<CameraState>(app.world);
    projv::graphics::RenderInstance& ri = projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);

    // --- Mouse/keyboard input ---
    static bool mouseCaptured = true;
    static double lastMouseX = 0, lastMouseY = 0;
    static bool mouseInit = false;

    if (mouseCaptured && glfwGetKey(ri.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetInputMode(ri.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL); mouseCaptured = false;
    } else if (!mouseCaptured && glfwGetMouseButton(ri.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        glfwSetInputMode(ri.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED); mouseCaptured = true;
    }

    double mx, my; glfwGetCursorPos(ri.window, &mx, &my);
    if (!mouseInit || !mouseCaptured) { lastMouseX = mx; lastMouseY = my; mouseInit = true; }
    float mouseDX = float(mx - lastMouseX), mouseDY = float(my - lastMouseY);
    lastMouseX = mx; lastMouseY = my;

    const float sens = 0.0025f;
    if (mouseCaptured && (mouseDX || mouseDY)) {
        cam.phi += mouseDX * sens;
        cam.pitch -= mouseDY * sens;
        if (cam.pitch > 1.55f) cam.pitch = 1.55f;
        if (cam.pitch < -1.55f) cam.pitch = -1.55f;
    }

    // Fly speed is per SECOND, scaled by the frame's delta time. It used to be per FRAME, which
    // made the camera's actual world velocity a function of the framerate -- the same key held for
    // the same wall-clock second moved you ten times further at 130fps than at 13fps. That
    // couples "the renderer got slower" to "the world got bigger", and it is half of what makes a
    // dropped frame feel like a lurch rather than a hitch. Clamped so a long stall (a texture
    // resize, an alt-tab) does not teleport the camera across the world on the recovery frame.
    static auto lastUpdateTime = std::chrono::high_resolution_clock::now();
    auto nowUpdate = std::chrono::high_resolution_clock::now();
    float dt = std::chrono::duration<float>(nowUpdate - lastUpdateTime).count();
    lastUpdateTime = nowUpdate;
    if (dt > 0.1f) dt = 0.1f;

    // Benchmark mode: fly forward at boost speed with no input, so the streaming path can be
    // measured reproducibly. A stationary camera exercises almost none of it -- eviction, the ring
    // re-seed, LOD transitions and refines are all camera-motion driven, and those are exactly
    // where frame-time spikes live. TERRAIN_AUTOFLY=1 to enable.
    static const bool kAutoFly = [] {
        const char* e = std::getenv("TERRAIN_AUTOFLY");
        return e && e[0] != '\0' && std::strcmp(e, "0") != 0;
    }();

    // --- Camera mode toggle (press 1 = fly, 2 = player+gravity+collision) ---
    static bool wasKey1 = false, wasKey2 = false;
    bool key1 = glfwGetKey(ri.window, GLFW_KEY_1) == GLFW_PRESS;
    bool key2 = glfwGetKey(ri.window, GLFW_KEY_2) == GLFW_PRESS;
    if (key1 && !wasKey1 && cam.playerMode) { cam.playerMode = false; cam.playerVelocityY = 0.0f; }
    if (key2 && !wasKey2 && !cam.playerMode) cam.playerMode = true;
    wasKey1 = key1; wasKey2 = key2;

    vec3 forward{cos(cam.phi), 0, sin(cam.phi)};

    if (cam.playerMode) {
        // Player mode: gravity, terrain collision, WASD on XZ plane, Space to jump
        static constexpr float kGravity       = -980.0f;      // units/s^2
        static constexpr float kJumpVelocity  =  260.0f;      // initial upward speed on jump
        static constexpr float kEyeHeight     =   40.0f;      // camera height above surface
        static constexpr float kWalkSpeed     =   80.0f;      // world units per second

        float groundH = sampleWorld(ts, cam.position.x, cam.position.z, /*detailOct=*/3, /*erosionOct=*/6).height;
        float targetY = groundH + kEyeHeight;

        if (cam.position.y > targetY + 1.0f) {
            cam.playerVelocityY += kGravity * dt;
            cam.position.y += cam.playerVelocityY * dt;
            if (cam.position.y <= targetY) {
                cam.position.y = targetY;
                cam.playerVelocityY = 0.0f;
            }
        } else {
            cam.position.y = targetY;
            cam.playerVelocityY = 0.0f;
            if (glfwGetKey(ri.window, GLFW_KEY_SPACE) == GLFW_PRESS) {
                cam.playerVelocityY = kJumpVelocity;
                cam.position.y += 1.0f;
            }
        }

        // Walk on XZ plane
        const float walkSpeed = (glfwGetKey(ri.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? kWalkSpeed * 3.0f : kWalkSpeed) * dt;
        if (glfwGetKey(ri.window, GLFW_KEY_W)) { cam.position.x += forward.x * walkSpeed; cam.position.z += forward.z * walkSpeed; }
        if (glfwGetKey(ri.window, GLFW_KEY_S)) { cam.position.x -= forward.x * walkSpeed; cam.position.z -= forward.z * walkSpeed; }
        if (glfwGetKey(ri.window, GLFW_KEY_A)) { float p = cam.phi + 1.57f; cam.position.x -= cos(p) * walkSpeed; cam.position.z -= sin(p) * walkSpeed; }
        if (glfwGetKey(ri.window, GLFW_KEY_D)) { float p = cam.phi - 1.57f; cam.position.x -= cos(p) * walkSpeed; cam.position.z -= sin(p) * walkSpeed; }
    } else {
        // Fly mode (original)
        const float baseSpeed = 120.0f;
        const float boostSpeed = baseSpeed * 30.0f;
        const float speed = (glfwGetKey(ri.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? boostSpeed : baseSpeed) * dt;
        if (glfwGetKey(ri.window, GLFW_KEY_W)) cam.position += forward * speed;
        if (glfwGetKey(ri.window, GLFW_KEY_S)) cam.position -= forward * speed;
        if (glfwGetKey(ri.window, GLFW_KEY_A)) { float p = cam.phi + 1.57f; cam.position -= vec3{cos(p), 0, sin(p)} * speed; }
        if (glfwGetKey(ri.window, GLFW_KEY_D)) { float p = cam.phi - 1.57f; cam.position -= vec3{cos(p), 0, sin(p)} * speed; }
        if (glfwGetKey(ri.window, GLFW_KEY_R)) cam.position.y += speed;
        if (glfwGetKey(ri.window, GLFW_KEY_F)) cam.position.y -= speed;

        if (kAutoFly) {
            static const float flySpeed = [] {
                const char* e = std::getenv("TERRAIN_AUTOFLY");
                float v = e ? float(atof(e)) : 0.0f;
                return v > 1.0f ? v : 960.0f;
            }();
            static bool armed = false;
            if (!armed && ts.pendingIndex >= ts.pendingTotal()) armed = true;
            static const float kFlyStopAfter = [] {
                const char* e = std::getenv("TERRAIN_AUTOFLY_STOP");
                return e ? float(atof(e)) : 0.0f;
            }();
            static float flownFor = 0.0f;
            if (armed) flownFor += dt;
            if (armed && !(kFlyStopAfter > 0.0f && flownFor > kFlyStopAfter))
                cam.position += forward * (flySpeed * dt);
        }
    }

    // --- Editing: E places a sphere of solid material, Q carves one out, both centred
    // kEditSphereDistanceVoxels ahead of where the camera is looking.
    //
    // Edge-triggered rather than repeating while held: one press re-generates up to eight full-res
    // chunks, which is real worker-pool work, and a held key would queue another batch every frame.
    static bool wasEditAdd = false, wasEditRemove = false;
    const bool editAdd    = glfwGetKey(ri.window, GLFW_KEY_E) == GLFW_PRESS;
    const bool editRemove = glfwGetKey(ri.window, GLFW_KEY_Q) == GLFW_PRESS;
    if ((editAdd && !wasEditAdd) || (editRemove && !wasEditRemove)) {
        const bool isAdd = editAdd && !wasEditAdd;
        vec3 look{cos(cam.pitch) * cos(cam.phi), sin(cam.pitch), cos(cam.pitch) * sin(cam.phi)};
        vec3 center = cam.position + look * (kEditSphereDistanceVoxels * TerrainState::kVoxelScale);
        // Snapped to the surface-colour lattice for the same reason tree colours are (see startup):
        // the terrain palette is a fixed kSurfaceLevels^3 lattice plus water shades, and an
        // off-lattice colour would spend one of the handful of slots that leaves.
        uint32_t packed = projv::packColor(projv::Color{quantizeSurfaceChannel(0.9f),
                                                        quantizeSurfaceChannel(0.15f),
                                                        quantizeSurfaceChannel(0.1f)});
        placeSphereEdit(scene, ts, center,
                        kEditSphereRadiusVoxels * TerrainState::kVoxelScale, isAdd, packed);
    }
    wasEditAdd = editAdd; wasEditRemove = editRemove;

    // Benchmark mode: place an alternating add/remove sphere every kAutoEditPeriod frames, so
    // [EDIT-LAT] can be read off a run with nobody at the keyboard. Same reason TERRAIN_AUTOFLY
    // exists -- edit latency is a function of how loaded the worker pool is, and reproducing a
    // given load by hand is not something a human can do twice. TERRAIN_AUTOEDIT=1 to enable.
    static const bool kAutoEdit = [] {
        const char* e = std::getenv("TERRAIN_AUTOEDIT");
        return e && e[0] != '\0' && std::strcmp(e, "0") != 0;
    }();
    if (kAutoEdit) {
        static constexpr int kAutoEditPeriod = 300;
        if (app.frameCount > 0 && app.frameCount % kAutoEditPeriod == 0) {
            vec3 look{cos(cam.pitch) * cos(cam.phi), sin(cam.pitch), cos(cam.pitch) * sin(cam.phi)};
            vec3 center = cam.position + look * (kEditSphereDistanceVoxels * TerrainState::kVoxelScale);
            uint32_t packed = projv::packColor(projv::Color{quantizeSurfaceChannel(0.9f),
                                                            quantizeSurfaceChannel(0.15f),
                                                            quantizeSurfaceChannel(0.1f)});
            placeSphereEdit(scene, ts, center, kEditSphereRadiusVoxels * TerrainState::kVoxelScale,
                            (app.frameCount / kAutoEditPeriod) % 2 == 1, packed);
        }
    }

    // --- Chunk streaming (X/Z only; generate full vertical columns each time) ---
    ivec3 camChunk = worldToChunkCoord(cam.position);
    if (camChunk.x != ts.lastCameraChunk.x || camChunk.z != ts.lastCameraChunk.z) {
        ts.lastCameraChunk = camChunk;
        ts.evictPending = true;
        // Re-seed the streaming ring around the new camera chunk. The whole re-seed is these two
        // assignments: the ring is a camera-relative template (TerrainState::xzOffsets), so moving
        // it is retargeting the origin, not rebuilding 684,000 coords and sorting them.
        ts.pendingOrigin = camChunk;
        ts.pendingIndex = 0;
        // Every resident chunk's distance to the camera just changed, so its desired LOD may have --
        // including the chunks a sweep already in progress has walked past this pass.
        ts.armLodSweep();
    }
    // Outside the camChunk-changed branch: eviction is budgeted per call, so a crossing that puts
    // more than the budget out of range keeps draining on the frames that follow.
    if (ts.evictPending) evictDistantChunks(scene, ts, camChunk);
    // One flush, after both stages, covering whatever either of them dirtied.
    //
    // applyLODRing MUST be inside the same flush as the streaming work. It used to run after
    // generatePendingChunks' own internal `if (generated > 0) flush`, so an LOD change only reached
    // the GPU on some later frame that happened to admit a chunk -- and since requestChunkLOD
    // latches the new level in Chunk::requestedLOD the moment it applies it, the change was never
    // re-requested and never retried. During initial streaming that was invisible (something is
    // admitted almost every frame, so a flush always followed within a frame or two), but once the
    // surrounding terrain is resident and admissions stop, LOD changes silently stopped taking
    // effect -- worst for the coarse->fine direction, which is a pure renderLOD flip on already-
    // resident geometry and so has no admission of its own to ride along with.
    bool needsFlush = generatePendingChunks(scene, ts);
    // Read before applyLODRing, which does not admit chunks and so must not change the
    // classification of this frame's flush. See AdmitCost.
    const int admittedThisFrame = g_admit.lastFrameChunks;
    needsFlush |= applyLODRing(scene, ts, camChunk);
    if (needsFlush) {
        const auto flushT0 = std::chrono::steady_clock::now();
        projv::graphics::flushSceneUpdates(scene, gpuData);
        const double flushMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - flushT0).count();
        // Every flushing frame feeds the fit, including the zero-admission ones -- those are what
        // pin down the fixed cost.
        g_admit.addFlush(admittedThisFrame, flushMs);
    }

    // Where chunk generation time goes, per storage ring. Reported on the same schedule as the
    // other perf counters; see GenPhases.
    if (app.frameCount % 300 == 0) {
        static const char* kPhaseNames[6] = {"columns", "voxels", "trees", "edits", "tree64", "materials"};
        for (int l = 0; l < 3; ++l) {
            uint64_t n = g_genPhaseCount[l].load(std::memory_order_relaxed);
            if (n == 0) continue;
            double tot = 0, per[6];
            for (int i = 0; i < 6; ++i) {
                per[i] = double(g_genPhaseNs[l][i].load(std::memory_order_relaxed)) / 1e6 / double(n);
                tot += per[i];
            }
            projv::core::perf("[GENPHASE] lod{} n={} avg={:.2f}ms | {}={:.2f} {}={:.2f} {}={:.2f} "
                              "{}={:.2f} {}={:.2f} {}={:.2f}",
                              l, n, tot,
                              kPhaseNames[0], per[0], kPhaseNames[1], per[1], kPhaseNames[2], per[2],
                              kPhaseNames[3], per[3], kPhaseNames[4], per[4], kPhaseNames[5], per[5]);
        }
    }

    // What an admitted chunk costs the main thread, and therefore what the admission cap should be.
    // See AdmitCost. Reported cumulatively rather than windowed: the interesting regime is the long
    // initial fill, and a running average over it is exactly the right summary.
    if (app.frameCount % 300 == 0) {
        const uint64_t n = g_admit.chunks.load(std::memory_order_relaxed);
        const uint64_t busy = g_admit.busyFrames.load(std::memory_order_relaxed);
        if (n > 0 && busy > 0) {
            const double admitPerChunk =
                double(g_admit.admitNs.load(std::memory_order_relaxed)) / 1e6 / double(n);
            const double drainPerFrame =
                double(g_admit.drainNs.load(std::memory_order_relaxed)) / 1e6 / double(busy);
            const double chunksPerFrame = double(n) / double(busy);
            double flushPerChunk = 0.0, flushFixed = 0.0;
            const bool haveFit = g_admit.flushFit(flushPerChunk, flushFixed);
            const double marginal = admitPerChunk + std::max(0.0, flushPerChunk);
            projv::core::perf(
                "[ADMIT] n={} frames={} ({:.1f}/frame) | admit={:.4f}ms/chunk | "
                "flush fit: {:.4f}ms/chunk + {:.2f}ms/frame fixed{} | marginal={:.4f}ms/chunk | "
                "drain={:.2f}ms/frame | stopped on budget {:.0f}% cap {:.0f}% | "
                "{:.0f} chunks/frame fits {:.1f}ms",
                n, busy, chunksPerFrame, admitPerChunk,
                flushPerChunk, flushFixed, haveFit ? "" : " (DEGENERATE)", marginal,
                drainPerFrame,
                100.0 * double(g_admit.budgetFrames.load(std::memory_order_relaxed)) / double(busy),
                100.0 * double(g_admit.capFrames.load(std::memory_order_relaxed)) / double(busy),
                marginal > 1e-9 ? TerrainState::kAdmitBudgetMs / marginal : 0.0,
                TerrainState::kAdmitBudgetMs);
        }

        // Column cache effectiveness. See ColumnCacheStats: `hit` should dominate by ~20:1 if the
        // prepass is being amortized over its vertical stack at all.
        const uint64_t ch = g_colStats.hit.load(std::memory_order_relaxed);
        const uint64_t cm = g_colStats.miss.load(std::memory_order_relaxed);
        const uint64_t cw = g_colStats.wait.load(std::memory_order_relaxed);
        if (ch + cm + cw > 0) {
            const double tot = double(ch + cm + cw);
            projv::core::perf(
                "[COLCACHE] hit={} ({:.1f}%) miss={} ({:.1f}%) wait={} ({:.1f}%) | "
                "avg wait={:.2f}ms, {:.1f}s total blocked across the pool",
                ch, 100.0 * double(ch) / tot, cm, 100.0 * double(cm) / tot,
                cw, 100.0 * double(cw) / tot,
                cw > 0 ? double(g_colStats.waitNs.load(std::memory_order_relaxed)) / 1e6 / double(cw) : 0.0,
                double(g_colStats.waitNs.load(std::memory_order_relaxed)) / 1e9);
        }
    }

    // Diagnostic: log chunk population progress every 60 frames.
    //
    // Off unless TERRAIN_DIAG=1 is in the environment. This block makes four separate full passes
    // over activeChunks (six figures) and geometryPool, which is a real cost even amortised over 60
    // frames -- and it lands as one long frame every second, which is precisely the periodic hitch
    // this pass is trying to remove. It is genuinely useful when tuning the LOD rings and VRAM
    // budget, so it stays available rather than being deleted; it just is not on by default.
    static const bool kDiagEnabled = [] {
        const char* e = std::getenv("TERRAIN_DIAG");
        return e && e[0] == '1';
    }();
    if (kDiagEnabled && app.frameCount % 60 == 0) {
        int filled = 0;
        for (const auto& [coord, h] : ts.activeChunks)
            if (h != kInvalidChunkHandle) filled++;
        // Storage-LOD effect: how many live blobs sit at each LOD, the full-res node count they
        // would cost, and the geometry texels actually resident. residentNodes / fullResNodes is
        // the VRAM saving the rings are buying.
        //
        // blob.renderLOD is "levels dropped below the chunk's own generated (native) resolution",
        // not the absolute displayed ring -- chunks are generated directly at their ring's native
        // resolution now, so renderLOD is usually 0. Recover the effective ring as
        // native + renderLOD (clamped to 2) via each blob's owning chunk's header.resolution.
        std::vector<uint32_t> nativeLODByBlob(scene.geometryPool.size(), 0u);
        for (const auto& c : scene.chunks)
            if (c.alive && c.geometryPoolIndex >= 0 &&
                static_cast<size_t>(c.geometryPoolIndex) < nativeLODByBlob.size())
                nativeLODByBlob[c.geometryPoolIndex] = nativeLODFromResolution(c.header.resolution);

        size_t lod0 = 0, lod1 = 0, lod2 = 0, fullResNodes = 0, residentNodes = 0;
        for (size_t b = 0; b < scene.geometryPool.size(); ++b) {
            const projv::GeometryBlob& blob = scene.geometryPool[b];
            if (blob.refCount == 0) continue;
            uint32_t effLOD = std::min(2u, nativeLODByBlob[b] + blob.renderLOD);
            if (effLOD == 0) lod0++;
            else if (effLOD == 1) lod1++;
            else lod2++;
            fullResNodes += blob.geometry.size() / 3;
            if (b < gpuData.blobRanges.size() && gpuData.blobRanges[b].uploaded)
                residentNodes += gpuData.blobRanges[b].geomTexelLen;
        }
        size_t readyRefinesCount;
        { std::lock_guard<std::mutex> rlock(g_worker.resultMutex); readyRefinesCount = g_worker.readyRefines.size(); }
        projv::core::info("[DIAG] activeChunks={} filled={} sceneChunks={} sceneBlobs={} pending={} workQ={} readyQ={} readyRefines={} lod0={} lod1={} lod2={} fullResNodes={} residentNodes={} ({:.1f}%)",
                   ts.activeChunks.size(), filled, scene.chunks.size(),
                   scene.geometryPool.size(),
                   ts.pendingTotal() - ts.pendingIndex,
                   g_worker.workQueue.size(), g_worker.readyChunks.size(), readyRefinesCount,
                   lod0, lod1, lod2, fullResNodes, residentNodes,
                   fullResNodes ? 100.0 * double(residentNodes) / double(fullResNodes) : 0.0);

        // Why is the fine ring under-populated? Two very different causes look identical from the
        // lod0/lod1/lod2 counts above, so separate them explicitly:
        //
        //   wantFiner > 0  -- resident chunks ARE sitting coarser than their distance says they
        //                     should. If wantFinerBlocked accounts for all of them, the
        //                     ts.refiningHandles guard is leaking and those chunks can never be
        //                     re-requested. wantFinerRegen is how many of them need a CPU
        //                     regeneration (request finer than their generated native resolution)
        //                     rather than a free renderLOD flip.
        //
        //   wantFiner == 0 -- every resident chunk is already at its correct LOD, so the missing
        //                     detail is chunks that are not resident AT ALL: coords that generated
        //                     empty at a coarse LOD and got the permanent kInvalidChunkHandle
        //                     sentinel in activeChunks. emptyFine/emptyNear count those sentinels
        //                     inside the fine/mid rings. Nothing ever revisits a sentinel, so a
        //                     chunk that samples empty at 16³ but has geometry at 256³ is lost for
        //                     good -- and the counts should then be implausibly high for a ring
        //                     that close to the camera.
        size_t wantFiner = 0, wantFinerBlocked = 0, wantFinerRegen = 0;
        size_t emptyFine = 0, emptyNear = 0, emptyStale = 0;
        for (const auto& [coord, h] : ts.activeChunks) {
            uint32_t desired = desiredLODClamped(ts, coord, camChunk);
            if (h == kInvalidChunkHandle) {
                if (desired == 0) emptyFine++;
                else if (desired == 1) emptyNear++;
                // Sentinels still owed a finer re-check. Should trend to 0 while the camera sits
                // still; a floor well above 0 means the re-check path is not draining.
                auto itE = ts.emptyChunks.find(coord);
                if (itE != ts.emptyChunks.end() && itE->second.marginal && desired < itE->second.lod)
                    emptyStale++;
                continue;
            }
            if (h >= scene.chunks.size()) continue;
            const projv::Chunk& c = scene.chunks[h];
            if (!c.alive || c.geometryPoolIndex < 0) continue;
            uint32_t eff = std::min(2u, nativeLODFromResolution(c.header.resolution) +
                                        scene.geometryPool[c.geometryPoolIndex].renderLOD);
            if (desired >= eff) continue;
            wantFiner++;
            if (ts.refiningHandles.count(h)) wantFinerBlocked++;
            if (uint32_t(TerrainState::resolutionForLOD(desired)) > c.header.resolution)
                wantFinerRegen++;
        }
        projv::core::info("[LODDIAG] refining={} wantFiner={} blocked={} needsRegen={} | empty: fineRing={} midRing={} awaitingRecheck={} inFlight={} tracked={}",
                   ts.refiningHandles.size(), wantFiner, wantFinerBlocked, wantFinerRegen,
                   emptyFine, emptyNear, emptyStale,
                   ts.revisitingEmpty.size(), ts.emptyChunks.size());

        // Per-LOD ALLOCATED footprint (what actually consumes the texture), split so the
        // marginal cost of one more coarse chunk is directly measurable.
        uint64_t gAlloc0 = 0, gAlloc1 = 0, gAlloc2 = 0, mAlloc0 = 0, mAlloc1 = 0, mAlloc2 = 0;
        // Used, as against allocated. GPUBlobRange deliberately rounds its ranges up so an edit can
        // grow a blob in place, and at 155k outer-ring chunks that rounding is paid 155k times --
        // so the ratio between these two is worth watching directly rather than inferring.
        uint64_t gLen0 = 0, gLen1 = 0, gLen2 = 0, mLen0 = 0, mLen1 = 0, mLen2 = 0;
        size_t n0 = 0, n1 = 0, n2 = 0;
        for (size_t b = 0; b < scene.geometryPool.size(); ++b) {
            const projv::GeometryBlob& blob = scene.geometryPool[b];
            if (blob.refCount == 0) continue;
            if (b >= gpuData.blobRanges.size() || !gpuData.blobRanges[b].uploaded) continue;
            const projv::GPUBlobRange& r = gpuData.blobRanges[b];
            // r.uploadedLOD mirrors blob.renderLOD's "relative to native" meaning -- same fix as above.
            uint32_t effLOD = std::min(2u, nativeLODByBlob[b] + r.uploadedLOD);
            if (effLOD == 0) { n0++; gAlloc0 += r.geomTexelAllocated; mAlloc0 += r.matTexelAllocated;
                               gLen0 += r.geomTexelLen; mLen0 += r.matTexelLen; }
            else if (effLOD == 1) { n1++; gAlloc1 += r.geomTexelAllocated; mAlloc1 += r.matTexelAllocated;
                                    gLen1 += r.geomTexelLen; mLen1 += r.matTexelLen; }
            else { n2++; gAlloc2 += r.geomTexelAllocated; mAlloc2 += r.matTexelAllocated;
                   gLen2 += r.geomTexelLen; mLen2 += r.matTexelLen; }
        }
        projv::core::info("[VRAM] lod0(256³)  n={} geomAlloc={} matAlloc={} ({:.0f}+{:.0f} texels/chunk) | lod1(64³) n={} geomAlloc={} matAlloc={} ({:.0f}+{:.0f}) | lod2(16³) n={} geomAlloc={} matAlloc={} ({:.0f}+{:.0f}) | totalBytes={:.1f}MB",
                   n0, gAlloc0, mAlloc0, n0 ? double(gAlloc0)/n0 : 0.0, n0 ? double(mAlloc0)/n0 : 0.0,
                   n1, gAlloc1, mAlloc1, n1 ? double(gAlloc1)/n1 : 0.0, n1 ? double(mAlloc1)/n1 : 0.0,
                   n2, gAlloc2, mAlloc2, n2 ? double(gAlloc2)/n2 : 0.0, n2 ? double(mAlloc2)/n2 : 0.0,
                   (double(gAlloc0 + gAlloc1 + gAlloc2) * 16.0 +
                    double(mAlloc0 + mAlloc1 + mAlloc2) * 4.0) / (1024.0*1024.0));

        // Allocated vs actually used, in MiB, per ring. The gap is over-allocation rounding, and it
        // is the cheapest VRAM to reclaim if it is large -- reclaiming it costs no quality at all,
        // unlike coarsening geometry or materials.
        auto mib = [](uint64_t g, uint64_t m) { return (double(g)*16.0 + double(m)*4.0) / (1024.0*1024.0); };
        const double a0 = mib(gAlloc0, mAlloc0), u0 = mib(gLen0, mLen0);
        const double a1 = mib(gAlloc1, mAlloc1), u1 = mib(gLen1, mLen1);
        const double a2 = mib(gAlloc2, mAlloc2), u2 = mib(gLen2, mLen2);
        projv::core::info("[VRAMFIT] lod0 {:.1f}/{:.1f}MiB used/alloc ({:.0f}%) | lod1 {:.1f}/{:.1f} ({:.0f}%) "
                   "| lod2 {:.1f}/{:.1f} ({:.0f}%) | total {:.1f}/{:.1f}MiB -> {:.1f}MiB reclaimable",
                   u0, a0, a0 > 0 ? 100.0*u0/a0 : 0.0, u1, a1, a1 > 0 ? 100.0*u1/a1 : 0.0,
                   u2, a2, a2 > 0 ? 100.0*u2/a2 : 0.0, u0+u1+u2, a0+a1+a2,
                   (a0+a1+a2) - (u0+u1+u2));
        // Material share, since it is 70% of the total and the only part with an obvious
        // quality-free compression left (uniform leaves already collapse; see bakeMaterialsFromBrickMap).
        projv::core::info("[VRAMSPLIT] geom {:.1f}MiB ({:.0f}%) | mat {:.1f}MiB ({:.0f}%) | "
                   "lod2 alone {:.1f}MiB ({:.0f}% of all)",
                   double(gAlloc0+gAlloc1+gAlloc2)*16.0/(1024.0*1024.0),
                   100.0*double(gAlloc0+gAlloc1+gAlloc2)*16.0/((double(gAlloc0+gAlloc1+gAlloc2)*16.0+double(mAlloc0+mAlloc1+mAlloc2)*4.0)),
                   double(mAlloc0+mAlloc1+mAlloc2)*4.0/(1024.0*1024.0),
                   100.0*double(mAlloc0+mAlloc1+mAlloc2)*4.0/((double(gAlloc0+gAlloc1+gAlloc2)*16.0+double(mAlloc0+mAlloc1+mAlloc2)*4.0)),
                   a2, 100.0*a2/(a0+a1+a2));
    }
}

void render(projv::Application& app) {
    using namespace projv::core;
    using namespace std::chrono;

    projv::graphics::RenderInstance& ri = projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    projv::GPUData& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    CameraState& cam = projv::core::getGlobalResource<CameraState>(app.world);

    static vec3 prevCameraPosition = cam.position;
    static vec3 prevCameraDirection{0, 0, 1};
    static bool prevInitialized = false;
    static int frameCameraLastMovedOn = 0;
    bool cameraMoved = false;

    vec3 camDir{cos(cam.pitch) * cos(cam.phi), sin(cam.pitch), cos(cam.pitch) * sin(cam.phi)};

    // Detect camera movement for temporal reprojection.
    if (length(cam.position - prevCameraPosition) > 0.001f ||
        length(camDir - prevCameraDirection) > 0.001f) {
        cameraMoved = true;
    }

    if (!prevInitialized) {
        prevCameraPosition = cam.position;
        prevCameraDirection = camDir;
        prevInitialized = true;
    }

    if (cameraMoved) frameCameraLastMovedOn = app.frameCount;

    // Sun elevation is mouse-scroll controlled (see g_sunElevation / sunScrollCallback);
    // azimuth stays fixed.
    const float sunAzX = -0.6402f, sunAzZ = 0.7682f;
    vec3 sunDirection = { cos(g_sunElevation) * sunAzX, sin(g_sunElevation), cos(g_sunElevation) * sunAzZ };

    FrameContext ctx;
    ctx.cameraPosition = cam.position;
    ctx.cameraDirection = camDir;
    ctx.prevCameraPosition = prevCameraPosition;
    ctx.prevCameraDirection = prevCameraDirection;
    ctx.sunDirection = sunDirection;
    ctx.frameCount = app.frameCount;
    ctx.cameraMoved = cameraMoved;
    ctx.frameCameraLastMovedOn = frameCameraLastMovedOn;
    ctx.windowResolution = ri.getWindowResolution();

    // --- RenderDoc auto-capture at frame 150 (polls; works with inject mode) ---
    {
        static RENDERDOC_API_1_6_0* rdoc = nullptr;
        static bool rdocFound = false;
        if (!rdocFound && app.frameCount % 10 == 0) {
            if (void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD)) {
                pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
                int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_6_0, (void**)&rdoc);
                if (ret == 1) { rdocFound = true; projv::core::info("[RDC] RenderDoc API loaded, will capture at frame 150"); }
                else rdoc = nullptr;
            }
        }
        if (rdocFound && rdoc && app.frameCount == 150) {
            projv::core::info("[RDC] Triggering capture at frame {}", app.frameCount);
            rdoc->TriggerCapture();
            projv::core::info("[RDC] Capture triggered");
        }
    }

    uploadCommonUniforms(ri.getActiveRenderer(), ctx);
    projv::graphics::renderConstructedRenderer(ri, ri.getActiveRenderer(), &gpuData);

    prevCameraPosition = cam.position;
    prevCameraDirection = camDir;

    static auto lastTime = high_resolution_clock::now();
    auto now = high_resolution_clock::now();
    float dt = duration_cast<duration<float, std::milli>>(now - lastTime).count();
    lastTime = now;
    static float accum = 0, minDt = 9999, maxDt = 0;
    static int timedFrames = 0;
    accum += dt; minDt = std::min(minDt, dt); maxDt = std::max(maxDt, dt);
    if (++timedFrames >= 60) {
        // Worker throughput per ring alongside the frame time. Chunks/sec on its own is
        // uninterpretable because ring cost varies by orders of magnitude -- see
        // WorkerState::completed.
        static uint64_t prevDone[3] = {};
        uint64_t done[3];
        for (int i = 0; i < 3; ++i) done[i] = g_worker.completed[i].load(std::memory_order_relaxed);
        float secs = accum / 1000.f;
        projv::core::info("[FPS] {:5.1f}ms avg {:5.1f}ms min {:5.1f}ms max {:3d}fps | gen/s lod0={:.0f} lod1={:.0f} lod2={:.0f}",
                  accum / 60.f, minDt, maxDt, int(60.f / accum * 1000.f),
                  (done[0] - prevDone[0]) / secs, (done[1] - prevDone[1]) / secs,
                  (done[2] - prevDone[2]) / secs);
        for (int i = 0; i < 3; ++i) prevDone[i] = done[i];
        accum = 0; minDt = 9999; maxDt = 0; timedFrames = 0;
    }
}

void shutdownApp(projv::Application&) {
    g_worker.running.store(false, std::memory_order_relaxed);
    g_worker.workCv.notify_all();
    g_worker.editCv.notify_all();
    for (auto& t : g_worker.threads) {
        if (t.joinable()) t.join();
    }
    g_worker.threads.clear();
    for (auto& t : g_worker.editThreads) {
        if (t.joinable()) t.join();
    }
    g_worker.editThreads.clear();
}

int main(int argc, char** argv) {
    projv::Application app = projv::core::createApp();
    std::string exeDir = fs::canonical(fs::path(argv[0])).parent_path().string();
    projv::core::createGlobalResource<std::string>(app.world) = std::move(exeDir);
    projv::core::createGlobalResource<CameraState>(app.world);
    projv::core::assignSystemStage(app, projv::SystemStage::Startup, startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update,  update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render,  render);
    projv::core::assignSystemStage(app, projv::SystemStage::Shutdown, shutdownApp);
    projv::core::runApplication(app);
    return 0;
}
