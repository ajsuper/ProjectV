#ifndef PROJV_UI_DEV_H
#define PROJV_UI_DEV_H

#include <functional>
#include <string>

#include <bgfx/bgfx.h>

#include "imgui.h"   // on purpose: a panel is ImGui code (docs/plans/ui.md, D2)

// The developer layer: Dear ImGui over the frame, shown with F1, for stats, inspectors and tweakables.
// ui::installUi installs it when the build or the command line allows it (UiOptions::devLayer,
// --dev-ui); a program adds its own panels:
//
//     projv::ui::dev::addPanel(app, "Mayhem", [](projv::Application& app) {
//         ImGui::SliderFloat("blast radius", &app.world.ctx().get<Tuning>().radius, 1.0f, 20.0f);
//     });
//
// A panel is the body of an ImGui window of that name, built every frame the layer is shown, in the
// Render stage, after the frame's Update. Hidden, panels cost nothing.
//
// The bgfx renderer for ImGui is here too, for a program that runs ImGui itself (the scene editor).
//
// Plan: docs/plans/ui.md (D9, D10).
namespace projv {
    class Application;
}

namespace projv::ui::dev {
    using Panel = std::function<void(Application&)>;

    // Adds a panel, shown as its own window while the dev layer is. Panels added before installUi, or
    // in a build without the dev layer, are kept and cost nothing.
    void addPanel(Application& app, std::string name, Panel panel);

    // Whether the dev layer is installed and shown.
    bool shown(const Application& app);

    // A reading that holds still long enough to read: the mean, least and greatest of the samples
    // added over the last complete period, republished once a period. The engine's Frame panel reads
    // every timing through one; a panel of a program's own may too.
    //
    //     static ui::dev::Meter step;
    //     step.add(stepMs, app.time().unscaledDelta);
    //     ImGui::Text("step %.2f ms (%.2f .. %.2f)", step.average(), step.min(), step.max());
    class Meter {
    public:
        Meter() = default;
        explicit Meter(double periodSeconds) : period(periodSeconds) {}

        // One sample, for a stretch of `seconds` (a frame's length). Each sample weighs the same.
        void add(double value, double seconds);
        // Takes effect from the next period.
        void setPeriod(double seconds) { period = seconds; }
        double periodSeconds() const { return period; }

        // A period has completed: before that, the readings are 0.
        bool ready() const { return published > 0; }
        double average() const { return mean; }
        double min() const { return least; }
        double max() const { return greatest; }
        // The newest sample, unaveraged.
        double last() const { return newest; }
        // How many samples the published reading is over.
        int samples() const { return published; }

    private:
        double period = 0.5;
        double elapsed = 0.0, sum = 0.0, low = 0.0, high = 0.0, newest = 0.0;
        int count = 0;
        double mean = 0.0, least = 0.0, greatest = 0.0;
        int published = 0;
    };

    // ---- ImGui on bgfx -----------------------------------------------------------------------------
    // For the current ImGui context. The dev layer uses these; so may a program with an ImGui context
    // of its own. The shaders are the engine's (vs_imgui, imgui), compiled into the library.

    // Creates the GPU resources and registers the backend with the current context. After
    // ImGui::CreateContext and bgfx::init. False (logged) when the shaders are missing for this
    // renderer type.
    bool initRenderer(bgfx::ViewId view);

    // Destroys them, the textures ImGui still owns included. Before ImGui::DestroyContext and
    // bgfx::shutdown.
    void shutdownRenderer();

    // Draws one frame of ImGui: after ImGui::Render(), with ImGui::GetDrawData(), before bgfx::frame().
    void renderDrawData(ImDrawData* drawData);

    // An engine texture as an ImTextureID, for ImGui::Image. The only correct way to build one: the
    // encoding is the backend's, not the bgfx index. The texture must live until the frame is submitted.
    ImTextureID textureID(bgfx::TextureHandle handle);
}

#endif
