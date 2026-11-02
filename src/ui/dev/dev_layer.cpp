// The developer layer (ui/dev.h): Dear ImGui as a ui::Layer. It owns an ImGui context of its own, fed
// from the engine's Input rather than from ImGui's GLFW backend (docs/plans/ui.md, D4), and draws in
// ui::DEV_VIEW. Shown and hidden with F1; while shown it releases a captured cursor, and gives it back
// when hidden.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include "ui/dev.h"
#include "ui/dev_layer.h"
#include "internal.h"
#include "ui/ui.h"

#include "core/application.h"
#include "core/time.h"
#include "graphics/input.h"
#include "graphics/render_instance.h"
#include "data_structures/constructedRenderer.h"

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

    }

    // The Frame panel's state. Sampled every frame while the layer is shown; see dev_layer.h.

    namespace {
        void framePanel(Application& app, DevLayer::Profiler& p, graphics::RenderInstance* renderInstance) {
            const Time& time = app.time();
            const Meter& frame = p.frame.meter;
            if (frame.ready()) {
                ImGui::Text("%.2f ms  %.0f fps", frame.average(), frame.average() > 0.0 ? 1000.0 / frame.average() : 0.0);
                ImGui::SameLine();
                ImGui::TextColored(MIN_TEXT, "  min %.2f", frame.min());
                ImGui::SameLine();
                ImGui::TextColored(frame.max() > frame.average() * 1.5 ? PEAK_TEXT : MIN_TEXT, " max %.2f", frame.max());
            } else {
                ImGui::TextDisabled("measuring...");
            }
            static const double PERIODS[] = {0.25, 0.5, 1.0, 2.0};
            static const char* PERIOD_NAMES[] = {"0.25 s", "0.5 s", "1 s", "2 s"};
            int periodIndex = 1;
            for (int i = 0; i < 4; i++)
                if (std::abs(PERIODS[i] - p.period()) < 1e-6) periodIndex = i;
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("averaged over", &periodIndex, PERIOD_NAMES, 4)) p.setPeriod(PERIODS[periodIndex]);
            ImGui::SameLine();
            ImGui::Checkbox("pause", &p.paused);
            ImGui::SameLine();
            ImGui::TextDisabled("frame %llu  step %llu  x%.2f", (unsigned long long)time.frame,
                                (unsigned long long)time.fixedStep, time.scale);

            if (ImGui::CollapsingHeader("CPU", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::vector<const Track*> parts;
                for (const Track& t : p.stages) parts.push_back(&t);
                parts.push_back(&p.outside);
                stackedGraph("##cpu", p.frame, parts, p.at, 70.0f);
                if (ImGui::BeginTable("##systems", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                    readingColumns("ms");
                    readingHeaders();
                    ImGui::TableHeadersRow();
                    for (size_t s = 0; s < STAGE_COUNT; s++) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        swatch(p.stages[s].color);
                        const bool open = ImGui::TreeNodeEx(p.stages[s].name.c_str(), ImGuiTreeNodeFlags_SpanAllColumns);
                        readingCells(p.stages[s].meter);
                        if (!open) continue;
                        for (const std::string& name : app.systemNames(STAGES[s])) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Indent();
                            ImGui::TextUnformatted(name.c_str());
                            ImGui::Unindent();
                            readingCells(p.systemMeter(STAGES[s], name));
                        }
                        ImGui::TreePop();
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    swatch(p.outside.color);
                    ImGui::TextUnformatted(p.outside.name.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The frame's time in no system: the platform, waiting on the display.");
                    readingCells(p.outside.meter);
                    ImGui::EndTable();
                }
            }

            if (ImGui::CollapsingHeader("GPU", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (renderInstance == nullptr) {
                    ImGui::TextDisabled("headless: no GPU");
                } else if (!p.gpuTimed) {
                    ImGui::TextDisabled("no GPU timings from %s yet", bgfx::getRendererName(bgfx::getRendererType()));
                } else {
                    const Meter& gpu = p.gpu.meter;
                    if (gpu.ready()) {
                        ImGui::Text("%.2f ms", gpu.average());
                        ImGui::SameLine();
                        ImGui::TextColored(MIN_TEXT, "  min %.2f", gpu.min());
                        ImGui::SameLine();
                        ImGui::TextColored(gpu.max() > gpu.average() * 1.5 ? PEAK_TEXT : MIN_TEXT, " max %.2f", gpu.max());
                    }
                    std::vector<const Track*> parts;
                    for (const auto& [view, track] : p.passes) parts.push_back(&track);
                    parts.push_back(&p.gpuOther);
                    stackedGraph("##gpu", p.gpu, parts, p.gpuAt, 70.0f);

                    // The active renderer's passes, in order, with their targets; then any other view.
                    std::shared_ptr<ConstructedRenderer> renderer = renderInstance->getActiveRenderer();
                    const size_t rendererPasses = renderer ? renderer->dependencyGraph.size() : 0;
                    if (ImGui::BeginTable("##passes", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                        readingColumns("pass");
                        ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                        readingHeaders();
                        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 32.0f);
                        ImGui::TableHeadersRow();
                        auto row = [&](const Track* track, const char* name, ImU32 color, core::ivec2 size, bool enabled) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            swatch(color);
                            if (enabled) ImGui::TextUnformatted(name);
                            else ImGui::TextDisabled("%s", name);
                            ImGui::TableNextColumn();
                            if (size.x > 0) ImGui::TextColored(MIN_TEXT, "%dx%d", size.x, size.y);
                            if (!enabled || track == nullptr) {
                                ImGui::TableNextColumn();
                                ImGui::TextDisabled(enabled ? "-" : "off");
                                ImGui::TableNextColumn();
                                ImGui::TableNextColumn();
                                ImGui::TableNextColumn();
                                return;
                            }
                            readingCells(track->meter);
                            ImGui::TableNextColumn();
                            if (gpu.ready() && gpu.average() > 0.0 && track->meter.ready())
                                ImGui::TextColored(MIN_TEXT, "%.0f", 100.0 * track->meter.average() / gpu.average());
                        };
                        for (size_t i = 0; i < rendererPasses; i++) {
                            const BGFXDependencyGraph& pass = renderer->dependencyGraph[i];
                            auto it = p.passes.find(uint16_t(pass.renderPassID));
                            const Track* track = it != p.passes.end() && it->second.name == pass.name && it->second.idle < 8
                                                     ? &it->second : nullptr;
                            const ImU32 color = track ? track->color
                                                      : PASS_COLORS[pass.renderPassID % (sizeof(PASS_COLORS) / sizeof(PASS_COLORS[0]))];
                            const std::string name = pass.name.empty() ? "pass " + std::to_string(i) : pass.name;
                            row(track, name.c_str(), color, pass.drawnSize, pass.enabled);
                        }
                        for (const auto& [view, track] : p.passes) {
                            if (view < rendererPasses || track.idle >= 8) continue;
                            row(&track, track.name.c_str(), track.color, core::ivec2(0, 0), true);
                        }
                        row(&p.gpuOther, p.gpuOther.name.c_str(), p.gpuOther.color, core::ivec2(0, 0), true);
                        ImGui::EndTable();
                    }
                }
            }

            if (renderInstance != nullptr && ImGui::CollapsingHeader("bgfx")) {
                const bgfx::Stats* stats = bgfx::getStats();
                ImGui::Text("%s, back buffer %ux%u, GPU latency %u", bgfx::getRendererName(bgfx::getRendererType()),
                            stats->width, stats->height, stats->maxGpuLatency);
                ImGui::Text("%u draws  %u computes  %u blits", stats->numDraw, stats->numCompute, stats->numBlit);
                if (ImGui::BeginTable("##bgfx", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                    readingColumns("ms");
                    readingHeaders();
                    ImGui::TableHeadersRow();
                    struct Line { const char* name; const Meter* meter; const char* tip; };
                    for (Line line : {Line{"between frames", &p.cpuFrame, "CPU time between two bgfx::frame calls"},
                                      Line{"submit", &p.submit, "the render thread issuing the frame's commands"},
                                      Line{"wait render", &p.waitRender, "waiting for the render thread to finish the last frame"},
                                      Line{"wait submit", &p.waitSubmit, "waiting for the API thread to hand over a frame"}}) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(line.name);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", line.tip);
                        readingCells(*line.meter);
                    }
                    ImGui::EndTable();
                }
                const double mib = 1.0 / (1024.0 * 1024.0);
                if (stats->gpuMemoryMax > 0)
                    ImGui::Text("GPU memory %.0f / %.0f MiB", double(stats->gpuMemoryUsed) * mib, double(stats->gpuMemoryMax) * mib);
                ImGui::Text("textures %.0f MiB (%u)  targets %.0f MiB (%u framebuffers)",
                            double(stats->textureMemoryUsed) * mib, stats->numTextures,
                            double(stats->rtMemoryUsed) * mib, stats->numFrameBuffers);
                ImGui::Text("%u programs  %u uniforms", stats->numPrograms, stats->numUniforms);
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

    void Meter::add(double value, double seconds) {
        newest = value;
        if (count == 0) low = high = value;
        low = std::min(low, value);
        high = std::max(high, value);
        sum += value;
        count++;
        elapsed += seconds;
        if (elapsed < period) return;
        mean = sum / double(count);
        least = low;
        greatest = high;
        published = count;
        elapsed = sum = 0.0;
        count = 0;
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
        : profiler(std::make_unique<Profiler>()), panelState(std::make_unique<PanelState>()), app(&app), renderInstance(renderInstance) {
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
        // PROJV_DEV_UI_OPEN=Physics,Renderer: windows open from the start, for captures.
        if (const char* list = std::getenv("PROJV_DEV_UI_OPEN")) {
            std::string names(list);
            for (size_t at = 0; at <= names.size();) {
                const size_t comma = std::min(names.find(',', at), names.size());
                if (comma > at) setPanelOpen(names.substr(at, comma - at), true);
                at = comma + 1;
            }
        }
    }

    DevLayer::~DevLayer() {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context);
        if (rendererReady) shutdownRenderer();
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous == context ? nullptr : previous);
    }

    bool& DevLayer::isOpen(const std::string& name) {
        // Frame, Log and a program's panels open the first time the layer is shown; the engine's
        // other panels wait for the Panels menu.
        auto [it, made] = open.try_emplace(name, false);
        if (made) {
            static const char* const closed[] = {"Physics", "Network", "Renderer", "Entities"};
            it->second = std::none_of(std::begin(closed), std::end(closed), [&](const char* c) { return name == c; });
        }
        return it->second;
    }

    void DevLayer::setPanelOpen(const std::string& name, bool show) { isOpen(name) = show; }

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
        if (!show && profiler->gpuProfiling) {
            // Hidden costs nothing: the GPU's per-pass timer queries stop with it.
            bgfx::setDebug(BGFX_DEBUG_NONE);
            profiler->gpuProfiling = false;
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

    Capture DevLayer::input(const Input& input, Capture taken) {
        if (renderInstance && !rendererReady) return {};
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();

        core::ivec2 window(1280, 720);
        int fbWidth = window.x, fbHeight = window.y;
        if (renderInstance) {
            window = renderInstance->getWindowResolution();
            // The back buffer bgfx draws into, not GLFW's framebuffer: the engine resets bgfx to the
            // window's size, so on a scaled display (Wayland at 170%) GLFW's framebuffer is 1.7x the
            // back buffer. Taken from there, every scissor came out 1.7x too large and too far
            // down: a panel's first line cut off, the Log's contents clipped away entirely.
            if (const bgfx::Stats* stats = bgfx::getStats(); stats && stats->width > 0 && stats->height > 0) {
                fbWidth = stats->width;
                fbHeight = stats->height;
            } else {
                fbWidth = window.x;
                fbHeight = window.y;
            }
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
        if (input.cursorCaptured) {
            // The game's (mouse-look): the panels see no cursor, so they neither hover nor take a
            // click, and the game keeps all of it. Buttons still end, so nothing stays held.
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            for (int b = 0; b < 3; b++) io.AddMouseButtonEvent(b, false);
        } else if (!taken.mouse) {
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

        // Readings first, every frame shown, so a collapsed Frame window still has its history.
        const bool gpuReadable = renderInstance && rendererReady;
        if (gpuReadable) {
            bgfx::setViewName(DEV_VIEW, "dev ui");
            if (!profiler->gpuProfiling) {
                bgfx::setDebug(BGFX_DEBUG_PROFILER);
                profiler->gpuProfiling = true;
            }
        }
        if (!profiler->paused) profiler->sample(*app, gpuReadable);

        // A menu bar to open and close every window, then the windows: the engine's (each only while
        // its subject exists), then the program's.
        const float x = ImGui::GetIO().DisplaySize.x;
        const Panels* programPanels = app->world.ctx().find<Panels>();
        struct Window { const char* name; bool present; };
        const Window engineWindows[] = {
            {"Frame", true},
            {"Log", true},
            {"Physics", physicsPanelHasSubject(*app)},
            {"Network", networkPanelHasSubject(*app)},
            {"Renderer", rendererPanelHasSubject(renderInstance)},
            {"Entities", true},
        };
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Panels")) {
                for (const Window& w : engineWindows)
                    if (w.present) ImGui::MenuItem(w.name, nullptr, &isOpen(w.name));
                if (programPanels && !programPanels->list.empty()) {
                    ImGui::Separator();
                    for (const auto& [name, panel] : programPanels->list) ImGui::MenuItem(name.c_str(), nullptr, &isOpen(name));
                }
                ImGui::EndMenu();
            }
            ImGui::TextDisabled("  F1 hides");
            ImGui::EndMainMenuBar();
        }
        drawn.clear();
        panelState->paused = profiler->paused;
        auto window = [&](const std::string& name, ImVec2 pos, ImVec2 size, auto&& body) {
            bool& open = isOpen(name);
            if (!open) return;
            ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
            if (ImGui::Begin(name.c_str(), &open)) body();
            ImGui::End();
            drawn.push_back(name);
        };
        window("Frame", ImVec2(10.0f, 30.0f), ImVec2(440.0f, 540.0f), [&] { framePanel(*app, *profiler, renderInstance); });
        window("Log", ImVec2(10.0f, 580.0f), ImVec2(700.0f, 190.0f), [&] { logPanel(*app); });
        const float right = 460.0f;   // beside Frame: the far right is the program panels'
        if (engineWindows[2].present)
            window("Physics", ImVec2(right, 30.0f), ImVec2(460.0f, 360.0f), [&] { physicsPanel(*app, *panelState); });
        if (engineWindows[3].present)
            window("Network", ImVec2(right, 60.0f), ImVec2(460.0f, 360.0f), [&] { networkPanel(*app, *panelState); });
        if (engineWindows[4].present)
            window("Renderer", ImVec2(right, 90.0f), ImVec2(460.0f, 600.0f), [&] { rendererPanel(*app, *panelState, *renderInstance, *profiler); });
        window("Entities", ImVec2(right, 120.0f), ImVec2(560.0f, 480.0f), [&] { entitiesPanel(*app, *panelState); });
        if (programPanels) {
            float y = 30.0f;
            for (const auto& [name, panel] : programPanels->list) {
                window(name, ImVec2(std::max(x - 370.0f, 460.0f), y), ImVec2(360.0f, 0.0f), [&] { panel(*app); });
                y += 40.0f;
            }
        }

        ImGui::Render();
        if (rendererReady) renderDrawData(ImGui::GetDrawData());
        frameBegun = false;
        ImGui::SetCurrentContext(previous);
    }
}
