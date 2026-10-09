// ProjectV Sandbox
//
// A physics toy built entirely out of the runtime: throw things into an arena, watch them bounce,
// pop them, set off chain reactions. Every system the engine now has is load-bearing here:
//
//   Prefabs are folders.             prefabs/ball, prefabs/crate, prefabs/bomb each hold their
//                                    voxels (compose.json) and an entity (entities.json) that links
//                                    to the prefab itself and carries its Body -- open
//                                    prefabs/bomb/entities.json. What the file says is what runs.
//   Spawning is one call.            runtime::instantiatePrefab grafts the folder into the live
//                                    Scene and spawns its entity, which owns the voxels.
//   The arena is data too.           scene/compose.json is the arena and two pylons;
//                                    scene/entities.json gives each pylon a Spawner. No line of this
//                                    file names a pylon.
//   Physics is the engine's.         A prefab's entities.json gives it a physics.rigidbody (the
//                                    ball a sphere, the crate and the bomb their own voxels); the
//                                    arena's gives the floor, walls and pylons a physics.static.
//                                    Jolt steps it 60 times a second in FixedUpdate, and each body
//                                    is drawn between its last two steps, so slow motion (T) is
//                                    smooth. This file never integrates or collides anything: it
//                                    throws, pushes and pulls, with impulses and velocities.
//   Popping is an event.             Right-click sends Popped. Three handlers answer it, none
//                                    knowing about the others: a shockwave, a score, and removal.
//                                    A bomb's handler pops its neighbours -- and because anything
//                                    sent during a pump waits for the next one, a chain reaction
//                                    spreads one ring per frame instead of all at once.
//   Input and Time.                  Everything below reads Input, and moves by Time.
//
// Controls
//   mouse look, W/A/S/D fly, R/F up/down       Esc releases the cursor, left-click recaptures
//   left click    throw the selected prefab    1 ball   2 crate   3 bomb
//   right click   pop what is under the crosshair
//   E (hold)      tractor beam: pull bodies to a point in front of you
//   B             rain: drop twenty of the selected prefab from the sky
//   T  slow motion   P pause   G low gravity   X spawners on/off   C clear
//
//   ./sandbox                         play
//   ./sandbox --write-assets <dir>    regenerate scene/ and prefabs/ into <dir>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

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
#include "runtime/physics.h"
#include "runtime/scene_bridge.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/picking.h"
#include "utils/scene_query.h"

using projv::core::vec3;
using projv::core::quat;
using projv::Entity;
using projv::World;
namespace fs = std::filesystem;

namespace {

constexpr float ARENA_HALF = 62.0f;    // inner face of the walls
constexpr float KILL_PLANE = -30.0f;   // the world's floor: a body below it has left the arena for good
constexpr float GRAVITY = -28.0f, LOW_GRAVITY = -5.0f;
constexpr size_t MAX_BODIES = 300;     // spawners hold off above this

// =========================================================================================
// Components: what the files say, and what runs
// =========================================================================================

// What a sandbox body is, beyond its physics: whether popping it sets off its neighbours. Its
// shape, mass and bounce are its physics.rigidbody, which the engine simulates.
struct Body {
    float explosive = 0.0f;      // > 0: popping it pops everything within this radius
};

// Launch a prefab every `interval` seconds. The first three fields are the file's.
struct Spawner {
    std::string prefab;
    float interval = 2.0f, speed = 20.0f;
    float timer = 0.0f;
    vec3 muzzle{0.0f};
};

double tidy(float v) { return std::round(double(v) * 1e4) / 1e4; }

}  // namespace

template<> struct projv::runtime::ComponentTraits<Body> {
    static constexpr const char* key = "sandbox.body";
    // 2: the physics moved to physics.rigidbody. A version 1 value's radius, mass and restitution
    // are ignored -- they described the sandbox's own sphere simulation, which is gone.
    static constexpr uint32_t version = 2;
    static nlohmann::json save(const Body& b) {
        nlohmann::json j = nlohmann::json::object();
        if (b.explosive > 0.0f) j["explosive"] = tidy(b.explosive);
        return j;
    }
    static std::optional<Body> load(const nlohmann::json& j, uint32_t) {
        Body b;
        b.explosive = j.value("explosive", 0.0f);
        return b;
    }
};

template<> struct projv::runtime::ComponentTraits<Spawner> {
    static constexpr const char* key = "sandbox.spawner";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const Spawner& s) {
        return nlohmann::json{{"prefab", s.prefab}, {"interval", tidy(s.interval)}, {"speed", tidy(s.speed)}};
    }
    static std::optional<Spawner> load(const nlohmann::json& j, uint32_t) {
        Spawner s;
        s.prefab = j.at("prefab").get<std::string>();
        s.interval = j.value("interval", 2.0f);
        s.speed = j.value("speed", 20.0f);
        return s;
    }
};

namespace {

struct Popping {};   // a Popped event is already on its way for this entity

struct Popped {
    Entity entity;
    vec3 position;
    float explosive;
};

// The sandbox's own state, in world.ctx().
struct Sandbox {
    fs::path prefabs;
    std::vector<std::string> kinds{"ball", "crate", "bomb"};
    int selected = 0;
    int popped = 0;
    int spawnerLaunches = 0;
    int lost = 0;             // left the arena (over a wall) and fell past the kill plane
    int fellThrough = 0;      // ...from *inside* the walls: through the floor. Must stay 0.
    bool lowGravity = false;
    bool spawners = true;
    float titleTimer = 0.0f;
    std::mt19937 random{1234};
};

struct Camera {
    vec3 position{0.0f, 32.0f, -95.0f};
    float yaw = 1.5708f;     // looking down +z, into the arena
    float pitch = -0.32f;
    vec3 forward() const {
        return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
    }
};

float uniform(Sandbox& s, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(s.random); }

// =========================================================================================
// Spawning
// =========================================================================================

void setUpSpawner(World& world, Entity entity) {
    Spawner& spawner = world.get<Spawner>(entity);
    spawner.timer = spawner.interval * 0.5f;
    // A pylon's origin is its corner; the muzzle is the middle of its top.
    if (const auto* t = world.try_get<projv::Transform>(entity)) spawner.muzzle = t->position + vec3(2.0f, 13.0f, 2.0f);
}

// A prefab, thrown. instantiatePrefab grafts the folder and spawns its entity, which owns the
// voxels (OnUnlink::Destroy): destroying the entity later removes them.
Entity spawnPrefab(World& world, const std::string& kind, vec3 position, vec3 velocity) {
    auto& sandbox = world.ctx().get<Sandbox>();
    Entity entity = projv::runtime::instantiatePrefab(world, (sandbox.prefabs / kind).string(), position);
    // Its body is made at the next fixed step, and this applies right after: thrown, with a tumble.
    // The NetId is what a server would hand out; here it is what orders the bodies made in a step.
    if (entity != projv::NullEntity) {
        projv::runtime::assignNetId(world, entity);
        vec3 spin(uniform(sandbox, -3, 3), uniform(sandbox, -3, 3), uniform(sandbox, -3, 3));
        projv::runtime::setVelocity(world, entity, velocity, spin);
    }
    return entity;
}

// =========================================================================================
// Systems
// =========================================================================================

void controls(projv::Application& app) {
    World& world = app.world;
    auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
    const auto& input = world.ctx().get<projv::Input>();
    auto& camera = world.ctx().get<Camera>();
    auto& sandbox = world.ctx().get<Sandbox>();
    projv::Time& time = app.time();

    if (input.cursorCaptured && input.pressed(projv::Key::Escape)) {
        projv::graphics::setCursorCaptured(app, renderInstance, false);
    } else if (!input.cursorCaptured && input.pressed(projv::MouseButton::Left)) {
        projv::graphics::setCursorCaptured(app, renderInstance, true);
        return;   // that click was for the cursor, not a throw
    }
    if (input.cursorCaptured) {
        camera.yaw   += input.cursorDelta.x * 0.0025f;
        camera.pitch -= input.cursorDelta.y * 0.0025f;
        camera.pitch = std::clamp(camera.pitch, -1.5f, 1.5f);
    }

    // Flying runs on the unscaled clock: pausing the world does not freeze you.
    const float speed = 45.0f * time.unscaledDelta;
    const vec3 flat{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
    const vec3 right{std::cos(camera.yaw - 1.5708f), 0.0f, std::sin(camera.yaw - 1.5708f)};
    if (input.down(projv::Key::W)) camera.position += flat * speed;
    if (input.down(projv::Key::S)) camera.position -= flat * speed;
    if (input.down(projv::Key::D)) camera.position += right * speed;
    if (input.down(projv::Key::A)) camera.position -= right * speed;
    if (input.down(projv::Key::R)) camera.position.y += speed;
    if (input.down(projv::Key::F)) camera.position.y -= speed;

    if (input.pressed(projv::Key::Num1)) sandbox.selected = 0;
    if (input.pressed(projv::Key::Num2)) sandbox.selected = 1;
    if (input.pressed(projv::Key::Num3)) sandbox.selected = 2;
    if (input.pressed(projv::Key::T)) time.scale = time.scale == 1.0f ? 0.2f : 1.0f;
    if (input.pressed(projv::Key::P)) time.scale = time.scale == 0.0f ? 1.0f : 0.0f;
    if (input.pressed(projv::Key::G)) {
        sandbox.lowGravity = !sandbox.lowGravity;
        projv::runtime::setGravity(world, vec3(0, sandbox.lowGravity ? LOW_GRAVITY : GRAVITY, 0));
    }
    if (input.pressed(projv::Key::X)) sandbox.spawners = !sandbox.spawners;

    const std::string& kind = sandbox.kinds[sandbox.selected];
    if (input.cursorCaptured && input.pressed(projv::MouseButton::Left)) {
        vec3 forward = camera.forward();
        spawnPrefab(world, kind, camera.position + forward * 4.0f, forward * 38.0f + vec3(0, 4, 0));
    }
    if (input.pressed(projv::Key::B)) {
        for (int i = 0; i < 20; i++) {
            vec3 at(uniform(sandbox, -50, 50), uniform(sandbox, 40, 70), uniform(sandbox, -50, 50));
            spawnPrefab(world, kind, at, vec3(0, uniform(sandbox, -5, 0), 0));
        }
    }

    // Pop: ray from the crosshair to the first voxel, up to the component an entity links, and
    // from there to the entity. The pop itself is an event; this system does not know what a pop
    // does.
    if (input.cursorCaptured && input.pressed(projv::MouseButton::Right)) {
        const auto& scene = world.ctx().get<projv::Scene>();
        projv::utils::VoxelPick pick = projv::utils::pickVoxel(scene, camera.position, camera.forward(), 500.0f);
        projv::ComponentHandle h = pick.hit ? pick.component : projv::INVALID_COMPONENT_HANDLE;
        Entity target = projv::NullEntity;
        while (h != projv::INVALID_COMPONENT_HANDLE && target == projv::NullEntity) {
            target = projv::runtime::entityFor(world, h);
            h = scene.components[h].parent;
        }
        if (target != projv::NullEntity && world.all_of<Body>(target) && !world.all_of<Popping>(target)) {
            world.emplace<Popping>(target);
            app.events().send(Popped{target, projv::runtime::bodyPose(world, target).position, world.get<Body>(target).explosive});
        }
    }
    if (input.pressed(projv::Key::C)) {
        for (Entity e : world.view<Body>()) {
            if (!world.all_of<Popping>(e)) {
                world.emplace<Popping>(e);
                app.events().send(Popped{e, projv::runtime::bodyPose(world, e).position, 0.0f});
            }
        }
    }
}

// FixedUpdate: launch from spawners, pull with the tractor beam, and retire whatever has left the
// arena. The engine's "physics" system, also on FixedUpdate, does the simulating; everything here
// is commands to it, which land at the next step.
void spawners(projv::Application& app) {
    World& world = app.world;
    auto& sandbox = world.ctx().get<Sandbox>();
    const float dt = app.time().fixedDelta;

    // Requests are collected and made after the loop, so the loop never walks a storage that
    // spawning is adding to.
    struct Launch { std::string kind; vec3 at, velocity; };
    std::vector<Launch> launches;
    size_t bodies = world.view<Body>().size();
    for (auto [entity, spawner] : world.view<Spawner>().each()) {
        spawner.timer -= dt;
        if (spawner.timer > 0.0f) continue;
        spawner.timer += spawner.interval;
        if (!sandbox.spawners || bodies + launches.size() >= MAX_BODIES) continue;
        vec3 toCentre = glm::normalize(vec3(-spawner.muzzle.x, 0, -spawner.muzzle.z));
        vec3 velocity = toCentre * spawner.speed + vec3(uniform(sandbox, -4, 4), 14.0f, uniform(sandbox, -4, 4));
        launches.push_back({spawner.prefab, spawner.muzzle, velocity});
    }
    for (const Launch& l : launches) {
        if (spawnPrefab(world, l.kind, l.at, l.velocity) != projv::NullEntity) sandbox.spawnerLaunches++;
    }
}

void tractorBeam(projv::Application& app) {
    World& world = app.world;
    if (!world.ctx().get<projv::Input>().down(projv::Key::E)) return;
    const auto& camera = world.ctx().get<Camera>();
    const float dt = app.time().fixedDelta;
    const vec3 beamPoint = camera.position + camera.forward() * 22.0f;
    for (Entity e : world.view<Body>()) {
        projv::runtime::BodyState pose = projv::runtime::bodyPose(world, e);
        vec3 pull = beamPoint - pose.position;
        float distance = glm::length(pull);
        vec3 change = -pose.linearVelocity * 2.5f * dt;   // damped, or everything orbits the beam point
        if (distance > 0.01f) change += pull / distance * std::min(distance, 30.0f) * 6.0f * dt;
        projv::runtime::addVelocity(world, e, change);
    }
}

void titleBar(projv::Application& app) {
    auto& sandbox = app.world.ctx().get<Sandbox>();
    sandbox.titleTimer -= app.time().unscaledDelta;
    if (sandbox.titleTimer > 0.0f) return;
    sandbox.titleTimer = 0.25f;
    const projv::Time& time = app.time();
    std::string speed = time.scale == 0.0f ? "PAUSED" : time.scale < 1.0f ? "slow motion" : "x1";
    std::string title = "ProjectV Sandbox  |  [" + std::to_string(sandbox.selected + 1) + "] " +
                        sandbox.kinds[sandbox.selected] + "  |  bodies " +
                        std::to_string(app.world.view<Body>().size()) + "  |  popped " +
                        std::to_string(sandbox.popped) + "  |  " + speed +
                        (sandbox.lowGravity ? "  |  low gravity" : "") +
                        (sandbox.spawners ? "" : "  |  spawners off") + "  |  " +
                        std::to_string(int(1.0f / std::max(time.unscaledDelta, 1e-4f))) + " fps  |  " +
                        fmt::format("physics {:.2f} ms", projv::runtime::physicsWorld(app.world).stats().lastStepMilliseconds);
    glfwSetWindowTitle(app.world.ctx().get<projv::graphics::RenderInstance>().window, title.c_str());
}

// =========================================================================================
// Self-test: SANDBOX_SELFTEST=<frames> plays by itself and exits
// =========================================================================================
//
// Throws every kind, rains, pops at random and sets bombs off among a crowd, then checks the books:
// every body spawned is either still alive or was popped, and every popped body's voxels are gone
// from the Scene. It exists so a change to spawning, the bridge or the GPU flush can be checked
// for crashes and leaks without anyone at the controls.
struct SelfTest {
    int frames = 0;
    int spawned = 0;
    double frameSeconds = 0.0, gpuSeconds = 0.0, worstFrame = 0.0;
    int timedFrames = 0;
    int measureBodies = -1;   // SANDBOX_MEASURE: a fixed crowd, no actions, just timing
    int churn = 0;            // SANDBOX_CHURN: bodies destroyed and respawned every frame
};

void selfTestSystem(projv::Application& app) {
    World& world = app.world;
    auto* test = world.ctx().find<SelfTest>();
    if (!test) return;
    auto& sandbox = world.ctx().get<Sandbox>();
    int frame = app.frameCount;

    // Frame cost: the whole frame from the unscaled clock, and the GPU's share from bgfx's own
    // timer queries (zero on a backend that does not report them).
    if (frame > 10) {
        const bgfx::Stats* stats = bgfx::getStats();
        test->frameSeconds += app.time().unscaledDelta;
        test->worstFrame = std::max(test->worstFrame, double(app.time().unscaledDelta));
        if (stats && stats->gpuTimerFreq > 0) {
            test->gpuSeconds += double(stats->gpuTimeEnd - stats->gpuTimeBegin) / double(stats->gpuTimerFreq);
        }
        test->timedFrames++;
    }

    auto spawn = [&](const std::string& kind, vec3 at, vec3 velocity) {
        if (spawnPrefab(world, kind, at, velocity) != projv::NullEntity) test->spawned++;
    };

    // Measurement: a fixed number of balls laid out in rows on the floor, nothing else happening,
    // and the camera where it starts. Separates what the arena costs from what each body costs.
    if (test->measureBodies >= 0) {
        if (frame == 2) {
            sandbox.spawners = false;
            int side = int(std::ceil(std::sqrt(double(std::max(test->measureBodies, 1)))));
            for (int i = 0; i < test->measureBodies; i++) {
                vec3 at(-45.0f + 90.0f * float(i % side) / float(std::max(side - 1, 1)), 1.5f,
                        -45.0f + 90.0f * float(i / side) / float(std::max(side - 1, 1)));
                spawn("ball", at, vec3(0));
            }
            // A still scene: no spin, no time. Two runs then draw exactly the same image whatever
            // their frame timings, so SANDBOX_CAPTURE images can be compared pixel for pixel.
            // SANDBOX_MEASURE_MOVING=1 keeps the bodies spinning instead: every Transform changes
            // every frame, so the bridge rewrites every header and the BVH is rebuilt each flush --
            // what playing costs.
            if (!std::getenv("SANDBOX_MEASURE_MOVING")) app.time().scale = 0.0f;
        }
        // Moving: every body spins in place about the vertical, so it never settles or rolls away.
        if (std::getenv("SANDBOX_MEASURE_MOVING") && frame > 2)
            for (Entity e : world.view<Body>()) projv::runtime::setVelocity(world, e, vec3(0), vec3(0, 3, 0));
        // Churn: the oldest bodies destroyed and as many new ones thrown in, every frame -- what
        // debris, fragments and projectiles do to the tables. With rows recycled, every table stays
        // the size of what is alive, and so does the frame.
        if (test->churn > 0 && frame > 2) {
            std::vector<Entity> oldest;
            for (auto [entity, body] : world.view<Body>().each()) oldest.push_back(entity);
            for (int k = 0; k < test->churn && k < int(oldest.size()); k++) world.destroy(oldest[size_t(k)]);
            for (int k = 0; k < test->churn; k++) {
                spawn("ball", vec3(-45.0f + float((frame * 7 + k * 13) % 90), 1.5f, -45.0f + float((k * 31) % 90)), vec3(0));
            }
        }
        if (frame == test->frames - 5) {
            if (const char* capture = std::getenv("SANDBOX_CAPTURE")) bgfx::requestScreenShot(BGFX_INVALID_HANDLE, capture);
        }
        if (frame < 60) { test->frameSeconds = test->gpuSeconds = test->worstFrame = 0.0; test->timedFrames = 0; }
        if (frame < test->frames) return;
        const auto& scene = world.ctx().get<projv::Scene>();
        const auto& gpuData = world.ctx().get<projv::GPUData>();
        size_t paletteEntries = 0;
        for (const projv::ComponentRecord& record : scene.components) paletteEntries += record.materialPalette.size();
        projv::core::info("SANDBOXMEASURE: {} bodies, {} loose chunks, {} grids | frames average {:.1f} ms "
                          "(worst {:.1f} ms), GPU average {:.1f} ms, physics step {:.2f} ms ({} threads)",
                          world.view<Body>().size(), scene.looseChunks.size(), scene.grids.size(),
                          1000.0 * test->frameSeconds / std::max(test->timedFrames, 1), 1000.0 * test->worstFrame,
                          1000.0 * test->gpuSeconds / std::max(test->timedFrames, 1),
                          projv::runtime::physicsWorld(world).stats().lastStepMilliseconds,
                          projv::runtime::physicsWorld(world).stats().threads);
        projv::core::info("SANDBOXMEASURE: tables after {} spawns: {} component rows, {} chunk rows, {} blobs, "
                          "{} header rows, {} palette entries", test->spawned, scene.components.size(), scene.chunks.size(),
                          scene.geometryPool.size(), gpuData.headerCapacity, paletteEntries);
        app.closeRequested = true;
        return;
    }
    if (frame % 4 == 0 && frame < test->frames - 120) {
        const std::string& kind = sandbox.kinds[(frame / 4) % 3];
        spawn(kind, vec3(uniform(sandbox, -20, 20), 30, uniform(sandbox, -20, 20)),
              vec3(uniform(sandbox, -10, 10), 0, uniform(sandbox, -10, 10)));
    }
    if (frame == 60) for (int i = 0; i < 20; i++) spawn("bomb", vec3(uniform(sandbox, -8, 8), 20 + i, uniform(sandbox, -8, 8)), vec3(0));
    // Pop a random body now and then, and the first bomb found once the crowd has landed.
    if (frame % 9 == 0 || frame == 200) {
        std::vector<Entity> candidates;
        for (Entity e : world.view<Body>()) {
            if (world.all_of<Popping>(e)) continue;
            if (frame == 200 && world.get<Body>(e).explosive <= 0.0f) continue;
            candidates.push_back(e);
        }
        if (!candidates.empty()) {
            Entity e = candidates[std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(sandbox.random)];
            world.emplace<Popping>(e);
            app.events().send(Popped{e, projv::runtime::bodyPose(world, e).position, world.get<Body>(e).explosive});
        }
    }
    if (frame == test->frames - 60) app.time().scale = 0.2f;   // slow motion, for the interpolation
    if (frame < test->frames) return;

    const auto& scene = world.ctx().get<projv::Scene>();
    size_t alive = world.view<Body>().size();
    size_t liveRoots = 0;
    for (projv::ComponentHandle h = 0; h < scene.components.size(); h++) {
        if (scene.components[h].parent == projv::INVALID_COMPONENT_HANDLE && projv::utils::isComponentAlive(scene, h)) liveRoots++;
    }
    size_t arenaRoots = 3;   // the arena and two pylons
    bool books = int(alive) + sandbox.popped + sandbox.lost == test->spawned + sandbox.spawnerLaunches;
    bool voxels = liveRoots == arenaRoots + alive;
    // Physics: every body alive has its simulation body (none were refused), no step dropped
    // contacts, and nothing went through the floor.
    size_t simulated = 0;
    for (Entity e : world.view<Body>()) simulated += projv::runtime::hasBody(world, e);
    const auto& stats = projv::runtime::physicsWorld(world).stats();
    bool physics = simulated == alive && stats.stepsWithDroppedContacts == 0 && stats.refusedBodies == 0 &&
                   sandbox.fellThrough == 0;
    // The whole session's physics, replayed in a fresh world: every step must come out the same.
    projv::runtime::PhysicsLog log = projv::runtime::physicsWorld(world).recording();
    projv::runtime::ReplayResult replay = projv::runtime::PhysicsWorld::replay(log);
    if (const char* path = std::getenv("SANDBOX_RECORD")) {
        if (log.saveToFile(path)) projv::core::info("SANDBOXTEST: physics recording written to {}", path);
    }
    projv::core::info("SANDBOXTEST: replay of {} steps ({} KB recorded): {}{}", replay.stepsReplayed,
                      log.bytes.size() / 1024, replay.matched ? "identical" : "DIVERGED: ", replay.problem);
    bool pass = books && voxels && physics && replay.matched;
    projv::core::info("SANDBOXTEST: spawned {} (+{} by spawners), popped {}, lost over the walls {}, alive {}, "
                      "live root components {} (expected {}) | books {} | voxels {}", test->spawned,
                      sandbox.spawnerLaunches, sandbox.popped, sandbox.lost, alive, liveRoots, arenaRoots + alive,
                      books ? "ok" : "WRONG", voxels ? "ok" : "WRONG");
    projv::core::info("SANDBOXTEST: physics: {} of {} bodies simulated, {} steps, {} with dropped contacts, {} refused, "
                      "{} fell through the floor | {} | {}", simulated, alive, stats.steps,
                      stats.stepsWithDroppedContacts, stats.refusedBodies, sandbox.fellThrough,
                      physics ? "ok" : "WRONG", pass ? "PASS" : "FAIL");
    if (test->timedFrames > 0) {
        projv::core::info("SANDBOXTEST: frames average {:.1f} ms (worst {:.1f} ms), GPU average {:.1f} ms",
                          1000.0 * test->frameSeconds / test->timedFrames, 1000.0 * test->worstFrame,
                          1000.0 * test->gpuSeconds / test->timedFrames);
    }
    app.closeRequested = true;
}

// =========================================================================================
// What a pop does: three handlers, independent of each other and of whoever sent it
// =========================================================================================

void connectPopHandlers(projv::Application& app) {
    World& world = app.world;
    projv::Events& events = app.events();

    // 1. A shockwave pushes everything near it away -- and a bomb's pops everything inside its
    //    radius. Those pops are *sent*, so they arrive next frame and pop their own neighbours the
    //    frame after: a chain reaction moves outward one ring per frame.
    events.on<Popped>([&world, &events](const Popped& p) {
        float reach = p.explosive > 0.0f ? p.explosive * 1.6f : 7.0f;
        float strength = p.explosive > 0.0f ? 55.0f : 18.0f;
        for (auto [entity, b] : world.view<Body>().each()) {
            if (entity == p.entity) continue;
            vec3 at = projv::runtime::bodyPose(world, entity).position;
            vec3 d = at - p.position;
            float distance = glm::length(d);
            if (distance > reach || distance < 1e-4f) continue;
            float falloff = 1.0f - distance / reach;
            // An impulse, so a heavy crate is shoved less than a light ball.
            projv::runtime::addImpulse(world, entity, (d / distance * strength + vec3(0, strength * 0.5f, 0)) * falloff);
            if (p.explosive > 0.0f && distance < p.explosive && !world.all_of<Popping>(entity)) {
                world.emplace<Popping>(entity);
                events.send(Popped{entity, at, b.explosive});
            }
        }
    });

    // 2. The score.
    events.on<Popped>([&world](const Popped&) { world.ctx().get<Sandbox>().popped++; });

    // 3. Removal, last. Destroying the entity unlinks it, and OnUnlink::Destroy deletes its voxels.
    events.on<Popped>([&world](const Popped& p) {
        if (world.valid(p.entity)) world.destroy(p.entity);
    });
}

// =========================================================================================
// Stages
// =========================================================================================

void startup(projv::Application& app) {
    World& world = app.world;
    const fs::path here = projv::core::executableDirectory();

    auto& renderInstance = world.ctx().emplace<projv::graphics::RenderInstance>();
    renderInstance.initialize(1600, 900, "ProjectV Sandbox");
    projv::graphics::installPlatform(app, renderInstance);
    projv::graphics::setCursorCaptured(app, renderInstance, true);

    world.ctx().emplace<Camera>();
    world.ctx().emplace<Sandbox>().prefabs = here / "prefabs";
    auto& scene = world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk((here / "scene").string()));

    projv::runtime::installSceneBridge(app);
    // The world ends below the kill plane: a body thrown over a wall falls past it, and the engine
    // reports it (BodyLeftWorld) and retires it.
    projv::runtime::PhysicsConfig physics;
    physics.settings.worldMin = vec3(-1e4f, KILL_PLANE, -1e4f);
    // Unattended runs record every physics input, and replay it at the end (see the self-test).
    physics.settings.record = std::getenv("SANDBOX_SELFTEST") != nullptr;
    projv::runtime::installPhysics(app, physics);
    projv::runtime::setGravity(world, vec3(0, GRAVITY, 0));
    projv::runtime::registerComponent<Body>(world);
    projv::runtime::registerComponent<Spawner>(world);
    world.on_construct<Spawner>().connect<&setUpSpawner>();
    projv::runtime::spawnEntities(world);   // the arena: its spawners come to life here
    connectPopHandlers(app);
    // Counted as lost. One that left from *inside* the walls went through the floor, which must
    // never happen: the self-test fails on it.
    app.events().on<projv::BodyLeftWorld>([&world](const projv::BodyLeftWorld& e) {
        auto& sandbox = world.ctx().get<Sandbox>();
        sandbox.lost++;
        if (std::abs(e.position.x) < ARENA_HALF - 1.0f && std::abs(e.position.z) < ARENA_HALF - 1.0f) {
            sandbox.fellThrough++;
            projv::core::error("sandbox: a body fell through the floor at ({:.1f}, {:.1f})", e.position.x, e.position.z);
        }
    });

    projv::RendererSpecification specification =
        projv::graphics::loadRendererSpecification((here / "sandboxRenderer").string() + "/");
    renderInstance.addRendererSpecification(1, specification);
    bgfx::ShaderHandle vertexShader =
        projv::graphics::loadShader((here / "sandboxRenderer/sandboxShaders/vs_quad.bin").string());
    renderInstance.setActiveRenderer(projv::graphics::constructRendererSpecification(
        renderInstance.getRendererSpecification(1), vertexShader));
    world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));

    projv::core::info("Sandbox: {} spawner(s) found in the arena. Left click throws, right click pops.",
                      world.view<Spawner>().size());
    if (const char* frames = std::getenv("SANDBOX_SELFTEST")) world.ctx().emplace<SelfTest>().frames = std::max(300, std::atoi(frames));
    if (const char* bodies = std::getenv("SANDBOX_MEASURE")) {
        SelfTest& test = world.ctx().emplace<SelfTest>();
        test.frames = 240;
        test.measureBodies = std::max(0, std::atoi(bodies));
        if (const char* churn = std::getenv("SANDBOX_CHURN")) {
            test.churn = std::max(0, std::atoi(churn));
            test.frames = 1200;   // long enough for anything that grows to show
        }
    }
    // Unattended runs leave the pointer alone.
    if (world.ctx().contains<SelfTest>()) projv::graphics::setCursorCaptured(app, renderInstance, false);
}

void render(projv::Application& app) {
    World& world = app.world;
    auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
    auto& scene = world.ctx().get<projv::Scene>();
    auto& gpuData = world.ctx().get<projv::GPUData>();
    const auto& camera = world.ctx().get<Camera>();

    // New prefabs' geometry, moved bodies' headers, and deleted components, all in one flush.
    projv::graphics::flushSceneUpdates(scene, gpuData);

    vec3 position = camera.position;
    vec3 direction = camera.forward();
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

// =========================================================================================
// The assets, as code (only for --write-assets)
// =========================================================================================

constexpr float VOXEL = 0.5f;

// A component filled by `colourAt` over a box of `size` voxels.
//
// **Always started as a 16-voxel chunk, whatever the size.** Anything larger overflows it, and the
// edit queue turns it into a grid of 16-voxel cells (convertChunkToGrid). That is the point: a
// chunk's bounds are a cube, and the shader marches every loose chunk whose cube a ray enters, so an
// arena stored as one 256-voxel floor chunk and four wall chunks put 128-unit cubes over a 1-unit
// slab, and cost 65 ms a frame on the GPU before anything was thrown. A grid is bounded by the cells
// it actually has and walked by a DDA. Continuous geometry on one lattice is one grid.
projv::ComponentHandle shape(projv::Scene& scene, const char* name, projv::ComponentHandle parent,
                             projv::core::ivec3 size, vec3 position, auto colourAt) {
    using namespace projv;
    ComponentHandle h = utils::addComponent(scene, ComponentKind::Chunk, name, parent, 16, VOXEL);
    utils::setComponentTransform(scene, h, position, quat(1, 0, 0, 0), 1.0f);
    std::vector<PendingVoxelOp> voxels;
    for (int z = 0; z < size.z; z++) for (int y = 0; y < size.y; y++) for (int x = 0; x < size.x; x++) {
        uint32_t colour = colourAt(x, y, z);
        if (colour) voxels.push_back({true, core::ivec3(x, y, z), colour});
    }
    utils::queueVoxelAdd(scene, h, voxels);
    return h;
}

// Saves entities made in code into `folder`/entities.json, the way a tool would: a throwaway
// Application with the folder's Scene loaded, the bridge, and the sandbox's components registered.
template<typename Make>
bool writeEntities(const fs::path& folder, Make make) {
    projv::Application app;
    auto& scene = app.world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(folder.string()));
    projv::runtime::installSceneBridge(app);
    projv::runtime::installPhysics(app);   // registers physics.rigidbody and physics.static
    projv::runtime::registerComponent<Body>(app.world);
    projv::runtime::registerComponent<Spawner>(app.world);
    make(app.world, scene);
    return projv::runtime::saveEntities(app.world, projv::INVALID_COMPONENT_HANDLE, folder.string());
}

bool writePrefab(const fs::path& folder, const Body& body, const projv::RigidBody& rigid, auto colourAt, int diameter) {
    projv::Scene scene;
    float half = diameter * 0.5f * VOXEL;
    shape(scene, "shape", projv::INVALID_COMPONENT_HANDLE, projv::core::ivec3(diameter), vec3(-half), colourAt);
    projv::utils::updateScene(scene);
    if (!projv::utils::saveComposeToDisk(scene, projv::INVALID_COMPONENT_HANDLE, folder.string())) return false;
    // The prefab's own entity: linked to "document", the node the folder becomes when grafted.
    return writeEntities(folder, [&](World& world, projv::Scene&) {
        Entity e = world.create();
        world.emplace<projv::runtime::Authored>(e, projv::runtime::Authored{
            projv::INVALID_COMPONENT_HANDLE, folder.filename().string(), projv::runtime::Authored::Link::Document});
        world.emplace<Body>(e, body);
        world.emplace<projv::RigidBody>(e, rigid);
    });
}

projv::RigidBody rigidOf(projv::RigidBody::Shape shape, float mass, float restitution) {
    projv::RigidBody r;
    r.shape = shape; r.mass = mass; r.restitution = restitution;
    return r;
}

int writeAssets(const fs::path& dir) {
    using projv::packRGB10;
    bool ok = true;

    // Prefabs: a striped ball, a crate, a bomb with a fuse band. Their physics is the RigidBody on
    // the prefab's own entity: the ball collides as a sphere, the others as their voxels.
    auto sphere = [](int n, auto inside) {
        return [n, inside](int x, int y, int z) -> uint32_t {
            vec3 d = vec3(x, y, z) + vec3(0.5f) - vec3(n * 0.5f);
            return glm::length(d) <= n * 0.5f ? inside(d) : 0u;
        };
    };
    using Shape = projv::RigidBody::Shape;
    ok &= writePrefab(dir / "prefabs/ball", Body{}, rigidOf(Shape::Sphere, 1.0f, 0.8f),
        sphere(6, [](vec3 d) { return std::fmod(std::atan2(d.z, d.x) + 3.15f, 1.05f) < 0.52f
                                   ? packRGB10(0.2f, 0.55f, 1.0f) : packRGB10(0.95f, 0.95f, 1.0f); }), 6);
    ok &= writePrefab(dir / "prefabs/crate", Body{}, rigidOf(Shape::Voxels, 2.5f, 0.25f),
        [](int x, int y, int z) -> uint32_t {
            int edges = (x == 0 || x == 5) + (y == 0 || y == 5) + (z == 0 || z == 5);
            return edges >= 2 ? packRGB10(0.45f, 0.28f, 0.12f) : packRGB10(0.78f, 0.56f, 0.3f);
        }, 6);
    ok &= writePrefab(dir / "prefabs/bomb", Body{9.0f}, rigidOf(Shape::Voxels, 1.5f, 0.4f),
        sphere(5, [](vec3 d) { return std::abs(d.y) < 0.6f ? packRGB10(1.0f, 0.85f, 0.1f) : packRGB10(0.85f, 0.1f, 0.08f); }), 5);

    // The arena: a checkered floor whose top is y = 0 and a low wall round its edge -- one continuous
    // object on one lattice, so one component, which the edit queue makes one grid. The pylons are
    // separate components only because each has its own entity, with its own Spawner.
    projv::Scene arena;
    shape(arena, "Arena", projv::INVALID_COMPONENT_HANDLE, {256, 14, 256}, vec3(-64, -1, -64),
          [](int x, int y, int z) -> uint32_t {
              if (y < 2) return ((x / 16) + (z / 16)) % 2 ? packRGB10(0.34f, 0.36f, 0.4f) : packRGB10(0.26f, 0.28f, 0.31f);
              bool edge = x < 4 || x >= 252 || z < 4 || z >= 252;
              if (!edge) return 0u;
              return y > 11 ? packRGB10(0.9f, 0.55f, 0.2f) : packRGB10(0.5f, 0.52f, 0.56f);
          });
    auto pylon = [](int, int y, int) { return y > 21 ? packRGB10(0.3f, 1.0f, 0.6f) : packRGB10(0.2f, 0.22f, 0.26f); };
    shape(arena, "Pylon A", projv::INVALID_COMPONENT_HANDLE, {8, 24, 8}, vec3(-42, 0, -42), pylon);
    shape(arena, "Pylon B", projv::INVALID_COMPONENT_HANDLE, {8, 24, 8}, vec3(38, 0, 38), pylon);
    projv::utils::updateScene(arena);
    ok &= projv::utils::saveComposeToDisk(arena, projv::INVALID_COMPONENT_HANDLE, (dir / "scene").string());

    // The arena's entity, which makes it solid, and each pylon's, with its Spawner (and solid too).
    ok &= writeEntities(dir / "scene", [](World& world, projv::Scene& scene) {
        auto entityFor = [&](const char* name) {
            for (projv::ComponentHandle h = 0; h < scene.components.size(); h++) {
                if (scene.components[h].name != name) continue;
                Entity e = projv::runtime::spawnComponent(world, h);
                world.emplace<projv::runtime::Authored>(e, projv::runtime::Authored{projv::INVALID_COMPONENT_HANDLE, name});
                world.emplace<projv::StaticCollider>(e);
                return e;
            }
            return Entity(projv::NullEntity);
        };
        entityFor("Arena");
        auto pylonEntity = [&](const char* name, const char* prefab, float interval, float speed) {
            Entity e = entityFor(name);
            if (e == projv::NullEntity) return;
            Spawner spawner;
            spawner.prefab = prefab; spawner.interval = interval; spawner.speed = speed;
            world.emplace<Spawner>(e, spawner);
        };
        pylonEntity("Pylon A", "ball", 1.1f, 24.0f);
        pylonEntity("Pylon B", "crate", 1.7f, 20.0f);
    });

    projv::core::info("Wrote the sandbox assets to {}{}", dir.string(), ok ? "" : " (with errors)");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--write-assets") == 0) return writeAssets(argv[2]);

    projv::Application app;
    app.addSystem(projv::Stage::Startup,     "startup",   startup);
    app.addSystem(projv::Stage::Update,      "controls",  controls);
    app.addSystem(projv::Stage::FixedUpdate, "spawners",  spawners);
    app.addSystem(projv::Stage::FixedUpdate, "tractor beam", tractorBeam);
    app.addSystem(projv::Stage::Update,      "title bar", titleBar);
    app.addSystem(projv::Stage::Update,      "self-test", selfTestSystem);
    app.addSystem(projv::Stage::Render,      "render",    render);
    app.addSystem(projv::Stage::Shutdown,    "shutdown",  shutdown);
    app.run();
    return 0;
}
