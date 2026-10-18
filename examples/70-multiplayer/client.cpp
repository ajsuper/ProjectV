// ProjectV Multiplayer: a client
//
// Connects to an mp_server, shows the arena and every player in it, and sends what you do with your
// own. Nothing here moves anything: the server walks every player, this draws where it says they
// are (runtime/replication.h). No prediction yet, so your own player answers a round trip late.
//
//   ./mp_client                          connect to 127.0.0.1:27015, in a window
//   ./mp_client --host 10.0.0.5 --port 27100 --name ann
//   ./mp_client --headless ...           no window: for the self-test (selftest.sh)
//       --walk 0,3        walk at this velocity (x,z m/s) once joined
//       --walk-seconds 2  for this long
//       --seconds 8       then quit, after checking:
//       --expect-players 2    seeing this many players
//       --expect-moved        another player moved more than 3 m while watched
//
// Controls (windowed): W/S/A/D walk, Space jumps, the mouse turns the camera (Esc releases it, a
// click recaptures).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

#include "arena.h"
#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "graphics/gpu_interface.h"
#include "graphics/input.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "graphics/disk_io.h"
#include "net/transport.h"
#include "runtime/net.h"
#include "runtime/replication.h"
#include "runtime/scene_bridge.h"

using projv::core::vec3;

namespace {
    struct Options {
        std::string host = "127.0.0.1";
        uint16_t    port = arena::PORT;
        std::string name = "player";
        bool        headless = false;
        vec3        walk{0.0f};
        double      walkSeconds = 0.0;
        double      seconds = 0.0;
        int         expectPlayers = -1;
        bool        expectMoved = false;
    };

    struct Camera {
        float yaw = 0.0f, pitch = -0.35f;
    };

    // What a headless client saw, for its checks.
    struct Watch {
        double joinedAt = -1.0, clock = 0.0;
        std::map<uint32_t, vec3> firstSeen;
        float moved = 0.0f;
        bool  failed = false;
    };

    Options options;
    int exitCode = 0;

    vec3 ownPosition(projv::World& world) {
        projv::Entity me = projv::runtime::entityForNetId(world, projv::runtime::sessionStats(world).player);
        if (me == projv::NullEntity || !world.all_of<projv::Transform>(me)) return vec3(0.0f);
        return world.get<projv::Transform>(me).position;
    }

    void playSystem(projv::Application& app) {
        auto& camera = app.world.ctx().get<Camera>();
        auto& watch = app.world.ctx().get<Watch>();
        watch.clock += app.time().unscaledDelta;
        const bool joined = projv::runtime::sessionStats(app.world).connected;
        if (joined && watch.joinedAt < 0.0) watch.joinedAt = watch.clock;

        if (options.headless) {
            const bool walking = joined && watch.clock - watch.joinedAt < options.walkSeconds;
            projv::runtime::setLocalInput(app.world, walking ? options.walk : vec3(0.0f));
            return;
        }
        auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
        const auto& input = app.world.ctx().get<projv::Input>();
        if (input.cursorCaptured && input.pressed(projv::Key::Escape)) projv::graphics::setCursorCaptured(app, renderInstance, false);
        else if (!input.cursorCaptured && input.pressed(projv::MouseButton::Left)) projv::graphics::setCursorCaptured(app, renderInstance, true);
        if (input.cursorCaptured) {
            camera.yaw += input.cursorDelta.x * 0.0025f;
            camera.pitch = std::clamp(camera.pitch - input.cursorDelta.y * 0.0025f, -1.2f, 0.3f);
        }
        const vec3 forward{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
        const vec3 right{-std::sin(camera.yaw), 0.0f, std::cos(camera.yaw)};
        vec3 move(0.0f);
        if (input.down(projv::Key::W)) move += forward;
        if (input.down(projv::Key::S)) move -= forward;
        if (input.down(projv::Key::D)) move += right;
        if (input.down(projv::Key::A)) move -= right;
        if (glm::length(move) > 0.0f) move = glm::normalize(move) * 4.0f;
        projv::runtime::setLocalInput(app.world, move, input.pressed(projv::Key::Space));
    }

    // Headless: note how far the other players went, and finish on time.
    void watchSystem(projv::Application& app) {
        auto& watch = app.world.ctx().get<Watch>();
        const projv::runtime::SessionStats stats = projv::runtime::sessionStats(app.world);
        size_t players = 0;
        for (auto [e, proxy, id, t] : app.world.view<projv::NetProxy, projv::NetId, projv::Transform>().each()) {
            players++;
            if (id == stats.player) continue;
            auto [it, fresh] = watch.firstSeen.try_emplace(id.value, t.position);
            if (!fresh) watch.moved = std::max(watch.moved, glm::distance(it->second, t.position));
        }
        if (options.seconds <= 0.0 || watch.clock < options.seconds) return;
        std::printf("MPTEST: %s: %s as client %u, sees %zu players, another moved %.2f m, own player at (%.2f, %.2f)\n",
                    options.name.c_str(), stats.connected ? "joined" : "not joined", unsigned(stats.you), players,
                    watch.moved, ownPosition(app.world).x, ownPosition(app.world).z);
        bool ok = stats.connected;
        if (options.expectPlayers >= 0 && int(players) != options.expectPlayers) ok = false;
        if (options.expectMoved && watch.moved < 3.0f) ok = false;
        std::printf("MPTEST: %s: %s\n", options.name.c_str(), ok ? "PASS" : "FAIL");
        std::fflush(stdout);
        exitCode = ok ? 0 : 1;
        app.closeRequested = true;
    }

    void startup(projv::Application& app) {
        const std::filesystem::path here = projv::core::executableDirectory();
        app.world.ctx().emplace<Camera>();
        app.world.ctx().emplace<Watch>();
        if (!options.headless) {
            auto& renderInstance = app.world.ctx().emplace<projv::graphics::RenderInstance>();
            renderInstance.initialize(1280, 720, ("ProjectV Multiplayer: " + options.name).c_str());
            projv::graphics::installPlatform(app, renderInstance);
            projv::graphics::setCursorCaptured(app, renderInstance, true);
        }
        const std::string folder = (here / arena::FOLDER).string();
        auto& scene = app.world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(folder));

        projv::runtime::ClientConfig config;
        config.host = options.host;
        config.port = options.port;
        config.sceneFolder = folder;
        config.name = options.name;
        if (!projv::runtime::installClient(app, projv::net::makeEnetTransport(), config)) std::exit(1);
        app.events().on<projv::runtime::LeftServer>([&app](const projv::runtime::LeftServer& e) {
            projv::core::warn("Multiplayer: left the server: {}", e.reason);
            if (options.headless) {
                std::printf("MPTEST: %s: left the server: %s\nMPTEST: %s: FAIL\n", options.name.c_str(), e.reason.c_str(),
                            options.name.c_str());
                exitCode = 1;
                app.closeRequested = true;
            }
        });

        if (options.headless) return;
        auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
        projv::RendererSpecification specification =
            projv::graphics::loadRendererSpecification((here / "clientRenderer").string() + "/");
        renderInstance.addRendererSpecification(1, specification);
        bgfx::ShaderHandle vertexShader = projv::graphics::loadShader((here / "clientRenderer/clientShaders/vs_quad.bin").string());
        renderInstance.setActiveRenderer(
            projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));
        app.world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));
    }

    void render(projv::Application& app) {
        if (options.headless) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return;
        }
        auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
        auto& scene = app.world.ctx().get<projv::Scene>();
        auto& gpuData = app.world.ctx().get<projv::GPUData>();
        const auto& camera = app.world.ctx().get<Camera>();
        // Players come and go as chunks; this uploads them, and where they moved to.
        projv::graphics::flushSceneUpdates(scene, gpuData);

        // Behind and above your own player, looking where the camera faces.
        vec3 direction{std::cos(camera.pitch) * std::cos(camera.yaw), std::sin(camera.pitch),
                       std::cos(camera.pitch) * std::sin(camera.yaw)};
        vec3 eye = ownPosition(app.world) + vec3(0.0f, 1.6f, 0.0f) - direction * 7.0f;
        auto renderer = renderInstance.getActiveRenderer();
        projv::core::vec2 resolution = renderInstance.getWindowResolution();
        projv::graphics::setUniformToValue(renderer, "cameraPos", eye);
        projv::graphics::setUniformToValue(renderer, "cameraDir", direction);
        projv::graphics::setUniformToValue(renderer, "windowRes", resolution);
        projv::graphics::renderConstructedRenderer(renderInstance, renderer, &gpuData);
    }

    void shutdown(projv::Application& app) {
        if (auto* gpu = app.world.ctx().find<projv::GPUData>()) projv::graphics::destroyGPUData(*gpu);
    }

    vec3 parseWalk(const char* text) {
        float x = 0.0f, z = 0.0f;
        std::sscanf(text, "%f,%f", &x, &z);
        return vec3(x, 0.0f, z);
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--host")) options.host = next();
        else if (!std::strcmp(argv[i], "--port")) options.port = uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--name")) options.name = next();
        else if (!std::strcmp(argv[i], "--headless")) options.headless = true;
        else if (!std::strcmp(argv[i], "--walk")) options.walk = parseWalk(next());
        else if (!std::strcmp(argv[i], "--walk-seconds")) options.walkSeconds = std::atof(next());
        else if (!std::strcmp(argv[i], "--seconds")) options.seconds = std::atof(next());
        else if (!std::strcmp(argv[i], "--expect-players")) options.expectPlayers = std::atoi(next());
        else if (!std::strcmp(argv[i], "--expect-moved")) options.expectMoved = true;
    }

    projv::Application app;
    app.addSystem(projv::Stage::Startup,  "startup", startup);
    app.addSystem(projv::Stage::Update,   "play",    playSystem);
    app.addSystem(projv::Stage::Update,   "watch",   watchSystem);
    app.addSystem(projv::Stage::Render,   "render",  render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    return exitCode;
}
