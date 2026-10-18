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
    projv::runtime::installPhysics(app);
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
    if (!projv::runtime::installServer(app, projv::net::makeEnetTransport(), config)) return 1;

    // How long a frame's work takes (the sleep below is not work), measured from the frame's first
    // system to its last.
    using Clock = std::chrono::steady_clock;
    Clock::time_point frameStart = Clock::now();
    double workTotal = 0.0, workWorst = 0.0;
    int workFrames = 0;
    app.addSystem(projv::Stage::PreUpdate, "frame start", [&](projv::Application&) { frameStart = Clock::now(); },
                  {.before = "net: receive", .after = ""});

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
            projv::core::info("MPSERVER: {} client(s), frames {:.2f} ms average {:.2f} ms worst, {:.0f} KB/s out, worst round trip {:.0f} ms{}",
                              projv::runtime::sessionStats(a.world).clients, workFrames ? workTotal / workFrames : 0.0, workWorst,
                              kbps, worstTrip, where);
            logClock = 0.0;
            workTotal = workWorst = 0.0;
            workFrames = 0;
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
