// ProjectV Terrain Generator (grid-based)
// Generates a Perlin-noise heightfield terrain as voxel chunks in a SceneGrid,
// streamed around the camera. The grid allows the shader's marchGrid DDA path
// to handle broadphase instead of brute-forcing every loose chunk per pixel.
//
// Controls: WASD/R/F + mouse to fly.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
// sun, scrolling down lowers it (through sunset and on below the horizon into night). Azimuth
// stays fixed -- see the sunAzX/sunAzZ constants in render(). Plain global (not ECS state)
// because it's written from a GLFW callback on the same thread the render loop reads it from.
static float g_sunElevation = 0.26f;

void sunScrollCallback(GLFWwindow* /*window*/, double /*xoffset*/, double yoffset) {
    const float sensitivity = 0.03f;
    g_sunElevation += float(yoffset) * sensitivity;
    if (g_sunElevation >  1.55f) g_sunElevation =  1.55f;
    if (g_sunElevation < -0.60f) g_sunElevation = -0.60f;
}

struct CameraState {
    projv::core::vec3 position       = projv::core::vec3(0, 1000, 0);
    projv::core::vec3 prevPosition   = projv::core::vec3(0, 1000, 0);
    projv::core::vec3 prevDirection  = projv::core::vec3(0, 1, 0);
    float pitch = -0.3f;
    float phi   = 1.57f;
    bool  prevInitialized = false;
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
    projv::ChunkHandle refineHandle = kInvalidChunkHandle;
};

struct WorkerState {
    std::mutex workMutex;
    std::condition_variable workCv;
    // FIFO: consumed front-to-back so nearest-first ordering (pendingCoords is sorted by
    // distance before being pushed here) is preserved. A LIFO pop would generate the
    // farthest-just-pushed chunks first and starve the near ones the camera actually needs.
    // Refines are pushed to the front (see applyLODRing) so they're picked up next regardless of
    // how deep the ordinary streaming backlog is.
    std::deque<ChunkWorkItem> workQueue;
    std::mutex resultMutex;
    std::vector<ProcessedChunk> readyChunks;
    // Refine results, kept separate from readyChunks so they can be admitted every frame
    // regardless of kMaxNewPerFrame -- refines are rare (capped by kMaxRefinesPerFrame at the
    // point they're requested) and time-sensitive (the chunk is already popped out of the scene),
    // so they shouldn't have to wait behind whatever ordinary streaming happened to finish first.
    std::vector<ProcessedChunk> readyRefines;
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;
    int numThreads = 1;
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
    // World size of one voxel edge at LOD0. kVoxelScale is scaled inversely with kChunkRes so
    // the chunk world footprint stays the same: 7.0 * 64 / 256 = 1.75.
    static constexpr float kVoxelScale = 1.75f;
    static constexpr float kChunkSize  = kChunkRes * kVoxelScale;
    // Voxel edge length for a chunk generated at the given ring's native resolution, keeping the
    // world-space chunk footprint (kChunkSize) identical across all three rings.
    static constexpr float voxelScaleForLOD(uint32_t lod) {
        return kChunkSize / float(resolutionForLOD(lod));
    }

    // Vertical extent of chunk generation, in chunks. Derived from kChunkSize rather than hardcoded
    // so changing kVoxelScale does not leave us generating (and discarding) columns of empty sky:
    // taller chunks need proportionally fewer levels to cover the same world height. The ceiling
    // covers terrain_noise's tallest archetype amplitude (ARCH_AMP mountain = 2000) plus headroom
    // for blend and detail overshoot.
    static constexpr float kTerrainCeiling = 2600.0f;
    static constexpr int   kChunkYMin = -1;
    static constexpr int   kChunkYMax = int((kTerrainCeiling + kChunkSize - 1.0f) / kChunkSize);
    static constexpr int   kViewRadius = 50;
    // Storage LOD rings. Three levels:
    //   LOD 0 (256^3) — chunks within kLodRadiusFine chunks of camera
    //   LOD 1 (64^3)  — chunks within kLodRadius chunks of camera
    //   LOD 2 (16^3)  — everything inside the view radius
    static constexpr int   kLodRadiusFine = 3;
    static constexpr int   kLodRadius     = 12;
    // Cap on refines (regenerate-at-finer-resolution) triggered per frame. Unlike an ordinary LOD
    // change (a cheap flag flip -- setBlobRenderLOD), a refine re-runs the full worker generation
    // pipeline at higher resolution, so it gets a budget while ordinary transitions do not.
    static constexpr int   kMaxRefinesPerFrame = 3;
    // Chunks are evicted past kViewRadius + this. The gap keeps a camera sitting on a chunk
    // boundary from evicting and regenerating the same ring every time it steps back and forth.
    static constexpr int   kEvictHysteresis = 2;
    // Was 2, tuned back when every chunk (near or far) cost a full 256^3 admission. Now that
    // generation and admission are LOD-appropriately cheap for the vast majority of chunks (only
    // the innermost ring is full-res), a much higher per-frame admission rate is affordable and
    // meaningfully cuts how long newly-visible/refined chunks stay popped-out during fast travel.
    static constexpr int   kMaxNewPerFrame = 16;
    static constexpr int   kBootstrapBatch = 5;
    // Backpressure cap on unadmitted worker results (see terrainWorkerFunc). Bounds how far ahead
    // of kMaxNewPerFrame's admission rate the worker pool is allowed to race.
    static constexpr size_t kMaxReadyBacklog = 256;

    static constexpr float kWaterLevel = 400.0f;

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
    int seed = std::mt19937(std::random_device{}())();

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
    // Coords from emptyChunks currently being re-generated at a finer ring. Same job as
    // refiningHandles, but keyed by coord because these have no chunk handle to key on.
    std::unordered_set<projv::core::ivec3, ivec3_hash> revisitingEmpty;

    projv::core::ivec3 lastCameraChunk{-9999, -9999, -9999};
    std::vector<projv::core::ivec3> pendingCoords;
    size_t pendingIndex = 0;

    // Chunk slots freed by eviction, reused before appending to Scene.chunks. Scene.chunks is
    // "slot-indexed with holes" and never shrinks, and its length drives the GPU header texture
    // (capped at maxTextureSize/4 slots), so without reuse a long flight would grow it forever.
    std::vector<projv::ChunkHandle> freeChunkSlots;
    // Monotonic, so an evicted chunk's ID is never handed to a later chunk.
    uint32_t nextChunkID = 0;
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
static projv::Color surfaceColor(float height, float t, float h, int worldY,
                                  float wx, float wz, float slope,
                                  const TerrainState& ts) {
    int wl = int(TerrainState::kWaterLevel);

    if (worldY < wl - 3) {
        return projv::Color{15, 25, 110};
    }
    if (worldY < wl) {
        float df = float(worldY) / float(wl);
        return projv::Color{uint8_t(38), uint8_t(80), uint8_t(uint8_t(lerpf(140, 100, df)))};
    }

    // Climate-driven material blending (simplified from TerrainTest's surfaceColor)
    float altN = clampf((height - float(wl)) / terrain_noise::MAX_HEIGHT, 0.0f, 1.0f);

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
    float bare = rock::exposure(slope, altN, t);
    if (bare > 0.001f && hash2f(int(wx * 3.0f), int(wz * 3.0f) + 4096) < bare) {
        rock::Column rc = rock::prepare(ts.noiseGen.microN, wx, wz, t, h, altN, slope);
        col = rock::color(rc, ts.noiseGen.rockNoise, wx, float(worldY), wz, /*detail=*/true);
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
static void stampTreesIntoChunk(projv::core::ivec3 coord, projv::VoxelBrickMap& map,
                                TerrainState& ts, int res, float vs, uint32_t lod) {
    (void)lod;
    if (ts.treeLib.empty()) return;

    int placeDetailOct, placeErosionOct;
    terrainOctavesForLOD(0, placeDetailOct, placeErosionOct);
    auto sampleGround = [&](float x, float z) {
        terrain_noise::TerrainSample s = sampleWorld(ts, x, z, placeDetailOct, placeErosionOct);
        return trees::GroundSample{s.height, s.temp, s.humid};
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

// outMarginal (optional) reports whether the terrain surface came within a coarse voxel of this
// chunk's vertical slab. It is only meaningful when the chunk comes back empty; see
// ProcessedChunk::marginal.
static void generateChunkVoxels(projv::Scene& scene, projv::ComponentRecord& comp,
                                projv::core::ivec3 coord, projv::VoxelBrickMap& map,
                                TerrainState& ts, int res, float vs, uint32_t lod,
                                bool* outMarginal = nullptr) {
    float ox = float(coord.x) * TerrainState::kChunkSize;
    float oz = float(coord.z) * TerrainState::kChunkSize;
    float oy = float(coord.y) * TerrainState::kChunkSize;
    int wl = int(TerrainState::kWaterLevel);
    int localWL = int(std::floor((wl - oy) / vs));

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

    // Height prepass, one voxel wider than the chunk on every side.
    //
    // Rock exposure needs the terrain gradient, and a gradient needs neighbouring heights. Sampling
    // them on demand would triple the noise cost; sampling the columns once into a grid costs
    // nothing extra, because every column is needed anyway. The one-voxel skirt is what makes the
    // slope continuous *across* chunk boundaries -- with one-sided differences at the edges, two
    // neighbouring chunks would disagree about the slope on their shared seam and draw a visible
    // line of mismatched rock down it. It costs (res+2)^2/res^2, about 1.6% more samples at LOD0.
    const int stride = res + 2;
    struct ColumnSample { float height, temp, humid; };
    static thread_local std::vector<ColumnSample> columns;
    columns.resize(size_t(stride) * size_t(stride));
    for (int j = 0; j < stride; ++j) {
        float wz = oz + float(j - 1) * vs;
        for (int i = 0; i < stride; ++i) {
            float wx = ox + float(i - 1) * vs;
            terrain_noise::TerrainSample s = sampleWorld(ts, wx, wz, detailOct, erosionOct);
            columns[size_t(j) * stride + i] = {s.height, s.temp, s.humid};
        }
    }
    auto columnAt = [&](int i, int j) -> const ColumnSample& {
        return columns[size_t(j + 1) * stride + (i + 1)];
    };

    // Does the terrain surface come near this chunk's vertical slab at all? The prepass already
    // holds every column height, so this is just a min/max over data we have.
    //
    // The margin is one coarse voxel on each side. That is the scale at which this ring's sampling
    // can be wrong: a finer ring samples more columns AND more detail/erosion octaves
    // (terrainOctavesForLOD), so a surface sitting just outside the slab here can cross into it
    // there. Well clear of the slab on both sides, no amount of extra detail brings it back, so the
    // chunk is empty at every ring and never needs re-checking.
    if (outMarginal) {
        float minH = columns[0].height, maxH = columns[0].height;
        for (const ColumnSample& c : columns) {
            if (c.height < minH) minH = c.height;
            if (c.height > maxH) maxH = c.height;
        }
        const float lo = oy - vs;
        const float hi = oy + TerrainState::kChunkSize + vs;
        // Water counts too: a slab at or below the water line gains water voxels the moment any
        // column's ground drops below it, which is just as ring-sensitive as the ground itself.
        *outMarginal = (maxH >= lo && minH < hi) || oy <= TerrainState::kWaterLevel;
    }

    for (int lz = 0; lz < res; ++lz) {
        float wz = oz + float(lz) * vs;
        for (int lx = 0; lx < res; ++lx) {
            float wx = ox + float(lx) * vs;
            const ColumnSample& sample = columnAt(lx, lz);
            float worldH = sample.height;

            // Central differences in world units: rise over run, so 1.0 is a 45-degree slope.
            float slope;
            {
                float dhdx = (columnAt(lx + 1, lz).height - columnAt(lx - 1, lz).height) / (2.0f * vs);
                float dhdz = (columnAt(lx, lz + 1).height - columnAt(lx, lz - 1).height) / (2.0f * vs);
                slope = std::sqrt(dhdx * dhdx + dhdz * dhdz);
            }

            float altN0 = clampf((worldH - float(wl)) / terrain_noise::MAX_HEIGHT, 0.0f, 1.0f);
            float bare = rock::exposure(slope, altN0, sample.temp);

            // Joints cut real geometry, not just colour: where one surfaces on bare rock, the whole
            // column is recessed. Applied to the HEIGHT before it is voxelized, and only above
            // water. Chunk seams stay consistent because this is a pure function of world XZ, and
            // because both chunks voxelize the carved height rather than the raw one.
            if (worldH > float(wl) + 4.0f) {
                worldH -= float(vs) * float(rock::carveDepth(ts.noiseGen.rockNoise, wx, worldH, wz,
                                                             rock::verticality(slope), bare));
            }

            int topLocalY = int(std::floor((worldH - oy) / vs));

            if (topLocalY < 0) {
                // Surface below chunk - only water
                if (worldH < float(wl)) {
                    int waterSurfaceLocal = localWL;
                    if (waterSurfaceLocal >= res) waterSurfaceLocal = res - 1;
                    if (waterSurfaceLocal >= 0) {
                        for (int ly = 0; ly <= waterSurfaceLocal; ++ly) {
                            float voxelWorldY = oy + float(ly) * vs;
                            // name="" (not "water"): internMaterial matches by NAME first when one is
                            // given, so a shared name would collapse every depth shade below back onto
                            // whichever color got interned first. "" falls through to the color-based
                            // lookup, same as the plain surface-color path below.
                            uint8_t wmatID = internColor("", projv::packColor(
                                waterColorForDepth(float(wl) - voxelWorldY)));
                            projv::utils::brickMapSetVoxel(map, lx, ly, lz, wmatID);
                        }
                    }
                }
                continue;
            }

            if (topLocalY >= res) continue;

            // Surface color
            projv::Color col = surfaceColor(sample.height, sample.temp, sample.humid,
                                            int(worldH), wx, wz, slope, ts);
            uint32_t packed = projv::packColor(col);
            uint8_t matID = internColor("", packed);
            projv::utils::brickMapSetVoxel(map, lx, topLocalY, lz, matID);

            // Fill below the surface. Previously this smeared the surface colour straight down,
            // which meant a cliff face was a vertical stripe of whatever grew on top of it. Each
            // voxel is now coloured from its own altitude instead, so bedding reads across a cut
            // face -- which is the entire point of having strata at all.
            int bottomFill = std::max(topLocalY - kFillDepth, 0);
            if (bottomFill < topLocalY) {
                float altN = clampf((worldH - float(wl)) / terrain_noise::MAX_HEIGHT, 0.0f, 1.0f);
                // One evaluation of everything that is constant down this column; the per-voxel
                // loop below then costs a hash rather than three Perlin lookups.
                rock::Column rc = rock::prepare(ts.noiseGen.microN, wx, wz,
                                                sample.temp, sample.humid, altN, slope);

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

                for (int ly = bottomFill; ly < topLocalY; ++ly) {
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

            // Water: fill from above terrain up to water surface
            if (worldH < float(wl)) {
                int waterSurfaceLocal = localWL;
                if (waterSurfaceLocal < 0) continue; // water surface below chunk
                if (waterSurfaceLocal >= res) waterSurfaceLocal = res - 1; // water surface above chunk
                int waterBottom = std::max(topLocalY + 1, 0);
                if (waterBottom <= waterSurfaceLocal) {
                    for (int ly = waterBottom; ly <= waterSurfaceLocal; ++ly) {
                        float voxelWorldY = oy + float(ly) * vs;
                        uint8_t wmatID = internColor("", projv::packColor(
                            waterColorForDepth(float(wl) - voxelWorldY)));
                        projv::utils::brickMapSetVoxel(map, lx, ly, lz, wmatID);
                    }
                }
            }
        }
    }

    stampTreesIntoChunk(coord, map, ts, res, vs, lod);
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
            g_worker.workCv.wait_for(lock, std::chrono::milliseconds(10), [&]{
                if (!g_worker.running.load(std::memory_order_relaxed)) return true;
                if (g_worker.workQueue.empty()) return false;
                if (g_worker.workQueue.front().jumpQueue) return true;
                std::lock_guard<std::mutex> rlock(g_worker.resultMutex);
                return g_worker.readyChunks.size() < TerrainState::kMaxReadyBacklog;
            });
            if (!g_worker.running.load(std::memory_order_relaxed)) return;
            if (!g_worker.workQueue.empty()) {
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
        if (gotWork) {
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
            generateChunkVoxels(scene, comp, item.coord, *brickMap, ts, res, vs, item.lod, &marginal);
            ProcessedChunk result;
            result.coord = item.coord;
            result.lod = item.lod;
            result.marginal = marginal;
            result.refineHandle = item.refineHandle;
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
                // Material IDs are already in final global-palette slots (interned directly above).
                projv::utils::bakeMaterialsFromBrickMap(result.chunk.geometryData,
                                                          result.materialIDs, *brickMap);
            }
            {
                std::lock_guard<std::mutex> lock(g_worker.resultMutex);
                if (result.refineHandle != kInvalidChunkHandle) g_worker.readyRefines.push_back(std::move(result));
                else                                            g_worker.readyChunks.push_back(std::move(result));
            }
        } // else: no work, loop back to wait on cv
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

// Chunks are now generated directly at the resolution their assigned ring needs (see
// terrainWorkerFunc), so a chunk's actual on-CPU detail is whatever ring it happened to be
// generated at -- recovered here from the resolution stamped on its header at generation time.
static uint32_t nativeLODFromResolution(uint32_t res) {
    if (res >= uint32_t(TerrainState::resolutionForLOD(0))) return 0u;
    if (res >= uint32_t(TerrainState::resolutionForLOD(1))) return 1u;
    return 2u;
}

// Retune resident chunks toward their distance-appropriate LOD, at most kMaxRefinesPerFrame refine
// triggers per call. The whole activeChunks map is walked every frame (a few thousand integer
// compares -- nothing), but only the chunks whose LOD actually differs cost anything:
// requestChunkLOD returns Unchanged and touches nothing when the value is already right. Re-walking
// from the start each frame rather than holding a cursor keeps this safe against the map mutating
// under generation and eviction, and still makes forward progress since applied chunks stop
// matching.
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
    int applied = 0;
    std::vector<std::pair<projv::core::ivec3, projv::ChunkHandle>> toRefine;
    std::vector<projv::core::ivec3> toRecheck;
    for (const auto& [coord, h] : ts.activeChunks) {
        uint32_t desired = desiredLODForChunk(coord, camChunk);

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
        } else if (result == projv::ChunkLODResult::NeedsRegeneration &&
                   toRefine.size() < size_t(TerrainState::kMaxRefinesPerFrame)) {
            toRefine.push_back({coord, h});
        }
    }

    for (const auto& [coord, h] : toRefine) {
        projv::SceneGrid& grid = scene.grids[ts.gridIndex];
        projv::core::vec3 wp = grid.origin + (projv::core::vec3(coord - grid.originCellCoord) * TerrainState::kChunkSize);
        uint32_t lod = desiredLODForChunk(coord, camChunk);
        ts.refiningHandles.insert(h);
        {
            // push_front (not push_back): the chunk is already visible at a coarser resolution, so
            // this should jump straight to the head of the line rather than wait behind whatever
            // ordinary streaming backlog happens to be queued (which can be tens of thousands of
            // coords deep during a big flight).
            std::lock_guard<std::mutex> lock(g_worker.workMutex);
            g_worker.workQueue.push_front(ChunkWorkItem{coord, wp, lod, /*refineHandle=*/h,
                                                       /*jumpQueue=*/true});
        }
        g_worker.workCv.notify_all();
    }

    // Empty re-checks go through the ORDINARY path (no refineHandle): there is no chunk to replace,
    // so if this ring does find geometry the result has to be admitted as a brand-new chunk. The
    // sentinel stays in activeChunks meanwhile, which keeps the pendingCoords sweep from queueing
    // the same coord a second time; admit() knows to look past a sentinel.
    for (const projv::core::ivec3& coord : toRecheck) {
        projv::SceneGrid& grid = scene.grids[ts.gridIndex];
        projv::core::vec3 wp = grid.origin + (projv::core::vec3(coord - grid.originCellCoord) * TerrainState::kChunkSize);
        ts.revisitingEmpty.insert(coord);
        {
            std::lock_guard<std::mutex> lock(g_worker.workMutex);
            g_worker.workQueue.push_front(ChunkWorkItem{coord, wp, desiredLODForChunk(coord, camChunk),
                                                       /*refineHandle=*/kInvalidChunkHandle,
                                                       /*jumpQueue=*/true});
        }
        g_worker.workCv.notify_all();
    }

    // Refines don't dirty anything yet (the geometry lands frames later, via readyRefines), so
    // only the coarsen/flip path needs a flush now.
    return applied > 0;
}

// Evict every streamed chunk that has left the view radius.
static void evictDistantChunks(projv::Scene& scene, TerrainState& ts, projv::core::ivec3 camChunk) {
    const int evictR = TerrainState::kViewRadius + TerrainState::kEvictHysteresis;
    const int evictR2 = evictR * evictR;
    int evicted = 0;
    for (auto it = ts.activeChunks.begin(); it != ts.activeChunks.end(); ) {
        int dx = it->first.x - camChunk.x;
        int dz = it->first.z - camChunk.z;
        if (dx * dx + dz * dz <= evictR2) { ++it; continue; }
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
    if (evicted > 0) {
        projv::core::info("[EVICT] {} chunks outside radius {} — freeChunkSlots={} blobFreeList={}",
                          evicted, evictR, ts.freeChunkSlots.size(), scene.blobFreeList.size());
    }
}

// Returns true if anything was admitted or refined, i.e. if a flushSceneUpdates is needed. The
// flush itself belongs to the caller so that one flush per frame can cover this and applyLODRing
// together -- see the call site.
static bool generatePendingChunks(projv::Scene& scene, TerrainState& ts) {
    int generated = 0;
    int consumed = 0;
    static constexpr int kMaxConsumedPerFrame = 64;

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
            // Record WHICH ring decided this was empty, so a later, finer ring can overrule it.
            ts.activeChunks[proc.coord] = kInvalidChunkHandle;
            ts.emptyChunks[proc.coord] = TerrainState::EmptyRecord{proc.lod, proc.marginal};
            return false;
        }
        // Non-empty: any previous empty verdict for this coord is superseded.
        ts.emptyChunks.erase(proc.coord);

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
        // closer during generation; that comes back as NeedsRegeneration and is left alone here,
        // since applyLODRing's normal per-frame pass picks it up as a refine candidate on a later
        // frame, exactly like any other chunk.
        projv::requestChunkLOD(scene, h, TerrainState::resolutionForLOD(desiredLODForChunk(coord, ts.lastCameraChunk)));

        projv::core::info("[GEN] coord=({},{},{}) h={} pool={} paletteSize={}",
                          coord.x, coord.y, coord.z, h, poolIdx,
                          scene.components[ts.gridCompHandle].materialPalette.size());
        return true;
    };

    {
        std::lock_guard<std::mutex> lock(g_worker.resultMutex);
        // Refines: install directly onto the existing handle via replaceChunkGeometry -- the chunk
        // was never evicted, so this is not "admission." Uncapped: rare (bounded by
        // kMaxRefinesPerFrame at request time in applyLODRing), so it can't flood this loop.
        while (!g_worker.readyRefines.empty()) {
            ProcessedChunk proc = std::move(g_worker.readyRefines.back());
            g_worker.readyRefines.pop_back();
            ts.refiningHandles.erase(proc.refineHandle);

            // Stale if the camera moved far enough away that evictDistantChunks already reclaimed
            // this coord/handle in the meantime (possibly recycling the slot for a different chunk
            // entirely via freeChunkSlots) -- discard rather than clobber whatever's there now.
            auto it = ts.activeChunks.find(proc.coord);
            if (it == ts.activeChunks.end() || it->second != proc.refineHandle) continue;
            if (proc.empty) continue;  // shouldn't happen for a refine of previously-solid terrain

            projv::replaceChunkGeometry(scene, proc.refineHandle,
                                        std::move(proc.chunk.geometryData),
                                        std::move(proc.materialIDs),
                                        proc.chunk.header.resolution);
            projv::core::info("[REFINE] coord=({},{},{}) h={} resolution={}",
                              proc.coord.x, proc.coord.y, proc.coord.z,
                              proc.refineHandle, proc.chunk.header.resolution);
            generated++;
        }
        while (!g_worker.readyChunks.empty() && generated < TerrainState::kMaxNewPerFrame && consumed < kMaxConsumedPerFrame) {
            ProcessedChunk proc = std::move(g_worker.readyChunks.back());
            g_worker.readyChunks.pop_back();
            consumed++;
            // Clear the in-flight marker whatever the verdict is (admitted, still empty, or
            // rejected as stale) -- admit() records the new empty ring on its own, and leaving the
            // marker set would freeze this coord out of every future re-check.
            ts.revisitingEmpty.erase(proc.coord);
            if (admit(std::move(proc))) generated++;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_worker.workMutex);
        while (ts.pendingIndex < ts.pendingCoords.size()) {
            projv::core::ivec3 c = ts.pendingCoords[ts.pendingIndex++];
            projv::SceneGrid& grid = scene.grids[ts.gridIndex];
            int lin = chunkCoordToLin(c, grid);
            if (lin < 0 || lin >= static_cast<int>(grid.cellToChunk.size())) {
                projv::core::ivec3 cell = c - grid.originCellCoord;
                projv::utils::expandGridToInclude(grid, cell, scene, ts.gridIndex);
                lin = chunkCoordToLin(c, grid);
            }
            if (ts.activeChunks.count(c)) continue;
            bool alreadyQueued = false;
            for (auto& wi : g_worker.workQueue) { if (wi.coord == c) { alreadyQueued = true; break; } }
            if (alreadyQueued) continue;
            {
                std::lock_guard<std::mutex> rlock(g_worker.resultMutex);
                bool alreadyReady = false;
                for (auto& pc : g_worker.readyChunks) { if (pc.coord == c) { alreadyReady = true; break; } }
                if (alreadyReady) continue;
            }
            projv::core::vec3 wp = grid.origin + (projv::core::vec3(c - grid.originCellCoord) * TerrainState::kChunkSize);
            uint32_t lod = desiredLODForChunk(c, ts.lastCameraChunk);
            g_worker.workQueue.push_back(ChunkWorkItem{c, wp, lod});
        }
        g_worker.workCv.notify_all();
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
        ts.treeParams.treeVoxelWorldSize = TerrainState::voxelScaleForLOD(0);
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
        const std::string treeFolder = "../ObjVoxelizer/trees";
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
                              "Generate them with: cd ../ObjVoxelizer && make && "
                              "./obj_voxelizer -m <model> -f <obj> -o trees/<name> -r 64",
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

    // Start worker threads — use all available cores
    int numWorkers = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    projv::core::info("[WORKER] spawning {} terrain generation threads", numWorkers);
    g_worker.numThreads = numWorkers;
    for (int i = 0; i < numWorkers; ++i) {
        g_worker.threads.emplace_back(terrainWorkerFunc, std::ref(scene), std::ref(ts));
    }

    // Seed initial pending chunk coords: sphere radius around camera start (0, 0, 0),
    // sorted by distance from camera so closest chunks generate first.
    ivec3 center(0, 0, 0);
    ts.lastCameraChunk = ivec3(-9999, -9999, -9999); // force first-frame rebuild
    for (int dz = -r; dz <= r; ++dz)
        for (int dy = yMin; dy <= yMax; ++dy)
            for (int dx = -r; dx <= r; ++dx)
                if (dx*dx + dy*dy + dz*dz <= r*r)
                    ts.pendingCoords.push_back(center + ivec3(dx, dy, dz));
    std::sort(ts.pendingCoords.begin(), ts.pendingCoords.end(),
        [](const ivec3& a, const ivec3& b) {
            return a.x*a.x + a.y*a.y + a.z*a.z < b.x*b.x + b.y*b.y + b.z*b.z;
        });
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

    vec3 forward{cos(cam.phi), 0, sin(cam.phi)};
    const float baseSpeed = 1.0f;
    const float boostSpeed = baseSpeed * 20.0f;
    const float speed = glfwGetKey(ri.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? boostSpeed : baseSpeed;
    if (glfwGetKey(ri.window, GLFW_KEY_W)) cam.position += forward * speed;
    if (glfwGetKey(ri.window, GLFW_KEY_S)) cam.position -= forward * speed;
    if (glfwGetKey(ri.window, GLFW_KEY_A)) { float p = cam.phi + 1.57f; cam.position -= vec3{cos(p), 0, sin(p)} * speed; }
    if (glfwGetKey(ri.window, GLFW_KEY_D)) { float p = cam.phi - 1.57f; cam.position -= vec3{cos(p), 0, sin(p)} * speed; }
    if (glfwGetKey(ri.window, GLFW_KEY_R)) cam.position.y += speed;
    if (glfwGetKey(ri.window, GLFW_KEY_F)) cam.position.y -= speed;

    // --- Chunk streaming (X/Z only; generate full vertical columns each time) ---
    ivec3 camChunk = worldToChunkCoord(cam.position);
    if (camChunk.x != ts.lastCameraChunk.x || camChunk.z != ts.lastCameraChunk.z) {
        ts.lastCameraChunk = camChunk;
        evictDistantChunks(scene, ts, camChunk);
        ts.pendingCoords.clear();
        ts.pendingIndex = 0;
        int r2 = TerrainState::kViewRadius * TerrainState::kViewRadius;
        for (int dz = -TerrainState::kViewRadius; dz <= TerrainState::kViewRadius; ++dz)
            for (int dy = TerrainState::kChunkYMin; dy <= TerrainState::kChunkYMax; ++dy)
                for (int dx = -TerrainState::kViewRadius; dx <= TerrainState::kViewRadius; ++dx)
                    if (dx*dx + dz*dz <= r2)
                        ts.pendingCoords.push_back(ivec3(camChunk.x + dx, dy, camChunk.z + dz));
        // Sort by distance from camera chunk so closest generates first
        std::sort(ts.pendingCoords.begin(), ts.pendingCoords.end(),
            [camChunk](const ivec3& a, const ivec3& b) {
                ivec3 da = a - camChunk, db = b - camChunk;
                return da.x*da.x + da.y*da.y + da.z*da.z < db.x*db.x + db.y*db.y + db.z*db.z;
            });
    }
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
    needsFlush |= applyLODRing(scene, ts, camChunk);
    if (needsFlush) projv::graphics::flushSceneUpdates(scene, gpuData);

    // Diagnostic: log chunk population progress every 60 frames.
    if (app.frameCount % 60 == 0) {
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
                   ts.pendingCoords.size() - ts.pendingIndex,
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
            uint32_t desired = desiredLODForChunk(coord, camChunk);
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
        size_t n0 = 0, n1 = 0, n2 = 0;
        for (size_t b = 0; b < scene.geometryPool.size(); ++b) {
            const projv::GeometryBlob& blob = scene.geometryPool[b];
            if (blob.refCount == 0) continue;
            if (b >= gpuData.blobRanges.size() || !gpuData.blobRanges[b].uploaded) continue;
            const projv::GPUBlobRange& r = gpuData.blobRanges[b];
            // r.uploadedLOD mirrors blob.renderLOD's "relative to native" meaning -- same fix as above.
            uint32_t effLOD = std::min(2u, nativeLODByBlob[b] + r.uploadedLOD);
            if (effLOD == 0) { n0++; gAlloc0 += r.geomTexelAllocated; mAlloc0 += r.matTexelAllocated; }
            else if (effLOD == 1) { n1++; gAlloc1 += r.geomTexelAllocated; mAlloc1 += r.matTexelAllocated; }
            else { n2++; gAlloc2 += r.geomTexelAllocated; mAlloc2 += r.matTexelAllocated; }
        }
        projv::core::info("[VRAM] lod0(256³)  n={} geomAlloc={} matAlloc={} ({:.0f}+{:.0f} texels/chunk) | lod1(64³) n={} geomAlloc={} matAlloc={} ({:.0f}+{:.0f}) | lod2(16³) n={} geomAlloc={} matAlloc={} ({:.0f}+{:.0f}) | totalBytes={:.1f}MB",
                   n0, gAlloc0, mAlloc0, n0 ? double(gAlloc0)/n0 : 0.0, n0 ? double(mAlloc0)/n0 : 0.0,
                   n1, gAlloc1, mAlloc1, n1 ? double(gAlloc1)/n1 : 0.0, n1 ? double(mAlloc1)/n1 : 0.0,
                   n2, gAlloc2, mAlloc2, n2 ? double(gAlloc2)/n2 : 0.0, n2 ? double(mAlloc2)/n2 : 0.0,
                   (double(gAlloc0 + gAlloc1 + gAlloc2) * 16.0 +
                    double(mAlloc0 + mAlloc1 + mAlloc2) * 4.0) / (1024.0*1024.0));
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
        projv::core::info("[FPS] {:5.1f}ms avg {:5.1f}ms min {:5.1f}ms max {:3d}fps",
                  accum / 60.f, minDt, maxDt, int(60.f / accum * 1000.f));
        accum = 0; minDt = 9999; maxDt = 0; timedFrames = 0;
    }
}

void shutdownApp(projv::Application&) {
    g_worker.running.store(false, std::memory_order_relaxed);
    g_worker.workCv.notify_all();
    for (auto& t : g_worker.threads) {
        if (t.joinable()) t.join();
    }
    g_worker.threads.clear();
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
