// The developer layer (ui/dev.h): Dear ImGui as a ui::Layer. It owns an ImGui context of its own, fed
// from the engine's Input rather than from ImGui's GLFW backend (docs/plans/ui.md, D4), and draws in
// ui::DEV_VIEW. Shown and hidden with F1; while shown it releases a captured cursor, and gives it back
// when hidden.

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include "ui/dev.h"
#include "ui/dev_layer.h"
#include "ui/ui.h"

#include "core/application.h"
#include "core/time.h"
#include "graphics/input.h"
#include "graphics/render_instance.h"

namespace projv::ui::dev {
    namespace {
        // Panels, kept whether or not the layer is installed: addPanel before installUi, or in a build
        // without the dev layer, registers and costs nothing.
        struct Panels {
            std::vector<std::pair<std::string, Panel>> list;
        };

        struct KeyMapping { Key key; ImGuiKey imgui; };
        constexpr KeyMapping KEYS[] = {
            {Key::A, ImGuiKey_A}, {Key::B, ImGuiKey_B}, {Key::C, ImGuiKey_C}, {Key::D, ImGuiKey_D},
            {Key::E, ImGuiKey_E}, {Key::F, ImGuiKey_F}, {Key::G, ImGuiKey_G}, {Key::H, ImGuiKey_H},
            {Key::I, ImGuiKey_I}, {Key::J, ImGuiKey_J}, {Key::K, ImGuiKey_K}, {Key::L, ImGuiKey_L},
            {Key::M, ImGuiKey_M}, {Key::N, ImGuiKey_N}, {Key::O, ImGuiKey_O}, {Key::P, ImGuiKey_P},
            {Key::Q, ImGuiKey_Q}, {Key::R, ImGuiKey_R}, {Key::S, ImGuiKey_S}, {Key::T, ImGuiKey_T},
            {Key::U, ImGuiKey_U}, {Key::V, ImGuiKey_V}, {Key::W, ImGuiKey_W}, {Key::X, ImGuiKey_X},
            {Key::Y, ImGuiKey_Y}, {Key::Z, ImGuiKey_Z},
            {Key::Num0, ImGuiKey_0}, {Key::Num1, ImGuiKey_1}, {Key::Num2, ImGuiKey_2}, {Key::Num3, ImGuiKey_3},
            {Key::Num4, ImGuiKey_4}, {Key::Num5, ImGuiKey_5}, {Key::Num6, ImGuiKey_6}, {Key::Num7, ImGuiKey_7},
            {Key::Num8, ImGuiKey_8}, {Key::Num9, ImGuiKey_9},
            {Key::Space, ImGuiKey_Space}, {Key::Enter, ImGuiKey_Enter}, {Key::Escape, ImGuiKey_Escape},
            {Key::Tab, ImGuiKey_Tab}, {Key::Backspace, ImGuiKey_Backspace}, {Key::Delete, ImGuiKey_Delete},
            {Key::Insert, ImGuiKey_Insert}, {Key::Home, ImGuiKey_Home}, {Key::End, ImGuiKey_End},
            {Key::PageUp, ImGuiKey_PageUp}, {Key::PageDown, ImGuiKey_PageDown},
            {Key::Left, ImGuiKey_LeftArrow}, {Key::Right, ImGuiKey_RightArrow},
            {Key::Up, ImGuiKey_UpArrow}, {Key::Down, ImGuiKey_DownArrow},
            {Key::LeftShift, ImGuiKey_LeftShift}, {Key::RightShift, ImGuiKey_RightShift},
            {Key::LeftControl, ImGuiKey_LeftCtrl}, {Key::RightControl, ImGuiKey_RightCtrl},
            {Key::LeftAlt, ImGuiKey_LeftAlt}, {Key::RightAlt, ImGuiKey_RightAlt},
            {Key::LeftSuper, ImGuiKey_LeftSuper}, {Key::RightSuper, ImGuiKey_RightSuper},
            {Key::F1, ImGuiKey_F1}, {Key::F2, ImGuiKey_F2}, {Key::F3, ImGuiKey_F3}, {Key::F4, ImGuiKey_F4},
            {Key::F5, ImGuiKey_F5}, {Key::F6, ImGuiKey_F6}, {Key::F7, ImGuiKey_F7}, {Key::F8, ImGuiKey_F8},
            {Key::F9, ImGuiKey_F9}, {Key::F10, ImGuiKey_F10}, {Key::F11, ImGuiKey_F11}, {Key::F12, ImGuiKey_F12},
            {Key::Minus, ImGuiKey_Minus}, {Key::Equal, ImGuiKey_Equal},
            {Key::LeftBracket, ImGuiKey_LeftBracket}, {Key::RightBracket, ImGuiKey_RightBracket},
            {Key::Backslash, ImGuiKey_Backslash}, {Key::Semicolon, ImGuiKey_Semicolon},
            {Key::Apostrophe, ImGuiKey_Apostrophe}, {Key::Comma, ImGuiKey_Comma},
            {Key::Period, ImGuiKey_Period}, {Key::Slash, ImGuiKey_Slash}, {Key::GraveAccent, ImGuiKey_GraveAccent},
        };

        // The log, as the log panel shows it: every line spdlog writes, the last few thousand.
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> logSink() {
            static std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink = [] {
                auto s = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(4000);
                s->set_pattern("%H:%M:%S.%e %v");
                spdlog::default_logger()->sinks().push_back(s);
                return s;
            }();
            return sink;
        }

        // The categories core/log.h tags each line with.
        constexpr const char* LOG_TAGS[] = {"[TRC]", "[PRF]", "[EDT]", "[RND]", "[INF]", "[WRN]", "[ERR]"};
        constexpr size_t LOG_TAG_COUNT = sizeof(LOG_TAGS) / sizeof(LOG_TAGS[0]);

        // ---- The engine's panels ------------------------------------------------------------------

        void framePanel(Application& app) {
            static float history[240] = {};
            static int at = 0;
            const Time& time = app.time();
            const float ms = time.unscaledDelta * 1000.0f;
            history[at] = ms;
            at = (at + 1) % 240;
            float sum = 0.0f, worst = 0.0f;
            for (float h : history) { sum += h; worst = std::max(worst, h); }
            ImGui::Text("%.2f ms  (%.0f fps)   mean %.2f ms, worst %.2f ms over 240 frames",
                        ms, ms > 0.0f ? 1000.0f / ms : 0.0f, sum / 240.0f, worst);
            ImGui::PlotLines("##frames", history, 240, at, nullptr, 0.0f, std::max(worst, 1.0f), ImVec2(-1.0f, 60.0f));
            ImGui::Text("frame %llu, fixed step %llu, time scale %.2f", (unsigned long long)time.frame,
                        (unsigned long long)time.fixedStep, time.scale);

            static const Stage stages[] = {Stage::PreUpdate, Stage::FixedUpdate, Stage::Update, Stage::PostUpdate, Stage::Render};
            if (ImGui::BeginTable("##systems", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("system");
                ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableHeadersRow();
                for (Stage stage : stages) {
                    const std::vector<std::string>& names = app.systemNames(stage);
                    const std::vector<double> seconds = app.systemSeconds(stage);
                    double total = 0.0;
                    for (double s : seconds) total += s;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    bool open = ImGui::TreeNodeEx(stageName(stage), ImGuiTreeNodeFlags_SpanAllColumns |
                                                                    ImGuiTreeNodeFlags_DefaultOpen);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", total * 1000.0);
                    if (!open) continue;
                    for (size_t i = 0; i < names.size() && i < seconds.size(); i++) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Indent();
                        ImGui::TextUnformatted(names[i].c_str());
                        ImGui::Unindent();
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", seconds[i] * 1000.0);
                    }
                    ImGui::TreePop();
                }
                ImGui::EndTable();
            }
        }

        void logPanel(Application&) {
            static bool show[LOG_TAG_COUNT] = {false, true, true, true, true, true, true};
            static ImGuiTextFilter filter;
            static bool follow = true;
            for (size_t i = 0; i < LOG_TAG_COUNT; i++) {
                if (i > 0) ImGui::SameLine();
                ImGui::Checkbox(LOG_TAGS[i], &show[i]);
            }
            ImGui::SameLine();
            ImGui::Checkbox("follow", &follow);
            filter.Draw("filter", 200.0f);
            ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            for (const std::string& line : logSink()->last_formatted()) {
                size_t tag = LOG_TAG_COUNT;
                for (size_t i = 0; i < LOG_TAG_COUNT; i++)
                    if (line.find(LOG_TAGS[i]) != std::string::npos) { tag = i; break; }
                if (tag < LOG_TAG_COUNT && !show[tag]) continue;
                if (!filter.PassFilter(line.c_str())) continue;
                const bool warn = tag == 5, error = tag == 6;
                if (warn || error) ImGui::PushStyleColor(ImGuiCol_Text, error ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
                ImGui::TextUnformatted(line.c_str());
                if (warn || error) ImGui::PopStyleColor();
            }
            if (follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
    }

    void addPanel(Application& app, std::string name, Panel panel) {
        auto& context = app.world.ctx();
        if (!context.contains<Panels>()) context.emplace<Panels>();
        context.get<Panels>().list.emplace_back(std::move(name), std::move(panel));
    }

    bool shown(const Application& app) {
        const Host* host = app.world.ctx().find<Host>();
        const Layer* layer = host ? host->layer(Level::Dev) : nullptr;
        return layer && layer->visible();
    }

    DevLayer::DevLayer(Application& app, graphics::RenderInstance* renderInstance)
        : app(&app), renderInstance(renderInstance) {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // windows open where a panel first puts them; nothing is written beside the program
        io.BackendPlatformName = "projv::Input";
        // Explicit, for the reason the scene editor gives: with no font source and the renderer owning
        // its textures, AddFontDefault picks between two embedded fonts by a size heuristic.
        io.Fonts->AddFontDefaultBitmap();
        ImGui::StyleColorsDark();
        ImGui::GetStyle().WindowRounding = 4.0f;
        ImGui::GetStyle().FrameRounding = 3.0f;
        ImGui::GetStyle().Alpha = 0.95f;
        if (renderInstance) {
            rendererReady = initRenderer(DEV_VIEW);
        } else {
            // Headless: claim the texture protocol and serve none of its requests. ImGui then builds
            // its atlas on the CPU and starts frames as it would with a GPU behind it.
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        }
        ImGui::SetCurrentContext(previous);
        logSink();   // collect the log from here on, so the panel has history when first opened
    }

    DevLayer::~DevLayer() {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context);
        if (rendererReady) shutdownRenderer();
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous == context ? nullptr : previous);
    }

    void DevLayer::setShown(bool show) {
        if (show == isShown) return;
        isShown = show;
        // The cursor is the panels' while they are up: a captured (mouse-look) cursor is released, and
        // captured again when they go.
        if (const Input* input = app->world.ctx().find<Input>(); input && renderInstance) {
            if (show && input->cursorCaptured) {
                graphics::setCursorCaptured(*app, *renderInstance, false);
                restoreCapture = true;
            } else if (!show && restoreCapture) {
                graphics::setCursorCaptured(*app, *renderInstance, true);
                restoreCapture = false;
            }
        }
        if (!show && frameBegun) {
            // A frame begun and never drawn: end it, or the next NewFrame asserts.
            ImGuiContext* previous = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(context);
            ImGui::EndFrame();
            ImGui::SetCurrentContext(previous);
            frameBegun = false;
        }
    }

    void DevLayer::keepCursor() {
        const Input* input = app->world.ctx().find<Input>();
        if (!isShown || !renderInstance || !input || !input->cursorCaptured) return;
        graphics::setCursorCaptured(*app, *renderInstance, false);
        restoreCapture = true;
    }

    Capture DevLayer::input(const Input& input, Capture taken) {
        if (renderInstance && !rendererReady) return {};
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();

        core::ivec2 window(1280, 720);
        int fbWidth = window.x, fbHeight = window.y;
        if (renderInstance) {
            window = renderInstance->getWindowResolution();
            glfwGetFramebufferSize(renderInstance->window, &fbWidth, &fbHeight);
        }
        io.DisplaySize = ImVec2(float(std::max(window.x, 1)), float(std::max(window.y, 1)));
        io.DisplayFramebufferScale = ImVec2(window.x > 0 ? float(fbWidth) / float(window.x) : 1.0f,
                                            window.y > 0 ? float(fbHeight) / float(window.y) : 1.0f);
        io.DeltaTime = std::max(app->time().unscaledDelta, 1.0f / 1000.0f);

        if (!taken.keyboard) {
            for (const KeyMapping& k : KEYS) {
                if (input.pressed(k.key)) io.AddKeyEvent(k.imgui, true);
                if (input.released(k.key)) io.AddKeyEvent(k.imgui, false);
            }
            io.AddKeyEvent(ImGuiMod_Ctrl, input.down(Key::LeftControl) || input.down(Key::RightControl));
            io.AddKeyEvent(ImGuiMod_Shift, input.down(Key::LeftShift) || input.down(Key::RightShift));
            io.AddKeyEvent(ImGuiMod_Alt, input.down(Key::LeftAlt) || input.down(Key::RightAlt));
            io.AddKeyEvent(ImGuiMod_Super, input.down(Key::LeftSuper) || input.down(Key::RightSuper));
            for (uint32_t c : input.text) io.AddInputCharacter(c);
        }
        if (!taken.mouse) {
            io.AddMousePosEvent(input.cursor.x, input.cursor.y);
            const MouseButton buttons[] = {MouseButton::Left, MouseButton::Right, MouseButton::Middle};
            for (int b = 0; b < 3; b++) {
                if (input.pressed(buttons[b])) io.AddMouseButtonEvent(b, true);
                if (input.released(buttons[b])) io.AddMouseButtonEvent(b, false);
            }
            if (input.scroll.x != 0.0f || input.scroll.y != 0.0f) io.AddMouseWheelEvent(input.scroll.x, input.scroll.y);
        }

        if (frameBegun) ImGui::EndFrame();   // the last frame was never drawn (no endFrame ran)
        ImGui::NewFrame();
        frameBegun = true;
        Capture took{io.WantCaptureKeyboard, io.WantCaptureMouse};
        ImGui::SetCurrentContext(previous);
        return took;
    }

    void DevLayer::draw(bgfx::ViewId, core::ivec2) {
        if (!frameBegun) return;
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context);

        // The engine's panels, then the program's, each a window.
        const float x = ImGui::GetIO().DisplaySize.x;
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420.0f, 420.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Frame")) framePanel(*app);
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(10.0f, 440.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(700.0f, 260.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Log")) logPanel(*app);
        ImGui::End();
        if (const Panels* panels = app->world.ctx().find<Panels>()) {
            float y = 10.0f;
            for (const auto& [name, panel] : panels->list) {
                ImGui::SetNextWindowPos(ImVec2(std::max(x - 370.0f, 440.0f), y), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
                y += 40.0f;
                if (ImGui::Begin(name.c_str())) panel(*app);
                ImGui::End();
            }
        }

        ImGui::Render();
        if (rendererReady) renderDrawData(ImGui::GetDrawData());
        frameBegun = false;
        ImGui::SetCurrentContext(previous);
    }
}
