// The engine's panels beyond Frame and Log (docs/plans/ui.md, U2): Physics, Network, Renderer and
// Entities. Each draws only while its subject exists -- no physics, no Physics window. The dev layer
// may see the runtime (D14); the runtime never sees it.

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <bgfx/bgfx.h>

#include "internal.h"

#include "data_structures/constructedRenderer.h"
#include "graphics/render_instance.h"
#include "runtime/entities.h"
#include "runtime/physics.h"
#include "runtime/physics/physics_world.h"
#include "runtime/replication.h"

namespace projv::ui::dev {
    // ---- Editable bindings (dev.h, D17) -----------------------------------------------------------

    namespace {
        void addBinding(Application& app, const ConstructedRenderer& renderer, std::string uniform, Editable::Kind kind,
                  void* value, EditRange range) {
            auto& context = app.world.ctx();
            if (!context.contains<Editables>()) context.emplace<Editables>();
            Editable e;
            e.renderer = &renderer;
            e.uniform = std::move(uniform);
            e.label = range.label ? std::string(range.label) : e.uniform;
            e.kind = kind;
            e.value = value;
            e.min = range.min;
            e.max = range.max;
            e.speed = range.speed;
            context.get<Editables>().list.push_back(std::move(e));
        }
    }

    void editable(Application& app, const ConstructedRenderer& r, std::string u, float* v, EditRange range) { addBinding(app, r, std::move(u), Editable::Float, v, range); }
    void editable(Application& app, const ConstructedRenderer& r, std::string u, int* v, EditRange range) { addBinding(app, r, std::move(u), Editable::Int, v, range); }
    void editable(Application& app, const ConstructedRenderer& r, std::string u, bool* v, EditRange range) { addBinding(app, r, std::move(u), Editable::Bool, v, range); }
    void editable(Application& app, const ConstructedRenderer& r, std::string u, core::vec3* v, EditRange range) { addBinding(app, r, std::move(u), Editable::Vec3, v, range); }
    void editable(Application& app, const ConstructedRenderer& r, std::string u, core::vec4* v, EditRange range) { addBinding(app, r, std::move(u), Editable::Vec4, v, range); }

    void forgetEditables(Application& app, const ConstructedRenderer& renderer) {
        if (Editables* editables = app.world.ctx().find<Editables>()) {
            std::erase_if(editables->list, [&](const Editable& e) { return e.renderer == &renderer; });
        }
    }

    namespace {
        // One bound value's control. True when it changed.
        bool editControl(const Editable& e) {
            ImGui::PushID(e.value);
            const bool ranged = e.max > e.min;
            bool changed = false;
            ImGui::SetNextItemWidth(-1.0f);
            switch (e.kind) {
                case Editable::Float:
                    changed = ranged ? ImGui::SliderFloat("##v", static_cast<float*>(e.value), e.min, e.max)
                                     : ImGui::DragFloat("##v", static_cast<float*>(e.value), e.speed);
                    break;
                case Editable::Int:
                    changed = ranged ? ImGui::SliderInt("##v", static_cast<int*>(e.value), int(e.min), int(e.max))
                                     : ImGui::DragInt("##v", static_cast<int*>(e.value), std::max(e.speed, 0.05f));
                    break;
                case Editable::Bool:
                    changed = ImGui::Checkbox("##v", static_cast<bool*>(e.value));
                    break;
                case Editable::Vec3:
                    changed = ranged ? ImGui::SliderFloat3("##v", &static_cast<core::vec3*>(e.value)->x, e.min, e.max)
                                     : ImGui::DragFloat3("##v", &static_cast<core::vec3*>(e.value)->x, e.speed);
                    break;
                case Editable::Vec4:
                    changed = ranged ? ImGui::SliderFloat4("##v", &static_cast<core::vec4*>(e.value)->x, e.min, e.max)
                                     : ImGui::DragFloat4("##v", &static_cast<core::vec4*>(e.value)->x, e.speed);
                    break;
            }
            ImGui::PopID();
            return changed;
        }

        // A counter that should stay 0: red when it is not.
        void counterRow(const char* name, uint64_t value, const char* tip) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            ImGui::TableNextColumn();
            if (value == 0) ImGui::TextColored(MIN_TEXT, "0");
            else ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%llu", (unsigned long long)value);
        }

        void meterRow(const char* name, const Meter& meter, const char* tip = nullptr) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            readingCells(meter);
        }

        const char* formatName(bgfx::TextureFormat::Enum format) {
            switch (format) {
                case bgfx::TextureFormat::R8: return "R8";
                case bgfx::TextureFormat::RG8: return "RG8";
                case bgfx::TextureFormat::RGBA8: return "RGBA8";
                case bgfx::TextureFormat::BGRA8: return "BGRA8";
                case bgfx::TextureFormat::R16F: return "R16F";
                case bgfx::TextureFormat::RG16F: return "RG16F";
                case bgfx::TextureFormat::RGBA16F: return "RGBA16F";
                case bgfx::TextureFormat::R32F: return "R32F";
                case bgfx::TextureFormat::RG32F: return "RG32F";
                case bgfx::TextureFormat::RGBA32F: return "RGBA32F";
                case bgfx::TextureFormat::RG11B10F: return "RG11B10F";
                case bgfx::TextureFormat::RGB10A2: return "RGB10A2";
                case bgfx::TextureFormat::R32U: return "R32U";
                case bgfx::TextureFormat::RG32U: return "RG32U";
                case bgfx::TextureFormat::RGBA32U: return "RGBA32U";
                case bgfx::TextureFormat::R16U: return "R16U";
                case bgfx::TextureFormat::RGBA16U: return "RGBA16U";
                case bgfx::TextureFormat::D24S8: return "D24S8";
                case bgfx::TextureFormat::D32F: return "D32F";
                default: return "other";
            }
        }

        // Whether ImGui's float sampler can read it: not an integer format, not depth.
        bool viewable(bgfx::TextureFormat::Enum format) {
            switch (format) {
                case bgfx::TextureFormat::R8I: case bgfx::TextureFormat::R8U:
                case bgfx::TextureFormat::R16I: case bgfx::TextureFormat::R16U:
                case bgfx::TextureFormat::R32I: case bgfx::TextureFormat::R32U:
                case bgfx::TextureFormat::RG8I: case bgfx::TextureFormat::RG8U:
                case bgfx::TextureFormat::RG16I: case bgfx::TextureFormat::RG16U:
                case bgfx::TextureFormat::RG32I: case bgfx::TextureFormat::RG32U:
                case bgfx::TextureFormat::RGB8I: case bgfx::TextureFormat::RGB8U:
                case bgfx::TextureFormat::RGBA8I: case bgfx::TextureFormat::RGBA8U:
                case bgfx::TextureFormat::RGBA16I: case bgfx::TextureFormat::RGBA16U:
                case bgfx::TextureFormat::RGBA32I: case bgfx::TextureFormat::RGBA32U:
                    return false;
                default:
                    return format < bgfx::TextureFormat::UnknownDepth;
            }
        }
    }

    // ---- Physics -----------------------------------------------------------------------------------

    bool physicsPanelHasSubject(const Application& app) { return runtime::hasPhysics(app.world); }

    void physicsPanel(Application& app, PanelState& state) {
        PanelState::Physics& p = state.physics;
        const runtime::PhysicsStats& stats = runtime::physicsWorld(app.world).stats();
        const float dt = app.time().unscaledDelta;
        if (!state.paused) {
            const uint64_t advanced = p.started ? stats.steps - p.lastSteps : 0;
            p.started = true;
            p.lastSteps = stats.steps;
            const double frameMs = double(advanced) * stats.lastStepMilliseconds;
            if (advanced > 0) p.step.add(stats.lastStepMilliseconds, dt);
            p.stepsPerFrame.add(double(advanced), dt);
            p.perFrame.meter.add(frameMs, dt);
            p.perFrame.history[p.at] = float(frameMs);
            p.bodies.add(stats.bodies, dt);
            p.awake.add(stats.awakeBodies, dt);
            p.debris.add(double(app.world.view<projv::Debris>().size()), dt);
            p.at = (p.at + 1) % HISTORY;
        }

        ImGui::Text("%u bodies, %u awake", stats.bodies, stats.awakeBodies);
        ImGui::SameLine();
        ImGui::TextColored(MIN_TEXT, "  step %llu, %d threads", (unsigned long long)stats.steps, stats.threads);
        p.perFrame.color = STAGE_COLORS[1];
        stackedGraph("##physics", p.perFrame, {&p.perFrame}, p.at, 60.0f);
        if (ImGui::BeginTable("##physicsReadings", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            readingColumns("");
            readingHeaders();
            ImGui::TableHeadersRow();
            meterRow("step ms", p.step, "wall time of one step (PhysicsStats::lastStepMilliseconds)");
            meterRow("steps a frame", p.stepsPerFrame);
            meterRow("ms a frame", p.perFrame.meter, "steps this frame x the last step's time: the graph");
            meterRow("bodies", p.bodies);
            meterRow("awake", p.awake);
            meterRow("debris", p.debris, "cheap debris pieces (PhysicsConfig::debris): drawn, not simulated");
            ImGui::EndTable();
        }
        if (ImGui::BeginTable("##physicsCounters", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            counterRow("steps with dropped contacts", stats.stepsWithDroppedContacts, "Jolt ran out of contact room: capacities too small");
            counterRow("bodies refused", stats.refusedBodies, "createBody refused: bad input or a full table");
            counterRow("non-finite recoveries", stats.nonFiniteRecoveries, "bodies put back after a NaN pose or velocity: a bug");
            counterRow("left the world", stats.leftWorld, "reports of bodies outside the world bounds");
            ImGui::EndTable();
        }
    }

    // ---- Network -----------------------------------------------------------------------------------

    bool networkPanelHasSubject(const Application& app) {
        return runtime::sessionRole(app.world) != runtime::SessionRole::None;
    }

    void networkPanel(Application& app, PanelState& state) {
        PanelState::Network& n = state.network;
        const runtime::SessionRole role = runtime::sessionRole(app.world);
        const runtime::SessionStats session = runtime::sessionStats(app.world);
        const float dt = std::max(app.time().unscaledDelta, 1e-4f);

        if (role == runtime::SessionRole::Client) {
            if (!state.paused) {
                if (n.started) n.snapshotsPerSecond.add(double(session.snapshotsReceived - n.lastSnapshots) / dt, dt);
                n.lastSnapshots = session.snapshotsReceived;
                n.started = true;
            }
            ImGui::Text("client %u, %s", unsigned(session.you), session.connected ? "connected" : "connecting");
            if (!session.refusal.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "refused: %s", session.refusal.c_str());
            ImGui::TextColored(MIN_TEXT, "player %u, server tick ~%llu, %s", session.player.value,
                               (unsigned long long)session.serverTick, session.predicting ? "predicting" : "not predicting");
            if (ImGui::BeginTable("##client", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                readingColumns("");
                readingHeaders();
                ImGui::TableHeadersRow();
                meterRow("snapshots / s", n.snapshotsPerSecond);
                ImGui::EndTable();
            }
            if (ImGui::BeginTable("##clientCounters", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                auto plain = [](const char* name, const char* format, auto value) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(name);
                    ImGui::TableNextColumn();
                    ImGui::Text(format, value);
                };
                plain("snapshots received", "%llu", (unsigned long long)session.snapshotsReceived);
                plain("world changes applied", "%llu", (unsigned long long)session.worldChanges);
                plain("reconciliations", "%llu", (unsigned long long)session.reconciliations);
                plain("largest correction, m", "%.3f", double(session.largestCorrection));
                counterRow("world mismatches", session.worldMismatches, "changed voxels that are not the server's after the change");
                counterRow("messages refused", session.messagesRefused, "read off the wire and refused (BitReader::failed)");
                ImGui::EndTable();
            }
            return;
        }

        // The server: one track a client, of what it is sent a second.
        const std::vector<runtime::ClientStats> clients = runtime::clientStats(app.world);
        if (!state.paused) {
            double total = 0.0;
            for (auto& [id, c] : n.clients) c.seen = false;
            size_t index = 0;
            for (const runtime::ClientStats& stats : clients) {
                PanelState::Network::Client& c = n.clients[stats.id];
                if (c.upload.name.empty()) {
                    c.upload.name = stats.name.empty() ? "client " + std::to_string(stats.id) : stats.name;
                    c.upload.color = PASS_COLORS[index % (sizeof(PASS_COLORS) / sizeof(PASS_COLORS[0]))];
                }
                const double kbSent = c.started ? double(stats.bytesSent - c.lastSent) / 1024.0 / dt : 0.0;
                const double kbReceived = c.started ? double(stats.bytesReceived - c.lastReceived) / 1024.0 / dt : 0.0;
                c.lastSent = stats.bytesSent;
                c.lastReceived = stats.bytesReceived;
                c.started = c.seen = true;
                c.roundTrip.add(stats.roundTripMs, dt);
                c.loss.add(stats.loss * 100.0, dt);
                c.upload.meter.add(kbSent, dt);
                c.download.add(kbReceived, dt);
                total += kbSent;
                index++;
            }
            for (auto& [id, c] : n.clients) c.upload.history[n.at] = c.seen ? float(c.upload.meter.last()) : 0.0f;
            n.upload.meter.add(total, dt);
            n.upload.history[n.at] = float(total);
            n.at = (n.at + 1) % HISTORY;
            std::erase_if(n.clients, [](const auto& entry) { return !entry.second.seen; });
        }

        ImGui::Text("server, %zu clients, tick %llu", session.clients, (unsigned long long)session.serverTick);
        if (n.upload.meter.ready()) {
            ImGui::SameLine();
            ImGui::TextColored(MIN_TEXT, "  sending %.1f KiB/s", n.upload.meter.average());
        }
        std::vector<const Track*> parts;
        for (const auto& [id, c] : n.clients) parts.push_back(&c.upload);
        stackedGraph("##upload", n.upload, parts, n.at, 60.0f, "KiB/s");
        if (ImGui::BeginTable("##clients", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("client", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("rtt ms", ImGuiTableColumnFlags_WidthFixed, 48.0f);
            ImGui::TableSetupColumn("loss %", ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("out KiB/s", ImGuiTableColumnFlags_WidthFixed, 64.0f);
            ImGui::TableSetupColumn("in KiB/s", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupColumn("inputs", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("entities", ImGuiTableColumnFlags_WidthFixed, 52.0f);
            ImGui::TableHeadersRow();
            for (const runtime::ClientStats& stats : clients) {
                auto it = n.clients.find(stats.id);
                if (it == n.clients.end()) continue;
                const PanelState::Network::Client& c = it->second;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                swatch(c.upload.color);
                ImGui::Text("%s", c.upload.name.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("id %u, %zu blocks, %llu snapshot bytes in all", unsigned(stats.id), stats.blocks,
                                      (unsigned long long)stats.snapshotBytes);
                auto cell = [](const Meter& m, const char* format) {
                    ImGui::TableNextColumn();
                    if (m.ready()) ImGui::Text(format, m.average());
                    else ImGui::TextDisabled("-");
                    if (m.ready() && ImGui::IsItemHovered()) ImGui::SetTooltip("min %.2f  max %.2f", m.min(), m.max());
                };
                cell(c.roundTrip, "%.1f");
                cell(c.loss, "%.1f");
                cell(c.upload.meter, "%.1f");
                cell(c.download, "%.1f");
                ImGui::TableNextColumn();
                ImGui::Text("%zu", stats.inputsWaiting);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", stats.entities);
            }
            ImGui::EndTable();
        }
        if (ImGui::BeginTable("##serverCounters", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("largest snapshot to one client, bytes");
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)session.snapshotBytes);
            counterRow("messages refused", session.messagesRefused, "read off the wire and refused (BitReader::failed)");
            ImGui::EndTable();
        }
    }

    // ---- Renderer ----------------------------------------------------------------------------------

    bool rendererPanelHasSubject(graphics::RenderInstance* renderInstance) {
        return renderInstance != nullptr && renderInstance->getActiveRenderer() != nullptr;
    }

    void rendererPanel(Application& app, PanelState& state, graphics::RenderInstance& renderInstance,
                       const DevLayer::Profiler& profiler) {
        std::shared_ptr<ConstructedRenderer> renderer = renderInstance.getActiveRenderer();
        if (!renderer) return;
        const BGFXResources& resources = renderer->resources;

        if (ImGui::CollapsingHeader("Passes", ImGuiTreeNodeFlags_DefaultOpen) &&
            ImGui::BeginTable("##rpasses", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 18.0f);
            ImGui::TableSetupColumn("pass", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("writes", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed, 52.0f);
            ImGui::TableHeadersRow();
            for (const BGFXDependencyGraph& pass : renderer->dependencyGraph) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(MIN_TEXT, "%u", pass.renderPassID);
                ImGui::TableNextColumn();
                if (pass.enabled) ImGui::TextUnformatted(pass.name.c_str());
                else ImGui::TextDisabled("%s (off)", pass.name.c_str());
                ImGui::TableNextColumn();
                if (pass.targetFrameBufferID < 0) ImGui::TextColored(MIN_TEXT, "window");
                else ImGui::TextColored(MIN_TEXT, "fb %d", pass.targetFrameBufferID);
                ImGui::TableNextColumn();
                if (pass.drawnSize.x > 0) ImGui::TextColored(MIN_TEXT, "%dx%d", pass.drawnSize.x, pass.drawnSize.y);
                ImGui::TableNextColumn();
                auto it = profiler.passes.find(uint16_t(pass.renderPassID));
                if (pass.enabled && it != profiler.passes.end() && it->second.name == pass.name && it->second.meter.ready())
                    ImGui::Text("%.2f", it->second.meter.average());
            }
            ImGui::EndTable();
        }

        // Uniforms, read-only; a bound one (editable) carries the program's control under it.
        const Editables* editables = app.world.ctx().find<Editables>();
        std::vector<const Editable*> bound;
        if (editables)
            for (const Editable& e : editables->list)
                if (e.renderer == renderer.get()) bound.push_back(&e);
        if (ImGui::CollapsingHeader("Uniforms", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextColored(MIN_TEXT, "read-only; a program offers values to edit with ui::dev::editable");
            std::vector<std::string> names;
            for (const auto& [name, handle] : resources.uniformHandles) names.push_back(name);
            std::sort(names.begin(), names.end());
            if (ImGui::BeginTable("##uniforms", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                                     ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("uniform", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
                for (const std::string& name : names) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::TableNextColumn();
                    bgfx::UniformInfo info{};
                    bgfx::getUniformInfo(resources.uniformHandles.at(name), info);
                    auto value = resources.uniformValues.find(name);
                    if (value == resources.uniformValues.end() || value->second.size() < sizeof(float)) {
                        ImGui::TextDisabled("unset");
                    } else {
                        const size_t floats = value->second.size() / sizeof(float);
                        std::vector<float> f(floats);
                        std::memcpy(f.data(), value->second.data(), floats * sizeof(float));
                        const size_t row = info.type == bgfx::UniformType::Mat3 ? 3 : 4;
                        const size_t rows = (floats + row - 1) / row;
                        for (size_t r = 0; r < rows && r < 8; r++) {
                            char line[128];
                            int at = 0;
                            for (size_t c = 0; c < row && r * row + c < floats; c++)
                                at += std::snprintf(line + at, sizeof(line) - size_t(at), "%s%.4g", c ? "  " : "", double(f[r * row + c]));
                            ImGui::TextUnformatted(line);
                        }
                        if (rows > 8) ImGui::TextDisabled("... %zu more rows", rows - 8);
                    }
                    for (const Editable* e : bound) {
                        if (e->uniform != name) continue;
                        ImGui::TextColored(PEAK_TEXT, "%s", e->label.c_str());
                        editControl(*e);
                    }
                }
                // Bound to a uniform this renderer does not have: still the program's to edit.
                for (const Editable* e : bound) {
                    if (resources.uniformHandles.count(e->uniform)) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", e->uniform.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(PEAK_TEXT, "%s", e->label.c_str());
                    editControl(*e);
                }
                ImGui::EndTable();
            }
        }

        // The render targets: every texture a framebuffer writes, as a thumbnail.
        if (ImGui::CollapsingHeader("Targets")) {
            std::vector<uint32_t> targets;
            for (const auto& [fb, textures] : resources.framebuffers.frameBufferTextureMapping)
                for (uint32_t id : textures)
                    if (std::find(targets.begin(), targets.end(), id) == targets.end()) targets.push_back(id);
            std::sort(targets.begin(), targets.end());
            const float thumb = 120.0f;
            const float width = ImGui::GetContentRegionAvail().x;
            float x = 0.0f;
            for (uint32_t id : targets) {
                auto handle = resources.textures.textureHandles.find(id);
                if (handle == resources.textures.textureHandles.end() || !bgfx::isValid(handle->second)) continue;
                const core::ivec2 size = resources.textures.textureResolutions.count(id) ? resources.textures.textureResolutions.at(id) : core::ivec2(1, 1);
                const bgfx::TextureFormat::Enum format = resources.textures.textureFormats.count(id) ? resources.textures.textureFormats.at(id) : bgfx::TextureFormat::Unknown;
                const std::string name = resources.textures.textureNames.count(id) && !resources.textures.textureNames.at(id).empty()
                                             ? resources.textures.textureNames.at(id) : "texture " + std::to_string(id);
                if (x > 0.0f && x + thumb > width) x = 0.0f;
                else if (x > 0.0f) ImGui::SameLine();
                ImGui::BeginGroup();
                const float h = thumb * float(std::max(size.y, 1)) / float(std::max(size.x, 1));
                ImGui::PushID(int(id));
                if (viewable(format)) {
                    opaqueImage(handle->second, ImVec2(thumb, std::min(h, thumb)));
                    if (ImGui::IsItemClicked()) state.renderer.enlarged = int(id);
                } else {
                    ImGui::Button("not viewable", ImVec2(thumb, std::min(h, thumb)));
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%dx%d %s%s", name.c_str(), size.x, size.y, formatName(format),
                                      viewable(format) ? "\nclick: full size" : "\nan integer or depth format");
                ImGui::PopID();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumb);
                ImGui::TextColored(MIN_TEXT, "%s", name.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
                x += thumb + ImGui::GetStyle().ItemSpacing.x;
            }
        }

        // The enlarged target, a window of its own.
        if (state.renderer.enlarged >= 0) {
            const uint32_t id = uint32_t(state.renderer.enlarged);
            auto handle = resources.textures.textureHandles.find(id);
            if (handle == resources.textures.textureHandles.end() || !bgfx::isValid(handle->second)) {
                state.renderer.enlarged = -1;
                return;
            }
            const core::ivec2 size = resources.textures.textureResolutions.count(id) ? resources.textures.textureResolutions.at(id) : core::ivec2(1, 1);
            const std::string name = resources.textures.textureNames.count(id) && !resources.textures.textureNames.at(id).empty()
                                         ? resources.textures.textureNames.at(id) : "texture " + std::to_string(id);
            bool open = true;
            ImGui::SetNextWindowSize(ImVec2(std::min(float(size.x), 900.0f) + 16.0f, std::min(float(size.y), 700.0f) + 40.0f), ImGuiCond_Appearing);
            if (ImGui::Begin(("Target: " + name + "###target").c_str(), &open)) {
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                const float scale = std::min(avail.x / float(std::max(size.x, 1)), avail.y / float(std::max(size.y, 1)));
                opaqueImage(handle->second, ImVec2(float(size.x) * scale, float(size.y) * scale));
            }
            ImGui::End();
            if (!open) state.renderer.enlarged = -1;
        }
    }

    // ---- Entities ----------------------------------------------------------------------------------

    void entitiesPanel(Application& app, PanelState& state) {
        PanelState::Entities& s = state.entities;
        World& world = app.world;

        // The component types in this world, by name, and which storage each is.
        struct Type { std::string name; const entt::sparse_set* storage; };
        std::vector<Type> types;
        for (auto [id, storage] : world.storage()) {
            if (storage.info() == entt::type_id<Entity>()) continue;
            types.push_back({std::string(storage.info().name()), &storage});
        }
        std::sort(types.begin(), types.end(), [](const Type& a, const Type& b) { return a.name < b.name; });

        ImGui::SetNextItemWidth(180.0f);
        s.filter.Draw("filter##entities");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("by name, id or component type (\"Body\", \"-Debris\")");

        // The list: every entity that passes the filter, by its authored name else its id.
        std::vector<Entity> shown;
        size_t alive = 0;
        for (auto [entity] : world.storage<Entity>().each()) {
            alive++;
            if (s.filter.IsActive()) {
                std::string text = std::to_string(uint32_t(entt::to_integral(entity)));
                if (const runtime::Authored* authored = world.try_get<runtime::Authored>(entity)) text += " " + authored->name;
                for (const Type& t : types)
                    if (t.storage->contains(entity)) text += " " + t.name;
                if (!s.filter.PassFilter(text.c_str())) continue;
            }
            shown.push_back(entity);
        }
        // By index: the raw value carries the version, which would put a reused index after newer ones.
        std::sort(shown.begin(), shown.end(), [](Entity a, Entity b) { return entt::to_entity(a) < entt::to_entity(b); });
        if (s.selected != entt::null && !world.valid(s.selected)) s.selected = entt::null;
        ImGui::SameLine();
        ImGui::TextColored(MIN_TEXT, "%zu of %zu entities, %zu component types", shown.size(), alive, types.size());

        const float listWidth = 190.0f;
        ImGui::BeginChild("##entityList", ImVec2(listWidth, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        ImGuiListClipper clipper;
        clipper.Begin(int(shown.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const Entity entity = shown[size_t(i)];
                const runtime::Authored* authored = world.try_get<runtime::Authored>(entity);
                char label[160];
                std::snprintf(label, sizeof(label), "%u  %s##%u", uint32_t(entt::to_entity(entity)),
                              authored && !authored->name.empty() ? authored->name.c_str() : "",
                              uint32_t(entt::to_integral(entity)));
                if (ImGui::Selectable(label, entity == s.selected)) s.selected = entity;
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("##entityDetail", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
        if (s.selected == entt::null) {
            ImGui::TextDisabled("select an entity");
        } else {
            const Entity entity = s.selected;
            const runtime::Authored* authored = world.try_get<runtime::Authored>(entity);
            ImGui::Text("entity %u (version %u)", uint32_t(entt::to_entity(entity)), uint32_t(entt::to_version(entity)));
            if (authored) ImGui::TextColored(MIN_TEXT, "authored \"%s\"", authored->name.c_str());
            ImGui::SeparatorText("components");
            for (const Type& t : types)
                if (t.storage->contains(entity)) ImGui::BulletText("%s", t.name.c_str());
            const std::vector<std::pair<std::string, nlohmann::json>> saved = runtime::registeredComponents(world, entity);
            if (!saved.empty()) {
                ImGui::SeparatorText("as entities.json writes them");
                for (const auto& [key, json] : saved) {
                    if (ImGui::TreeNodeEx(key.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        const std::string text = json.dump(2);
                        ImGui::PushStyleColor(ImGuiCol_Text, MIN_TEXT);
                        ImGui::TextUnformatted(text.c_str());
                        ImGui::PopStyleColor();
                        ImGui::TreePop();
                    }
                }
            }
        }
        ImGui::EndChild();
    }
}
