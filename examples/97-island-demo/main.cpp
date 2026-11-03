// ProjectV Island demo (docs/plans/island-demo.md)
//
// A generated island to break, gather and build with: hills, rock and trees made from a seed, walked in
// first person, drawn with the renderer gallery's restir-pt; grass and flowers, butterflies and falling
// leaves. Built up milestone by milestone (I0: the
// island and walking it).
//
//   ./island_demo                  a new island from a random seed (worlds/island-<seed>/)
//   ./island_demo --seed 42        that island: made the first time, loaded after
//   ISLAND_CAPTURE=/tmp/i ./island_demo --seed 42   frame 240 saved as /tmp/i.tga, then it exits
//                                  (ISLAND_VIEW=overview: from high above the shore)
//
// Controls: W/A/S/D walk, Space jump, the mouse looks (Esc frees the cursor, a click takes it back);
// 1/2 or the wheel pick the pickaxe or the chisel, the left button swings it (held: again); E turns
// collecting on or off (on: rubble near you and where you look comes in as it lands); F1 the dev layer.
//
//   ISLAND_SELFTEST=1 ./island_demo   checks itself and exits 0 or 1 (D10; what it checks grows per milestone)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>

#include <bgfx/bgfx.h>

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

#include "critters.h"
#include "fx.h"
#include "gather.h"
#include "island.h"
#include "tools.h"

namespace fs = std::filesystem;
using projv::Entity;
using projv::World;
using projv::core::vec3;
namespace runtime = projv::runtime;

namespace {
    constexpr float EYE = 1.62f;          // eye height above the feet, m
    constexpr float WALK_SPEED = 4.5f;    // m/s
    constexpr float WALK_GRIP = 12.0f, AIR_GRIP = 2.0f;

    struct Options {
        uint32_t seed = 0;
        bool     seeded = false;
    };
    Options options;
    int exitCode = 0;   // what main returns: the self-test's verdict

    struct Player {
        Entity walker = projv::NullEntity;
        float  yaw = 0.0f, pitch = -0.1f;
        vec3   velocity{0.0f};
        vec3   eye{0.0f};
        vec3 forward() const { return vec3(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)); }
    };

    struct Island {
        uint32_t seed = 0;
        fs::path folder;
        projv::ComponentHandle component = projv::INVALID_COMPONENT_HANDLE;
        projv::ComponentHandle plants = projv::INVALID_COMPONENT_HANDLE;   // drawn, no collider (D12)
        Entity entity = projv::NullEntity;
    };

    // What the HUD shows (a placeholder until I5's pixel-art HUD: a crosshair and the tool in hand).
    struct Hud {
        std::string tool = "Pickaxe";
        bool aiming = false;
        std::string prompt;      // "E: gather 3.4 stone, 1.2 dirt"
        std::string inventory;   // blocks by material
        std::string collect = "Collect: off  (E)";
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

    // ---- The island ------------------------------------------------------------------------------

    // Makes the island for the seed if its folder has none yet, then loads it: a new world and a saved one
    // come in the same way (D11).
    bool loadIsland(projv::Application& app, const fs::path& here) {
        World& world = app.world;
        Island& island = world.ctx().emplace<Island>();
        island.seed = options.seeded ? options.seed : std::random_device{}() % 1000000u;
        island.folder = here / "worlds" / ("island-" + std::to_string(island.seed));
        if (!fs::exists(island.folder / "compose.json")) {
            const auto start = std::chrono::steady_clock::now();
            if (!island::generate(island.folder, island.seed)) {
                projv::core::error("island: could not write '{}'", island.folder.string());
                return false;
            }
            projv::core::info("island: generated in {:.1f} s",
                              std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
        auto& scene = world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(island.folder.string()));
        for (size_t i = 0; i < scene.components.size(); i++)
            if (scene.components[i].name == "island") island.component = projv::ComponentHandle(i);
            else if (scene.components[i].name == "plants") island.plants = projv::ComponentHandle(i);
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

    void spawnPlayer(World& world) {
        Player& player = world.ctx().emplace<Player>();
        const Island& island = world.ctx().get<Island>();
        const vec3 at = island::spawnPoint(island.seed);
        player.walker = world.create();
        world.emplace<projv::Transform>(player.walker, projv::Transform{at});
        projv::CharacterController character;
        character.strength = 600.0f;
        world.emplace<projv::CharacterController>(player.walker, character);
        world.emplace<projv::CollisionGroups>(player.walker, projv::CollisionGroups{{"player"}, {}, 0});
        // Facing the most open view: the direction the ground stays under eye height the furthest.
        float best = -1.0f;
        for (int i = 0; i < 24; i++) {
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
    }

    // ---- Each frame --------------------------------------------------------------------------------

    void controls(projv::Application& app) {
        World& world = app.world;
        auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
        const auto& input = world.ctx().get<projv::Input>();
        Player& player = world.ctx().get<Player>();
        const float dt = app.time().unscaledDelta;

        if (input.cursorCaptured && input.pressed(projv::Key::Escape)) projv::graphics::setCursorCaptured(app, renderInstance, false);
        else if (!input.cursorCaptured && !input.wantCaptureMouse && input.pressed(projv::MouseButton::Left))
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

    // The tools (tools.h): the left button uses the one in hand while the cursor is the game's.
    void useTools(projv::Application& app) {
        World& world = app.world;
        const auto& input = world.ctx().get<projv::Input>();
        Player& player = world.ctx().get<Player>();
        bool use = input.cursorCaptured && !input.wantCaptureMouse && input.down(projv::MouseButton::Left) &&
                   !world.ctx().get<SelfTest>().on;
        // ISLAND_SWING_FRAME=n: one swing at frame n (for a capture of the swing and its particles).
        if (const char* at = std::getenv("ISLAND_SWING_FRAME"); at && app.frameCount == uint64_t(std::atoi(at))) use = true;
        tools::update(world, input, app.time().unscaledDelta, player.eye, player.forward(), use);
        Hud& hud = world.ctx().get<Hud>();
        const tools::Tools& t = world.ctx().get<tools::Tools>();
        hud.tool = std::string("[") + (t.selected == tools::Tool::Pickaxe ? "1" : "2") + "] " + tools::toolName(t.selected);
        hud.aiming = t.aiming;
        gather::update(world, input, app.time().unscaledDelta, player.eye, player.forward(), input.cursorCaptured && !world.ctx().get<SelfTest>().on);
        hud.prompt = world.ctx().get<gather::Gather>().prompt;
        hud.inventory = gather::inventoryText(world.ctx().get<gather::Inventory>());
        hud.collecting = world.ctx().get<gather::Gather>().collect;
        hud.collect = hud.collecting ? "Collect: ON  (E)" : "Collect: off  (E)";
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
                    st.step++;
                } else if (st.tries >= 8) {
                    check(st, false, "the tree came down within 8 strikes");
                    st.step++;
                } else st.step = 4;
                break;
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
        critters::update(app.world, app.time().delta, app.world.ctx().get<Player>().eye);
    }

    // After the camera: the tool in hand, its swing, and the particles strikes throw off (fx.h).
    void toolLook(projv::Application& app) {
        World& world = app.world;
        Player& player = world.ctx().get<Player>();
        const float walking = std::min(1.0f, glm::length(vec3(player.velocity.x, 0.0f, player.velocity.z)) / WALK_SPEED);
        fx::update(world, app.time().unscaledDelta, player.eye, player.yaw, player.pitch, walking);
    }

    // After physics presents: the eye where the walker is drawn.
    void camera(projv::Application& app) {
        Player& player = app.world.ctx().get<Player>();
        if (const projv::Transform* t = app.world.try_get<projv::Transform>(player.walker)) player.eye = t->position + vec3(0.0f, EYE, 0.0f);
    }

    void render(projv::Application& app) {
        World& world = app.world;
        auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
        auto& scene = world.ctx().get<projv::Scene>();
        auto& gpu = world.ctx().get<projv::GPUData>();
        Player& player = world.ctx().get<Player>();
        projv::graphics::flushSceneUpdates(scene, gpu);

        vec3 eye = player.eye, look = player.forward();
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

        if (const char* capture = std::getenv("ISLAND_CAPTURE")) {
            if (app.frameCount == 240) bgfx::requestScreenShot(BGFX_INVALID_HANDLE, capture);
            if (app.frameCount == 245) app.closeRequested = true;
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
        projv::graphics::setCursorCaptured(app, renderInstance, true);

        if (!loadIsland(app, here)) std::exit(1);
        installWorld(app, here);
        spawnPlayer(world);
        critters::install(world, world.ctx().get<Island>().seed, world.ctx().get<Player>().eye);
        tools::install(app, world.ctx().get<Island>().entity, world.ctx().get<Island>().plants, world.ctx().get<Player>().walker);
        gather::install(world);
        fx::install(world, world.ctx().get<Island>().component);
        world.ctx().emplace<SelfTest>().on = std::getenv("ISLAND_SELFTEST") != nullptr;

        // The HUD: a placeholder until I5 (a crosshair, the tool in hand).
        Hud& hud = world.ctx().emplace<Hud>();
        projv::ui::user::loadFont(app, (here / "shared_ui/fonts/LatoLatin-Regular.ttf").string());
        projv::ui::user::loadFont(app, (here / "shared_ui/fonts/LatoLatin-Bold.ttf").string());
        projv::ui::user::model(app, "island").bind("tool", &hud.tool).bind("aiming", &hud.aiming).bind("prompt", &hud.prompt).bind("inventory", &hud.inventory)
            .bind("collect", &hud.collect).bind("collecting", &hud.collecting);
        projv::ui::user::openFromMemory(app, "hud", R"(<rml><head><style>
            body { width: 100%; height: 100%; pointer-events: none; font-family: LatoLatin; }
            .cross { position: absolute; left: 50%; top: 50%; width: 6dp; height: 6dp; margin-left: -3dp; margin-top: -3dp;
                     background-color: #ffffffa0; }
            .cross.on { background-color: #ffffffff; width: 8dp; height: 8dp; margin-left: -4dp; margin-top: -4dp; }
            .tool { position: absolute; left: 0; bottom: 24dp; width: 100%; text-align: center; font-size: 20dp; color: #ffffff;
                    font-effect: shadow(0dp 2dp #000000c0); }
            .inventory { position: absolute; left: 0; bottom: 52dp; width: 100%; text-align: center; font-size: 17dp; color: #ffffffd0;
                    font-effect: shadow(0dp 2dp #000000c0); }
            .collect { position: absolute; right: 24dp; bottom: 24dp; font-size: 18dp; color: #ffffffa0;
                    font-effect: shadow(0dp 2dp #000000c0); }
            .collect.on { color: #b8ff9c; }
            .prompt { position: absolute; left: 0; top: 56%; width: 100%; text-align: center; font-size: 18dp; color: #fff6c8;
                    font-effect: shadow(0dp 2dp #000000c0); }
            </style></head><body data-model="island">
            <div class="cross" data-class-on="aiming"></div>
            <div class="tool">{{tool}}</div>
            <div class="inventory">{{inventory}}</div>
            <div class="collect" data-class-on="collecting">{{collect}}</div>
            <div class="prompt" data-if="prompt != ''">{{prompt}}</div>
            </body></rml>)");

        if (!restir_pt::install(renderInstance)) std::exit(1);
        Restir& restir = world.ctx().emplace<Restir>();
        restir.scene.finest = island::VOXEL;
        restir.scene.radius = island::SIZE * 0.75f;
        restir.scene.ground = 0.0f;
        restir.scene.height = 80.0f;   // the clouds are up to 80 m
        world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(world.ctx().get<projv::Scene>()));
        glfwSetWindowTitle(renderInstance.window, ("ProjectV Island  |  seed " + std::to_string(world.ctx().get<Island>().seed)).c_str());
    }

    void shutdown(projv::Application& app) {
        if (auto* gpu = app.world.ctx().find<projv::GPUData>()) projv::graphics::destroyGPUData(*gpu);
    }
}

int main(int argc, char** argv) {
    projv::ui::setCommandLine(argc, argv);
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) {
            options.seed = uint32_t(std::strtoul(argv[++i], nullptr, 10));
            options.seeded = true;
        }

    projv::Application app;
    app.addSystem(projv::Stage::Startup, "startup", startup);
    app.addSystem(projv::Stage::Update, "controls", controls);
    app.addSystem(projv::Stage::Update, "life", life);
    app.addSystem(projv::Stage::Update, "tools", useTools, {.after = "controls"});
    app.addSystem(projv::Stage::Update, "self-test", selfTest, {.after = "tools"});
    app.addSystem(projv::Stage::PostUpdate, "camera", camera, {.after = "physics: present"});
    app.addSystem(projv::Stage::PostUpdate, "tool look", toolLook, {.before = "scene bridge", .after = "camera"});
    app.addSystem(projv::Stage::Render, "render", render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    return exitCode;
}
