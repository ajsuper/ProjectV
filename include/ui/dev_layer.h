#ifndef PROJV_UI_DEV_LAYER_H
#define PROJV_UI_DEV_LAYER_H

#include <memory>

#include "ui/ui.h"

struct ImGuiContext;

// The developer layer as the host holds it: Dear ImGui in a context of its own, fed from Input. A
// program does not usually make one -- ui::installUi does -- but a test can, headless.
//
// Plan: docs/plans/ui.md (D9, D11).
namespace projv::ui::dev {
    class DevLayer : public Layer {
    public:
        // `renderInstance` nullptr is headless: no GPU backend, a 1280x720 display, and draw builds the
        // frame without submitting it. For tests (D11).
        DevLayer(Application& app, graphics::RenderInstance* renderInstance);
        ~DevLayer() override;
        DevLayer(const DevLayer&) = delete;
        DevLayer& operator=(const DevLayer&) = delete;

        bool visible() const override { return isShown; }
        Capture input(const Input& input, Capture taken) override;
        void draw(bgfx::ViewId view, core::ivec2 size) override;

        // Shows or hides it. Showing releases a captured cursor, so the panels can be reached; hiding
        // captures it again if showing released it. While shown, the program may capture the cursor
        // again (a click on the scene, in a mouse-look program) and play on: a captured cursor is the
        // game's, and the panels see no mouse until it is released (Esc, by the examples' convention).
        void setShown(bool show);

        // The Frame panel's readings: sampled every frame while shown, whether or not its window is
        // open. Showing turns on bgfx's profiler (BGFX_DEBUG_PROFILER) for the per-pass GPU times;
        // hiding turns it off again.
        struct Profiler;

    private:
        std::unique_ptr<Profiler> profiler;
        Application* app;
        graphics::RenderInstance* renderInstance;
        ImGuiContext* context = nullptr;
        bool rendererReady = false;
        bool isShown = false;
        bool frameBegun = false;
        bool restoreCapture = false;
    };
}

#endif
