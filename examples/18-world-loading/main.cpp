// ProjectV World Loading
//
// A terrain 768 m across, loaded around you as you fly: only the blocks near the camera are in
// memory, and they come and go as it moves. The world is a compose folder like any other; what is
// loaded is decided by one rule (runtime::openWorld, utils::loadNear), and the engine does the rest
// -- reading blocks on a worker thread, nearest first, a budget per frame.
//
//   1. Startup  window, platform; write the terrain the first time (terrain/ beside the binary);
//               openWorld opens it with no voxels loaded
//   2. Update   the camera flies; its position is the viewer the rule judges blocks by
//   3. PreUpdate (next frame) "world loading" asks the rule, starts reads, makes and frees chunks
//   4. Render   flushSceneUpdates uploads the blocks that arrived and draws
//
//   ./world_loading                   run
//   ./world_loading --regenerate      write the terrain again
//   ./world_loading --headless-check  no window: fly the self-test's course with the loader alone
//
// Controls: W/S/A/D move, R/F up/down, Shift fast, mouse looks (Esc releases, click recaptures).
// 1: loadNear(96, 192)   2: loadNear(32, 64)   3: loadEverything   -- stats are logged each second.
//
// WORLD_LOADING_SELFTEST=1 flies a fixed course with rule 1 and checks that resident memory stays
// bounded and every block near the camera is loaded when it gets there; prints WORLDTEST lines and
// exits 0 or 1.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/input.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "runtime/loading.h"
#include "runtime/physics.h"
#include "utils/compose_io.h"
#include "utils/material.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

using projv::core::vec3;

// -----------------------------------------------------------------------------------------
// The terrain, written once
// -----------------------------------------------------------------------------------------

namespace terrain {
    constexpr int   CELLS = 24;           // per side
    constexpr int   LAYERS = 3;           // cells high
    constexpr int   RES = 64;             // voxels per cell edge
    constexpr float VOXEL = 0.5f;         // metres
    constexpr float CELL = RES * VOXEL;   // 32 m
    constexpr int   SHELL = 3;            // voxels of ground under the surface; hollow below

    enum Slot : uint8_t { Grass, Dirt, Rock, Snow };

    float height(float x, float z) {
        return 38.0f + 22.0f * std::sin(x * 0.011f) * std::cos(z * 0.013f) + 9.0f * std::sin(x * 0.037f + z * 0.021f) +
               4.0f * std::sin(x * 0.09f) * std::sin(z * 0.08f);
    }

    uint8_t slotAt(float y, float surface, float slope) {
        if (y < surface - 1.0f) return Dirt;
        if (surface > 62.0f) return Snow;
        if (slope > 0.9f) return Rock;
        return Grass;
    }

    bool write(const std::filesystem::path& folder) {
        projv::core::info("World loading: writing the terrain ({} x {} x {} cells) to {}", CELLS, LAYERS, CELLS, folder.string());
        projv::DataFile file;
        file.resolution = RES;
        file.voxelScale = VOXEL;
        const projv::core::ivec3 brickDims = projv::utils::computeBrickDims(RES);
        for (int cz = 0; cz < CELLS; cz++)
            for (int cy = 0; cy < LAYERS; cy++)
                for (int cx = 0; cx < CELLS; cx++) {
                    auto map = projv::utils::createVoxelBrickMap(brickDims);
                    bool any = false;
                    for (int z = 0; z < RES; z++)
                        for (int x = 0; x < RES; x++) {
                            float wx = (cx * RES + x + 0.5f) * VOXEL, wz = (cz * RES + z + 0.5f) * VOXEL;
                            float surface = height(wx, wz);
                            float slope = std::abs(height(wx + 1.0f, wz) - surface) + std::abs(height(wx, wz + 1.0f) - surface);
                            int top = int(std::floor(surface / VOXEL)) - cy * RES;
                            for (int y = std::max(0, top - SHELL); y <= std::min(RES - 1, top); y++) {
                                projv::utils::brickMapSetVoxel(*map, x, y, z, slotAt((cy * RES + y) * VOXEL, surface, slope));
                                any = true;
                            }
                        }
                    if (!any) continue;
                    projv::DataBlock block;
                    block.gridX = cx;
                    block.gridY = cy;
                    block.gridZ = cz;
                    block.geometry = projv::utils::buildTree64FromBrickMap(*map, RES);
                    projv::utils::bakeMaterialsFromBrickMap(block.geometry, block.materialIDs, *map);
                    file.blocks.push_back(std::move(block));
                }
        std::filesystem::create_directories(folder);
        if (!projv::utils::writeDataFile((folder / "terrain.data").string(), file)) return false;
        std::ofstream(folder / "compose.json") << R"({
    "version": 1,
    "name": "terrain",
    "components": [
        { "type": "data", "source": "terrain.data", "id": 1, "name": "terrain",
          "palette": [
              { "name": "grass", "color": [330, 620, 250] },
              { "name": "dirt",  "color": [450, 330, 210] },
              { "name": "rock",  "color": [480, 470, 460] },
              { "name": "snow",  "color": [960, 970, 1000] }
          ] }
    ]
})";
        projv::core::info("World loading: {} blocks written", file.blocks.size());
        return true;
    }
}

// -----------------------------------------------------------------------------------------
// Camera, rules, stats
// -----------------------------------------------------------------------------------------

struct Camera {
    vec3 position{40.0f, 90.0f, 40.0f};
    float yaw = 0.785f;
    float pitch = -0.35f;
};

struct Stats {
    double windowStart = 0.0;
    double worstFrameMs = 0.0;
    size_t peakResident = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};

// The self-test's course: out across the terrain and back, at 60 m/s, 60 m up.
struct SelfTest {
    bool  on = false;
    int   frame = 0;
    bool  failed = false;
    size_t peakResident = 0;
    size_t wholeWorld = 0;
};

vec3 coursePoint(float t) {
    float s = std::fmod(t * 60.0f, 1400.0f);
    float along = s < 700.0f ? s : 1400.0f - s;
    return vec3(40.0f + along, 0.0f, 40.0f + along * 0.8f) + vec3(0.0f, 60.0f + terrain::height(40.0f + along, 40.0f + along * 0.8f), 0.0f);
}

void cameraSystem(projv::Application& app) {
    auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
    const auto& input = app.world.ctx().get<projv::Input>();
    auto& camera = app.world.ctx().get<Camera>();
    auto& test = app.world.ctx().get<SelfTest>();

    if (test.on) {
        float t = float(test.frame) / 60.0f;
        vec3 next = coursePoint(t + 1.0f / 60.0f);
        camera.position = coursePoint(t);
        vec3 d = glm::normalize(next - camera.position);
        camera.yaw = std::atan2(d.z, d.x);
        camera.pitch = -0.3f;
    } else {
        if (input.cursorCaptured && input.pressed(projv::Key::Escape)) {
            projv::graphics::setCursorCaptured(app, renderInstance, false);
        } else if (!input.cursorCaptured && input.pressed(projv::MouseButton::Left)) {
            projv::graphics::setCursorCaptured(app, renderInstance, true);
        }
        if (input.cursorCaptured) {
            camera.yaw += input.cursorDelta.x * 0.0025f;
            camera.pitch = std::clamp(camera.pitch - input.cursorDelta.y * 0.0025f, -1.55f, 1.55f);
        }
        const float speed = (input.down(projv::Key::LeftShift) ? 160.0f : 40.0f) * app.time().unscaledDelta;
        const vec3 forward{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
        const vec3 right{-std::sin(camera.yaw), 0.0f, std::cos(camera.yaw)};
        if (input.down(projv::Key::W)) camera.position += forward * speed;
        if (input.down(projv::Key::S)) camera.position -= forward * speed;
        if (input.down(projv::Key::D)) camera.position += right * speed;
        if (input.down(projv::Key::A)) camera.position -= right * speed;
        if (input.down(projv::Key::R)) camera.position.y += speed;
        if (input.down(projv::Key::F)) camera.position.y -= speed;

        if (input.pressed(projv::Key::Num1)) projv::runtime::setLoadRule(app.world, projv::utils::loadNear(96.0f, 192.0f));
        if (input.pressed(projv::Key::Num2)) projv::runtime::setLoadRule(app.world, projv::utils::loadNear(32.0f, 64.0f));
        if (input.pressed(projv::Key::Num3)) projv::runtime::setLoadRule(app.world, projv::utils::loadEverything());
    }
    // The camera is the viewer. A game would more likely give its player entity a LoadViewer.
    projv::runtime::setViewers(app.world, {{camera.position, 0}});
}

// Every block within `radius` of the camera is loaded.
bool nearIsLoaded(projv::World& world, float radius) {
    const auto& scene = world.ctx().get<projv::Scene>();
    const auto& loader = projv::runtime::worldLoader(world);
    const vec3 at = world.ctx().get<Camera>().position;
    for (projv::utils::BlockId id = 0; id < loader.blocks().size(); id++) {
        if (projv::utils::distanceToBox(loader.blocks().bounds(scene, id), at) <= radius && !loader.blocks().loaded(id)) return false;
    }
    return true;
}

void statsSystem(projv::Application& app) {
    auto& stats = app.world.ctx().get<Stats>();
    auto& test = app.world.ctx().get<SelfTest>();
    const auto& scene = app.world.ctx().get<projv::Scene>();
    auto now = std::chrono::steady_clock::now();
    double frameMs = std::chrono::duration<double, std::milli>(now - stats.last).count();
    stats.last = now;
    stats.worstFrameMs = std::max(stats.worstFrameMs, frameMs);
    size_t resident = projv::utils::residentVoxelBytes(scene);
    stats.peakResident = std::max(stats.peakResident, resident);

    stats.windowStart += app.time().unscaledDelta;
    if (stats.windowStart >= 1.0) {
        projv::utils::LoaderStats s = projv::runtime::worldLoader(app.world).stats();
        projv::core::info("World loading: {}/{} blocks loaded, {} wanted, {} reading, {:.1f} MB resident, worst frame {:.1f} ms",
                          s.loaded, s.blocks, s.wanted, s.pendingReads, resident / 1048576.0, stats.worstFrameMs);
        stats.windowStart = 0.0;
        stats.worstFrameMs = 0.0;
    }

    if (!test.on) return;
    test.frame++;
    test.peakResident = std::max(test.peakResident, resident);
    // Every second, once the first loads are done: is everything within 64 m there?
    if (test.frame > 120 && test.frame % 60 == 0 && !nearIsLoaded(app.world, 64.0f)) {
        std::printf("WORLDTEST: frame %d: blocks within 64 m of the camera were not loaded\n", test.frame);
        test.failed = true;
    }
    if (test.frame == 60 * 25) {
        double share = double(test.peakResident) / double(std::max<size_t>(test.wholeWorld, 1));
        std::printf("WORLDTEST: peak resident %.1f MB of %.1f MB for the whole world (%.0f%%)\n",
                    test.peakResident / 1048576.0, test.wholeWorld / 1048576.0, share * 100.0);
        if (share > 0.5) {
            std::printf("WORLDTEST: more than half the world was resident at once\n");
            test.failed = true;
        }
        std::printf("WORLDTEST: %s\n", test.failed ? "FAIL" : "PASS");
        std::exit(test.failed ? 1 : 0);
    }
}

// -----------------------------------------------------------------------------------------
// Stages
// -----------------------------------------------------------------------------------------

bool g_regenerate = false;

void startup(projv::Application& app) {
    const std::filesystem::path here = projv::core::executableDirectory();
    auto& renderInstance = app.world.ctx().emplace<projv::graphics::RenderInstance>();
    renderInstance.initialize(1280, 720, "ProjectV World Loading");
    projv::graphics::installPlatform(app, renderInstance);
    app.world.ctx().emplace<Camera>();
    app.world.ctx().emplace<Stats>();
    auto& test = app.world.ctx().emplace<SelfTest>();
    test.on = std::getenv("WORLD_LOADING_SELFTEST") != nullptr;
    if (!test.on) projv::graphics::setCursorCaptured(app, renderInstance, true);

    const std::filesystem::path folder = here / "terrain";
    if (g_regenerate || !std::filesystem::exists(folder / "compose.json")) {
        if (!terrain::write(folder)) {
            projv::core::error("World loading: could not write the terrain");
            std::exit(1);
        }
    }
    if (test.on) test.wholeWorld = projv::utils::residentVoxelBytes(projv::utils::loadComposeFromDisk(folder.string()));

    projv::runtime::WorldLoadingConfig config;
    config.policy = projv::utils::loadNear(96.0f, 192.0f);
    if (!projv::runtime::openWorld(app, folder.string(), config)) std::exit(1);

    projv::RendererSpecification specification =
        projv::graphics::loadRendererSpecification((here / "worldRenderer").string() + "/");
    renderInstance.addRendererSpecification(1, specification);
    bgfx::ShaderHandle vertexShader = projv::graphics::loadShader((here / "worldRenderer/worldShaders/vs_quad.bin").string());
    renderInstance.setActiveRenderer(
        projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));

    auto& scene = app.world.ctx().get<projv::Scene>();
    app.world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));
    projv::core::info("World loading: {} blocks in the world, none loaded yet", projv::runtime::worldLoader(app.world).blocks().size());
}

void render(projv::Application& app) {
    auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
    auto& scene = app.world.ctx().get<projv::Scene>();
    auto& gpuData = app.world.ctx().get<projv::GPUData>();
    const auto& camera = app.world.ctx().get<Camera>();

    // Blocks that arrived are new chunks; blocks that went are freed ones. This uploads both.
    projv::graphics::flushSceneUpdates(scene, gpuData);

    vec3 direction{std::cos(camera.pitch) * std::cos(camera.yaw), std::sin(camera.pitch),
                   std::cos(camera.pitch) * std::sin(camera.yaw)};
    auto renderer = renderInstance.getActiveRenderer();
    projv::graphics::setUniformToValue(renderer, "cameraPos", camera.position);
    projv::graphics::setUniformToValue(renderer, "cameraDir", direction);
    projv::core::vec2 resolution = renderInstance.getWindowResolution();
    projv::graphics::setUniformToValue(renderer, "windowRes", resolution);
    projv::graphics::renderConstructedRenderer(renderInstance, renderer, &gpuData);
}

void shutdown(projv::Application& app) {
    projv::graphics::destroyGPUData(app.world.ctx().get<projv::GPUData>());
}

// The self-test's course with no window, GPU or Application: the loader alone, as a program that
// only wants the voxels (a server, a tool) would use it.
int headlessCheck() {
    const std::filesystem::path folder = projv::core::executableDirectory() / "terrain";
    if (!std::filesystem::exists(folder / "compose.json") && !terrain::write(folder)) return 1;
    const size_t whole = projv::utils::residentVoxelBytes(projv::utils::loadComposeFromDisk(folder.string()));

    projv::Scene scene;
    projv::utils::SceneLoader loader;
    if (!loader.open(scene, folder.string())) return 1;
    loader.setRule(projv::utils::loadNear(96.0f, 192.0f));
    size_t peak = 0;
    bool failed = false;
    auto start = std::chrono::steady_clock::now();
    double worstUpdateMs = 0.0;
    for (int frame = 0; frame < 60 * 25; frame++) {
        vec3 at = coursePoint(float(frame) / 60.0f);
        loader.setViewers({{at, 0}});
        auto t0 = std::chrono::steady_clock::now();
        loader.update(scene);
        worstUpdateMs = std::max(worstUpdateMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        peak = std::max(peak, projv::utils::residentVoxelBytes(scene));
        if (frame > 120 && frame % 60 == 0) {
            for (projv::utils::BlockId id = 0; id < loader.blocks().size(); id++) {
                if (projv::utils::distanceToBox(loader.blocks().bounds(scene, id), at) <= 64.0f && !loader.blocks().loaded(id)) {
                    std::printf("WORLDTEST: frame %d: block %u within 64 m not loaded\n", frame, id);
                    failed = true;
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));   // a frame's worth of time for the worker
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("WORLDTEST: %zu blocks, peak resident %.1f MB of %.1f MB (%.0f%%), worst update %.2f ms, %.1f s\n",
                loader.blocks().size(), peak / 1048576.0, whole / 1048576.0, 100.0 * double(peak) / double(whole),
                worstUpdateMs, seconds);
    if (double(peak) > 0.5 * double(whole)) failed = true;

    // The same course with physics: the terrain one static collider, rebuilt as its blocks come and
    // go. What a frame costs then -- loader, rebuilds and the step together.
    {
        projv::Application app;
        double now = 0.0;
        app.clock = [&] { return now; };
        projv::runtime::WorldLoadingConfig config;
        config.policy = projv::utils::loadNear(96.0f, 192.0f);
        if (!projv::runtime::openWorld(app, folder.string(), config)) return 1;
        projv::runtime::installPhysics(app);
        projv::Entity ground = projv::runtime::spawnComponent(app.world, 0);
        app.world.emplace<projv::StaticCollider>(ground);
        app.runStartup();
        app.runFrame();
        double worst = 0.0, worstAfter = 0.0, total = 0.0;
        int frames = 60 * 25;
        for (int frame = 0; frame < frames; frame++) {
            projv::runtime::setViewers(app.world, {{coursePoint(float(frame) / 60.0f), 0}});
            now += 1.0 / 60.0;
            auto t0 = std::chrono::steady_clock::now();
            app.runFrame();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            worst = std::max(worst, ms);
            if (frame >= 120) worstAfter = std::max(worstAfter, ms);   // after the first loads around the start
            total += ms;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        std::printf("WORLDTEST: with physics: %.2f ms a frame on average, %.2f ms at worst (%.2f ms after the first "
                    "two seconds); the terrain %s a body\n",
                    total / frames, worst, worstAfter, projv::runtime::hasBody(app.world, ground) ? "has" : "has no");
        if (!projv::runtime::hasBody(app.world, ground)) failed = true;
    }
    std::printf("WORLDTEST: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}

int main(int argc, char** argv) {
    g_regenerate = argc >= 2 && std::strcmp(argv[1], "--regenerate") == 0;
    if (argc >= 2 && std::strcmp(argv[1], "--headless-check") == 0) return headlessCheck();

    projv::Application app;
    app.addSystem(projv::Stage::Startup,  "startup", startup);
    app.addSystem(projv::Stage::Update,   "camera",  cameraSystem);
    app.addSystem(projv::Stage::Update,   "stats",   statsSystem);
    app.addSystem(projv::Stage::Render,   "render",  render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    return 0;
}
