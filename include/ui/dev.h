#ifndef PROJV_UI_DEV_H
#define PROJV_UI_DEV_H

#include <functional>
#include <string>

#include <bgfx/bgfx.h>

#include "imgui.h"   // on purpose: a panel is ImGui code (docs/plans/ui.md, D2)

#include "core/math.h"

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
    struct ConstructedRenderer;
}

namespace projv::ui::dev {
    using Panel = std::function<void(Application&)>;

    // Adds a panel, shown as its own window while the dev layer is. Panels added before installUi, or
    // in a build without the dev layer, are kept and cost nothing.
    void addPanel(Application& app, std::string name, Panel panel);

    // Whether the dev layer is installed and shown.
    bool shown(const Application& app);

    // ---- Values a program lets the Renderer panel edit (docs/plans/ui.md, D17) ---------------------
    // The Renderer panel shows the active renderer's uniforms read-only. A program makes one editable
    // by binding *its own variable* -- the one it writes that uniform from each frame -- so an edit
    // reaches the program, which sees it change and keeps what it derives from it (a history reset)
    // right:
    //
    //     ui::dev::editable(app, *renderer, "sunDir", &sun.height, {.label = "sun height", .min = 0.0f, .max = 1.5f});
    //
    // The control sits on that uniform's row while `renderer` is the active one. The variable must
    // outlive the binding: forgetEditables when either goes. A uniform the renderer does not have is
    // listed apart, under its own name. Free when the dev layer is not shown.
    struct EditRange {
        const char* label = nullptr;   // the control's name; nullptr: the uniform's
        float min = 0.0f, max = 0.0f;  // a slider between them; equal: a drag with no bounds
        float speed = 0.01f;           // a drag's step per pixel
    };
    void editable(Application& app, const ConstructedRenderer& renderer, std::string uniform, float* value, EditRange range = {});
    void editable(Application& app, const ConstructedRenderer& renderer, std::string uniform, int* value, EditRange range = {});
    void editable(Application& app, const ConstructedRenderer& renderer, std::string uniform, bool* value, EditRange range = {});
    void editable(Application& app, const ConstructedRenderer& renderer, std::string uniform, core::vec3* value, EditRange range = {});
    void editable(Application& app, const ConstructedRenderer& renderer, std::string uniform, core::vec4* value, EditRange range = {});
    // Drops every binding on `renderer` (before it, or the variables, are destroyed).
    void forgetEditables(Application& app, const ConstructedRenderer& renderer);

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

    // ImGui::Image drawn opaque: colour only, its alpha ignored. For a render target whose alpha is
    // data (a G-buffer's depth, a mask), which the usual blend would draw faint or not at all.
    void opaqueImage(bgfx::TextureHandle handle, ImVec2 size, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1));
}

#endif
