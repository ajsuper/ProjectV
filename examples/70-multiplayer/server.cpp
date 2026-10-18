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

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "arena.h"
#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "net/transport.h"
#include "runtime/entities.h"
#include "runtime/physics.h"
#include "runtime/replication.h"
#include "runtime/scene_bridge.h"

using projv::core::vec3;

namespace {
    struct Options {
        uint16_t port = arena::PORT;
        double   seconds = 0.0;   // 0: forever
    };

    // Where each new player starts: round the middle of the arena.
    vec3 spawnPoint(projv::runtime::ClientId id) {
        const float angle = float(id) * 2.39996f;   // the golden angle: no two land together
        return vec3(std::cos(angle) * 4.0f, 0.0f, std::sin(angle) * 4.0f);
    }
}

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--write-arena") && i + 1 < argc) return arena::write(argv[++i]) ? 0 : 1;
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) options.port = uint16_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) options.seconds = std::atof(argv[++i]);
    }

    const std::string folder = (projv::core::executableDirectory() / arena::FOLDER).string();
    projv::Application app;
    app.world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(folder));
    projv::runtime::installSceneBridge(app);
    projv::runtime::installPhysics(app);
    projv::runtime::spawnEntities(app.world);

    projv::runtime::ServerConfig config;
    config.port = options.port;
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
    if (!projv::runtime::installServer(app, projv::net::makeEnetTransport(), config)) return 1;

    // Each second, who is here and where.
    double logClock = 0.0, total = 0.0;
    app.addSystem(projv::Stage::Update, "report", [&](projv::Application& a) {
        logClock += a.time().unscaledDelta;
        total += a.time().unscaledDelta;
        if (logClock >= 1.0) {
            logClock = 0.0;
            std::string where;
            for (auto [e, character] : a.world.view<projv::PhysicsCharacter>().each()) {
                const vec3 p = projv::runtime::characterState(a.world, e).position;
                const projv::NetId* id = a.world.try_get<projv::NetId>(e);
                where += " player " + std::to_string(id ? id->value : 0) + " at (" + std::to_string(p.x) + ", " +
                         std::to_string(p.y) + ", " + std::to_string(p.z) + ")";
            }
            projv::core::info("MPSERVER: {} client(s){}", projv::runtime::sessionStats(a.world).clients, where);
        }
        if (options.seconds > 0.0 && total >= options.seconds) a.closeRequested = true;
    });
    // No window to wait on: sleep through the time no fixed step is owed, rather than spin.
    app.addSystem(projv::Stage::Render, "pace", [](projv::Application&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });
    app.run();
    return 0;
}
