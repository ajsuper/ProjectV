// The UI host (docs/plans/ui.md, U0): which layer is offered input first, what the capture flags say, and
// the order the layers and the render instance's overlays draw in. No window, no GPU: the layers here
// are fakes that record what the host did with them.

#include "doctest/doctest.h"

#include <algorithm>
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

TEST_CASE("ui: a button released over a panel still ends its drag") {
    // The mask is a view: the button's own state is kept, so the release is an edge the layers see.
    // Clearing the state instead left a panel resize held forever (found by hand in Voxel Mayhem).
    Layers l;
    l.dev->takes.mouse = true;
    projv::Input input;
    input.setMouse(projv::MouseButton::Left, true);   // the platform, frame 1: pressed on a panel
    l.host.route(input);
    CHECK_FALSE(input.down(projv::MouseButton::Left)); // hidden from the game
    input.setMouse(projv::MouseButton::Left, true);   // frame 2: still held
    CHECK_FALSE(input.pressed(projv::MouseButton::Left));
    l.host.route(input);
    input.setMouse(projv::MouseButton::Left, false);  // frame 3: let go
    CHECK(input.released(projv::MouseButton::Left));   // what the layers are offered
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

TEST_CASE("ui: a captured cursor is the game's, even over a dev panel") {
    projv::Application app;
    projv::ui::dev::DevLayer dev(app, nullptr);
    dev.setShown(true);
    projv::Input input;
    input.cursor = {50.0f, 50.0f};   // over the Frame panel
    input.cursorCaptured = true;     // the game took it back with a click
    devFrame(dev, input);
    devFrame(dev, input);
    CHECK_FALSE(devFrame(dev, input).mouse);
    input.cursorCaptured = false;    // released (Esc): the panel has it again
    devFrame(dev, input);
    devFrame(dev, input);
    CHECK(devFrame(dev, input).mouse);
}

TEST_CASE("ui: --dev-ui on the command line asks for the dev layer") {
    char program[] = "game", other[] = "--fast", dev[] = "--dev-ui";
    char* without[] = {program, other};
    char* with[] = {program, other, dev};
    CHECK_FALSE(projv::ui::devUiRequested(2, without));
    CHECK(projv::ui::devUiRequested(3, with));
}

TEST_CASE("application: each system's run time is reported for the last completed frame") {
    projv::Application app;
    app.addSystem(projv::Stage::Update, "quick", [](projv::Application&) {});
    app.addSystem(projv::Stage::Update, "slow", [](projv::Application&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    });
    app.runStartup();
    app.runFrame();
    CHECK(app.systemSeconds(projv::Stage::Update)[1] == 0.0);   // the frame is not done until the next begins
    app.runFrame();
    std::vector<double> seconds = app.systemSeconds(projv::Stage::Update);
    REQUIRE(seconds.size() == 2);
    CHECK(app.systemNames(projv::Stage::Update)[1] == "slow");
    CHECK(seconds[1] >= 0.003);
    CHECK(seconds[0] < seconds[1]);
}

TEST_CASE("ui: a Meter reads nothing until a period completes, then holds that period's mean, least and greatest") {
    projv::ui::dev::Meter meter(0.5);
    meter.add(10.0, 0.2);
    meter.add(30.0, 0.2);
    CHECK_FALSE(meter.ready());
    CHECK(meter.average() == 0.0);
    CHECK(meter.last() == 30.0);
    meter.add(20.0, 0.2);   // 0.6 s: the period is complete
    REQUIRE(meter.ready());
    CHECK(meter.average() == doctest::Approx(20.0));
    CHECK(meter.min() == 10.0);
    CHECK(meter.max() == 30.0);
    CHECK(meter.samples() == 3);

    // The next period's samples do not move the reading until it, too, completes.
    meter.add(100.0, 0.2);
    CHECK(meter.max() == 30.0);
    meter.add(50.0, 0.4);
    CHECK(meter.average() == doctest::Approx(75.0));
    CHECK(meter.min() == 50.0);
    CHECK(meter.max() == 100.0);
}

TEST_CASE("ui: a Meter's sample longer than its period publishes on its own") {
    projv::ui::dev::Meter meter(0.25);
    meter.add(40.0, 1.0);
    REQUIRE(meter.ready());
    CHECK(meter.average() == 40.0);
    CHECK(meter.samples() == 1);
    meter.setPeriod(2.0);
    CHECK(meter.periodSeconds() == 2.0);
}

TEST_CASE("ui: the Frame panel samples every frame while shown, headless") {
    projv::Application app;
    app.addSystem(projv::Stage::Update, "work", [](projv::Application&) {});
    projv::ui::dev::DevLayer dev(app, nullptr);
    dev.setShown(true);
    projv::Input input;
    for (int i = 0; i < 300; i++) devFrame(dev, input);   // more frames than a graph holds
    dev.setShown(false);
    CHECK_FALSE(dev.visible());
}

// ---- U2: the engine's panels -----------------------------------------------------------------------

#include "nlohmann/json.hpp"
#include "runtime/entities.h"
#include "runtime/physics.h"
#include "runtime/scene_bridge.h"

namespace {
    struct Tag { int n = 0; };

    bool drew(const projv::ui::dev::DevLayer& dev, const std::string& name) {
        const auto& drawn = dev.windowsDrawn();
        return std::find(drawn.begin(), drawn.end(), name) != drawn.end();
    }

    void openAll(projv::ui::dev::DevLayer& dev) {
        for (const char* name : {"Physics", "Network", "Renderer", "Entities"}) dev.setPanelOpen(name, true);
    }
}

template<> struct projv::runtime::ComponentTraits<Tag> {
    static constexpr const char* key = "test.tag";
    static constexpr uint32_t version = 2;
    static nlohmann::json save(const Tag& t) { return {{"n", t.n}}; }
    static std::optional<Tag> load(const nlohmann::json& j, uint32_t) { return Tag{j.value("n", 0)}; }
};

TEST_CASE("ui: the engine's panels beyond Frame and Log start closed, and open from the Panels menu") {
    projv::Application app;
    projv::ui::dev::DevLayer dev(app, nullptr);
    dev.setShown(true);
    projv::Input input;
    devFrame(dev, input);
    CHECK(drew(dev, "Frame"));
    CHECK(drew(dev, "Log"));
    CHECK_FALSE(drew(dev, "Entities"));
    dev.setPanelOpen("Entities", true);
    devFrame(dev, input);
    CHECK(drew(dev, "Entities"));
    dev.setPanelOpen("Frame", false);
    devFrame(dev, input);
    CHECK_FALSE(drew(dev, "Frame"));
}

TEST_CASE("ui: a panel whose subject is absent has no window") {
    projv::Application app;
    projv::ui::dev::DevLayer dev(app, nullptr);
    dev.setShown(true);
    openAll(dev);
    projv::Input input;
    devFrame(dev, input);
    CHECK_FALSE(drew(dev, "Physics"));    // no physics installed
    CHECK_FALSE(drew(dev, "Network"));    // no session
    CHECK_FALSE(drew(dev, "Renderer"));   // headless: no renderer
    CHECK(drew(dev, "Entities"));         // a world always has entities to list
}

TEST_CASE("ui: the Physics panel is drawn once there is physics, and reads it every frame") {
    projv::Application app;
    app.world.ctx().emplace<projv::Scene>();
    double now = 0.0;
    app.clock = [&now] { return now; };
    projv::runtime::installSceneBridge(app);
    projv::runtime::installPhysics(app);
    app.runStartup();
    projv::ui::dev::DevLayer dev(app, nullptr);
    dev.setShown(true);
    openAll(dev);
    projv::Input input;
    for (int i = 0; i < 30; i++) {
        now += 1.0 / 60.0;
        app.runFrame();
        devFrame(dev, input);
    }
    CHECK(drew(dev, "Physics"));
    CHECK_FALSE(drew(dev, "Network"));
}

TEST_CASE("ui: an entity's registered components come out as entities.json writes them") {
    projv::Application app;
    projv::runtime::registerComponent<Tag>(app.world);
    const projv::Entity tagged = app.world.create();
    app.world.emplace<Tag>(tagged, Tag{7});
    const projv::Entity bare = app.world.create();

    const auto components = projv::runtime::registeredComponents(app.world, tagged);
    REQUIRE(components.size() == 1);
    CHECK(components[0].first == "test.tag");
    CHECK(components[0].second["n"] == 7);
    CHECK(components[0].second["v"] == 2);
    CHECK(projv::runtime::registeredComponents(app.world, bare).empty());

    // The inspector, open on a world with both, draws without trouble.
    projv::ui::dev::DevLayer dev(app, nullptr);
    dev.setShown(true);
    dev.setPanelOpen("Entities", true);
    projv::Input input;
    devFrame(dev, input);
    CHECK(drew(dev, "Entities"));
}
