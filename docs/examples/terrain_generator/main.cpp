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
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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

#include <dlfcn.h>
#include "renderdoc_app.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace fs = std::filesystem;

static inline float lerpf(float a, float b, float t) { return a + t * (b - a); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

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
struct ChunkWorkItem {
    projv::core::ivec3 coord;
    projv::core::vec3 worldPos;
};

struct ProcessedChunk {
    projv::core::ivec3 coord;
    projv::Chunk chunk;
    std::vector<uint8_t> materialIDs;       // baked material IDs (local slots)
    std::vector<projv::Material> materialPalette;  // worker-local palette (merged by main thread)
    bool empty = false;  // true if the batch was empty (no geometry to intern)
};

struct WorkerState {
    std::mutex workMutex;
    std::condition_variable workCv;
    std::vector<ChunkWorkItem> workQueue;
    std::mutex resultMutex;
    std::vector<ProcessedChunk> readyChunks;
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;
    int numThreads = 1;
};

static WorkerState g_worker;

struct TerrainState {
    static constexpr int kChunkRes    = 64;
    // World size of one voxel edge. Each voxel covers kVoxelScale^2 of world footprint, so this is
    // sqrt() of the area factor: 3.0 -> 7.0 is ~5.4x the area per voxel. Chunk resolution is
    // unchanged, so a chunk covers ~5.4x more ground for the same voxel count and the same view
    // radius reaches ~2.3x further, at no extra memory.
    static constexpr float kVoxelScale = 7.0f;
    static constexpr float kChunkSize  = kChunkRes * kVoxelScale;

    // Vertical extent of chunk generation, in chunks. Derived from kChunkSize rather than hardcoded
    // so changing kVoxelScale does not leave us generating (and discarding) columns of empty sky:
    // taller chunks need proportionally fewer levels to cover the same world height. The ceiling
    // covers terrain_noise's tallest archetype amplitude (ARCH_AMP mountain = 2000) plus headroom
    // for blend and detail overshoot.
    static constexpr float kTerrainCeiling = 2600.0f;
    static constexpr int   kChunkYMin = -1;
    static constexpr int   kChunkYMax = int((kTerrainCeiling + kChunkSize - 1.0f) / kChunkSize);
    static constexpr int   kViewRadius = 30;
    // Chunks are evicted past kViewRadius + this. The gap keeps a camera sitting on a chunk
    // boundary from evicting and regenerating the same ring every time it steps back and forth.
    static constexpr int   kEvictHysteresis = 2;
    static constexpr int   kMaxNewPerFrame = 2;
    static constexpr int   kBootstrapBatch = 5;

    static constexpr float kWaterLevel = 400.0f;

    terrain_noise::Generator noiseGen;
    terrain_noise::BlendParams blendParams;
    int seed = 133;

    // Grid
    int gridIndex = -1;
    projv::ComponentHandle gridCompHandle = projv::INVALID_COMPONENT_HANDLE;

    std::unordered_map<projv::core::ivec3, projv::ChunkHandle, ivec3_hash> activeChunks;
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

static projv::core::ivec3 worldToChunkCoord(projv::core::vec3 p) {
    return projv::core::ivec3(int(std::floor(p.x / TerrainState::kChunkSize)),
                              int(std::floor(p.y / TerrainState::kChunkSize)),
                              int(std::floor(p.z / TerrainState::kChunkSize)));
}

static int chunkCoordToLin(projv::core::ivec3 coord, const projv::SceneGrid& grid) {
    projv::core::ivec3 cell = coord - grid.originCellCoord;
    return cell.x + grid.dims.x * (cell.y + grid.dims.y * cell.z);
}

static constexpr int kFillDepth = 100;

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

static projv::Color surfaceColor(const terrain_noise::TerrainSample& sample, int worldY) {
    float height = sample.height;
    float t = sample.temp;
    float h = sample.humid;
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

    // Rock
    float redW = clampf((1.0f - h * 2.5f) * (t * 3.0f - 0.8f), 0.0f, 1.0f);
    float darkW = clampf(h * 2.0f - 0.3f, 0.0f, 1.0f) * (1.0f - redW);
    float paleW = clampf((1.0f - t) * 2.5f - 0.5f, 0.0f, 1.0f) * (1.0f - redW) * (1.0f - darkW);
    float tempW = clampf(1.0f - redW - darkW - paleW, 0.0f, 1.0f);
    float rSum = redW + darkW + paleW + tempW;
    projv::core::vec3 rockColor = (projv::core::vec3{0.78f, 0.42f, 0.24f} * redW
                    + projv::core::vec3{0.26f, 0.28f, 0.31f} * darkW
                    + projv::core::vec3{0.88f, 0.90f, 0.95f} * paleW
                    + projv::core::vec3{0.52f, 0.48f, 0.42f} * tempW) * (1.0f / rSum);

    // Simplified material affinities
    struct Mat { projv::core::vec3 color; float tC, tW, hC, hW, aC, aW, rockBias, bias; };
    using vec3 = projv::core::vec3;
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

    float grey = col.x * 0.299f + col.y * 0.587f + col.z * 0.114f;
    col = vec3{clampf(grey + (col.x - grey) * 1.0f, 0.0f, 1.0f),
               clampf(grey + (col.y - grey) * 1.0f, 0.0f, 1.0f),
               clampf(grey + (col.z - grey) * 1.0f, 0.0f, 1.0f)};

    // Quantized to the lattice so the set of distinct surface colors stays inside the palette cap.
    // The water returns above are left exact -- they are already only four colors in total.
    return projv::Color{quantizeSurfaceChannel(col.x),
                        quantizeSurfaceChannel(col.y),
                        quantizeSurfaceChannel(col.z)};
}

static uint8_t internLocal(std::vector<projv::Material>& palette,
                              const std::string& name, uint32_t packedColor) {
    if (!name.empty()) {
        for (size_t i = 0; i < palette.size(); ++i)
            if (palette[i].name == name) return static_cast<uint8_t>(i);
    }
    for (size_t i = 0; i < palette.size(); ++i)
        if (palette[i].packedColor == packedColor) return static_cast<uint8_t>(i);
    palette.push_back({packedColor, 0, name});
    return static_cast<uint8_t>(palette.size() - 1);
}

static void generateChunkVoxels(projv::core::ivec3 coord, projv::VoxelBrickMap& map,
                                std::vector<projv::Material>& palette, TerrainState& ts) {
    float ox = float(coord.x) * TerrainState::kChunkSize;
    float oz = float(coord.z) * TerrainState::kChunkSize;
    float oy = float(coord.y) * TerrainState::kChunkSize;
    int res = TerrainState::kChunkRes;
    float vs = TerrainState::kVoxelScale;
    int wl = int(TerrainState::kWaterLevel);
    int localWL = int(std::floor((wl - oy) / vs));

    for (int lz = 0; lz < res; ++lz) {
        float wz = oz + float(lz) * vs;
        for (int lx = 0; lx < res; ++lx) {
            float wx = ox + float(lx) * vs;
            terrain_noise::TerrainSample sample = terrain_noise::sampleTerrain(ts.noiseGen, ts.blendParams, wx, wz);
            float worldH = sample.height;
            int topLocalY = int(std::floor((worldH - oy) / vs));

            if (topLocalY < 0) {
                // Surface below chunk - only water
                if (worldH < float(wl)) {
                    int waterSurfaceLocal = localWL;
                    if (waterSurfaceLocal >= res) waterSurfaceLocal = res - 1;
                    if (waterSurfaceLocal >= 0) {
                        uint32_t wpacked = projv::packColor(projv::Color{30, 60, 155});
                        uint8_t wmatID = internLocal(palette, "water", wpacked);
                        for (int ly = 0; ly <= waterSurfaceLocal; ++ly) {
                            projv::utils::brickMapSetVoxel(map, lx, ly, lz, wmatID);
                        }
                    }
                }
                continue;
            }

            if (topLocalY >= res) continue;

            // Surface color
            projv::Color col = surfaceColor(sample, int(worldH));
            uint32_t packed = projv::packColor(col);
            uint8_t matID = internLocal(palette, "", packed);
            projv::utils::brickMapSetVoxel(map, lx, topLocalY, lz, matID);

            // Fill ~30 voxels below surface with the same color (hollow terrain below that)
            int bottomFill = std::max(topLocalY - kFillDepth, 0);
            for (int ly = bottomFill; ly < topLocalY; ++ly) {
                projv::utils::brickMapSetVoxel(map, lx, ly, lz, matID);
            }

            // Water: fill from above terrain up to water surface
            if (worldH < float(wl)) {
                int waterSurfaceLocal = localWL;
                if (waterSurfaceLocal < 0) continue; // water surface below chunk
                if (waterSurfaceLocal >= res) waterSurfaceLocal = res - 1; // water surface above chunk
                int waterBottom = std::max(topLocalY + 1, 0);
                if (waterBottom <= waterSurfaceLocal) {
                    uint32_t wpacked = projv::packColor(projv::Color{30, 60, 155});
                    uint8_t wmatID = internLocal(palette, "water", wpacked);
                    for (int ly = waterBottom; ly <= waterSurfaceLocal; ++ly) {
                        projv::utils::brickMapSetVoxel(map, lx, ly, lz, wmatID);
                    }
                }
            }
        }
    }
}

static void terrainWorkerFunc(TerrainState& ts) {
    while (g_worker.running.load(std::memory_order_relaxed)) {
        ChunkWorkItem item;
        bool gotWork = false;
        {
            std::unique_lock<std::mutex> lock(g_worker.workMutex);
            g_worker.workCv.wait_for(lock, std::chrono::milliseconds(10),
                [&]{ return !g_worker.workQueue.empty() || !g_worker.running.load(std::memory_order_relaxed); });
            if (!g_worker.running.load(std::memory_order_relaxed)) return;
            if (!g_worker.workQueue.empty()) {
                item = g_worker.workQueue.back();
                g_worker.workQueue.pop_back();
                gotWork = true;
            }
        }
        if (gotWork) {
            int res = TerrainState::kChunkRes;
            auto brickMap = projv::utils::createVoxelBrickMap(
                projv::utils::computeBrickDims(res));
            std::vector<projv::Material> localPalette;
            generateChunkVoxels(item.coord, *brickMap, localPalette, ts);
            ProcessedChunk result;
            result.coord = item.coord;
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
                hdr.voxelScale = TerrainState::kVoxelScale;
                hdr.resolution = res;
                hdr.rotation   = projv::core::quat(1, 0, 0, 0);
                result.chunk = projv::utils::createChunk(hdr);
                result.chunk.gridIndex = ts.gridIndex;
                projv::utils::updateChunkFromBrickMap(result.chunk, *brickMap);
                // Bake material IDs (local slots against worker-local palette).
                projv::utils::bakeMaterialsFromBrickMap(result.chunk.geometryData,
                                                          result.materialIDs, *brickMap);
                result.materialPalette = std::move(localPalette);
            }
            {
                std::lock_guard<std::mutex> lock(g_worker.resultMutex);
                g_worker.readyChunks.push_back(std::move(result));
            }
        } // else: no work, loop back to wait on cv
    }
}

// Merge a worker-local palette into the component palette and remap materialIDs.
static std::vector<uint8_t> mergePaletteIntoComponent(projv::ComponentRecord& comp,
                                                       const std::vector<projv::Material>& localPalette) {
    std::vector<uint8_t> remap(localPalette.size(), 0);
    for (size_t i = 0; i < localPalette.size(); ++i)
        remap[i] = projv::utils::internMaterial(comp, localPalette[i].name, localPalette[i].packedColor);
    return remap;
}

static void remapMaterialIDs(std::vector<uint8_t>& ids, const std::vector<uint8_t>& remap) {
    for (auto& id : ids) id = remap[id];
}

// Tear down one streamed chunk and hand its resources back.
//
// Order matters: clearing the grid cell is what actually hides the chunk, because syncSceneTables
// rebuilds the cellMap from cellToChunk each flush and a -1 cell becomes 0xFFFFFFFF, which the
// shader skips. The stale GPU header row for this slot is therefore unreachable, and it gets
// rewritten anyway if the slot is later reused (headerDirty below).
static void releaseChunk(projv::Scene& scene, TerrainState& ts, projv::ChunkHandle h) {
    if (h == static_cast<projv::ChunkHandle>(-1) || h >= scene.chunks.size()) return;
    projv::Chunk& c = scene.chunks[h];
    if (!c.alive) return;

    if (c.gridIndex >= 0 && static_cast<size_t>(c.gridIndex) < scene.grids.size()) {
        projv::SceneGrid& grid = scene.grids[c.gridIndex];
        if (c.cellIndex >= 0 && static_cast<size_t>(c.cellIndex) < grid.cellToChunk.size() &&
            grid.cellToChunk[c.cellIndex] == static_cast<int32_t>(h)) {
            grid.cellToChunk[c.cellIndex] = -1;
        }
    }

    // Drop this chunk's reference to its geometry. At zero the CPU-side voxel data is released here
    // and the pool slot is recycled; the GPU texel ranges are freed by uploadDirtyBlobs, which looks
    // for exactly this (refCount == 0 with an uploaded range) on the next flush.
    int32_t pool = c.geometryPoolIndex;
    if (pool >= 0 && static_cast<size_t>(pool) < scene.geometryPool.size()) {
        projv::GeometryBlob& blob = scene.geometryPool[pool];
        if (blob.refCount > 0 && --blob.refCount == 0) {
            blob.geometry.clear();      blob.geometry.shrink_to_fit();
            blob.materialIDs.clear();   blob.materialIDs.shrink_to_fit();
            blob.brickMap.reset();
            blob.dirty = false;
            scene.blobFreeList.push_back(static_cast<uint32_t>(pool));
        }
    }

    c.alive = false;
    c.geometryPoolIndex = -1;
    c.gridIndex = -1;
    c.cellIndex = -1;
    ts.freeChunkSlots.push_back(h);
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
        releaseChunk(scene, ts, it->second);   // no-op for the -1 "known empty" sentinel
        it = ts.activeChunks.erase(it);
        evicted++;
    }
    if (evicted > 0) {
        projv::core::info("[EVICT] {} chunks outside radius {} — freeChunkSlots={} blobFreeList={}",
                          evicted, evictR, ts.freeChunkSlots.size(), scene.blobFreeList.size());
    }
}

static void generatePendingChunks(projv::Scene& scene, projv::GPUData& gpuData, TerrainState& ts) {
    int generated = 0;
    int consumed = 0;
    static constexpr int kMaxConsumedPerFrame = 64;

    {
        std::lock_guard<std::mutex> lock(g_worker.resultMutex);
        while (!g_worker.readyChunks.empty() && generated < TerrainState::kMaxNewPerFrame && consumed < kMaxConsumedPerFrame) {
            ProcessedChunk proc = std::move(g_worker.readyChunks.back());
            g_worker.readyChunks.pop_back();
            consumed++;
            if (ts.activeChunks.count(proc.coord)) continue;

            // The camera can have moved since this was queued. Eviction removed the coord from
            // activeChunks, so without this it would be admitted right back and then evicted again
            // on the next camera step.
            int ddx = proc.coord.x - ts.lastCameraChunk.x;
            int ddz = proc.coord.z - ts.lastCameraChunk.z;
            const int evictR = TerrainState::kViewRadius + TerrainState::kEvictHysteresis;
            if (ddx * ddx + ddz * ddz > evictR * evictR) continue;

            projv::SceneGrid& grid = scene.grids[ts.gridIndex];
            int lin = chunkCoordToLin(proc.coord, grid);
            if (lin < 0 || lin >= static_cast<int>(grid.cellToChunk.size())) {
                projv::core::ivec3 cell = proc.coord - grid.originCellCoord;
                projv::utils::expandGridToInclude(grid, cell, scene, ts.gridIndex);
                lin = chunkCoordToLin(proc.coord, grid);
            }
            if (grid.cellToChunk[lin] >= 0) continue;

            if (proc.empty) {
                ts.activeChunks[proc.coord] = static_cast<projv::ChunkHandle>(-1);
                continue;
            }

            // Monotonic: activeChunks.size() repeats once eviction starts removing entries.
            proc.chunk.header.chunkID = int(ts.nextChunkID++);
            proc.chunk.gridIndex       = ts.gridIndex;
            proc.chunk.cellIndex       = lin;
            proc.chunk.componentHandle = ts.gridCompHandle;
            proc.chunk.alive           = true;

            projv::internChunkGeometry(scene, proc.chunk);

            // Merge worker-local palette into component palette, remap materialIDs.
            projv::ComponentRecord& comp = scene.components[ts.gridCompHandle];
            std::vector<uint8_t> remap = mergePaletteIntoComponent(comp, proc.materialPalette);
            remapMaterialIDs(proc.materialIDs, remap);

            // Transfer remapped material IDs to the blob.
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
            ts.activeChunks[proc.coord] = h;
            projv::core::info("[GEN] coord=({},{},{}) h={} pool={} paletteSize={} localPaletteSize={}",
                              proc.coord.x, proc.coord.y, proc.coord.z,
                              h, poolIdx,
                              comp.materialPalette.size(), proc.materialPalette.size());
            generated++;
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
            g_worker.workQueue.push_back(ChunkWorkItem{c, wp});
        }
        g_worker.workCv.notify_all();
    }

    if (generated > 0) projv::graphics::flushSceneUpdates(scene, gpuData);
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
        g_worker.threads.emplace_back(terrainWorkerFunc, std::ref(ts));
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
    const float speed = 4.13f * 2.0f;
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
    generatePendingChunks(scene, gpuData, ts);

    // Diagnostic: log chunk population progress every 60 frames.
    if (app.frameCount % 60 == 0) {
        int filled = 0;
        for (const auto& [coord, h] : ts.activeChunks)
            if (h != static_cast<projv::ChunkHandle>(-1)) filled++;
        projv::core::info("[DIAG] activeChunks={} filled={} sceneChunks={} sceneBlobs={} pending={} workQ={} readyQ={}",
                   ts.activeChunks.size(), filled, scene.chunks.size(),
                   scene.geometryPool.size(),
                   ts.pendingCoords.size() - ts.pendingIndex,
                   g_worker.workQueue.size(), g_worker.readyChunks.size());
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
