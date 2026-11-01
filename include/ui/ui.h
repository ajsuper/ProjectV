#ifndef PROJV_UI_UI_H
#define PROJV_UI_UI_H

#include <array>
#include <memory>

#include <bgfx/bgfx.h>

#include "core/math.h"

// The UI host: two layers over the frame -- the developer's (Dear ImGui: stats, inspectors, tools) and
// the player's (RmlUi: HUD, menus) -- fed from one input stream and drawn in one order.
//
//     projv::ui::installUi(app, renderInstance);   // after graphics::installPlatform
//
// Each frame the host offers the input to the dev layer first (when it is shown), then to the user
// layer, then to the game: a layer that takes the keyboard or the mouse sets Input::wantCapture*, and
// gameplay yields to it. The layers draw after the renderer's passes, before the frame is submitted,
// in views of their own: the user layer under the dev layer, both over everything the renderer drew.
//
// Plan: docs/plans/ui.md (D1, D3, D4).
namespace projv {
    class Application;
    struct Input;
}
namespace projv::graphics {
    class RenderInstance;
}

namespace projv::ui {
    // The bgfx views the layers draw in: the top of bgfx's range, above any renderer's passes.
    inline constexpr bgfx::ViewId USER_VIEW = 254;
    inline constexpr bgfx::ViewId DEV_VIEW = 255;

    // The two levels, in routing order: the one first offered input.
    enum class Level { Dev, User, Count };

    // What a layer took of a frame's input.
    struct Capture {
        bool keyboard = false;   // key presses and typed text
        bool mouse = false;      // the cursor, its buttons and the wheel
    };

    // One level's UI, as the host sees it. The dev layer (U1) and the user layer (U3) implement this.
    class Layer {
    public:
        virtual ~Layer() = default;
        // Whether it takes input and draws this frame. A hidden layer is skipped by both.
        virtual bool visible() const = 0;
        // This frame's input. `taken` is what the layers before it took: a layer under a panel that has
        // the mouse should not react to that mouse. Returns what this layer takes.
        virtual Capture input(const Input& input, Capture taken) = 0;
        // Draws into the back buffer, in `view`, at `size`.
        virtual void draw(bgfx::ViewId view, core::ivec2 size) = 0;
    };

    // The world.ctx() resource installUi emplaces.
    class Host {
    public:
        // Sets a level's layer, replacing any before it; nullptr removes it.
        void setLayer(Level level, std::unique_ptr<Layer> layer);
        Layer* layer(Level level) const;

        // Offers `input` to the visible layers in routing order and records what they took in
        // input.wantCaptureKeyboard / wantCaptureMouse. installUi runs it every PreUpdate.
        Capture route(Input& input);

        // Draws the visible layers: the user layer in USER_VIEW, the dev layer in DEV_VIEW.
        void draw(core::ivec2 size);

    private:
        std::array<std::unique_ptr<Layer>, size_t(Level::Count)> layers;
    };

    // Emplaces the Host, routes input every PreUpdate (after the platform system, which fills Input),
    // and draws the layers as an overlay of `renderInstance` (graphics::endFrame). Install it after
    // graphics::installPlatform. `renderInstance` must outlive the application's loop.
    Host& installUi(Application& app, graphics::RenderInstance& renderInstance);
}

#endif
