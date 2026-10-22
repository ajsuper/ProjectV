// ProjectV Multiplayer: a client
//
// Connects to an mp_server, shows the arena and every player in it, and sends what you do with your
// own. The server walks every player; this one walks yours too, at once, and is put right when the
// two differ (prediction, runtime/replication.h). Everyone else is drawn where the server says.
//
//   ./mp_client                          connect to 127.0.0.1:27015, in a window
//   ./mp_client --host 10.0.0.5 --port 27100 --name ann
//   ./mp_client --headless ...           no window: for the self-test (selftest.sh)
//       --walk 0,3        walk at this velocity (x,z m/s) once joined
//       --walk-seconds 2  for this long
//       --seconds 8       then quit, after checking:
//       --expect-players 2    seeing this many players
//       --expect-moved        another player moved more than 3 m while watched
//       --throw 3 --aim 15,4,10   throw three balls at that point, then blast it
//       --expect-breaks       something broke, and every break matched the server's
//
// Controls (windowed): W/S/A/D walk, Space jumps, the mouse turns the camera (Esc releases it, a
// click recaptures). Left click throws an iron ball; right click blasts what the crosshair is on.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <map>
#include <string>
#include <thread>

#include "arena.h"
#include "basic_renderer.h"
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
#include "net/wire.h"
#include "runtime/entities.h"
#include "runtime/net.h"
#include "runtime/physics.h"
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
        int         throws = 0;            // headless: balls to throw at `aim`, then a blast there
        vec3        aim{0.0f};
        bool        expectBreaks = false;  // something broke here, and every break was the server's
    };

    struct Camera {
        float yaw = 0.0f, pitch = -0.35f;
    };

    // What a headless client saw, for its checks.
    struct Watch {
        double joinedAt = -1.0, clock = 0.0, lastShot = 0.0;
        int    shots = 0;
        std::map<uint32_t, vec3> firstSeen;
        float moved = 0.0f;
        bool  failed = false;
    };

    Options options;
    int exitCode = 0;

    // ---- What only this client shows: a stand-in for a ball it threw, and chips where things break.
    //
    // Neither is the engine's, and neither is sent anywhere: the server's ball replaces the stand-in
    // when it comes, and chips are for the look of a break (the server turns the smallest fragments
    // into dust, PhysicsConfig::dustVoxels, so nothing that small is simulated or sent).
    struct StandIn { double at; };
    struct Claimed {};
    struct Chip { double at; };
    struct Burst { vec3 centre; float size; std::vector<uint32_t> colours; int count; };
    struct Effects {
        double clock = 0.0;
        std::deque<projv::Entity> standIns;
        std::vector<Burst> bursts;
        std::mt19937 random{7};
    };
    constexpr size_t MAX_CHIPS = 160;

    // A ball of our own, at once, flying as the server's will: shown until the server's arrives.
    void throwStandIn(projv::World& world, vec3 from, vec3 aim) {
        auto& effects = world.ctx().get<Effects>();
        const std::string ball = (projv::core::executableDirectory() / arena::FOLDER / arena::BALL).string();
        projv::Entity e = projv::runtime::instantiatePrefab(world, ball, from + aim * 1.5f);
        if (e == projv::NullEntity) return;
        projv::RigidBody body;
        body.shape = projv::RigidBody::Shape::Sphere;
        body.material = "iron";
        body.quality = projv::RigidBody::Quality::Continuous;
        world.emplace<projv::RigidBody>(e, body);
        world.emplace<StandIn>(e, StandIn{effects.clock});
        projv::runtime::setVelocity(world, e, aim * 32.0f);
        effects.standIns.push_back(e);
    }

    // Where something broke, as the server said: a burst of chips in its colours.
    void noteBreak(projv::World& world, const projv::BodyBroken& event) {
        const auto& scene = world.ctx().get<projv::Scene>();
        const projv::ComponentHandle h = projv::runtime::voxelComponentOf(world, event.entity);
        if (h == projv::INVALID_COMPONENT_HANDLE) return;
        vec3 sum(0.0f);
        size_t n = 0;
        for (const auto& piece : event.pieceVoxels) for (auto v : piece) { sum += vec3(v) + 0.5f; n++; }
        for (auto v : event.dust) { sum += vec3(v) + 0.5f; n++; }
        if (n == 0) return;
        const projv::ComponentRecord& rec = scene.components[h];
        float voxel = 0.25f;
        const projv::ChunkHandle first = rec.kind == projv::ComponentKind::Chunk ? rec.chunkHandle
            : [&] { for (int32_t c : scene.grids[size_t(rec.gridIndex)].cellToChunk) if (c >= 0) return projv::ChunkHandle(c); return projv::INVALID_CHUNK_HANDLE; }();
        if (first < scene.chunks.size()) voxel = scene.chunks[first].header.voxelScale;
        Burst burst;
        burst.centre = vec3(projv::utils::getComponentWorldMatrix(scene, h) * projv::core::vec4(sum / float(n) * (voxel / glm::length(vec3(projv::utils::getComponentWorldMatrix(scene, h)[0]))), 1.0f));
        burst.size = std::clamp(voxel * 0.6f, 0.05f, 0.15f);
        for (const auto& m : rec.materialPalette) burst.colours.push_back(m.packedColor);
        burst.count = int(std::min<size_t>(10 + event.dust.size() / 3, 36));
        world.ctx().get<Effects>().bursts.push_back(std::move(burst));
    }

    void effectsSystem(projv::Application& app) {
        projv::World& world = app.world;
        auto& effects = world.ctx().get<Effects>();
        effects.clock += app.time().delta;
        const projv::runtime::ClientId me = projv::runtime::sessionStats(world).you;
        // The server's ball for one we threw: it replaces the oldest stand-in.
        std::vector<projv::Entity> arrived;
        for (auto [e, thrower] : world.view<arena::Thrower>(entt::exclude<Claimed>).each()) arrived.push_back(e);
        for (projv::Entity e : arrived) {
            world.emplace<Claimed>(e);
            if (world.get<arena::Thrower>(e).client != me || effects.standIns.empty()) continue;
            if (world.valid(effects.standIns.front())) world.destroy(effects.standIns.front());
            effects.standIns.pop_front();
        }
        while (!effects.standIns.empty() && (!world.valid(effects.standIns.front()) ||
                                             effects.clock - world.get<StandIn>(effects.standIns.front()).at > 2.0)) {
            if (world.valid(effects.standIns.front())) world.destroy(effects.standIns.front());
            effects.standIns.pop_front();
        }
        // Chips: made for each burst, gone after three seconds, never more than MAX_CHIPS.
        std::vector<projv::Entity> old;
        for (auto [e, chip] : world.view<Chip>().each()) if (effects.clock - chip.at > 3.0) old.push_back(e);
        for (projv::Entity e : old) world.destroy(e);
        auto& scene = world.ctx().get<projv::Scene>();
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        for (const Burst& burst : effects.bursts) {
            for (int i = 0; i < burst.count && world.view<Chip>().size() < MAX_CHIPS; i++) {
                const uint32_t colour = burst.colours.empty() ? arena::rgb(0.5f, 0.5f, 0.5f)
                                                              : burst.colours[effects.random() % burst.colours.size()];
                projv::ComponentHandle h = projv::utils::addComponent(scene, projv::ComponentKind::Chunk, "chip",
                                                                      projv::INVALID_COMPONENT_HANDLE, 4, burst.size * 0.5f);
                std::vector<projv::PendingVoxelOp> cube;
                for (int k = 0; k < 8; k++) cube.push_back({true, projv::core::ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1), colour});
                projv::utils::queueVoxelAdd(scene, h, cube);
                projv::utils::updateScene(scene);
                const vec3 spread(unit(effects.random), unit(effects.random), unit(effects.random));
                projv::utils::setComponentTransform(scene, h, burst.centre + spread * 0.4f, projv::core::quat(1, 0, 0, 0), 1.0f);
                projv::Entity e = projv::runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
                projv::RigidBody body;
                body.density = 1500.0f;
                world.emplace<projv::RigidBody>(e, body);
                world.emplace<Chip>(e, Chip{effects.clock});
                projv::runtime::setVelocity(world, e, glm::normalize(spread + vec3(0, 1.2f, 0)) * (3.0f + 4.0f * std::abs(unit(effects.random))),
                                            spread * 8.0f);
            }
        }
        effects.bursts.clear();
    }

    vec3 ownPosition(projv::World& world) {
        projv::Entity me = projv::runtime::entityForNetId(world, projv::runtime::sessionStats(world).player);
        if (me == projv::NullEntity || !world.all_of<projv::Transform>(me)) return vec3(0.0f);
        return world.get<projv::Transform>(me).position;
    }

    // Asks the server to throw (kind 1) or blast (kind 2) from `from` along `aim`.
    void ask(projv::World& world, int kind, vec3 from, vec3 aim) {
        projv::net::BitWriter w;
        w.bits(uint64_t(kind), 8);
        for (int i = 0; i < 3; i++) w.f32(from[i]);
        for (int i = 0; i < 3; i++) w.f32(aim[i]);
        projv::runtime::sendToServer(world, w.data());
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
            // A second after joining, a shot every half second at the target: balls, then a blast.
            if (joined && watch.shots <= options.throws && options.throws > 0 && watch.clock - watch.joinedAt > 1.0 &&
                watch.clock - watch.lastShot > 0.5) {
                const vec3 from = ownPosition(app.world) + vec3(0.0f, 1.4f, 0.0f);
                ask(app.world, watch.shots < options.throws ? 1 : 2, from, glm::normalize(options.aim - from));
                if (watch.shots < options.throws) throwStandIn(app.world, from, glm::normalize(options.aim - from));
                watch.shots++;
                watch.lastShot = watch.clock;
            }
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

        // Left click throws a ball, right click blasts what the crosshair is on: asked of the server,
        // which decides. Aimed from the camera, through the crosshair.
        const bool throwing = input.cursorCaptured && input.pressed(projv::MouseButton::Left);
        const bool blasting = input.cursorCaptured && input.pressed(projv::MouseButton::Right);
        if (throwing || blasting) {
            const vec3 direction{std::cos(camera.pitch) * std::cos(camera.yaw), std::sin(camera.pitch),
                                 std::cos(camera.pitch) * std::sin(camera.yaw)};
            const vec3 eye = ownPosition(app.world) + vec3(0.0f, 1.6f, 0.0f) - direction * 7.0f;
            // From in front of the player, not from the camera behind it: the server checks the eye
            // is near the player.
            const vec3 from = ownPosition(app.world) + vec3(0.0f, 1.4f, 0.0f);
            const vec3 target = eye + direction * 60.0f;
            ask(app.world, throwing ? 1 : 2, throwing ? from : eye, throwing ? glm::normalize(target - from) : direction);
            if (throwing) throwStandIn(app.world, from, glm::normalize(target - from));
        }
    }

    // Headless: note how far the other players went, and finish on time.
    void watchSystem(projv::Application& app) {
        auto& watch = app.world.ctx().get<Watch>();
        const projv::runtime::SessionStats stats = projv::runtime::sessionStats(app.world);
        size_t players = 0;
        // Players: proxies that are not bodies (crates, balls and pieces are proxies too).
        for (auto [e, proxy, id, t] : app.world.view<projv::NetProxy, projv::NetId, projv::Transform>(entt::exclude<projv::RigidBody>).each()) {
            players++;
            if (id == stats.player) continue;
            auto [it, fresh] = watch.firstSeen.try_emplace(id.value, t.position);
            if (!fresh) watch.moved = std::max(watch.moved, glm::distance(it->second, t.position));
        }
        if (options.seconds <= 0.0 || watch.clock < options.seconds) return;
        if (stats.predicting) players++;   // its own player: walked here, not a proxy
        std::printf("MPTEST: %s: %s as client %u, sees %zu players, another moved %.2f m, own player at (%.2f, %.2f), "
                    "predicting %s, largest correction %.3f m\n",
                    options.name.c_str(), stats.connected ? "joined" : "not joined", unsigned(stats.you), players,
                    watch.moved, ownPosition(app.world).x, ownPosition(app.world).z, stats.predicting ? "yes" : "no",
                    stats.largestCorrection);
        bool ok = stats.connected;
        if (options.expectPlayers >= 0 && int(players) != options.expectPlayers) ok = false;
        if (options.expectMoved && watch.moved < 3.0f) ok = false;
        if (options.expectBreaks && (stats.worldChanges == 0 || stats.worldMismatches != 0)) ok = false;
        std::printf("MPTEST: %s: world changes %llu, mismatches %llu\n", options.name.c_str(),
                    (unsigned long long)stats.worldChanges, (unsigned long long)stats.worldMismatches);
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
        // Physics here too: the client walks its own player at once (prediction), and the server
        // puts it right when they differ.
        projv::runtime::installSceneBridge(app);
        projv::runtime::installPhysics(app);
        projv::runtime::loadPhysicsMaterials(app.world, (std::filesystem::path(folder) / arena::MATERIALS).string());
        projv::runtime::registerComponent<arena::Thrower>(app.world);
        projv::runtime::spawnEntities(app.world);
        app.world.ctx().emplace<Effects>();
        app.events().on<projv::BodyBroken>([&app](const projv::BodyBroken& e) { noteBreak(app.world, e); });

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
        // The feature examples' basic renderer (examples/shared/basicRenderer): the voxels' colours under
        // a fixed sun. This example is about the network; the party and playground demos draw better.
        basic_renderer::install(renderInstance, here);
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
        // Behind and above your own player, looking where the camera faces. Players, crates and rubble
        // come and go as chunks: draw uploads them, and where they moved to.
        vec3 direction{std::cos(camera.pitch) * std::cos(camera.yaw), std::sin(camera.pitch),
                       std::cos(camera.pitch) * std::sin(camera.yaw)};
        vec3 eye = ownPosition(app.world) + vec3(0.0f, 1.6f, 0.0f) - direction * 7.0f;
        // A crosshair: what a throw or a blast aims at.
        basic_renderer::draw(renderInstance, gpuData, scene, eye, direction, {.crosshair = true});
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
        else if (!std::strcmp(argv[i], "--throw")) options.throws = std::atoi(next());
        else if (!std::strcmp(argv[i], "--aim")) {
            float x = 0, y = 0, z = 0;
            std::sscanf(next(), "%f,%f,%f", &x, &y, &z);
            options.aim = vec3(x, y, z);
        }
        else if (!std::strcmp(argv[i], "--expect-breaks")) options.expectBreaks = true;
    }

    projv::Application app;
    app.addSystem(projv::Stage::Startup,  "startup", startup);
    app.addSystem(projv::Stage::Update,   "play",    playSystem);
    app.addSystem(projv::Stage::Update,   "watch",   watchSystem);
    app.addSystem(projv::Stage::Update,   "effects", effectsSystem);
    app.addSystem(projv::Stage::Render,   "render",  render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    return exitCode;
}
