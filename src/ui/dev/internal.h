// The dev layer's insides, shared by its panels (src/ui/dev/): the widgets every reading is drawn
// with, and the Frame panel's profiler, which the renderer panel reads its pass times from. Not a
// public header.
#ifndef PROJV_UI_DEV_INTERNAL_H
#define PROJV_UI_DEV_INTERNAL_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <bgfx/bgfx.h>

#include "ui/dev.h"
#include "ui/dev_layer.h"
#include "ui/ui.h"
#include "core/application.h"
#include "core/time.h"
#include "core/world.h"

namespace projv {
    struct ConstructedRenderer;
}
namespace projv::graphics {
    class RenderInstance;
}

namespace projv::ui::dev {
    // Inline, not anonymous: the profiler below holds these, and is shared between files.
    inline namespace widgets {
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

        inline double toMs(int64_t ticks, int64_t frequency) {
            return frequency > 0 ? double(ticks) * 1000.0 / double(frequency) : 0.0;
        }

        // The graph's height in ms: the next step above nearly all of its columns, so it changes rarely
        // and a one-off hitch (a load, a resize) does not flatten the rest. Taller columns are cut at
        // the top and marked.
        inline float graphScale(const Track& total) {
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
        inline void swatch(ImU32 color) {
            const float h = ImGui::GetTextLineHeight();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + 1.0f, p.y + 2.0f),
                                                      ImVec2(p.x + h - 3.0f, p.y + h - 2.0f), color, 2.0f);
            ImGui::Dummy(ImVec2(h - 2.0f, h));
            ImGui::SameLine();
        }

        // A meter's average, least and greatest, one table cell each. A peak well over the average
        // stands out.
        inline void readingCells(const Meter& meter) {
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

        inline void readingColumns(const char* first, float firstWidth = 0.0f) {
            ImGui::TableSetupColumn(first, firstWidth > 0.0f ? ImGuiTableColumnFlags_WidthFixed
                                                             : ImGuiTableColumnFlags_WidthStretch, firstWidth);
        }

        inline void readingHeaders() {
            ImGui::TableSetupColumn("avg", ImGuiTableColumnFlags_WidthFixed, 52.0f);
            ImGui::TableSetupColumn("min", ImGuiTableColumnFlags_WidthFixed, 52.0f);
            ImGui::TableSetupColumn("max", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        }

        // A bar a column, oldest on the left, each part stacked in its colour; `total` sets the scale
        // and heads the tooltip. Guides at 120, 60 and 30 fps.
        inline void stackedGraph(const char* id, const Track& total, const std::vector<const Track*>& parts,
                          int at, float height, const char* unit = "ms") {
            const bool milliseconds = std::string(unit) == "ms";
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
                if (!milliseconds || guide.ms >= scale) continue;
                const float y = hi.y - guide.ms / scale * height;
                draw->AddLine(ImVec2(lo.x, y), ImVec2(hi.x, y), IM_COL32(255, 255, 255, 70));
                if (lastLabel - y < lineHeight || y - lineHeight < lo.y) continue;
                draw->AddText(ImVec2(lo.x + 4.0f, y - lineHeight), IM_COL32(255, 255, 255, 120), guide.label);
                lastLabel = y;
            }
            char top[32];
            std::snprintf(top, sizeof(top), "%.1f %s", scale, unit);
            draw->AddText(ImVec2(hi.x - ImGui::CalcTextSize(top).x - 4.0f, lo.y + 2.0f), IM_COL32(255, 255, 255, 120), top);

            if (ImGui::IsItemHovered()) {
                const int i = std::clamp(int((ImGui::GetIO().MousePos.x - lo.x) / column), 0, HISTORY - 1);
                const int slot = (at + i) % HISTORY;
                const float x = lo.x + (float(i) + 0.5f) * column;
                draw->AddLine(ImVec2(x, lo.y), ImVec2(x, hi.y), IM_COL32(255, 255, 255, 160));
                ImGui::BeginTooltip();
                ImGui::Text("%d ago: %.2f %s", HISTORY - 1 - i, total.history[slot], unit);
                for (auto part = parts.rbegin(); part != parts.rend(); ++part) {
                    if ((*part)->history[slot] <= 0.0f) continue;
                    swatch((*part)->color);
                    ImGui::Text("%-22s %6.2f", (*part)->name.c_str(), (*part)->history[slot]);
                }
                ImGui::EndTooltip();
            }
        }
    }

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
    // ---- The engine's other panels (panels.cpp) ---------------------------------------------------

    // What a program bound with editable(), in world.ctx().
    struct Editable {
        enum Kind { Float, Int, Bool, Vec3, Vec4 };
        const ConstructedRenderer* renderer = nullptr;   // compared, never followed
        std::string uniform, label;
        Kind kind = Float;
        void* value = nullptr;
        float min = 0.0f, max = 0.0f, speed = 0.01f;
    };
    struct Editables {
        std::vector<Editable> list;
    };

    // The panels' readings and selections, held by the layer.
    struct PanelState {
        bool paused = false;   // the Frame panel's pause, for all of them
        struct Physics {
            bool started = false;
            uint64_t lastSteps = 0;
            int at = 0;
            Track perFrame{"physics"};
            Meter step, stepsPerFrame, bodies, awake, debris;
        } physics;
        struct Network {
            struct Client {
                bool started = false, seen = false;
                uint64_t lastSent = 0, lastReceived = 0;
                Track upload;
                Meter roundTrip, loss, download;
            };
            bool started = false;
            uint64_t lastSnapshots = 0;
            Meter snapshotsPerSecond;
            int at = 0;
            Track upload{"sent"};
            std::map<uint16_t, Client> clients;
        } network;
        struct Renderer {
            int enlarged = -1;   // the target shown full size, or -1
        } renderer;
        struct Entities {
            ImGuiTextFilter filter;
            Entity selected = entt::null;
        } entities;
    };

    bool physicsPanelHasSubject(const Application& app);
    void physicsPanel(Application& app, PanelState& state);
    bool networkPanelHasSubject(const Application& app);
    void networkPanel(Application& app, PanelState& state);
    bool rendererPanelHasSubject(graphics::RenderInstance* renderInstance);
    void rendererPanel(Application& app, PanelState& state, graphics::RenderInstance& renderInstance,
                       const DevLayer::Profiler& profiler);
    void entitiesPanel(Application& app, PanelState& state);
}

#endif
