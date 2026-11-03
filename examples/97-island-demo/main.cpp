// ProjectV Island demo (docs/plans/island-demo.md)
//
// A generated island to break, gather and build with: hills, rock and trees made from a seed, walked in
// first person, drawn with the renderer gallery's restir-pt; grass and flowers, butterflies and falling
// leaves; tools to break it, rubble to gather, blocks to build with; worlds saved and loaded.
//
//   ./island_demo                  the start screen: a new world, or a saved one (worlds/ beside it)
//   ./island_demo --seed 42        straight into worlds/island-42/: made the first time, loaded after
//   ./island_demo --world <folder> straight into worlds/<folder>/ (the start screen starts a world so)
//   ./island_demo --graphics performance   restir-pt's preset for this run (else settings.json's, else timed:
//                                  the Esc menu switches it and keeps the choice)
//   ISLAND_CAPTURE=/tmp/i ./island_demo --seed 42   240 frames into play saved as /tmp/i.tga, then it exits (no save)
//                                  (ISLAND_VIEW=overview: from high above the shore)
//                                  (ISLAND_MENU=pause|graphics: that menu open)
//
// Controls: W/A/S/D walk, Space jump, the mouse looks; Esc the menu (resume, save and quit);
// 1/2 or the wheel pick the pickaxe or the chisel, the left button swings it (held: again); E turns
// collecting on or off (on: rubble near you and where you look comes in as it lands); F1 the dev layer.
//
//   ISLAND_SELFTEST=1 ./island_demo   checks itself and exits 0 or 1 (D10; what it checks grows per milestone)

#include <algorithm>
#include <cctype>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <bgfx/bgfx.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "graphics/gpu_interface.h"
#include "graphics/input.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "restir_pt.h"
#include "runtime/physics.h"
#include "runtime/scene_bridge.h"
#include "ui/ui.h"
#include "ui/user.h"
#include "utils/compose_io.h"

#include "build.h"
#include "critters.h"
#include "fx.h"
#include "icons.h"
#include "gather.h"
#include "island.h"
#include "saving.h"
#include "tools.h"

namespace fs = std::filesystem;
using projv::Entity;
using projv::World;
using projv::core::vec3;
namespace runtime = projv::runtime;

namespace {
    constexpr float EYE = 1.62f;          // eye height above the feet, m
    constexpr float STEP_EASE = 14.0f;    // 1/s: how fast the eye's height catches up with a step on the ground
    constexpr float STEP_LAG = 0.35f;     // m: the most it may trail behind
    constexpr float WALK_SPEED = 4.5f;    // m/s
    constexpr float WALK_GRIP = 12.0f, AIR_GRIP = 2.0f;

    struct Options {
        uint32_t seed = 0;
        bool     seeded = false;
        std::string world;     // --world <folder>: straight into worlds/<folder>/ (how the start screen starts one)
        std::string graphics;  // --graphics quality|performance: this run's preset (not saved)
    };
    Options options;
    int exitCode = 0;   // what main returns: the self-test's verdict
    // A world chosen on the start screen: main starts the program again, straight into it, once this one has
    // shut down. A world is never swapped into a running program: the GPU's copy of the scene would have to
    // be made again under a renderer that holds the old one, and the driver falls over (it did, twice).
    std::string relaunchInto;

    struct Player {
        Entity walker = projv::NullEntity;
        float  yaw = 0.0f, pitch = -0.1f;
        vec3   velocity{0.0f};
        vec3   eye{0.0f};
        bool   eased = false;   // the eye has a height to ease from (not on the first frame)
        vec3 forward() const { return vec3(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)); }
    };

    struct Island {
        uint32_t seed = 0;
        fs::path folder;
        projv::ComponentHandle component = projv::INVALID_COMPONENT_HANDLE;
        projv::ComponentHandle plants = projv::INVALID_COMPONENT_HANDLE;   // drawn, no collider (D12)
        Entity entity = projv::NullEntity;
    };

    // Which world, and whether it is being played yet (D11). Before a world is chosen the start screen is up
    // over the sky; choosing one starts the program again in it (relaunchInto), so a program plays one world
    // and the menu's way out is Save and quit.
    struct Session {
        bool playing = false;
        bool saves = true;             // off for captures: they leave their world as it was
        fs::path worlds;               // worlds/ beside the program
        fs::path folder;               // the world being played
        saving::WorldInfo info;
        std::string pending;           // a world chosen on the start screen, to load next frame
        bool pendingNew = false;
        double sessionStart = 0.0;     // app time the world was entered (for the time played)
        saving::Saver saver;
        uint64_t editSignature = 0;    // what the voxels' edit count was at the last frame
        uint64_t playFrame = 0;        // the frame play began: captures and timed tests count from it
    };

    // Entering a world (D15): made if it is new and read from disk on a worker thread, the window drawing the
    // loading screen meanwhile; then taken in on the main thread.
    struct Loader {
        enum Stage { Idle, Making, Reading, Ready, Failed };
        std::thread thread;
        std::atomic<int> stage{Idle};
        std::atomic<int> cells{0};     // made so far, of island::CELLS * LAYERS * CELLS
        projv::Scene scene;            // what was read: the world's once Ready
        double seconds = 0.0;          // the making's
        int readyFrames = 0;           // frames "Preparing" has been on screen
        std::chrono::steady_clock::time_point began;
    };
    // The loading screen's model ("loading").
    struct LoadingScreen {
        std::string title = "Loading";
        std::string world;
        std::string fill = "0dp";      // the bar's width, of LOADING_BAR
        std::string detail;
    };
    constexpr int LOADING_BAR = 142;

    // The start screen's model ("start"): the saved worlds, a new world's name and seed.
    struct StartScreen {
        std::vector<std::vector<std::string>> worlds;   // folder, name, "seed n", last played, time played
        bool hasWorlds = false;
        std::string newName = "My island";
        std::string newSeed;           // empty: a random one
        std::string status;
    };
    // The in-game menu's model ("pause").
    struct PauseMenu {
        bool open = false;
        std::string world;             // the world's name, as a title
        std::string saved;             // "Saved 2 min ago"
        std::string graphics = "Graphics";   // the button to the graphics panel
    };

    // restir-pt's settings (docs/plans/restir-pt-performance.md, D6, D9), kept in settings.json beside the
    // program: a preset, or custom ones. With none there, the first play is timed: frames over 25 ms on
    // average pick performance, and that is kept. The Esc menu's Graphics panel changes them (and keeps the
    // change); --graphics sets a preset for one run.
    struct Graphics {
        restir_pt::Preset preset = restir_pt::Preset::Quality;
        bool  custom = false;          // the settings were changed one by one: the preset is only where they began
        std::vector<std::vector<std::string>> rows;   // the panel's (model "graphics"): label, value, key, class
        std::string note;
        bool  open = false;            // the panel is shown (over the paused game)
        bool  chosen = false;          // false: timed on the first play
        bool  keep = true;             // false under --graphics, captures and the self-test: nothing written
        float timed = 0.0f;            // seconds of play timed so far
        float total = 0.0f;            // and the frames' time in it, after the settling
        int   frames = 0;
    };
    constexpr float GRAPHICS_SETTLE = 1.0f, GRAPHICS_TIMED = 3.0f, GRAPHICS_SLOW = 0.025f;

    // What the HUD shows (ui/hud.rml, D9), model "island".
    struct Hud {
        std::vector<std::vector<std::string>> slots;     // the hotbar: icon, count, class ("sel", "none")
        std::vector<std::vector<std::string>> compass;   // the marks in view: label, left ("37dp"), class
        std::string heading;     // "042"
        std::string held;        // what is in hand, shown a moment after it is picked
        bool heldShown = false;
        float heldFor = 0.0f;    // seconds it has been in hand
        int lastSlot = -1;
        bool aiming = false;
        std::string prompt;      // "collect 3.4 stone, 1.2 dirt" (the HUD puts the key before it)
        bool collecting = false;
    };

    // ISLAND_SELFTEST=1: the program checks itself (D10), growing with each milestone. I1: a pickaxe
    // strike breaks the ground and clears the plants over it, a chisel strike breaks exactly one voxel,
    // and strikes at a trunk bring its tree down.
    struct SelfTest {
        bool  on = false;
        int   step = 0, wait = 0, tries = 0;
        int   shardsBefore = 0, breaksBefore = 0;
        vec3  struck{0.0f};      // where the pickaxe hit the ground
        std::string material;    // what the blocks are built of
        double before = 0.0;     // the inventory of it before
        std::vector<projv::core::ivec3> blocks;   // the cells built in
        bool  failed = false;
        std::string report;
    };

    // restir-pt, as Voxel Mayhem draws it (examples/shared/restir_pt.h): the camera, the sun, the scene's scale.
    struct Restir {
        restir_pt::Settings   settings;
        restir_pt::SceneScale scene;
        float sun = 0.95f;   // the sun's angle up its arc
        int   frame = 0;
        bool  has = false;
        vec3  prevPosition{0.0f}, prevDirection{0.0f, 0.0f, 1.0f};
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

        void draw(World& world, projv::graphics::RenderInstance& renderInstance, vec3 position, vec3 direction) {
            if (!has) {
                prevPosition = position;
                prevDirection = direction;
            }
            restir_pt::Frame f;
            f.position = position;
            f.direction = direction;
            f.sunDirection = glm::normalize(vec3(std::cos(sun) * -0.6402f, std::sin(sun), std::cos(sun) * 0.7682f));
            f.moved = glm::length(position - prevPosition) > 1e-5f || glm::length(direction - prevDirection) > 1e-6f;
            f.first = !has;
            f.count = frame;
            f.window = renderInstance.getWindowResolution();
            const auto now = std::chrono::steady_clock::now();
            f.seconds = std::chrono::duration<float>(now - last).count();
            last = now;
            restir_pt::upload(renderInstance.getActiveRenderer(), f, scene, settings, &world);
            prevPosition = position;
            prevDirection = direction;
            has = true;
            frame++;
        }
    };

    // ---- The graphics settings ----------------------------------------------------------------------

    fs::path settingsPath() { return projv::core::executableDirectory() / "settings.json"; }

    // The settings the panel offers, as settings.json keeps them.
    nlohmann::json settingsJson(const restir_pt::Settings& s) {
        nlohmann::json j;
        j["resolution"] = s.renderScale;
        j["lighting"] = s.simpleLighting ? 0 : int(s.bounces);
        j["screenReflections"] = s.screenReflections;
        j["softShadows"] = s.softShadow;
        j["ambientOcclusion"] = s.ssao > 0.0f;
        j["bloom"] = s.post.bloom;
        j["lightShafts"] = s.shafts;
        j["depthOfField"] = s.post.dof;
        j["motionBlur"] = s.post.motionBlur;
        return j;
    }
    void settingsFromJson(const nlohmann::json& j, restir_pt::Settings& s) {
        if (j.contains("resolution") && j["resolution"].is_number()) s.renderScale = std::clamp(j["resolution"].get<float>(), 0.25f, 1.0f);
        if (j.contains("lighting") && j["lighting"].is_number_integer()) {
            const int l = std::clamp(j["lighting"].get<int>(), 0, 2);
            s.simpleLighting = l == 0;
            if (l > 0) s.bounces = float(l);
        }
        auto flag = [&](const char* key, bool& value) { if (j.contains(key) && j[key].is_boolean()) value = j[key].get<bool>(); };
        flag("screenReflections", s.screenReflections);
        flag("softShadows", s.softShadow);
        bool ao = s.ssao > 0.0f;
        flag("ambientOcclusion", ao);
        s.ssao = ao ? restir_pt::SSAO : 0.0f;
        flag("bloom", s.post.bloom);
        flag("lightShafts", s.shafts);
        flag("depthOfField", s.post.dof);
        flag("motionBlur", s.post.motionBlur);
    }

    void readGraphics(Graphics& g, restir_pt::Settings& s) {
        std::ifstream in(settingsPath());
        if (!in) return;
        const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("graphics") || !j["graphics"].is_string()) return;
        const std::string name = j["graphics"].get<std::string>();
        if (name != "quality" && name != "performance" && name != "custom") return;
        g.preset = name == "performance" ? restir_pt::Preset::Performance : restir_pt::Preset::Quality;
        if (j.contains("base") && j["base"] == "performance") g.preset = restir_pt::Preset::Performance;
        s = restir_pt::settingsFor(g.preset);
        if (name == "custom" && j.contains("settings") && j["settings"].is_object()) {
            settingsFromJson(j["settings"], s);
            g.custom = true;
        }
        g.chosen = true;
    }

    std::string capitalised(std::string text) {
        if (!text.empty()) text[0] = char(std::toupper(text[0]));
        return text;
    }

    // The panel's rows, from the settings drawn now.
    void graphicsRows(World& world) {
        Graphics& g = world.ctx().get<Graphics>();
        const restir_pt::Settings& s = world.ctx().get<Restir>().settings;
        auto onOff = [](bool on) { return std::string(on ? "On" : "Off"); };
        char resolution[16];
        std::snprintf(resolution, sizeof resolution, "%d%%", int(std::lround(s.renderScale * 100.0f)));
        g.rows = {
            {"Preset", g.custom ? "Custom" : capitalised(restir_pt::presetName(g.preset)), "preset", "preset"},
            {"Resolution", resolution, "resolution", ""},
            {"Lighting", s.simpleLighting ? "Simple" : s.bounces < 1.5f ? "Traced, 1 bounce" : "Traced, 2 bounces", "lighting", ""},
            {"Reflections", s.screenReflections ? "Screen space" : "Traced", "reflections", ""},
            {"Shadows", s.softShadow ? "Soft" : "Hard", "shadows", ""},
            {"Ambient occlusion", onOff(s.ssao > 0.0f), "ao", ""},
            {"Bloom", onOff(s.post.bloom), "bloom", ""},
            {"Light shafts", onOff(s.shafts), "shafts", ""},
            {"Depth of field", onOff(s.post.dof), "dof", ""},
            {"Motion blur", onOff(s.post.motionBlur), "mblur", ""},
        };
        g.note = s.simpleLighting ? "Simple lighting: clean and fast, no bounced light" : "Traced lighting: richer, noisier, slower";
    }

    // The settings drawn from now on (every history starts over), the panel's rows, and settings.json when
    // it may be written.
    void applyGraphics(World& world) {
        Graphics& g = world.ctx().get<Graphics>();
        g.chosen = true;
        Restir& restir = world.ctx().get<Restir>();
        restir.has = false;
        world.ctx().get<PauseMenu>().graphics = "Graphics";   // the panel says which
        graphicsRows(world);
        if (g.keep) {
            nlohmann::json j;
            j["graphics"] = g.custom ? "custom" : restir_pt::presetName(g.preset);
            if (g.custom) {
                j["base"] = restir_pt::presetName(g.preset);
                j["settings"] = settingsJson(restir.settings);
            }
            std::ofstream(settingsPath()) << j.dump(2) << "\n";
        }
    }

    void setGraphics(World& world, restir_pt::Preset preset) {
        Graphics& g = world.ctx().get<Graphics>();
        g.preset = preset;
        g.custom = false;
        Restir& restir = world.ctx().get<Restir>();
        const int view = restir.settings.view;
        restir.settings = restir_pt::settingsFor(preset);
        restir.settings.view = view;
        applyGraphics(world);
    }

    // A click on a row of the panel: its setting's next value (the preset's: the other preset).
    void cycleGraphics(World& world, const std::string& key) {
        Graphics& g = world.ctx().get<Graphics>();
        restir_pt::Settings& s = world.ctx().get<Restir>().settings;
        if (key == "preset") {
            setGraphics(world, g.custom || g.preset == restir_pt::Preset::Performance ? restir_pt::Preset::Quality : restir_pt::Preset::Performance);
            return;
        }
        if (key == "resolution") s.renderScale = s.renderScale < 0.6f ? 0.75f : s.renderScale < 0.9f ? 1.0f : 0.5f;
        else if (key == "lighting") {
            if (s.simpleLighting) { s.simpleLighting = false; s.bounces = 1.0f; }
            else if (s.bounces < 1.5f) s.bounces = 2.0f;
            else s.simpleLighting = true;
        }
        else if (key == "reflections") s.screenReflections = !s.screenReflections;
        else if (key == "shadows") s.softShadow = !s.softShadow;
        else if (key == "ao") s.ssao = s.ssao > 0.0f ? 0.0f : restir_pt::SSAO;
        else if (key == "bloom") s.post.bloom = !s.post.bloom;
        else if (key == "shafts") s.shafts = !s.shafts;
        else if (key == "dof") s.post.dof = !s.post.dof;
        else if (key == "mblur") s.post.motionBlur = !s.post.motionBlur;
        else return;
        g.custom = true;
        applyGraphics(world);
    }

    // While no preset is chosen: the first seconds of play timed, after a moment to settle.
    void timeGraphics(World& world, float dt) {
        Graphics& g = world.ctx().get<Graphics>();
        if (g.chosen) return;
        g.timed += dt;
        if (g.timed <= GRAPHICS_SETTLE) return;
        g.total += dt;
        g.frames++;
        if (g.timed < GRAPHICS_SETTLE + GRAPHICS_TIMED) return;
        const float average = g.total / float(std::max(g.frames, 1));
        const restir_pt::Preset preset = average > GRAPHICS_SLOW ? restir_pt::Preset::Performance : restir_pt::Preset::Quality;
        projv::core::info("island: frames took {:.1f} ms on average: the {} preset from now on", average * 1000.0f, restir_pt::presetName(preset));
        setGraphics(world, preset);
    }

    // ---- The island ------------------------------------------------------------------------------

    // Loads the world in `folder` (made from `seed` first if it has none yet): a new world and a saved one
    // come in the same way (D11).
    // The island in `scene` (read from `folder` by the loader, below) becomes the world's.
    bool takeIsland(projv::Application& app, const fs::path& folder, uint32_t seed, projv::Scene&& scene) {
        World& world = app.world;
        Island& island = world.ctx().emplace<Island>();
        island.seed = seed;
        island.folder = folder;
        world.ctx().get<projv::Scene>() = std::move(scene);
        const projv::Scene& taken = world.ctx().get<projv::Scene>();
        for (size_t i = 0; i < taken.components.size(); i++)
            if (taken.components[i].name == "island") island.component = projv::ComponentHandle(i);
            else if (taken.components[i].name == "plants") island.plants = projv::ComponentHandle(i);
        if (island.component == projv::INVALID_COMPONENT_HANDLE) {
            projv::core::error("island: '{}' has no island in it", island.folder.string());
            return false;
        }
        return true;
    }

    void installWorld(projv::Application& app, const fs::path& here) {
        World& world = app.world;
        runtime::installSceneBridge(app);
        runtime::PhysicsConfig physics;
        physics.settings.worldMin = vec3(-60.0f, -30.0f, -60.0f);
        physics.settings.worldMax = vec3(island::SIZE + 60.0f, 160.0f, island::SIZE + 60.0f);
        // The island collides as its voxels' tile tables, read in place: nothing built at load.
        physics.collision.representation = projv::utils::CollisionRepresentation::Tiles;
        // A break's small pieces are cheap debris: drawn, falling, gathered (D7), never in the solver.
        physics.debris.maxVoxels = 512;   // 64 voxels of 0.25 m: the same size of piece
        physics.debris.seconds = 60.0f;
        physics.maxPieces = 1500;
        physics.fracture.minImpactBreak = 2.0f;
        runtime::installPhysics(app, physics);
        runtime::installImpactFracture(app);
        if (!runtime::loadPhysicsMaterials(world, (here / "physics_materials.json").string()))
            projv::core::error("island: physics_materials.json did not load; nothing will break");
        runtime::definePhysicsGroup(world, "player");

        // The island: static, and what a break or a dig leaves standing on nothing falls -- held up by its
        // bottom, the sea floor.
        Island& island = world.ctx().get<Island>();
        island.entity = runtime::spawnComponent(world, island.component);
        projv::StaticCollider collider;
        collider.splits = true;
        collider.anchorBase = true;
        world.emplace<projv::StaticCollider>(island.entity, collider);
    }

    void spawnPlayer(World& world, const saving::WorldInfo& info) {
        Player& player = world.ctx().emplace<Player>();
        const Island& island = world.ctx().get<Island>();
        const vec3 at = info.hasPlayer ? info.feet + vec3(0.0f, 0.05f, 0.0f) : island::spawnPoint(island.seed);
        player.walker = world.create();
        world.emplace<projv::Transform>(player.walker, projv::Transform{at});
        projv::CharacterController character;
        character.strength = 600.0f;
        world.emplace<projv::CharacterController>(player.walker, character);
        world.emplace<projv::CollisionGroups>(player.walker, projv::CollisionGroups{{"player"}, {}, 0});
        // A saved world: as it was left. A new one: facing the most open view, the direction the ground stays
        // under eye height the furthest.
        float best = info.hasPlayer ? 1e9f : -1.0f;
        if (info.hasPlayer) {
            player.yaw = info.yaw;
            player.pitch = info.pitch;
        }
        for (int i = 0; i < 24 && !info.hasPlayer; i++) {
            const float yaw = float(i) / 24.0f * 6.2831853f;
            float clear = 0.0f;
            for (float r = 2.0f; r <= 40.0f; r += 2.0f) {
                const float x = at.x + std::cos(yaw) * r, z = at.z + std::sin(yaw) * r;
                if (island::surfaceAt(x, z, island.seed) > at.y + EYE + r * 0.1f) break;
                clear = r;
            }
            if (clear > best) {
                best = clear;
                player.yaw = yaw;
            }
        }
        player.eye = at + vec3(0.0f, EYE, 0.0f);
        if (const char* pitch = std::getenv("ISLAND_PITCH")) player.pitch = float(std::atof(pitch));   // for captures
        const vec3 look = player.eye + player.forward() * 10.0f;   // as the gallery's --look takes it
        projv::core::info("island: the player stands at {:.2f},{:.2f},{:.2f} looking at {:.2f},{:.2f},{:.2f}", player.eye.x, player.eye.y,
                          player.eye.z, look.x, look.y, look.z);
    }

    // ---- Each frame --------------------------------------------------------------------------------

    bool playing(const World& world) { return world.ctx().get<Session>().playing; }
    void saveWorld(projv::Application& app, bool wait);   // below

    // The graphics panel, in place of the menu while it is open.
    void showGraphics(projv::Application& app, bool open) {
        Graphics& g = app.world.ctx().get<Graphics>();
        if (g.open == open) return;
        g.open = open;
        if (open) {
            graphicsRows(app.world);
            projv::ui::user::hide(app, "pause");
            projv::ui::user::show(app, "graphics");
        } else {
            projv::ui::user::hide(app, "graphics");
            if (app.world.ctx().get<PauseMenu>().open) projv::ui::user::show(app, "pause");
        }
    }

    void setPause(projv::Application& app, bool open) {
        World& world = app.world;
        PauseMenu& menu = world.ctx().get<PauseMenu>();
        menu.open = open;
        if (open) {
            const Session& session = world.ctx().get<Session>();
            menu.world = session.info.name;
            const float ago = session.saver.sinceSave;
            menu.saved = session.saver.saves == 0 ? "Not saved yet this session"
                                                  : "Saved " + (ago < 60.0f ? std::string("just now") : std::to_string(int(ago / 60.0f)) + " min ago");
            projv::ui::user::show(app, "pause");
        } else {
            projv::ui::user::hide(app, "pause");
            showGraphics(app, false);
        }
        projv::graphics::setCursorCaptured(app, world.ctx().get<projv::graphics::RenderInstance>(), !open);
    }

    void controls(projv::Application& app) {
        World& world = app.world;
        if (!playing(world)) return;
        auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
        const auto& input = world.ctx().get<projv::Input>();
        Player& player = world.ctx().get<Player>();
        const float dt = app.time().unscaledDelta;

        if (input.pressed(projv::Key::Escape)) {
            if (world.ctx().get<Graphics>().open) showGraphics(app, false);   // back to the menu
            else setPause(app, !world.ctx().get<PauseMenu>().open);
        }
        else if (!input.cursorCaptured && !world.ctx().get<PauseMenu>().open && !input.wantCaptureMouse && input.pressed(projv::MouseButton::Left))
            projv::graphics::setCursorCaptured(app, renderInstance, true);
        if (input.cursorCaptured) {
            player.yaw += input.cursorDelta.x * 0.0025f;
            player.pitch = std::clamp(player.pitch - input.cursorDelta.y * 0.0025f, -1.5f, 1.5f);
        }

        const vec3 flat(std::cos(player.yaw), 0.0f, std::sin(player.yaw)), right(-std::sin(player.yaw), 0.0f, std::cos(player.yaw));
        vec3 walk(0.0f);
        if (!input.wantCaptureKeyboard) {
            if (input.down(projv::Key::W)) walk += flat;
            if (input.down(projv::Key::S)) walk -= flat;
            if (input.down(projv::Key::D)) walk += right;
            if (input.down(projv::Key::A)) walk -= right;
        }
        if (glm::length(walk) > 0.0f) walk = glm::normalize(walk) * WALK_SPEED;
        const bool grounded = runtime::characterState(world, player.walker).onGround;
        player.velocity += (walk - player.velocity) * (1.0f - std::exp(-(grounded ? WALK_GRIP : AIR_GRIP) * dt));
        runtime::setCharacterInput(world, player.walker, player.velocity, input.pressed(projv::Key::Space), 5.5f);
    }

    // The HUD's model (ui/hud.rml): the hotbar, the compass, what is in hand, the prompt.
    void updateHud(World& world, float dt) {
        Hud& hud = world.ctx().get<Hud>();
        const tools::Tools& t = world.ctx().get<tools::Tools>();
        const gather::Inventory& inventory = world.ctx().get<gather::Inventory>();
        const gather::Gather& g = world.ctx().get<gather::Gather>();

        // The hotbar: the pickaxe, the chisel, then the materials, each with how many blocks of it you have.
        hud.slots.resize(int(tools::Tool::Count) + std::size(gather::MATERIALS));
        for (int i = 0; i < int(hud.slots.size()); i++) {
            const char* material = build::materialOf(i);
            const std::string name = material ? material : (i == 0 ? "pickaxe" : "chisel");
            const double blocks = material ? inventory.blocks(material) : 1.0;
            hud.slots[i] = {"../icons/" + name + ".pixel.tga", material && blocks > 0.0 ? gather::blocksText(blocks * gather::VOXELS_PER_BLOCK) : "",
                            std::string(i == t.slot ? "sel " : "") + (blocks < 1.0 ? "none" : "")};
        }
        if (t.slot != hud.lastSlot) {
            hud.lastSlot = t.slot;
            hud.heldFor = 0.0f;
        }
        hud.heldFor += dt;
        if (const char* material = build::materialOf(t.slot)) {
            std::string name = material;
            name[0] = char(std::toupper(name[0]));
            hud.held = name + " block" + (inventory.blocks(material) < 1.0 ? " - none yet" : "");
        } else {
            hud.held = tools::toolName(t.selected);
        }
        hud.heldShown = hud.heldFor < 1.6f;

        // The compass: north is -z, a degree a dp, 60 either side of the heading (the strip is 121dp inside).
        const Player& player = world.ctx().get<Player>();
        float heading = glm::degrees(std::atan2(std::cos(player.yaw), -std::sin(player.yaw)));
        heading = std::fmod(heading + 360.0f, 360.0f);
        char text[8];
        std::snprintf(text, sizeof text, "%03d", int(std::lround(heading)) % 360);
        hud.heading = text;
        hud.compass.clear();
        static const char* const NAMES[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        for (int a = int(std::ceil((heading - 60.0f) / 15.0f)) * 15; float(a) <= heading + 60.0f; a += 15) {
            const int at = ((a % 360) + 360) % 360, off = int(std::lround(float(a) - heading));
            const std::string left = std::to_string(off + 60) + "dp";
            if (at % 45 != 0) hud.compass.push_back({"", left, "tick"});
            else if (std::abs(off) <= 52) hud.compass.push_back({NAMES[at / 45], left, at == 0 ? "card north" : at % 90 == 0 ? "card" : "inter"});   // whole, inside the strip
        }
        // Always as many rows, the spare ones hidden: a list that changes length as you turn has the
        // document read a row that is gone for a frame.
        while (hud.compass.size() < 9) hud.compass.push_back({"", "0dp", "off"});

        hud.aiming = t.aiming;
        hud.prompt = g.prompt.rfind("E: ", 0) == 0 ? g.prompt.substr(3) : g.prompt;
        hud.collecting = g.collect;
    }

    // The tools (tools.h): the left button uses the one in hand while the cursor is the game's.
    void useTools(projv::Application& app) {
        World& world = app.world;
        if (!playing(world)) return;
        const auto& input = world.ctx().get<projv::Input>();
        Player& player = world.ctx().get<Player>();
        bool use = input.cursorCaptured && !input.wantCaptureMouse && input.down(projv::MouseButton::Left) &&
                   !world.ctx().get<SelfTest>().on;
        // ISLAND_SWING_FRAME=n: one swing at frame n (for a capture of the swing and its particles).
        if (const char* at = std::getenv("ISLAND_SWING_FRAME"); at && app.frameCount - world.ctx().get<Session>().playFrame == uint64_t(std::atoi(at))) use = true;
        tools::update(world, input, app.time().unscaledDelta, player.eye, player.forward(), use);
        build::update(world, app.time().unscaledDelta, player.eye, player.forward(), use);
        gather::update(world, input, app.time().unscaledDelta, player.eye, player.forward(), input.cursorCaptured && !world.ctx().get<SelfTest>().on);
        updateHud(world, app.time().unscaledDelta);
    }

    // ---- The self-test ---------------------------------------------------------------------------------

    void check(SelfTest& st, bool ok, const std::string& what) {
        st.report += std::string(ok ? "  ok    " : "  FAIL  ") + what + "\n";
        if (!ok) st.failed = true;
    }

    // Strikes with `tool` from `eye` toward `target`; false if nothing is in reach that way.
    bool selfStrike(World& world, tools::Tool tool, vec3 eye, vec3 target, vec3* where = nullptr) {
        tools::Tools& t = world.ctx().get<tools::Tools>();
        const vec3 forward = glm::normalize(target - eye);
        const auto a = tools::aim(world, eye, forward, t.walker);
        if (!a) return false;
        tools::strike(world, t, tool, *a, forward);
        if (where) *where = a->point;
        return true;
    }

    void selfTest(projv::Application& app) {
        World& world = app.world;
        if (!playing(world)) return;
        SelfTest& st = world.ctx().get<SelfTest>();
        if (!st.on) return;
        tools::Tools& t = world.ctx().get<tools::Tools>();
        const Island& island = world.ctx().get<Island>();
        const Player& player = world.ctx().get<Player>();
        if (st.wait > 0) { st.wait--; return; }
        const vec3 spawn = player.eye;
        switch (st.step) {
            case 0:   // let the world settle
                st.wait = 90;
                st.step++;
                break;
            case 1: {   // the pickaxe, at the ground two metres ahead
                st.breaksBefore = t.breaks;
                st.shardsBefore = t.shards;
                const vec3 ahead = spawn + glm::normalize(vec3(player.forward().x, 0.0f, player.forward().z)) * 2.0f;
                check(st, selfStrike(world, tools::Tool::Pickaxe, spawn, ahead - vec3(0.0f, 3.0f, 0.0f), &st.struck), "the pickaxe reaches the ground ahead");
                st.wait = 30;
                st.step++;
                break;
            }
            case 2: {
                check(st, t.breaks > st.breaksBefore, "the pickaxe broke the ground");
                check(st, t.shards - st.shardsBefore > 8, "into many shards (" + std::to_string(t.shards - st.shardsBefore) + ")");
                check(st, t.rubbleKept > 0 && t.rubbleKept < t.shards - st.shardsBefore,
                      "of which a share is kept as rubble (" + std::to_string(t.rubbleKept) + ")");
                check(st, t.plantsCleared > 0, "and cleared the plants over it (" + std::to_string(t.plantsCleared) + " voxels)");
                // The chisel, at the ground to the side.
                st.breaksBefore = t.breaks;
                st.shardsBefore = t.shards;
                const vec3 f = player.forward();
                const vec3 side = spawn + glm::normalize(vec3(-f.z, 0.0f, f.x)) * 1.5f;
                check(st, selfStrike(world, tools::Tool::Chisel, spawn, side - vec3(0.0f, 3.0f, 0.0f)), "the chisel reaches the ground beside");
                st.wait = 30;
                st.step++;
                break;
            }
            case 3: {
                check(st, t.breaks == st.breaksBefore + 1 && t.shards - st.shardsBefore == 1,
                      "the chisel broke exactly one voxel (" + std::to_string(t.shards - st.shardsBefore) + " shard)");
                st.wait = 90;   // the rubble settles
                st.step = 20;
                break;
            }
            case 20: {   // gather the pickaxe's rubble, as E would
                gather::Gather& g = world.ctx().get<gather::Gather>();
                gather::Inventory& inventory = world.ctx().get<gather::Inventory>();
                g.near.clear();
                g.offer.clear();
                gather::lookAround(world, g, st.struck, t.island, t.walker);
                const size_t pieces = g.near.size();
                gather::take(world, g, inventory);
                const double ratio = t.brokenVoxels > 0.0 ? g.gathered / t.brokenVoxels : 0.0;
                check(st, pieces > 0 && g.gathered > 0.0, "gathering at the strike took " + std::to_string(pieces) + " pieces, " +
                                                              std::to_string(int(g.gathered)) + " voxels");
                check(st, ratio > 0.3 && ratio < 2.5, "about what was broken (" + std::to_string(int(t.brokenVoxels)) + " voxels; ratio " +
                                                          std::to_string(ratio).substr(0, 4) + ")");
                check(st, !gather::inventoryText(inventory).empty(), "into the inventory: " + gather::inventoryText(inventory));
                st.wait = 30;
                st.step = 21;
                break;
            }
            case 21: {
                const gather::Gather& g = world.ctx().get<gather::Gather>();
                check(st, g.pulling.empty(), "and the pieces pulled in are gone");
                st.step = 22;
                break;
            }
            case 22: {   // three blocks of what there is most of, on the ground round the start
                gather::Inventory& inventory = world.ctx().get<gather::Inventory>();
                for (const char* m : gather::MATERIALS)
                    if (st.material.empty() || inventory.voxels[m] > inventory.voxels[st.material]) st.material = m;
                st.before = inventory.voxels[st.material];
                build::Build& b = world.ctx().get<build::Build>();
                const vec3 f = glm::normalize(vec3(player.forward().x, 0.0f, player.forward().z)), r(-f.z, 0.0f, f.x);
                const vec3 spots[3] = {spawn + f * 2.6f - r * 1.6f, spawn + f * 2.6f + r * 1.6f, spawn + f * 3.6f};
                for (const vec3& spot : spots) {
                    const vec3 target = spot - vec3(0.0f, 3.0f, 0.0f);
                    const auto hit = runtime::castRay(world, spawn, glm::normalize(target - spawn), 8.0f, 0xffffffffu, t.walker);
                    if (!hit || hit->entity != t.island) continue;
                    const projv::core::ivec3 cell = build::cellFor(hit->position, hit->normal);
                    if (build::place(world, b, st.material, cell)) st.blocks.push_back(cell);
                }
                check(st, st.blocks.size() == 3, "three " + st.material + " blocks placed (" + std::to_string(st.blocks.size()) + ")" +
                                                     (b.refused.empty() ? "" : "; " + b.refused));
                st.wait = 20;
                st.step = 23;
                break;
            }
            case 23: {
                auto& scene = world.ctx().get<projv::Scene>();
                const projv::SceneGrid* ground = build::gridOf(world, island.component);
                int full = 0;
                for (const projv::core::ivec3& cell : st.blocks) {
                    int solid = 0;
                    for (int z = 0; z < build::BLOCK; z++)
                        for (int y = 0; y < build::BLOCK; y++)
                            for (int x = 0; x < build::BLOCK; x++) solid += tools::plantAt(scene, *ground, cell + projv::core::ivec3(x, y, z)) ? 1 : 0;
                    if (solid == build::BLOCK * build::BLOCK * build::BLOCK) full++;
                }
                check(st, full == int(st.blocks.size()) && full > 0, "they are in the island (" + std::to_string(full) + " cells full)");
                const double spent = st.before - world.ctx().get<gather::Inventory>().voxels[st.material];
                check(st, std::abs(spent - 3.0 * gather::VOXELS_PER_BLOCK) < 1e-6,
                      "and cost " + std::to_string(int(spent)) + " voxels of " + st.material + " (64 a block)");
                st.step = 4;
                break;
            }
            case 4: {   // a tree near the start: strike its trunk until it comes down
                const auto trees = island::treesNear(spawn - vec3(40.0f), spawn + vec3(40.0f), island.seed);
                const island::Tree* pick = nullptr;
                float best = 1e9f;
                for (const island::Tree& tree : trees) {
                    if (tree.kind != island::TreeKind::Broad) continue;
                    const float d = glm::length(vec3(tree.base.x - spawn.x, 0.0f, tree.base.z - spawn.z));
                    if (d < best) { best = d; pick = &tree; }
                }
                if (!pick) { check(st, false, "a broadleaf tree near the start"); st.step = 6; break; }
                const vec3 trunk = pick->base + pick->lean * (1.4f / pick->height) + vec3(0.0f, 1.4f + 0.3f * float(st.tries), 0.0f);
                bool reached = false;
                if (st.tries == 0) {
                    // The first: aimed, from whichever side has a clear line to the trunk.
                    for (int k = 0; k < 8 && !reached; k++) {
                        const float a = float(k) * 0.785398f;
                        const vec3 from = trunk + vec3(std::cos(a) * 1.8f, 0.3f, std::sin(a) * 1.8f);
                        const vec3 forward = glm::normalize(trunk - from);
                        const auto hit = tools::aim(world, from, forward, t.walker);
                        if (!hit || glm::length(vec3(hit->point.x - trunk.x, 0.0f, hit->point.z - trunk.z)) > pick->trunk + 0.6f) continue;
                        tools::strike(world, t, tools::Tool::Pickaxe, *hit, forward);
                        reached = true;
                    }
                    check(st, reached, "the pickaxe reaches a trunk");
                } else {
                    // Then at the trunk's middle, a little higher each time, as a player chopping round it would.
                    runtime::Strike s;
                    s.at = trunk;
                    s.energy = tools::PICKAXE_ENERGY;
                    s.shardSize = tools::PICKAXE_SHARD;
                    runtime::fracture(world, island.entity, s);
                    reached = true;
                }
                if (!reached) { st.step = 6; break; }
                st.tries++;
                st.wait = 25;
                st.step++;
                break;
            }
            case 5:
                if (t.fallen > 0) {
                    check(st, true, "the tree came down after " + std::to_string(st.tries) + " strikes");
                    st.wait = 30;
                    st.step = 30;
                } else if (st.tries >= 8) {
                    check(st, false, "the tree came down within 8 strikes");
                    st.step = 30;
                } else st.step = 4;
                break;
            case 30: {   // save, and read the save back: the same voxels, inventory and place (D10)
                saveWorld(app, true);
                const Session& session = world.ctx().get<Session>();
                saving::WorldInfo saved;
                check(st, saving::readInfo(session.folder, saved), "the world saved, and its world.json reads back");
                const gather::Inventory& inventory = world.ctx().get<gather::Inventory>();
                bool same = saved.inventory.size() == inventory.voxels.size();
                for (const auto& [material, voxels] : inventory.voxels) same = same && std::abs(saved.inventory[material] - voxels) < 1e-6;
                check(st, same, "with the inventory as it is");
                const projv::Transform* w = world.try_get<projv::Transform>(player.walker);
                check(st, saved.hasPlayer && w && glm::distance(saved.feet, w->position) < 1e-3f, "and the player where they stand");
                const projv::Scene back = projv::utils::loadComposeFromDisk(session.folder.string());
                projv::ComponentHandle backIsland = projv::INVALID_COMPONENT_HANDLE;
                for (size_t i = 0; i < back.components.size(); i++)
                    if (back.components[i].name == "island") backIsland = projv::ComponentHandle(i);
                const projv::DataFile live = projv::utils::dataFileFromComponent(world.ctx().get<projv::Scene>(), island.component);
                const projv::DataFile file = backIsland != projv::INVALID_COMPONENT_HANDLE ? projv::utils::dataFileFromComponent(back, backIsland)
                                                                                          : projv::DataFile{};
                bool voxels = live.blocks.size() == file.blocks.size() && !live.blocks.empty();
                for (size_t i = 0; voxels && i < live.blocks.size(); i++)
                    voxels = live.blocks[i].gridX == file.blocks[i].gridX && live.blocks[i].gridY == file.blocks[i].gridY &&
                             live.blocks[i].gridZ == file.blocks[i].gridZ && live.blocks[i].geometry == file.blocks[i].geometry &&
                             live.blocks[i].materialIDs == file.blocks[i].materialIDs;
                check(st, voxels, "and the island loaded back from it has the same voxels (" + std::to_string(file.blocks.size()) + " cells)");
                st.step = 99;
                break;
            }
            default:
                projv::core::info("island self-test:\n{}island self-test: {}", st.report, st.failed ? "FAILED" : "passed");
                exitCode = st.failed ? 1 : 0;
                app.closeRequested = true;
                st.on = false;
                break;
        }
    }

    // Butterflies and falling leaves (critters.h): written to Transforms before the bridge reads them.
    void life(projv::Application& app) {
        if (!playing(app.world)) return;
        critters::update(app.world, app.time().delta, app.world.ctx().get<Player>().eye);
    }

    // After the camera: the tool in hand, its swing, and the particles strikes throw off (fx.h).
    void toolLook(projv::Application& app) {
        World& world = app.world;
        if (!playing(world)) return;
        Player& player = world.ctx().get<Player>();
        const float walking = std::min(1.0f, glm::length(vec3(player.velocity.x, 0.0f, player.velocity.z)) / WALK_SPEED);
        fx::update(world, app.time().unscaledDelta, player.eye, player.yaw, player.pitch, walking);
    }

    // After physics presents: the eye where the walker is drawn.
    // The eye over the walker's feet. On the ground its height eases over the voxels' steps (the controller
    // climbs and drops them whole, 12.5 cm at a time, which the view felt as a jolt); in the air it follows.
    void camera(projv::Application& app) {
        if (!playing(app.world)) return;
        Player& player = app.world.ctx().get<Player>();
        const projv::Transform* t = app.world.try_get<projv::Transform>(player.walker);
        if (!t) return;
        const vec3 target = t->position + vec3(0.0f, EYE, 0.0f);
        float y = target.y;
        if (player.eased && runtime::characterState(app.world, player.walker).onGround) {
            y = player.eye.y + (target.y - player.eye.y) * (1.0f - std::exp(-STEP_EASE * app.time().unscaledDelta));
            y = std::clamp(y, target.y - STEP_LAG, target.y + STEP_LAG);
        }
        player.eye = vec3(target.x, y, target.z);
        player.eased = true;
    }

    // ---- Worlds: saving, loading, the start screen --------------------------------------------------------

    // The voxels' edit count: anything that changes the island or the plants moves it.
    uint64_t editSignature(const World& world) {
        const tools::Tools& t = world.ctx().get<tools::Tools>();
        return uint64_t(t.breaks) * 1000003u + uint64_t(t.plantsCleared) * 7919u + uint64_t(world.ctx().get<build::Build>().placed);
    }

    // Saves the world being played: world.json now, the voxels (if they changed) on the worker. `wait`: until
    // the voxels are on disk (quitting).
    void saveWorld(projv::Application& app, bool wait) {
        World& world = app.world;
        Session& session = world.ctx().get<Session>();
        if (!session.playing || !session.saves) return;
        const Player& player = world.ctx().get<Player>();
        saving::WorldInfo& info = session.info;
        if (const projv::Transform* t = world.try_get<projv::Transform>(player.walker)) {
            info.feet = t->position;
            info.hasPlayer = true;
        }
        info.yaw = player.yaw;
        info.pitch = player.pitch;
        info.inventory = std::map<std::string, double>(world.ctx().get<gather::Inventory>().voxels.begin(),
                                                       world.ctx().get<gather::Inventory>().voxels.end());
        info.collect = world.ctx().get<gather::Gather>().collect;
        info.played += app.time().elapsed - session.sessionStart;
        session.sessionStart = app.time().elapsed;
        info.lastPlayed = saving::now();
        if (!saving::writeInfo(session.folder, info)) projv::core::error("island: could not write '{}'", (session.folder / "world.json").string());
        const uint64_t edits = editSignature(world);
        const bool voxels = edits != session.saver.savedSignature;
        if (voxels) {
            const auto& scene = world.ctx().get<projv::Scene>();
            const Island& island = world.ctx().get<Island>();
            std::vector<std::pair<std::string, projv::DataFile>> files;
            files.push_back({"island.data", projv::utils::dataFileFromComponent(scene, island.component)});
            if (island.plants != projv::INVALID_COMPONENT_HANDLE)
                files.push_back({"plants.data", projv::utils::dataFileFromComponent(scene, island.plants)});
            saving::writeVoxels(session.saver, session.folder, std::move(files));
            session.saver.savedSignature = edits;
        }
        if (wait) session.saver.wait();
        session.saver.sinceSave = 0.0f;
        session.saver.saves++;
        projv::core::info("island: saved '{}'{}", info.name, voxels ? "" : " (world.json only: no voxels changed)");
    }

    void refreshStartScreen(World& world) {
        Session& session = world.ctx().get<Session>();
        StartScreen& start = world.ctx().get<StartScreen>();
        start.worlds.clear();
        for (const saving::WorldInfo& w : saving::listWorlds(session.worlds))
            if (w.folder.rfind("selftest-", 0) != 0)   // the self-test's own world is not one to play
            start.worlds.push_back({w.folder, w.name, "seed " + std::to_string(w.seed), saving::lastPlayedText(w.lastPlayed), saving::playedText(w.played)});
        start.hasWorlds = !start.worlds.empty();
    }

    void openHud(projv::Application& app);   // below

    // Loads the world in session.folder (new: made from session.info.seed) and starts playing it.
    void finishWorld(projv::Application& app, const fs::path& here);   // below

    // Starts entering the world in session.folder (new: made from session.info.seed): the loader's thread
    // makes and reads it while the loading screen shows; finishWorld takes it in when it is read.
    void beginWorld(projv::Application& app, const fs::path&) {
        World& world = app.world;
        Session& session = world.ctx().get<Session>();
        Loader& loader = world.ctx().get<Loader>();
        loader.began = std::chrono::steady_clock::now();
        loader.stage = fs::exists(session.folder / "compose.json") ? Loader::Reading : Loader::Making;
        const fs::path folder = session.folder;
        const uint32_t seed = session.info.seed;
        loader.thread = std::thread([&loader, folder, seed] {
            if (loader.stage == Loader::Making) {
                const auto start = std::chrono::steady_clock::now();
                if (!island::generate(folder, seed, &loader.cells)) {
                    projv::core::error("island: could not write '{}'", folder.string());
                    loader.stage = Loader::Failed;
                    return;
                }
                loader.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                projv::core::info("island: generated in {:.1f} s", loader.seconds);
                loader.stage = Loader::Reading;
            }
            loader.scene = projv::utils::loadComposeFromDisk(folder.string());
            loader.stage = Loader::Ready;
        });
        world.ctx().get<LoadingScreen>().world = session.info.name;
        projv::ui::user::hide(app, "start");
        projv::ui::user::show(app, "loading");
        projv::graphics::setCursorCaptured(app, world.ctx().get<projv::graphics::RenderInstance>(), false);
    }

    // Each frame while a world is being entered: the loading screen, and the world taken in once it is read
    // (after "Preparing" has been drawn: what follows holds the frame for a few seconds).
    void loadingSystem(projv::Application& app) {
        World& world = app.world;
        Loader& loader = world.ctx().get<Loader>();
        const int stage = loader.stage;
        if (stage == Loader::Idle) return;
        LoadingScreen& screen = world.ctx().get<LoadingScreen>();
        const float total = float(island::CELLS * island::LAYERS * island::CELLS);
        const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - loader.began).count();
        if (stage == Loader::Failed) {
            if (loader.thread.joinable()) loader.thread.join();
            std::exit(1);
        }
        if (stage == Loader::Making) {
            const float done = std::min(float(loader.cells) / total, 1.0f);
            screen.title = "Making the island";
            screen.fill = std::to_string(int(done * 0.9f * float(LOADING_BAR))) + "dp";   // the last tenth is reading it
            screen.detail = std::to_string(int(done * 100.0f)) + "%  -  " + std::to_string(int(elapsed)) + " s";
        } else if (stage == Loader::Reading) {
            screen.title = "Loading the island";
            screen.fill = std::to_string(int(0.92f * float(LOADING_BAR))) + "dp";
            screen.detail = "Reading it from disk";
        } else if (stage == Loader::Ready) {
            screen.title = "Preparing the world";
            screen.fill = std::to_string(LOADING_BAR) + "dp";
            screen.detail = "Almost there";
            if (++loader.readyFrames < 3) return;   // drawn first
            loader.thread.join();
            loader.stage = Loader::Idle;
            finishWorld(app, projv::core::executableDirectory());
        }
    }

    // Takes the read world in: physics, the player, life, tools, the GPU's copy; then play.
    void finishWorld(projv::Application& app, const fs::path& here) {
        World& world = app.world;
        Session& session = world.ctx().get<Session>();
        Loader& loader = world.ctx().get<Loader>();
        // How long each part of entering a world takes, for the log.
        auto clock = std::chrono::steady_clock::now();
        auto lap = [&clock]() {
            const auto now = std::chrono::steady_clock::now();
            const double s = std::chrono::duration<double>(now - clock).count();
            clock = now;
            return s;
        };
        if (!takeIsland(app, session.folder, session.info.seed, std::move(loader.scene))) std::exit(1);
        const double tRead = lap();
        installWorld(app, here);
        const double tInstall = lap();
        spawnPlayer(world, session.info);
        critters::install(world, world.ctx().get<Island>().seed, world.ctx().get<Player>().eye);
        tools::install(app, world.ctx().get<Island>().entity, world.ctx().get<Island>().plants, world.ctx().get<Player>().walker);
        gather::install(world);
        build::install(world);
        fx::install(world, world.ctx().get<Island>().component);
        for (const auto& [material, voxels] : session.info.inventory) world.ctx().get<gather::Inventory>().voxels[material] = voxels;
        world.ctx().get<gather::Gather>().collect = session.info.collect;
        session.saver.savedSignature = editSignature(world);
        session.sessionStart = app.time().elapsed;
        const double tLife = lap();

        // The scene changed whole: the GPU's copy is made again.
        if (auto* gpu = world.ctx().find<projv::GPUData>()) projv::graphics::destroyGPUData(*gpu);
        world.ctx().insert_or_assign<projv::GPUData>(projv::graphics::createTexturesForScene(world.ctx().get<projv::Scene>()));
        world.ctx().get<Restir>().has = false;
        const double tGpu = lap();
        projv::core::info("island: entered in {:.2f} s, {:.2f} s of it on the main thread -- taken in {:.2f}, physics and world {:.2f}, "
                          "player and life {:.2f}, GPU {:.2f}",
                          std::chrono::duration<double>(std::chrono::steady_clock::now() - loader.began).count(),
                          tRead + tInstall + tLife + tGpu, tRead, tInstall, tLife, tGpu);

        session.playing = true;
        if (session.saves) {
            session.info.lastPlayed = saving::now();
            saving::writeInfo(session.folder, session.info);   // so it is on the start screen from now on
        }
        projv::ui::user::hide(app, "loading");
        openHud(app);
        session.playFrame = app.frameCount;
        projv::graphics::setCursorCaptured(app, world.ctx().get<projv::graphics::RenderInstance>(), true);
        glfwSetWindowTitle(world.ctx().get<projv::graphics::RenderInstance>().window,
                           ("ProjectV Island  |  " + session.info.name + "  |  seed " + std::to_string(session.info.seed)).c_str());
        projv::core::info("island: playing '{}' (seed {})", session.info.name, session.info.seed);
    }

    // Each frame: a world chosen on the start screen is loaded; while playing, the autosave.
    void sessionSystem(projv::Application& app) {
        World& world = app.world;
        Session& session = world.ctx().get<Session>();
        const fs::path here = projv::core::executableDirectory();
        // The pixel art is drawn at a whole number of screen pixels per art pixel (D9): about 450 screen
        // pixels tall make one.
        if (const bgfx::Stats* stats = bgfx::getStats(); stats && stats->height > 0)
            projv::ui::user::setDpRatio(app, float(std::max(1, int(std::lround(float(stats->height) / 450.0f)))));
        // ISLAND_START_PICK=<folder>: the start screen picks that world at frame 60 (to test the restart into it).
        if (const char* pick = std::getenv("ISLAND_START_PICK"); pick && !session.playing && app.frameCount == 60) session.pending = pick;
        if (!session.playing && (session.pendingNew || !session.pending.empty())) {
            StartScreen& start = world.ctx().get<StartScreen>();
            std::string folder = session.pending;
            if (session.pendingNew) {
                // A new world: its folder and world.json now (the island is made when it is entered).
                saving::WorldInfo info;
                info.name = start.newName.empty() ? "My island" : start.newName;
                char* end = nullptr;
                const unsigned long typed = std::strtoul(start.newSeed.c_str(), &end, 10);
                info.seed = !start.newSeed.empty() && end && *end == '\0' ? uint32_t(typed) : std::random_device{}() % 1000000u;
                info.folder = folder = saving::folderFor(session.worlds, info.name);
                std::error_code error;
                fs::create_directories(session.worlds / folder, error);
                if (!saving::writeInfo(session.worlds / folder, info)) {
                    start.status = "The new world could not be written.";
                    session.pendingNew = false;
                    return;
                }
            }
            session.pending.clear();
            session.pendingNew = false;
            relaunchInto = folder;
            app.closeRequested = true;
            return;
        }
        if (!session.playing) return;
        timeGraphics(world, app.time().unscaledDelta);
        session.saver.sinceSave += app.time().unscaledDelta;
        if (session.saver.sinceSave >= saving::AUTOSAVE_SECONDS && !session.saver.busy) saveWorld(app, false);
    }

    void render(projv::Application& app) {
        World& world = app.world;
        auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
        auto& scene = world.ctx().get<projv::Scene>();
        auto& gpu = world.ctx().get<projv::GPUData>();
        projv::graphics::flushSceneUpdates(scene, gpu);

        // Before a world is chosen: the sky, turning slowly, under the start screen.
        vec3 eye(island::CENTRE, 20.0f, island::CENTRE), look(1.0f, 0.12f, 0.0f);
        if (!playing(world)) {
            const float a = float(app.time().elapsed) * 0.03f;
            look = glm::normalize(vec3(std::cos(a), 0.12f, std::sin(a)));
        } else {
            const Player& player = world.ctx().get<Player>();
            eye = player.eye;
            look = player.forward();
        }
        if (const char* view = std::getenv("ISLAND_VIEW"); view && std::string(view) == "overview") {
            eye = vec3(island::CENTRE - 30.0f, 85.0f, -55.0f);
            look = glm::normalize(vec3(island::CENTRE, 10.0f, island::CENTRE * 0.9f) - eye);
        }
        // ISLAND_LOOK="x,y,z,tx,ty,tz": the eye at x,y,z looking at tx,ty,tz (for captures).
        if (const char* at = std::getenv("ISLAND_LOOK")) {
            float v[6];
            if (std::sscanf(at, "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                eye = vec3(v[0], v[1], v[2]);
                look = glm::normalize(vec3(v[3], v[4], v[5]) - eye);
            }
        }
        world.ctx().get<Restir>().draw(world, renderInstance, eye, look);
        projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpu);
        if (renderInstance.shouldClose) app.closeRequested = true;

        // ISLAND_START_CAPTURE=/tmp/s: the start screen at frame 120, then exit.
        if (const char* capture = std::getenv("ISLAND_START_CAPTURE")) {
            if (app.frameCount == 120) bgfx::requestScreenShot(BGFX_INVALID_HANDLE, capture);
            if (app.frameCount == 125) app.closeRequested = true;
        }
        // ISLAND_MENU=pause|graphics: that menu open at frame 200 (for a capture of it).
        const uint64_t played = playing(world) ? app.frameCount - world.ctx().get<Session>().playFrame : 0;   // frames since play began
        if (const char* menu = std::getenv("ISLAND_MENU"); menu && played == 200) {
            setPause(app, true);
            if (std::string(menu) == "graphics") showGraphics(app, true);
        }
        if (const char* capture = std::getenv("ISLAND_CAPTURE")) {
            if (played == 240) bgfx::requestScreenShot(BGFX_INVALID_HANDLE, capture);
            if (played == 245) app.closeRequested = true;
        }
    }

    void startup(projv::Application& app) {
        World& world = app.world;
        const fs::path here = projv::core::executableDirectory();
        std::error_code error;
        fs::current_path(here, error);   // restir-pt names its shaders relative to the working directory

        auto& renderInstance = world.ctx().emplace<projv::graphics::RenderInstance>();
        renderInstance.initialize(1600, 900, "ProjectV Island");
        projv::graphics::installPlatform(app, renderInstance);
        projv::ui::installUi(app, renderInstance);

        world.ctx().emplace<projv::Scene>();   // empty until a world is chosen
        Session& session = world.ctx().emplace<Session>();
        session.worlds = here / "worlds";
        fs::create_directories(session.worlds, error);
        world.ctx().emplace<SelfTest>().on = std::getenv("ISLAND_SELFTEST") != nullptr;
        world.ctx().emplace<Hud>();
        StartScreen& start = world.ctx().emplace<StartScreen>();
        world.ctx().emplace<Loader>();
        LoadingScreen& loading = world.ctx().emplace<LoadingScreen>();
        PauseMenu& menu = world.ctx().emplace<PauseMenu>();

        projv::ui::user::loadFont(app, (here / "ui/fonts/PixelifySans.ttf").string());
        if (!icons::writeAll(here / "icons")) projv::core::warn("island: the HUD's icons could not be written to '{}'", (here / "icons").string());
        projv::ui::user::model(app, "start").bind("worlds", &start.worlds).bind("hasWorlds", &start.hasWorlds).bind("newName", &start.newName)
            .bind("newSeed", &start.newSeed).bind("status", &start.status);
        projv::ui::user::model(app, "loading").bind("title", &loading.title).bind("world", &loading.world).bind("fill", &loading.fill)
            .bind("detail", &loading.detail);
        projv::ui::user::open(app, "loading", (here / "ui/loading.rml").string(), {.shown = false});
        projv::ui::user::model(app, "pause").bind("world", &menu.world).bind("saved", &menu.saved).bind("graphics", &menu.graphics);
        Hud& hud = world.ctx().get<Hud>();
        projv::ui::user::model(app, "island").bind("slots", &hud.slots).bind("compass", &hud.compass).bind("heading", &hud.heading)
            .bind("held", &hud.held).bind("heldShown", &hud.heldShown).bind("aiming", &hud.aiming).bind("prompt", &hud.prompt)
            .bind("collecting", &hud.collecting);
        app.events().on<projv::ui::UiAction>([&app](const projv::ui::UiAction& action) {
            World& w = app.world;
            Session& s = w.ctx().get<Session>();
            if (action.name == "play" && !action.args.empty() && !s.playing) s.pending = action.args[0];
            else if (action.name == "new" && !s.playing) s.pendingNew = true;
            else if (action.name == "quit") app.closeRequested = true;
            else if (action.name == "resume") setPause(app, false);
            else if (action.name == "graphics") showGraphics(app, true);
            else if (action.name == "back") showGraphics(app, false);
            else if (action.name == "cycle" && !action.args.empty()) cycleGraphics(w, action.args[0]);
            else if (action.name == "savequit") {
                saveWorld(app, true);
                s.saves = false;   // saved: nothing more to save on the way out
                app.closeRequested = true;
            }
        });

        if (!restir_pt::install(renderInstance)) std::exit(1);
        Restir& restir = world.ctx().emplace<Restir>();
        restir.scene.finest = island::VOXEL;
        restir.scene.radius = island::SIZE * 0.75f;
        restir.scene.ground = 0.0f;
        restir.scene.height = 80.0f;   // the clouds are up to 80 m

        // The preset: settings.json's, else timed on the first play; --graphics, captures and the self-test
        // choose for themselves and keep nothing.
        Graphics& graphics = world.ctx().emplace<Graphics>();
        readGraphics(graphics, restir.settings);
        const bool unattended = std::getenv("ISLAND_CAPTURE") || std::getenv("ISLAND_START_CAPTURE") || world.ctx().get<SelfTest>().on;
        if (!options.graphics.empty() || unattended) {
            graphics.keep = false;
            graphics.chosen = true;
            graphics.preset = options.graphics == "performance" ? restir_pt::Preset::Performance
                            : options.graphics == "quality" || unattended ? restir_pt::Preset::Quality : graphics.preset;
        }
        const bool chosen = graphics.chosen, keep = graphics.keep;   // drawn now; not yet chosen or written
        graphics.keep = false;
        if (graphics.custom && options.graphics.empty() && !unattended) applyGraphics(world);   // settings.json's own
        else setGraphics(world, graphics.preset);
        graphics.chosen = chosen;
        graphics.keep = keep;
        projv::ui::user::model(app, "graphics").bind("rows", &graphics.rows).bind("note", &graphics.note);
        world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(world.ctx().get<projv::Scene>()));

        // Captures, the self-test and --seed go straight into a world; otherwise the start screen.
        const bool capture = std::getenv("ISLAND_CAPTURE") != nullptr;
        if (!options.world.empty()) {
            session.folder = session.worlds / options.world;
            if (!saving::readInfo(session.folder, session.info)) {
                projv::core::error("island: no world in '{}'", session.folder.string());
                std::exit(1);
            }
            beginWorld(app, here);
        } else if (options.seeded || capture || world.ctx().get<SelfTest>().on) {
            session.saves = !capture;
            session.info.seed = options.seeded ? options.seed : 42u;
            session.info.folder = (world.ctx().get<SelfTest>().on ? "selftest-" : "island-") + std::to_string(session.info.seed);
            session.folder = session.worlds / session.info.folder;
            if (world.ctx().get<SelfTest>().on) fs::remove_all(session.folder, error);   // the self-test starts from a fresh world
            if (!saving::readInfo(session.folder, session.info)) session.info.name = session.info.folder;
            beginWorld(app, here);
        } else {
            refreshStartScreen(world);
            projv::ui::user::open(app, "start", (here / "ui/start.rml").string(), {.shown = true, .modal = true});
            projv::graphics::setCursorCaptured(app, renderInstance, false);
        }
        projv::ui::user::open(app, "pause", (here / "ui/pause.rml").string(), {.shown = false, .modal = true, .fadeOut = 0.2f});
        projv::ui::user::open(app, "graphics", (here / "ui/graphics.rml").string(), {.shown = false, .modal = true, .fadeOut = 0.15f});
    }

    // The HUD (ui/hud.rml, D9): the compass, the crosshair, the prompt, the hotbar.
    void openHud(projv::Application& app) {
        projv::ui::user::open(app, "hud", (projv::core::executableDirectory() / "ui/hud.rml").string());
    }

    void shutdown(projv::Application& app) {
        if (auto* loader = app.world.ctx().find<Loader>(); loader && loader->thread.joinable()) loader->thread.join();   // a world half made
        // Closing the window saves too (the menu's Save and quit has already, and turned this off).
        if (auto* session = app.world.ctx().find<Session>(); session && session->playing && session->saves) saveWorld(app, true);
        if (auto* gpu = app.world.ctx().find<projv::GPUData>()) projv::graphics::destroyGPUData(*gpu);
    }
}

int main(int argc, char** argv) {
    projv::ui::setCommandLine(argc, argv);
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) {
            options.seed = uint32_t(std::strtoul(argv[++i], nullptr, 10));
            options.seeded = true;
        } else if (!std::strcmp(argv[i], "--world") && i + 1 < argc) {
            options.world = argv[++i];
        } else if (!std::strcmp(argv[i], "--graphics") && i + 1 < argc) {
            options.graphics = argv[++i];
            if (options.graphics != "quality" && options.graphics != "performance") {
                std::fprintf(stderr, "--graphics takes quality or performance\n");
                return 2;
            }
        }

    projv::Application app;
    app.addSystem(projv::Stage::Startup, "startup", startup);
    app.addSystem(projv::Stage::Update, "session", sessionSystem);
    app.addSystem(projv::Stage::Update, "loading", loadingSystem, {.after = "session"});
    app.addSystem(projv::Stage::Update, "controls", controls);
    app.addSystem(projv::Stage::Update, "life", life);
    app.addSystem(projv::Stage::Update, "tools", useTools, {.after = "controls"});
    app.addSystem(projv::Stage::Update, "self-test", selfTest, {.after = "tools"});
    app.addSystem(projv::Stage::PostUpdate, "camera", camera, {.after = "physics: present"});
    app.addSystem(projv::Stage::PostUpdate, "tool look", toolLook, {.before = "scene bridge", .after = "camera"});
    app.addSystem(projv::Stage::Render, "render", render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    if (!relaunchInto.empty()) {
        // The start screen chose a world: this program has shut down; start it again, straight into it.
        const std::string self = (projv::core::executableDirectory() / fs::path(argv[0]).filename()).string();
        std::vector<char*> args = {const_cast<char*>(self.c_str()), const_cast<char*>("--world"), relaunchInto.data(), nullptr};
#ifdef _WIN32
        _execv(self.c_str(), args.data());
#else
        execv(self.c_str(), args.data());
#endif
        std::fprintf(stderr, "island: could not start again in '%s'\n", relaunchInto.c_str());
        return 1;
    }
    return exitCode;
}
