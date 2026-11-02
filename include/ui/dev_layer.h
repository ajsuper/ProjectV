#ifndef PROJV_UI_DEV_LAYER_H
#define PROJV_UI_DEV_LAYER_H

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

        // Shows or hides it. Showing releases a captured cursor; hiding captures it again.
        void setShown(bool show);

        // While shown the cursor is the panels': a program that captures it again (mouse-look's
        // click-to-capture) has it released, and captured once the layer hides. Run every frame
        // before routing (installUi does).
        void keepCursor();

    private:
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
