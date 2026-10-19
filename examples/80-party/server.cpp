// ProjectV Party: the dedicated server
//
// No window: it loads the arenas, runs the rules (game.h) and the bots, and serves every client that
// joins. A client started without --host runs the same rules itself, so this is for a game that
// should outlive whoever started it.
//
//   ./party_server                       serve on port 27017 until stopped (Ctrl-C)
//   ./party_server --port 27100          another port
//   ./party_server --players 3           bots fill the seats up to three (at most four)
//   ./party_server --rounds 12           a longer match
//   ./party_server --mode tag            just the one game: tag, dash, climb, crumble or blueprint
//   ./party_server --seconds 30          stop after thirty seconds (the self-test)
//   PARTY_BOTS=1 ./party_server          log where each bot is, twice a second
//   ./party_server --write-assets <dir>  regenerate the scene folder this example ships

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "game.h"

static int modeNamed(const char* name) {
    if (!std::strcmp(name, "crumble")) return party::Crumble;
    if (!std::strcmp(name, "tag")) return party::Tag;
    if (!std::strcmp(name, "dash")) return party::Dash;
    if (!std::strcmp(name, "climb")) return party::Climb;
    if (!std::strcmp(name, "blueprint")) return party::Blueprint;
    return -1;
}

int main(int argc, char** argv) {
    party::ServerOptions options;
    options.folder = (projv::core::executableDirectory() / party::FOLDER).string();
    double seconds = 0.0;
    for (int i = 1; i < argc; i++) {
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--write-assets")) return party::write(next()) ? 0 : 1;
        if (!std::strcmp(argv[i], "--port")) options.port = uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--players")) options.players = std::clamp(std::atoi(next()), 1, party::MAX_PLAYERS);
        else if (!std::strcmp(argv[i], "--rounds")) options.rounds = std::clamp(std::atoi(next()), 1, 99);
        else if (!std::strcmp(argv[i], "--mode")) options.only = modeNamed(next());
        else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(next());
    }

    projv::Application app;
    if (!party::installServer(app, options)) {
        projv::core::error("party: could not start a server on port {}", options.port);
        return 1;
    }
    // Each ten seconds, how the match stands.
    double clock = 0.0, total = 0.0;
    app.addSystem(projv::Stage::Update, "report", [&](projv::Application& a) {
        clock += a.time().unscaledDelta;
        total += a.time().unscaledDelta;
        if (clock >= 10.0) {
            clock = 0.0;
            const party::Game& game = a.world.ctx().get<party::Game>();
            std::string players;
            int dropped = 0;
            for (const auto& t : game.tiles) dropped += t.dropped;
            for (auto [e, p] : a.world.view<party::Player>().each())
                players += " " + p.name + (p.alive ? "" : " (out)") + " " + std::to_string(p.points) + "pt";
            projv::core::info("PARTYSERVER: round {}/{} {} phase {}, {:.0f} s, {} client(s), {} tiles dropped;{}", game.round,
                              game.options.rounds, party::modeName(game.mode), int(game.phase), game.clock,
                              projv::runtime::sessionStats(a.world).clients, dropped, players);
        }
        // PARTY_BOTS=1: where each bot is, twice a second, and where it is going.
        static double botClock = 0.0;
        if (std::getenv("PARTY_BOTS") && (botClock += a.time().unscaledDelta) >= 0.5) {
            botClock = 0.0;
            for (auto [e, bot, p] : a.world.view<party::Bot, party::Player>().each()) {
                const party::vec3 at = party::feet(a.world, e);
                projv::core::info("PARTYBOT: {} {} at ({:.1f}, {:.1f}, {:.1f}) goal ({:.1f}, {:.1f}, {:.1f}) stuck {} blocks {} place {}", p.name,
                                  p.alive ? "up" : (p.out ? "out" : "down"), at.x, at.y, at.z, bot.goal.x, bot.goal.y, bot.goal.z, bot.stuck,
                                  p.blocks, p.place);
            }
        }
        if (seconds > 0.0 && total >= seconds) a.closeRequested = true;
    });
    // No window to wait on: sleep through the time no fixed step is owed, rather than spin.
    app.addSystem(projv::Stage::Render, "pace", [](projv::Application&) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
    app.run();
    return 0;
}
