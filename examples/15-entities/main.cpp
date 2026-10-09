// ProjectV Entities
//
// The runtime on top of the voxels: entities, systems, Time, Input, events, and the Scene bridge
// that links them to voxel components. Hello Voxel's window and renderer, with things that move.
//
// What is on screen is a compose folder (`scene/`, staged beside the binary): a sun, and a planet
// with a moon inside it. The voxels are in compose.json. The *entities* are in entities.json beside
// it -- open scene/entities.json and scene/Planet/entities.json. Each entity names the component it
// links to by its id and carries its ECS components as they are: "example.spin", "example.orbit".
// What the file says is what runs.
//
//   1. Startup    window, platform (Input + close events), load the compose folder into a Scene
//   2.            install the Scene bridge; register Spin and Orbit as saveable components
//   3.            spawnEntities: every entities.json in the scene, inner folders first
//   4. Update     Spin and Orbit systems write each entity's Transform, scaled by Time::delta
//   5. PostUpdate the bridge writes changed Transforms into the Scene (and rebakes the subtree)
//   6. Render     flush the moved chunk headers to the GPU and draw
//
// The planet is Root-linked and orbits the sun; the moon moves with it for free, because it is a
// child of the planet in the Scene hierarchy. The moon's entity is declared in the planet's own
// entities.json, so it is linked as a Part, and orbits inside the planet -- in the planet's space,
// which is what a Part link's Transform is measured in.
//
//   ./entities                       run
//   ./entities --write-scene <dir>   regenerate the folder this example ships
//
// Controls: W/S/A/D move, R/F up/down, mouse looks (Esc releases the cursor, left-click recaptures),
// Space pauses time (Time::scale), close the window to quit.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

#include <glm/gtc/quaternion.hpp>

#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/input.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "runtime/entities.h"
#include "runtime/scene_bridge.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

using projv::core::vec3;
using projv::core::quat;

// -----------------------------------------------------------------------------------------
// The components: what the file says, and what runs
// -----------------------------------------------------------------------------------------
//
// { "example.spin":  { "v": 1, "speed": 0.4 } }
// { "example.orbit": { "v": 1, "speed": 0.35, "radius": 45 } }

struct Spin  { float speed = 1.0f; };                              // radians per second
struct Orbit { float speed = 1.0f; float radius = 0.0f; float angle = 0.0f; };

double tidy(float v) { return std::round(double(v) * 1e4) / 1e4; }   // 0.4, not 0.4000000059604645

template<> struct projv::runtime::ComponentTraits<Spin> {
    static constexpr const char* key = "example.spin";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const Spin& s) { return nlohmann::json{{"speed", tidy(s.speed)}}; }
    static std::optional<Spin> load(const nlohmann::json& j, uint32_t) { return Spin{j.value("speed", 1.0f)}; }
};

// The angle is where the orbit is now, not something the asset says, so it is not saved.
template<> struct projv::runtime::ComponentTraits<Orbit> {
    static constexpr const char* key = "example.orbit";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const Orbit& o) { return nlohmann::json{{"speed", tidy(o.speed)}, {"radius", tidy(o.radius)}}; }
    static std::optional<Orbit> load(const nlohmann::json& j, uint32_t) {
        return Orbit{j.value("speed", 1.0f), j.value("radius", 0.0f), 0.0f};
    }
};

// Setup an orbit needs from its entity: start where the asset put it, so spawning moves nothing.
// on_construct runs after the link has seeded the Transform.
void startOrbit(projv::World& world, projv::Entity entity) {
    if (const auto* t = world.try_get<projv::Transform>(entity)) {
        world.get<Orbit>(entity).angle = std::atan2(t->position.z, t->position.x);
    }
}

void spinSystem(projv::Application& app) {
    const float dt = app.time().delta;
    auto& world = app.world;
    for (auto [entity, spin] : world.view<Spin>().each()) {
        // patch, not get: the bridge only writes Transforms it was told changed.
        world.patch<projv::Transform>(entity, [&](projv::Transform& t) {
            t.rotation = glm::normalize(glm::angleAxis(spin.speed * dt, vec3(0, 1, 0)) * t.rotation);
        });
    }
}

void orbitSystem(projv::Application& app) {
    const float dt = app.time().delta;
    auto& world = app.world;
    for (auto [entity, orbit] : world.view<Orbit>().each()) {
        orbit.angle += orbit.speed * dt;
        world.patch<projv::Transform>(entity, [&](projv::Transform& t) {
            t.position = vec3(std::cos(orbit.angle) * orbit.radius, t.position.y, std::sin(orbit.angle) * orbit.radius);
        });
    }
}

// -----------------------------------------------------------------------------------------
// The scene, as code (only for --write-scene)
// -----------------------------------------------------------------------------------------

// A body: an Asset whose origin is the centre of a ball inside it. Anything that turns or orbits has
// to be built this way round -- a chunk's own origin is its corner, so spinning the ball directly
// would swing it about its corner. The ball is in alternating wedges of two colours, because a
// plain sphere turning about its own axis looks exactly like one standing still.
projv::ComponentHandle body(projv::Scene& scene, const char* name, projv::ComponentHandle parent,
                            vec3 centre, int diameter, uint32_t color, uint32_t stripe) {
    using namespace projv;
    constexpr float VOXEL = 0.5f;
    ComponentHandle node = utils::addComponent(scene, ComponentKind::Asset, name, parent, 4, 1.0f);
    utils::setComponentTransform(scene, node, centre, quat(1, 0, 0, 0), 1.0f);

    ComponentHandle ball = utils::addComponent(scene, ComponentKind::Chunk, (std::string(name) + " ball").c_str(),
                                               node, 64, VOXEL);
    const float r = diameter * 0.5f;
    utils::setComponentTransform(scene, ball, vec3(-r * VOXEL), quat(1, 0, 0, 0), 1.0f);
    std::vector<PendingVoxelOp> voxels;
    for (int z = 0; z < diameter; z++) for (int y = 0; y < diameter; y++) for (int x = 0; x < diameter; x++) {
        vec3 d = vec3(x, y, z) + vec3(0.5f) - vec3(r);
        if (glm::length(d) > r) continue;
        int wedge = int(std::floor((std::atan2(d.z, d.x) + 3.14159265f) / (3.14159265f / 3.0f)));
        voxels.push_back({true, core::ivec3(x, y, z), wedge % 2 ? stripe : color});
    }
    utils::queueVoxelAdd(scene, ball, voxels);
    return node;
}

int writeScene(const std::string& folder) {
    using namespace projv;
    {
        Scene scene;
        body(scene, "Sun", INVALID_COMPONENT_HANDLE, vec3(0), 40,
             packRGB10(1.0f, 0.75f, 0.2f), packRGB10(0.95f, 0.45f, 0.1f));
        ComponentHandle planet = body(scene, "Planet", INVALID_COMPONENT_HANDLE, vec3(45, 0, 0), 16,
                                      packRGB10(0.25f, 0.45f, 0.95f), packRGB10(0.2f, 0.75f, 0.35f));
        // Inside the planet, so it goes wherever the planet goes; its orbit is in the planet's space.
        body(scene, "Moon", planet, vec3(12, 0, 0), 6,
             packRGB10(0.85f, 0.85f, 0.85f), packRGB10(0.55f, 0.55f, 0.6f));
        utils::updateScene(scene);
        if (!utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder)) return 1;
    }

    // The entities, made the way a game would and saved: load the folder back (so every component
    // has its id and the planet its folder), link, add components, saveEntities per folder.
    Application app;
    Scene& scene = app.world.ctx().emplace<Scene>(utils::loadComposeFromDisk(folder));
    runtime::installSceneBridge(app);
    runtime::registerComponent<Spin>(app.world);
    runtime::registerComponent<Orbit>(app.world);
    auto find = [&](const char* name) {
        for (ComponentHandle h = 0; h < scene.components.size(); h++) if (scene.components[h].name == name) return h;
        return INVALID_COMPONENT_HANDLE;
    };
    ComponentHandle planet = find("Planet");
    // Authored says which file each entity belongs in, and gives it a name there.
    auto make = [&](const char* name, ComponentHandle document, LinkMode mode) {
        Entity e = runtime::spawnComponent(app.world, find(name), mode);
        app.world.emplace<runtime::Authored>(e, runtime::Authored{document, name});
        return e;
    };
    app.world.emplace<Spin>(make("Sun", INVALID_COMPONENT_HANDLE, LinkMode::Root), 0.4f);
    app.world.emplace<Orbit>(make("Planet", INVALID_COMPONENT_HANDLE, LinkMode::Root), 0.35f, 45.0f);
    // The moon's entity is the planet folder's: it is about something inside it.
    app.world.emplace<Orbit>(make("Moon", planet, LinkMode::Part), 1.4f, 12.0f);

    bool ok = runtime::saveEntities(app.world, INVALID_COMPONENT_HANDLE, folder) &&
              runtime::saveEntities(app.world, planet, (std::filesystem::path(folder) / "Planet").string());
    core::info("Wrote the entities scene to {}{}", folder, ok ? "" : " (with errors)");
    return ok ? 0 : 1;
}

// -----------------------------------------------------------------------------------------
// Camera
// -----------------------------------------------------------------------------------------

struct Camera {
    vec3 position{-70.0f, 45.0f, -70.0f};
    float yaw = 0.785f;
    float pitch = -0.45f;
};

void cameraSystem(projv::Application& app) {
    auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
    const auto& input = app.world.ctx().get<projv::Input>();
    auto& camera = app.world.ctx().get<Camera>();

    if (input.cursorCaptured && input.pressed(projv::Key::Escape)) {
        projv::graphics::setCursorCaptured(app, renderInstance, false);
    } else if (!input.cursorCaptured && input.pressed(projv::MouseButton::Left)) {
        projv::graphics::setCursorCaptured(app, renderInstance, true);
    }
    if (input.cursorCaptured) {
        camera.yaw   += input.cursorDelta.x * 0.0025f;
        camera.pitch -= input.cursorDelta.y * 0.0025f;
        camera.pitch = std::clamp(camera.pitch, -1.55f, 1.55f);
    }
    if (input.pressed(projv::Key::Space)) app.time().scale = app.time().scale == 0.0f ? 1.0f : 0.0f;

    // Units per second, from the unscaled clock: pausing the planets does not freeze the camera.
    const float speed = 40.0f * app.time().unscaledDelta;
    const vec3 forward{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
    const vec3 right{std::cos(camera.yaw - 1.5708f), 0.0f, std::sin(camera.yaw - 1.5708f)};
    if (input.down(projv::Key::W)) camera.position += forward * speed;
    if (input.down(projv::Key::S)) camera.position -= forward * speed;
    if (input.down(projv::Key::D)) camera.position += right * speed;
    if (input.down(projv::Key::A)) camera.position -= right * speed;
    if (input.down(projv::Key::R)) camera.position.y += speed;
    if (input.down(projv::Key::F)) camera.position.y -= speed;
}

// -----------------------------------------------------------------------------------------
// Stages
// -----------------------------------------------------------------------------------------

void startup(projv::Application& app) {
    const std::filesystem::path here = projv::core::executableDirectory();
    auto& renderInstance = app.world.ctx().emplace<projv::graphics::RenderInstance>();
    renderInstance.initialize(1280, 720, "ProjectV Entities");
    projv::graphics::installPlatform(app, renderInstance);
    projv::graphics::setCursorCaptured(app, renderInstance, true);
    app.world.ctx().emplace<Camera>();

    auto& scene = app.world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk((here / "scene").string()));

    projv::runtime::installSceneBridge(app);
    projv::runtime::registerComponent<Spin>(app.world);
    projv::runtime::registerComponent<Orbit>(app.world);
    app.world.on_construct<Orbit>().connect<&startOrbit>();
    projv::runtime::spawnEntities(app.world);

    projv::RendererSpecification specification =
        projv::graphics::loadRendererSpecification((here / "entitiesRenderer").string() + "/");
    renderInstance.addRendererSpecification(1, specification);
    bgfx::ShaderHandle vertexShader =
        projv::graphics::loadShader((here / "entitiesRenderer/entitiesShaders/vs_quad.bin").string());
    renderInstance.setActiveRenderer(projv::graphics::constructRendererSpecification(
        renderInstance.getRendererSpecification(1), vertexShader));

    app.world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));
    projv::core::info("Entities: {} entities, {} component(s) in the scene.",
                      app.world.view<projv::VoxelComponent>().size(), scene.components.size());
}

void render(projv::Application& app) {
    auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
    auto& scene = app.world.ctx().get<projv::Scene>();
    auto& gpuData = app.world.ctx().get<projv::GPUData>();
    const auto& camera = app.world.ctx().get<Camera>();

    // The bridge has rewritten the moved chunks' headers on the CPU; this is what reaches the GPU.
    projv::graphics::flushSceneUpdates(scene, gpuData);

    projv::core::vec3 position = camera.position;
    projv::core::vec3 direction{std::cos(camera.pitch) * std::cos(camera.yaw), std::sin(camera.pitch),
                                std::cos(camera.pitch) * std::sin(camera.yaw)};
    projv::core::vec2 resolution = renderInstance.getWindowResolution();
    auto renderer = renderInstance.getActiveRenderer();
    projv::graphics::setUniformToValue(renderer, "cameraPos", position);
    projv::graphics::setUniformToValue(renderer, "cameraDir", direction);
    projv::graphics::setUniformToValue(renderer, "windowRes", resolution);
    projv::graphics::renderConstructedRenderer(renderInstance, renderer, &gpuData);
}

void shutdown(projv::Application& app) {
    projv::graphics::destroyGPUData(app.world.ctx().get<projv::GPUData>());
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--write-scene") == 0) return writeScene(argv[2]);

    projv::Application app;
    app.addSystem(projv::Stage::Startup,  "startup", startup);
    app.addSystem(projv::Stage::Update,   "camera",  cameraSystem);
    app.addSystem(projv::Stage::Update,   "spin",    spinSystem);
    app.addSystem(projv::Stage::Update,   "orbit",   orbitSystem);
    app.addSystem(projv::Stage::Render,   "render",  render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    return 0;
}
