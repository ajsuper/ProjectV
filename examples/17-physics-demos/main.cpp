// ProjectV Physics Demos
//
// Eleven set pieces for the engine's physics (runtime/physics.h), each built from voxels in code:
//
//   1  Pyramid        a stepped pyramid of 1496 crates, sixteen high on a 16x16 base, and beside
//                     it two stepped towers side by side, thirty crates high on 5x5 bases.
//                     Click throws a heavy ball. F launches a 4 m ball where you look: kinematic,
//                     at a constant 30 m/s (ConstantVelocity), ploughing through whatever is there.
//   2  Wall blast     a staggered wall of 120 voxel bricks, a tower, a house, columns and a long low wall,
//                     all fired brick. Click sets off a big blast where the crosshair is: the
//                     bricks near it shatter (runtime::fracture), everything is thrown outward, and
//                     bricks thrown hard enough break again where they land.
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
//                     34 m off -- one on each side, each a seamless static grid, each built of
//                     something else: ashlar that shatters, brick that parts at its mortar, planks
//                     that splinter along the grain, plaster that crumbles. The boulder breaks them
//                     itself: the engine breaks what is hit hard enough for its material's strength,
//                     and the boulder carries on with what the break did not use. Click: fire; again,
//                     a new boulder and fire. Left/Right: turn it on its turntable. Up/Down: a higher or flatter
//                     throw (the angle the sling lets go at).
//  10  Slide          physics materials (physics_materials.json, beside the binary): three 14 m
//                     ramps of ice, polished wood and slate, the same crate on each, sliding down
//                     into walls of rubber, oak and clay. Each ramp is striped in three colours that
//                     all name one material, so it still collides as one box per grid cell; the
//                     crates hit their walls at the speeds their ramps allow and bounce back as far
//                     as their walls allow. Click: again.
//  11  Overhangs      support: a sandstone arch, a timber tree, a chalk cliff with a shelf and a
//                     stalactite, a brick tower with a balcony, and a composite wall -- a concrete
//                     frame with rebar, brick, pine and glass -- all static colliders that split.
//                     Click chips where you aim (F: a demolition blow); cut away what holds an
//                     overhang up and it falls, as a body of what it was made of.
//
// Controls
//   1-9, 0, -  pick a demo (0 is 10, - is 11; it is rebuilt)  R  restart the demo
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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <set>
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
#include "utils/collision_geometry.h"
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
        vec3 impactAt{0.0f};
        int struckWall = -1;               // which of `walls` the boulder broke
        // The release pin: the sling lets go when the boulder flies this far above the horizontal
        // (rad). Up and Down bend it: higher arcs, flatter shots.
        float releaseAngle = 0.2f;
        Entity axle = projv::NullEntity;   // the arm's hinge; its servo is the latch
        Entity sling = projv::NullEntity;  // the boulder's fixed joint to the arm's tip
        // A wall: one static grid of 128 x 96 x 4 voxels (32 m x 24 m x 1 m). Local x along the
        // wall, y up, z through it toward the middle.
        struct Wall { Entity entity = projv::NullEntity; vec3 corner{0.0f}; quat rotation{1, 0, 0, 0}; };
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
    // Demo 11: what the self-test chips at, and what support dropped.
    struct Overhangs {
        Entity tree = projv::NullEntity;
        Entity wall = projv::NullEntity;   // the composite wall: concrete frame, rebar, brick, pine, glass
        std::vector<projv::BodyBroken> fell;
        // A click's shards, made frictionless in the step that made them, waiting for their bodies to
        // be thrown (see throwChip).
        std::vector<Entity> toThrow;
        // A click's chip, until its pieces come back (see chip): where it was struck, which way is
        // out of the face, and how fast the pieces leave.
        bool chipped = false;
        vec3 chipAt{0.0f}, chipOut{0.0f};
        float chipSpeed = 0.0f;
        // Thrown pieces, frictionless for a moment (see throwChip): what to give back, and when.
        struct Slippery { Entity piece; float friction; std::string material; int steps; };
        std::vector<Slippery> slippery;
    } overhangs;
    // What right-click holds: a body, the point on it that was grabbed (in the body's frame), and how
    // far in front of the camera that point is pulled.
    struct Grab {
        Entity body = projv::NullEntity;
        vec3 local{0.0f};
        float distance = 0.0f;
    } grab;
    vec3 blastAt{0.0f};
    int blastKickIn = 0;                   // fixed steps until a detonation's kick (see detonate)
    // The physics step's cost since the last blast: what the self-test reports.
    double stepSum = 0.0, stepWorst = 0.0;
    int stepCount = 0;
    int pieces = 0;                        // bodies that came away from what broke (BodyBroken)
    int fellThrough = 0;                   // left the world from above the floor: must stay 0
    // Bodies seen below the floor's top beyond its edge: gone over it, so their leaving the world
    // from under the floor (they can drift back in as they fall) is not a fall through it.
    std::set<Entity> overEdge;
    std::mt19937 random{7};
    float titleTimer = 0.0f;
    bool unattended = false;               // the self-test is driving
};

float uniform(Demos& d, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(d.random); }

constexpr int LAST_DEMO = 11;

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
        case 11: return "Overhangs  (Click: chip where you aim, F: a harder blow)";
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
    demos.overhangs = Demos::Overhangs{};
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
// A brick's body: 30 kg of fired brick (physics_materials.json), which a blast shatters and which
// breaks on impact when thrown hard enough -- at about 15 m/s into something that does not move,
// not when a tower's top course falls the 8 m to the ground.
projv::RigidBody brickBody() {
    projv::RigidBody r = body(30.0f);
    r.material = "fired brick";
    r.breaksOnImpact = true;   // thrown bricks break where they land hard
    return r;
}

void brick(World& world, Demos& demos, vec3 corner, vec3 centre, bool alongZ) {
    const quat turned(std::cos(0.7854f), 0.0f, std::sin(0.7854f), 0.0f);
    place(world, demos.brick, corner + vec3(centre.x * 1.006f, centre.y, centre.z * 1.006f),
          alongZ ? turned : quat(1, 0, 0, 0), brickBody());
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
    // 1 m x 0.5 m x 0.5 m, in 12.5 cm voxels, so a blast can shatter it.
    const vec3 brick(1.0f, 0.5f, 0.5f);
    for (int course = 0; course < 10; course++) {
        float shift = (course % 2) ? 0.5f : 0.0f;
        int count = (course % 2) ? 11 : 12;
        for (int i = 0; i < count; i++) {
            float x = (float(i) - 5.5f + shift) * (brick.x + 0.005f);
            float y = 0.5f * brick.y + float(course) * (brick.y + 0.003f);
            place(world, demos.brick, vec3(x, y, 0), quat(1, 0, 0, 0), brickBody());
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

// The four walls, each built of its own material (physics_materials.json), so each breaks its own
// way. The patterns read only what the materials say -- except "seams", which reads the palette:
// a brick is a run of one entry, so neighbouring bricks are different shades.
//
// Ashlar: blocks 1.5 m long and 0.75 m high, each course offset by half a block. It shatters, and
// the joints do not matter: Voronoi cells run straight through blocks and mortar alike.
uint32_t wallStone(int x, int y, int z) {
    bool mortar = y % 3 == 0 || (x + (y / 3) % 2 * 3) % 6 == 0;
    return mortar ? rgb(0.62f, 0.6f, 0.55f) : stone(x, y, z);
}

// Brick: 1.25 m x 0.5 m, a quarter-metre of mortar between, in running bond; three shades of brick,
// chosen per brick (the mortar between keeps bricks apart, so any two may match).
// It parts at its seams: each brick the blast reaches comes away whole, the mortar crumbles.
uint32_t wallBrick(int x, int y, int) {
    int course = y / 3, shifted = x + course % 2 * 3;
    if (y % 3 == 2 || shifted % 6 == 5) return rgb(0.74f, 0.71f, 0.65f);   // mortar
    int which = (course * 7 + shifted / 6 * 3) % 3;
    static const float reds[3][3] = {{0.55f, 0.22f, 0.15f}, {0.63f, 0.28f, 0.18f}, {0.47f, 0.19f, 0.14f}};
    return rgb(reds[which][0], reds[which][1], reds[which][2]);
}

// Planks: boards 0.75 m high running the wall's length, a dark seam under each. It splinters along
// the grain -- the material's grain is the wall's own x -- into long shards.
uint32_t wallPlanks(int x, int y, int) {
    if (y % 3 == 0) return rgb(0.3f, 0.2f, 0.12f);   // the seam between boards
    int board = y / 3;
    float shade = 0.85f + 0.15f * float((board * 5 + (x / 9 + board) % 3) % 4) / 3.0f;
    return board % 2 ? rgb(0.58f * shade, 0.4f * shade, 0.23f * shade) : rgb(0.5f * shade, 0.34f * shade, 0.19f * shade);
}

// Plaster over rubble: off-white, blotched. It crumbles: small even pieces, the hole round.
uint32_t wallPlaster(int x, int y, int) {
    int blotch = ((x / 5) * 73 + (y / 4) * 151 + (x / 11) * 17) % 7;
    return blotch < 2 ? rgb(0.8f, 0.77f, 0.7f) : rgb(0.87f, 0.85f, 0.79f);
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
    tb.struckWall = -1;
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
    // Four walls round it, each one seamless static grid, 32 m by 24 m and 1 m thick, each of its
    // own material: ahead (+x) ashlar, then turning left (towards -z) brick, planks behind, plaster.
    // A strike breaks out its middle and leaves its edges and top standing.
    auto& scene = world.ctx().get<projv::Scene>();
    using Colour = uint32_t (*)(int, int, int);
    static const Colour colours[4] = {wallStone, wallBrick, wallPlanks, wallPlaster};
    static const char* const materials[4] = {"ashlar", "brick", "planks", "plaster"};
    for (int side = 0; side < 4; side++) {
        vec3 out = glm::angleAxis(float(side) * 1.5708f, vec3(0, 1, 0)) * vec3(1, 0, 0), along(-out.z, 0, out.x);
        Demos::Trebuchet::Wall wall;
        wall.rotation = glm::quat_cast(glm::mat3(along, vec3(0, 1, 0), -out));
        wall.corner = out * (WALL_DISTANCE + 0.5f) - along * (0.5f * float(WALL_W) * WALL_VOXEL);
        ComponentHandle h = voxels(scene, materials[side], ivec3(WALL_W, WALL_H, WALL_T), WALL_VOXEL, wall.corner, colours[side]);
        utils::setComponentTransform(scene, h, wall.corner, wall.rotation, 1.0f);
        wall.entity = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        projv::StaticCollider made;
        made.material = materials[side];   // how it breaks: physics_materials.json
        made.splits = true;                // and what a break leaves hanging falls
        made.anchorBase = true;            // it stands on its own y = 0
        made.breaksOnImpact = true;        // the boulder breaks it
        world.emplace<projv::StaticCollider>(wall.entity, made);
        demos.owned.push_back(wall.entity);
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

// Each step of demo 9: the delayed fire after a rebuild, the release, and the strike.
void trebuchetTick(World& world, Demos& demos) {
    Demos::Trebuchet& tb = demos.trebuchet;
    drawSling(world, demos);
    if (tb.fireIn >= 0.0f && (tb.fireIn -= 1.0f / 60.0f) < 0.0f) fireTrebuchet(world, demos);
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
    // The strike itself is the engine's: it breaks the wall the step after the boulder hits it
    // (BodyBroken, handled in main).
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

// ---- Demo 11: overhangs ----
//
// Four static things standing on the floor, each of a material that breaks its own way, each built
// with something hanging off it, and each a collider that splits (StaticCollider::splits): chip at
// what holds the overhang up until nothing does, and it falls -- as a body of its own, of what it
// was made of, breaking again where it lands hard.
//
//   an arch of sandstone (shatter): cut a pillar and the span still hangs from the other; cut both
//   a tree of timber (splinter, the grain upright): cut the trunk and the crown comes down
//   a chalk cliff (crumble) with a shelf, a stalactite under it and a cornice: undercut them
//   a brick tower (seams, the bricks laid with mortar all round) with a balcony: the mortar
//   crumbles where you chip, and bricks with nothing left holding them drop out whole

constexpr float OVERHANG_VOXEL = 0.25f;
constexpr float CHIP_ENERGY = 90000.0f;    // J: about 0.7 m of these materials
constexpr float CHIP_SHARD = 0.35f;        // m: a pickaxe breaks finer than the materials do on their own
constexpr float BLOW_ENERGY = 3.0e6f;      // J: F, a demolition blow -- 2 m of concrete, 3 m of chalk
constexpr float TRUNK_BLOW = 120000.0f;    // J: each of the self-test's four blows round the tree's trunk
constexpr float CHIP_SPEED = 6.0f, BLOW_SPEED = 14.0f;   // m/s: how fast each throws its pieces out

// A static thing of `material` that splits, its corner at `corner`, standing on its own y = 0 (what
// support takes for the ground).
template <typename ColourAt>
Entity overhang(World& world, Demos& demos, const char* material, ivec3 size, vec3 corner, ColourAt colourAt) {
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle h = voxels(scene, material, size, OVERHANG_VOXEL, corner, colourAt);
    Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    projv::StaticCollider c;
    c.material = material;
    c.splits = true;
    c.anchorBase = true;   // standing on its own y = 0
    c.breaksOnImpact = true;   // and what falls breaks again where it lands hard
    world.emplace<projv::StaticCollider>(e, c);
    runtime::assignNetId(world, e);
    demos.owned.push_back(e);
    return e;
}

uint32_t sandstone(int x, int y, int z) {
    float shade = 0.85f + 0.15f * float((x * 7 + y * 13 + z * 5) % 4) / 3.0f;
    return (y / 2) % 3 == 0 ? rgb(0.8f * shade, 0.66f * shade, 0.45f * shade) : rgb(0.86f * shade, 0.72f * shade, 0.5f * shade);
}

uint32_t chalk(int x, int y, int z) {
    return (x * 3 + y * 5 + z * 7) % 6 == 0 ? rgb(0.82f, 0.82f, 0.78f) : rgb(0.92f, 0.92f, 0.88f);
}

// Bricks with mortar on every side, through the wall's depth too: so each brick is a run of one
// palette entry, and "seams" can part it from its neighbours.
uint32_t towerBrick(int x, int y, int z) {
    int course = y / 3, shifted = x + course % 2 * 3, deep = z + course % 2 * 2;
    if (y % 3 == 2 || shifted % 6 == 5 || deep % 4 == 3) return rgb(0.74f, 0.71f, 0.65f);   // mortar
    int which = (course * 7 + shifted / 6 * 3 + deep / 4 * 5) % 3;
    static const float reds[3][3] = {{0.55f, 0.22f, 0.15f}, {0.63f, 0.28f, 0.18f}, {0.47f, 0.19f, 0.14f}};
    return rgb(reds[which][0], reds[which][1], reds[which][2]);
}

// The composite wall: 12 m wide, 8 m high, half a metre thick, in eighth-metre voxels. A concrete
// frame -- three columns and two beams -- with steel rebar running through it, and four bays: brick
// laid in mortar, pine boards with a glass window, a glass curtain on pine mullions, and brick with a
// small window. Each colour names its physics material, so a blow breaks each part its own way: the
// concrete shatters off the rebar, which never breaks (it has no strength) and goes on holding up
// what it runs through; the bricks come out of their mortar, the boards splinter along their grain,
// and the glass, the weakest, goes furthest.
constexpr float COMPOSITE_VOXEL = 0.125f;
constexpr ivec3 COMPOSITE_SIZE(96, 64, 4);
const vec3 COMPOSITE_CORNER(20.0f, 0.0f, 5.0f);

Entity buildCompositeWall(World& world, Demos& demos) {
    std::map<uint32_t, const char*> materialOf;
    auto paint = [&materialOf](uint32_t colour, const char* material) {
        materialOf[colour] = material;
        return colour;
    };
    const uint32_t concrete[2] = {paint(rgb(0.62f, 0.62f, 0.6f), "concrete"), paint(rgb(0.57f, 0.57f, 0.55f), "concrete")};
    const uint32_t rebar = paint(rgb(0.4f, 0.24f, 0.16f), "rebar");
    const uint32_t mortar = paint(rgb(0.78f, 0.76f, 0.7f), "mortar");
    const uint32_t bricks[3] = {paint(rgb(0.56f, 0.23f, 0.15f), "wall brick"), paint(rgb(0.64f, 0.29f, 0.18f), "wall brick"),
                                paint(rgb(0.48f, 0.2f, 0.14f), "wall brick")};
    const uint32_t pine[2] = {paint(rgb(0.72f, 0.55f, 0.33f), "pine"), paint(rgb(0.66f, 0.49f, 0.29f), "pine")};
    const uint32_t glass[2] = {paint(rgb(0.62f, 0.8f, 0.86f), "glass"), paint(rgb(0.56f, 0.75f, 0.82f), "glass")};

    auto colourAt = [&](int x, int y, int z) -> uint32_t {
        const bool column = x < 8 || (x >= 44 && x < 52) || x >= 88;
        const bool beam = (y >= 28 && y < 34) || y >= 58;
        if (column || beam) {
            // Bars: up each column at x 2 and 5 of it, and along each beam 1 and 4 voxels in, two
            // deep -- inside the concrete, out of sight until it breaks away.
            const int cx = x < 8 ? x : x < 52 ? x - 44 : x - 88;
            const bool upright = column && (cx == 2 || cx == 5) && (z == 1 || z == 2);
            const int by = y >= 58 ? y - 58 : y - 28;
            const bool along = beam && (by == 1 || by == 4) && (z == 1 || z == 2);
            if (upright || along) return rebar;
            return concrete[(x / 3 + y / 3 + z) % 2];
        }
        const bool left = x < 44, lower = y < 28;
        const int bx = left ? x - 8 : x - 52, by = lower ? y : y - 34;   // within the bay
        auto brick = [&] {
            // Bricks four voxels long and two high, a mortar joint round each, every other course
            // shifted by half a brick.
            const int course = by / 3, shifted = bx + course % 2 * 2;
            if (by % 3 == 2 || shifted % 5 == 4) return mortar;
            return bricks[(course * 7 + shifted / 5 * 3) % 3];
        };
        auto pane = [&] { return z == 1 || z == 2 ? glass[(x + y) % 7 == 0] : 0u; };
        if (left && lower) return brick();
        if (!left && lower) {
            // Pine boards three voxels high, a window in the middle.
            if (bx >= 8 && bx < 28 && by >= 8 && by < 24) return pane();
            return pine[(by / 3) % 2];
        }
        if (left) {
            // A glass curtain on pine mullions every 9 voxels and a transom across the middle.
            if (bx % 9 == 0 || by == 11 || by == 12) return pine[(bx + by) % 2];
            return pane();
        }
        if (bx >= 12 && bx < 24 && by >= 6 && by < 18) return pane();
        return brick();
    };

    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle h = voxels(scene, "composite wall", COMPOSITE_SIZE, COMPOSITE_VOXEL, COMPOSITE_CORNER, colourAt);
    const auto& palette = scene.components[h].materialPalette;
    for (size_t slot = 0; slot < palette.size(); slot++)
        if (auto found = materialOf.find(palette[slot].packedColor); found != materialOf.end())
            utils::setMaterialPhysics(scene, h, uint8_t(slot), found->second);
    Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    projv::StaticCollider c;
    c.material = "concrete";
    c.splits = true;
    c.anchorBase = true;
    c.breaksOnImpact = true;
    world.emplace<projv::StaticCollider>(e, c);
    runtime::assignNetId(world, e);
    demos.owned.push_back(e);
    return e;
}

// How many of the composite wall's voxels are rebar, and how many it has in all.
std::pair<uint64_t, uint64_t> wallVoxels(World& world, Entity wall) {
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle h = runtime::voxelComponentOf(world, wall);
    if (h == projv::INVALID_COMPONENT_HANDLE) return {0, 0};
    const std::vector<uint32_t> counts = utils::countMaterialUsage(scene, h);
    const auto& palette = scene.components[h].materialPalette;
    uint64_t rebar = 0, all = 0;
    for (size_t slot = 0; slot < counts.size() && slot < palette.size(); slot++) {
        all += counts[slot];
        if (palette[slot].physics == "rebar") rebar += counts[slot];
    }
    return {rebar, all};
}

void buildOverhangs(World& world, Demos& demos, Camera& camera) {
    // The arch: two 1 m pillars 6 m high, 5 m apart, and a span across them, overhanging each by 1 m.
    overhang(world, demos, "sandstone", ivec3(32, 30, 4), vec3(-17, 0, 6), [](int x, int y, int z) -> uint32_t {
        bool pillar = (x >= 4 && x <= 7) || (x >= 24 && x <= 27);
        return (pillar && y < 24) || y >= 24 ? sandstone(x, y, z) : 0u;
    });
    // The tree: a 1 m trunk 5.5 m up, and a crown 5 m across and 1.5 m thick.
    demos.overhangs.tree = overhang(world, demos, "timber", ivec3(20, 28, 20), vec3(-6.5f, 0, 5), [](int x, int y, int z) -> uint32_t {
        bool trunk = x >= 8 && x <= 11 && z >= 8 && z <= 11 && y < 22;
        float dx = float(x) - 9.5f, dz = float(z) - 9.5f;
        bool crown = y >= 22 && dx * dx + dz * dz <= 9.5f * 9.5f;
        if (trunk) return (y + x) % 3 ? rgb(0.45f, 0.3f, 0.17f) : rgb(0.4f, 0.26f, 0.15f);
        if (crown) return (x + z + y) % 4 ? rgb(0.25f, 0.45f, 0.18f) : rgb(0.3f, 0.52f, 0.2f);
        return 0u;
    });
    // The cliff: a 1.5 m-deep face 8 m high; a shelf jutting 2.5 m out at 4.5 m, a stalactite under
    // its end, and a cornice along the top.
    overhang(world, demos, "chalk", ivec3(16, 32, 12), vec3(2, 0, 5), [](int x, int y, int z) -> uint32_t {
        bool face = x < 6;
        bool shelf = y >= 18 && y <= 21 && x < 16;
        bool stalactite = x >= 12 && x <= 13 && z >= 5 && z <= 6 && y >= 12 && y < 18;
        bool cornice = y >= 29 && x < 10;
        return face || shelf || stalactite || cornice ? chalk(x, y, z) : 0u;
    });
    // The tower: 2 m square, 10 m high, a balcony 3 m out and 2 m wide at 6 m.
    overhang(world, demos, "tower brick", ivec3(20, 40, 8), vec3(12, 0, 5), [](int x, int y, int z) -> uint32_t {
        bool tower = x < 8;
        bool balcony = y >= 24 && y <= 26 && x < 20;
        return tower || balcony ? towerBrick(x, y, z) : 0u;
    });
    // And beside the tower, the composite wall.
    demos.overhangs.wall = buildCompositeWall(world, demos);
    camera.position = vec3(7.5f, 6, -20);
    camera.yaw = 1.5708f;
    camera.pitch = -0.1f;
}

// A blow where the crosshair is, like a pickaxe's: the engine's fracture, with `energy`, just inside
// what the ray hits, breaking to `shardSize` (0: what the material breaks to); then its shards, when
// they come back (BodyBroken, in main), are sent flying out of the face (throwChip). Left alone they
// would not move: they are cut from one solid, so they mesh perfectly and friction holds each in its
// socket.
void chip(World& world, const Camera& camera, float energy, float speed, float shardSize) {
    auto hit = runtime::castRay(world, camera.position, camera.forward(), 80.0f, 0xffffffffu, camera.walker);
    if (!hit || hit->entity == projv::NullEntity) return;
    Demos& demos = world.ctx().get<Demos>();
    runtime::Strike strike;
    strike.at = hit->position + camera.forward() * 0.1f;
    strike.energy = energy;
    strike.shardSize = shardSize;
    runtime::fracture(world, hit->entity, strike);
    demos.overhangs.chipped = true;
    demos.overhangs.chipAt = strike.at;
    demos.overhangs.chipOut = glm::dot(hit->normal, camera.forward()) < 0.0f ? hit->normal : -hit->normal;
    demos.overhangs.chipSpeed = speed;
}

// A chip's shards are thrown out of the face, spreading from where it was struck and tumbling. Cut
// from one solid, each sits flush in its socket, and friction against the faces round it holds it
// there whatever speed it is given (measured: set to 6 m/s, none moved more than 2 cm; frictionless,
// nearly all flew). So each is made frictionless for a moment, in the step that made it, before its
// body exists (slipChip); thrown once it has one (throwChip); and given its own friction back
// (restoreChips) before it lands. The crumbs are gone already: the engine's dust (dustVoxels).
constexpr int SLIPPERY_STEPS = 30;   // fixed steps: half a second
void slipChip(World& world, Demos::Overhangs& o, const projv::BodyBroken& e) {
    for (size_t i = 0; i < e.shards; i++) {
        Entity shard = e.pieces[i];
        const projv::RigidBody& rigid = world.get<projv::RigidBody>(shard);
        o.slippery.push_back({shard, rigid.friction, rigid.material, SLIPPERY_STEPS});
        world.patch<projv::RigidBody>(shard, [](projv::RigidBody& r) {
            r.friction = 0.0f;
            r.material.clear();   // a material's friction would override the field
        });
        o.toThrow.push_back(shard);
    }
}

void throwChip(World& world, Demos::Overhangs& o) {
    for (Entity shard : o.toThrow) {
        if (!world.valid(shard) || !runtime::hasBody(world, shard)) continue;
        vec3 from = runtime::bodyPose(world, shard).centerOfMass - o.chipAt;
        float distance = glm::length(from);
        vec3 spread = distance > 1e-3f ? from / distance : o.chipOut;
        vec3 velocity = glm::normalize(o.chipOut + 0.5f * spread) * o.chipSpeed + vec3(0, 1.5f, 0);
        runtime::setVelocity(world, shard, velocity, glm::cross(o.chipOut, spread) * 4.0f);
    }
    o.toThrow.clear();
}

// Thrown pieces whose moment is up get their friction and material back.
void restoreChips(World& world, Demos::Overhangs& o) {
    for (auto& s : o.slippery)
        if (--s.steps == 0 && world.valid(s.piece) && world.all_of<projv::RigidBody>(s.piece))
            world.patch<projv::RigidBody>(s.piece, [&s](projv::RigidBody& r) {
                r.friction = s.friction;
                r.material = s.material;
            });
    std::erase_if(o.slippery, [](const Demos::Overhangs::Slippery& s) { return s.steps <= 0; });
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
        case 11: buildOverhangs(world, demos, camera); break;
    }
    projv::core::info("Demo {}: {}", n, demoName(n));
}

Entity fireBall(World& world, Demos& demos, int size, vec3 from, vec3 velocity, float density = 800.0f) {
    Thing t = place(world, demos.balls[size], from, quat(1, 0, 0, 0), ballBody(density, 0.35f));
    runtime::setVelocity(world, t.entity, velocity, vec3(0));
    return t.entity;
}

// The blast in demo 2, in two parts. First, the engine's fracture (runtime::fracture): every brick
// within 3.5 m is struck at its middle with the blast's energy, falling off with the square of the
// distance -- the bricks beside it shatter into small pieces, those further out lose a corner or
// crack in two, by the brick's own material (strength 40 kJ/m^3, shattering). Second, two steps
// later, the kick: everything near, the shards included, thrown outward. What is thrown breaks again
// where it hits something hard enough (the engine does that on impact) -- the shards too, as the
// bricks they came from do, until they are too small for a hit to break (minImpactBreak).
constexpr float BLAST_ENERGY = 80000.0f, BLAST_REACH = 3.5f;   // J at the centre, m

void detonate(World& world, Demos& demos) {
    const vec3 at = demos.blastAt;
    std::vector<Entity> near;
    for (Entity e : world.view<projv::RigidBody>())
        if (glm::length(runtime::bodyPose(world, e).position - at) < BLAST_REACH) near.push_back(e);
    for (Entity e : near) {
        vec3 middle = runtime::bodyPose(world, e).centerOfMass;
        float falloff = 1.0f - glm::length(middle - at) / BLAST_REACH;
        runtime::Strike strike;
        strike.at = middle;
        strike.energy = BLAST_ENERGY * falloff * falloff;
        runtime::fracture(world, e, strike);
    }
    demos.blastKickIn = 2;
    demos.stepSum = demos.stepWorst = 0.0;
    demos.stepCount = 0;
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
        case 11: chip(world, camera, CHIP_ENERGY, CHIP_SPEED, CHIP_SHARD); break;
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
    {
        Demos& demos = app.world.ctx().get<Demos>();
        for (auto [e, body] : app.world.view<projv::PhysicsBody>().each()) {
            if (body.motion != runtime::MotionType::Dynamic || body.position.y > 0.0f) continue;
            if (std::abs(body.position.x) > FLOOR_HALF || std::abs(body.position.z) > FLOOR_HALF) demos.overEdge.insert(e);
        }
    }
    World& world = app.world;
    auto& demos = world.ctx().get<Demos>();
    const float dt = app.time().fixedDelta;
    demos.clock += dt;
    dragTick(world, demos, dt);
    if (demos.current == 2) {
        double ms = runtime::physicsWorld(world).stats().lastStepMilliseconds;
        demos.stepSum += ms;
        demos.stepWorst = std::max(demos.stepWorst, ms);
        demos.stepCount++;
    }
    if (demos.blastKickIn > 0 && --demos.blastKickIn == 0) blastKick(world, demos);
    if (!demos.overhangs.toThrow.empty()) throwChip(world, demos.overhangs);
    if (!demos.overhangs.slippery.empty()) restoreChips(world, demos.overhangs);
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
    if (input.pressed(projv::Key::Minus)) startDemo(world, 11);
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
    if (input.pressed(projv::Key::F) && demos.current == 11 && captured) chip(world, camera, BLOW_ENERGY, BLOW_SPEED, 0.0f);
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
    static std::pair<uint64_t, uint64_t> wallBefore;   // demo 11: the composite wall's rebar and all its voxels
    static bool wallChecked = false;
    // DEMOS_CAPTURE=<prefix>: a frame of each demo, mid-action, as <prefix>-<demo>.tga.
    static int captured = 0;
    const float captureAt[LAST_DEMO + 1] = {0, 14.6f, 2.6f, 5.0f, 9.0f, 3.3f, 5.6f, 6.5f, 12.0f, 6.1f, 3.4f, 4.0f};
    if (const char* prefix = std::getenv("DEMOS_CAPTURE"); prefix && stage >= 1 && stage <= LAST_DEMO && captured < stage &&
        t > captureAt[stage]) {
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, (std::string(prefix) + "-" + std::to_string(stage)).c_str());
        captured = stage;
    }
    // On to the first selected demo from `next`, or to the session-wide checks after the last.
    auto go = [&](int next) {
        while (next <= LAST_DEMO && !test->only.empty() &&
               std::find(test->only.begin(), test->only.end(), next) == test->only.end())
            next++;
        acted = false;
        if (next > LAST_DEMO) { stage = LAST_DEMO + 1; return; }
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
    // DEMOS_BLASTS=n: n blasts a second apart round the structures -- the wall, the tower, the house,
    // the columns, the long wall -- then the frames' and steps' cost while they last.
    static const int blasts = std::max(1, std::getenv("DEMOS_BLASTS") ? std::atoi(std::getenv("DEMOS_BLASTS")) : 1);
    static int blasted = 0;
    static double frameSum = 0.0, frameWorst = 0.0;
    static int frames = 0;
    static uint64_t droppedBefore = 0;
    static auto lastFrame = std::chrono::steady_clock::now();
    if (stage == 2 && blasts > 1) {
        auto now = std::chrono::steady_clock::now();
        if (blasted > 0) {
            double ms = std::chrono::duration<double, std::milli>(now - lastFrame).count();
            frameSum += ms;
            frameWorst = std::max(frameWorst, ms);
            frames++;
        }
        lastFrame = now;
        if (blasted < blasts && t > 2.0f + float(blasted)) {
            static const vec3 spots[6] = {{0.3f, 2.2f, 0.9f},  {-15.0f, 3.0f, 6.0f}, {12.5f, 2.0f, 6.5f},
                                          {-6.0f, 3.0f, 12.5f}, {0.0f, 1.2f, 20.8f}, {6.0f, 4.5f, 12.5f}};
            if (blasted == 0) {
                droppedBefore = app.time().droppedFixedSteps;
                demos.stepSum = demos.stepWorst = 0.0;
                demos.stepCount = 0;
            }
            demos.blastAt = spots[blasted % 6] + vec3(0.4f * float(blasted / 6), 0.3f * float(blasted / 6 % 3), 0.0f);
            double sum = demos.stepSum, worst = demos.stepWorst;
            int count = demos.stepCount;
            detonate(world, demos);
            demos.stepSum = sum; demos.stepWorst = worst; demos.stepCount = count;   // over all the blasts
            blasted++;
        }
        if (t > 2.0f + float(blasts) + 5.0f) {
            projv::core::info("DEMOTEST: {} blasts: frames {:.1f} ms on average, {:.1f} ms at worst; physics step {:.2f} ms on "
                              "average, {:.2f} ms at worst; {} steps dropped; {} bodies, {} pieces",
                              blasts, frameSum / std::max(frames, 1), frameWorst, demos.stepSum / std::max(demos.stepCount, 1),
                              demos.stepWorst, app.time().droppedFixedSteps - droppedBefore,
                              world.view<projv::RigidBody>().size(), demos.pieces);
            go(3);
            test->start = watchedPositions();
        }
        return;
    }
    if (stage == 2 && t > 2.0f && !acted) { act(world, demos, camera); acted = true; }
    if (stage == 2 && t > 7.0f) {
        size_t bodies = world.view<projv::RigidBody>().size();
        check(*test, demos.pieces >= 10, fmt::format("the blast split bricks: {} fragments, {} bodies", demos.pieces, bodies));
        projv::core::info("DEMOTEST: blast physics step: {:.2f} ms on average, {:.2f} ms at worst, {} threads",
                          demos.stepSum / std::max(demos.stepCount, 1), demos.stepWorst,
                          runtime::physicsWorld(world).stats().threads);
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
        // Four shots, one at each wall: the first as fired above, the rest from a machine rebuilt
        // facing the next wall. Each is checked 8 s after it is fired.
        static const char* const walls[4] = {"ashlar", "brick", "planks", "plaster"};
        static const float aims[4] = {0.3f, 1.5708f + 0.2f, 3.1416f - 0.25f, -1.5708f + 0.15f};
        static bool rebuilt[4] = {}, checked[4] = {};
        for (int shot = 0; shot < 4; shot++) {
            const float firedAt = 2.5f + 9.0f * float(shot), checkAt = firedAt + 8.0f;
            if (shot > 0 && t > firedAt - 0.5f && !rebuilt[shot]) {
                rebuilt[shot] = true;
                piecesBefore = demos.pieces;
                tb.aim = aims[shot];
                buildTrebuchetMachine(world, demos);
                tb.fireIn = 0.5f;
            }
            if (!(t > checkAt) || checked[shot]) continue;
            checked[shot] = true;
            float thrown = glm::length(vec2(tb.impactAt.x, tb.impactAt.z) - vec2(tb.releasedAt.x, tb.releasedAt.z));
            check(*test, tb.released, fmt::format("{}: the sling let the boulder go at {:.1f} m/s, {:.1f} m up", walls[shot],
                                                  tb.releaseSpeed, tb.releasedAt.y));
            check(*test, tb.struck && tb.struckWall == shot,
                  fmt::format("{}: the boulder broke the {} wall, striking at {:.1f} m/s", walls[shot],
                              tb.struckWall >= 0 ? walls[tb.struckWall] : "no", tb.impactSpeed));
            check(*test, demos.pieces - piecesBefore >= 10,
                  fmt::format("{}: the wall broke up: {} pieces", walls[shot], demos.pieces - piecesBefore));
            check(*test, tb.struck && thrown > 20.0f, fmt::format("{}: the boulder flew {:.1f} m from the release to the wall",
                                                                  walls[shot], thrown));
            // Where it ended up: in the rubble, or on through the wall (off the floor's edge, even).
            if (world.valid(tb.boulder)) {
                vec3 at = runtime::bodyPose(world, tb.boulder).position;
                projv::core::info("DEMOTEST: {}: the boulder came to {:.1f} m from the middle (the wall is at {:.0f} m)", walls[shot],
                                  glm::length(vec2(at.x, at.z)), WALL_DISTANCE);
            } else {
                projv::core::info("DEMOTEST: {}: the boulder went on through the wall and off the edge of the floor", walls[shot]);
            }
            if (shot == 3) {
                go(10);   // the slide
                return;
            }
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
    // The tree, its trunk cut through by four hard blows round it 1.5 m up: the crown, held up by
    // nothing now, is a body of its own -- cut level, it settles on the stump, so it is nudged off
    // (as right-drag would) and falls.
    auto crown = [&] {
        Entity found = projv::NullEntity;
        float height = 0.0f;
        for (const projv::BodyBroken& split : demos.overhangs.fell)
            if (split.entity == demos.overhangs.tree)
                for (Entity piece : split.pieces)
                    if (world.valid(piece) && runtime::hasBody(world, piece)) {
                        float y = runtime::bodyPose(world, piece).centerOfMass.y;
                        if (found == projv::NullEntity || y > height) { found = piece; height = y; }
                    }
        return std::make_pair(found, height);
    };
    if (stage == 11 && t > 1.0f && !acted) {
        const vec3 trunk(-6.5f + 2.5f, 1.5f, 5.0f + 2.5f);
        for (vec3 side : {vec3(-1, 0, 0), vec3(1, 0, 0), vec3(0, 0, -1), vec3(0, 0, 1)}) {
            runtime::Strike strike;
            strike.at = trunk + side * 0.55f;
            strike.energy = TRUNK_BLOW;
            strike.push = -side * 2.0f;
            runtime::fracture(world, demos.overhangs.tree, strike);
        }
        // And F's blow, on the composite wall's middle beam above the brick bay.
        wallBefore = wallVoxels(world, demos.overhangs.wall);
        runtime::Strike blow;
        blow.at = COMPOSITE_CORNER + vec3(26, 31, 0) * COMPOSITE_VOXEL + vec3(0, 0, 0.1f);
        blow.energy = BLOW_ENERGY;
        runtime::fracture(world, demos.overhangs.wall, blow);
        acted = true;
    }
    if (stage == 11 && t > 3.0f && !wallChecked) {
        auto [rebar, all] = wallVoxels(world, demos.overhangs.wall);
        size_t pieces = 0;
        for (const projv::BodyBroken& e : demos.overhangs.fell)
            if (e.entity == demos.overhangs.wall) pieces += e.pieces.size();
        check(*test, all + 2000 < wallBefore.second && pieces > 20 && rebar == wallBefore.first && rebar > 0,
              fmt::format("F broke {} voxels out of the composite wall in {} pieces; its {} voxels of rebar all stayed ({} now)",
                          wallBefore.second - all, pieces, wallBefore.first, rebar));
        wallChecked = true;
    }
    static bool nudged = false;
    if (stage == 11 && t > 3.0f && !nudged) {
        auto [piece, height] = crown();
        check(*test, piece != projv::NullEntity && height > 5.0f,
              fmt::format("the trunk cut through, the crown came loose: {} pieces fell, the crown's middle {:.1f} m up",
                          demos.overhangs.fell.size(), height));
        if (piece != projv::NullEntity) runtime::addVelocity(world, piece, vec3(3, 0, 0));
        nudged = true;
    }
    if (stage == 11 && t > 8.0f) {
        auto [piece, height] = crown();
        // Off the stump it drops; where it comes to rest -- flat, or leaning on the stump -- varies.
        check(*test, piece != projv::NullEntity && height < 5.5f,
              fmt::format("nudged off its stump, the crown fell: its middle {:.1f} m up (it hung at 6 m)", height));
        go(LAST_DEMO + 1);
        return;
    }
    if (stage == LAST_DEMO + 1) {
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
    // DEMOS_PHYSICS_THREADS: the physics step's worker threads, for measuring (default: the engine's).
    if (const char* threads = std::getenv("DEMOS_PHYSICS_THREADS")) physics.settings.threads = std::atoi(threads);
    physics.dustVoxels = 5;   // what breaks into crumbs this small is gone, not a body
    runtime::installPhysics(app, physics);
    runtime::installImpactFracture(app);   // what is struck hard enough breaks (demos 2, 9, 11)
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
    // Delivered inside the physics step, before the pieces' bodies are made.
    app.events().on<projv::BodyBroken>([&world](const projv::BodyBroken& e) {
        Demos& demos = world.ctx().get<Demos>();
        demos.pieces += int(e.pieces.size());
        if (demos.current == 11) demos.overhangs.fell.push_back(e);
        // Demo 11: a click's shards, thrown out of the face.
        if (demos.current == 11 && demos.overhangs.chipped && e.cause == projv::BreakCause::Strike &&
            e.striker == projv::NullEntity) {
            demos.overhangs.chipped = false;
            slipChip(world, demos.overhangs, e);
        }
        // Demo 9: the boulder broke a wall.
        Demos::Trebuchet& tb = demos.trebuchet;
        if (e.striker != projv::NullEntity && e.striker == tb.boulder && !tb.struck) {
            tb.struck = true;
            // The speed it struck at, from the strike's energy (all of it the boulder's: the wall
            // does not move).
            float mass = world.valid(tb.boulder) && world.all_of<projv::PhysicsBody>(tb.boulder)
                             ? runtime::physicsWorld(world).bodyMass(world.get<projv::PhysicsBody>(tb.boulder).id)
                             : 0.0f;
            tb.impactSpeed = mass > 0.0f ? std::sqrt(2.0f * e.energy / mass) : 0.0f;
            tb.impactAt = e.at;
            for (size_t k = 0; k < tb.walls.size(); k++)
                if (tb.walls[k].entity == e.entity) tb.struckWall = int(k);
        }
    });
    app.events().on<projv::JointBroke>([&world](const projv::JointBroke& e) {
        world.ctx().get<Demos>().brokenJoints.push_back(e.entity);
    });
    app.events().on<projv::BodyEmptied>([&world](const projv::BodyEmptied& e) {
        if (world.valid(e.entity)) world.destroy(e.entity);
    });
    app.events().on<projv::BodyLeftWorld>([&world](const projv::BodyLeftWorld& e) {
        Demos& demos = world.ctx().get<Demos>();
        if (demos.overEdge.erase(e.entity)) return;   // went over the edge, not through the floor
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
        startDemo(world, std::clamp(std::atoi(benchDemo), 1, LAST_DEMO));
    } else if (unattended) {
        SelfTest& test = world.ctx().emplace<SelfTest>();
        if (const char* only = std::getenv("DEMOS_ONLY"))
            for (const char* c = only; *c;) {
                char* end = nullptr;
                long n = std::strtol(c, &end, 10);
                if (end == c) { c++; continue; }   // a separator
                if (n >= 1 && n <= LAST_DEMO) test.only.push_back(int(n));
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
