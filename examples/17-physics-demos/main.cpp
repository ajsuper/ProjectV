// ProjectV Physics Demos
//
// Ten set pieces for the engine's physics (runtime/physics.h), each built from voxels in code:
//
//   1  Pyramid        a stepped pyramid of 1496 crates, sixteen high on a 16x16 base, and beside
//                     it two stepped towers side by side, thirty crates high on 5x5 bases.
//                     Click throws a heavy ball. F launches a 4 m ball where you look: kinematic,
//                     at a constant 30 m/s (ConstantVelocity), ploughing through whatever is there.
//   2  Wall blast     a staggered wall of 120 voxel bricks, a tower, a house, columns and a long low wall,
//                     all brick. Click sets off a big blast where the crosshair is:
//                     the bricks near it are carved and split into fragments, and everything is
//                     thrown outward.
//   3  Ball barrage   balls of five sizes -- the same density, so very different masses -- fired
//                     at a wall of crates, one after another.
//   4  Sweeper        a 12 m beam spinning about its middle while its middle circles, sweeping
//                     through a field of crates and balls. It is kinematic: moved by its Transform,
//                     pushing what it meets, pushed by nothing.
//   5  Titans         a 14 m stone boulder and a 20 m stone hammer, thousands of tonnes each,
//                     launched into each other across a field of 25 kg crates. Click: again.
//   6  Joints         a plank bridge on ball joints, a wrecking ball on a rope swinging into a weight
//                     on a breakable chain, a motor-driven windmill, a swinging door. Click: drop a
//                     crate on the bridge and fire a ball at the door.
//   7  Car            a car made of joints you drive: W/S throttle and reverse, A/D steer, Space
//                     brake. Rear wheels on driven hinges, front wheels on knuckles steered by servo
//                     hinges. Ramps, a jump, crates and balls to drive into.
//   8  Gear winch     a four-peg gear on a motor turning an eight-peg gear by its pegs, whose drum
//                     winds up a chain lifting a weight. Up (held): raise. Let go: it holds.
//                     Down (held): the motor lets go and the weight unwinds it.
//   9  Trebuchet      a counterweight trebuchet: a 36 t counterweight swings a 10.5 m arm, which
//                     whips a 2 t boulder round on a sling and lets it go, at four 32 x 24 m walls
//                     of stone 34 m off -- one on each side, each a seamless static grid. Where it strikes,
//                     the stone shatters into shards that fly. Click: fire; again, a new boulder and
//                     fire. Left/Right: turn it on its turntable. Up/Down: a higher or flatter
//                     throw (the angle the sling lets go at).
//  10  Slide          physics materials (physics_materials.json, beside the binary): three 14 m
//                     ramps of ice, polished wood and slate, the same crate on each, sliding down
//                     into walls of rubber, oak and clay. Each ramp is striped in three colours that
//                     all name one material, so it still collides as one box per grid cell; the
//                     crates hit their walls at the speeds their ramps allow and bounce back as far
//                     as their walls allow. Click: again.
//
// Controls
//   1-9, 0     pick a demo (0 is 10; it is rebuilt)          R  restart the demo
//   click      the demo's action                             V  walk (a character) / fly
//   mouse look, W/A/S/D fly, Space/Shift up/down             T  slow motion   P  pause
//   right-drag hold and drag the body under the crosshair (the wheel: nearer / farther)
//   Esc        release the cursor (a click recaptures it)
//   H  sun shadows on/off   O  ambient occlusion on/off   U  upscaler off (the raw quarter-res frame)
//   G  where the upscaler rebuilt the pixel (green) or fell back to filtering (red)
//
// The renderer (demoRenderer/) traces a quarter of the window's pixels -- half its width and height
// -- with a sun shadow ray each, adds screen-space AO, and rebuilds the frame at full resolution voxel
// by voxel, antialiased (reconstruct.frag).
//
//   DEMOS_SELFTEST=1 ./physics_demos    runs every demo unattended and checks it
//   DEMOS_BENCH=10 ./physics_demos      times the GPU on demo 10 with the shadow ray on and off, interleaved
//   DEMOS_LOOK=ho                        shadows (h) and AO (o) off; u: upscaler off; g: its debug view
//   (DEMOS_ONLY=9 or DEMOS_ONLY=2,10: just those)

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
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
#include "utils/material.h"
#include "utils/scene_query.h"

using projv::ComponentHandle;
using projv::Entity;
using projv::World;
using glm::ivec2;
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
constexpr float FLY_SPEED = 20.0f;
constexpr float ORB_RADIUS = 2.0f;   // demo 1's kinematic ball
// How quickly the walker's and the fly camera's velocity closes on what the keys ask for (1/s): the
// walker gets ~95% of the way in a quarter second on the ground, and steers only a little in the air.
constexpr float WALK_GRIP = 12.0f, AIR_GRIP = 1.5f, FLY_GRIP = 5.0f;

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

// What the renderer draws (renderParams): sun shadows, AO, the upscaler, its debug view.
struct Look {
    bool shadows = true, ambientOcclusion = true, upscale = true, upscaleDebug = false;
    bool marchHeat = false;   // the G-buffer's march statistics, as a heat map
};

struct Camera {
    vec3 position{0.0f, 6.0f, -22.0f};
    float yaw = 1.5708f, pitch = -0.12f;
    Entity walker = projv::NullEntity;
    vec3 velocity{0.0f};   // flying, or the walker's horizontal walk: eased toward what the keys ask for
    vec3 forward() const { return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)}; }
};

struct Demos {
    int current = 0;                       // 1..4
    // Prototypes, made once at startup and never drawn: every crate, brick and ball is a copy.
    ComponentHandle crate, brick, beamProto, balls[5], plank, link, wheel, knuckle, chainLink, orb, rockProto;
    std::vector<Entity> orbs;              // demo 1's kinematic balls, oldest first
    struct Trebuchet {
        Entity arm = projv::NullEntity, boulder = projv::NullEntity;
        std::vector<Entity> parts;         // the machine: frame, arm, counterweight, joints, sling links
        std::vector<Entity> oldStones;     // boulders thrown, oldest first
        float aim = 0.0f;                  // rad about the vertical, from +x toward -z: the machine as built
        float fireIn = -1.0f;              // seconds to the automatic fire after a rebuild
        Entity frame = projv::NullEntity, turntable = projv::NullEntity;
        float turnSpeed = 0.0f;            // what the turntable's motor was last told, rad/s
        vec3 impactAt{0.0f}, impactDirection{1, 0, 0};
        int kickIn = 0;                    // steps to the shards' kick after a strike
        // The release pin: the sling lets go when the boulder flies this far above the horizontal
        // (rad). Up and Down bend it: higher arcs, flatter shots.
        float releaseAngle = 0.2f;
        Entity axle = projv::NullEntity;   // the arm's hinge; its servo is the latch
        Entity sling = projv::NullEntity;  // the boulder's fixed joint to the arm's tip
        // A wall: one static grid of 128 x 96 x 4 voxels (32 m x 24 m x 1 m), and which of its
        // voxels are still there. Local x along the wall, y up, z through it toward the middle.
        struct Wall { Entity entity = projv::NullEntity; vec3 corner{0.0f}; quat rotation{1, 0, 0, 0}; std::vector<uint8_t> solid; };
        std::vector<Wall> walls;
        std::vector<Entity> slingLinks;    // drawn only: no bodies (in parts too)
        float releaseSpeed = 0.0f;
        bool fired = false, released = false, struck = false;
        vec3 releasedAt{0.0f};
        float impactSpeed = 0.0f;
    } trebuchet;
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
        // What the driver's motor is doing: winding up, holding still (at zero speed), or let go.
        enum class Mode { Hold, Raise, Release } mode = Mode::Hold;
        float raiseSpeed = -1.0f;          // rad/s at the driver: clockwise winds the weight up
        float turned[2]{};                 // each gear's rotation so far, rad
    } winch;
    // Demo 10: three ramps of different materials, a crate on each, a wall at each foot.
    struct Slide {
        struct Track {
            const char* ramp = "";         // the ramp's and the wall's physics materials
            const char* wall = "";
            Entity rampEntity = projv::NullEntity, wallEntity = projv::NullEntity, crate = projv::NullEntity;
            vec3 corner{0.0f};             // the ramp's top corner; its local x runs down the slope
            // Measured along the slope, of the crate's centre: the fastest it went before the wall,
            // where it struck, and the furthest back up it came after.
            float fastest = 0.0f, impactSpeed = 0.0f, struckAt = 0.0f, backTo = 0.0f;
            bool struck = false, reported = false;
            float struckClock = 0.0f;
        };
        Track tracks[3];
        quat rotation{1, 0, 0, 0};
    } slide;
    // What right-click holds: a body, the point on it that was grabbed (in the body's frame), and how
    // far in front of the camera that point is pulled.
    struct Grab {
        Entity body = projv::NullEntity;
        vec3 local{0.0f};
        float distance = 0.0f;
    } grab;
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
        case 1: return "Pyramid  (Click: throw a ball, F: launch a kinematic ball)";
        case 2: return "Wall blast  (Click: a blast where you aim)";
        case 3: return "Ball barrage";
        case 4: return "Sweeper";
        case 5: return "Titans  (Click: again)";
        case 6: return "Joints  (Click: a crate on the bridge, a ball at the door)";
        case 7: return "Car  (W/S drive, A/D steer, Space brake)";
        case 8: return "Gear winch  (Up: raise, Down: let it unwind)";
        case 9: return "Trebuchet  (Click: fire / reload and fire, Left/Right: turn, Up/Down: higher/flatter)";
        case 10: return "Slide  (ice, polished wood, slate into rubber, oak, clay -- Click: again)";
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
    // The sling's links are drawn only -- no RigidBody -- so the sweep above misses them.
    for (Entity e : demos.trebuchet.parts) if (world.valid(e)) world.destroy(e);
    demos.trebuchet = Demos::Trebuchet{};
    demos.slide = Demos::Slide{};
    auto& camera = world.ctx().get<Camera>();
    if (camera.walker != projv::NullEntity) { world.destroy(camera.walker); camera.walker = projv::NullEntity; }
    demos.beam = projv::NullEntity;
    demos.fired.clear();
    demos.orbs.clear();
    demos.watched.clear();
    demos.clock = demos.fireTimer = 0.0f;
    demos.blastKickIn = 0;
    demos.grab = Demos::Grab{};
}

void buildPyramid(World& world, Demos& demos, Camera& camera) {
    // Crates 1 m on a side, 2 cm apart, each layer 5 mm above the last so nothing starts touching.
    const float size = 1.0f, gap = 0.02f, pitch = size + gap;
    auto crate = [&](float x, int layer, float z) {
        return place(world, demos.crate, vec3(x, 0.5f * size + float(layer) * (size + 0.005f), z), quat(1, 0, 0, 0), body(40.0f));
    };
    // The pyramid: square layers 16x16, 15x15 ... 1x1, each centred on the one below.
    for (int layer = 0; layer < 16; layer++) {
        int count = 16 - layer;
        for (int i = 0; i < count; i++)
            for (int k = 0; k < count; k++) {
                Thing t = crate((float(i) - float(count - 1) * 0.5f) * pitch, layer, (float(k) - float(count - 1) * 0.5f) * pitch);
                if (layer == 15) demos.watched.push_back(t.entity);   // the top crate
            }
    }
    // Beside it, two towers side by side, a metre apart: solid, stepping in as they rise -- 5x5 for
    // twelve layers, 4x4 for ten, 3x3 for eight, thirty high. Their layers start 1 mm apart, not
    // 5, so a tall stack doesn't settle with a thump that sets it swaying.
    for (float cx : {14.0f, 20.0f})
        for (int layer = 0; layer < 30; layer++) {
            int n = layer < 12 ? 5 : layer < 22 ? 4 : 3;
            for (int i = 0; i < n; i++)
                for (int k = 0; k < n; k++)
                    place(world, demos.crate,
                          vec3(cx + (float(i) - float(n - 1) * 0.5f) * pitch, 0.5f * size + float(layer) * (size + 0.001f),
                               (float(k) - float(n - 1) * 0.5f) * pitch),
                          quat(1, 0, 0, 0), body(40.0f));
        }
    camera.position = vec3(5, 16, -55);
    camera.yaw = 1.5708f;
    camera.pitch = -0.05f;
}

// A 4 m ball from just in front of the camera, flying on where it looks at 30 m/s and turning
// over as it goes: a kinematic body with a ConstantVelocity -- no gravity, nothing slows it, and
// what it hits is shoved aside. Six at most; the oldest goes.
void launchOrb(World& world, Demos& demos, const Camera& camera) {
    vec3 forward = camera.forward();
    projv::RigidBody rigid;
    rigid.motion = runtime::MotionType::Kinematic;
    rigid.shape = projv::RigidBody::Shape::Sphere;
    Thing t = place(world, demos.orb, camera.position + forward * 7.0f, quat(1, 0, 0, 0), rigid);
    vec3 axis = glm::cross(forward, vec3(0, 1, 0));
    vec3 spin = glm::length(axis) > 1e-3f ? -glm::normalize(axis) * 3.0f : vec3(0);   // rolling forward
    world.emplace<projv::ConstantVelocity>(t.entity, projv::ConstantVelocity{forward * 30.0f, spin});
    demos.orbs.push_back(t.entity);
    if (demos.orbs.size() > 6) {
        if (world.valid(demos.orbs.front())) world.destroy(demos.orbs.front());
        demos.orbs.erase(demos.orbs.begin());
    }
}

// Bricks for demo 2's buildings: 1 m x 0.5 m x 0.5 m, along x, or along z turned a quarter about y.
// Horizontal positions are spread 0.6% from `corner` so no two bricks start touching.
void brick(World& world, Demos& demos, vec3 corner, vec3 centre, bool alongZ) {
    const quat turned(std::cos(0.7854f), 0.0f, std::sin(0.7854f), 0.0f);
    place(world, demos.brick, corner + vec3(centre.x * 1.006f, centre.y, centre.z * 1.006f),
          alongZ ? turned : quat(1, 0, 0, 0), body(30.0f));
}

// A rectangle of brick walls, `w` x `d` metres outside, half a metre thick, `courses` high. The
// corners interlock: even courses run the front and back walls through, odd ones the sides. A
// doorway `doorWidth` bricks wide, `doorCourses` high, starts `doorAt` metres along the front wall
// (z = 0, the side facing the camera). `d` of 1 is a single straight wall along x.
void brickBox(World& world, Demos& demos, vec3 corner, int w, int d, int courses, int doorAt = -1, int doorWidth = 0,
              int doorCourses = 0) {
    for (int c = 0; c < courses; c++) {
        const float y = 0.25f + float(c) * 0.503f;
        const bool through = d == 1 || c % 2 == 0;
        // Front and back: through, w bricks from the corner; otherwise w - 1, inset half a brick.
        int n = through ? w : w - 1;
        float start = through ? 0.5f : 1.0f;
        for (int i = 0; i < n; i++) {
            float x = start + float(i);
            bool door = c < doorCourses && x > float(doorAt) && x < float(doorAt + doorWidth) + 0.5f;
            if (!door) brick(world, demos, corner, vec3(x, y, 0.25f), false);
            if (d > 1) brick(world, demos, corner, vec3(x, y, float(d) - 0.25f), false);
        }
        if (d == 1) continue;
        // The sides, the other way round.
        n = through ? d - 1 : d;
        start = through ? 1.0f : 0.5f;
        for (int i = 0; i < n; i++) {
            brick(world, demos, corner, vec3(0.25f, y, start + float(i)), true);
            brick(world, demos, corner, vec3(float(w) - 0.25f, y, start + float(i)), true);
        }
    }
}

// A 1 m square column: two bricks a course, side by side, crossing the course below.
void brickColumn(World& world, Demos& demos, vec3 corner, int courses) {
    for (int c = 0; c < courses; c++) {
        const float y = 0.25f + float(c) * 0.503f;
        bool alongZ = c % 2 == 1;
        for (int i = 0; i < 2; i++) {
            float across = 0.25f + float(i) * 0.5f;
            brick(world, demos, corner, alongZ ? vec3(across, y, 0.5f) : vec3(0.5f, y, across), alongZ);
        }
    }
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
    demos.blastAt = vec3(0.3f, 2.2f, 0.9f);   // behind the wall, a little off-centre (the self-test's blast)

    // Round it, more to blow up: a tower, a house with a doorway, a row of columns, and a long low
    // wall at the back.
    brickBox(world, demos, vec3(-17, 0, 4), 4, 4, 16);
    brickBox(world, demos, vec3(9, 0, 4), 7, 5, 9, 3, 2, 5);
    for (int i = 0; i < 5; i++) brickColumn(world, demos, vec3(-8.0f + float(i) * 4.0f, 0, 12), 14);
    brickBox(world, demos, vec3(-12, 0, 20), 24, 1, 5);
    camera.position = vec3(0, 7.0f, -22);
    camera.yaw = 1.5708f;
    camera.pitch = -0.18f;
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
    // Light wood (balsa-ish): heavy gears are slow to spin up, and a released winch should run
    // back down at a pace worth watching.
    wheel.density = 150.0f;
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
            axle.motorMode = runtime::MotorMode::Position;   // holding where it starts until told otherwise
            axle.motorFrequency = 10.0f;                     // a stiff servo: the weight barely sags
            axle.motorLimit = 3000.0f;
        }
        w.hinges[i] = joint(world, demos, w.gears[i], projv::NullEntity, axle);
    }
    // A wall behind the whole works (behind the drum, which is behind the gears), to see them against.
    scenery(world, demos, ivec3(80, 40, 2), 0.125f, vec3(-1.5f, 3.5f, 1.3f), [](int x, int yy, int) {
        return (x / 8 + yy / 8) % 2 ? rgb(0.24f, 0.25f, 0.28f) : rgb(0.21f, 0.22f, 0.25f);
    });

    // The chain: 24 links of 0.2 m from the right of the drum, down to a 40 kg weight. Each link to
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
    Thing weight = place(world, demos.crate, top - vec3(0, 4.8f + 0.5f, 0), quat(1, 0, 0, 0), body(40.0f));
    w.weight = weight.entity;
    projv::Joint hook;
    hook.type = runtime::JointType::Point;
    hook.anchor = top - vec3(0, 4.8f, 0);
    w.chain.push_back(joint(world, demos, weight.entity, previous, hook));

    camera.position = vec3(0.9f, 4.5f, -7.5f);
    camera.yaw = 1.5708f;
    camera.pitch = 0.0f;
}

// ---- Demo 9: a trebuchet ------------------------------------------------------------------------

// The arm turns about a pivot 7 m up on a hinge whose servo is the latch: it holds the arm cocked,
// its long end down at the back and the counterweight up at the front. The boulder lies on the
// ground under the frame, on a 6 m sling (a rope: a Distance joint) from the arm's tip. Fired, the
// counterweight falls, the long end sweeps up and over, and the sling whips the boulder round after
// it, far faster than the tip itself moves. The release is the trebuchet's pin: the sling lets go
// once the boulder is flying at the pin's angle above the horizontal (11 degrees to begin with:
// flat enough to strike a wall, not sail over it; Up and Down change it). Built in its own frame, throwing along +x with the arm in the x-y plane, then
// turned about the vertical by the aim.
constexpr float TREB_PIVOT = 7.0f, TREB_LONG = 8.0f, TREB_SHORT = 2.5f, TREB_SLING = 6.0f;
constexpr float TREB_COCKED = 3.9897f;   // 228.6 degrees: the tip a metre off the ground, at the back
constexpr float TREB_ARM_MIDDLE = 0.5f * (TREB_LONG - TREB_SHORT);   // the arm's box centre, from the pivot
// Four walls round the trebuchet, 34 m out on each side, inside the 80 m floor.
constexpr float WALL_DISTANCE = 34.0f, WALL_VOXEL = 0.25f;
constexpr int WALL_W = 128, WALL_H = 96, WALL_T = 4;

// Ashlar: blocks 1.5 m long and 0.75 m high, each course offset by half a block.
uint32_t wallStone(int x, int y, int z) {
    bool mortar = y % 3 == 0 || (x + (y / 3) % 2 * 3) % 6 == 0;
    return mortar ? rgb(0.62f, 0.6f, 0.55f) : stone(x, y, z);
}
constexpr int SLING_LINKS = 16;

vec3 trebuchetTip(World& world, const Demos::Trebuchet& tb) {
    runtime::BodyState arm = runtime::bodyPose(world, tb.arm);
    return arm.position + arm.rotation * vec3(TREB_LONG - TREB_ARM_MIDDLE, 0, 0);
}

// The sling drawn: links strung tip to boulder while it holds, then hanging from the tip.
void drawSling(World& world, Demos& demos) {
    Demos::Trebuchet& tb = demos.trebuchet;
    if (tb.slingLinks.empty() || !world.valid(tb.arm)) return;
    vec3 tip = trebuchetTip(world, tb);
    vec3 end = tb.released || !world.valid(tb.boulder) ? tip - vec3(0, 3.0f, 0) : runtime::bodyPose(world, tb.boulder).position;
    vec3 along = end - tip;
    float length = glm::length(along);
    quat turn = length > 1e-3f ? quat(vec3(0, 1, 0), along / length) : quat(1, 0, 0, 0);
    for (size_t k = 0; k < tb.slingLinks.size(); k++) {
        float f = (float(k) + 0.5f) / float(tb.slingLinks.size());
        if (world.valid(tb.slingLinks[k]))
            world.emplace_or_replace<projv::Transform>(tb.slingLinks[k], projv::Transform{tip + along * f, turn});
    }
}

// Takes the machine away: frame, arm, counterweight, sling and its joints. A boulder already thrown
// stays where it lies (the last eight do); one still in the sling goes with the machine.
void clearTrebuchet(World& world, Demos& demos) {
    Demos::Trebuchet& tb = demos.trebuchet;
    for (Entity e : tb.parts) if (world.valid(e)) world.destroy(e);
    tb.parts.clear();
    tb.slingLinks.clear();
    if (world.valid(tb.boulder)) {
        if (tb.released) {
            tb.oldStones.push_back(tb.boulder);
            if (tb.oldStones.size() > 8) {
                if (world.valid(tb.oldStones.front())) world.destroy(tb.oldStones.front());
                tb.oldStones.erase(tb.oldStones.begin());
            }
        } else {
            world.destroy(tb.boulder);
        }
    }
    tb.boulder = tb.arm = tb.axle = tb.sling = tb.frame = tb.turntable = projv::NullEntity;
    tb.fired = tb.released = tb.struck = false;
    tb.fireIn = -1.0f;
}

// The machine, cocked, a new boulder in its sling, facing `tb.aim`.
void buildTrebuchetMachine(World& world, Demos& demos) {
    Demos::Trebuchet& tb = demos.trebuchet;
    clearTrebuchet(world, demos);
    auto& scene = world.ctx().get<projv::Scene>();
    const quat aim = glm::angleAxis(tb.aim, vec3(0, 1, 0));   // +x turned to the aim
    const vec3 pivot(0, TREB_PIVOT, 0);
    const vec3 d = aim * vec3(std::cos(TREB_COCKED), std::sin(TREB_COCKED), 0);   // along the arm, to the long end
    const quat cocked = aim * glm::angleAxis(TREB_COCKED, vec3(0, 0, 1));
    auto wood = [](int x, int y, int z) { return (x / 4) % 2 ? rgb(0.5f, 0.33f, 0.18f) : rgb(0.58f, 0.4f, 0.22f); };
    auto keep = [&](Entity e) { tb.parts.push_back(e); return e; };

    // The frame: a post either side of the arm, up to the pivot, on two 8 m rails -- one 30 t body,
    // standing 5 cm off the floor on a turntable: a hinge about the vertical whose motor the arrow
    // keys drive. The arm's axle is on the frame, so it turns with it, mid-throw too.
    projv::RigidBody frameBody;
    frameBody.mass = 30000.0f;
    frameBody.angularDamping = 0.0f;
    tb.frame = keep(big(world, "frame", ivec3(40, 35, 16), 0.2f, vec3(0, 0.05f + 3.5f, 0), aim, [](int x, int y, int z) -> uint32_t {
        bool side = z <= 1 || z >= 14, rail = y <= 1, post = x >= 18 && x <= 21;
        if (!side || !(rail || post)) return 0u;
        return (rail ? x : y) / 4 % 2 ? rgb(0.5f, 0.33f, 0.18f) : rgb(0.58f, 0.4f, 0.22f);
    }, frameBody));
    projv::Joint turntable;
    turntable.type = runtime::JointType::Hinge;
    turntable.anchor = vec3(0, 0.05f, 0);
    turntable.axis = vec3(0, 1, 0);
    turntable.motor = true;
    turntable.motorVelocity = 0.0f;
    turntable.motorLimit = 3.0e6f;
    tb.turntable = keep(joint(world, demos, tb.frame, projv::NullEntity, turntable));
    tb.turnSpeed = 0.0f;
    // The arm: 10.5 m from its short end to its long.
    projv::RigidBody armBody;
    armBody.shape = projv::RigidBody::Shape::Box;
    armBody.mass = 1500.0f;
    armBody.angularDamping = 0.0f;
    tb.arm = keep(big(world, "arm", ivec3(70, 4, 4), 0.15f, pivot + d * TREB_ARM_MIDDLE, cocked, wood, armBody));
    // The counterweight, a 1.6 m iron-banded box on the short end, fixed to it.
    projv::RigidBody weightBody;
    weightBody.shape = projv::RigidBody::Shape::Box;
    weightBody.mass = 36000.0f;
    weightBody.angularDamping = 0.0f;
    Entity counterweight = keep(big(world, "counterweight", ivec3(8), 0.2f, pivot - d * TREB_SHORT, cocked, [](int x, int y, int z) {
        return (y == 1 || y == 6) ? rgb(0.25f, 0.25f, 0.27f) : stone(x, y, z);
    }, weightBody));
    // The boulder: 1.4 m of stone, 2 t, a sphere to the simulation, lying on the ground ahead of
    // the tip with the sling straight.
    const vec3 tip = pivot + d * TREB_LONG;
    const vec3 rest = vec3(tip.x, 0.7f, tip.z) +
                      aim * vec3(std::sqrt(TREB_SLING * TREB_SLING - (tip.y - 0.7f) * (tip.y - 0.7f)), 0, 0);
    projv::RigidBody rock = ballBody(800.0f, 0.1f);
    rock.mass = 2000.0f;
    rock.friction = 0.3f;
    rock.quality = projv::RigidBody::Quality::Continuous;
    tb.boulder = place(world, demos.rockProto, rest, quat(1, 0, 0, 0), rock).entity;

    projv::Joint fixed;
    fixed.type = runtime::JointType::Fixed;
    fixed.anchor = pivot - d * TREB_SHORT;
    keep(joint(world, demos, counterweight, tb.arm, fixed));
    projv::Joint sling;
    sling.type = runtime::JointType::Distance;
    sling.anchor = rest;
    sling.anchorB = tip;
    sling.limitMin = 0.0f;
    sling.limitMax = TREB_SLING;
    tb.sling = keep(joint(world, demos, tb.boulder, tb.arm, sling));
    projv::Joint axle;
    axle.type = runtime::JointType::Hinge;
    axle.anchor = pivot;
    axle.axis = aim * vec3(0, 0, 1);
    axle.motor = true;   // the latch: a stiff servo holding it cocked
    axle.motorMode = runtime::MotorMode::Position;
    axle.motorTarget = 0.0f;
    axle.motorFrequency = 8.0f;
    axle.motorLimit = 5.0e6f;
    tb.axle = keep(joint(world, demos, tb.arm, tb.frame, axle));

    // The sling's links: drawn only (no bodies), strung along it each step.
    for (int k = 0; k < SLING_LINKS; k++) {
        ComponentHandle h = utils::duplicateComponent(scene, demos.chainLink);
        tb.slingLinks.push_back(keep(runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy)));
    }
    drawSling(world, demos);
}

void buildTrebuchet(World& world, Demos& demos, Camera& camera) {
    Demos::Trebuchet& tb = demos.trebuchet;
    tb.aim = 0.0f;
    buildTrebuchetMachine(world, demos);
    // Four walls round it, each one seamless static grid, 32 m by 24 m and 1 m thick: a strike
    // breaks out its middle and leaves its edges and top standing.
    auto& scene = world.ctx().get<projv::Scene>();
    for (int side = 0; side < 4; side++) {
        vec3 out = glm::angleAxis(float(side) * 1.5708f, vec3(0, 1, 0)) * vec3(1, 0, 0), along(-out.z, 0, out.x);
        Demos::Trebuchet::Wall wall;
        wall.rotation = glm::quat_cast(glm::mat3(along, vec3(0, 1, 0), -out));
        wall.corner = out * (WALL_DISTANCE + 0.5f) - along * (0.5f * float(WALL_W) * WALL_VOXEL);
        ComponentHandle h = voxels(scene, "wall", ivec3(WALL_W, WALL_H, WALL_T), WALL_VOXEL, wall.corner, wallStone);
        utils::setComponentTransform(scene, h, wall.corner, wall.rotation, 1.0f);
        wall.entity = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        world.emplace<projv::StaticCollider>(wall.entity);
        demos.owned.push_back(wall.entity);
        wall.solid.assign(size_t(WALL_W) * WALL_H * WALL_T, 1);
        tb.walls.push_back(std::move(wall));
    }
    camera.position = vec3(-24, 15, 0);   // behind the trebuchet, looking where it throws
    camera.yaw = 0.0f;
    camera.pitch = -0.12f;
}

// Click: lets the arm go -- or, once it has thrown, builds it again with a new boulder, facing the
// current aim, and lets it go when it has settled half a second.
void fireTrebuchet(World& world, Demos& demos) {
    Demos::Trebuchet& tb = demos.trebuchet;
    if (tb.fired) {
        // Built again facing where the turntable has it now.
        if (world.valid(tb.frame)) {
            vec3 ahead = runtime::bodyPose(world, tb.frame).rotation * vec3(1, 0, 0);
            tb.aim = std::atan2(-ahead.z, ahead.x);
        }
        buildTrebuchetMachine(world, demos);
        tb.fireIn = 0.5f;
        return;
    }
    if (!world.valid(tb.axle)) return;
    tb.fired = true;
    tb.fireIn = -1.0f;
    runtime::setJointMotor(world, tb.axle, false, 0.0f);
}

// Left/right: the turntable's motor, at `speed` rad/s (positive turns the throw from +x toward
// -z); 0 holds it where it is. Sent only when it changes.
void turnTrebuchet(World& world, Demos& demos, float speed) {
    Demos::Trebuchet& tb = demos.trebuchet;
    if (speed == tb.turnSpeed || !world.valid(tb.turntable)) return;
    tb.turnSpeed = speed;
    runtime::setJointMotor(world, tb.turntable, true, speed);
}

// Breaks a wall round a strike, the way stone shatters: Voronoi cells. Seeds are scattered over
// the wall's face round the strike (`at`, in its voxels), thick near it and thinning out to about 6 m;
// every voxel still there within that belongs to its nearest seed. Each cell is cut out of the
// static wall and comes back as a shard of its own -- a dynamic body made of exactly those voxels,
// where they were -- so the wall breaks into irregular pieces, small at the strike and larger
// further out, with nothing lost to the cracks. A cell more than 16 voxels across (one chunk) is
// cut in two. Beyond that the wall stands. Two steps on, the shards are thrown on, the boulder's way.
void crackWall(World& world, Demos& demos, Demos::Trebuchet::Wall& wall, vec2 at) {
    Demos::Trebuchet& tb = demos.trebuchet;
    auto& scene = world.ctx().get<projv::Scene>();
    // Seeds that break, out to 6 m; then a ring of seeds that stay whole, from 6 to 9 m. A voxel
    // nearest a whole seed stays in the wall, so the break's edge runs along cell boundaries --
    // jagged -- rather than round a circle.
    const float reach = 6.0f / WALL_VOXEL, outer = 1.5f * reach;
    std::vector<vec2> seeds;
    const int breaking = 70;
    for (int k = 0; k < breaking; k++) {
        float r = reach * std::pow(uniform(demos, 0.0f, 1.0f), 1.3f), angle = uniform(demos, 0.0f, 6.2832f);
        seeds.push_back(at + vec2(std::cos(angle), std::sin(angle)) * r);
    }
    for (int k = 0; k < 36; k++) {
        float r = uniform(demos, reach, outer), angle = (float(k) + uniform(demos, -0.4f, 0.4f)) * 6.2832f / 36.0f;
        seeds.push_back(at + vec2(std::cos(angle), std::sin(angle)) * r);
    }
    auto solid = [&](int x, int y, int z) -> uint8_t& { return wall.solid[(size_t(z) * WALL_H + y) * WALL_W + x]; };
    std::map<int, std::vector<ivec3>> cells;
    const ivec2 low = glm::max(ivec2(glm::floor(at - outer)), ivec2(0));
    const ivec2 high = glm::min(ivec2(glm::ceil(at + outer)), ivec2(WALL_W - 1, WALL_H - 1));
    for (int y = low.y; y <= high.y; y++)
        for (int x = low.x; x <= high.x; x++) {
            vec2 c = vec2(x, y) + vec2(0.5f);
            if (glm::length(c - at) > outer) continue;
            int best = 0;
            float bestDistance = 1e9f;
            for (int k = 0; k < int(seeds.size()); k++) {
                float distance = glm::length(c - seeds[k]);
                if (distance < bestDistance) { bestDistance = distance; best = k; }
            }
            if (best >= breaking) continue;   // stays whole
            for (int z = 0; z < WALL_T; z++)
                if (solid(x, y, z)) cells[best].push_back(ivec3(x, y, z));
        }
    // Cells too big for a chunk, halved until they fit.
    std::vector<std::vector<ivec3>> shards;
    std::vector<std::vector<ivec3>> pending;
    for (auto& [k, voxelsOf] : cells) pending.push_back(std::move(voxelsOf));
    while (!pending.empty()) {
        std::vector<ivec3> cell = std::move(pending.back());
        pending.pop_back();
        ivec3 lo(1 << 20), hi(-(1 << 20));
        for (const ivec3& v : cell) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
        ivec3 extent = hi - lo + 1;
        if (extent.x <= 16 && extent.y <= 16) { shards.push_back(std::move(cell)); continue; }
        int axis = extent.x >= extent.y ? 0 : 1, middle = (lo[axis] + hi[axis]) / 2;
        std::vector<ivec3> a, b;
        for (const ivec3& v : cell) (v[axis] <= middle ? a : b).push_back(v);
        pending.push_back(std::move(a));
        pending.push_back(std::move(b));
    }
    std::vector<projv::PendingVoxelOp> cut;
    projv::RigidBody rock = body(0.0f, 0.0f, 0.8f);
    rock.density = 500.0f;    // a fifth of stone's 2400: a wall that gives, for the show
    for (const std::vector<ivec3>& shard : shards) {
        if (shard.empty()) continue;
        ivec3 lo(1 << 20), hi(-(1 << 20));
        for (const ivec3& v : shard) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
        ivec3 size = hi - lo + 1;
        std::vector<uint8_t> in(size_t(size.x) * size.y * size.z, 0);
        for (const ivec3& v : shard) {
            ivec3 l = v - lo;
            in[(size_t(l.z) * size.y + l.y) * size.x + l.x] = 1;
            solid(v.x, v.y, v.z) = 0;
            cut.push_back({false, v, 0});
        }
        // An asset node at the shard's centre, its voxels inside, offset by minus half their size.
        vec3 centre = wall.corner + wall.rotation * ((vec3(lo) + 0.5f * vec3(size)) * WALL_VOXEL);
        ComponentHandle node = utils::addComponent(scene, projv::ComponentKind::Asset, "shard", projv::INVALID_COMPONENT_HANDLE, 4, 1.0f);
        voxels(scene, "shard", size, WALL_VOXEL, -0.5f * vec3(size) * WALL_VOXEL, [&](int x, int y, int z) -> uint32_t {
            return in[(size_t(z) * size.y + y) * size.x + x] ? wallStone(lo.x + x, lo.y + y, lo.z + z) : 0u;
        }, node);
        utils::setComponentTransform(scene, node, centre, wall.rotation, 1.0f);
        Entity e = runtime::spawnComponent(world, node, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        world.emplace<projv::RigidBody>(e, rock);
        runtime::assignNetId(world, e);
        demos.pieces++;
    }
    if (!cut.empty()) runtime::editVoxels(world, wall.entity, cut);
    tb.kickIn = 2;
}

// The kick after a strike: everything within 7 m of it, the new shards most of all, thrown on the
// way the boulder was going, scattered a little.
void kickShards(World& world, Demos& demos) {
    Demos::Trebuchet& tb = demos.trebuchet;
    for (Entity e : world.view<projv::RigidBody>()) {
        if (e == tb.boulder || e == tb.arm || e == tb.frame) continue;
        vec3 d = runtime::bodyPose(world, e).position - tb.impactAt;
        float distance = glm::length(d);
        if (distance > 7.0f) continue;
        float falloff = 1.0f - distance / 7.0f;
        vec3 scatter(uniform(demos, -1, 1), uniform(demos, 0, 1.5f), uniform(demos, -1, 1));
        runtime::addVelocity(world, e, (tb.impactDirection * 14.0f + scatter * 4.0f) * falloff);
    }
}

// Each step of demo 9: the delayed fire after a rebuild, the release, and the strike.
void trebuchetTick(World& world, Demos& demos) {
    Demos::Trebuchet& tb = demos.trebuchet;
    drawSling(world, demos);
    if (tb.fireIn >= 0.0f && (tb.fireIn -= 1.0f / 60.0f) < 0.0f) fireTrebuchet(world, demos);
    if (tb.kickIn > 0 && --tb.kickIn == 0) kickShards(world, demos);
    if (!tb.fired || !world.valid(tb.arm) || !world.valid(tb.boulder)) return;
    runtime::BodyState rock = runtime::bodyPose(world, tb.boulder);
    if (!tb.released) {
        vec3 ahead = runtime::bodyPose(world, tb.frame).rotation * vec3(1, 0, 0);
        float forward = glm::dot(rock.linearVelocity, ahead);
        float climb = std::atan2(rock.linearVelocity.y, forward);
        if (forward > 4.0f && std::abs(climb - tb.releaseAngle) < 0.15f) {
            tb.released = true;
            if (world.valid(tb.sling)) world.destroy(tb.sling);
            tb.releasedAt = rock.position;
            tb.releaseSpeed = glm::length(rock.linearVelocity);
        }
        return;
    }
    if (tb.struck) return;
    // Struck once it is within three steps of a wall's face, so the shards are out when it arrives.
    const float lead = 3.0f / 60.0f * glm::length(rock.linearVelocity);
    for (Demos::Trebuchet::Wall& wall : tb.walls) {
        vec3 l = glm::inverse(wall.rotation) * (rock.position - wall.corner) / WALL_VOXEL;
        const float face = float(WALL_T), near = (0.7f + lead) / WALL_VOXEL;
        if (l.x < -2.0f || l.x > float(WALL_W) + 2.0f || l.y > float(WALL_H) + 2.0f || l.z > face + near || l.z < -2.0f) continue;
        vec2 at(glm::clamp(l.x, 0.0f, float(WALL_W)), glm::clamp(l.y, 0.0f, float(WALL_H)));
        tb.struck = true;
        tb.impactSpeed = glm::length(rock.linearVelocity);
        tb.impactAt = wall.corner + wall.rotation * (vec3(at, face) * WALL_VOXEL);
        tb.impactDirection = glm::normalize(rock.linearVelocity);
        crackWall(world, demos, wall, at);
        return;
    }
}

// The winch's control: the driver's motor winding at raiseSpeed; holding -- a servo locked at the
// angle it had when told to hold, since a motor merely held at zero speed corrects no drift and the
// weight creeps down; or switched off, so the weight's pull runs the gears backwards. Commands only
// when the mode changes.
// ---- Demo 10: the slide ----

constexpr float SLIDE_ANGLE = 0.349f;      // 20 degrees: steeper than ice, wood or slate can hold a crate on
constexpr int SLIDE_LENGTH = 56;           // the ramps: 56 x 2 x 8 voxels of 25 cm, 14 m x 0.5 m x 2 m
constexpr int SLIDE_WIDTH = 8;
constexpr float SLIDE_VOXEL = 0.25f;
constexpr float SLIDE_CRATE_START = 1.0f;  // the crate's centre, metres down the slope from the top
// Where the crate's centre is when it touches the wall: the wall's face, 0.5 m short of the
// ramp's end, less half the crate.
constexpr float SLIDE_CRATE_AT_WALL = SLIDE_LENGTH * SLIDE_VOXEL - 0.5f - 0.5f;

// The crate's friction is 1.0, so the friction under it is sqrt(1 x the ramp's): 0.1 on ice, 0.24
// on polished wood, 0.32 on slate -- all under tan 20 degrees (0.36), so each slides, at its own
// pace. Its restitution is 0, so each bounce is its wall's.
projv::RigidBody slideCrate() {
    projv::RigidBody r = body(40.0f, 0.0f, 1.0f);
    r.linearDamping = 0.0f;   // so it slides as the friction says, and the self-test can check it
    return r;
}

// Static voxels turned by `rotation` about their corner at `corner`, every palette entry naming
// `material`.
template <typename ColourAt>
Entity materialScenery(World& world, Demos& demos, ivec3 size, vec3 corner, quat rotation, const char* material,
                       ColourAt colourAt) {
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle h = voxels(scene, material, size, SLIDE_VOXEL, corner, colourAt);
    utils::setComponentTransform(scene, h, corner, rotation, 1.0f);
    for (size_t slot = 0; slot < scene.components[h].materialPalette.size(); slot++)
        utils::setMaterialPhysics(scene, h, uint8_t(slot), material);
    Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    world.emplace<projv::StaticCollider>(e);
    demos.owned.push_back(e);
    return e;
}

void placeSlideCrates(World& world, Demos& demos) {
    Demos::Slide& slide = demos.slide;
    for (Demos::Slide::Track& t : slide.tracks) {
        if (world.valid(t.crate)) world.destroy(t.crate);
        vec3 centre = t.corner + slide.rotation * vec3(SLIDE_CRATE_START, 0.5f + 0.5f + 0.01f, 0.5f * SLIDE_WIDTH * SLIDE_VOXEL);
        t.crate = place(world, demos.crate, centre, slide.rotation, slideCrate()).entity;
        t.fastest = t.impactSpeed = t.struckAt = t.backTo = 0.0f;
        t.struck = t.reported = false;
    }
}

void buildSlide(World& world, Demos& demos, Camera& camera) {
    Demos::Slide& slide = demos.slide;
    slide.rotation = glm::angleAxis(-SLIDE_ANGLE, vec3(0, 0, 1));   // local +x runs down the slope
    const float length = SLIDE_LENGTH * SLIDE_VOXEL;
    // Each ramp's lowest edge on the floor, its top 4.8 m up on a pillar.
    const float top = length * std::sin(SLIDE_ANGLE);
    struct Look { const char* ramp; const char* wall; vec3 a, b, c; vec3 wallColour; };
    const Look looks[3] = {
        {"ice", "rubber", {0.78f, 0.9f, 0.98f}, {0.62f, 0.8f, 0.95f}, {0.9f, 0.96f, 1.0f}, {0.85f, 0.2f, 0.15f}},
        {"polished wood", "oak", {0.72f, 0.5f, 0.28f}, {0.62f, 0.42f, 0.22f}, {0.8f, 0.6f, 0.36f}, {0.5f, 0.33f, 0.16f}},
        {"slate", "clay", {0.3f, 0.32f, 0.36f}, {0.38f, 0.4f, 0.45f}, {0.24f, 0.26f, 0.3f}, {0.72f, 0.45f, 0.3f}},
    };
    for (int i = 0; i < 3; i++) {
        Demos::Slide::Track& t = slide.tracks[i];
        const Look& look = looks[i];
        t.ramp = look.ramp;
        t.wall = look.wall;
        t.corner = vec3(-8.0f, top, -5.0f + 4.0f * float(i));
        // Striped across in three colours -- three palette entries -- all one material: physics sees
        // one surface, and merges it as one.
        t.rampEntity = materialScenery(world, demos, ivec3(SLIDE_LENGTH, 2, SLIDE_WIDTH), t.corner, slide.rotation, look.ramp,
                                       [&look](int x, int, int) {
                                           int band = (x / 3) % 3;
                                           vec3 c = band == 0 ? look.a : band == 1 ? look.b : look.c;
                                           return rgb(c.r, c.g, c.b);
                                       });
        // The wall: on the ramp's surface at its foot, square to it, 2 m high and wider than the
        // ramp.
        vec3 wallCorner = t.corner + slide.rotation * vec3(length - 0.5f, 0.5f, -0.25f);
        t.wallEntity = materialScenery(world, demos, ivec3(2, 8, SLIDE_WIDTH + 2), wallCorner, slide.rotation, look.wall,
                                       [&look](int, int y, int) {
                                           float shade = y % 2 ? 1.0f : 0.85f;
                                           return rgb(look.wallColour.r * shade, look.wallColour.g * shade, look.wallColour.b * shade);
                                       });
        // A pillar under the top.
        scenery(world, demos, ivec3(4, int(top / SLIDE_VOXEL) - 2, SLIDE_WIDTH), SLIDE_VOXEL, vec3(-8.0f, 0.0f, t.corner.z),
                [](int, int y, int) { return y % 4 ? rgb(0.45f, 0.45f, 0.47f) : rgb(0.38f, 0.38f, 0.4f); });
    }
    placeSlideCrates(world, demos);
    // From beyond the walls, raised, looking back up the three ramps.
    camera.position = vec3(13.0f, 7.5f, -13.0f);
    camera.yaw = 2.36f;
    camera.pitch = -0.3f;
}

// Each crate's progress along its slope: the fastest it went before its wall, and how far back up
// it came after. Logged once it has settled.
void slideTick(World& world, Demos& demos) {
    Demos::Slide& slide = demos.slide;
    const vec3 down = slide.rotation * vec3(1, 0, 0);
    for (Demos::Slide::Track& t : slide.tracks) {
        if (!world.valid(t.crate) || !runtime::hasBody(world, t.crate)) continue;
        runtime::BodyState s = runtime::bodyPose(world, t.crate);
        float along = glm::dot(s.position - t.corner, down);
        float speed = glm::dot(s.linearVelocity, down);
        if (!t.struck) {
            t.fastest = std::max(t.fastest, speed);
            // Struck: moving back up, or near the wall and suddenly much slower.
            if (speed < -0.05f || (along > SLIDE_CRATE_AT_WALL - 0.3f && speed < 0.5f * t.fastest)) {
                t.struck = true;
                t.impactSpeed = t.fastest;
                t.struckAt = t.backTo = along;
                t.struckClock = demos.clock;
            }
        } else {
            t.backTo = std::min(t.backTo, along);
            if (!t.reported && demos.clock - t.struckClock > 2.5f) {
                t.reported = true;
                projv::core::info("Slide: on {}, into {}: struck at {:.2f} m/s, came {:.2f} m back up", t.ramp, t.wall,
                                  t.impactSpeed, t.struckAt - t.backTo);
            }
        }
    }
}

void setWinch(World& world, Demos& demos, Demos::Winch::Mode mode) {
    Demos::Winch& w = demos.winch;
    if (mode == w.mode || !world.valid(w.hinges[0])) return;
    w.mode = mode;
    using Mode = Demos::Winch::Mode;
    switch (mode) {
        case Mode::Raise:   runtime::setJointMotor(world, w.hinges[0], true, w.raiseSpeed); break;
        case Mode::Release: runtime::setJointMotor(world, w.hinges[0], false, 0.0f); break;
        case Mode::Hold:
            runtime::setJointMotor(world, w.hinges[0], true, runtime::jointState(world, w.hinges[0]).position,
                                   runtime::MotorMode::Position);
            break;
    }
}

void startDemo(World& world, int n) {
    auto& demos = world.ctx().get<Demos>();
    auto& camera = world.ctx().get<Camera>();
    clearDemo(world);
    demos.current = n;
    camera.velocity = vec3(0.0f);
    switch (n) {
        case 1: buildPyramid(world, demos, camera); break;
        case 2: buildWall(world, demos, camera); break;
        case 3: buildBarrage(world, demos, camera); break;
        case 4: buildSweeper(world, demos, camera); break;
        case 5: buildTitans(world, demos, camera); break;
        case 6: buildJoints(world, demos, camera); break;
        case 7: buildCarPark(world, demos, camera); break;
        case 8: buildWinch(world, demos, camera); break;
        case 9: buildTrebuchet(world, demos, camera); break;
        case 10: buildSlide(world, demos, camera); break;
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

// The action for a click.
void act(World& world, Demos& demos, const Camera& camera) {
    switch (demos.current) {
        // A wrecking ball: a metre across, as dense as iron, thrown where you are looking.
        case 1: fireBall(world, demos, 4, camera.position + camera.forward() * 2.0f, camera.forward() * 32.0f, 7800.0f); break;
        // Where the crosshair is; the self-test blasts its scripted spot behind the first wall.
        case 2:
            if (!demos.unattended) {
                auto hit = runtime::castRay(world, camera.position, camera.forward(), 150.0f, 0xffffffffu, camera.walker);
                if (!hit) break;
                demos.blastAt = hit->position + camera.forward() * 0.3f;   // just inside what it hit
            }
            detonate(world, demos);
            break;
        case 3: fireBall(world, demos, 4, vec3(0, 3, -6), vec3(0, 2, 28)); break;
        case 5: startDemo(world, 5); break;
        case 9: fireTrebuchet(world, demos); break;
        case 10: placeSlideCrates(world, demos); break;
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
// Right-click's hold, each fixed step: an impulse at the grabbed point, toward its spot in front of
// the camera -- a spring that closes the gap in about a tenth of a second, damped by the point's own
// velocity. It is a force, not a set velocity, and capped at GRAB_FORCE, so joints and contacts
// still win: a chained block strains at its chain rather than tearing it apart, and what weighs
// more than about 2.5 t will not lift. At the point rather than the centre of mass, so a body held
// by a corner hangs and swings from it. A third of the impulse a point mass would need, because
// a body turns as well as moves when pushed off-centre, and the whole of it would overshoot.
constexpr float GRAB_FORCE = 25000.0f;   // N
void dragTick(World& world, Demos& demos, float dt) {
    Demos::Grab& grab = demos.grab;
    if (grab.body == projv::NullEntity) return;
    if (!world.valid(grab.body) || !runtime::hasBody(world, grab.body)) { grab.body = projv::NullEntity; return; }
    const Camera& camera = world.ctx().get<Camera>();
    runtime::BodyState pose = runtime::bodyPose(world, grab.body);
    float mass = runtime::physicsWorld(world).bodyMass(world.get<projv::PhysicsBody>(grab.body).id);
    if (mass <= 0.0f) return;
    vec3 point = pose.position + pose.rotation * grab.local;
    vec3 r = point - pose.centerOfMass;
    vec3 pointVelocity = pose.linearVelocity + glm::cross(pose.angularVelocity, r);
    vec3 want = (camera.position + camera.forward() * grab.distance - point) * 10.0f;
    if (glm::length(want) > 25.0f) want = glm::normalize(want) * 25.0f;
    vec3 impulse = (want - pointVelocity) * mass * 0.3f;
    if (glm::length(impulse) > GRAB_FORCE * dt) impulse = glm::normalize(impulse) * GRAB_FORCE * dt;
    runtime::addImpulse(world, grab.body, impulse);
    runtime::addAngularImpulse(world, grab.body, glm::cross(r, impulse));
}

void demoTick(projv::Application& app) {
    World& world = app.world;
    auto& demos = world.ctx().get<Demos>();
    const float dt = app.time().fixedDelta;
    demos.clock += dt;
    dragTick(world, demos, dt);
    if (demos.blastKickIn > 0 && --demos.blastKickIn == 0) blastKick(world, demos);
    // Demo 1's kinematic balls level off a crate's height above the floor. Nothing stops a kinematic
    // body, so one driven down onto crates would press them through the floor; this way it ploughs
    // into the pyramid and rolls over whatever lies flat.
    for (Entity orb : demos.orbs) {
        if (!world.valid(orb) || !world.all_of<projv::ConstantVelocity>(orb)) continue;
        auto& motion = world.get<projv::ConstantVelocity>(orb);
        if (motion.velocity.y < 0.0f && runtime::bodyPose(world, orb).position.y < ORB_RADIUS + 1.05f) motion.velocity.y = 0.0f;
    }
    if (demos.current == 9) trebuchetTick(world, demos);
    if (demos.current == 10) slideTick(world, demos);
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

    const bool captured = input.cursorCaptured;   // the click that recaptures the cursor is not an action
    if (input.cursorCaptured && input.pressed(projv::Key::Escape)) {
        projv::graphics::setCursorCaptured(app, renderInstance, false);
    } else if (!input.cursorCaptured && input.pressed(projv::MouseButton::Left)) {
        projv::graphics::setCursorCaptured(app, renderInstance, true);
    }
    if (input.cursorCaptured) {
        camera.yaw += input.cursorDelta.x * 0.0025f;
        camera.pitch = std::clamp(camera.pitch - input.cursorDelta.y * 0.0025f, -1.5f, 1.5f);
    }

    for (int n = 1; n <= 9; n++)
        if (input.pressed(projv::Key(int(projv::Key::Num0) + n))) startDemo(world, n);
    if (input.pressed(projv::Key::Num0)) startDemo(world, 10);
    if (input.pressed(projv::Key::R)) startDemo(world, demos.current);
    if (captured && input.pressed(projv::MouseButton::Left) && demos.current != 7) act(world, demos, camera);
    // Right-click holds the dynamic body under the crosshair until it is let go; the wheel pulls it
    // nearer or pushes it away.
    if (captured && input.pressed(projv::MouseButton::Right)) {
        auto hit = runtime::castRay(world, camera.position, camera.forward(), 80.0f, 0xffffffffu, camera.walker);
        if (hit && world.valid(hit->entity) && world.all_of<projv::RigidBody>(hit->entity) &&
            world.get<projv::RigidBody>(hit->entity).motion == runtime::MotionType::Dynamic) {
            runtime::BodyState pose = runtime::bodyPose(world, hit->entity);
            demos.grab = {hit->entity, glm::inverse(pose.rotation) * (hit->position - pose.position), hit->distance};
        }
    }
    if (input.released(projv::MouseButton::Right)) demos.grab.body = projv::NullEntity;
    if (demos.grab.body != projv::NullEntity)
        demos.grab.distance = std::clamp(demos.grab.distance + input.scroll.y, 1.5f, 60.0f);
    if (input.pressed(projv::Key::F) && demos.current == 1) launchOrb(world, demos, camera);
    if (input.pressed(projv::Key::T)) time.scale = time.scale == 1.0f ? 0.2f : 1.0f;
    Look& look = world.ctx().get<Look>();
    if (input.pressed(projv::Key::H)) look.shadows = !look.shadows;
    if (input.pressed(projv::Key::O)) look.ambientOcclusion = !look.ambientOcclusion;
    if (input.pressed(projv::Key::U)) look.upscale = !look.upscale;
    if (input.pressed(projv::Key::G)) look.upscaleDebug = !look.upscaleDebug;
    if (input.pressed(projv::Key::P)) time.scale = time.scale == 0.0f ? 1.0f : 0.0f;

    const vec3 flat{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
    const vec3 right{-std::sin(camera.yaw), 0.0f, std::cos(camera.yaw)};   // cross(flat, up), as the renderer has it
    if (input.pressed(projv::Key::V)) {
        if (camera.walker == projv::NullEntity) {
            camera.walker = world.create();
            world.emplace<projv::Transform>(camera.walker, projv::Transform{camera.position - vec3(0, EYE_HEIGHT, 0)});
            world.emplace<projv::CharacterController>(camera.walker);
            camera.velocity.y = 0.0f;   // keeps its horizontal speed: steps out of flight walking
        } else {
            world.destroy(camera.walker);
            camera.walker = projv::NullEntity;
        }
    }
    if (demos.current == 9 && !demos.unattended) {
        float turn = (input.down(projv::Key::Left) ? 1.0f : 0.0f) - (input.down(projv::Key::Right) ? 1.0f : 0.0f);
        turnTrebuchet(world, demos, turn * 0.5f);
        float bend = (input.down(projv::Key::Up) ? 1.0f : 0.0f) - (input.down(projv::Key::Down) ? 1.0f : 0.0f);
        float& pin = demos.trebuchet.releaseAngle;
        if (bend != 0.0f) pin = std::clamp(pin + bend * 0.35f * time.unscaledDelta, -0.3f, 1.1f);
        if (input.released(projv::Key::Up) || input.released(projv::Key::Down))
            projv::core::info("Trebuchet: the sling lets go at {:.0f} degrees above the horizontal", glm::degrees(pin));
    }
    if (demos.current == 8 && !demos.unattended) {
        using Mode = Demos::Winch::Mode;
        setWinch(world, demos, input.down(projv::Key::Up) ? Mode::Raise : input.down(projv::Key::Down) ? Mode::Release : Mode::Hold);
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
        bool grounded = runtime::characterState(world, camera.walker).onGround;
        float ease = 1.0f - std::exp(-(grounded ? WALK_GRIP : AIR_GRIP) * time.unscaledDelta);
        camera.velocity += (walk - camera.velocity) * ease;
        runtime::setCharacterInput(world, camera.walker, camera.velocity, input.pressed(projv::Key::Space), 6.0f);
        // The camera follows in PostUpdate (followWalker), once physics has placed the character for
        // this frame.
    } else {
        vec3 fly(0.0f);
        if (input.down(projv::Key::W)) fly += flat;
        if (input.down(projv::Key::S)) fly -= flat;
        if (input.down(projv::Key::D)) fly += right;
        if (input.down(projv::Key::A)) fly -= right;
        if (input.down(projv::Key::Space)) fly.y += 1.0f;
        if (input.down(projv::Key::LeftShift)) fly.y -= 1.0f;
        if (glm::length(fly) > 0.0f) fly = glm::normalize(fly) * FLY_SPEED;
        camera.velocity += (fly - camera.velocity) * (1.0f - std::exp(-FLY_GRIP * time.unscaledDelta));
        camera.position += camera.velocity * time.unscaledDelta;
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

// =========================================================================================
// Bench: DEMOS_BENCH=<demo> times the GPU with the shadow ray on and off, interleaved
// =========================================================================================

// Alternates the shadow ray every PHASE frames after a warm-up, skipping the first few frames after
// each switch, so both configurations see the same moments of the demo and the same clocks. Reports
// the mean GPU frame and each pass (bgfx views, in render.json order), then quits.
struct Bench {
    static constexpr int WARMUP = 120, PHASE = 60, SETTLE = 6;
    int phases = 16;   // DEMOS_BENCH_PHASES: fewer for a quick look
    int frame = 0;
    double frameMs[2] = {}, passMs[2][8] = {};
    int samples[2] = {};
};

void bench(projv::Application& app) {
    World& world = app.world;
    Bench* b = world.ctx().find<Bench>();
    if (!b) return;
    Look& look = world.ctx().get<Look>();
    const int f = b->frame++;
    if (f < Bench::WARMUP) return;
    const int at = f - Bench::WARMUP, phase = at / Bench::PHASE;
    if (phase >= b->phases) {
        projv::core::vec2 size = world.ctx().get<projv::graphics::RenderInstance>().getWindowResolution();
        projv::core::info("BENCH demo {} resolution={}x{} (rendered at {}x{}), {} frames each", world.ctx().get<Demos>().current,
                          int(size.x), int(size.y), int(size.x / 2), int(size.y / 2), b->samples[0]);
        const char* passes[5] = {"gbuffer", "ssao", "shade", "reconstruct", "display"};
        for (int on = 1; on >= 0; on--) {
            double n = std::max(b->samples[on], 1);
            std::string line = fmt::format("BENCH   shadow ray {}: gpu frame {:.3f} ms |", on ? "on " : "off", b->frameMs[on] / n);
            for (int v = 0; v < 5; v++) line += fmt::format(" {} {:.3f}", passes[v], b->passMs[on][v] / n);
            projv::core::info("{}", line);
        }
        projv::core::info("BENCH   the shadow ray costs {:.3f} ms",
                          b->frameMs[1] / std::max(b->samples[1], 1) - b->frameMs[0] / std::max(b->samples[0], 1));
        app.closeRequested = true;
        return;
    }
    const int on = phase % 2 == 0 ? 1 : 0;
    look.shadows = on;
    // bgfx's stats are the frame before this one's: only count frames well inside a phase.
    if (at % Bench::PHASE < Bench::SETTLE) return;
    const bgfx::Stats* stats = bgfx::getStats();
    if (!stats || stats->gpuTimerFreq <= 0) return;
    const double toMs = 1000.0 / double(stats->gpuTimerFreq);
    b->frameMs[on] += double(stats->gpuTimeEnd - stats->gpuTimeBegin) * toMs;
    for (uint16_t v = 0; v < stats->numViews; v++)
        if (stats->viewStats[v].view < 8)
            b->passMs[on][stats->viewStats[v].view] += double(stats->viewStats[v].gpuTimeEnd - stats->viewStats[v].gpuTimeBegin) * toMs;
    b->samples[on]++;
}

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
    const float captureAt[11] = {0, 14.6f, 2.6f, 5.0f, 9.0f, 3.3f, 5.6f, 6.5f, 12.0f, 6.1f, 3.4f};
    if (const char* prefix = std::getenv("DEMOS_CAPTURE"); prefix && stage >= 1 && stage <= 10 && captured < stage &&
        t > captureAt[stage]) {
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, (std::string(prefix) + "-" + std::to_string(stage)).c_str());
        captured = stage;
    }
    // On to the first selected demo from `next`, or to the session-wide checks after the last.
    auto go = [&](int next) {
        while (next <= 10 && !test->only.empty() &&
               std::find(test->only.begin(), test->only.end(), next) == test->only.end())
            next++;
        acted = false;
        if (next > 10) { stage = 11; return; }
        startDemo(world, next);
        stage = next;
        test->start = watchedPositions();
    };
    if (f == 0) { go(1); return; }
    if (stage == 1 && t > 14.0f && !acted) {
        // The pyramid and towers have settled: the top crate is where it was built, sixteen up.
        vec3 top = runtime::bodyPose(world, demos.watched[0]).position;
        check(*test, std::abs(top.y - 15.5f) < 0.15f && runtime::physicsWorld(world).stats().awakeBodies == 0,
              fmt::format("pyramid stands and sleeps: top crate at {:.2f} m, {} bodies awake", top.y,
                          runtime::physicsWorld(world).stats().awakeBodies));
        test->start = watchedPositions();
        camera.position = vec3(0, 15.0f, -20); camera.yaw = 1.5708f; camera.pitch = 0.0f;   // aimed at its top
        launchOrb(world, demos, camera);
        acted = true;
    }
    if (stage == 1 && t > 17.0f) {
        vec3 top = runtime::bodyPose(world, demos.watched[0]).position;
        check(*test, top.y < 14.5f, fmt::format("the kinematic ball knocked the top off: top crate at {:.2f} m", top.y));
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
        // Up held for 12 s, then let go for 2 s, then Down for 2 s.
        using Mode = Demos::Winch::Mode;
        static float startHeight = 0.0f, heldAt = 0.0f, releasedAt = 0.0f;
        static bool raised = false, held = false;
        auto height = [&] { return runtime::bodyPose(world, demos.winch.weight).position.y; };
        setWinch(world, demos, t < 1.0f ? Mode::Hold : t < 13.0f ? Mode::Raise : t < 15.0f ? Mode::Hold : Mode::Release);
        if (t > 1.0f && startHeight == 0.0f) {
            startHeight = height();
            for (float& a : demos.winch.turned) a = 0.0f;
        }
        if (t > 13.5f && heldAt == 0.0f) heldAt = height();   // let it settle half a second first
        if (t > 15.0f && !held) {
            held = true;
            releasedAt = height();
            check(*test, std::abs(releasedAt - heldAt) < 0.08f,
                  fmt::format("let go of Up, it holds: the weight moved {:.3f} m in 1.5 s", releasedAt - heldAt));
        }
        if (t > 17.0f) {
            float dropped = releasedAt - height();
            check(*test, dropped > 0.5f, fmt::format("Down releases the motor: the weight unwound {:.2f} m in 2 s", dropped));
            go(9);
            return;
        }
        if (t > 13.0f && !raised) {
            raised = true;
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
        }
    }
    if (stage == 9) {
        static int piecesBefore = 0;
        Demos::Trebuchet& tb = demos.trebuchet;
        // Turned on its turntable by 0.3 rad over a second, then fired.
        if (t > 1.0f && t < 2.0f) turnTrebuchet(world, demos, 0.3f);
        if (t > 2.0f) turnTrebuchet(world, demos, 0.0f);
        if (t > 2.5f && !acted) {
            vec3 ahead = runtime::bodyPose(world, tb.frame).rotation * vec3(1, 0, 0);
            float turned = std::atan2(-ahead.z, ahead.x);
            check(*test, std::abs(turned - 0.3f) < 0.05f, fmt::format("the turntable turned it {:.3f} rad (0.300)", turned));
            check(*test, world.valid(tb.boulder) && std::abs(runtime::bodyPose(world, tb.boulder).position.y - 0.7f) < 0.1f &&
                             std::abs(runtime::jointState(world, tb.axle).position) < 0.06f,
                  fmt::format("the latch holds the trebuchet cocked (arm off by {:.3f} rad), the boulder on the ground",
                              runtime::jointState(world, tb.axle).position));
            piecesBefore = demos.pieces;
            act(world, demos, camera);
            acted = true;
        }
        if (t > 10.5f) {
            float thrown = glm::length(vec2(tb.impactAt.x, tb.impactAt.z) - vec2(tb.releasedAt.x, tb.releasedAt.z));
            check(*test, tb.released, fmt::format("the sling let the boulder go at {:.1f} m/s, {:.1f} m up", tb.releaseSpeed, tb.releasedAt.y));
            check(*test, tb.struck, fmt::format("the boulder reached the wall at {:.1f} m/s", tb.impactSpeed));
            check(*test, demos.pieces - piecesBefore >= 10,
                  fmt::format("the wall broke up: {} pieces split off", demos.pieces - piecesBefore));
            check(*test, tb.struck && thrown > 20.0f, fmt::format("the boulder flew {:.1f} m from the release to the wall", thrown));
            go(10);   // the slide
            return;
        }
    }
    if (stage == 10 && t > 1.0f && !acted) {
        // Three colours, one material: each ramp collides as one box per grid cell, as a plain one would.
        for (const Demos::Slide::Track& tr : demos.slide.tracks) {
            const auto& parts = world.get<projv::PhysicsBody>(tr.rampEntity).voxelParts;
            uint32_t pieces = 0;
            bool materials = true;
            for (const auto& part : parts) { pieces += part.shape->pieces; materials = materials && part.shape->hasMaterials; }
            check(*test, !parts.empty() && pieces == parts.size() && materials,
                  fmt::format("the {} ramp, three colours of one material, is {} boxes in {} cells", tr.ramp, pieces, parts.size()));
        }
        acted = true;
    }
    if (stage == 10 && t > 12.0f) {
        // Each crate reached its wall as fast as its ramp's friction allows -- v^2 = 2 g (sin a - mu
        // cos a) d -- and came back up as far as its wall's restitution sends it: (e v)^2 / (2 g (sin
        // a + mu cos a)).
        const float g = 9.81f, d = SLIDE_CRATE_AT_WALL - SLIDE_CRATE_START, sa = std::sin(SLIDE_ANGLE), ca = std::cos(SLIDE_ANGLE);
        float speeds[3], backs[3];
        for (int i = 0; i < 3; i++) {
            const Demos::Slide::Track& tr = demos.slide.tracks[i];
            float mu = std::sqrt(runtime::physicsMaterial(world, tr.ramp)->friction * 1.0f);
            float e = runtime::physicsMaterial(world, tr.wall)->restitution;
            float expected = std::sqrt(2.0f * g * (sa - mu * ca) * d);
            float expectedBack = (e * expected) * (e * expected) / (2.0f * g * (sa + mu * ca));
            speeds[i] = tr.impactSpeed;
            backs[i] = tr.struckAt - tr.backTo;
            check(*test, tr.struck && std::abs(tr.impactSpeed - expected) < 0.1f * expected,
                  fmt::format("the crate on {} struck its wall at {:.2f} m/s ({:.2f} predicted)", tr.ramp, tr.impactSpeed, expected));
            bool bounced = e == 0.0f ? backs[i] < 0.1f : backs[i] > 0.6f * expectedBack && backs[i] < 1.3f * expectedBack;
            check(*test, tr.struck && bounced,
                  fmt::format("{} sent it {:.2f} m back up ({:.2f} predicted)", tr.wall, backs[i], expectedBack));
        }
        check(*test, speeds[0] > speeds[1] && speeds[1] > speeds[2] && backs[0] > backs[1] && backs[1] > backs[2],
              "slicker ramps are faster, bouncier walls send crates further back");
        go(11);
        return;
    }
    if (stage == 11) {
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
    const char* benchDemo = std::getenv("DEMOS_BENCH");
    const bool unattended = std::getenv("DEMOS_SELFTEST") != nullptr || benchDemo;

    auto& renderInstance = world.ctx().emplace<projv::graphics::RenderInstance>();
    renderInstance.initialize(1600, 900, "ProjectV Physics Demos");
    projv::graphics::installPlatform(app, renderInstance);
    projv::graphics::setCursorCaptured(app, renderInstance, !unattended);

    world.ctx().emplace<Camera>();
    Look& look = world.ctx().emplace<Look>();
    // DEMOS_LOOK: the same switches as the keys, for unattended runs -- h, o and u turn shadows, AO
    // and the upscaler off; g turns the upscaler's debug view on.
    if (const char* switches = std::getenv("DEMOS_LOOK")) {
        std::string s = switches;
        look.shadows = s.find('h') == std::string::npos;
        look.ambientOcclusion = s.find('o') == std::string::npos;
        look.upscale = s.find('u') == std::string::npos;
        look.upscaleDebug = s.find('g') != std::string::npos;
        look.marchHeat = s.find('s') != std::string::npos;
    }
    auto& scene = world.ctx().emplace<projv::Scene>();
    auto& demos = world.ctx().emplace<Demos>();

    runtime::installSceneBridge(app);
    runtime::PhysicsConfig physics;
    physics.settings.worldMin = vec3(-1e4f, -30.0f, -1e4f);
    physics.settings.record = unattended && !benchDemo;   // the self-test replays the whole session at the end
    runtime::installPhysics(app, physics);
    // The physics materials the demos' voxels name (demo 10's ramps and walls).
    if (!runtime::loadPhysicsMaterials(world, (here / "physics_materials.json").string()))
        projv::core::error("demos: physics_materials.json did not load; the slide's ramps will all behave alike");

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
    // A plane, nose to -z (the way quatLookAt faces it): fuselage, wings, a tailplane and a fin.
    // The kinematic ball: 4 m across, in 25 cm voxels, a red band round it so it is seen to turn.
    demos.orb = prototype(scene, "orb", ivec3(16), 0.25f, [](int x, int y, int z) -> uint32_t {
        vec3 c = vec3(float(x), float(y), float(z)) + vec3(0.5f) - vec3(8.0f);
        if (glm::length(c) > 8.0f) return 0u;
        return std::abs(c.y) < 1.0f ? rgb(0.85f, 0.15f, 0.1f) : rgb(0.88f, 0.88f, 0.86f);
    });
    // The boulder: a ball of stone 1.4 m across, in 10 cm voxels.
    demos.rockProto = prototype(scene, "boulder", ivec3(14), 0.1f, [](int x, int y, int z) -> uint32_t {
        vec3 c = vec3(float(x), float(y), float(z)) + vec3(0.5f) - vec3(7.0f);
        return glm::length(c) <= 7.0f ? stone(x, y, z) : 0u;
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
                              demos.balls[3], demos.balls[4], demos.plank, demos.link, demos.wheel, demos.knuckle, demos.chainLink,
                              demos.orb, demos.rockProto})
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
        projv::graphics::loadRendererSpecification((here / "demoRenderer").string() + "/");
    renderInstance.addRendererSpecification(1, specification);
    bgfx::ShaderHandle vertexShader = projv::graphics::loadShader((here / "demoRenderer/shaders/vs_quad.bin").string());
    renderInstance.setActiveRenderer(
        projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));
    world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));

    demos.unattended = unattended;
    if (benchDemo) {
        Bench& b = world.ctx().emplace<Bench>();
        if (const char* phases = std::getenv("DEMOS_BENCH_PHASES")) b.phases = std::max(2, std::atoi(phases));
        bgfx::setDebug(BGFX_DEBUG_PROFILER);
        startDemo(world, std::clamp(std::atoi(benchDemo), 1, 10));
    } else if (unattended) {
        SelfTest& test = world.ctx().emplace<SelfTest>();
        if (const char* only = std::getenv("DEMOS_ONLY"))
            for (const char* c = only; *c;) {
                char* end = nullptr;
                long n = std::strtol(c, &end, 10);
                if (end == c) { c++; continue; }   // a separator
                if (n >= 1 && n <= 10) test.only.push_back(int(n));
                c = end;
            }
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
    const Look& look = world.ctx().get<Look>();
    projv::core::vec4 sun(glm::normalize(vec3(0.35f, 0.85f, 0.4f)), 0.0f);
    projv::core::vec4 params(look.shadows, look.ambientOcclusion, !look.upscale,
                             look.upscaleDebug ? 1.0f : look.marchHeat ? 2.0f : 0.0f);
    projv::graphics::setUniformToValue(renderer, "sunDir", sun);
    projv::graphics::setUniformToValue(renderer, "renderParams", params);
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
    app.addSystem(projv::Stage::Update,      "bench",     bench);
    app.addSystem(projv::Stage::Update,      "title bar", titleBar);
    app.addSystem(projv::Stage::PostUpdate,  "follow camera", followCamera, {.after = "physics: present"});
    app.addSystem(projv::Stage::Render,      "render",    render);
    app.addSystem(projv::Stage::Shutdown,    "shutdown",  shutdown);
    app.run();
    return 0;
}
