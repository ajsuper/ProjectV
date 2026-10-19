// ProjectV Playground: the dedicated server
//
// No window: it loads the field, runs the rules (game.h) and serves every client that joins. A client
// started without --host runs the same rules itself, so this is for a playground that should outlive
// whoever started it.
//
//   ./playground_server                       serve on port 27018 until stopped (Ctrl-C)
//   ./playground_server --port 27100          another port
//   ./playground_server --seconds 30          stop after thirty seconds (the self-test)
//   ./playground_server --write-assets <dir>  regenerate the scene folder this example ships

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "game.h"

int main(int argc, char** argv) {
    playground::ServerOptions options;
    options.folder = (projv::core::executableDirectory() / playground::FOLDER).string();
    double seconds = 0.0;
    for (int i = 1; i < argc; i++) {
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--write-assets")) return playground::write(next()) ? 0 : 1;
        if (!std::strcmp(argv[i], "--port")) options.port = uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(next());
    }

    projv::Application app;
    if (!playground::installServer(app, options)) {
        projv::core::error("playground: could not start a server on port {}", options.port);
        return 1;
    }
    // Each ten seconds, how the playground stands.
    double clock = 0.0, total = 0.0;
    app.addSystem(projv::Stage::Update, "report", [&](projv::Application& a) {
        clock += a.time().unscaledDelta;
        total += a.time().unscaledDelta;
        if (clock >= 10.0) {
            clock = 0.0;
            const playground::Game& game = a.world.ctx().get<playground::Game>();
            projv::core::info("PLAYSERVER: {} client(s), {} shots, {} blasts, {} breaks, {} resets, {} things, {} pieces, {} gravity",
                              projv::runtime::sessionStats(a.world).clients, game.shots, game.blasts, game.breaks, game.resets,
                              game.things.size(), a.world.view<projv::Piece>().size(), game.moon ? "moon" : "earth");
        }
        if (seconds > 0.0 && total >= seconds) a.closeRequested = true;
    });
    // No window to wait on: sleep through the time no fixed step is owed, rather than spin.
    app.addSystem(projv::Stage::Render, "pace", [](projv::Application&) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
    app.run();
    return 0;
}
