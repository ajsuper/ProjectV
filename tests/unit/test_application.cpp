// Application: stages, ordering, the frame loop, and resources in the registry's context.

#include "doctest/doctest.h"

#include <string>
#include <type_traits>
#include <vector>

#include "core/application.h"

static_assert(!std::is_copy_constructible_v<projv::Application>);
static_assert(!std::is_move_constructible_v<projv::Application>);
static_assert(!std::is_copy_assignable_v<projv::Application>);
static_assert(!std::is_move_assignable_v<projv::Application>);

namespace {
    // A clock the test owns, advanced by hand.
    struct ManualClock {
        double now = 0.0;
        std::function<double()> fn() { return [this] { return now; }; }
    };
}

TEST_CASE("stages run in order, and systems within a stage in the order they were added") {
    projv::Application app;
    std::vector<std::string> log;
    auto record = [&log](std::string name) {
        return [&log, name](projv::Application&) { log.push_back(name); };
    };

    // Added out of stage order on purpose.
    app.addSystem(projv::Stage::Render,      "render",  record("render"));
    app.addSystem(projv::Stage::Update,      "update1", record("update1"));
    app.addSystem(projv::Stage::PreUpdate,   "pre",     record("pre"));
    app.addSystem(projv::Stage::Update,      "update2", record("update2"));
    app.addSystem(projv::Stage::PostUpdate,  "post",    record("post"));
    app.addSystem(projv::Stage::Startup,     "startup", record("startup"));
    app.addSystem(projv::Stage::Shutdown,    "shutdown",record("shutdown"));
    app.addSystem(projv::Stage::Update, "close", [](projv::Application& a) { a.closeRequested = true; });

    app.run();
    CHECK(log == std::vector<std::string>{"startup", "pre", "update1", "update2", "post", "render", "shutdown"});
    CHECK(app.frameCount == 1);
    CHECK(app.systemNames(projv::Stage::Update) == std::vector<std::string>{"update1", "update2", "close"});
}

TEST_CASE("a second system on a stage is added, not substituted") {
    // The old assignSystemStage replaced the previous system silently.
    projv::Application app;
    int first = 0, second = 0;
    app.addSystem(projv::Stage::Update, "first",  [&first](projv::Application&) { first++; });
    app.addSystem(projv::Stage::Update, "second", [&second](projv::Application& a) {
        if (++second == 3) a.closeRequested = true;
    });
    app.run();
    CHECK(first == 3);
    CHECK(second == 3);
}

TEST_CASE("FixedUpdate runs as many times as Time owes, before Update") {
    projv::Application app;
    ManualClock clock;
    app.clock = clock.fn();
    app.time().fixedDelta = 0.1f;

    std::vector<char> log;
    app.addSystem(projv::Stage::FixedUpdate, "fixed",  [&log](projv::Application&) { log.push_back('F'); });
    app.addSystem(projv::Stage::Update,      "update", [&log](projv::Application&) { log.push_back('U'); });

    app.runStartup();
    app.runFrame();            // t = 0: starting point, no steps
    clock.now = 0.25;
    app.runFrame();            // 0.25: two steps, 0.05 carried
    clock.now = 0.30;
    app.runFrame();            // 0.05 + 0.05: one step
    CHECK(log == std::vector<char>{'U', 'F', 'F', 'U', 'F', 'U'});
    CHECK(app.time().fixedStep == 3);
    CHECK(app.time().frame == 3);
}

TEST_CASE("Time scale 0 pauses FixedUpdate while frames keep running") {
    projv::Application app;
    ManualClock clock;
    app.clock = clock.fn();
    app.time().fixedDelta = 0.1f;
    int fixed = 0, frames = 0;
    app.addSystem(projv::Stage::FixedUpdate, "fixed", [&fixed](projv::Application&) { fixed++; });
    app.addSystem(projv::Stage::Update, "count", [&frames](projv::Application&) { frames++; });

    app.runStartup();
    app.runFrame();
    app.time().scale = 0.0f;
    for (int i = 1; i <= 5; i++) { clock.now = 0.1 * i; app.runFrame(); }
    CHECK(fixed == 0);
    CHECK(frames == 6);
}

TEST_CASE("Shutdown runs once when the loop ends, with or without systems on it") {
    // The exit_path regression, automated: the old createApp left Shutdown empty and a clean exit
    // threw std::bad_function_call.
    {
        projv::Application app;
        app.addSystem(projv::Stage::Update, "close", [](projv::Application& a) { a.closeRequested = true; });
        app.run();             // nothing on Shutdown: must simply return
        CHECK(app.frameCount == 1);
    }
    {
        projv::Application app;
        int shutdowns = 0;
        app.addSystem(projv::Stage::Update, "close", [](projv::Application& a) { a.closeRequested = true; });
        app.addSystem(projv::Stage::Shutdown, "count", [&shutdowns](projv::Application&) { shutdowns++; });
        app.run();
        app.runShutdown();     // a second call does nothing
        CHECK(shutdowns == 1);
    }
}

TEST_CASE("a system added while its stage runs, runs later in that same pass") {
    projv::Application app;
    std::vector<std::string> log;
    app.addSystem(projv::Stage::Update, "adder", [&log](projv::Application& a) {
        log.push_back("adder");
        if (a.frameCount == 0) {
            a.addSystem(projv::Stage::Update, "added", [&log](projv::Application&) { log.push_back("added"); });
        }
    });
    app.addSystem(projv::Stage::Update, "close", [](projv::Application& a) {
        if (a.frameCount == 1) a.closeRequested = true;
    });
    app.run();
    CHECK(log == std::vector<std::string>{"adder", "added", "adder", "added"});
}

TEST_CASE("resources live in the registry's context, and Time and Events are there from the start") {
    projv::Application app;
    CHECK(app.world.ctx().find<projv::Time>() == &app.time());
    CHECK(app.world.ctx().find<projv::Events>() == &app.events());

    struct Score { int points = 0; };
    app.world.ctx().emplace<Score>().points = 7;
    CHECK(app.world.ctx().get<Score>().points == 7);
    CHECK(app.world.ctx().find<double>() == nullptr);
}

TEST_CASE("CloseRequested ends the application, unless the application has taken that decision") {
    {
        projv::Application app;
        app.addSystem(projv::Stage::Update, "ask", [](projv::Application& a) { a.events().send(projv::CloseRequested{}); });
        app.run();             // returns: the pump after the first frame's PostUpdate closes it
        CHECK(app.frameCount == 1);
    }
    {
        projv::Application app;
        app.closeOnRequest = false;
        int asked = 0;
        app.events().on<projv::CloseRequested>([&asked](const projv::CloseRequested&) { asked++; });
        app.addSystem(projv::Stage::Update, "ask, then close on the third frame", [](projv::Application& a) {
            a.events().send(projv::CloseRequested{});
            if (a.frameCount == 2) a.closeRequested = true;
        });
        app.run();
        CHECK(asked == 3);     // heard every time, acted on by nobody but the app itself
        CHECK(app.frameCount == 3);
    }
}

TEST_CASE("a system can be ordered before or after another by name, whichever is added first") {
    auto order = [](bool bridgeFirst) {
        projv::Application app;
        std::vector<std::string> log;
        auto record = [&log](std::string name) {
            return [&log, name](projv::Application&) { log.push_back(name); };
        };
        app.addSystem(projv::Stage::PostUpdate, "a", record("a"));
        if (bridgeFirst) app.addSystem(projv::Stage::PostUpdate, "bridge", record("bridge"));
        app.addSystem(projv::Stage::PostUpdate, "present", record("present"), {.before = "bridge"});
        if (!bridgeFirst) app.addSystem(projv::Stage::PostUpdate, "bridge", record("bridge"));
        app.addSystem(projv::Stage::PostUpdate, "z", record("z"));
        app.runStartup();
        app.runFrame();
        CHECK(app.systemNames(projv::Stage::PostUpdate) == log);
        return log;
    };
    CHECK(order(true) == std::vector<std::string>{"a", "present", "bridge", "z"});
    CHECK(order(false) == std::vector<std::string>{"a", "present", "bridge", "z"});

    // `after` likewise, including a system added before the one it names.
    projv::Application app;
    app.addSystem(projv::Stage::Update, "late", [](projv::Application&) {}, {.after = "early"});
    app.addSystem(projv::Stage::Update, "other", [](projv::Application&) {});
    app.addSystem(projv::Stage::Update, "early", [](projv::Application&) {});
    CHECK(app.systemNames(projv::Stage::Update) == std::vector<std::string>{"early", "late", "other"});
}

TEST_CASE("an order that cannot hold is logged and the system appended") {
    projv::Application app;
    app.addSystem(projv::Stage::Update, "x", [](projv::Application&) {});
    app.addSystem(projv::Stage::Update, "y", [](projv::Application&) {});
    app.addSystem(projv::Stage::Update, "bad", [](projv::Application&) {}, {.before = "x", .after = "y"});
    CHECK(app.systemNames(projv::Stage::Update) == std::vector<std::string>{"x", "y", "bad"});
}
