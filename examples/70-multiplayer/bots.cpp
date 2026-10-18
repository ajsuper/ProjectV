// ProjectV Multiplayer: bots
//
// Many clients in one process, for loading a server: each its own Application and its own ENet
// connection, so the server sees real clients over real sockets. Bots have no physics and no window
// -- they walk their players in random directions and watch what they are sent -- so a hundred cost
// this machine little and the server what a hundred players cost it.
//
//   ./mp_bots --count 100 --seconds 20            against 127.0.0.1:27015
//   ./mp_bots --count 50 --host 10.0.0.5 --port 27100
//
// At the end it prints a BOTS line: how many joined, and what they were sent.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "arena.h"
#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "net/transport.h"
#include "runtime/replication.h"
#include "runtime/scene_bridge.h"

using projv::core::vec3;

int main(int argc, char** argv) {
    int count = 10;
    double seconds = 10.0;
    std::string host = "127.0.0.1";
    uint16_t port = arena::PORT;
    for (int i = 1; i < argc; i++) {
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--count")) count = std::max(1, std::atoi(next()));
        else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(next());
        else if (!std::strcmp(argv[i], "--host")) host = next();
        else if (!std::strcmp(argv[i], "--port")) port = uint16_t(std::atoi(next()));
    }

    const std::string folder = (projv::core::executableDirectory() / arena::FOLDER).string();
    std::vector<std::unique_ptr<projv::Application>> bots;
    for (int i = 0; i < count; i++) {
        auto app = std::make_unique<projv::Application>();
        app->world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(folder));
        projv::runtime::installSceneBridge(*app);
        projv::runtime::ClientConfig config;
        config.host = host;
        config.port = port;
        config.sceneFolder = folder;
        config.name = "bot" + std::to_string(i);
        if (!projv::runtime::installClient(*app, projv::net::makeEnetTransport(), config)) return 1;
        app->runStartup();
        bots.push_back(std::move(app));
    }

    std::mt19937 random(1);
    std::uniform_real_distribution<float> angle(0.0f, 6.2831853f);
    const auto start = std::chrono::steady_clock::now();
    double nextTurn = 0.0;
    for (;;) {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (now >= seconds) break;
        // Every two seconds each bot picks a new way to walk.
        if (now >= nextTurn) {
            nextTurn = now + 2.0;
            for (auto& bot : bots) {
                const float a = angle(random);
                projv::runtime::setLocalInput(bot->world, vec3(std::cos(a), 0.0f, std::sin(a)) * 3.0f);
            }
        }
        for (auto& bot : bots) bot->runFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    int joined = 0;
    uint64_t fewest = ~uint64_t(0), most = 0, total = 0;
    for (auto& bot : bots) {
        const auto stats = projv::runtime::sessionStats(bot->world);
        joined += stats.connected;
        fewest = std::min(fewest, stats.snapshotsReceived);
        most = std::max(most, stats.snapshotsReceived);
        total += stats.snapshotsReceived;
    }
    std::printf("BOTS: %d of %d joined; snapshots received %llu fewest, %llu most, %.0f each on average over %.0f s\n", joined,
                count, (unsigned long long)fewest, (unsigned long long)most, double(total) / double(count), seconds);
    std::fflush(stdout);
    for (auto& bot : bots) bot->runShutdown();
    return joined == count ? 0 : 1;
}
