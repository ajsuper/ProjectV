// The developer layer (ui/dev.h): Dear ImGui as a ui::Layer. It owns an ImGui context of its own, fed
// from the engine's Input rather than from ImGui's GLFW backend (docs/plans/ui.md, D4), and draws in
// ui::DEV_VIEW. Shown and hidden with F1; while shown it releases a captured cursor, and gives it back
// when hidden.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
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

        // ---- The Frame panel: readings and graphs ------------------------------------------------

        constexpr int HISTORY = 240;   // columns in a graph: frames for the CPU, GPU frames for the GPU
        constexpr size_t STAGE_COUNT = 5;
        constexpr Stage STAGES[STAGE_COUNT] = {Stage::PreUpdate, Stage::FixedUpdate, Stage::Update,
                                               Stage::PostUpdate, Stage::Render};
        constexpr ImU32 STAGE_COLORS[STAGE_COUNT] = {
            IM_COL32(120, 144, 156, 255),   // PreUpdate
            IM_COL32(255, 152, 0, 255),     // FixedUpdate
            IM_COL32(102, 187, 106, 255),   // Update
            IM_COL32(171, 71, 188, 255),    // PostUpdate
            IM_COL32(66, 165, 245, 255),    // Render
        };
        constexpr ImU32 OTHER_COLOR = IM_COL32(97, 97, 97, 255);
        constexpr ImU32 UI_COLOR = IM_COL32(240, 98, 146, 255);
        // The passes take these in view order, so a renderer's passes keep their colours frame to frame.
        constexpr ImU32 PASS_COLORS[] = {
            IM_COL32(78, 121, 167, 255), IM_COL32(242, 142, 43, 255), IM_COL32(89, 161, 79, 255),
            IM_COL32(225, 87, 89, 255), IM_COL32(118, 183, 178, 255), IM_COL32(237, 201, 72, 255),
            IM_COL32(176, 122, 161, 255), IM_COL32(255, 157, 167, 255), IM_COL32(156, 117, 95, 255),
            IM_COL32(186, 176, 172, 255),
        };
        constexpr ImVec4 MIN_TEXT(0.62f, 0.66f, 0.70f, 1.0f);
        constexpr ImVec4 PEAK_TEXT(1.0f, 0.62f, 0.32f, 1.0f);

        // One thing timed each column: a stage, a pass, the rest of a frame.
        struct Track {
            Track() = default;
            explicit Track(std::string name) : name(std::move(name)) {}
            std::string name;
            ImU32 color = OTHER_COLOR;
            Meter meter;
            float history[HISTORY] = {};
            int idle = 0;   // columns since it was last sampled (a pass not drawn)
        };

        double toMs(int64_t ticks, int64_t frequency) {
            return frequency > 0 ? double(ticks) * 1000.0 / double(frequency) : 0.0;
        }

        // The graph's height in ms: the next step above nearly all of its columns, so it changes rarely
        // and a one-off hitch (a load, a resize) does not flatten the rest. Taller columns are cut at
        // the top and marked.
        float graphScale(const Track& total) {
            float sorted[HISTORY];
            std::copy(std::begin(total.history), std::end(total.history), sorted);
            std::nth_element(sorted, sorted + HISTORY * 97 / 100, sorted + HISTORY);
            const float typical = sorted[HISTORY * 97 / 100];
            constexpr float STEPS[] = {2.0f, 4.0f, 8.333f, 16.667f, 33.333f, 50.0f, 66.667f, 100.0f,
                                       150.0f, 250.0f, 500.0f, 1000.0f};
            for (float step : STEPS)
                if (typical * 1.1f <= step) return step;
            return typical * 1.1f;
        }

        // A coloured square the height of a line, for legends.
        void swatch(ImU32 color) {
            const float h = ImGui::GetTextLineHeight();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + 1.0f, p.y + 2.0f),
                                                      ImVec2(p.x + h - 3.0f, p.y + h - 2.0f), color, 2.0f);
            ImGui::Dummy(ImVec2(h - 2.0f, h));
            ImGui::SameLine();
        }

        // A meter's average, least and greatest, one table cell each. A peak well over the average
        // stands out.
        void readingCells(const Meter& meter) {
            ImGui::TableNextColumn();
            if (!meter.ready()) {
                ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                return;
            }
            ImGui::Text("%.2f", meter.average());
            ImGui::TableNextColumn();
            ImGui::TextColored(MIN_TEXT, "%.2f", meter.min());
            ImGui::TableNextColumn();
            const bool spike = meter.max() > meter.average() * 1.5 + 0.05;
            if (spike) ImGui::TextColored(PEAK_TEXT, "%.2f", meter.max());
            else ImGui::TextColored(MIN_TEXT, "%.2f", meter.max());
        }

        void readingColumns(const char* first, float firstWidth = 0.0f) {
            ImGui::TableSetupColumn(first, firstWidth > 0.0f ? ImGuiTableColumnFlags_WidthFixed
                                                             : ImGuiTableColumnFlags_WidthStretch, firstWidth);
        }

        void readingHeaders() {
            ImGui::TableSetupColumn("avg", ImGuiTableColumnFlags_WidthFixed, 52.0f);
            ImGui::TableSetupColumn("min", ImGuiTableColumnFlags_WidthFixed, 52.0f);
            ImGui::TableSetupColumn("max", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        }

        // A bar a column, oldest on the left, each part stacked in its colour; `total` sets the scale
        // and heads the tooltip. Guides at 120, 60 and 30 fps.
        void stackedGraph(const char* id, const Track& total, const std::vector<const Track*>& parts,
                          int at, float height) {
            const float width = std::max(ImGui::GetContentRegionAvail().x, 50.0f);
            ImGui::InvisibleButton(id, ImVec2(width, height));
            const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(lo, hi, IM_COL32(20, 22, 26, 230), 3.0f);
            const float scale = graphScale(total);
            const float column = width / float(HISTORY);
            for (int i = 0; i < HISTORY; i++) {
                const int slot = (at + i) % HISTORY;
                const float x0 = lo.x + float(i) * column, x1 = lo.x + float(i + 1) * column;
                float y = hi.y;
                for (const Track* part : parts) {
                    const float h = part->history[slot] / scale * height;
                    if (h <= 0.0f) continue;
                    draw->AddRectFilled(ImVec2(x0, std::max(y - h, lo.y)), ImVec2(x1, y), part->color);
                    y -= h;
                    if (y <= lo.y) break;
                }
                if (total.history[slot] > scale)   // over the top: cut, and marked
                    draw->AddRectFilled(ImVec2(x0, lo.y), ImVec2(x1, lo.y + 3.0f), IM_COL32(255, 82, 82, 255));
            }
            struct Guide { float ms; const char* label; };
            const float lineHeight = ImGui::GetTextLineHeight();
            float lastLabel = hi.y + lineHeight;   // labels go up the graph; one too close to the last is left out
            for (Guide guide : {Guide{8.333f, "120 fps"}, Guide{16.667f, "60 fps"}, Guide{33.333f, "30 fps"}}) {
                if (guide.ms >= scale) continue;
                const float y = hi.y - guide.ms / scale * height;
                draw->AddLine(ImVec2(lo.x, y), ImVec2(hi.x, y), IM_COL32(255, 255, 255, 70));
                if (lastLabel - y < lineHeight || y - lineHeight < lo.y) continue;
                draw->AddText(ImVec2(lo.x + 4.0f, y - lineHeight), IM_COL32(255, 255, 255, 120), guide.label);
                lastLabel = y;
            }
            char top[32];
            std::snprintf(top, sizeof(top), "%.1f ms", scale);
            draw->AddText(ImVec2(hi.x - ImGui::CalcTextSize(top).x - 4.0f, lo.y + 2.0f), IM_COL32(255, 255, 255, 120), top);

            if (ImGui::IsItemHovered()) {
                const int i = std::clamp(int((ImGui::GetIO().MousePos.x - lo.x) / column), 0, HISTORY - 1);
                const int slot = (at + i) % HISTORY;
                const float x = lo.x + (float(i) + 0.5f) * column;
                draw->AddLine(ImVec2(x, lo.y), ImVec2(x, hi.y), IM_COL32(255, 255, 255, 160));
                ImGui::BeginTooltip();
                ImGui::Text("%d ago: %.2f ms", HISTORY - 1 - i, total.history[slot]);
                for (auto part = parts.rbegin(); part != parts.rend(); ++part) {
                    if ((*part)->history[slot] <= 0.0f) continue;
                    swatch((*part)->color);
                    ImGui::Text("%-22s %6.2f", (*part)->name.c_str(), (*part)->history[slot]);
                }
                ImGui::EndTooltip();
            }
        }
    }

    // The Frame panel's state. Sampled every frame while the layer is shown; see dev_layer.h.
    struct DevLayer::Profiler {
        bool paused = false;
        bool gpuProfiling = false;   // this layer turned BGFX_DEBUG_PROFILER on

        // CPU: a column a frame.
        int at = 0;
        Track frame{"frame"};
        Track stages[STAGE_COUNT];
        Track outside{"outside systems"};   // the frame's time in no system
        std::map<std::string, Meter> systems;   // by "<stage>/<name>"

        // GPU: a column a GPU frame (they arrive a frame or more late, and not one per frame).
        int gpuAt = 0;
        uint32_t gpuFrame = UINT32_MAX;
        bool gpuTimed = false;
        Track gpu{"gpu frame"};
        Track gpuOther{"between passes"};
        std::map<uint16_t, Track> passes;   // by view
        Meter cpuFrame, submit, waitRender, waitSubmit;

        Profiler() {
            for (size_t s = 0; s < STAGE_COUNT; s++) {
                stages[s].name = stageName(STAGES[s]);
                stages[s].color = STAGE_COLORS[s];
            }
        }

        void setPeriod(double seconds) {
            for (Meter* m : {&frame.meter, &outside.meter, &gpu.meter, &gpuOther.meter, &cpuFrame, &submit,
                             &waitRender, &waitSubmit})
                m->setPeriod(seconds);
            for (Track& t : stages) t.meter.setPeriod(seconds);
            for (auto& [key, meter] : systems) meter.setPeriod(seconds);
            for (auto& [view, track] : passes) track.meter.setPeriod(seconds);
        }

        double period() const { return frame.meter.periodSeconds(); }

        Meter& systemMeter(Stage stage, const std::string& name) {
            auto [it, made] = systems.try_emplace(std::string(stageName(stage)) + "/" + name, period());
            return it->second;
        }

        void sample(Application& app, bool gpuReadable) {
            const float dt = app.time().unscaledDelta;
            const double frameMs = double(dt) * 1000.0;
            frame.meter.add(frameMs, dt);
            frame.history[at] = float(frameMs);
            double inSystems = 0.0;
            for (size_t s = 0; s < STAGE_COUNT; s++) {
                const std::vector<std::string>& names = app.systemNames(STAGES[s]);
                const std::vector<double> seconds = app.systemSeconds(STAGES[s]);
                double total = 0.0;
                for (size_t i = 0; i < names.size() && i < seconds.size(); i++) {
                    systemMeter(STAGES[s], names[i]).add(seconds[i] * 1000.0, dt);
                    total += seconds[i] * 1000.0;
                }
                stages[s].meter.add(total, dt);
                stages[s].history[at] = float(total);
                inSystems += total;
            }
            const double rest = std::max(frameMs - inSystems, 0.0);
            outside.meter.add(rest, dt);
            outside.history[at] = float(rest);
            at = (at + 1) % HISTORY;

            if (!gpuReadable) return;
            const bgfx::Stats* stats = bgfx::getStats();
            if (stats == nullptr) return;
            if (stats->cpuTimerFreq > 0) {
                cpuFrame.add(toMs(stats->cpuTimeFrame, stats->cpuTimerFreq), dt);
                submit.add(toMs(stats->cpuTimeEnd - stats->cpuTimeBegin, stats->cpuTimerFreq), dt);
                waitRender.add(toMs(stats->waitRender, stats->cpuTimerFreq), dt);
                waitSubmit.add(toMs(stats->waitSubmit, stats->cpuTimerFreq), dt);
            }
            gpuTimed = stats->gpuTimerFreq > 0 && stats->numViews > 0;
            if (!gpuTimed || stats->gpuFrameNum == gpuFrame) return;
            gpuFrame = stats->gpuFrameNum;

            const double gpuMs = std::max(toMs(stats->gpuTimeEnd - stats->gpuTimeBegin, stats->gpuTimerFreq), 0.0);
            gpu.meter.add(gpuMs, dt);
            gpu.history[gpuAt] = float(gpuMs);
            for (auto& [view, track] : passes) {
                track.history[gpuAt] = 0.0f;
                track.idle++;
            }
            double inPasses = 0.0;
            for (uint16_t v = 0; v < stats->numViews; v++) {
                const bgfx::ViewStats& view = stats->viewStats[v];
                std::string name = view.name[0] != '\0' ? std::string(view.name) : "view " + std::to_string(view.view);
                if (view.view == DEV_VIEW && view.name[0] == '\0') name = "dev ui";
                if (view.view == USER_VIEW && view.name[0] == '\0') name = "user ui";
                Track& track = passes[view.view];
                if (track.name != name) {
                    // A new pass, or another renderer's in this view (the gallery switching): start over.
                    track = Track{};
                    track.name = std::move(name);
                    track.meter.setPeriod(period());
                    track.color = view.view >= USER_VIEW ? UI_COLOR
                                                         : PASS_COLORS[view.view % (sizeof(PASS_COLORS) / sizeof(PASS_COLORS[0]))];
                }
                const double ms = std::max(toMs(view.gpuTimeEnd - view.gpuTimeBegin, stats->gpuTimerFreq), 0.0);
                track.meter.add(ms, dt);
                track.history[gpuAt] = float(ms);
                track.idle = 0;
                inPasses += ms;
            }
            const double between = std::max(gpuMs - inPasses, 0.0);
            gpuOther.meter.add(between, dt);
            gpuOther.history[gpuAt] = float(between);
            for (auto it = passes.begin(); it != passes.end();) {
                if (it->second.idle > HISTORY) it = passes.erase(it);
                else ++it;
            }
            gpuAt = (gpuAt + 1) % HISTORY;
        }
    };

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
        : profiler(std::make_unique<Profiler>()), app(&app), renderInstance(renderInstance) {
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

        // The engine's panels, then the program's, each a window.
        const float x = ImGui::GetIO().DisplaySize.x;
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(440.0f, 540.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Frame")) framePanel(*app, *profiler, renderInstance);
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(10.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(700.0f, 200.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Log")) logPanel(*app);
        ImGui::End();
        if (const Panels* panels = app->world.ctx().find<Panels>()) {
            float y = 10.0f;
            for (const auto& [name, panel] : panels->list) {
                ImGui::SetNextWindowPos(ImVec2(std::max(x - 370.0f, 460.0f), y), ImGuiCond_FirstUseEver);
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
