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
//   5  Titans         a 14 m stone boulder and a 20 m stone hammer, thousands of tonnes each,
//                     launched into each other across a field of 25 kg crates. Space: again.
//   6  Joints         a plank bridge on ball joints, a wrecking ball on a rope swinging into a weight
//                     on a breakable chain, a motor-driven windmill, a swinging door. Space: drop a
//                     crate on the bridge and fire a ball at the door.
//   7  Car            a car made of joints you drive: W/S throttle and reverse, A/D steer, Space
//                     brake. Rear wheels on driven hinges, front wheels on knuckles steered by servo
//                     hinges. Ramps, a jump, crates and balls to drive into.
//   8  Gear winch     three voxel gears turning each other by their teeth -- only the first has a
//                     motor -- the last with a drum that winds up a chain lifting a weight.
//                     Space: reverse.
//
// Controls
//   1-8        pick a demo (it is rebuilt from scratch)      R  restart the demo
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
using projv::core::vec2;
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
    ComponentHandle crate, brick, beamProto, balls[5], plank, link, wheel, knuckle, chainLink;
    // Demo state.
    float clock = 0.0f;                    // seconds of simulation since the demo began
    float fireTimer = 0.0f;
    Entity beam = projv::NullEntity;
    std::vector<Entity> fired;             // demo 3's balls, oldest first
    std::vector<Entity> watched;           // what a demo's self-test looks at
    std::vector<Entity> owned;             // the demo's other entities: static scenery, joints
    // Demo 5.
    Entity boulder = projv::NullEntity, hammer = projv::NullEntity;
    // Demo 6.
    std::vector<Entity> bridgeJoints, chainJoints;
    Entity windmill = projv::NullEntity, door = projv::NullEntity, doorHinge = projv::NullEntity;
    Entity wreckingBall = projv::NullEntity, rope = projv::NullEntity, weight = projv::NullEntity;
    std::vector<Entity> brokenJoints;      // JointBroke, as they arrive
    float dropTimer = 0.0f;
    // Demo 7: the car.
    struct Car {
        Entity chassis = projv::NullEntity;
        Entity wheels[4]{};                // front left, front right, rear left, rear right
        Entity axles[4]{};                 // each wheel's hinge (front: to its knuckle; rear: to the chassis)
        Entity steering[2]{};              // the front knuckles' servo hinges
        vec3 mounts[4]{};                  // where each wheel sits, in the chassis's frame
        float throttle = 0.0f, steer = 0.0f;
        bool brake = false, driven = false;
    } car;
    // Demo 8: the gear winch.
    struct Winch {
        Entity gears[2]{};                 // the driver, and the winch gear with its drum
        Entity hinges[2]{};
        Entity weight = projv::NullEntity;
        std::vector<Entity> chain;         // the links' joints, drum to weight
        float speed = -1.0f;               // the driver's motor, rad/s: clockwise winds the weight up
        float turned[2]{};                 // each gear's rotation so far, rad
    } winch;
    vec3 blastAt{0.0f};
    int blastKickIn = 0;                   // fixed steps until a detonation's kick (see detonate)
    int pieces = 0;                        // bodies cut off by blasts (BodySplit)
    int fellThrough = 0;                   // left the world from above the floor: must stay 0
    std::mt19937 random{7};
    float titleTimer = 0.0f;
    bool unattended = false;               // the self-test is driving
};

float uniform(Demos& d, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(d.random); }

const char* demoName(int n) {
    switch (n) {
        case 1: return "Pyramid  (Space: throw a ball)";
        case 2: return "Wall blast  (Space: detonate)";
        case 3: return "Ball barrage";
        case 4: return "Sweeper";
        case 5: return "Titans  (Space: again)";
        case 6: return "Joints  (Space: a crate on the bridge, a ball at the door)";
        case 7: return "Car  (W/S drive, A/D steer, Space brake)";
        case 8: return "Gear winch  (Space: reverse)";
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
    for (Entity e : demos.owned) if (world.valid(e)) world.destroy(e);
    demos.owned.clear();
    demos.bridgeJoints.clear();
    demos.chainJoints.clear();
    demos.brokenJoints.clear();
    demos.boulder = demos.hammer = demos.windmill = demos.door = demos.doorHinge = projv::NullEntity;
    demos.wreckingBall = demos.rope = demos.weight = projv::NullEntity;
    demos.dropTimer = 0.0f;
    demos.car = Demos::Car{};
    demos.winch = Demos::Winch{};
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

// A big thing, built in place rather than copied: an asset node at its centre with its voxels
// inside, offset by minus half their size -- more than 16 voxels across, so a grid of cells.
template <typename ColourAt>
Entity big(World& world, const char* name, ivec3 size, float voxel, vec3 centre, quat rotation, ColourAt colourAt,
           const projv::RigidBody& rigid) {
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle node = utils::addComponent(scene, projv::ComponentKind::Asset, name, projv::INVALID_COMPONENT_HANDLE, 4, 1.0f);
    voxels(scene, name, size, voxel, -0.5f * vec3(size) * voxel, colourAt, node);
    utils::setComponentTransform(scene, node, centre, rotation, 1.0f);
    Entity e = runtime::spawnComponent(world, node, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    world.emplace<projv::RigidBody>(e, rigid);
    runtime::assignNetId(world, e);
    return e;
}

// Static scenery: voxels whose corner is at `corner`, solid, owned by the demo.
template <typename ColourAt>
void scenery(World& world, Demos& demos, ivec3 size, float voxel, vec3 corner, ColourAt colourAt) {
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle h = voxels(scene, "scenery", size, voxel, corner, colourAt);
    Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    world.emplace<projv::StaticCollider>(e);
    demos.owned.push_back(e);
}

Entity joint(World& world, Demos& demos, Entity a, Entity b, projv::Joint j) {
    Entity e = runtime::connectBodies(world, a, b, j);
    demos.owned.push_back(e);
    return e;
}

uint32_t stone(int x, int y, int z) {
    float shade = 0.75f + 0.25f * float((x * 13 + y * 7 + z * 11) % 5) / 4.0f;
    return rgb(0.52f * shade, 0.5f * shade, 0.47f * shade);
}

void launchTitans(World& world, Demos& demos) {
    runtime::setVelocity(world, demos.boulder, vec3(13, 1, 0), vec3(0, 0, -1.2f));
    runtime::setVelocity(world, demos.hammer, vec3(-13, 1, 0), vec3(0, 0.15f, 0.2f));
}

void buildTitans(World& world, Demos& demos, Camera& camera) {
    // A field of crates for scale, where the two will meet.
    for (int i = 0; i < 60; i++) {
        vec3 at(uniform(demos, -12, 12), 0.5f, uniform(demos, -7, 7));
        Thing t = place(world, demos.crate, at, glm::angleAxis(uniform(demos, 0, 6.28f), vec3(0, 1, 0)), body(25.0f));
        demos.watched.push_back(t.entity);
    }
    projv::RigidBody rock;
    rock.density = 2500.0f;   // stone
    rock.friction = 0.8f;
    // The boulder: a lumpy ball 14 m across, in half-metre voxels.
    demos.boulder = big(world, "boulder", ivec3(28), 0.5f, vec3(-26, 7.3f, 0), quat(1, 0, 0, 0), [](int x, int y, int z) -> uint32_t {
        vec3 d = vec3(x, y, z) + vec3(0.5f) - vec3(14.0f);
        float bumps = 1.2f * std::sin(float(x) * 0.7f) * std::cos(float(z) * 0.6f) + 0.8f * std::sin(float(y) * 0.9f);
        return glm::length(d) <= 13.0f + bumps ? stone(x, y, z) : 0u;
    }, rock);
    // The hammer: a 16 m shaft, 2 m square, with a 4 m x 5 m x 5 m head on the end facing the boulder.
    demos.hammer = big(world, "hammer", ivec3(40, 10, 10), 0.5f, vec3(24, 2.6f, 0), quat(1, 0, 0, 0), [](int x, int y, int z) -> uint32_t {
        bool head = x < 8;
        bool shaft = y >= 3 && y < 7 && z >= 3 && z < 7;
        if (!head && !shaft) return 0u;
        return head ? rgb(0.38f, 0.4f, 0.45f) : rgb(0.55f, 0.36f, 0.2f);
    }, rock);
    launchTitans(world, demos);
    camera.position = vec3(0, 16, -44);
    camera.yaw = 1.5708f;
    camera.pitch = -0.28f;
}

void buildJoints(World& world, Demos& demos, Camera& camera) {
    // 1. A plank bridge between two posts: sixteen planks, each joined to the next by a ball joint at
    //    either side (two per seam, so the bridge hangs but does not twist), the ends to the world.
    const float bridgeX = -12.0f, bridgeY = 3.0f, pitch = 0.52f;
    const int planks = 16;
    const float z0 = -float(planks - 1) * pitch * 0.5f;
    Entity previous = projv::NullEntity;
    auto seam = [&](Entity a, Entity b, float z) {
        for (float side : {-0.55f, 0.55f}) {
            projv::Joint j;
            j.type = runtime::JointType::Point;
            j.anchor = vec3(bridgeX + side, bridgeY, z);
            demos.bridgeJoints.push_back(joint(world, demos, a, b, j));
        }
    };
    for (int i = 0; i < planks; i++) {
        float z = z0 + float(i) * pitch;
        Thing p = place(world, demos.plank, vec3(bridgeX, bridgeY, z), quat(1, 0, 0, 0), body(8.0f));
        seam(p.entity, previous, z - pitch * 0.5f);   // the first plank's back seam: to the world
        previous = p.entity;
        demos.watched.push_back(p.entity);
    }
    seam(previous, projv::NullEntity, z0 + float(planks - 1) * pitch + pitch * 0.5f);
    for (float z : {z0 - pitch * 0.5f - 1.0f, -z0 + pitch * 0.5f}) {
        scenery(world, demos, ivec3(12, 26, 8), 0.125f, vec3(bridgeX - 0.75f, 0.0f, z), [](int, int y, int) {
            return y > 22 ? rgb(0.3f, 0.8f, 0.5f) : rgb(0.3f, 0.3f, 0.34f);
        });
    }

    // 2. A windmill: a 6 m cross on a motor-driven hinge, its blades just clearing the floor.
    projv::RigidBody blades;
    blades.density = 400.0f;
    demos.windmill = big(world, "windmill", ivec3(24, 24, 2), 0.25f, vec3(0, 3.6f, 8), quat(1, 0, 0, 0), [](int x, int y, int) -> uint32_t {
        bool arm = (x >= 10 && x < 14) || (y >= 10 && y < 14);
        if (!arm) return 0u;
        return (x / 3 + y / 3) % 2 ? rgb(0.92f, 0.92f, 0.95f) : rgb(0.85f, 0.2f, 0.18f);
    }, blades);
    projv::Joint axle;
    axle.type = runtime::JointType::Hinge;
    axle.anchor = vec3(0, 3.6f, 8);
    axle.axis = vec3(0, 0, 1);
    axle.motor = true;
    axle.motorVelocity = 1.2f;
    axle.motorLimit = 60000.0f;
    joint(world, demos, demos.windmill, projv::NullEntity, axle);
    // Its post stands well behind the blades, so nothing they carry round can be pinned against it.
    scenery(world, demos, ivec3(4, 15, 4), 0.25f, vec3(-0.5f, 0, 9.0f), [](int, int, int) { return rgb(0.3f, 0.3f, 0.34f); });

    // 3. A door: 2 m x 2.5 m, hinged on its left edge, swinging up to 110 degrees either way.
    projv::RigidBody slab;
    slab.density = 600.0f;
    demos.door = big(world, "door", ivec3(16, 20, 1), 0.125f, vec3(9, 1.27f, 0), quat(1, 0, 0, 0), [](int x, int y, int) -> uint32_t {
        bool frame = x < 1 || x > 14 || y < 1 || y > 18;
        return frame ? rgb(0.35f, 0.22f, 0.1f) : rgb(0.6f, 0.42f, 0.22f);
    }, slab);
    projv::Joint hinge;
    hinge.type = runtime::JointType::Hinge;
    hinge.anchor = vec3(8, 1.27f, 0);
    hinge.axis = vec3(0, 1, 0);
    hinge.limitMin = -1.9f;
    hinge.limitMax = 1.9f;
    hinge.friction = 15.0f;
    demos.doorHinge = joint(world, demos, demos.door, projv::NullEntity, hinge);
    scenery(world, demos, ivec3(2, 23, 2), 0.125f, vec3(7.75f, 0, -0.125f), [](int, int, int) { return rgb(0.3f, 0.3f, 0.34f); });

    // 4. A weight on a breakable chain, and a wrecking ball on a rope pulled back to swing into it.
    const vec3 chainTop(18, 9, 2.6f);
    previous = projv::NullEntity;
    for (int i = 0; i < 6; i++) {
        vec3 centre = chainTop - vec3(0, 0.375f + 0.75f * float(i), 0);
        Thing l = place(world, demos.link, centre, quat(1, 0, 0, 0), body(4.0f));
        projv::Joint j;
        j.type = runtime::JointType::Point;
        j.anchor = centre + vec3(0, 0.375f, 0);
        j.breakForce = 9000.0f;   // three times the weight's pull at rest
        demos.chainJoints.push_back(joint(world, demos, l.entity, previous, j));
        previous = l.entity;
    }
    Thing weight = place(world, demos.crate, chainTop - vec3(0, 4.5f + 0.5f, 0), quat(1, 0, 0, 0), body(300.0f));
    demos.weight = weight.entity;
    projv::Joint hook;
    hook.type = runtime::JointType::Point;
    hook.anchor = chainTop - vec3(0, 4.5f, 0);
    hook.breakForce = 9000.0f;
    demos.chainJoints.push_back(joint(world, demos, weight.entity, previous, hook));

    const vec3 pivot(18, 11.5f, 1.6f);
    const float ropeLength = 7.5f, pulledBack = 80.0f * 3.14159f / 180.0f;
    vec3 start = pivot + ropeLength * vec3(0, -std::cos(pulledBack), -std::sin(pulledBack));
    Thing ball = place(world, demos.balls[4], start, quat(1, 0, 0, 0), ballBody(7800.0f, 0.1f));   // a tonne of iron... four
    demos.wreckingBall = ball.entity;
    projv::Joint rope;
    rope.type = runtime::JointType::Distance;
    rope.anchor = start;
    rope.anchorB = pivot;
    rope.limitMin = 0.0f;
    rope.limitMax = ropeLength;
    demos.rope = joint(world, demos, ball.entity, projv::NullEntity, rope);

    camera.position = vec3(3, 12, -27);   // the whole row: bridge, windmill, door, chain and ball
    camera.yaw = 1.5708f;
    camera.pitch = -0.3f;
}

// ---- Demo 7: a car made of joints --------------------------------------------------------------

constexpr float WHEEL_RADIUS = 0.4375f;      // 3.5 voxels of 12.5 cm
constexpr float WHEEL_TOP_SPEED = 45.0f;     // rad/s at the wheel: about 20 m/s
constexpr float STEER_LOCK = 0.5f;           // rad

void buildCar(World& world, Demos& demos, vec3 at) {
    Demos::Car& car = demos.car;
    // The body: a 2 m x 4 m tub of 12.5 cm voxels with a cabin on it, 900 kg.
    projv::RigidBody shell;
    shell.mass = 900.0f;
    car.chassis = big(world, "car", ivec3(16, 10, 32), 0.125f, at + vec3(0, 0.95f, 0), quat(1, 0, 0, 0), [](int x, int y, int z) -> uint32_t {
        bool tub = y < 4;
        bool cabin = y >= 4 && z >= 8 && z < 22 && x >= 1 && x < 15;
        if (!tub && !cabin) return 0u;
        if (cabin) return (y >= 6 && (x == 1 || x == 14 || z == 8 || z == 21)) ? rgb(0.5f, 0.7f, 0.85f) : rgb(0.85f, 0.15f, 0.1f);
        return (z < 2 || z > 29) ? rgb(0.9f, 0.85f, 0.3f) : rgb(0.85f, 0.15f, 0.1f);
    }, shell);
    // The wheels collide as spheres (see below), wider than they look: set out far enough that a
    // sphere clears the 2 m body. A front wheel is joined to its knuckle, not to the chassis, so the
    // two would collide -- and jam the steering -- if they met.
    const float track = 1.5f, base = 1.4f;
    const vec3 corners[4] = {vec3(-track, 0, base), vec3(track, 0, base), vec3(-track, 0, -base), vec3(track, 0, -base)};
    const vec3 chassisCentre = at + vec3(0, 0.95f, 0);
    projv::RigidBody tyre = ballBody(1200.0f, 0.0f);   // a sphere: rolls smoothly where a voxel disc would bump
    tyre.mass = 25.0f;
    tyre.friction = 1.4f;
    for (int i = 0; i < 4; i++) {
        vec3 centre = at + corners[i] + vec3(0, WHEEL_RADIUS, 0);
        car.mounts[i] = centre - chassisCentre;
        car.wheels[i] = place(world, demos.wheel, centre, quat(1, 0, 0, 0), tyre).entity;
        projv::Joint axle;
        axle.type = runtime::JointType::Hinge;
        axle.anchor = centre;
        axle.axis = vec3(1, 0, 0);
        axle.motorLimit = i < 2 ? 500.0f : 900.0f;   // front: brakes only; rear: brakes and drive
        if (i < 2) {
            // A knuckle between chassis and wheel, turned about the vertical by a servo: steering.
            projv::RigidBody k = body(15.0f);
            Entity knuckle = place(world, demos.knuckle, centre, quat(1, 0, 0, 0), k).entity;
            projv::Joint steer;
            steer.type = runtime::JointType::Hinge;
            steer.anchor = centre;
            steer.axis = vec3(0, 1, 0);
            steer.limitMin = -STEER_LOCK - 0.05f;
            steer.limitMax = STEER_LOCK + 0.05f;
            steer.motor = true;
            steer.motorMode = runtime::MotorMode::Position;
            steer.motorLimit = 4000.0f;
            steer.motorFrequency = 12.0f;
            car.steering[i] = joint(world, demos, knuckle, car.chassis, steer);
            car.axles[i] = joint(world, demos, car.wheels[i], knuckle, axle);
        } else {
            car.axles[i] = joint(world, demos, car.wheels[i], car.chassis, axle);
        }
    }
}

// What the driver asks for, as joint motor commands: the rear axles driven, all four braked (held
// at zero speed), or left to roll; the front knuckles' servos turned. Sent only when it changes.
void driveCar(World& world, Demos& demos, float throttle, float steer, bool brake) {
    Demos::Car& car = demos.car;
    if (!world.valid(car.chassis)) return;
    if (car.driven && throttle == car.throttle && steer == car.steer && brake == car.brake) return;
    car.driven = true;
    car.throttle = throttle;
    car.steer = steer;
    car.brake = brake;
    for (int i = 0; i < 4; i++) {
        bool rear = i >= 2;
        if (brake) runtime::setJointMotor(world, car.axles[i], true, 0.0f);
        else if (rear && throttle != 0.0f) runtime::setJointMotor(world, car.axles[i], true, throttle * WHEEL_TOP_SPEED);
        else runtime::setJointMotor(world, car.axles[i], false, 0.0f);
    }
    for (int i = 0; i < 2; i++)
        runtime::setJointMotor(world, car.steering[i], true, steer * STEER_LOCK, runtime::MotorMode::Position);
}

vec3 carForward(World& world, const Demos& demos) {
    return runtime::bodyPose(world, demos.car.chassis).rotation * vec3(0, 0, 1);
}

void buildCarPark(World& world, Demos& demos, Camera& camera) {
    // A ramp up to a 1.5 m lip, and the drop beyond it: a jump. 12.5 cm voxels keep its slope fine.
    scenery(world, demos, ivec3(48, 12, 64), 0.125f, vec3(-3, 0, 12), [](int, int y, int z) -> uint32_t {
        return y <= z * 12 / 64 ? (z % 8 < 4 ? rgb(0.5f, 0.5f, 0.55f) : rgb(0.42f, 0.42f, 0.47f)) : 0u;
    });
    // A gentler, longer ramp off to the side, and a wall of crates and a few balls to drive into.
    scenery(world, demos, ivec3(40, 6, 96), 0.125f, vec3(-14, 0, 0), [](int, int y, int z) -> uint32_t {
        return y <= z * 6 / 96 ? rgb(0.45f, 0.5f, 0.45f) : 0u;
    });
    for (int row = 0; row < 3; row++)
        for (int i = 0; i < 6; i++)
            place(world, demos.crate, vec3(10 + float(i) * 1.02f, 0.5f + float(row) * 1.005f, 14), quat(1, 0, 0, 0), body(30.0f));
    for (int i = 0; i < 12; i++)
        place(world, demos.balls[i % 5], vec3(uniform(demos, 6, 18), 1, uniform(demos, 20, 30)), quat(1, 0, 0, 0), ballBody(400.0f, 0.4f));
    // A 40 m run-up to the ramp.
    buildCar(world, demos, vec3(0, 0, -30));
    camera.position = vec3(0, 4, -40);
}

// ---- Demo 8: a gear train winding a winch --------------------------------------------------------

// Cross gears. A gear here is one or more crosses -- two straight bars through its middle, four
// pegs -- each laid along the voxel grid's own axes, so its faces are flat: it collides as two exact
// boxes, with no stair-steps for another gear to catch on. (Gears with slanted teeth or radial pegs
// drawn in voxels jammed: their stair-stepped edges caught at every handover, at any resolution we
// tried.) More pegs come from more crosses in the same body, each turned and drawn in its own grid:
// two crosses 45 degrees apart are eight straight pegs.
constexpr float GEAR_VOXEL = 0.05f;
constexpr float GEAR_THICKNESS = 0.2f;
constexpr float PEG_WIDTH = 0.2f;

// One cross, inside the gear's asset node: bars `reach` from the centre each way, turned `angle`
// about the axle.
void addCross(projv::Scene& scene, ComponentHandle node, float reach, float angle, uint32_t colour) {
    const int n = 2 * int(std::round(reach / GEAR_VOXEL));
    const int w = int(std::round(PEG_WIDTH / GEAR_VOXEL));
    const int t = int(std::round(GEAR_THICKNESS / GEAR_VOXEL));
    const int low = n / 2 - w / 2, high = low + w;   // the bars' voxels across the middle
    ComponentHandle h = voxels(scene, "cross", ivec3(n, n, t), GEAR_VOXEL, vec3(0), [=](int x, int y, int) -> uint32_t {
        bool across = y >= low && y < high, up = x >= low && x < high;
        if (!across && !up) return 0u;
        bool tip = std::abs(x - n / 2) > n / 2 - 3 || std::abs(y - n / 2) > n / 2 - 3;
        return tip ? rgb(0.95f, 0.93f, 0.88f) : colour;
    }, node);
    quat turn = glm::angleAxis(angle, vec3(0, 0, 1));
    utils::setComponentTransform(scene, h, turn * vec3(-reach, -reach, -0.5f * GEAR_THICKNESS), turn, 1.0f);
}

// A gear: an asset node at `centre`, its crosses, and optionally a drum behind them (from the back of
// the gear 0.6 m along the axle) for a chain to wind onto.
Entity crossGear(World& world, vec3 centre, float reach, std::vector<float> angles, uint32_t colour, float drum) {
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle node = utils::addComponent(scene, projv::ComponentKind::Asset, "gear", projv::INVALID_COMPONENT_HANDLE, 4, 1.0f);
    for (float a : angles) addCross(scene, node, reach, a, colour);
    if (drum > 0.0f) {
        int n = 2 * int(std::ceil(drum / GEAR_VOXEL)), length = int(std::round(0.6f / GEAR_VOXEL));
        voxels(scene, "drum", ivec3(n, n, length), GEAR_VOXEL, vec3(-0.5f * float(n) * GEAR_VOXEL, -0.5f * float(n) * GEAR_VOXEL, 0.5f * GEAR_THICKNESS),
               [n, drum](int x, int y, int z) -> uint32_t {
                   vec2 d = (vec2(float(x), float(y)) + vec2(0.5f) - vec2(float(n) * 0.5f)) * GEAR_VOXEL;
                   if (glm::length(d) > drum) return 0u;
                   return (z / 2) % 2 ? rgb(0.45f, 0.3f, 0.15f) : rgb(0.55f, 0.38f, 0.2f);
               }, node);
    }
    utils::setComponentTransform(scene, node, centre, quat(1, 0, 0, 0), 1.0f);
    Entity e = runtime::spawnComponent(world, node, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    projv::RigidBody wheel;
    wheel.density = 300.0f;   // wood
    wheel.friction = 0.2f;
    world.emplace<projv::RigidBody>(e, wheel);
    runtime::assignNetId(world, e);
    return e;
}

void buildWinch(World& world, Demos& demos, Camera& camera) {
    Demos::Winch& w = demos.winch;
    const float y = 6.0f;
    // The driver: one cross, four pegs, on a motor. The winch gear: two crosses 45 degrees apart,
    // eight pegs, twice the pitch radius (0.6 m and 1.2 m), so it turns at half the driver's speed.
    // Pegs reach 0.25 m past the pitch circle. The winch gear starts turned 22.5 degrees, so the
    // driver's peg pointing at it sits between two of its pegs, with 17 cm to spare either side.
    const float driverX = 0.0f, winchX = 1.8f, drumRadius = 0.35f;
    w.gears[0] = crossGear(world, vec3(driverX, y, 0), 0.85f, {0.0f}, rgb(0.85f, 0.65f, 0.15f), 0.0f);
    w.gears[1] = crossGear(world, vec3(winchX, y, 0), 1.45f, {0.3927f, 1.1781f}, rgb(0.75f, 0.35f, 0.2f), drumRadius);
    for (int i = 0; i < 2; i++) {
        projv::Joint axle;
        axle.type = runtime::JointType::Hinge;
        axle.anchor = vec3(i == 0 ? driverX : winchX, y, 0);
        axle.axis = vec3(0, 0, 1);
        if (i == 0) {
            axle.motor = true;
            axle.motorVelocity = w.speed;
            axle.motorLimit = 3000.0f;
        }
        w.hinges[i] = joint(world, demos, w.gears[i], projv::NullEntity, axle);
    }
    // A wall behind the whole works (behind the drum, which is behind the gears), to see them against.
    scenery(world, demos, ivec3(80, 40, 2), 0.125f, vec3(-1.5f, 3.5f, 1.3f), [](int x, int yy, int) {
        return (x / 8 + yy / 8) % 2 ? rgb(0.24f, 0.25f, 0.28f) : rgb(0.21f, 0.22f, 0.25f);
    });

    // The chain: 24 links of 0.2 m from the right of the drum, down to a 20 kg weight. Each link to
    // the next by a ball joint; the first to the drum's rim. As the drum turns the links wrap onto it.
    const float drumZ = GEAR_THICKNESS * 0.5f + 0.3f + 0.05f;   // the middle of the drum's length
    const vec3 top(winchX + drumRadius + 0.06f, y, drumZ);
    Entity previous = w.gears[1];
    for (int i = 0; i < 24; i++) {
        vec3 centre = top - vec3(0, 0.1f + 0.2f * float(i), 0);
        Thing link = place(world, demos.chainLink, centre, quat(1, 0, 0, 0), body(0.8f));
        projv::Joint j;
        j.type = runtime::JointType::Point;
        j.anchor = centre + vec3(0, 0.1f, 0);
        w.chain.push_back(joint(world, demos, link.entity, previous, j));
        previous = link.entity;
    }
    Thing weight = place(world, demos.crate, top - vec3(0, 4.8f + 0.5f, 0), quat(1, 0, 0, 0), body(20.0f));
    w.weight = weight.entity;
    projv::Joint hook;
    hook.type = runtime::JointType::Point;
    hook.anchor = top - vec3(0, 4.8f, 0);
    w.chain.push_back(joint(world, demos, weight.entity, previous, hook));

    camera.position = vec3(0.9f, 4.5f, -7.5f);
    camera.yaw = 1.5708f;
    camera.pitch = 0.0f;
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
        case 5: buildTitans(world, demos, camera); break;
        case 6: buildJoints(world, demos, camera); break;
        case 7: buildCarPark(world, demos, camera); break;
        case 8: buildWinch(world, demos, camera); break;
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
        case 5: startDemo(world, 5); break;
        case 8:
            demos.winch.speed = -demos.winch.speed;
            runtime::setJointMotor(world, demos.winch.hinges[0], true, demos.winch.speed);
            break;
        case 6:
            place(world, demos.crate, vec3(-12, 7, uniform(demos, -2, 2)), quat(1, 0, 0, 0), body(60.0f));
            fireBall(world, demos, 3, vec3(uniform(demos, 8.5f, 9.5f), 1.5f, -9), vec3(0, 1.5f, 22), 2000.0f);
            break;
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
    if (demos.current == 8)
        for (int i = 0; i < 2; i++)
            if (world.valid(demos.winch.gears[i])) demos.winch.turned[i] += runtime::bodyPose(world, demos.winch.gears[i]).angularVelocity.z * dt;

    if (demos.current == 6) {
        // A crate dropped onto the windmill's blades every 1.5 s, a ball at the door every 2.5 s.
        demos.dropTimer -= dt;
        if (demos.dropTimer <= 0.0f) {
            demos.dropTimer += 1.5f;
            place(world, demos.crate, vec3(uniform(demos, -2.5f, 2.5f), 9, 7.6f), quat(1, 0, 0, 0), body(15.0f));
        }
        demos.fireTimer -= dt;
        if (demos.fireTimer <= 0.0f) {
            demos.fireTimer += 2.5f;
            fireBall(world, demos, 3, vec3(uniform(demos, 8.4f, 9.6f), 1.4f, -9), vec3(0, 1.5f, 20), 1500.0f);
        }
    }
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

    for (int n = 1; n <= 8; n++)
        if (input.pressed(projv::Key(int(projv::Key::Num0) + n))) startDemo(world, n);
    if (input.pressed(projv::Key::R)) startDemo(world, demos.current);
    if (input.pressed(projv::Key::Space) && camera.walker == projv::NullEntity && demos.current != 7) act(world, demos, camera);
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
    if (demos.current == 7 && camera.walker == projv::NullEntity) {
        // Driving: the chase camera follows in PostUpdate (followCamera). The self-test drives on its own.
        if (!demos.unattended) {
            float throttle = (input.down(projv::Key::W) ? 1.0f : 0.0f) - (input.down(projv::Key::S) ? 0.6f : 0.0f);
            float steer = (input.down(projv::Key::D) ? 1.0f : 0.0f) - (input.down(projv::Key::A) ? 1.0f : 0.0f);
            driveCar(world, demos, throttle, steer, input.down(projv::Key::Space));
        }
    } else if (camera.walker != projv::NullEntity) {
        vec3 walk(0.0f);
        if (input.down(projv::Key::W)) walk += flat;
        if (input.down(projv::Key::S)) walk -= flat;
        if (input.down(projv::Key::D)) walk += right;
        if (input.down(projv::Key::A)) walk -= right;
        if (glm::length(walk) > 0.0f) walk = glm::normalize(walk) * WALK_SPEED;
        runtime::setCharacterInput(world, camera.walker, walk, input.pressed(projv::Key::Space), 6.0f);
        // The camera follows in PostUpdate (followWalker), once physics has placed the character for
        // this frame.
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

// PostUpdate, after physics has written this frame's interpolated pose: the camera is the walker's
// eyes. Read in Update instead, it would be last frame's pose, a frame behind the world it is
// drawn with -- the lag that made walking feel loose.
// In demo 7 the same goes for the chase camera: behind the car and above it, looking at it, eased
// so it swings round behind rather than snapping.
void followCamera(projv::Application& app) {
    World& world = app.world;
    auto& camera = world.ctx().get<Camera>();
    auto& demos = world.ctx().get<Demos>();
    if (camera.walker != projv::NullEntity && world.valid(camera.walker)) {
        camera.position = world.get<projv::Transform>(camera.walker).position + vec3(0, EYE_HEIGHT, 0);
        return;
    }
    if (demos.current != 7 || !world.valid(demos.car.chassis) || !world.all_of<projv::Transform>(demos.car.chassis)) return;
    const projv::Transform& t = world.get<projv::Transform>(demos.car.chassis);
    vec3 heading = t.rotation * vec3(0, 0, 1);
    heading.y = 0.0f;
    heading = glm::length(heading) > 1e-3f ? glm::normalize(heading) : vec3(0, 0, 1);
    vec3 wanted = t.position - heading * 9.0f + vec3(0, 3.5f, 0);
    float ease = 1.0f - std::exp(-6.0f * app.time().unscaledDelta);
    camera.position = glm::mix(camera.position, wanted, ease);
    vec3 look = glm::normalize(t.position + vec3(0, 1, 0) - camera.position);
    camera.yaw = std::atan2(look.z, look.x);
    camera.pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
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
    // DEMOS_ONLY=8 or DEMOS_ONLY=2,8: the demos to run; all of them when empty.
    std::vector<int> only;
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
    const float captureAt[9] = {0, 5.5f, 2.6f, 5.0f, 9.0f, 3.3f, 5.6f, 6.5f, 12.0f};
    if (const char* prefix = std::getenv("DEMOS_CAPTURE"); prefix && stage >= 1 && stage <= 8 && captured < stage &&
        t > captureAt[stage]) {
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, (std::string(prefix) + "-" + std::to_string(stage)).c_str());
        captured = stage;
    }
    // On to the first selected demo from `next`, or to the session-wide checks after the last.
    auto go = [&](int next) {
        while (next <= 8 && !test->only.empty() &&
               std::find(test->only.begin(), test->only.end(), next) == test->only.end())
            next++;
        acted = false;
        if (next > 8) { stage = 9; return; }
        startDemo(world, next);
        stage = next;
        test->start = watchedPositions();
    };
    if (f == 0) { go(1); return; }
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
        go(2); acted = false; demos.pieces = 0;
        return;   // `t` above is the last demo's clock
    }
    if (stage == 2 && t > 2.0f && !acted) { act(world, demos, camera); acted = true; }
    if (stage == 2 && t > 7.0f) {
        size_t bodies = world.view<projv::RigidBody>().size();
        check(*test, demos.pieces >= 10, fmt::format("the blast split bricks: {} fragments, {} bodies", demos.pieces, bodies));
        go(3);
        test->start = watchedPositions();
        return;
    }
    if (stage == 3 && t > 12.0f) {
        int knocked = moved(0.5f);
        check(*test, knocked >= 10, fmt::format("the barrage knocked {} of 48 crates out of the wall", knocked));
        go(4);
        test->start = watchedPositions();
        return;
    }
    if (stage == 4 && t > 15.0f) {
        int swept = moved(1.0f);
        vec3 beam = runtime::bodyPose(world, demos.beam).position;
        check(*test, swept >= 25, fmt::format("the sweeper moved {} of 70 objects", swept));
        check(*test, std::abs(beam.y - 0.6f) < 0.01f, "the beam stayed at its height (kinematic: pushed by nothing)");
        go(5);
        test->start = watchedPositions();
        return;
    }
    if (stage == 5 && t > 6.0f) {
        const auto& sim = runtime::physicsWorld(world);
        runtime::BodyState boulder = runtime::bodyPose(world, demos.boulder), hammer = runtime::bodyPose(world, demos.hammer);
        float boulderMass = sim.bodyMass(world.get<projv::PhysicsBody>(demos.boulder).id);
        float hammerMass = sim.bodyMass(world.get<projv::PhysicsBody>(demos.hammer).id);
        check(*test, boulderMass > 1e6f && hammerMass > 3e5f,
              fmt::format("titans: boulder {:.0f} t, hammer {:.0f} t", boulderMass / 1000.0f, hammerMass / 1000.0f));
        // They met: neither is still heading on at 13 m/s.
        check(*test, boulder.linearVelocity.x < 8.0f && hammer.linearVelocity.x > -8.0f,
              fmt::format("they collided: boulder now {:.1f} m/s, hammer {:.1f} m/s along x", boulder.linearVelocity.x,
                          hammer.linearVelocity.x));
        // Nothing pressed into the floor: thousands of tonnes on 25 kg crates is the hard case.
        int under = 0;
        for (Entity e : demos.watched) if (world.valid(e) && runtime::bodyPose(world, e).position.y < 0.2f) under++;
        check(*test, under == 0 && boulder.position.y > 5.0f,
              fmt::format("no crate pushed into the floor ({}), the boulder on it at {:.1f} m", under, boulder.position.y));
        check(*test, moved(1.0f) >= 10, fmt::format("the impact scattered {} crates", moved(1.0f)));
        go(6);
        return;
    }
    if (stage == 6 && t > 4.0f && !acted) { act(world, demos, camera); act(world, demos, camera); acted = true; }
    if (stage == 6 && t > 14.0f) {
        auto isBroken = [&](Entity j) {
            return std::find(demos.brokenJoints.begin(), demos.brokenJoints.end(), j) != demos.brokenJoints.end();
        };
        int bridgeBroken = 0, chainBroken = 0;
        for (Entity j : demos.bridgeJoints) bridgeBroken += isBroken(j);
        for (Entity j : demos.chainJoints) chainBroken += isBroken(j);
        float lowest = 1e9f, highest = -1e9f;
        for (Entity e : demos.watched) {
            float y = runtime::bodyPose(world, e).position.y;
            lowest = std::min(lowest, y);
            highest = std::max(highest, y);
        }
        check(*test, bridgeBroken == 0 && lowest > 0.5f && highest < 3.2f,
              fmt::format("the bridge holds and hangs: {} joints broken, planks from {:.2f} to {:.2f} m", bridgeBroken, lowest, highest));
        check(*test, chainBroken >= 1, fmt::format("the wrecking ball broke the chain: {} of its joints went", chainBroken));
        runtime::JointState rope = runtime::jointState(world, demos.rope);
        check(*test, rope.alive && rope.position < 7.5f * 1.02f, fmt::format("the rope is never longer than 7.5 m: {:.2f} m", rope.position));
        float spin = runtime::bodyPose(world, demos.windmill).angularVelocity.z;
        check(*test, std::abs(spin - 1.2f) < 0.25f, fmt::format("the windmill turns at its motor's 1.2 rad/s: {:.2f}", spin));
        runtime::JointState hinge = runtime::jointState(world, demos.doorHinge);
        check(*test, hinge.alive && std::abs(hinge.position) < 1.9f + 0.06f,
              fmt::format("the door swings within its 1.9 rad limits: at {:.2f}", hinge.position));
        go(7);
        return;
    }
    if (stage == 7) {
        // Drive: full throttle for four seconds, then a right turn, then the brakes.
        static float heading0 = 0.0f, speedAtFive = 0.0f;
        static bool steered = false;
        auto speed = [&] { return glm::length(runtime::bodyPose(world, demos.car.chassis).linearVelocity); };
        auto heading = [&] { vec3 f = carForward(world, demos); return std::atan2(f.x, f.z); };
        if (t < 1.0f) driveCar(world, demos, 0, 0, false);
        else if (t < 5.0f) driveCar(world, demos, 1, 0, false);
        else if (t < 8.0f) driveCar(world, demos, 0.5f, 1, false);
        else driveCar(world, demos, 0, 0, true);
        if (t > 1.0f && t < 1.1f) heading0 = heading();
        if (t > 4.9f && speedAtFive == 0.0f) {
            speedAtFive = speed();
            vec3 up = runtime::bodyPose(world, demos.car.chassis).rotation * vec3(0, 1, 0);
            vec3 at = runtime::bodyPose(world, demos.car.chassis).position;
            check(*test, speedAtFive > 8.0f && up.y > 0.9f && at.z > -10.0f,
                  fmt::format("the car drives: {:.1f} m/s after 4 s, {:.0f} m on, upright ({:.2f})", speedAtFive, at.z + 30.0f, up.y));
        }
        if (t > 8.0f && !steered) {
            steered = true;
            float turned = heading() - heading0;
            while (turned > 3.14159f) turned -= 6.28318f;
            while (turned < -3.14159f) turned += 6.28318f;
            check(*test, turned > 0.8f, fmt::format("it steers: turned {:.0f} degrees right in 3 s", turned * 57.3f));
        }
        if (t > 11.0f) {
            check(*test, speed() < 1.0f, fmt::format("it brakes: {:.2f} m/s after 3 s of braking", speed()));
            // Every wheel still where it belongs on the chassis, and no joint gone.
            runtime::BodyState c = runtime::bodyPose(world, demos.car.chassis);
            float worst = 0.0f;
            for (int i = 0; i < 4; i++) {
                vec3 local = glm::conjugate(c.rotation) * (runtime::bodyPose(world, demos.car.wheels[i]).position - c.position);
                worst = std::max(worst, glm::length(local - demos.car.mounts[i]));
            }
            int alive = 0;
            for (int i = 0; i < 4; i++) alive += runtime::jointState(world, demos.car.axles[i]).alive;
            for (int i = 0; i < 2; i++) alive += runtime::jointState(world, demos.car.steering[i]).alive;
            check(*test, alive == 6 && worst < 0.1f,
                  fmt::format("the wheels stayed on: {} of 6 joints, worst {:.3f} m from its mount", alive, worst));
            go(8);
            test->start.clear();
            return;
        }
    }
    if (stage == 8) {
        static float startHeight = 0.0f;
        if (t > 1.0f && startHeight == 0.0f) {
            startHeight = runtime::bodyPose(world, demos.winch.weight).position.y;
            for (float& a : demos.winch.turned) a = 0.0f;
        }
        if (t > 13.0f) {
            const float* turned = demos.winch.turned;
            // Turned by its pegs alone: the winch gear the other way, at half the driver's speed.
            float ratio = -turned[1] / turned[0];
            check(*test, -turned[0] > 9.0f, fmt::format("the motor turned the driver {:.1f} rad in 12 s", -turned[0]));
            check(*test, std::abs(ratio - 0.5f) < 0.04f,
                  fmt::format("the driver turns the winch gear at their peg ratio: {:.3f} (0.500)", ratio));
            float risen = runtime::bodyPose(world, demos.winch.weight).position.y - startHeight;
            check(*test, risen > 1.0f, fmt::format("the winch lifted the weight {:.2f} m", risen));
            projv::core::info("DEMOTEST: winch physics step {:.2f} ms", runtime::physicsWorld(world).stats().lastStepMilliseconds);
            int alive = 0;
            for (Entity j : demos.winch.chain) alive += runtime::jointState(world, j).alive;
            check(*test, alive == int(demos.winch.chain.size()), fmt::format("the chain holds: {} of {} joints", alive, demos.winch.chain.size()));
            stage = 9;
        }
    }
    if (stage == 9) {
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
    demos.plank = prototype(scene, "plank", ivec3(10, 1, 4), 0.125f, [](int x, int, int z) {
        return (x + z) % 3 ? rgb(0.66f, 0.46f, 0.26f) : rgb(0.58f, 0.39f, 0.21f);
    });
    demos.link = prototype(scene, "link", ivec3(2, 6, 2), 0.125f, [](int, int y, int) {
        return y % 2 ? rgb(0.5f, 0.52f, 0.56f) : rgb(0.36f, 0.38f, 0.42f);
    });
    demos.wheel = prototype(scene, "wheel", ivec3(3, 7, 7), 0.125f, [](int x, int y, int z) -> uint32_t {
        vec2 d(float(y) + 0.5f - 3.5f, float(z) + 0.5f - 3.5f);
        float r = glm::length(d);
        if (r > 3.5f) return 0u;
        return r < 1.6f ? rgb(0.75f, 0.75f, 0.78f) : (x == 1 ? rgb(0.12f, 0.12f, 0.13f) : rgb(0.18f, 0.18f, 0.2f));
    });
    demos.knuckle = prototype(scene, "knuckle", ivec3(2), 0.125f, [](int, int, int) { return rgb(0.3f, 0.3f, 0.32f); });
    demos.chainLink = prototype(scene, "chain link", ivec3(2, 4, 2), 0.05f, [](int, int y, int) {
        return y % 2 ? rgb(0.62f, 0.64f, 0.68f) : rgb(0.42f, 0.44f, 0.48f);
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
                              demos.balls[3], demos.balls[4], demos.plank, demos.link, demos.wheel, demos.knuckle, demos.chainLink})
        utils::setComponentTransform(scene, p, vec3(0, -1000, 0), quat(1, 0, 0, 0), 1.0f);

    // A blast cuts bricks in pieces; each piece is counted. Something that leaves the world from
    // above the floor went through it, which must never happen.
    app.events().on<projv::BodySplit>([&world](const projv::BodySplit& e) {
        world.ctx().get<Demos>().pieces += int(e.pieces.size());
    });
    app.events().on<projv::JointBroke>([&world](const projv::JointBroke& e) {
        world.ctx().get<Demos>().brokenJoints.push_back(e.entity);
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

    demos.unattended = unattended;
    if (unattended) {
        SelfTest& test = world.ctx().emplace<SelfTest>();
        if (const char* only = std::getenv("DEMOS_ONLY"))
            for (const char* c = only; *c; c++)
                if (*c >= '1' && *c <= '8') test.only.push_back(*c - '0');
    }
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
    app.addSystem(projv::Stage::PostUpdate,  "follow camera", followCamera, {.after = "physics: present"});
    app.addSystem(projv::Stage::Render,      "render",    render);
    app.addSystem(projv::Stage::Shutdown,    "shutdown",  shutdown);
    app.run();
    return 0;
}
