// ProjectV Sandbox
//
// A physics toy built entirely out of the runtime: throw things into an arena, watch them bounce,
// pop them, set off chain reactions. Every system the engine now has is load-bearing here:
//
//   Prefabs are compose folders.     prefabs/ball, prefabs/crate, prefabs/bomb each hold their
//                                    voxels and, as a folder-level attachment, their physics
//                                    ("sandbox.body") -- open prefabs/bomb/compose.json.
//   Spawning is the Scene bridge.    instantiateComposeInto grafts a prefab into the live Scene;
//                                    spawnComponent links it to a new entity and runs the spawn
//                                    handler that turns "sandbox.body" into a Body component.
//   The arena is data too.           scene/compose.json: a floor, walls, and two pylons whose
//                                    "sandbox.spawner" attachment makes them launch balls on their
//                                    own. No line of this file names a pylon.
//   Physics is FixedUpdate.          60 steps a second whatever the frame rate; Update draws each
//                                    body between its last two steps by Time::fixedAlpha, so slow
//                                    motion (T) is smooth rather than steppy.
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
#include "runtime/scene_bridge.h"
#include "utils/attachments.h"
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
constexpr float FLOOR_TOP = 0.0f;
constexpr size_t MAX_BODIES = 300;     // spawners hold off above this

// =========================================================================================
// Attachments: what the assets say
// =========================================================================================

// On a prefab folder (document scope): how the thing behaves when it exists.
struct BodyDef {
    float radius = 1.0f;
    float restitution = 0.5f;    // bounciness, 0..1
    float mass = 1.0f;
    float explosive = 0.0f;      // > 0: popping it pops everything within this radius
};

// On an arena component: launch a prefab every `interval` seconds.
struct SpawnerDef {
    std::string prefab;
    float interval = 2.0f;
    float speed = 20.0f;
};

double tidy(float v) { return std::round(double(v) * 1e4) / 1e4; }

}  // namespace

template<> struct projv::utils::AttachmentTraits<BodyDef> {
    static constexpr const char* key = "sandbox.body";
    static constexpr uint32_t version = 1;
    static constexpr projv::OnDuplicate onDuplicate = projv::OnDuplicate::Copy;
    static nlohmann::json save(const BodyDef& b) {
        nlohmann::json j{{"radius", tidy(b.radius)}, {"restitution", tidy(b.restitution)}, {"mass", tidy(b.mass)}};
        if (b.explosive > 0.0f) j["explosive"] = tidy(b.explosive);
        return j;
    }
    static std::optional<BodyDef> load(const nlohmann::json& j, uint32_t) {
        return BodyDef{j.value("radius", 1.0f), j.value("restitution", 0.5f), j.value("mass", 1.0f),
                       j.value("explosive", 0.0f)};
    }
};

template<> struct projv::utils::AttachmentTraits<SpawnerDef> {
    static constexpr const char* key = "sandbox.spawner";
    static constexpr uint32_t version = 1;
    static constexpr projv::OnDuplicate onDuplicate = projv::OnDuplicate::Copy;
    static nlohmann::json save(const SpawnerDef& s) {
        return nlohmann::json{{"prefab", s.prefab}, {"interval", tidy(s.interval)}, {"speed", tidy(s.speed)}};
    }
    static std::optional<SpawnerDef> load(const nlohmann::json& j, uint32_t) {
        return SpawnerDef{j.at("prefab").get<std::string>(), j.value("interval", 2.0f), j.value("speed", 20.0f)};
    }
};

namespace {

// =========================================================================================
// ECS components and events: what the running program is doing
// =========================================================================================

struct Body {
    vec3 position{0.0f}, previous{0.0f}, velocity{0.0f}, spin{0.0f};
    quat orientation{1, 0, 0, 0}, previousOrientation{1, 0, 0, 0};
    float radius = 1.0f, restitution = 0.5f, inverseMass = 1.0f, explosive = 0.0f;
};

struct Spawner {
    std::string prefab;
    float interval = 2.0f, speed = 20.0f, timer = 0.0f;
    vec3 muzzle{0.0f};
};

struct Popping {};   // a Popped event is already on its way for this entity

struct Popped {
    Entity entity;
    vec3 position;
    float radius;
    float explosive;
};

// The sandbox's own state, in world.ctx().
struct Sandbox {
    fs::path prefabs;
    std::vector<std::string> kinds{"ball", "crate", "bomb"};
    int selected = 0;
    int popped = 0;
    int spawnerLaunches = 0;
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

// The spawn handler for "sandbox.body": a prefab that says how it behaves becomes a Body.
void spawnBody(World& world, Entity entity, const BodyDef& def) {
    const projv::Transform& t = world.get<projv::Transform>(entity);
    Body body;
    body.position = body.previous = t.position;
    body.orientation = body.previousOrientation = t.rotation;
    body.radius = def.radius;
    body.restitution = def.restitution;
    body.inverseMass = def.mass > 0.0f ? 1.0f / def.mass : 0.0f;
    body.explosive = def.explosive;
    world.emplace<Body>(entity, body);
}

// The spawn handler for "sandbox.spawner": an arena component that launches things.
void spawnSpawner(World& world, Entity entity, const SpawnerDef& def) {
    const projv::Transform& t = world.get<projv::Transform>(entity);
    // A pylon's origin is its chunk's corner; the muzzle is the middle of its top.
    world.emplace<Spawner>(entity, def.prefab, def.interval, def.speed, def.interval * 0.5f,
                           t.position + vec3(2.0f, 13.0f, 2.0f));
}

// Graft a prefab folder into the live Scene and give it an entity. OnUnlink::Destroy: when the
// entity goes, so do its voxels.
Entity spawnPrefab(World& world, const std::string& kind, vec3 position, vec3 velocity) {
    auto& scene = world.ctx().get<projv::Scene>();
    auto& sandbox = world.ctx().get<Sandbox>();
    projv::ComponentHandle root = projv::utils::instantiateComposeInto(
        scene, (sandbox.prefabs / kind).string(), projv::INVALID_COMPONENT_HANDLE, position);
    if (root == projv::INVALID_COMPONENT_HANDLE) return projv::NullEntity;
    Entity entity = projv::runtime::spawnComponent(world, root, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    if (Body* body = world.try_get<Body>(entity)) {
        body->velocity = velocity;
        body->spin = vec3(uniform(sandbox, -3, 3), uniform(sandbox, -3, 3), uniform(sandbox, -3, 3));
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
    if (input.pressed(projv::Key::G)) sandbox.lowGravity = !sandbox.lowGravity;
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
            const Body& body = world.get<Body>(target);
            world.emplace<Popping>(target);
            app.events().send(Popped{target, body.position, body.radius, body.explosive});
        }
    }
    if (input.pressed(projv::Key::C)) {
        for (Entity e : world.view<Body>()) {
            if (!world.all_of<Popping>(e)) { world.emplace<Popping>(e); app.events().send(Popped{e, world.get<Body>(e).position, 0.0f, 0.0f}); }
        }
    }
}

// FixedUpdate: launch from spawners, then integrate and collide. Every step is exactly fixedDelta.
void physics(projv::Application& app) {
    World& world = app.world;
    auto& sandbox = world.ctx().get<Sandbox>();
    const auto& input = world.ctx().get<projv::Input>();
    const auto& camera = world.ctx().get<Camera>();
    const float dt = app.time().fixedDelta;
    const float gravity = sandbox.lowGravity ? -5.0f : -28.0f;

    // Spawners: requests are collected and made after the loop, so the loop never walks a storage
    // that spawning is adding to.
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

    // Integrate.
    const bool tractor = input.down(projv::Key::E);
    const vec3 beamPoint = camera.position + camera.forward() * 22.0f;
    auto view = world.view<Body>();
    for (auto [entity, b] : view.each()) {
        b.previous = b.position;
        b.previousOrientation = b.orientation;
        b.velocity.y += gravity * dt;
        if (tractor) {
            vec3 pull = beamPoint - b.position;
            float distance = glm::length(pull);
            if (distance > 0.01f) b.velocity += pull / distance * std::min(distance, 30.0f) * 6.0f * dt;
            b.velocity *= 1.0f - 2.5f * dt;   // damped, or everything orbits the beam point forever
        }
        b.position += b.velocity * dt;

        // The floor and the walls.
        if (b.position.y - b.radius < FLOOR_TOP) {
            b.position.y = FLOOR_TOP + b.radius;
            if (b.velocity.y < 0.0f) b.velocity.y = -b.velocity.y * b.restitution;
            b.velocity.x *= 1.0f - 1.5f * dt;   // rolling friction
            b.velocity.z *= 1.0f - 1.5f * dt;
            // Roll with the ground rather than tumble freely.
            vec3 rolling = glm::cross(vec3(0, 1, 0), b.velocity) / b.radius;
            b.spin = glm::mix(b.spin, rolling, 0.25f);
        }
        for (int axis : {0, 2}) {
            float limit = ARENA_HALF - b.radius;
            if (b.position[axis] > limit)  { b.position[axis] = limit;  if (b.velocity[axis] > 0) b.velocity[axis] *= -b.restitution; }
            if (b.position[axis] < -limit) { b.position[axis] = -limit; if (b.velocity[axis] < 0) b.velocity[axis] *= -b.restitution; }
        }

        float angle = glm::length(b.spin) * dt;
        if (angle > 1e-5f) b.orientation = glm::normalize(glm::angleAxis(angle, glm::normalize(b.spin)) * b.orientation);
    }

    // Collide every pair. A few hundred bodies is a few tens of thousands of pairs, which is fine for
    // a toy; a real broadphase is what the engine's content bounds (promotion #4) are for.
    std::vector<Body*> all;
    for (auto [entity, b] : view.each()) all.push_back(&b);
    for (size_t i = 0; i < all.size(); i++) {
        for (size_t j = i + 1; j < all.size(); j++) {
            Body& a = *all[i];
            Body& c = *all[j];
            vec3 d = c.position - a.position;
            float reach = a.radius + c.radius;
            float distanceSquared = glm::dot(d, d);
            if (distanceSquared >= reach * reach || distanceSquared < 1e-8f) continue;
            float distance = std::sqrt(distanceSquared);
            vec3 n = d / distance;
            float totalInverse = a.inverseMass + c.inverseMass;
            if (totalInverse <= 0.0f) continue;
            float overlap = reach - distance;
            a.position -= n * overlap * (a.inverseMass / totalInverse);
            c.position += n * overlap * (c.inverseMass / totalInverse);
            float approaching = glm::dot(c.velocity - a.velocity, n);
            if (approaching < 0.0f) {
                float e = std::min(a.restitution, c.restitution);
                float impulse = -(1.0f + e) * approaching / totalInverse;
                a.velocity -= n * impulse * a.inverseMass;
                c.velocity += n * impulse * c.inverseMass;
            }
        }
    }
}

// Update: draw each body where it is *between* its last two fixed steps. This is what keeps a
// 60 Hz simulation smooth on a faster display, and slow motion smooth at all.
void present(projv::Application& app) {
    World& world = app.world;
    const float alpha = app.time().fixedAlpha;
    for (auto [entity, b] : world.view<Body>().each()) {
        vec3 position = glm::mix(b.previous, b.position, alpha);
        quat rotation = glm::slerp(b.previousOrientation, b.orientation, alpha);
        world.patch<projv::Transform>(entity, [&](projv::Transform& t) {
            t.position = position;
            t.rotation = rotation;
        });
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
                        std::to_string(int(1.0f / std::max(time.unscaledDelta, 1e-4f))) + " fps";
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
        }
        if (frame < 60) { test->frameSeconds = test->gpuSeconds = test->worstFrame = 0.0; test->timedFrames = 0; }
        if (frame < test->frames) return;
        const auto& scene = world.ctx().get<projv::Scene>();
        projv::core::info("SANDBOXMEASURE: {} bodies, {} loose chunks, {} grids | frames average {:.1f} ms "
                          "(worst {:.1f} ms), GPU average {:.1f} ms", world.view<Body>().size(),
                          scene.looseChunks.size(), scene.grids.size(),
                          1000.0 * test->frameSeconds / std::max(test->timedFrames, 1), 1000.0 * test->worstFrame,
                          1000.0 * test->gpuSeconds / std::max(test->timedFrames, 1));
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
            const Body& b = world.get<Body>(e);
            world.emplace<Popping>(e);
            app.events().send(Popped{e, b.position, b.radius, b.explosive});
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
    bool books = int(alive) + sandbox.popped == test->spawned + sandbox.spawnerLaunches;
    bool voxels = liveRoots == arenaRoots + alive;
    projv::core::info("SANDBOXTEST: spawned {} (+{} by spawners), popped {}, alive {}, live root components {} "
                      "(expected {}) | books {} | voxels {} | {}", test->spawned, sandbox.spawnerLaunches,
                      sandbox.popped, alive, liveRoots, arenaRoots + alive, books ? "ok" : "WRONG",
                      voxels ? "ok" : "WRONG", books && voxels ? "PASS" : "FAIL");
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
            vec3 d = b.position - p.position;
            float distance = glm::length(d);
            if (distance > reach || distance < 1e-4f) continue;
            float falloff = 1.0f - distance / reach;
            b.velocity += (d / distance * strength + vec3(0, strength * 0.5f, 0)) * falloff * b.inverseMass;
            if (p.explosive > 0.0f && distance < p.explosive && !world.all_of<Popping>(entity)) {
                world.emplace<Popping>(entity);
                events.send(Popped{entity, b.position, b.radius, b.explosive});
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
    projv::runtime::registerDocumentSpawnHandler<BodyDef>(world, spawnBody);
    projv::runtime::registerSpawnHandler<SpawnerDef>(world, spawnSpawner);
    projv::runtime::spawnFromCompose(world);   // the arena: its spawners come to life here
    connectPopHandlers(app);

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

bool writePrefab(const fs::path& folder, const BodyDef& def, auto colourAt, int diameter) {
    projv::Scene scene;
    float half = diameter * 0.5f * VOXEL;
    shape(scene, "shape", projv::INVALID_COMPONENT_HANDLE, projv::core::ivec3(diameter), vec3(-half), colourAt);
    projv::utils::setAttachment(scene, projv::INVALID_COMPONENT_HANDLE, def, projv::AttachmentScope::Document);
    projv::utils::updateScene(scene);
    return projv::utils::saveComposeToDisk(scene, projv::INVALID_COMPONENT_HANDLE, folder.string());
}

int writeAssets(const fs::path& dir) {
    using projv::packRGB10;
    bool ok = true;

    // Prefabs: a striped ball, a crate, a bomb with a fuse band. Their physics is the folder's
    // own attachment.
    auto sphere = [](int n, auto inside) {
        return [n, inside](int x, int y, int z) -> uint32_t {
            vec3 d = vec3(x, y, z) + vec3(0.5f) - vec3(n * 0.5f);
            return glm::length(d) <= n * 0.5f ? inside(d) : 0u;
        };
    };
    ok &= writePrefab(dir / "prefabs/ball", BodyDef{1.5f, 0.8f, 1.0f, 0.0f},
        sphere(6, [](vec3 d) { return std::fmod(std::atan2(d.z, d.x) + 3.15f, 1.05f) < 0.52f
                                   ? packRGB10(0.2f, 0.55f, 1.0f) : packRGB10(0.95f, 0.95f, 1.0f); }), 6);
    ok &= writePrefab(dir / "prefabs/crate", BodyDef{1.6f, 0.25f, 2.5f, 0.0f},
        [](int x, int y, int z) -> uint32_t {
            int edges = (x == 0 || x == 5) + (y == 0 || y == 5) + (z == 0 || z == 5);
            return edges >= 2 ? packRGB10(0.45f, 0.28f, 0.12f) : packRGB10(0.78f, 0.56f, 0.3f);
        }, 6);
    ok &= writePrefab(dir / "prefabs/bomb", BodyDef{1.2f, 0.4f, 1.5f, 9.0f},
        sphere(5, [](vec3 d) { return std::abs(d.y) < 0.6f ? packRGB10(1.0f, 0.85f, 0.1f) : packRGB10(0.85f, 0.1f, 0.08f); }), 5);

    // The arena: a checkered floor whose top is y = 0 and a low wall round its edge -- one continuous
    // object on one lattice, so one component, which the edit queue makes one grid. The pylons are
    // separate components only because each carries its own spawner attachment.
    projv::Scene arena;
    shape(arena, "Arena", projv::INVALID_COMPONENT_HANDLE, {256, 14, 256}, vec3(-64, -1, -64),
          [](int x, int y, int z) -> uint32_t {
              if (y < 2) return ((x / 16) + (z / 16)) % 2 ? packRGB10(0.34f, 0.36f, 0.4f) : packRGB10(0.26f, 0.28f, 0.31f);
              bool edge = x < 4 || x >= 252 || z < 4 || z >= 252;
              if (!edge) return 0u;
              return y > 11 ? packRGB10(0.9f, 0.55f, 0.2f) : packRGB10(0.5f, 0.52f, 0.56f);
          });
    auto pylon = [](int, int y, int) { return y > 21 ? packRGB10(0.3f, 1.0f, 0.6f) : packRGB10(0.2f, 0.22f, 0.26f); };
    projv::ComponentHandle a = shape(arena, "Pylon A", projv::INVALID_COMPONENT_HANDLE, {8, 24, 8}, vec3(-42, 0, -42), pylon);
    projv::ComponentHandle b = shape(arena, "Pylon B", projv::INVALID_COMPONENT_HANDLE, {8, 24, 8}, vec3(38, 0, 38), pylon);
    projv::utils::setAttachment(arena, a, SpawnerDef{"ball", 1.1f, 24.0f});
    projv::utils::setAttachment(arena, b, SpawnerDef{"crate", 1.7f, 20.0f});
    projv::utils::updateScene(arena);
    ok &= projv::utils::saveComposeToDisk(arena, projv::INVALID_COMPONENT_HANDLE, (dir / "scene").string());

    projv::core::info("Wrote the sandbox assets to {}{}", dir.string(), ok ? "" : " (with errors)");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--write-assets") == 0) return writeAssets(argv[2]);

    projv::Application app;
    app.addSystem(projv::Stage::Startup,     "startup",   startup);
    app.addSystem(projv::Stage::Update,      "controls",  controls);
    app.addSystem(projv::Stage::FixedUpdate, "physics",   physics);
    app.addSystem(projv::Stage::Update,      "present",   present);
    app.addSystem(projv::Stage::Update,      "title bar", titleBar);
    app.addSystem(projv::Stage::Update,      "self-test", selfTestSystem);
    app.addSystem(projv::Stage::Render,      "render",    render);
    app.addSystem(projv::Stage::Shutdown,    "shutdown",  shutdown);
    app.run();
    return 0;
}
