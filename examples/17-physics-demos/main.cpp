// ProjectV Physics Demos
//
// Four set pieces for the engine's physics (runtime/physics.h), each built from voxels in code:
//
//   1  Pyramid        55 crates stacked ten wide. Space throws a heavy ball into it.
//   2  Wall blast     a staggered wall of 120 voxel bricks. Space sets off a big blast behind it:
//                     the bricks near it are carved and split into fragments, and everything is
//                     thrown outward.
//   3  Ball barrage   balls of five sizes -- the same density, so very different masses -- fired
//                     at a wall of crates, one after another.
//   4  Sweeper        a 12 m beam spinning about its middle while its middle circles, sweeping
//                     through a field of crates and balls. It is kinematic: moved by its Transform,
//                     pushing what it meets, pushed by nothing.
//
// Controls
//   1-4        pick a demo (it is rebuilt from scratch)      R  restart the demo
//   Space      the demo's action                             V  walk (a character) / fly
//   mouse look, W/A/S/D fly, Q/E down/up                     T  slow motion   P  pause
//   Esc        release the cursor (a click recaptures it)
//
//   DEMOS_SELFTEST=1 ./physics_demos    runs every demo unattended and checks it

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
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
#include "runtime/physics.h"
#include "runtime/scene_bridge.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

using projv::ComponentHandle;
using projv::Entity;
using projv::World;
using projv::core::ivec3;
using projv::core::quat;
using projv::core::vec3;
namespace fs = std::filesystem;
namespace runtime = projv::runtime;
namespace utils = projv::utils;

namespace {

constexpr float FLOOR_HALF = 40.0f;   // the floor is 80 m square, its top at y = 0
constexpr float EYE_HEIGHT = 1.6f;
constexpr float WALK_SPEED = 7.0f;

uint32_t rgb(float r, float g, float b) { return projv::packRGB10(r, g, b); }

// =========================================================================================
// Building voxel things
// =========================================================================================

// A component of `size` voxels of `voxel` metres, coloured by `colourAt` (0 = empty), at `position`
// (its corner). Started as a 16-voxel chunk; anything bigger becomes a grid, as continuous
// geometry should.
template <typename ColourAt>
ComponentHandle voxels(projv::Scene& scene, const char* name, ivec3 size, float voxel, vec3 position, ColourAt colourAt,
                       ComponentHandle parent = projv::INVALID_COMPONENT_HANDLE) {
    ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, name, parent, 16, voxel);
    utils::setComponentTransform(scene, h, position, quat(1, 0, 0, 0), 1.0f);
    std::vector<projv::PendingVoxelOp> ops;
    for (int z = 0; z < size.z; z++)
        for (int y = 0; y < size.y; y++)
            for (int x = 0; x < size.x; x++)
                if (uint32_t c = colourAt(x, y, z)) ops.push_back({true, ivec3(x, y, z), c});
    utils::queueVoxelAdd(scene, h, ops);
    utils::updateScene(scene);
    return h;
}

// A thing that moves: placed by `position` and `rotation` at its *centre* -- an asset node there,
// its voxels a chunk offset by minus half their size inside it, the way a prefab is built -- and
// given a RigidBody. Copies of `like` (when given) share its geometry, and so its collision shape.
struct Thing {
    ComponentHandle node = projv::INVALID_COMPONENT_HANDLE;
    Entity entity = projv::NullEntity;
};

template <typename ColourAt>
ComponentHandle prototype(projv::Scene& scene, const char* name, ivec3 size, float voxel, ColourAt colourAt) {
    ComponentHandle node = utils::addComponent(scene, projv::ComponentKind::Asset, name, projv::INVALID_COMPONENT_HANDLE, 4, 1.0f);
    voxels(scene, "voxels", size, voxel, -0.5f * vec3(size) * voxel, colourAt, node);
    return node;
}

Thing place(World& world, ComponentHandle proto, vec3 position, quat rotation, const projv::RigidBody& rigid) {
    auto& scene = world.ctx().get<projv::Scene>();
    Thing t;
    t.node = utils::duplicateComponent(scene, proto);
    utils::setComponentTransform(scene, t.node, position, rotation, 1.0f);
    t.entity = runtime::spawnComponent(world, t.node, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    world.emplace<projv::RigidBody>(t.entity, rigid);
    runtime::assignNetId(world, t.entity);
    return t;
}

projv::RigidBody body(float mass = 0.0f, float restitution = 0.0f, float friction = 0.6f) {
    projv::RigidBody r;
    r.mass = mass;
    r.restitution = restitution;
    r.friction = friction;
    return r;
}

projv::RigidBody ballBody(float density, float restitution) {
    projv::RigidBody r;
    r.shape = projv::RigidBody::Shape::Sphere;
    r.density = density;
    r.restitution = restitution;
    return r;
}

// =========================================================================================
// State
// =========================================================================================

struct Camera {
    vec3 position{0.0f, 6.0f, -22.0f};
    float yaw = 1.5708f, pitch = -0.12f;
    Entity walker = projv::NullEntity;
    vec3 forward() const { return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)}; }
};

struct Demos {
    int current = 0;                       // 1..4
    // Prototypes, made once at startup and never drawn: every crate, brick and ball is a copy.
    ComponentHandle crate, brick, beamProto, balls[5];
    // Demo state.
    float clock = 0.0f;                    // seconds of simulation since the demo began
    float fireTimer = 0.0f;
    Entity beam = projv::NullEntity;
    std::vector<Entity> fired;             // demo 3's balls, oldest first
    std::vector<Entity> watched;           // what a demo's self-test looks at
    vec3 blastAt{0.0f};
    int blastKickIn = 0;                   // fixed steps until a detonation's kick (see detonate)
    int pieces = 0;                        // bodies cut off by blasts (BodySplit)
    int fellThrough = 0;                   // left the world from above the floor: must stay 0
    std::mt19937 random{7};
    float titleTimer = 0.0f;
};

float uniform(Demos& d, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(d.random); }

const char* demoName(int n) {
    switch (n) {
        case 1: return "Pyramid  (Space: throw a ball)";
        case 2: return "Wall blast  (Space: detonate)";
        case 3: return "Ball barrage";
        case 4: return "Sweeper";
    }
    return "";
}

// =========================================================================================
// The demos
// =========================================================================================

void clearDemo(World& world) {
    auto& demos = world.ctx().get<Demos>();
    std::vector<Entity> bodies(world.view<projv::RigidBody>().begin(), world.view<projv::RigidBody>().end());
    for (Entity e : bodies) world.destroy(e);
    auto& camera = world.ctx().get<Camera>();
    if (camera.walker != projv::NullEntity) { world.destroy(camera.walker); camera.walker = projv::NullEntity; }
    demos.beam = projv::NullEntity;
    demos.fired.clear();
    demos.watched.clear();
    demos.clock = demos.fireTimer = 0.0f;
    demos.blastKickIn = 0;
}

void buildPyramid(World& world, Demos& demos, Camera& camera) {
    // Ten crates on the bottom row, nine on the next... a gap of 2 cm between neighbours.
    const float size = 1.0f, gap = 0.02f;
    for (int layer = 0; layer < 10; layer++) {
        int count = 10 - layer;
        for (int i = 0; i < count; i++) {
            float x = (float(i) - float(count - 1) * 0.5f) * (size + gap);
            float y = 0.5f * size + float(layer) * (size + 0.005f);
            Thing t = place(world, demos.crate, vec3(x, y, 0), quat(1, 0, 0, 0), body(40.0f));
            if (layer == 9) demos.watched.push_back(t.entity);   // the top crate
        }
    }
    camera.position = vec3(0, 6, -18);
    camera.yaw = 1.5708f;
    camera.pitch = -0.1f;
}

void buildWall(World& world, Demos& demos, Camera& camera) {
    // Running bond: 12 bricks a course, every other course shifted by half a brick. Each brick is
    // 1 m x 0.5 m x 0.5 m, in 12.5 cm voxels, so a blast can carve it and split it.
    const vec3 brick(1.0f, 0.5f, 0.5f);
    for (int course = 0; course < 10; course++) {
        float shift = (course % 2) ? 0.5f : 0.0f;
        int count = (course % 2) ? 11 : 12;
        for (int i = 0; i < count; i++) {
            float x = (float(i) - 5.5f + shift) * (brick.x + 0.005f);
            float y = 0.5f * brick.y + float(course) * (brick.y + 0.003f);
            place(world, demos.brick, vec3(x, y, 0), quat(1, 0, 0, 0), body(30.0f));
        }
    }
    demos.blastAt = vec3(0.3f, 2.2f, 0.9f);   // behind the wall, a little off-centre
    camera.position = vec3(0, 3.5f, -14);
    camera.yaw = 1.5708f;
    camera.pitch = -0.08f;
}

void buildBarrage(World& world, Demos& demos, Camera& camera) {
    // A wall of crates, eight wide and six high, for the balls to knock down.
    for (int row = 0; row < 6; row++)
        for (int i = 0; i < 8; i++) {
            Thing t = place(world, demos.crate, vec3(float(i) * 1.02f - 3.57f, 0.5f + float(row) * 1.005f, 10), quat(1, 0, 0, 0),
                            body(25.0f));
            demos.watched.push_back(t.entity);
        }
    camera.position = vec3(-12, 5, -6);
    camera.yaw = 0.75f;
    camera.pitch = -0.12f;
}

void buildSweeper(World& world, Demos& demos, Camera& camera) {
    // A field of crates and balls, and the beam.
    for (int i = 0; i < 70; i++) {
        float angle = uniform(demos, 0, 6.2832f), radius = uniform(demos, 1.5f, 9.0f);
        vec3 at(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);
        Thing t = (i % 3 == 0)
                      ? place(world, demos.balls[i % 5], at + vec3(0, 1.5f, 0), quat(1, 0, 0, 0), ballBody(600.0f, 0.2f))
                      : place(world, demos.crate, at + vec3(0, 0.5f, 0), glm::angleAxis(angle, vec3(0, 1, 0)), body(20.0f));
        demos.watched.push_back(t.entity);
    }
    projv::RigidBody kinematic;
    kinematic.motion = runtime::MotionType::Kinematic;
    demos.beam = place(world, demos.beamProto, vec3(0, 0.6f, 0), quat(1, 0, 0, 0), kinematic).entity;
    camera.position = vec3(0, 14, -16);
    camera.yaw = 1.5708f;
    camera.pitch = -0.65f;
}

void startDemo(World& world, int n) {
    auto& demos = world.ctx().get<Demos>();
    auto& camera = world.ctx().get<Camera>();
    clearDemo(world);
    demos.current = n;
    switch (n) {
        case 1: buildPyramid(world, demos, camera); break;
        case 2: buildWall(world, demos, camera); break;
        case 3: buildBarrage(world, demos, camera); break;
        case 4: buildSweeper(world, demos, camera); break;
    }
    projv::core::info("Demo {}: {}", n, demoName(n));
}

Entity fireBall(World& world, Demos& demos, int size, vec3 from, vec3 velocity, float density = 800.0f) {
    Thing t = place(world, demos.balls[size], from, quat(1, 0, 0, 0), ballBody(density, 0.35f));
    runtime::setVelocity(world, t.entity, velocity, vec3(0));
    return t.entity;
}

// The blast in demo 2, in two parts. First, voxels: a ball of space carved out of every brick it
// reaches, and the bricks near it cracked -- slots cut through them (a brick is 8 x 4 x 4 voxels;
// cutting at x = 2 and 5 and at y = 2 leaves six pieces) -- so they break into fragments, each a body
// of its own (BodySplit). Second, a step later, the kick: everything near, fragments included, thrown
// outward. Kicking in the same step as the edit would throw only the bricks whole: the fragments are
// made from the edit at the next step, with the motion their brick had then.
void detonate(World& world, Demos& demos) {
    const vec3 at = demos.blastAt;
    for (Entity e : world.view<projv::RigidBody>()) {
        std::vector<projv::PendingVoxelOp> ops = runtime::voxelsInSphere(world, e, at, 1.3f);
        if (glm::length(runtime::bodyPose(world, e).position - at) < 3.2f)
            for (int z = 0; z < 4; z++)
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++)
                        if (x == 2 || x == 5 || y == 2) ops.push_back({false, ivec3(x, y, z), 0});
        if (!ops.empty()) runtime::editVoxels(world, e, ops);
    }
    demos.blastKickIn = 2;
}

void blastKick(World& world, Demos& demos) {
    const vec3 at = demos.blastAt;
    for (Entity e : world.view<projv::RigidBody>()) {
        vec3 d = runtime::bodyPose(world, e).position - at;
        float distance = glm::length(d);
        if (distance > 7.0f || distance < 1e-3f) continue;
        float falloff = 1.0f - distance / 7.0f;
        runtime::addVelocity(world, e, (d / distance * 24.0f + vec3(0, 7, 0)) * falloff);
    }
}

// The action for Space.
void act(World& world, Demos& demos, const Camera& camera) {
    switch (demos.current) {
        // A wrecking ball: a metre across, as dense as iron, thrown where you are looking.
        case 1: fireBall(world, demos, 4, camera.position + camera.forward() * 2.0f, camera.forward() * 32.0f, 7800.0f); break;
        case 2: detonate(world, demos); break;
        case 3: fireBall(world, demos, 4, vec3(0, 3, -6), vec3(0, 2, 28)); break;
        default: break;
    }
}

// =========================================================================================
// Systems
// =========================================================================================

// FixedUpdate: what the demos do over time, in fixed steps, so it plays the same at any frame rate.
void demoTick(projv::Application& app) {
    World& world = app.world;
    auto& demos = world.ctx().get<Demos>();
    const float dt = app.time().fixedDelta;
    demos.clock += dt;
    if (demos.blastKickIn > 0 && --demos.blastKickIn == 0) blastKick(world, demos);

    if (demos.current == 3) {
        // A ball every 0.4 s, of a size taken in turn, from a launcher in front of the wall. The
        // oldest are retired past forty.
        demos.fireTimer -= dt;
        if (demos.fireTimer <= 0.0f) {
            demos.fireTimer += 0.4f;
            int size = int(demos.fired.size()) % 5;
            vec3 from(uniform(demos, -3, 3), 2.0f + uniform(demos, 0, 2), -8);
            vec3 target(uniform(demos, -3, 3), uniform(demos, 1, 5), 10);
            vec3 velocity = glm::normalize(target - from) * 24.0f + vec3(0, 2.5f, 0);
            demos.fired.push_back(fireBall(world, demos, size, from, velocity));
            if (demos.fired.size() > 40) {
                if (world.valid(demos.fired.front())) world.destroy(demos.fired.front());
                demos.fired.erase(demos.fired.begin());
            }
        }
    }
}

// Update: the sweeper's beam is placed by its Transform, which physics follows (MoveKinematic),
// pushing everything in its way with the speed it moves at.
void moveBeam(projv::Application& app) {
    World& world = app.world;
    auto& demos = world.ctx().get<Demos>();
    if (demos.current != 4 || !world.valid(demos.beam)) return;
    float t = demos.clock;
    vec3 centre(std::cos(t * 0.35f) * 3.0f, 0.6f, std::sin(t * 0.35f) * 3.0f);
    quat spin = glm::angleAxis(t * 1.1f, vec3(0, 1, 0));
    world.patch<projv::Transform>(demos.beam, [&](projv::Transform& tr) {
        tr.position = centre;
        tr.rotation = spin;
    });
}

void controls(projv::Application& app) {
    World& world = app.world;
    auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
    const auto& input = world.ctx().get<projv::Input>();
    auto& camera = world.ctx().get<Camera>();
    auto& demos = world.ctx().get<Demos>();
    projv::Time& time = app.time();

    if (input.cursorCaptured && input.pressed(projv::Key::Escape)) {
        projv::graphics::setCursorCaptured(app, renderInstance, false);
    } else if (!input.cursorCaptured && input.pressed(projv::MouseButton::Left)) {
        projv::graphics::setCursorCaptured(app, renderInstance, true);
    }
    if (input.cursorCaptured) {
        camera.yaw += input.cursorDelta.x * 0.0025f;
        camera.pitch = std::clamp(camera.pitch - input.cursorDelta.y * 0.0025f, -1.5f, 1.5f);
    }

    for (int n = 1; n <= 4; n++)
        if (input.pressed(projv::Key(int(projv::Key::Num0) + n))) startDemo(world, n);
    if (input.pressed(projv::Key::R)) startDemo(world, demos.current);
    if (input.pressed(projv::Key::Space) && camera.walker == projv::NullEntity) act(world, demos, camera);
    if (input.pressed(projv::Key::T)) time.scale = time.scale == 1.0f ? 0.2f : 1.0f;
    if (input.pressed(projv::Key::P)) time.scale = time.scale == 0.0f ? 1.0f : 0.0f;

    const vec3 flat{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
    const vec3 right{std::cos(camera.yaw - 1.5708f), 0.0f, std::sin(camera.yaw - 1.5708f)};
    if (input.pressed(projv::Key::V)) {
        if (camera.walker == projv::NullEntity) {
            camera.walker = world.create();
            world.emplace<projv::Transform>(camera.walker, projv::Transform{camera.position - vec3(0, EYE_HEIGHT, 0)});
            world.emplace<projv::CharacterController>(camera.walker);
        } else {
            world.destroy(camera.walker);
            camera.walker = projv::NullEntity;
        }
    }
    if (camera.walker != projv::NullEntity) {
        vec3 walk(0.0f);
        if (input.down(projv::Key::W)) walk += flat;
        if (input.down(projv::Key::S)) walk -= flat;
        if (input.down(projv::Key::D)) walk += right;
        if (input.down(projv::Key::A)) walk -= right;
        if (glm::length(walk) > 0.0f) walk = glm::normalize(walk) * WALK_SPEED;
        runtime::setCharacterInput(world, camera.walker, walk, input.pressed(projv::Key::Space), 6.0f);
        camera.position = world.get<projv::Transform>(camera.walker).position + vec3(0, EYE_HEIGHT, 0);
    } else {
        const float speed = 20.0f * time.unscaledDelta;
        if (input.down(projv::Key::W)) camera.position += flat * speed;
        if (input.down(projv::Key::S)) camera.position -= flat * speed;
        if (input.down(projv::Key::D)) camera.position += right * speed;
        if (input.down(projv::Key::A)) camera.position -= right * speed;
        if (input.down(projv::Key::E)) camera.position.y += speed;
        if (input.down(projv::Key::Q)) camera.position.y -= speed;
    }
}

void titleBar(projv::Application& app) {
    auto& demos = app.world.ctx().get<Demos>();
    demos.titleTimer -= app.time().unscaledDelta;
    if (demos.titleTimer > 0.0f) return;
    demos.titleTimer = 0.25f;
    const auto& stats = runtime::physicsWorld(app.world).stats();
    std::string title = fmt::format("ProjectV Physics Demos  |  {} {}  |  {} bodies ({} awake)  |  physics {:.2f} ms  |  {}  |  {} fps",
                                    demos.current, demoName(demos.current), stats.bodies, stats.awakeBodies,
                                    stats.lastStepMilliseconds,
                                    app.time().scale == 0.0f ? "PAUSED" : app.time().scale < 1.0f ? "slow motion" : "x1",
                                    int(1.0f / std::max(app.time().unscaledDelta, 1e-4f)));
    glfwSetWindowTitle(app.world.ctx().get<projv::graphics::RenderInstance>().window, title.c_str());
}

// =========================================================================================
// Self-test: DEMOS_SELFTEST=1 runs each demo for a while and checks what it should have done
// =========================================================================================

struct SelfTest {
    int frame = 0;
    bool pass = true;
    std::vector<vec3> start;     // where the watched bodies began
};

void check(SelfTest& test, bool ok, const std::string& what) {
    projv::core::info("DEMOTEST: {} | {}", what, ok ? "ok" : "WRONG");
    test.pass = test.pass && ok;
}

void selfTest(projv::Application& app) {
    World& world = app.world;
    auto* test = world.ctx().find<SelfTest>();
    if (!test) return;
    auto& demos = world.ctx().get<Demos>();
    auto& camera = world.ctx().get<Camera>();
    const int f = test->frame++;
    // Each demo gets a fixed amount of *simulated* time, whatever the frame rate: the checks are
    // about what physics did.
    const float t = demos.clock;
    auto watchedPositions = [&] {
        std::vector<vec3> out;
        for (Entity e : demos.watched) out.push_back(world.valid(e) ? runtime::bodyPose(world, e).position : vec3(1e9f));
        return out;
    };
    auto moved = [&](float more) {
        std::vector<vec3> now = watchedPositions();
        int n = 0;
        for (size_t i = 0; i < now.size() && i < test->start.size(); i++) n += glm::length(now[i] - test->start[i]) > more;
        return n;
    };

    static int stage = 0;
    static bool acted = false;
    // DEMOS_CAPTURE=<prefix>: a frame of each demo, mid-action, as <prefix>-<demo>.tga.
    static int captured = 0;
    const float captureAt[5] = {0, 5.5f, 2.6f, 5.0f, 9.0f};
    if (const char* prefix = std::getenv("DEMOS_CAPTURE"); prefix && stage >= 1 && stage <= 4 && captured < stage &&
        t > captureAt[stage]) {
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, (std::string(prefix) + "-" + std::to_string(stage)).c_str());
        captured = stage;
    }
    if (f == 0) { startDemo(world, 1); stage = 1; acted = false; return; }
    if (stage == 1 && t > 6.0f && !acted) {
        // The pyramid has settled: the top crate is where it was built, ten crates up.
        vec3 top = runtime::bodyPose(world, demos.watched[0]).position;
        check(*test, std::abs(top.y - 9.5f) < 0.1f && runtime::physicsWorld(world).stats().awakeBodies == 0,
              fmt::format("pyramid stands and sleeps: top crate at {:.2f} m", top.y));
        test->start = watchedPositions();
        camera.position = vec3(0, 4, -14); camera.yaw = 1.5708f; camera.pitch = 0.0f;   // aimed at its middle
        act(world, demos, camera);   // a ball at it
        acted = true;
    }
    if (stage == 1 && t > 10.0f) {
        vec3 top = runtime::bodyPose(world, demos.watched[0]).position;
        check(*test, top.y < 9.0f, fmt::format("the ball brought the pyramid down: top crate at {:.2f} m", top.y));
        startDemo(world, 2); stage = 2; acted = false; demos.pieces = 0;
        return;   // `t` above is the last demo's clock
    }
    if (stage == 2 && t > 2.0f && !acted) { act(world, demos, camera); acted = true; }
    if (stage == 2 && t > 7.0f) {
        size_t bodies = world.view<projv::RigidBody>().size();
        check(*test, demos.pieces >= 10, fmt::format("the blast split bricks: {} fragments, {} bodies", demos.pieces, bodies));
        startDemo(world, 3); stage = 3;
        test->start = watchedPositions();
        return;
    }
    if (stage == 3 && t > 12.0f) {
        int knocked = moved(0.5f);
        check(*test, knocked >= 10, fmt::format("the barrage knocked {} of 48 crates out of the wall", knocked));
        startDemo(world, 4); stage = 4;
        test->start = watchedPositions();
        return;
    }
    if (stage == 4 && t > 15.0f) {
        int swept = moved(1.0f);
        vec3 beam = runtime::bodyPose(world, demos.beam).position;
        check(*test, swept >= 25, fmt::format("the sweeper moved {} of 70 objects", swept));
        check(*test, std::abs(beam.y - 0.6f) < 0.01f, "the beam stayed at its height (kinematic: pushed by nothing)");
        stage = 5;
    }
    if (stage == 5) {
        const auto& stats = runtime::physicsWorld(world).stats();
        check(*test, stats.stepsWithDroppedContacts == 0 && stats.nonFiniteRecoveries == 0,
              fmt::format("{} steps, {} with dropped contacts, {} non-finite recoveries", stats.steps,
                          stats.stepsWithDroppedContacts, stats.nonFiniteRecoveries));
        check(*test, demos.fellThrough == 0, fmt::format("{} bodies fell through the floor", demos.fellThrough));
        runtime::ReplayResult replay = runtime::PhysicsWorld::replay(runtime::physicsWorld(world).recording());
        check(*test, replay.matched, fmt::format("the whole session replays identically ({} steps){}", replay.stepsReplayed,
                                                 replay.matched ? "" : ": " + replay.problem));
        projv::core::info("DEMOTEST: {}", test->pass ? "PASS" : "FAIL");
        app.closeRequested = true;
    }
}

// =========================================================================================
// Stages
// =========================================================================================

void startup(projv::Application& app) {
    World& world = app.world;
    const fs::path here = projv::core::executableDirectory();
    const bool unattended = std::getenv("DEMOS_SELFTEST") != nullptr;

    auto& renderInstance = world.ctx().emplace<projv::graphics::RenderInstance>();
    renderInstance.initialize(1600, 900, "ProjectV Physics Demos");
    projv::graphics::installPlatform(app, renderInstance);
    projv::graphics::setCursorCaptured(app, renderInstance, !unattended);

    world.ctx().emplace<Camera>();
    auto& scene = world.ctx().emplace<projv::Scene>();
    auto& demos = world.ctx().emplace<Demos>();

    runtime::installSceneBridge(app);
    runtime::PhysicsConfig physics;
    physics.settings.worldMin = vec3(-1e4f, -30.0f, -1e4f);
    physics.settings.record = unattended;   // the self-test replays the whole session at the end
    runtime::installPhysics(app, physics);

    // The floor: 80 m square, half-metre voxels, checkered, two voxels thick with its top at y = 0.
    // One component, so one grid; solid because of its StaticCollider.
    ComponentHandle floor = voxels(scene, "floor", ivec3(160, 2, 160), 0.5f, vec3(-FLOOR_HALF, -1, -FLOOR_HALF),
                                   [](int x, int, int z) { return ((x / 8 + z / 8) % 2) ? rgb(0.35f, 0.37f, 0.41f) : rgb(0.27f, 0.29f, 0.32f); });
    Entity floorEntity = runtime::spawnComponent(world, floor);
    world.emplace<projv::StaticCollider>(floorEntity);

    // Prototypes, parked far below the world and never given bodies: what the demos copy.
    demos.crate = prototype(scene, "crate", ivec3(4), 0.25f, [](int x, int y, int z) {
        int edges = (x == 0 || x == 3) + (y == 0 || y == 3) + (z == 0 || z == 3);
        return edges >= 2 ? rgb(0.45f, 0.28f, 0.12f) : rgb(0.78f, 0.56f, 0.3f);
    });
    demos.brick = prototype(scene, "brick", ivec3(8, 4, 4), 0.125f, [](int x, int y, int z) {
        float shade = 0.85f + 0.15f * float((x * 7 + y * 3 + z * 5) % 4) / 3.0f;
        return rgb(0.62f * shade, 0.24f * shade, 0.16f * shade);
    });
    demos.beamProto = prototype(scene, "beam", ivec3(48, 4, 3), 0.25f, [](int x, int, int) {
        return (x / 4) % 2 ? rgb(0.95f, 0.75f, 0.1f) : rgb(0.15f, 0.15f, 0.17f);
    });
    const int diameters[5] = {2, 3, 4, 6, 8};   // 0.25 m to 1 m, in 12.5 cm voxels
    for (int i = 0; i < 5; i++) {
        int n = diameters[i];
        float hue = float(i) / 4.0f;
        demos.balls[i] = prototype(scene, "ball", ivec3(n), 0.125f, [n, hue](int x, int y, int z) -> uint32_t {
            vec3 d = vec3(x, y, z) + vec3(0.5f) - vec3(float(n) * 0.5f);
            if (glm::length(d) > float(n) * 0.5f) return 0u;
            bool stripe = std::abs(d.y) < float(n) * 0.15f;
            return stripe ? rgb(0.95f, 0.95f, 0.95f) : rgb(0.2f + 0.7f * hue, 0.45f, 1.0f - 0.7f * hue);
        });
    }
    for (ComponentHandle p : {demos.crate, demos.brick, demos.beamProto, demos.balls[0], demos.balls[1], demos.balls[2],
                              demos.balls[3], demos.balls[4]})
        utils::setComponentTransform(scene, p, vec3(0, -1000, 0), quat(1, 0, 0, 0), 1.0f);

    // A blast cuts bricks in pieces; each piece is counted. Something that leaves the world from
    // above the floor went through it, which must never happen.
    app.events().on<projv::BodySplit>([&world](const projv::BodySplit& e) {
        world.ctx().get<Demos>().pieces += int(e.pieces.size());
    });
    app.events().on<projv::BodyEmptied>([&world](const projv::BodyEmptied& e) {
        if (world.valid(e.entity)) world.destroy(e.entity);
    });
    app.events().on<projv::BodyLeftWorld>([&world](const projv::BodyLeftWorld& e) {
        if (std::abs(e.position.x) < FLOOR_HALF - 1.0f && std::abs(e.position.z) < FLOOR_HALF - 1.0f) {
            world.ctx().get<Demos>().fellThrough++;
            projv::core::error("demos: a body fell through the floor at ({:.1f}, {:.1f})", e.position.x, e.position.z);
        }
    });

    projv::RendererSpecification specification =
        projv::graphics::loadRendererSpecification((here / "sandboxRenderer").string() + "/");
    renderInstance.addRendererSpecification(1, specification);
    bgfx::ShaderHandle vertexShader = projv::graphics::loadShader((here / "sandboxRenderer/sandboxShaders/vs_quad.bin").string());
    renderInstance.setActiveRenderer(
        projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));
    world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));

    if (unattended) world.ctx().emplace<SelfTest>();
    else startDemo(world, 1);
}

void render(projv::Application& app) {
    World& world = app.world;
    auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
    auto& scene = world.ctx().get<projv::Scene>();
    auto& gpuData = world.ctx().get<projv::GPUData>();
    const auto& camera = world.ctx().get<Camera>();
    projv::graphics::flushSceneUpdates(scene, gpuData);
    auto renderer = renderInstance.getActiveRenderer();
    vec3 position = camera.position, direction = camera.forward();
    projv::core::vec2 resolution = renderInstance.getWindowResolution();
    projv::graphics::setUniformToValue(renderer, "cameraPos", position);
    projv::graphics::setUniformToValue(renderer, "cameraDir", direction);
    projv::graphics::setUniformToValue(renderer, "windowRes", resolution);
    projv::graphics::renderConstructedRenderer(renderInstance, renderer, &gpuData);
}

void shutdown(projv::Application& app) { projv::graphics::destroyGPUData(app.world.ctx().get<projv::GPUData>()); }

}  // namespace

int main() {
    projv::Application app;
    app.addSystem(projv::Stage::Startup,     "startup",   startup);
    app.addSystem(projv::Stage::FixedUpdate, "demos",     demoTick);
    app.addSystem(projv::Stage::Update,      "controls",  controls);
    app.addSystem(projv::Stage::Update,      "beam",      moveBeam);
    app.addSystem(projv::Stage::Update,      "self-test", selfTest);
    app.addSystem(projv::Stage::Update,      "title bar", titleBar);
    app.addSystem(projv::Stage::Render,      "render",    render);
    app.addSystem(projv::Stage::Shutdown,    "shutdown",  shutdown);
    app.run();
    return 0;
}
