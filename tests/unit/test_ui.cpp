// The UI host (docs/plans/ui.md, U0): which layer is offered input first, what the capture flags say, and
// the order the layers and the render instance's overlays draw in. No window, no GPU: the layers here
// are fakes that record what the host did with them.

#include "doctest/doctest.h"

#include <memory>
#include <string>
#include <vector>

#include "core/application.h"
#include "graphics/input.h"
#include "graphics/render_instance.h"
#include "ui/ui.h"

namespace {
    struct FakeLayer : projv::ui::Layer {
        FakeLayer(std::string name, std::vector<std::string>& log) : name(std::move(name)), log(log) {}
        bool visible() const override { return shown; }
        projv::ui::Capture input(const projv::Input& input, projv::ui::Capture taken) override {
            log.push_back(name + " input");
            seenTaken = taken;
            seenText = input.text;
            return takes;
        }
        void draw(bgfx::ViewId view, projv::core::ivec2) override {
            log.push_back(name + " draw " + std::to_string(view));
        }

        std::string name;
        std::vector<std::string>& log;
        bool shown = true;
        projv::ui::Capture takes;
        projv::ui::Capture seenTaken;
        std::vector<uint32_t> seenText;
    };

    struct Layers {
        std::vector<std::string> log;
        projv::ui::Host host;
        FakeLayer* dev = nullptr;
        FakeLayer* user = nullptr;
        Layers() {
            auto d = std::make_unique<FakeLayer>("dev", log);
            auto u = std::make_unique<FakeLayer>("user", log);
            dev = d.get();
            user = u.get();
            host.setLayer(projv::ui::Level::Dev, std::move(d));
            host.setLayer(projv::ui::Level::User, std::move(u));
        }
    };
}

TEST_CASE("ui: the dev layer is offered input first, then the user layer") {
    Layers l;
    projv::Input input;
    l.host.route(input);
    CHECK(l.log == std::vector<std::string>{"dev input", "user input"});
}

TEST_CASE("ui: a hidden layer is neither offered input nor drawn") {
    Layers l;
    l.dev->shown = false;
    l.dev->takes.mouse = true;   // ignored: it is not shown
    projv::Input input;
    l.host.route(input);
    l.host.draw({640, 480});
    CHECK(l.log == std::vector<std::string>{"user input", "user draw " + std::to_string(projv::ui::USER_VIEW)});
    CHECK_FALSE(input.wantCaptureMouse);
}

TEST_CASE("ui: what a layer takes reaches the layers after it, and the game through Input") {
    Layers l;
    l.dev->takes.mouse = true;
    projv::Input input;
    l.host.route(input);
    CHECK(l.user->seenTaken.mouse);
    CHECK_FALSE(l.user->seenTaken.keyboard);
    CHECK(input.wantCaptureMouse);
    CHECK_FALSE(input.wantCaptureKeyboard);

    // The user layer's keyboard adds to the dev layer's mouse.
    l.user->takes.keyboard = true;
    l.host.route(input);
    CHECK(input.wantCaptureMouse);
    CHECK(input.wantCaptureKeyboard);

    // Recomputed every frame: nothing taken, nothing captured.
    l.dev->takes = {};
    l.user->takes = {};
    l.host.route(input);
    CHECK_FALSE(input.wantCaptureMouse);
    CHECK_FALSE(input.wantCaptureKeyboard);
}

TEST_CASE("ui: what a layer took is gone from the Input the game reads") {
    Layers l;
    projv::Input input;
    input.setMouse(projv::MouseButton::Left, true);
    input.setKey(projv::Key::W, true);
    input.scroll = {0.0f, 2.0f};
    input.text = {U'w'};
    l.dev->takes.mouse = true;
    l.host.route(input);
    CHECK(l.user->seenTaken.mouse);
    CHECK_FALSE(input.pressed(projv::MouseButton::Left));   // the panel's click
    CHECK(input.scroll.y == 0.0f);
    CHECK(input.down(projv::Key::W));                        // the keyboard was not taken
    CHECK(input.text.size() == 1);

    l.user->takes.keyboard = true;
    input.setKey(projv::Key::W, true);
    input.text = {U'w'};
    l.host.route(input);
    CHECK_FALSE(input.down(projv::Key::W));
    CHECK(input.text.empty());
}

TEST_CASE("ui: the typed text reaches the layers") {
    Layers l;
    projv::Input input;
    input.text = {U'h', U'é', 0x1F600u};
    l.host.route(input);
    CHECK(l.dev->seenText == input.text);
    CHECK(l.user->seenText == input.text);
}

TEST_CASE("ui: the user layer draws under the dev layer, each in its own view") {
    Layers l;
    l.host.draw({640, 480});
    CHECK(projv::ui::USER_VIEW < projv::ui::DEV_VIEW);
    CHECK(l.log == std::vector<std::string>{"user draw " + std::to_string(projv::ui::USER_VIEW),
                                             "dev draw " + std::to_string(projv::ui::DEV_VIEW)});
}

TEST_CASE("ui: overlays run in ascending order, and in the order added within one") {
    projv::graphics::RenderInstance instance;
    std::vector<std::string> order;
    instance.addOverlay(5, [&](projv::core::ivec2) { order.push_back("5a"); });
    instance.addOverlay(0, [&](projv::core::ivec2) { order.push_back("0"); });
    instance.addOverlay(5, [&](projv::core::ivec2) { order.push_back("5b"); });
    instance.addOverlay(-1, [&](projv::core::ivec2 size) { order.push_back("-1 " + std::to_string(size.x)); });
    instance.drawOverlays({320, 200});
    CHECK(order == std::vector<std::string>{"-1 320", "0", "5a", "5b"});
}

TEST_CASE("ui: installUi routes after the platform fills Input, and draws as an overlay") {
    projv::Application app;
    projv::graphics::RenderInstance instance;
    std::vector<std::string> log;
    bool keyboardSeenByGame = false;
    // Stands in for graphics::installPlatform's system, added after the host's on purpose: the host
    // orders itself after "platform" whichever is added first.
    projv::ui::Host& host = projv::ui::installUi(app, instance, {.devLayer = false});
    auto user = std::make_unique<FakeLayer>("user", log);
    user->takes.keyboard = true;
    host.setLayer(projv::ui::Level::User, std::move(user));
    app.addSystem(projv::Stage::PreUpdate, "platform", [&log](projv::Application& a) {
        log.push_back("platform");
        a.world.ctx().insert_or_assign(projv::Input{});
    });
    app.addSystem(projv::Stage::Update, "game", [&](projv::Application& a) {
        keyboardSeenByGame = a.world.ctx().get<projv::Input>().wantCaptureKeyboard;
    });
    app.runStartup();
    app.runFrame();
    CHECK(log == std::vector<std::string>{"platform", "user input"});
    CHECK(keyboardSeenByGame);

    instance.drawOverlays({800, 600});
    CHECK(log.back() == "user draw " + std::to_string(projv::ui::USER_VIEW));
}

// ---- The dev layer, headless (D11): a real ImGui context, no GPU. ------------------------------------

#include <chrono>
#include <thread>

#include "ui/dev.h"
#include "ui/dev_layer.h"

namespace {
    // One frame of the dev layer as the host runs it: input, then draw.
    projv::ui::Capture devFrame(projv::ui::dev::DevLayer& layer, projv::Input& input) {
        projv::ui::Capture took = layer.input(input, {});
        layer.draw(projv::ui::DEV_VIEW, {1280, 720});
        return took;
    }
}

TEST_CASE("ui: dev panels registered before the layer exists are drawn once it is shown, and not while hidden") {
    projv::Application app;
    int drawn = 0;
    projv::ui::dev::addPanel(app, "Counter", [&drawn](projv::Application&) { drawn++; });
    CHECK_FALSE(projv::ui::dev::shown(app));

    projv::ui::Host& host = app.world.ctx().emplace<projv::ui::Host>();
    auto layer = std::make_unique<projv::ui::dev::DevLayer>(app, nullptr);
    projv::ui::dev::DevLayer& dev = *layer;
    host.setLayer(projv::ui::Level::Dev, std::move(layer));

    projv::Input input;
    host.route(input);
    host.draw({1280, 720});
    CHECK(drawn == 0);   // hidden: neither offered input nor drawn

    dev.setShown(true);
    CHECK(projv::ui::dev::shown(app));
    host.route(input);
    host.draw({1280, 720});
    CHECK(drawn == 1);
}

TEST_CASE("ui: the dev layer takes the mouse over its panels and leaves it elsewhere") {
    projv::Application app;
    projv::ui::dev::DevLayer dev(app, nullptr);
    dev.setShown(true);
    projv::Input input;

    // The engine's Frame panel opens at (10, 10), 420 wide. ImGui decides hover from the frame before.
    input.cursor = {50.0f, 50.0f};
    devFrame(dev, input);
    devFrame(dev, input);
    projv::ui::Capture over = devFrame(dev, input);
    CHECK(over.mouse);
    CHECK_FALSE(over.keyboard);

    input.cursor = {1200.0f, 700.0f};
    devFrame(dev, input);
    projv::ui::Capture away = devFrame(dev, input);
    CHECK_FALSE(away.mouse);
}

TEST_CASE("ui: --dev-ui on the command line asks for the dev layer") {
    char program[] = "game", other[] = "--fast", dev[] = "--dev-ui";
    char* without[] = {program, other};
    char* with[] = {program, other, dev};
    CHECK_FALSE(projv::ui::devUiRequested(2, without));
    CHECK(projv::ui::devUiRequested(3, with));
}

TEST_CASE("application: each system's run time is reported for the last frame") {
    projv::Application app;
    app.addSystem(projv::Stage::Update, "quick", [](projv::Application&) {});
    app.addSystem(projv::Stage::Update, "slow", [](projv::Application&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    });
    app.runStartup();
    app.runFrame();
    std::vector<double> seconds = app.systemSeconds(projv::Stage::Update);
    REQUIRE(seconds.size() == 2);
    CHECK(app.systemNames(projv::Stage::Update)[1] == "slow");
    CHECK(seconds[1] >= 0.003);
    CHECK(seconds[0] < seconds[1]);
}
