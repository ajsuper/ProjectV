// ProjectV Siege: the dedicated server
//
// No window: it loads the siege field, runs the rules (game.h) and the bots, and serves every client
// that joins. A client started without --host runs the same rules itself, so this is for a game that
// should outlive whoever started it.
//
//   ./siege_server                       serve on port 27016 until stopped (Ctrl-C)
//   ./siege_server --port 27100          another port
//   ./siege_server --team-size 6         bots fill each team up to six
//   ./siege_server --round-seconds 60    shorter rounds
//   ./siege_server --seconds 30          stop after thirty seconds (the self-test)
//   ./siege_server --write-assets <dir>  regenerate the scene folder this example ships

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "game.h"

int main(int argc, char** argv) {
    siege::ServerOptions options;
    options.folder = (projv::core::executableDirectory() / siege::FOLDER).string();
    double seconds = 0.0;
    for (int i = 1; i < argc; i++) {
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--write-assets")) return siege::write(next()) ? 0 : 1;
        if (!std::strcmp(argv[i], "--port")) options.port = uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--team-size")) options.teamSize = std::clamp(std::atoi(next()), 1, 16);
        else if (!std::strcmp(argv[i], "--round-seconds")) options.roundSeconds = std::max(10.0f, float(std::atof(next())));
        else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(next());
    }

    projv::Application app;
    if (!siege::installServer(app, options)) {
        projv::core::error("siege: could not start a server on port {}", options.port);
        return 1;
    }
    // Each ten seconds, how the round stands.
    double clock = 0.0, total = 0.0;
    app.addSystem(projv::Stage::Update, "report", [&](projv::Application& a) {
        clock += a.time().unscaledDelta;
        total += a.time().unscaledDelta;
        if (clock >= 10.0) {
            clock = 0.0;
            const siege::Game& game = a.world.ctx().get<siege::Game>();
            projv::core::info("SIEGESERVER: round {}, {:.0f} s left, {} client(s), blue {} standing (flag {}), red {} standing (flag {}); {} cannon shots, {} blasts, {} breaks",
                              game.round, game.timeLeft, projv::runtime::sessionStats(a.world).clients,
                              siege::standing(a.world, siege::Blue), game.flags[siege::Blue].down ? "down" : "up",
                              siege::standing(a.world, siege::Red), game.flags[siege::Red].down ? "down" : "up",
                              game.cannonShots, game.blasts, game.breaks);
            // SIEGE_BOTS=1: where each bot is, and what it is doing.
            if (std::getenv("SIEGE_BOTS"))
                for (auto [e, bot, s] : a.world.view<siege::Bot, siege::Soldier>().each()) {
                    const siege::vec3 at = siege::feet(a.world, e);
                    const siege::vec3 to = bot.next < bot.path.size() ? bot.path[bot.next] : at;
                    projv::core::info("SIEGEBOT: {} {} role {} at ({:.1f}, {:.1f}, {:.1f}) waypoint {}/{} ({:.1f}, {:.1f}, {:.1f}) stuck {} cannon {}",
                                      s.name, siege::teamName(s.team), int(bot.role), at.x, at.y, at.z, bot.next, bot.path.size(),
                                      to.x, to.y, to.z, bot.stuck, s.cannon);
                }
        }
        if (seconds > 0.0 && total >= seconds) a.closeRequested = true;
    });
    // No window to wait on: sleep through the time no fixed step is owed, rather than spin.
    app.addSystem(projv::Stage::Render, "pace", [](projv::Application&) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
    app.run();
    return 0;
}
