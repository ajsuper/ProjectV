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

#include "noise.hpp"

#include <dlfcn.h>
#include "renderdoc_app.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace fs = std::filesystem;

static inline float lerpf(float a, float b, float t) { return a + t * (b - a); }

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
struct WorkerState {
    std::mutex workMutex;
    std::vector<projv::core::ivec3> workQueue;
    std::mutex resultMutex;
    std::vector<std::pair<projv::core::ivec3, projv::VoxelBatch>> readyChunks;
    std::atomic<bool> running{true};
};

static WorkerState g_worker;

struct TerrainState {
    static constexpr int kChunkRes    = 64;
    static constexpr float kVoxelScale = 1.0f;
    static constexpr float kChunkSize  = kChunkRes * kVoxelScale;
    static constexpr int   kViewRadius = 10;
    static constexpr int   kMaxNewPerFrame = 10;
    static constexpr int   kBootstrapBatch = 40;

    static constexpr float kWaterLevel = 400.0f;
    static constexpr float kBeachLevel = 450.0f;
    static constexpr float kPlainsLevel = 550.0f;
    static constexpr float kHillsLevel = 700.0f;
    static constexpr float kMountainLevel = 850.0f;
    static constexpr float kSnowLevel = 950.0f;

    siv::PerlinNoise terrainNoise;
    siv::PerlinNoise moistureNoise;

    int seed = 133;

    // Grid
    int gridIndex = -1;
    projv::ComponentHandle gridCompHandle = projv::INVALID_COMPONENT_HANDLE;

    std::unordered_map<projv::core::ivec3, projv::ChunkHandle, ivec3_hash> activeChunks;
    projv::core::ivec3 lastCameraChunk{-9999, -9999, -9999};
    std::vector<projv::core::ivec3> pendingCoords;
    size_t pendingIndex = 0;
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

static float terrainHeight(float worldX, float worldZ, TerrainState& ts) {
    float nx = worldX * 0.004f;
    float nz = worldZ * 0.004f;
    float n = float(ts.terrainNoise.normalizedOctave2D(nx, nz, 4, 0.55f));
    float ridged = 1.0f - std::abs(n);
    return n * 300.0f + 450.0f + ridged * 120.0f;
}

static float terrainMoisture(float worldX, float worldZ, TerrainState& ts) {
    float nx = worldX * 0.01f + 100.0f;
    float nz = worldZ * 0.01f - 50.0f;
    return float(ts.moistureNoise.octave2D(nx, nz, 2, 0.55f));
}

static projv::Color biomeColor(float height, float moisture, int y, float frac) {
    if (y < TerrainState::kWaterLevel - 3.0f) {
        return projv::Color{15, 25, 110};
    }
    if (y < TerrainState::kWaterLevel) {
        float df = float(y) / TerrainState::kWaterLevel;
        return projv::Color{uint8_t(38), uint8_t(80), uint8_t(uint8_t(lerpf(140, 100, df)))};
    }
    if (y < TerrainState::kBeachLevel) {
        return projv::Color{210, 196, 135};
    }
    if (y < TerrainState::kPlainsLevel) {
        if (moisture > 0.35f) return projv::Color{44, 128, 45};
        if (moisture > -0.2f) return projv::Color{74, 153, 55};
        return projv::Color{130, 150, 60};
    }
    if (y < TerrainState::kHillsLevel) {
        if (moisture > 0.3f) return projv::Color{28, 92, 38};
        return projv::Color{55, 110, 48};
    }
    if (y < TerrainState::kMountainLevel) {
        return projv::Color{uint8_t(lerpf(90, 135, frac)), uint8_t(lerpf(90, 130, frac)), uint8_t(lerpf(85, 125, frac))};
    }
    if (y < TerrainState::kSnowLevel) {
        return projv::Color{180, 180, 185};
    }
    return projv::Color{240, 245, 255};
}

static projv::VoxelBatch generateChunkVoxels(projv::core::ivec3 coord, TerrainState& ts) {
    projv::VoxelBatch batch;
    float ox = float(coord.x) * TerrainState::kChunkSize;
    float oz = float(coord.z) * TerrainState::kChunkSize;
    float oy = float(coord.y) * TerrainState::kChunkSize;
    int res = TerrainState::kChunkRes;
    int wl = int(TerrainState::kWaterLevel);
    int localWL = wl - int(oy);

    for (int lz = 0; lz < res; ++lz) {
        float wz = oz + float(lz);
        for (int lx = 0; lx < res; ++lx) {
            float wx = ox + float(lx);
            float worldH = terrainHeight(wx, wz, ts);
            int topLocalY = int(worldH - oy);

            if (topLocalY < 0) {
                if (localWL >= 0 && localWL < res)
                    batch.push_back(projv::utils::createVoxel(projv::Color{30, 60, 155}, {lx, localWL, lz}));
                continue;
            }

            if (topLocalY >= res) continue;

            int worldTopY = int(worldH);
            float moisture = terrainMoisture(wx, wz, ts);

            for (int ly = 0; ly <= topLocalY; ++ly) {
                projv::Color col;
                if (ly == topLocalY) {
                    col = biomeColor(worldH, moisture, worldTopY, 1.0f);
                } else if (ly >= topLocalY - 4) {
                    col = biomeColor(worldH, moisture, worldTopY, 0.3f);
                } else {
                    col = projv::Color{80, 75, 70};
                }
                batch.push_back(projv::utils::createVoxel(col, {lx, ly, lz}));
            }

            if (localWL >= 0 && localWL < res && worldH < float(wl)) {
                for (int ly = topLocalY + 1; ly <= localWL; ++ly) {
                    batch.push_back(projv::utils::createVoxel(projv::Color{30, 60, 155}, {lx, ly, lz}));
                }
            }
        }
    }
    return batch;
}

static bool createChunkAt(projv::Scene& scene, projv::GPUData& /*gpuData*/, TerrainState& ts, projv::core::ivec3 coord, projv::VoxelBatch&& batch) {
    if (batch.empty()) {
        ts.activeChunks[coord] = static_cast<projv::ChunkHandle>(-1);
        return false;
    }
    projv::SceneGrid& grid = scene.grids[ts.gridIndex];
    float chunkSize = grid.cellSize;
    int res = TerrainState::kChunkRes;
    float vs = TerrainState::kVoxelScale;

    projv::core::ivec3 localCell = coord - grid.originCellCoord;
    projv::core::vec3 worldPos = grid.origin + glm::mat3_cast(grid.rotation) * (projv::core::vec3(localCell) * grid.cellSize);

    projv::ChunkHeader hdr;
    hdr.chunkID    = int(ts.activeChunks.size());
    hdr.position   = worldPos;
    hdr.scale      = chunkSize;
    hdr.voxelScale = vs;
    hdr.resolution = res;
    hdr.rotation   = grid.rotation;

    projv::Chunk chunk = projv::utils::createChunk(hdr);
    projv::utils::moveVoxelBatchToChunk(batch, chunk);

    int lin = chunkCoordToLin(coord, grid);
    chunk.gridIndex       = ts.gridIndex;
    chunk.cellIndex       = lin;
    chunk.componentHandle = ts.gridCompHandle;

    projv::utils::updateChunkFromItsVoxelBatch(chunk);
    projv::internChunkGeometry(scene, chunk);

    projv::ChunkHandle h = projv::ChunkHandle(scene.chunks.size());
    chunk.alive           = true;

    scene.chunks.push_back(std::move(chunk));
    grid.cellToChunk[lin] = static_cast<int32_t>(h);
    ts.activeChunks[coord] = h;
    return true;
}

static void terrainWorkerFunc(TerrainState& ts) {
    while (g_worker.running.load(std::memory_order_relaxed)) {
        projv::core::ivec3 coord;
        bool gotWork = false;
        {
            std::lock_guard<std::mutex> lock(g_worker.workMutex);
            if (!g_worker.workQueue.empty()) {
                coord = g_worker.workQueue.back();
                g_worker.workQueue.pop_back();
                gotWork = true;
            }
        }
        if (gotWork) {
            projv::VoxelBatch batch = generateChunkVoxels(coord, ts);
            {
                std::lock_guard<std::mutex> lock(g_worker.resultMutex);
                g_worker.readyChunks.emplace_back(coord, std::move(batch));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
}

static void generatePendingChunks(projv::Scene& scene, projv::GPUData& gpuData, TerrainState& ts) {
    int generated = 0;

    {
        std::lock_guard<std::mutex> lock(g_worker.resultMutex);
        while (!g_worker.readyChunks.empty() && generated < TerrainState::kMaxNewPerFrame) {
            auto [coord, batch] = std::move(g_worker.readyChunks.back());
            g_worker.readyChunks.pop_back();
            if (ts.activeChunks.count(coord)) continue;

            projv::SceneGrid& grid = scene.grids[ts.gridIndex];
            int lin = chunkCoordToLin(coord, grid);
            if (lin < 0 || lin >= static_cast<int>(grid.cellToChunk.size())) {
                projv::core::ivec3 cell = coord - grid.originCellCoord;
                projv::utils::expandGridToInclude(grid, cell, scene, ts.gridIndex);
                lin = chunkCoordToLin(coord, grid);
            }
            if (grid.cellToChunk[lin] >= 0) continue;

            if (createChunkAt(scene, gpuData, ts, coord, std::move(batch)))
                generated++;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_worker.workMutex);
        while (ts.pendingIndex < ts.pendingCoords.size()) {
            projv::core::ivec3 coord = ts.pendingCoords[ts.pendingIndex++];
            projv::SceneGrid& grid = scene.grids[ts.gridIndex];
            int lin = chunkCoordToLin(coord, grid);
            if (lin < 0 || lin >= static_cast<int>(grid.cellToChunk.size())) {
                projv::core::ivec3 cell = coord - grid.originCellCoord;
                projv::utils::expandGridToInclude(grid, cell, scene, ts.gridIndex);
                lin = chunkCoordToLin(coord, grid);
            }
            if (ts.activeChunks.count(coord)) continue;
            bool alreadyQueued = false;
            for (auto& wc : g_worker.workQueue) { if (wc == coord) { alreadyQueued = true; break; } }
            if (alreadyQueued) continue;
            {
                std::lock_guard<std::mutex> rlock(g_worker.resultMutex);
                bool alreadyReady = false;
                for (auto& [rc, _] : g_worker.readyChunks) { if (rc == coord) { alreadyReady = true; break; } }
                if (alreadyReady) continue;
            }
            g_worker.workQueue.push_back(coord);
        }
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

    projv::Scene& scene    = projv::core::createGlobalResource<projv::Scene>(app.world);
    projv::GPUData& gpuData = projv::core::createGlobalResource<projv::GPUData>(app.world);
    TerrainState& ts        = projv::core::createGlobalResource<TerrainState>(app.world);
    ts.terrainNoise.reseed(uint32_t(ts.seed));
    ts.moistureNoise.reseed(uint32_t(ts.seed + 9999));

    // --- Create terrain grid (centered on world origin, expands dynamically via expandGridToInclude) ---
    int r = ts.kViewRadius;
    int yMin = -1, yMax = 18;

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

    // Start worker thread
    std::thread(terrainWorkerFunc, std::ref(ts)).detach();

    // Seed initial pending chunk coords: sphere radius around camera start (0, 0, 0).
    ivec3 center(0, 0, 0);
    ts.lastCameraChunk = ivec3(-9999, -9999, -9999); // force first-frame rebuild
    for (int dz = -r; dz <= r; ++dz)
        for (int dy = yMin; dy <= yMax; ++dy)
            for (int dx = -r; dx <= r; ++dx)
                if (dx*dx + dy*dy + dz*dz <= r*r)
                    ts.pendingCoords.push_back(center + ivec3(dx, dy, dz));
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
        ts.pendingCoords.clear();
        ts.pendingIndex = 0;
        int r2 = TerrainState::kViewRadius * TerrainState::kViewRadius;
        for (int dz = -TerrainState::kViewRadius; dz <= TerrainState::kViewRadius; ++dz)
            for (int dy = -1; dy <= 18; ++dy)
                for (int dx = -TerrainState::kViewRadius; dx <= TerrainState::kViewRadius; ++dx)
                    if (dx*dx + dz*dz <= r2)
                        ts.pendingCoords.push_back(ivec3(camChunk.x + dx, dy, camChunk.z + dz));
    }
    generatePendingChunks(scene, gpuData, ts);
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

    // Static sun direction (fixed, no scroll wheel).
    float sunAngle = 0.86f;
    const float sunAzX = -0.6402f, sunAzZ = 0.7682f;
    vec3 sunDirection = { cos(sunAngle) * sunAzX, sin(sunAngle), cos(sunAngle) * sunAzZ };

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

int main(int argc, char** argv) {
    projv::Application app = projv::core::createApp();
    std::string exeDir = fs::canonical(fs::path(argv[0])).parent_path().string();
    projv::core::createGlobalResource<std::string>(app.world) = std::move(exeDir);
    projv::core::createGlobalResource<CameraState>(app.world);
    projv::core::assignSystemStage(app, projv::SystemStage::Startup, startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update,  update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render,  render);
    projv::core::runApplication(app);
    return 0;
}
