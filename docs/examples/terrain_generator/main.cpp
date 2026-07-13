// ProjectV Terrain Generator
// Generates an infinite Perlin-noise heightfield terrain as voxel chunks, streamed around
// the camera in a 5-chunk-radius sphere. New chunks are generated incrementally per frame.
//
// Biomes: water (deep/shallow), beach, plains, hills/forest, mountains, snow caps.
// Controls:
//   W/S       — move forward / backward
//   A/D       — strafe left / right
//   R/F       — move up / down
//   Mouse     — look around (cursor is captured; Esc releases it, left-click re-captures)

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
#include "utils/voxel_management.h"

#include "noise.hpp"

namespace fs = std::filesystem;

static inline float lerpf(float a, float b, float t) { return a + t * (b - a); }

struct ivec3_hash { size_t operator()(const projv::core::ivec3& v) const { return size_t(v.x) * 73856093 ^ size_t(v.y) * 19349663 ^ size_t(v.z) * 83492791; } };

struct FrameContext {
    projv::core::vec3 cameraPosition, cameraDirection;
    projv::core::vec2 windowResolution;
};

struct CameraState {
    projv::core::vec3 position       = projv::core::vec3(0, 120, 0);
    projv::core::vec3 prevPosition   = projv::core::vec3(0, 120, 0);
    projv::core::vec3 prevDirection  = projv::core::vec3(0, 1, 0);
    float pitch = -0.3f;
    float phi   = 1.57f;
    bool  prevInitialized = false;
};

static void uploadCommonUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    projv::core::vec3 cp = ctx.cameraPosition, cd = ctx.cameraDirection;
    projv::core::vec2 wr = ctx.windowResolution;
    projv::graphics::setUniformToValue(renderer, "cameraPos",  cp);
    projv::graphics::setUniformToValue(renderer, "cameraDir",  cd);
    projv::graphics::setUniformToValue(renderer, "windowRes",  wr);
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

    static constexpr float kWaterLevel = 42.0f;
    static constexpr float kBeachLevel = 48.0f;
    static constexpr float kPlainsLevel = 68.0f;
    static constexpr float kHillsLevel = 92.0f;
    static constexpr float kMountainLevel = 120.0f;
    static constexpr float kSnowLevel = 130.0f;

    siv::PerlinNoise terrainNoise;
    siv::PerlinNoise moistureNoise;

    int seed = 1337;

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

static float terrainHeight(float worldX, float worldZ, TerrainState& ts) {
    float nx = worldX * 0.0012f;
    float nz = worldZ * 0.0012f;
    float n = float(ts.terrainNoise.normalizedOctave2D(nx, nz, 4, 0.55f));
    float ridged = 1.0f - std::abs(n);
    return n * 60.0f + 50.0f + ridged * ridged * 30.0f;
}

static float terrainMoisture(float worldX, float worldZ, TerrainState& ts) {
    float nx = worldX * 0.003f + 100.0f;
    float nz = worldZ * 0.003f - 50.0f;
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
            int worldTopY = int(worldH);

            if (topLocalY < 0) {
                if (localWL >= 0 && localWL < res)
                    batch.push_back(projv::utils::createVoxel(projv::Color{30, 60, 155}, {lx, localWL, lz}));
                continue;
            }

            if (topLocalY >= res) continue;

            float moisture = terrainMoisture(wx, wz, ts);
            batch.push_back(projv::utils::createVoxel(biomeColor(worldH, moisture, worldTopY, 0.9f), {lx, topLocalY, lz}));

            int subY = topLocalY - 1;
            if (subY >= 0 && subY < res) {
                float frac = float(subY) / float(res);
                batch.push_back(projv::utils::createVoxel(biomeColor(worldH, moisture, worldTopY - 1, frac), {lx, subY, lz}));
            }

            if (localWL >= 0 && localWL < res && worldTopY < wl) {
                if (localWL != topLocalY && localWL != subY)
                    batch.push_back(projv::utils::createVoxel(projv::Color{30, 60, 155}, {lx, localWL, lz}));
            }
        }
    }
    return batch;
}

static projv::ChunkHandle registerLooseChunk(projv::Scene& scene, projv::Chunk&& chunk, projv::ComponentHandle compH) {
    projv::internChunkGeometry(scene, chunk);
    projv::ChunkHandle h = projv::ChunkHandle(scene.chunks.size());
    chunk.componentHandle = compH;
    scene.chunks.push_back(std::move(chunk));
    return h;
}

static void addLooseChunkToList(projv::Scene& scene, projv::ChunkHandle h) {
    if (size_t(scene.looseChunkCount) >= scene.looseChunks.size())
        scene.looseChunks.push_back(int32_t(h));
    else
        scene.looseChunks[size_t(scene.looseChunkCount)] = int32_t(h);
    scene.looseChunkCount++;
}

static void ensureComponentForChunk(projv::Scene& scene, projv::ChunkHandle chunkHandle) {
    projv::ComponentRecord comp{};
    comp.kind = projv::ComponentKind::Chunk;
    comp.chunkHandle = chunkHandle;
    comp.gridIndex = -1;
    comp.dataRefID = -1;
    comp.name = "terrain_c" + std::to_string(chunkHandle);
    scene.components.push_back(std::move(comp));
}

static void createChunkAt(projv::Scene& scene, projv::GPUData& gpuData, TerrainState& ts, projv::core::ivec3 coord, projv::VoxelBatch&& batch) {
    float chunkSize = TerrainState::kChunkSize;
    int res = TerrainState::kChunkRes;
    float vs = TerrainState::kVoxelScale;

    projv::ChunkHeader hdr;
    hdr.chunkID    = int(ts.activeChunks.size());
    hdr.position   = projv::core::vec3(float(coord.x) * chunkSize, float(coord.y) * chunkSize, float(coord.z) * chunkSize);
    hdr.scale      = chunkSize;
    hdr.voxelScale = vs;
    hdr.resolution = res;
    hdr.rotation   = projv::core::quat(1, 0, 0, 0);

    projv::Chunk chunk = projv::utils::createChunk(hdr);
    projv::utils::moveVoxelBatchToChunk(batch, chunk);
    projv::utils::updateChunkFromItsVoxelBatch(chunk);

    projv::ChunkHandle ch = registerLooseChunk(scene, std::move(chunk),
        projv::ComponentHandle(scene.components.size()));
    ensureComponentForChunk(scene, ch);
    addLooseChunkToList(scene, ch);
    ts.activeChunks[coord] = ch;
}

static void terrainWorkerFunc(TerrainState& ts) {
    using namespace std::chrono;
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
            std::this_thread::sleep_for(microseconds(200));
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
            createChunkAt(scene, gpuData, ts, coord, std::move(batch));
            generated++;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_worker.workMutex);
        while (ts.pendingIndex < ts.pendingCoords.size()) {
            projv::core::ivec3 coord = ts.pendingCoords[ts.pendingIndex++];
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
    std::thread(terrainWorkerFunc, std::ref(ts)).detach();

    projv::RendererSpecification spec = projv::graphics::loadRendererSpecification("./fastRenderer/");
    renderInstance.addRendererSpecification(1, spec);
    bgfx::ShaderHandle vsh = projv::graphics::loadShader("./fastRenderer/pathTracerShaders/vs_quad.bin");
    auto cr = projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vsh);

    renderInstance.setActiveRenderer(cr);
    gpuData = projv::graphics::createTexturesForScene(scene);

    // Seed initial pending chunk coords: sphere radius 5 around camera start.
    vec3 startPos(0.0f, 120.0f, 0.0f);
    ivec3 center = worldToChunkCoord(startPos);
    ts.lastCameraChunk = ivec3(-9999, -9999, -9999); // force first-frame rebuild
    int r2 = TerrainState::kViewRadius * TerrainState::kViewRadius;
    for (int dz = -TerrainState::kViewRadius; dz <= TerrainState::kViewRadius; ++dz)
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -TerrainState::kViewRadius; dx <= TerrainState::kViewRadius; ++dx)
                if (dx*dx + dy*dy + dz*dz <= r2)
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
    const float speed = 4.13f * 16.0f;
    if (glfwGetKey(ri.window, GLFW_KEY_W)) cam.position += forward * speed;
    if (glfwGetKey(ri.window, GLFW_KEY_S)) cam.position -= forward * speed;
    if (glfwGetKey(ri.window, GLFW_KEY_A)) { float p = cam.phi + 1.57f; cam.position -= vec3{cos(p), 0, sin(p)} * speed; }
    if (glfwGetKey(ri.window, GLFW_KEY_D)) { float p = cam.phi - 1.57f; cam.position -= vec3{cos(p), 0, sin(p)} * speed; }
    if (glfwGetKey(ri.window, GLFW_KEY_R)) cam.position.y += speed;
    if (glfwGetKey(ri.window, GLFW_KEY_F)) cam.position.y -= speed;

    // --- Chunk streaming ---
    ivec3 cameraChunk = worldToChunkCoord(cam.position);
    if (cameraChunk != ts.lastCameraChunk) {
        ts.lastCameraChunk = cameraChunk;
        ts.pendingCoords.clear();
        ts.pendingIndex = 0;
        int r2 = TerrainState::kViewRadius * TerrainState::kViewRadius;
        for (int dz = -TerrainState::kViewRadius; dz <= TerrainState::kViewRadius; ++dz)
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -TerrainState::kViewRadius; dx <= TerrainState::kViewRadius; ++dx)
                    if (dx*dx + dy*dy + dz*dz <= r2)
                        ts.pendingCoords.push_back(cameraChunk + ivec3(dx, dy, dz));
    }
    generatePendingChunks(scene, gpuData, ts);
}

void render(projv::Application& app) {
    using namespace projv::core;
    using namespace std::chrono;
    static auto lastTime = high_resolution_clock::now();

    projv::graphics::RenderInstance& ri = projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    projv::GPUData& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    TerrainState& ts = projv::core::getGlobalResource<TerrainState>(app.world);
    CameraState& cam = projv::core::getGlobalResource<CameraState>(app.world);

    vec3 camDir{cos(cam.pitch) * cos(cam.phi), sin(cam.pitch), cos(cam.pitch) * sin(cam.phi)};

    core_info_every(60, "Pos: ({:.1f}, {:.1f}, {:.1f})  Chunks: {}",
                    cam.position.x, cam.position.y, cam.position.z, ts.activeChunks.size());

    FrameContext ctx;
    ctx.cameraPosition = cam.position;
    ctx.cameraDirection = camDir;
    ctx.windowResolution = ri.getWindowResolution();

    uploadCommonUniforms(ri.getActiveRenderer(), ctx);
    projv::graphics::renderConstructedRenderer(ri, ri.getActiveRenderer(), &gpuData);

    cam.prevPosition = cam.position;
    cam.prevDirection = camDir;

    auto now = high_resolution_clock::now();
    float dt = duration_cast<duration<float, std::milli>>(now - lastTime).count();
    lastTime = now;
    static float accum = 0, minDt = 9999, maxDt = 0;
    static int timedFrames = 0;
    accum += dt; minDt = std::min(minDt, dt); maxDt = std::max(maxDt, dt);
    if (++timedFrames >= 60) {
        projv::core::info("[FPS] {:5.1f}ms avg {:5.1f}ms min {:5.1f}ms max {:3d}fps chunks={}",
                  accum / 60.f, minDt, maxDt, int(60.f / accum * 1000.f), ts.activeChunks.size());
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
