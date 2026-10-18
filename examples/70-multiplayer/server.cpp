// ProjectV Multiplayer: the server
//
// A dedicated server: no window, no renderer. It loads the arena, simulates it -- physics, every
// player's character -- and sends it to whoever connects (runtime/replication.h). Clients send what
// their player wants to do; this is the only place anything moves.
//
//   ./mp_server                      serve on port 27015 until stopped (Ctrl-C)
//   ./mp_server --port 27100         another port
//   ./mp_server --seconds 10         stop after ten seconds (the self-test)
//   ./mp_server --write-arena <dir>  regenerate the arena this example ships
//
// Each second it logs who is connected and where their players are.
//
// The game's rules live here, because they are the server's: a client asks (sendToServer) to throw
// a ball or to blast what it aims at, and the server decides. Breaking on impact is the server's
// policy too (installImpactFracture); what breaks reaches every client as it happened.

#include <chrono>
#include <cmath>
#include <span>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "arena.h"
#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "net/transport.h"
#include "net/wire.h"
#include "runtime/entities.h"
#include "runtime/physics.h"
#include "runtime/replication.h"
#include "runtime/scene_bridge.h"

using projv::core::vec3;

namespace {
    struct Options {
        uint16_t port = arena::PORT;
        uint32_t maxClients = 128;
        double   seconds = 0.0;   // 0: forever
    };

    // Where each new player starts: a sunflower round the middle, so a hundred land apart.
    vec3 spawnPoint(projv::runtime::ClientId id) {
        const float angle = float(id) * 2.39996f;   // the golden angle
        const float radius = 3.0f + 1.3f * std::sqrt(float(id));
        return vec3(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);
    }
}

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--write-arena") && i + 1 < argc) return arena::write(argv[++i]) ? 0 : 1;
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) options.port = uint16_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) options.seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--max-clients") && i + 1 < argc) options.maxClients = uint32_t(std::atoi(argv[++i]));
    }

    const std::string folder = (projv::core::executableDirectory() / arena::FOLDER).string();
    projv::Application app;
    app.world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(folder));
    projv::runtime::installSceneBridge(app);
    // Fragments of one or two voxels are dust: cut, and made into nothing -- no body to simulate or
    // send. Clients throw out chips of their own where things break, for the look of it. Keep it below
    // the materials' shards (stone breaks into 0.5 m shards: eight voxels of the tower's): at 12 every
    // shard was dust, and the rubble vanished.
    projv::runtime::PhysicsConfig physics;
    physics.dustVoxels = 2;
    projv::runtime::installPhysics(app, physics);
    projv::runtime::installImpactFracture(app);
    projv::runtime::registerComponent<arena::Thrower>(app.world);
    projv::runtime::loadPhysicsMaterials(app.world, (std::filesystem::path(folder) / arena::MATERIALS).string());
    projv::runtime::spawnEntities(app.world);

    projv::runtime::ServerConfig config;
    config.port = options.port;
    config.maxClients = options.maxClients;
    config.sceneFolder = folder;
    const std::string avatar = (std::filesystem::path(folder) / arena::AVATAR).string();
    // A player: the avatar prefab walked by a character controller. Clients get the prefab and
    // the entity's data, not the controller: the server walks it.
    config.spawnPlayer = [avatar](projv::World& world, projv::runtime::ClientId id) {
        projv::Entity e = projv::runtime::instantiatePrefab(world, avatar, spawnPoint(id));
        world.emplace<projv::CharacterController>(e);
        world.emplace<projv::Replicated>(e, projv::Replicated{arena::AVATAR, {"physics.character"}});
        return e;
    };
    // What players ask for: a ball thrown, or a blast where they aim. From an eye near their player,
    // so a client cannot throw from across the map.
    const std::string ball = (std::filesystem::path(folder) / arena::BALL).string();
    struct Thrown { double at; };
    double clock = 0.0;
    config.onMessage = [ball, &clock](projv::World& world, projv::runtime::ClientId from, projv::Entity player,
                                      std::span<const uint8_t> message) {
        projv::net::BitReader r(message);
        const uint64_t kind = r.bits(8);
        vec3 eye, aim;
        for (int i = 0; i < 3; i++) eye[i] = r.f32();
        for (int i = 0; i < 3; i++) aim[i] = r.f32();
        if (r.failed || !std::isfinite(glm::dot(eye, eye) + glm::dot(aim, aim)) || glm::length(aim) < 0.5f) return;
        aim = glm::normalize(aim);
        if (!world.valid(player) || glm::distance(eye, projv::runtime::characterState(world, player).position) > 12.0f) return;
        if (kind == 1) {
            projv::Entity e = projv::runtime::instantiatePrefab(world, ball, eye + aim * 1.5f);
            projv::RigidBody body;
            body.shape = projv::RigidBody::Shape::Sphere;
            body.material = "iron";
            body.quality = projv::RigidBody::Quality::Continuous;
            world.emplace<projv::RigidBody>(e, body);
            world.emplace<arena::Thrower>(e, arena::Thrower{from});
            world.emplace<projv::Replicated>(e, projv::Replicated{arena::BALL, {}});
            world.emplace<Thrown>(e, Thrown{clock});
            projv::runtime::setVelocity(world, e, aim * 32.0f);
        } else if (kind == 2) {
            auto hit = projv::runtime::castRay(world, eye, aim, 80.0f, 0xffffffffu, player);
            if (!hit) return;
            if (projv::runtime::canBreak(world, hit->entity)) projv::runtime::fracture(world, hit->entity, hit->position, 4.0e5f, aim * 6.0f);
            for (projv::Entity near : projv::runtime::overlapSphere(world, hit->position, 4.0f)) {
                if (!world.all_of<projv::RigidBody>(near)) continue;
                const vec3 away = projv::runtime::bodyPose(world, near).position - hit->position;
                const float d = std::max(glm::length(away), 0.5f);
                projv::runtime::addVelocity(world, near, away / d * (8.0f / d) + vec3(0, 3, 0));
            }
        }
    };
    if (!projv::runtime::installServer(app, projv::net::makeEnetTransport(), config)) return 1;
    // A piece broken again down to nothing is gone: destroyed, so it is despawned everywhere.
    app.events().on<projv::BodyEmptied>([&app](const projv::BodyEmptied& e) {
        if (app.world.valid(e.entity) && app.world.all_of<projv::Piece>(e.entity)) app.world.destroy(e.entity);
    });
    // Balls go after ten seconds.
    app.addSystem(projv::Stage::Update, "balls", [&](projv::Application& a) {
        clock += a.time().delta;
        std::vector<projv::Entity> old;
        for (auto [e, thrown] : a.world.view<Thrown>().each())
            if (clock - thrown.at > 10.0) old.push_back(e);
        for (projv::Entity e : old) a.world.destroy(e);
    });

    // How long a frame's work takes (the sleep below is not work), measured from the frame's first
    // system to its last.
    using Clock = std::chrono::steady_clock;
    Clock::time_point frameStart = Clock::now();
    double workTotal = 0.0, workWorst = 0.0;
    int workFrames = 0;
    app.addSystem(projv::Stage::PreUpdate, "frame start", [&](projv::Application&) { frameStart = Clock::now(); },
                  {.before = "net: receive", .after = ""});
    // And each simulation step on its own: a frame that catches up runs two.
    Clock::time_point stepStart = Clock::now();
    double stepTotal = 0.0, stepWorst = 0.0;
    int steps = 0;
    app.addSystem(projv::Stage::FixedUpdate, "step start", [&](projv::Application&) { stepStart = Clock::now(); },
                  {.before = "net: input", .after = ""});
    app.addSystem(projv::Stage::FixedUpdate, "step end", [&](projv::Application&) {
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - stepStart).count();
        stepTotal += ms;
        stepWorst = std::max(stepWorst, ms);
        steps++;
    }, {.before = "", .after = "physics: impact fracture"});

    // Each second, who is here, where, and what it costs.
    double logClock = 0.0, total = 0.0;
    uint64_t lastSent = 0;
    app.addSystem(projv::Stage::Update, "report", [&](projv::Application& a) {
        logClock += a.time().unscaledDelta;
        total += a.time().unscaledDelta;
        if (logClock >= 1.0) {
            std::string where;
            size_t listed = 0;
            for (auto [e, character] : a.world.view<projv::PhysicsCharacter>().each()) {
                if (listed++ >= 4) { where += " ..."; break; }
                const vec3 p = projv::runtime::characterState(a.world, e).position;
                const projv::NetId* id = a.world.try_get<projv::NetId>(e);
                where += " player " + std::to_string(id ? id->value : 0) + " at (" + std::to_string(p.x) + ", " +
                         std::to_string(p.y) + ", " + std::to_string(p.z) + ")";
            }
            uint64_t sent = 0;
            float worstTrip = 0.0f;
            for (const auto& c : projv::runtime::clientStats(a.world)) {
                sent += c.bytesSent;
                worstTrip = std::max(worstTrip, c.roundTripMs);
            }
            const double kbps = double(sent > lastSent ? sent - lastSent : 0) / logClock / 1024.0;
            lastSent = sent;
            size_t pieces = 0, pieceVoxels = 0;
            for (auto [e, piece, link] : a.world.view<projv::Piece, projv::VoxelComponent>().each()) {
                pieces++;
                pieceVoxels += projv::utils::getComponentVoxelCount(a.world.ctx().get<projv::Scene>(), link.handle);
            }
            projv::core::info("MPSERVER: {} client(s), frames {:.2f} ms average {:.2f} ms worst, steps {:.2f} ms average "
                              "{:.2f} ms worst, {:.0f} KB/s out, worst round trip {:.0f} ms, {} pieces of {} voxels{}",
                              projv::runtime::sessionStats(a.world).clients, workFrames ? workTotal / workFrames : 0.0, workWorst,
                              steps ? stepTotal / steps : 0.0, stepWorst, kbps, worstTrip, pieces, pieceVoxels, where);
            logClock = 0.0;
            workTotal = workWorst = stepTotal = stepWorst = 0.0;
            workFrames = steps = 0;
        }
        if (options.seconds > 0.0 && total >= options.seconds) a.closeRequested = true;
    });
    // No window to wait on: sleep through the time no fixed step is owed, rather than spin.
    app.addSystem(projv::Stage::Render, "pace", [&](projv::Application&) {
        const double work = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
        workTotal += work;
        workWorst = std::max(workWorst, work);
        workFrames++;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });
    app.run();
    return 0;
}
