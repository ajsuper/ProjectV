#include "ui/ui.h"

#include <cstdlib>
#include <cstring>

#include "core/application.h"
#include "graphics/input.h"
#include "graphics/render_instance.h"
#include "ui/dev_layer.h"

namespace projv::ui {
    void Host::setLayer(Level level, std::unique_ptr<Layer> layer) {
        layers[size_t(level)] = std::move(layer);
    }

    Layer* Host::layer(Level level) const {
        return layers[size_t(level)].get();
    }

    Capture Host::route(Input& input) {
        Capture taken;
        for (const auto& layer : layers) {   // Level order: the dev layer first
            if (!layer || !layer->visible()) continue;
            Capture took = layer->input(input, taken);
            taken.keyboard = taken.keyboard || took.keyboard;
            taken.mouse = taken.mouse || took.mouse;
        }
        input.wantCaptureKeyboard = taken.keyboard;
        input.wantCaptureMouse = taken.mouse;
        // What a layer took is the layer's: the game, reading Input after this, does not see it.
        if (taken.keyboard) input.maskKeyboard();
        if (taken.mouse) input.maskMouse();
        return taken;
    }

    void Host::draw(core::ivec2 size) {
        // The reverse of routing: the layer offered input first is drawn last, on top.
        if (Layer* user = layer(Level::User); user && user->visible()) user->draw(USER_VIEW, size);
        if (Layer* dev = layer(Level::Dev); dev && dev->visible()) dev->draw(DEV_VIEW, size);
    }

    namespace {
        bool g_devUiOnCommandLine = false;
    }

    void setCommandLine(int argc, char** argv) {
        g_devUiOnCommandLine = devUiRequested(argc, argv);
    }

    bool devUiRequested(int argc, char** argv) {
        for (int i = 1; i < argc; i++)
            if (std::strcmp(argv[i], "--dev-ui") == 0) return true;
        return false;
    }

    Host& installUi(Application& app, graphics::RenderInstance& renderInstance, UiOptions options) {
        Host& host = app.world.ctx().emplace<Host>();
        app.addSystem(Stage::PreUpdate, "ui: route input", [](Application& a) {
            if (Input* input = a.world.ctx().find<Input>()) a.world.ctx().get<Host>().route(*input);
        }, {.before = "", .after = "platform"});
        // Looked up each frame rather than held: world.ctx() may move a small resource as it grows. The
        // Application cannot move (it is neither copyable nor movable), so it is safe to hold.
        Application* application = &app;
        renderInstance.addOverlay(0, [application](core::ivec2 size) {
            if (Host* host = application->world.ctx().find<Host>()) host->draw(size);
        });

#if defined(PROJV_DEV_UI_DEFAULT)
        const bool devByDefault = true;
#else
        const bool devByDefault = false;
#endif
        if (options.devLayer.value_or(devByDefault || g_devUiOnCommandLine)) {
            auto devLayer = std::make_unique<dev::DevLayer>(app, &renderInstance);
            const char* shownEnv = std::getenv("PROJV_DEV_UI_SHOWN");
            if (options.devShown.value_or(shownEnv && std::strcmp(shownEnv, "1") == 0)) devLayer->setShown(true);
            host.setLayer(Level::Dev, std::move(devLayer));
            const Key toggle = options.devToggle;
            // Before routing, so the frame it is shown on already offers it input.
            app.addSystem(Stage::PreUpdate, "ui: dev toggle", [toggle](Application& a) {
                const Input* input = a.world.ctx().find<Input>();
                Host* h = a.world.ctx().find<Host>();
                auto* devLayer = h ? static_cast<dev::DevLayer*>(h->layer(Level::Dev)) : nullptr;
                if (!input || !devLayer) return;
                if (input->pressed(toggle)) devLayer->setShown(!devLayer->visible());
                devLayer->keepCursor();
            }, {.before = "ui: route input", .after = "platform"});
            // Its GPU resources go before bgfx does: ahead of a program's "shutdown", which is where
            // a program that shuts bgfx down itself does so.
            app.addSystem(Stage::Shutdown, "ui: shutdown", [](Application& a) {
                if (Host* h = a.world.ctx().find<Host>()) h->setLayer(Level::Dev, nullptr);
            }, {.before = "shutdown", .after = ""});
        }
        return host;
    }
}
