#include "ui/ui.h"

#include "core/application.h"
#include "graphics/input.h"
#include "graphics/render_instance.h"

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
        return taken;
    }

    void Host::draw(core::ivec2 size) {
        // The reverse of routing: the layer offered input first is drawn last, on top.
        if (Layer* user = layer(Level::User); user && user->visible()) user->draw(USER_VIEW, size);
        if (Layer* dev = layer(Level::Dev); dev && dev->visible()) dev->draw(DEV_VIEW, size);
    }

    Host& installUi(Application& app, graphics::RenderInstance& renderInstance) {
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
        return host;
    }
}
