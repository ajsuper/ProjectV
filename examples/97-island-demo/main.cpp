// ProjectV Island demo (docs/plans/island-demo.md)
//
// A generated island to break, gather and build with: hills, rock and trees made from a seed, walked in
// first person, drawn with the renderer gallery's restir-pt. Built up milestone by milestone (I0: the
// island and walking it).
//
//   ./island_demo                  a new island from a random seed (worlds/island-<seed>/)
//   ./island_demo --seed 42        that island: made the first time, loaded after
//   ISLAND_CAPTURE=/tmp/i ./island_demo --seed 42   frame 240 saved as /tmp/i.tga, then it exits
//                                  (ISLAND_VIEW=overview: from high above the shore)
//
// Controls: W/A/S/D walk, Space jump, the mouse looks (Esc frees the cursor, a click takes it back),
// F1 the dev layer.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
#include "utils/compose_io.h"

#include "island.h"

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
        Entity entity = projv::NullEntity;
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
        physics.debris.maxVoxels = 64;
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
        // Facing the island's middle.
        const vec3 d = vec3(island::CENTRE, at.y, island::CENTRE) - at;
        player.yaw = glm::length(d) > 1.0f ? std::atan2(d.z, d.x) : 0.0f;
        player.eye = at + vec3(0.0f, EYE, 0.0f);
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

        if (!restir_pt::install(renderInstance)) std::exit(1);
        Restir& restir = world.ctx().emplace<Restir>();
        restir.scene.finest = island::VOXEL;
        restir.scene.radius = island::SIZE * 0.75f;
        restir.scene.ground = 0.0f;
        restir.scene.height = island::HEIGHT;
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
    app.addSystem(projv::Stage::PostUpdate, "camera", camera, {.after = "physics: present"});
    app.addSystem(projv::Stage::Render, "render", render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    return 0;
}
