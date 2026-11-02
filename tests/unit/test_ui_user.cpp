// The user layer (docs/plans/ui.md, U3), headless (D11): RmlUi with a recording render interface, no
// GPU. Documents open from memory or from files, take input from synthetic Input, show bound values,
// send UiActions, write what a control is bound to, and reload when their files change.

#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/application.h"
#include "graphics/input.h"
#include "ui/ui.h"
#include "ui/user.h"
#include "ui/user_layer.h"

namespace {
    namespace user = projv::ui::user;

    // An Application with a host and a headless user layer, and the shared demo font.
    struct Fixture {
        projv::Application app;
        projv::ui::Host& host;
        user::UserLayer* layer = nullptr;
        projv::Input input;
        std::vector<projv::ui::UiAction> actions;

        Fixture() : host(app.world.ctx().emplace<projv::ui::Host>()) {
            auto made = std::make_unique<user::UserLayer>(app, nullptr, true);
            layer = made.get();
            host.setLayer(projv::ui::Level::User, std::move(made));
            REQUIRE(user::loadFont(app, std::string(PROJV_SHARED_UI_DIR) + "/fonts/LatoLatin-Regular.ttf"));
            app.events().on<projv::ui::UiAction>([this](const projv::ui::UiAction& a) { actions.push_back(a); });
        }

        // One frame: route the input through the host, draw, deliver the events.
        projv::ui::Capture frame() {
            projv::ui::Capture took = host.route(input);
            host.draw({1280, 720});
            app.events().pump();
            return took;
        }

        // A left click at the centre of an element: press on one frame, release on the next.
        void click(const std::string& document, const std::string& id) {
            float x = 0, y = 0, w = 0, h = 0;
            REQUIRE(layer->elementBox(document, id, x, y, w, h));
            input.cursor = {x + w * 0.5f, y + h * 0.5f};
            frame();   // hover first, as a real cursor would
            input.setMouse(projv::MouseButton::Left, true);
            frame();
            input.setMouse(projv::MouseButton::Left, false);
            frame();
            input.setMouse(projv::MouseButton::Left, false);   // the release edge passes
        }
    };

    const char* STYLE = R"(
        <style>
            body { font-family: LatoLatin; font-size: 20dp; color: #fff; width: 100%; height: 100%; }
            .hud { pointer-events: none; }
            button { display: block; width: 200dp; height: 40dp; background-color: #345; }
            input.checkbox { width: 20dp; height: 20dp; background-color: #666; }
            input.text { width: 200dp; height: 30dp; background-color: #222; }
        </style>)";

    std::string document(const std::string& body, const std::string& bodyAttributes = "") {
        return std::string("<rml><head>") + STYLE + "</head><body " + bodyAttributes + ">" + body + "</body></rml>";
    }
}

TEST_CASE("ui: a user document is drawn while shown, and the layer is hidden when none is") {
    Fixture f;
    CHECK_FALSE(f.layer->visible());
    REQUIRE(user::openFromMemory(f.app, "menu", document("<p id='title'>Main menu</p>")));
    CHECK(user::isOpen(f.app, "menu"));
    CHECK(f.layer->visible());
    f.frame();
    CHECK(f.layer->drawCallsRecorded() > 0);
    CHECK(f.layer->elementText("menu", "title") == "Main menu");

    user::hide(f.app, "menu");
    CHECK_FALSE(f.layer->visible());
    CHECK_FALSE(user::isShown(f.app, "menu"));
    user::show(f.app, "menu");
    CHECK(f.layer->visible());
    user::close(f.app, "menu");
    CHECK_FALSE(user::isOpen(f.app, "menu"));
}

TEST_CASE("ui: a bound value shows in its document, and follows the program's variable") {
    Fixture f;
    int score = 5;
    std::string name = "ann";
    user::model(f.app, "hud").bind("score", &score).bind("name", &name);
    REQUIRE(user::openFromMemory(f.app, "hud", document("<p id='score'>Score {{score}}</p><p id='name'>{{name}}</p>",
                                                         "data-model='hud'")));
    f.frame();
    CHECK(f.layer->elementText("hud", "score") == "Score 5");
    CHECK(f.layer->elementText("hud", "name") == "ann");
    score = 12;
    name = "bob";
    f.frame();
    CHECK(f.layer->elementText("hud", "score") == "Score 12");
    CHECK(f.layer->elementText("hud", "name") == "bob");
}

TEST_CASE("ui: an element's on<event> sends a UiAction with its document, name and arguments") {
    Fixture f;
    REQUIRE(user::openFromMemory(f.app, "shop", document("<button id='buy' onclick='buy sword 2'>Buy</button>")));
    f.frame();
    f.click("shop", "buy");
    REQUIRE(f.actions.size() == 1);
    CHECK(f.actions[0].document == "shop");
    CHECK(f.actions[0].name == "buy");
    REQUIRE(f.actions[0].args.size() == 2);
    CHECK(f.actions[0].args[0] == "sword");
    CHECK(f.actions[0].args[1] == "2");
    CHECK(f.actions[0].event == "click");
}

TEST_CASE("ui: a control writes the program's variable it is bound to") {
    Fixture f;
    bool vsync = false;
    user::model(f.app, "settings").bind("vsync", &vsync);
    REQUIRE(user::openFromMemory(f.app, "settings",
                                 document("<input id='vsync' type='checkbox' class='checkbox' data-checked='vsync'/>",
                                          "data-model='settings'")));
    f.frame();
    f.click("settings", "vsync");
    f.frame();
    CHECK(vsync);
}

TEST_CASE("ui: the user layer takes the mouse over an element, and leaves it where pointer events are off") {
    Fixture f;
    REQUIRE(user::openFromMemory(f.app, "menu", document("<button id='play'>Play</button>")));
    REQUIRE(user::openFromMemory(f.app, "hud", document("<p id='ammo'>30</p>", "class='hud'")));
    f.frame();
    float x = 0, y = 0, w = 0, h = 0;
    REQUIRE(f.layer->elementBox("menu", "play", x, y, w, h));
    f.input.cursor = {x + w * 0.5f, y + h * 0.5f};
    f.frame();
    CHECK(f.frame().mouse);   // over the button
    CHECK(f.input.wantCaptureMouse);

    user::close(f.app, "menu");
    f.input.cursor = {640.0f, 600.0f};   // over nothing but the HUD's body, which lets the game have it
    f.frame();
    CHECK_FALSE(f.frame().mouse);
}

TEST_CASE("ui: a modal document takes all input while shown, a text field the keyboard while focused") {
    Fixture f;
    REQUIRE(user::openFromMemory(f.app, "pause", document("<p>Paused</p>"), {.shown = true, .modal = true}));
    f.input.cursor = {1200.0f, 700.0f};
    projv::ui::Capture took = f.frame();
    CHECK(took.mouse);
    CHECK(took.keyboard);
    user::hide(f.app, "pause");
    CHECK_FALSE(f.layer->visible());

    REQUIRE(user::openFromMemory(f.app, "chat", document("<input id='line' type='text' class='text'/>")));
    f.frame();
    CHECK_FALSE(f.frame().keyboard);   // not focused yet
    f.click("chat", "line");
    CHECK(f.frame().keyboard);         // focused: typing is the field's
}

TEST_CASE("ui: a document is loaded again when its file changes, shown as it was") {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "projv_ui_reload_test";
    std::filesystem::create_directories(folder);
    const std::filesystem::path file = folder / "menu.rml";
    {
        std::ofstream(file) << document("<p id='title'>First</p>");
    }
    Fixture f;
    REQUIRE(user::open(f.app, "menu", file.string()));
    f.frame();
    CHECK(f.layer->elementText("menu", "title") == "First");

    {
        std::ofstream(file) << document("<p id='title'>Second</p>");
    }
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(2));
    for (int i = 0; i < 31; i++) f.frame();   // the layer looks every 30 frames
    CHECK(f.layer->elementText("menu", "title") == "Second");
    CHECK(user::isShown(f.app, "menu"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("ui: the user layer's calls fail, logged, without the UI installed") {
    projv::Application app;
    CHECK_FALSE(user::openFromMemory(app, "x", "<rml><body/></rml>"));
    CHECK_FALSE(user::model(app, "m").valid());
    CHECK_FALSE(user::isOpen(app, "x"));
}

TEST_CASE("ui: a hidden document with a fadeOut is still drawn until it has faded, and is not modal meanwhile") {
    Fixture f;
    REQUIRE(user::openFromMemory(f.app, "menu", document("<p>Menu</p>"), {.shown = true, .modal = true, .fadeOut = 0.5f}));
    f.app.time().unscaledDelta = 0.1f;   // the layer's clock runs on the frame's unscaled time
    f.frame();
    user::hide(f.app, "menu");
    CHECK_FALSE(user::isShown(f.app, "menu"));
    CHECK(f.layer->visible());           // fading out
    projv::ui::Capture took = f.frame();
    CHECK_FALSE(took.keyboard);          // no longer modal
    for (int i = 0; i < 6; i++) f.frame();
    CHECK_FALSE(f.layer->visible());     // faded: hidden for good
    user::show(f.app, "menu");
    CHECK(f.layer->visible());
}
