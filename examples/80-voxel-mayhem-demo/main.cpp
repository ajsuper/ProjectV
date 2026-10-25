// Voxel Mayhem -- every physics feature ProjectV has, in one world, all running at once.
//
// A 220 m park of voxels, every piece of it simulated, laid out round a plaza:
//
//   South   the playground   a ball pit of rubber balls behind a gate that a pressure plate opens
//                            (sensor), three jump pads (sensors) fed by a ball fountain, helium
//                            balloons on ropes (negative gravity factor, distance joints that break),
//                            three slides of ice, polished wood and slate into walls of rubber, oak and
//                            clay (physics materials), and a car made of joints, with a stunt ramp
//   Centre  the plaza        a Ferris wheel on a motorised hinge, its gondolas on ball joints in the
//                            same collision family as the wheel, a crate riding in each
//   North   the castle       four walls of ashlar, brick, plaster and planks that split and break on
//                            impact, each its own way; a brick keep with a balcony; a crane's wrecking
//                            ball (B) on a rope; a composite wall of concrete, rebar, brick, pine and
//                            glass; a stack of TNT by the gate
//   East    the stacks       a 240-domino spiral set off by a pressure plate, a crate pyramid, a
//                            Jenga tower
//   West    the machine yard a rope bridge of planks, chains that snap (breakForce), a piston
//                            elevator (a slider joint's servo), a bungee platform hung on springs, and
//                            the Blender: a kinematic beam spinning through a field of crates
//   N.West  the quarry       a sandstone arch, a timber tree, a chalk cliff, a shale cliff that breaks
//                            in slabs (a fracture pattern registered here), and an earth hill held up
//                            by bedrock (an anchor material): dig under it and it falls
//   N.East  the bowling lane a 6 m stone ball (N) into ten 5 m pins that collide as capsules
//
// Tools (1-8, or the wheel), used with a click:
//   1 cannon    a half-tonne iron ball at 50 m/s         5 spawn    a crate, barrel, ball, glass box
//   2 blast     an explosion where you aim                          or balloon (Q picks which)
//   3 pickaxe   chips what you aim at, throwing the chip  6 TNT      thrown; goes off when it hits hard,
//   4 dig       carves a 1.5 m hole out of anything                 and sets off the TNT round it
//               (voxel edits; what is cut loose falls)    7 orb      a 4 m kinematic ball on a
//   8 push      a shove of everything in front of you               constant velocity, ploughing on
//
// Other keys
//   right-drag  hold and drag a body (the wheel: nearer/farther)   V  walk / fly   E  drive the car
//   B  wrecking ball: let go / wind back    N  bowl (again: re-rack)   G  gravity: Earth / Moon / none
//   T  slow motion   P  pause   F5  snapshot   F9  rewind to it   R  rebuild the world
//   H  sun shadows   O  ambient occlusion   I  GI   K  TAA
//   U  debug view (AO / sun visibility / indirect light)
//   [ ]  the sun   F1  this help   Esc  free the cursor
//
//   cd build/examples/voxel_mayhem_demo && ./voxel_mayhem_demo
//
//   MAYHEM_SELFTEST=1 ./voxel_mayhem_demo   plays itself for a while and checks that every feature did what
//                                      it should (MAYHEM_CAPTURE=<prefix> also saves screenshots)
//   MAYHEM_BENCH=cannon|blast|barrage  times a break at the castle wall (MAYHEM_ONLY=castle: nothing else)
//   MAYHEM_MAP=<folder>                 an authored map (compose.json, entities.json, physics_materials.json:
//                                      the Teardown imports, say) in place of the park, to break with the
//                                      same tools, brought to life size (MAYHEM_MAP_SCALE, default 0.1); the
//                                      park's set pieces (B, N, R, E) are not there. Its bodies start asleep
//                                      (MAYHEM_MAP_AWAKE=1: not); bodies the converter left overlapping are
//                                      welded, and world voxels inside a body cleared (MAYHEM_MAP_SCAN=1
//                                      reports the overlaps and exits); its world collides as its tile tables
//                                      (MAYHEM_COLLISION=boxes: as boxes); pieces of up to 64 voxels are cheap
//                                      debris (MAYHEM_DEBRIS=<voxels>, 0: bodies)
//   MAYHEM_LOOK=x,y,z,tx,ty,tz          a still from there, at 4 s, then exits (MAYHEM_FIRE, MAYHEM_AO,
//                                      MAYHEM_GI, MAYHEM_DEBUG set it up); MAYHEM_GPU=1 prints each pass's GPU time

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <bgfx/bgfx.h>
#include <GLFW/glfw3.h>

#include "voxel_renderer.h"
#include "restir_test.h"   // MAYHEM_RENDERER=restir-pt: an experiment
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
#include "runtime/entities.h"
#include "runtime/scene_bridge.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/scene_query.h"

using projv::ComponentHandle;
using projv::Entity;
using projv::NullEntity;
using projv::World;
using projv::core::ivec3;
using projv::core::quat;
using projv::core::vec2;
using projv::core::vec3;
using projv::core::vec4;
namespace fs = std::filesystem;
namespace runtime = projv::runtime;
namespace utils = projv::utils;

namespace {

constexpr float PI = 3.14159265f;
constexpr float FLOOR_HALF = 110.0f;   // the floor is 220 m square, its top at y = 0
constexpr float EYE_HEIGHT = 1.6f;
constexpr float WALK_SPEED = 7.0f, FLY_SPEED = 24.0f;
constexpr float WALK_GRIP = 12.0f, AIR_GRIP = 1.5f, FLY_GRIP = 5.0f;
constexpr uint32_t SMALL_DEBRIS_VOXELS = 3;   // walked through and aimed through
// On a map (10 cm voxels, soft materials) a break makes thousands of shards, and each landing on the
// map's detailed ground is what a step spent its time on (measured: a blast at the marina, 1,900 pieces,
// 64 ms of physics a frame). A piece this small or smaller is light debris there: it collides as one box
// fitted to it, without continuous collision, and not with other debris.
constexpr uint32_t MAP_DEBRIS_VOXELS = 64;
constexpr int SHARD_FAMILY_STEPS = 15;        // fixed steps a break's shards pass through each other
const quat IDENTITY(1, 0, 0, 0);

uint32_t rgb(float r, float g, float b) { return projv::packRGB10(r, g, b); }
uint32_t rgb(vec3 c) { return projv::packRGB10(c.r, c.g, c.b); }

vec3 hsv(float h, float s, float v) {
    vec3 k = glm::clamp(glm::abs(glm::fract(vec3(h) + vec3(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
    return v * glm::mix(vec3(1.0f), k, s);
}

// =================================================================================================
// Building voxel things (the same helpers the engine's physics demos use)
// =================================================================================================

// A component of `size` voxels of `voxel` metres, coloured by `colourAt` (0 = empty), its corner at
// `position`. A 16-voxel chunk to start; anything bigger becomes a grid.
template <typename ColourAt>
ComponentHandle voxels(projv::Scene& scene, const char* name, ivec3 size, float voxel, vec3 position, ColourAt colourAt,
                       ComponentHandle parent = projv::INVALID_COMPONENT_HANDLE) {
    ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, name, parent, 16, voxel);
    utils::setComponentTransform(scene, h, position, IDENTITY, 1.0f);
    std::vector<projv::PendingVoxelOp> ops;
    for (int z = 0; z < size.z; z++)
        for (int y = 0; y < size.y; y++)
            for (int x = 0; x < size.x; x++)
                if (uint32_t c = colourAt(x, y, z)) ops.push_back({true, ivec3(x, y, z), c});
    utils::queueVoxelAdd(scene, h, ops);
    utils::updateScene(scene);
    return h;
}

// Every palette entry of `h` glows in its own colour.
void glow(projv::Scene& scene, ComponentHandle h, float strength) {
    const auto& palette = scene.components[h].materialPalette;
    for (size_t slot = 0; slot < palette.size(); slot++)
        utils::setMaterialProperties(scene, h, uint8_t(slot), palette[slot].packedColor,
                                     projv::packSurfaceWord(0.0f, 0.0f, 0.0f, 1.0f),
                                     projv::packExtraWord(strength, 0.0f, 0u));
}

// Every palette entry of `h` is made of `material` (to physics).
void madeOf(projv::Scene& scene, ComponentHandle h, const char* material) {
    for (size_t slot = 0; slot < scene.components[h].materialPalette.size(); slot++)
        utils::setMaterialPhysics(scene, h, uint8_t(slot), material);
}

// A prototype: an asset node at its centre with its voxels inside, never drawn where anything is
// (parked far below) and never given a body. Everything that moves is a duplicateComponent of one,
// so copies share geometry on the GPU and one collision shape in the physics.
struct Proto {
    ComponentHandle node = projv::INVALID_COMPONENT_HANDLE;
    ComponentHandle chunk = projv::INVALID_COMPONENT_HANDLE;
};

template <typename ColourAt>
Proto prototype(projv::Scene& scene, const char* name, ivec3 size, float voxel, ColourAt colourAt) {
    Proto p;
    p.node = utils::addComponent(scene, projv::ComponentKind::Asset, name, projv::INVALID_COMPONENT_HANDLE, 4, 1.0f);
    p.chunk = voxels(scene, "voxels", size, voxel, -0.5f * vec3(size) * voxel, colourAt, p.node);
    utils::setComponentTransform(scene, p.node, vec3(0, -1000, 0), IDENTITY, 1.0f);
    return p;
}

Entity place(World& world, const Proto& proto, vec3 position, quat rotation, const projv::RigidBody& rigid) {
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle node = utils::duplicateComponent(scene, proto.node);
    utils::setComponentTransform(scene, node, position, rotation, 1.0f);
    Entity e = runtime::spawnComponent(world, node, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    world.emplace<projv::RigidBody>(e, rigid);
    runtime::assignNetId(world, e);
    return e;
}

// A copy that is only drawn: no body (rope links, a fireball).
Entity decoration(World& world, const Proto& proto, vec3 position, quat rotation = IDENTITY) {
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle node = utils::duplicateComponent(scene, proto.node);
    utils::setComponentTransform(scene, node, position, rotation, 1.0f);
    return runtime::spawnComponent(world, node, projv::LinkMode::Root, projv::OnUnlink::Destroy);
}

projv::RigidBody body(float mass = 0.0f, float restitution = 0.0f, float friction = 0.6f) {
    projv::RigidBody r;
    r.mass = mass;
    r.restitution = restitution;
    r.friction = friction;
    return r;
}

projv::RigidBody ballBody(float density, float restitution, const char* material = "") {
    projv::RigidBody r;
    r.shape = projv::RigidBody::Shape::Sphere;
    r.density = density;
    r.restitution = restitution;
    r.material = material;
    return r;
}

// A big moving thing built in place: an asset node at `centre` with its voxels inside.
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

projv::RigidBody tntBody() {
    projv::RigidBody r = body(30.0f);
    r.material = "tnt";
    return r;
}

uint32_t stone(int x, int y, int z) {
    float shade = 0.75f + 0.25f * float((x * 13 + y * 7 + z * 11) % 5) / 4.0f;
    return rgb(0.52f * shade, 0.5f * shade, 0.47f * shade);
}

// =================================================================================================
// State
// =================================================================================================

enum class Tool { Cannon = 1, Blast, Pickaxe, Dig, Spawn, Tnt, Orb, Push };
constexpr int TOOL_COUNT = 8;
const char* toolName(Tool t) {
    switch (t) {
        case Tool::Cannon: return "cannon";
        case Tool::Blast: return "blast";
        case Tool::Pickaxe: return "pickaxe";
        case Tool::Dig: return "dig";
        case Tool::Spawn: return "spawn";
        case Tool::Tnt: return "TNT";
        case Tool::Orb: return "orb";
        case Tool::Push: return "push";
    }
    return "";
}
enum class SpawnKind { Crate, Barrel, Ball, Glass, Balloon, Count };
const char* spawnName(SpawnKind k) {
    switch (k) {
        case SpawnKind::Crate: return "crate";
        case SpawnKind::Barrel: return "barrel";
        case SpawnKind::Ball: return "bouncy ball";
        case SpawnKind::Glass: return "glass box";
        case SpawnKind::Balloon: return "balloon";
        default: return "";
    }
}

struct Camera {
    vec3 position{0.0f, 16.0f, -100.0f};
    float yaw = PI * 0.5f, pitch = -0.12f;
    Entity walker = NullEntity;
    vec3 velocity{0.0f};
    bool padJump = false;   // a jump pad under the walker: the next jump is a big one
    bool driving = false;
    vec3 forward() const { return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)}; }
};

// Trauma that impacts and explosions add and time drains; the drawn camera moves and turns by its
// square, so a light knock barely shows and a close blast jolts. How much a thing adds depends on how far
// off it is: all of it within SHAKE_NEAR, falling off linearly to nothing at SHAKE_FAR.
// (A TNT crate at 20 m: about 17 cm and 1 degree; at 50 m, 4 cm and a quarter of a degree.)
struct Shake { float trauma = 0.0f, clock = 0.0f; };
constexpr float SHAKE_FULL = 4.0e5f;                 // J: an impact this big is a full shake, a tenth of it none
constexpr float SHAKE_NEAR = 8.0f, SHAKE_FAR = 80.0f; // m
constexpr float SHAKE_DECAY = 1.2f;                  // trauma lost per second
constexpr float SHAKE_MOVE = 0.25f, SHAKE_TURN = 0.025f;   // m and rad, at full trauma

void addShake(World& world, vec3 at, float amount) {
    float distance = glm::length(at - world.ctx().get<Camera>().position);
    float reach = std::clamp((SHAKE_FAR - distance) / (SHAKE_FAR - SHAKE_NEAR), 0.0f, 1.0f);
    Shake& shake = world.ctx().get<Shake>();
    shake.trauma = std::min(1.0f, shake.trauma + amount * reach);
}

// A rope, drawn only: links strung between a point on `a` and a point on `b` (or a world point), for
// as long as its joint holds.
struct Rope {
    Entity joint = NullEntity;
    Entity a = NullEntity, b = NullEntity;
    vec3 localA{0.0f}, localB{0.0f};   // in each body's frame; localB is a world point when b is null
    std::vector<Entity> links;
};

// An explosion's kick, two fixed steps after its strikes, so the shards they make are thrown too.
struct Kick { vec3 at; float radius; float speed; int steps; };
struct Fuse { Entity tnt; float seconds; };

// A chip's shards are frictionless for half a second so they fly out of their sockets (see throwChip).
struct Slippery {
    Entity piece;
    float friction;
    std::string material;
    std::vector<std::pair<uint8_t, std::string>> entries;
    int steps;
};

struct Car {
    Entity chassis = NullEntity;
    Entity wheels[4]{}, axles[4]{}, steering[2]{};
    float throttle = 0.0f, steer = 0.0f;
    bool brake = false, driven = false;
};

struct Mayhem {
    // Prototypes.
    Proto crate, balls[5], cannonball, domino, jenga, plank, link, wheel, knuckle, ropeLink, pin, bowlingBallProto, tntCrate,
        barrel, glassBox, balloon, orb, gondola, wreckerProto, fireball, beam;

    Entity floor = NullEntity;
    bool map = false;            // MAYHEM_MAP: an authored map instead of the park
    std::vector<Entity> owned;   // everything the world was built of that is not swept up by body type

    Tool tool = Tool::Cannon;
    SpawnKind spawnKind = SpawnKind::Crate;
    int gravityMode = 0;   // Earth, Moon, none

    // Rings of what the player and the spawners made, oldest first: past their cap the oldest goes.
    std::vector<Entity> cannonballs, spawned, fountain, bungeeDrops;
    std::vector<std::pair<Entity, float>> orbs;   // and how long each has left
    std::set<Entity> tnt;
    std::vector<Fuse> fuses;
    std::vector<Kick> kicks;
    std::vector<Rope> ropes;
    struct Flash { Entity e; float age; };
    std::vector<Flash> flashes;

    // The playground.
    std::map<Entity, vec3> pads;   // jump pads and the velocity each gives
    Entity gate = NullEntity, gateBody = NullEntity, gatePlate = NullEntity;   // the gate's hinge, its slab, its plate
    bool gateOpen = false;
    float fountainTimer = 0.0f;
    struct Slide { Entity crate = NullEntity; vec3 corner{0.0f}; };
    Slide slides[3];
    quat slideRotation{1, 0, 0, 0};
    float slideTimer = 0.0f;
    Car car;

    // The plaza.
    Entity ferris = NullEntity;

    // The castle.
    Entity wrecker = NullEntity, wreckerLatch = NullEntity;
    vec3 wreckerStart{0.0f}, wreckerPivot{0.0f};
    int relatchIn = 0;   // fixed steps until the latch is made again, after the ball is wound back
    bool wreckerStruck = false;

    // The stacks.
    std::vector<Entity> dominoes;
    Entity dominoPlate = NullEntity;
    vec3 dominoPush{0.0f};
    bool dominoesStarted = false;

    // The machine yard.
    Entity elevator = NullEntity, elevatorPlatform = NullEntity;
    float elevatorTimer = 0.0f;
    bool elevatorUp = false;
    Entity bungeePlate = NullEntity;
    float bungeeTimer = 0.0f;
    Entity blender = NullEntity;

    // The bowling lane.
    Entity bowlingBall = NullEntity;
    std::vector<Entity> pins;
    float bowlIn = -1.0f;

    // Pickaxe chips.
    bool chipped = false;
    Entity chipTarget = NullEntity;
    vec3 chipAt{0.0f}, chipDirection{0.0f, 0.0f, 1.0f};
    std::vector<Entity> toThrow;
    std::vector<Slippery> slippery;
    std::vector<std::pair<Entity, int>> newborn;   // shards still in their break's family, and for how many steps

    struct Grab { Entity body = NullEntity; vec3 local{0.0f}; float distance = 0.0f; } grab;
    std::vector<uint8_t> snapshot;

    // What happened, for the title bar and the self-test.
    int pieces = 0, jointsBroken = 0, explosions = 0, padLaunches = 0, fellThrough = 0, piecesCleared = 0;
    float clock = 0.0f, titleTimer = 0.0f;
    std::mt19937 random{1234};
    bool unattended = false;
};

float uniform(Mayhem& m, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(m.random); }

// Keeps a ring of at most `cap` entities: past it the oldest is destroyed.
void keep(World& world, std::vector<Entity>& ring, Entity e, size_t cap) {
    ring.push_back(e);
    std::erase_if(ring, [&](Entity x) { return !world.valid(x); });
    while (ring.size() > cap) {
        world.destroy(ring.front());
        ring.erase(ring.begin());
    }
}

Entity own(Mayhem& m, Entity e) {
    m.owned.push_back(e);
    return e;
}

// Static scenery: voxels whose corner is at `corner`, solid.
template <typename ColourAt>
Entity scenery(World& world, Mayhem& m, const char* name, ivec3 size, float voxel, vec3 corner, ColourAt colourAt,
               projv::StaticCollider collider = {}) {
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle h = voxels(scene, name, size, voxel, corner, colourAt);
    Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    world.emplace<projv::StaticCollider>(e, collider);
    return own(m, e);
}

Entity joint(World& world, Mayhem& m, Entity a, Entity b, projv::Joint j) { return own(m, runtime::connectBodies(world, a, b, j)); }

// A rope drawn between two points, each on a body or (b null) in the world, held by `j`.
void addRope(World& world, Mayhem& m, Entity j, Entity a, vec3 worldA, Entity b, vec3 worldB, int links) {
    Rope r;
    r.joint = j;
    r.a = a;
    r.b = b;
    auto local = [&](Entity e, vec3 p) {
        const projv::Transform& t = world.get<projv::Transform>(e);
        return glm::inverse(t.rotation) * (p - t.position);
    };
    r.localA = local(a, worldA);
    r.localB = b == NullEntity ? worldB : local(b, worldB);
    for (int i = 0; i < links; i++) r.links.push_back(decoration(world, m.ropeLink, worldA));
    m.ropes.push_back(std::move(r));
}

// =================================================================================================
// A fracture pattern of our own: shale parts along its bedding, in slabs
// =================================================================================================

// Registered as "slabs" and named by the shale in physics_materials.json. Each voxel within the
// reach joins the slab of its layer (half a shard size thick) and its cell across (two shard sizes):
// flat plates, the way bedded rock splits. Deterministic, as a pattern must be -- the only randomness
// is FractureRandom's, which shifts the cells so no two strikes cut the same grid.
std::vector<int32_t> slabs(const runtime::FractureInput& in, runtime::FractureRandom& random) {
    const float thick = std::max(1.0f, in.shardSize * 0.5f), across = std::max(2.0f, in.shardSize * 2.0f);
    const vec2 offset(random.uniform(0.0f, across), random.uniform(0.0f, across));
    std::map<std::tuple<int, int, int>, int32_t> ids;
    std::vector<int32_t> shard(in.voxels.size(), -1);
    for (size_t i = 0; i < in.voxels.size(); i++) {
        const vec3 centre = vec3(in.voxels[i].position) + 0.5f;
        if (glm::distance(centre, in.at) > in.reach) continue;
        auto key = std::make_tuple(int(std::floor(centre.y / thick)), int(std::floor((centre.x + offset.x) / across)),
                                   int(std::floor((centre.z + offset.y) / across)));
        auto [it, added] = ids.try_emplace(key, int32_t(ids.size()));
        shard[i] = it->second;
    }
    return shard;
}

// =================================================================================================
// The playground (south)
// =================================================================================================

// A trigger volume that stays put: drawn, thin, glowing, sensing what comes into it. Not solid --
// what lands on it stands on the floor beneath.
Entity sensorPad(World& world, Mayhem& m, vec3 corner, ivec3 size, vec3 colour, float strength) {
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle h = voxels(scene, "pad", size, 0.125f, corner, [&](int x, int, int z) {
        bool edge = x == 0 || z == 0 || x == size.x - 1 || z == size.z - 1;
        return rgb(edge ? colour * 0.5f : colour);
    });
    glow(scene, h, strength);
    Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    projv::StaticCollider sensor;
    sensor.layer = runtime::PhysicsLayer::Sensor;
    world.emplace<projv::StaticCollider>(e, sensor);
    runtime::assignNetId(world, e);
    return own(m, e);
}

void buildBallPit(World& world, Mayhem& m) {
    // Walls 1.5 m high round a 10 m square, a 4 m gap in the south wall, which the gate closes.
    const vec3 corner(-33.0f, 0.0f, -50.0f);
    scenery(world, m, "ball pit", ivec3(44, 6, 44), 0.25f, corner, [](int x, int y, int z) -> uint32_t {
        bool wall = x < 2 || x >= 42 || z < 2 || z >= 42;
        bool gap = z < 2 && x >= 14 && x < 30;
        if (!wall || gap) return 0u;
        return (x + z) % 4 < 2 ? rgb(0.9f, 0.85f, 0.2f) : rgb(0.2f, 0.5f, 0.9f);
    });
    // A hundred and forty rubber balls, piled.
    int n = 0;
    for (int layer = 0; layer < 4 && n < 140; layer++)
        for (int i = 0; i < 9 && n < 140; i++)
            for (int k = 0; k < 9 && n < 140; k++, n++) {
                int size = 1 + (i * 7 + k * 3 + layer) % 3;
                vec3 at = corner + vec3(1.2f + float(i) * 0.95f, 0.6f + float(layer) * 0.95f, 1.4f + float(k) * 0.95f);
                place(world, m.balls[size], at, IDENTITY, ballBody(300.0f, 0.8f, "bouncy"));
            }
    // The gate: 4 m wide, hinged on its west edge, a servo holding it shut until the plate is pressed. It
    // hangs just outside the wall's face rather than in the gap: turning in the gap, its corners swung
    // into the wall beside it and jammed. Outside, opening (outward) swings it away from the wall.
    projv::RigidBody slab = body(200.0f);
    m.gate = own(m, big(world, "gate", ivec3(32, 12, 2), 0.125f, corner + vec3(5.5f, 0.78f, -0.135f), IDENTITY,
                        [](int x, int y, int) -> uint32_t {
                            bool frame = x < 1 || x > 30 || y < 1 || y > 10 || x == 15 || x == 16;
                            return frame ? rgb(0.35f, 0.22f, 0.1f) : rgb(0.65f, 0.45f, 0.24f);
                        },
                        slab));
    projv::Joint hinge;
    hinge.type = runtime::JointType::Hinge;
    hinge.anchor = corner + vec3(3.5f, 0.78f, -0.135f);
    hinge.axis = vec3(0, 1, 0);
    hinge.limitMin = -0.1f;
    hinge.limitMax = 1.7f;
    hinge.motor = true;
    hinge.motorMode = runtime::MotorMode::Position;
    hinge.motorTarget = 0.0f;
    hinge.motorLimit = 40000.0f;
    hinge.motorFrequency = 2.0f;
    m.gateBody = m.gate;
    m.gate = joint(world, m, m.gateBody, NullEntity, hinge);   // from here on, m.gate is the hinge
    m.gatePlate = sensorPad(world, m, corner + vec3(4.0f, 0.0f, -6.0f), ivec3(24, 1, 24), vec3(1.0f, 0.75f, 0.1f), 1.0f);
}

void buildJumpPads(World& world, Mayhem& m) {
    for (int i = 0; i < 3; i++) {
        Entity pad = sensorPad(world, m, vec3(-7.5f + float(i) * 6.0f, 0.0f, -62.0f), ivec3(24, 1, 24), vec3(0.1f, 0.9f, 1.0f), 4.0f);
        m.pads[pad] = vec3(float(i - 1) * 2.0f, 17.0f, 11.0f);   // up and north, fanned out
    }
}

// Helium: a sphere with a negative gravity factor, on a rope (a distance joint that can slacken but not
// stretch, and breaks past 2.5 kN) to a crate that is just heavy enough to hold it down -- or, for the
// escapees, three balloons on one light crate, which take it away into the sky until it leaves the world.
void buildBalloons(World& world, Mayhem& m) {
    projv::RigidBody helium = ballBody(40.0f, 0.5f, "balloon");
    helium.gravityFactor = -0.6f;
    helium.linearDamping = 0.8f;
    helium.angularDamping = 0.8f;
    auto tie = [&](Entity balloon, vec3 at, Entity crate, vec3 crateTop) {
        projv::Joint rope;
        rope.type = runtime::JointType::Distance;
        rope.anchor = at - vec3(0, 0.6f, 0);
        rope.anchorB = crateTop;
        rope.limitMin = 0.0f;
        rope.limitMax = 3.5f;
        rope.breakForce = 2500.0f;
        Entity j = joint(world, m, balloon, crate, rope);
        addRope(world, m, j, balloon, rope.anchor, crate, rope.anchorB, 12);
    };
    for (int i = 0; i < 8; i++) {
        vec3 base(-14.0f + float(i % 4) * 3.0f, 0.5f, -36.0f + float(i / 4) * 3.0f);
        Entity crate = place(world, m.crate, base, IDENTITY, body(35.0f));
        vec3 at = base + vec3(0, 3.8f, 0);
        tie(place(world, m.balloon, at, IDENTITY, helium), at, crate, base + vec3(0, 0.5f, 0));
    }
    for (int i = 0; i < 3; i++) {
        vec3 base(0.0f + float(i) * 4.0f, 0.5f, -32.0f);
        Entity crate = place(world, m.crate, base, IDENTITY, body(10.0f));
        for (int k = 0; k < 3; k++) {
            vec3 at = base + vec3(float(k - 1) * 1.3f, 3.6f + float(k % 2) * 0.4f, 0.0f);
            tie(place(world, m.balloon, at, IDENTITY, helium), at, crate, base + vec3(0, 0.5f, 0));
        }
    }
}

// Physics materials: three 14 m ramps -- ice, polished wood, slate -- the same crate on each, into
// walls of rubber, oak and clay at their feet. Every 10 s the crates start again.
constexpr float SLIDE_ANGLE = 0.349f, SLIDE_VOXEL = 0.25f;
constexpr int SLIDE_LENGTH = 56, SLIDE_WIDTH = 8;

void placeSlideCrates(World& world, Mayhem& m) {
    for (Mayhem::Slide& s : m.slides) {
        if (world.valid(s.crate)) world.destroy(s.crate);
        vec3 centre = s.corner + m.slideRotation * vec3(1.0f, 1.01f, 0.5f * SLIDE_WIDTH * SLIDE_VOXEL);
        projv::RigidBody r = body(40.0f, 0.0f, 1.0f);
        r.linearDamping = 0.0f;
        s.crate = place(world, m.crate, centre, m.slideRotation, r);
    }
}

void buildSlides(World& world, Mayhem& m) {
    auto& scene = world.ctx().get<projv::Scene>();
    m.slideRotation = glm::angleAxis(-SLIDE_ANGLE, vec3(0, 0, 1));   // local +x runs down the slope
    const float length = SLIDE_LENGTH * SLIDE_VOXEL, top = length * std::sin(SLIDE_ANGLE);
    struct Look { const char* ramp; const char* wall; vec3 a, b; vec3 wallColour; };
    const Look looks[3] = {
        {"ice", "rubber", {0.78f, 0.9f, 0.98f}, {0.62f, 0.8f, 0.95f}, {0.85f, 0.2f, 0.15f}},
        {"polished wood", "oak", {0.72f, 0.5f, 0.28f}, {0.62f, 0.42f, 0.22f}, {0.5f, 0.33f, 0.16f}},
        {"slate", "clay", {0.3f, 0.32f, 0.36f}, {0.38f, 0.4f, 0.45f}, {0.72f, 0.45f, 0.3f}},
    };
    auto materialScenery = [&](ivec3 size, vec3 corner, const char* material, auto colourAt) {
        ComponentHandle h = voxels(scene, material, size, SLIDE_VOXEL, corner, colourAt);
        utils::setComponentTransform(scene, h, corner, m.slideRotation, 1.0f);
        madeOf(scene, h, material);
        Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        world.emplace<projv::StaticCollider>(e);
        own(m, e);
    };
    for (int i = 0; i < 3; i++) {
        const Look& look = looks[i];
        Mayhem::Slide& s = m.slides[i];
        s.corner = vec3(16.0f, top, -70.0f + 4.0f * float(i));
        materialScenery(ivec3(SLIDE_LENGTH, 2, SLIDE_WIDTH), s.corner, look.ramp,
                        [&look](int x, int, int) { return rgb((x / 3) % 2 ? look.a : look.b); });
        materialScenery(ivec3(2, 8, SLIDE_WIDTH + 2), s.corner + m.slideRotation * vec3(length - 0.5f, 0.5f, -0.25f), look.wall,
                        [&look](int, int y, int) { return rgb(look.wallColour * (y % 2 ? 1.0f : 0.85f)); });
        scenery(world, m, "pillar", ivec3(4, int(top / SLIDE_VOXEL) - 2, SLIDE_WIDTH), SLIDE_VOXEL, vec3(16.0f, 0.0f, s.corner.z),
                [](int, int y, int) { return y % 4 ? rgb(0.45f, 0.45f, 0.47f) : rgb(0.38f, 0.38f, 0.4f); });
    }
    placeSlideCrates(world, m);
}

// A car made of joints: a 900 kg body, rear wheels on driven hinges, front wheels on free hinges to
// knuckles that a servo hinge turns -- the steering. The wheels collide as spheres.
constexpr float WHEEL_RADIUS = 0.4375f, WHEEL_TOP_SPEED = 45.0f, STEER_LOCK = 0.5f;

void buildCar(World& world, Mayhem& m, vec3 at) {
    Car& car = m.car;
    projv::RigidBody shell;
    shell.mass = 900.0f;
    car.chassis = big(world, "car", ivec3(16, 10, 32), 0.125f, at + vec3(0, 0.95f, 0), IDENTITY, [](int x, int y, int z) -> uint32_t {
        bool tub = y < 4;
        bool cabin = y >= 4 && z >= 8 && z < 22 && x >= 1 && x < 15;
        if (!tub && !cabin) return 0u;
        if (cabin) return (y >= 6 && (x == 1 || x == 14 || z == 8 || z == 21)) ? rgb(0.5f, 0.7f, 0.85f) : rgb(0.1f, 0.45f, 0.85f);
        return (z < 2 || z > 29) ? rgb(0.95f, 0.9f, 0.3f) : rgb(0.1f, 0.45f, 0.85f);
    }, shell);
    const float track = 1.5f, base = 1.4f;
    const vec3 corners[4] = {vec3(-track, 0, base), vec3(track, 0, base), vec3(-track, 0, -base), vec3(track, 0, -base)};
    projv::RigidBody tyre = ballBody(1200.0f, 0.0f);
    tyre.mass = 25.0f;
    tyre.friction = 1.4f;
    for (int i = 0; i < 4; i++) {
        vec3 centre = at + corners[i] + vec3(0, WHEEL_RADIUS, 0);
        car.wheels[i] = place(world, m.wheel, centre, IDENTITY, tyre);
        projv::Joint axle;
        axle.type = runtime::JointType::Hinge;
        axle.anchor = centre;
        axle.axis = vec3(1, 0, 0);
        axle.motorLimit = i < 2 ? 500.0f : 900.0f;
        if (i < 2) {
            Entity knuckle = place(world, m.knuckle, centre, IDENTITY, body(15.0f));
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
            car.steering[i] = joint(world, m, knuckle, car.chassis, steer);
            car.axles[i] = joint(world, m, car.wheels[i], knuckle, axle);
        } else {
            car.axles[i] = joint(world, m, car.wheels[i], car.chassis, axle);
        }
    }
}

void driveCar(World& world, Mayhem& m, float throttle, float steer, bool brake) {
    Car& car = m.car;
    if (!world.valid(car.chassis)) return;
    if (car.driven && throttle == car.throttle && steer == car.steer && brake == car.brake) return;
    car.driven = true;
    car.throttle = throttle;
    car.steer = steer;
    car.brake = brake;
    for (int i = 0; i < 4; i++) {
        if (!world.valid(car.axles[i])) continue;
        if (brake) runtime::setJointMotor(world, car.axles[i], true, 0.0f);
        else if (i >= 2 && throttle != 0.0f) runtime::setJointMotor(world, car.axles[i], true, throttle * WHEEL_TOP_SPEED);
        else runtime::setJointMotor(world, car.axles[i], false, 0.0f);
    }
    for (int i = 0; i < 2; i++)
        if (world.valid(car.steering[i]))
            runtime::setJointMotor(world, car.steering[i], true, steer * STEER_LOCK, runtime::MotorMode::Position);
}

void buildCarPark(World& world, Mayhem& m) {
    // A ramp to a 2 m lip and a jump, then a wall of crates and balls to drive through.
    scenery(world, m, "stunt ramp", ivec3(48, 16, 80), 0.125f, vec3(42.0f, 0.0f, -62.0f), [](int, int y, int z) -> uint32_t {
        return y <= z * 16 / 80 ? (z % 8 < 4 ? rgb(0.85f, 0.3f, 0.2f) : rgb(0.95f, 0.95f, 0.95f)) : 0u;
    });
    for (int row = 0; row < 3; row++)
        for (int i = 0; i < 7; i++)
            place(world, m.crate, vec3(41.5f + float(i) * 1.02f, 0.5f + float(row) * 1.005f, -36.0f), IDENTITY, body(30.0f));
    buildCar(world, m, vec3(45.0f, 0.0f, -84.0f));
}

void buildPlayground(World& world, Mayhem& m) {
    buildBallPit(world, m);
    buildJumpPads(world, m);
    buildBalloons(world, m);
    buildSlides(world, m);
    buildCarPark(world, m);
}

// =================================================================================================
// The plaza: a Ferris wheel
// =================================================================================================

void buildFerrisWheel(World& world, Mayhem& m) {
    const vec3 hub(0.0f, 12.0f, 18.0f);
    const float radius = 9.0f;
    // The wheel and its gondolas are one collision family: they never collide with each other, only
    // with what rides in them.
    const projv::CollisionGroups family{{}, {}, 7};
    projv::RigidBody rim;
    rim.density = 250.0f;
    rim.angularDamping = 0.0f;
    m.ferris = own(m, big(world, "ferris wheel", ivec3(76, 76, 2), 0.25f, hub, IDENTITY, [](int x, int y, int) -> uint32_t {
        vec2 d = vec2(float(x), float(y)) + 0.5f - 38.0f;
        float r = glm::length(d);
        if (r > 38.0f) return 0u;
        bool ring = r > 35.0f;
        float angle = std::atan2(d.y, d.x);
        bool spoke = r > 2.0f && std::abs(std::sin(4.0f * angle)) * r < 1.1f;
        bool hubDisc = r < 3.0f;
        if (!ring && !spoke && !hubDisc) return 0u;
        if (ring) return int(angle * 6.0f + 20.0f) % 2 ? rgb(0.95f, 0.3f, 0.35f) : rgb(0.98f, 0.95f, 0.9f);
        return rgb(0.85f, 0.85f, 0.9f);
    }, rim));
    world.emplace<projv::CollisionGroups>(m.ferris, family);
    projv::Joint axle;
    axle.type = runtime::JointType::Hinge;
    axle.anchor = hub;
    axle.axis = vec3(0, 0, 1);
    axle.motor = true;
    axle.motorVelocity = 0.25f;
    axle.motorLimit = 4.0e6f;
    joint(world, m, m.ferris, NullEntity, axle);
    // The stand: two legs behind the wheel up to the axle.
    scenery(world, m, "ferris stand", ivec3(64, 50, 4), 0.25f, vec3(-8.0f, 0.0f, 18.75f), [](int x, int y, int) -> uint32_t {
        float h = float(y) / 49.0f;
        float left = 32.0f * h, right = 64.0f - 32.0f * h;   // two legs meeting at the top
        bool leg = std::abs(float(x) - left) < 1.5f || std::abs(float(x) - right) < 1.5f || (y == 10 && x > left && x < right);
        return leg ? rgb(0.4f, 0.42f, 0.48f) : 0u;
    });
    // Eight gondolas, each on a ball joint at the rim, hanging in front of the wheel's plane, a crate
    // riding in each.
    for (int i = 0; i < 8; i++) {
        float a = float(i) * PI / 4.0f;
        vec3 pivot = hub + vec3(std::cos(a) * radius, std::sin(a) * radius, -1.8f);
        vec3 centre = pivot - vec3(0, 1.625f, 0);
        Entity g = place(world, m.gondola, centre, IDENTITY, body(120.0f));
        world.emplace<projv::CollisionGroups>(g, family);
        projv::Joint hang;
        hang.type = runtime::JointType::Point;
        hang.anchor = pivot;
        joint(world, m, g, m.ferris, hang);
        Entity rider = place(world, m.crate, centre - vec3(0, 0.55f, 0), glm::angleAxis(a, vec3(0, 1, 0)), body(20.0f));
        (void)rider;
    }
}

// =================================================================================================
// The castle (north)
// =================================================================================================

constexpr float WALL_VOXEL = 0.25f;
constexpr int WALL_W = 128, WALL_H = 48, WALL_T = 4;
const vec3 CASTLE(0.0f, 0.0f, 72.0f);

bool crenel(int x, int y) { return y < WALL_H - 3 || (x / 4) % 2 == 0; }

uint32_t wallStone(int x, int y, int z) {
    bool mortar = y % 3 == 0 || (x + (y / 3) % 2 * 3) % 6 == 0;
    return mortar ? rgb(0.62f, 0.6f, 0.55f) : stone(x, y, z);
}
uint32_t wallBrick(int x, int y, int) {
    int course = y / 3, shifted = x + course % 2 * 3;
    if (y % 3 == 2 || shifted % 6 == 5) return rgb(0.74f, 0.71f, 0.65f);
    int which = (course * 7 + shifted / 6 * 3) % 3;
    static const float reds[3][3] = {{0.55f, 0.22f, 0.15f}, {0.63f, 0.28f, 0.18f}, {0.47f, 0.19f, 0.14f}};
    return rgb(reds[which][0], reds[which][1], reds[which][2]);
}
uint32_t wallPlanks(int x, int y, int) {
    if (y % 3 == 0) return rgb(0.3f, 0.2f, 0.12f);
    int board = y / 3;
    float shade = 0.85f + 0.15f * float((board * 5 + (x / 9 + board) % 3) % 4) / 3.0f;
    return board % 2 ? rgb(0.58f * shade, 0.4f * shade, 0.23f * shade) : rgb(0.5f * shade, 0.34f * shade, 0.19f * shade);
}
uint32_t wallPlaster(int x, int y, int) {
    int blotch = ((x / 5) * 73 + (y / 4) * 151 + (x / 11) * 17) % 7;
    return blotch < 2 ? rgb(0.8f, 0.77f, 0.7f) : rgb(0.87f, 0.85f, 0.79f);
}
uint32_t towerBrick(int x, int y, int z) {
    int course = y / 3, shifted = x + course % 2 * 3, deep = z + course % 2 * 2;
    if (y % 3 == 2 || shifted % 6 == 5 || deep % 4 == 3) return rgb(0.74f, 0.71f, 0.65f);
    int which = (course * 7 + shifted / 6 * 3 + deep / 4 * 5) % 3;
    static const float reds[3][3] = {{0.55f, 0.22f, 0.15f}, {0.63f, 0.28f, 0.18f}, {0.47f, 0.19f, 0.14f}};
    return rgb(reds[which][0], reds[which][1], reds[which][2]);
}

// A static thing of `material` that splits (what a break leaves hanging falls), standing on its own
// y = 0, and breaks where it is hit hard enough.
template <typename ColourAt>
Entity breakable(World& world, Mayhem& m, const char* material, ivec3 size, float voxel, vec3 corner, quat rotation,
                 ColourAt colourAt, bool anchorBase = true) {
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle h = voxels(scene, material, size, voxel, corner, colourAt);
    utils::setComponentTransform(scene, h, corner, rotation, 1.0f);
    Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    projv::StaticCollider c;
    c.material = material;
    c.splits = true;
    c.anchorBase = anchorBase;
    c.breaksOnImpact = true;
    world.emplace<projv::StaticCollider>(e, c);
    runtime::assignNetId(world, e);
    return own(m, e);
}

// The composite wall: a concrete frame with steel rebar through it, and bays of brick in mortar, pine
// boards with a window, and a glass curtain. Every colour names its material, so a blow breaks each
// part its own way, as far as it reaches in that material.
void buildCompositeWall(World& world, Mayhem& m, vec3 corner) {
    std::map<uint32_t, const char*> materialOf;
    auto paint = [&materialOf](uint32_t colour, const char* material) { materialOf[colour] = material; return colour; };
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
            const int cx = x < 8 ? x : x < 52 ? x - 44 : x - 88;
            const bool upright = column && (cx == 2 || cx == 5) && (z == 1 || z == 2);
            const int by = y >= 58 ? y - 58 : y - 28;
            const bool along = beam && (by == 1 || by == 4) && (z == 1 || z == 2);
            if (upright || along) return rebar;
            return concrete[(x / 3 + y / 3 + z) % 2];
        }
        const bool left = x < 44, lower = y < 28;
        const int bx = left ? x - 8 : x - 52, by = lower ? y : y - 34;
        auto brick = [&] {
            const int course = by / 3, shifted = bx + course % 2 * 2;
            if (by % 3 == 2 || shifted % 5 == 4) return mortar;
            return bricks[(course * 7 + shifted / 5 * 3) % 3];
        };
        auto pane = [&] { return z == 1 || z == 2 ? glass[(x + y) % 7 == 0] : 0u; };
        if (left && lower) return brick();
        if (!left && lower) {
            if (bx >= 8 && bx < 28 && by >= 8 && by < 24) return pane();
            return pine[(by / 3) % 2];
        }
        if (left) {
            if (bx % 9 == 0 || by == 11 || by == 12) return pine[(bx + by) % 2];
            return pane();
        }
        if (bx >= 12 && bx < 24 && by >= 6 && by < 18) return pane();
        return brick();
    };
    Entity e = breakable(world, m, "concrete", ivec3(96, 64, 4), 0.125f, corner, IDENTITY, colourAt);
    auto& scene = world.ctx().get<projv::Scene>();
    ComponentHandle h = runtime::voxelComponentOf(world, e);
    const auto& palette = scene.components[h].materialPalette;
    for (size_t slot = 0; slot < palette.size(); slot++)
        if (auto found = materialOf.find(palette[slot].packedColor); found != materialOf.end())
            utils::setMaterialPhysics(scene, h, uint8_t(slot), found->second);
}

void buildCastle(World& world, Mayhem& m) {
    // Four walls 32 m long and 12 m high, each one seamless static grid of its own material, each
    // breaking its own way: the front (south) ashlar with a gateway, brick to the east, planks to the
    // north, plaster to the west. Crenellated.
    using Colour = uint32_t (*)(int, int, int);
    static const Colour colours[4] = {wallBrick, wallStone, wallPlaster, wallPlanks};
    static const char* const materials[4] = {"brick", "ashlar", "plaster", "planks"};
    for (int side = 0; side < 4; side++) {
        vec3 out = glm::angleAxis(float(side) * PI * 0.5f, vec3(0, 1, 0)) * vec3(1, 0, 0), along(-out.z, 0, out.x);
        quat rotation = glm::quat_cast(glm::mat3(along, vec3(0, 1, 0), -out));
        vec3 corner = CASTLE + out * 16.5f - along * (0.5f * float(WALL_W) * WALL_VOXEL);
        Colour colour = colours[side];
        const bool front = side == 1;
        breakable(world, m, materials[side], ivec3(WALL_W, WALL_H, WALL_T), WALL_VOXEL, corner, rotation,
                  [colour, front](int x, int y, int z) -> uint32_t {
                      if (!crenel(x, y)) return 0u;
                      if (front && x >= 56 && x < 72 && y < 26) return 0u;   // the gateway
                      return colour(x, y, z);
                  });
    }
    // The keep: a 4 m brick tower 14 m high with a balcony at 9 m, in the courtyard.
    breakable(world, m, "tower brick", ivec3(32, 56, 16), 0.25f, CASTLE + vec3(-2.0f, 0.0f, 0.0f), IDENTITY,
              [](int x, int y, int z) -> uint32_t {
                  bool tower = x < 16 && (y < 53 || (x + z) % 4 < 2);
                  bool balcony = y >= 36 && y <= 38 && x < 30 && z >= 2 && z < 14;
                  return tower || balcony ? towerBrick(x, y, z) : 0u;
              });
    // Beside the castle, the composite wall.
    buildCompositeWall(world, m, vec3(22.0f, 0.0f, 60.0f));

    // A stack of TNT by the gate: set one off and the rest go with it.
    for (int layer = 0; layer < 3; layer++)
        for (int i = 0; i < 3 - layer; i++)
            for (int k = 0; k < 3 - layer; k++) {
                vec3 at(9.0f + (float(i) + 0.5f * float(layer)) * 1.02f, 0.5f + float(layer) * 1.005f, 49.0f + (float(k) + 0.5f * float(layer)) * 1.02f);
                m.tnt.insert(place(world, m.tntCrate, at, IDENTITY, tntBody()));
            }

    // The crane: a tower and a jib, and from the jib's end a 4 t iron ball on a 13 m rope, held back
    // by a latch (a ball joint to the world) until B lets it go, to swing into the front wall.
    const float ropeLength = 13.0f, pulledBack = 75.0f * PI / 180.0f;
    m.wreckerPivot = vec3(-8.0f, 18.0f, CASTLE.z - 16.0f);
    m.wreckerStart = m.wreckerPivot + ropeLength * vec3(0, -std::cos(pulledBack), -std::sin(pulledBack));
    scenery(world, m, "crane", ivec3(20, 78, 92), 0.25f, vec3(-12.5f, 0.0f, CASTLE.z - 39.0f), [](int x, int y, int z) -> uint32_t {
        bool tower = x < 4 && z < 4;
        bool jib = y >= 74 && x >= 14 && x < 18;
        bool arm = y >= 74 && z < 4;
        bool lattice = (x + y + z) % 3 == 0;
        if (!tower && !jib && !arm) return 0u;
        return lattice ? rgb(0.95f, 0.75f, 0.1f) : rgb(0.85f, 0.62f, 0.08f);
    });
    projv::RigidBody iron = ballBody(7800.0f, 0.1f, "iron");
    iron.mass = 4000.0f;
    iron.quality = projv::RigidBody::Quality::Continuous;
    m.wrecker = place(world, m.wreckerProto, m.wreckerStart, IDENTITY, iron);
}

// =================================================================================================
// The stacks (east)
// =================================================================================================

void buildStacks(World& world, Mayhem& m) {
    // A domino spiral: an Archimedean spiral from 14 m in to 5 m, a domino every metre along it,
    // each turned to face along the path. The first stands just past a pressure plate.
    const vec3 centre(64.0f, 0.0f, -6.0f);
    const float b = 2.3f / (2.0f * PI), r0 = 5.0f, r1 = 14.0f;
    float theta = r1 / b;
    m.dominoes.clear();
    int index = 0;
    while (b * theta > r0) {
        float r = b * theta;
        vec3 p = centre + vec3(std::cos(theta) * r, 1.0f, std::sin(theta) * r);
        // The path runs inward (theta falling): its tangent.
        vec3 t = glm::normalize(vec3(-(b * std::cos(theta) - r * std::sin(theta)), 0.0f, -(b * std::sin(theta) + r * std::cos(theta))));
        quat facing = glm::angleAxis(std::atan2(-t.z, t.x), vec3(0, 1, 0));
        Entity d = place(world, m.domino, p, facing, body(20.0f, 0.0f, 0.5f));
        if (index == 0) {
            m.dominoPush = t;
            m.dominoPlate = sensorPad(world, m, p - t * 2.5f - vec3(1.0f, 1.0f, 1.0f), ivec3(16, 1, 16), vec3(1.0f, 0.3f, 0.6f), 1.5f);
        }
        m.dominoes.push_back(d);
        theta -= 1.05f / std::sqrt(r * r + b * b);
        index++;
    }
    projv::core::info("mayhem: {} dominoes", m.dominoes.size());

    // A pyramid of crates, eight high.
    for (int layer = 0; layer < 8; layer++) {
        int count = 8 - layer;
        for (int i = 0; i < count; i++)
            for (int k = 0; k < count; k++)
                place(world, m.crate,
                      vec3(92.0f + (float(i) - float(count - 1) * 0.5f) * 1.02f, 0.5f + float(layer) * 1.005f,
                           -14.0f + (float(k) - float(count - 1) * 0.5f) * 1.02f),
                      IDENTITY, body(40.0f));
    }
    // A Jenga tower: sixteen layers of three blocks, crossing.
    const quat turned = glm::angleAxis(PI * 0.5f, vec3(0, 1, 0));
    for (int layer = 0; layer < 16; layer++)
        for (int i = 0; i < 3; i++) {
            float off = (float(i) - 1.0f) * 1.01f, y = 0.25f + float(layer) * 0.502f;
            bool alongZ = layer % 2;
            place(world, m.jenga, vec3(90.0f + (alongZ ? off : 0.0f), y, 16.0f + (alongZ ? 0.0f : off)), alongZ ? turned : IDENTITY,
                  body(25.0f, 0.0f, 0.5f));
        }
}

// =================================================================================================
// The machine yard (west)
// =================================================================================================

void buildMachineYard(World& world, Mayhem& m) {
    // 1. A rope bridge: 24 planks between two towers, each joined to the next by a ball joint at
    //    either side, the ends to the world.
    {
        const float x = -60.0f, y = 4.0f, pitch = 0.52f;
        const int planks = 24;
        const float z0 = 20.0f - float(planks - 1) * pitch * 0.5f;
        Entity previous = NullEntity;
        auto seam = [&](Entity a, Entity b, float z) {
            for (float side : {-0.55f, 0.55f}) {
                projv::Joint j;
                j.type = runtime::JointType::Point;
                j.anchor = vec3(x + side, y, z);
                joint(world, m, a, b, j);
            }
        };
        for (int i = 0; i < planks; i++) {
            float z = z0 + float(i) * pitch;
            Entity p = place(world, m.plank, vec3(x, y, z), IDENTITY, body(8.0f));
            seam(p, previous, z - pitch * 0.5f);
            previous = p;
        }
        seam(previous, NullEntity, z0 + float(planks - 1) * pitch + pitch * 0.5f);
        for (float z : {z0 - pitch * 0.5f - 1.0f, z0 + float(planks) * pitch - pitch * 0.5f})
            scenery(world, m, "bridge tower", ivec3(12, 34, 8), 0.125f, vec3(x - 0.75f, 0.0f, z),
                    [](int, int y, int) { return y > 30 ? rgb(0.3f, 0.8f, 0.5f) : rgb(0.3f, 0.3f, 0.34f); });
    }
    // 2. Chains that snap: three chains of six links on a gallows, each holding a 300 kg weight, each
    //    joint breaking at 9 kN -- three times what the weight pulls at rest. Shoot them.
    scenery(world, m, "gallows", ivec3(4, 4, 80), 0.125f, vec3(-48.25f, 9.5f, 5.0f), [](int, int, int) { return rgb(0.35f, 0.25f, 0.15f); });
    scenery(world, m, "gallows post", ivec3(4, 76, 4), 0.125f, vec3(-48.25f, 0.0f, 4.5f), [](int, int, int) { return rgb(0.35f, 0.25f, 0.15f); });
    for (int c = 0; c < 3; c++) {
        const vec3 top(-47.75f, 9.5f, 7.0f + float(c) * 2.5f);
        Entity previous = NullEntity;
        for (int i = 0; i < 6; i++) {
            vec3 centre = top - vec3(0, 0.375f + 0.75f * float(i), 0);
            Entity l = place(world, m.link, centre, IDENTITY, body(4.0f));
            projv::Joint j;
            j.type = runtime::JointType::Point;
            j.anchor = centre + vec3(0, 0.375f, 0);
            j.breakForce = 9000.0f;
            joint(world, m, l, previous, j);
            previous = l;
        }
        Entity weight = place(world, m.crate, top - vec3(0, 5.0f, 0), IDENTITY, body(300.0f));
        projv::Joint hook;
        hook.type = runtime::JointType::Point;
        hook.anchor = top - vec3(0, 4.5f, 0);
        hook.breakForce = 9000.0f;
        joint(world, m, weight, previous, hook);
    }
    // 3. A piston elevator: a platform on a slider joint to the world, its servo carrying it between
    //    the floor and 8.5 m every seven seconds, balls and crates riding on it.
    {
        const vec3 at(-72.0f, 0.3f, 8.0f);
        m.elevatorPlatform = big(world, "elevator", ivec3(24, 4, 24), 0.125f, at, IDENTITY, [](int x, int y, int z) -> uint32_t {
            bool rail = y >= 1 && (x == 0 || x == 23 || z == 0 || z == 23);
            if (y >= 1 && !rail) return 0u;
            return rail ? rgb(0.95f, 0.75f, 0.1f) : ((x / 4 + z / 4) % 2 ? rgb(0.3f, 0.3f, 0.33f) : rgb(0.38f, 0.38f, 0.42f));
        }, body(500.0f));
        projv::Joint piston;
        piston.type = runtime::JointType::Slider;
        piston.anchor = at;
        piston.axis = vec3(0, 1, 0);
        piston.limitMin = 0.0f;
        piston.limitMax = 9.0f;
        piston.motor = true;
        piston.motorMode = runtime::MotorMode::Position;
        piston.motorTarget = 0.0f;
        piston.motorLimit = 1.5e5f;
        piston.motorFrequency = 1.0f;
        m.elevator = joint(world, m, m.elevatorPlatform, NullEntity, piston);
        for (int i = 0; i < 4; i++)
            place(world, m.balls[3], at + vec3(-0.8f + float(i % 2) * 1.6f, 0.6f, -0.8f + float(i / 2) * 1.6f), IDENTITY,
                  ballBody(500.0f, 0.3f));
        // A tower beside it, to see the height against.
        scenery(world, m, "elevator tower", ivec3(4, 72, 4), 0.125f, vec3(-74.25f, 0.0f, 6.0f),
                [](int, int y, int) { return (y / 8) % 2 ? rgb(0.95f, 0.75f, 0.1f) : rgb(0.15f, 0.15f, 0.17f); });
    }
    // 4. A bungee platform: a rubber plate hung by four springs (distance joints with a spring) from a
    //    frame, with balls dropped on it.
    {
        const vec3 at(-60.0f, 2.0f, 40.0f);
        scenery(world, m, "bungee frame", ivec3(40, 56, 40), 0.125f, at + vec3(-2.5f, -2.0f, -2.5f), [](int x, int y, int z) -> uint32_t {
            bool post = (x < 2 || x >= 38) && (z < 2 || z >= 38);
            bool top = y >= 54 && (x < 2 || x >= 38 || z < 2 || z >= 38);
            return post || top ? rgb(0.25f, 0.55f, 0.3f) : 0u;
        });
        projv::RigidBody plate = body(60.0f, 0.6f, 0.9f);
        m.bungeePlate = big(world, "bungee", ivec3(24, 2, 24), 0.125f, at, IDENTITY,
                            [](int x, int, int z) { return (x + z) % 2 ? rgb(0.9f, 0.2f, 0.5f) : rgb(0.75f, 0.15f, 0.4f); }, plate);
        for (int c = 0; c < 4; c++) {
            vec3 corner = at + vec3(c % 2 ? 1.4f : -1.4f, 0.1f, c / 2 ? 1.4f : -1.4f);
            vec3 hook = at + vec3(c % 2 ? 2.25f : -2.25f, 4.85f, c / 2 ? 2.25f : -2.25f);
            projv::Joint spring;
            spring.type = runtime::JointType::Distance;
            spring.anchor = corner;
            spring.anchorB = hook;
            float length = glm::length(hook - corner);
            spring.limitMin = spring.limitMax = length;
            spring.springFrequency = 2.0f;
            spring.springDamping = 0.05f;
            Entity j = joint(world, m, m.bungeePlate, NullEntity, spring);
            addRope(world, m, j, m.bungeePlate, corner, NullEntity, hook, 10);
        }
    }
    // 5. The Blender: a 12 m kinematic beam spinning about its middle while its middle circles, through
    //    a field of crates and balls. Moved by its Transform; physics follows, pushing what it meets.
    {
        const vec3 c(-60.0f, 0.0f, -28.0f);
        for (int i = 0; i < 60; i++) {
            float angle = uniform(m, 0, 2 * PI), radius = uniform(m, 1.5f, 10.0f);
            vec3 at = c + vec3(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);
            if (i % 3 == 0) place(world, m.balls[i % 5], at + vec3(0, 1.5f, 0), IDENTITY, ballBody(600.0f, 0.2f));
            else place(world, m.crate, at + vec3(0, 0.5f, 0), glm::angleAxis(angle, vec3(0, 1, 0)), body(20.0f));
        }
        projv::RigidBody kinematic;
        kinematic.motion = runtime::MotionType::Kinematic;
        m.blender = place(world, m.beam, c + vec3(0, 0.6f, 0), IDENTITY, kinematic);
        scenery(world, m, "blender ring", ivec3(104, 4, 104), 0.25f, c - vec3(13.0f, 0.0f, 13.0f), [](int x, int, int z) -> uint32_t {
            float r = glm::length(vec2(float(x), float(z)) + 0.5f - 52.0f);
            return r > 50.0f && r < 52.0f ? rgb(0.15f, 0.15f, 0.17f) : 0u;
        });
    }
}

// =================================================================================================
// The quarry (north-west)
// =================================================================================================

uint32_t sandstone(int x, int y, int z) {
    float shade = 0.85f + 0.15f * float((x * 7 + y * 13 + z * 5) % 4) / 3.0f;
    return (y / 2) % 3 == 0 ? rgb(0.8f * shade, 0.66f * shade, 0.45f * shade) : rgb(0.86f * shade, 0.72f * shade, 0.5f * shade);
}
uint32_t chalk(int x, int y, int z) { return (x * 3 + y * 5 + z * 7) % 6 == 0 ? rgb(0.82f, 0.82f, 0.78f) : rgb(0.92f, 0.92f, 0.88f); }
uint32_t shale(int, int y, int) {
    static const vec3 beds[4] = {{0.32f, 0.33f, 0.36f}, {0.4f, 0.4f, 0.42f}, {0.28f, 0.29f, 0.33f}, {0.45f, 0.43f, 0.4f}};
    return rgb(beds[(y / 2) % 4]);
}

void buildQuarry(World& world, Mayhem& m) {
    const float v = 0.25f;
    // A sandstone arch (shatters): cut one pillar and the span hangs from the other.
    breakable(world, m, "sandstone", ivec3(32, 30, 4), v, vec3(-90.0f, 0.0f, 70.0f), IDENTITY, [](int x, int y, int z) -> uint32_t {
        bool pillar = (x >= 4 && x <= 7) || (x >= 24 && x <= 27);
        return (pillar && y < 24) || y >= 24 ? sandstone(x, y, z) : 0u;
    });
    // A timber tree (splinters along its upright grain): cut the trunk and the crown comes down.
    breakable(world, m, "timber", ivec3(20, 28, 20), v, vec3(-79.0f, 0.0f, 68.0f), IDENTITY, [](int x, int y, int z) -> uint32_t {
        bool trunk = x >= 8 && x <= 11 && z >= 8 && z <= 11 && y < 22;
        float dx = float(x) - 9.5f, dz = float(z) - 9.5f;
        bool crown = y >= 22 && dx * dx + dz * dz <= 9.5f * 9.5f;
        if (trunk) return (y + x) % 3 ? rgb(0.45f, 0.3f, 0.17f) : rgb(0.4f, 0.26f, 0.15f);
        if (crown) return (x + z + y) % 4 ? rgb(0.25f, 0.45f, 0.18f) : rgb(0.3f, 0.52f, 0.2f);
        return 0u;
    });
    // A chalk cliff (crumbles), a shelf, a stalactite under it and a cornice along the top.
    breakable(world, m, "chalk", ivec3(16, 32, 12), v, vec3(-68.0f, 0.0f, 70.0f), IDENTITY, [](int x, int y, int z) -> uint32_t {
        bool face = x < 6, shelf = y >= 18 && y <= 21, stalactite = x >= 12 && x <= 13 && z >= 5 && z <= 6 && y >= 12 && y < 18;
        bool cornice = y >= 29 && x < 10;
        return face || shelf || stalactite || cornice ? chalk(x, y, z) : 0u;
    });
    // A shale cliff that breaks in slabs -- the pattern registered in startup -- with an overhang.
    breakable(world, m, "shale", ivec3(24, 40, 12), v, vec3(-56.0f, 0.0f, 70.0f), IDENTITY, [](int x, int y, int z) -> uint32_t {
        bool face = x < 10 || (y >= 26 && x < 22);
        return face ? shale(x, y, z) : 0u;
    });
    // An earth hill on bedrock. It does not stand on its own y = 0: what holds it up is the bedrock
    // under it, an anchor material. Dig under its overhang and that falls; dig a tunnel through and
    // what is left with no bedrock under it comes down.
    {
        auto& scene = world.ctx().get<projv::Scene>();
        const uint32_t grass = rgb(0.3f, 0.55f, 0.2f), dirt[2] = {rgb(0.45f, 0.32f, 0.2f), rgb(0.4f, 0.28f, 0.17f)};
        const uint32_t rock = rgb(0.22f, 0.22f, 0.25f);
        const vec3 corner(-82.0f, 0.0f, 40.0f);
        ComponentHandle h = voxels(scene, "earth hill", ivec3(56, 32, 40), v, corner, [&](int x, int y, int z) -> uint32_t {
            if (y < 2 && x < 40) return rock;
            float dx = (float(x) - 20.0f) / 20.0f, dz = (float(z) - 20.0f) / 20.0f;
            float height = 30.0f * (1.0f - dx * dx - dz * dz * 0.8f);
            bool hill = x < 40 && float(y) < height;
            bool overhang = x >= 30 && y >= 18 && y < 24 && z >= 14 && z < 26 && x < 54;   // a ledge out over nothing
            if (!hill && !overhang) return 0u;
            bool top = float(y + 1) >= height || (overhang && y == 23);
            return top ? grass : dirt[(x + y * 3 + z) % 2];
        });
        const auto& palette = scene.components[h].materialPalette;
        for (size_t slot = 0; slot < palette.size(); slot++)
            utils::setMaterialPhysics(scene, h, uint8_t(slot), palette[slot].packedColor == rock ? "bedrock" : "earth");
        Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        projv::StaticCollider c;
        c.material = "earth";
        c.splits = true;          // held up by the bedrock alone
        c.breaksOnImpact = true;
        world.emplace<projv::StaticCollider>(e, c);
        runtime::assignNetId(world, e);
        own(m, e);
    }
}

// =================================================================================================
// The bowling lane (north-east)
// =================================================================================================

constexpr float BOWL_X = 66.0f;

void rackPins(World& world, Mayhem& m) {
    for (Entity e : m.pins) if (world.valid(e)) world.destroy(e);
    m.pins.clear();
    if (world.valid(m.bowlingBall)) world.destroy(m.bowlingBall);
    // Ten 5 m pins that collide as capsules (fitted to their voxels), in a triangle.
    projv::RigidBody pin;
    pin.shape = projv::RigidBody::Shape::Capsule;
    pin.mass = 600.0f;
    pin.friction = 0.4f;
    for (int row = 0; row < 4; row++)
        for (int i = 0; i <= row; i++)
            m.pins.push_back(place(world, m.pin, vec3(BOWL_X + (float(i) - float(row) * 0.5f) * 2.6f, 2.51f, 74.0f + float(row) * 2.3f),
                                   IDENTITY, pin));
    // The ball: 6 m of stone, a sphere to the simulation.
    projv::RigidBody rock = ballBody(1500.0f, 0.1f);
    rock.friction = 0.3f;
    m.bowlingBall = place(world, m.bowlingBallProto, vec3(BOWL_X, 3.02f, 34.0f), IDENTITY, rock);
}

void bowl(World& world, Mayhem& m) {
    if (!world.valid(m.bowlingBall)) return;
    runtime::setVelocity(world, m.bowlingBall, vec3(0.6f, 0.0f, 16.0f), vec3(16.0f / 3.0f, 0.0f, 0.0f));
}

void buildBowling(World& world, Mayhem& m) {
    scenery(world, m, "lane", ivec3(56, 1, 220), 0.25f, vec3(BOWL_X - 7.0f, 0.0f, 30.0f), [](int x, int, int z) -> uint32_t {
        bool gutter = x < 3 || x >= 53;
        bool arrow = z > 60 && z < 80 && (x - 28) * (x - 28) < (80 - z) * (80 - z) / 9 && x % 7 == 0;
        if (gutter) return rgb(0.2f, 0.2f, 0.22f);
        return arrow ? rgb(0.9f, 0.2f, 0.15f) : ((x / 4) % 2 ? rgb(0.82f, 0.62f, 0.38f) : rgb(0.76f, 0.56f, 0.33f));
    });
    rackPins(world, m);
}

// =================================================================================================
// The world, whole
// =================================================================================================

void clearWorld(World& world) {
    auto& m = world.ctx().get<Mayhem>();
    auto& camera = world.ctx().get<Camera>();
    if (camera.walker != NullEntity && world.valid(camera.walker)) world.destroy(camera.walker);
    camera.walker = NullEntity;
    camera.driving = false;
    std::vector<Entity> doomed;
    for (Entity e : world.view<projv::RigidBody>()) doomed.push_back(e);
    for (Entity e : world.view<projv::Joint>()) doomed.push_back(e);
    for (Entity e : world.view<projv::StaticCollider>()) if (e != m.floor) doomed.push_back(e);
    for (const Rope& r : m.ropes) doomed.insert(doomed.end(), r.links.begin(), r.links.end());
    for (const auto& f : m.flashes) doomed.push_back(f.e);
    doomed.insert(doomed.end(), m.owned.begin(), m.owned.end());
    std::sort(doomed.begin(), doomed.end());
    doomed.erase(std::unique(doomed.begin(), doomed.end()), doomed.end());
    for (Entity e : doomed) if (world.valid(e)) world.destroy(e);
    m.owned.clear();
    m.cannonballs.clear(); m.spawned.clear(); m.fountain.clear(); m.bungeeDrops.clear(); m.orbs.clear();
    m.tnt.clear(); m.fuses.clear(); m.kicks.clear(); m.ropes.clear(); m.flashes.clear(); m.pads.clear();
    m.dominoes.clear(); m.pins.clear(); m.toThrow.clear(); m.slippery.clear();
    m.car = Car{};
    m.grab = Mayhem::Grab{};
    m.gate = m.gateBody = m.gatePlate = m.ferris = m.wrecker = m.wreckerLatch = m.dominoPlate = NullEntity;
    m.elevator = m.elevatorPlatform = m.bungeePlate = m.blender = m.bowlingBall = NullEntity;
    m.gateOpen = m.dominoesStarted = m.wreckerStruck = m.chipped = false;
    m.relatchIn = 0;
    m.bowlIn = -1.0f;
    m.snapshot.clear();
}

void latchWrecker(World& world, Mayhem& m) {
    projv::Joint latch;
    latch.type = runtime::JointType::Point;
    latch.anchor = m.wreckerStart;
    m.wreckerLatch = joint(world, m, m.wrecker, NullEntity, latch);
}

void buildWorld(World& world) {
    auto& m = world.ctx().get<Mayhem>();
    // MAYHEM_ONLY=castle: the castle alone (for measuring a break without the rest of the park).
    const char* only = std::getenv("MAYHEM_ONLY");
    const bool all = !only || std::string(only) != "castle";
    if (all) {
        buildPlayground(world, m);
        buildFerrisWheel(world, m);
    }
    buildCastle(world, m);
    if (all) {
        buildStacks(world, m);
        buildMachineYard(world, m);
        buildQuarry(world, m);
        buildBowling(world, m);
    }
    // The wrecking ball's rope, and its latch.
    projv::Joint rope;
    rope.type = runtime::JointType::Distance;
    rope.anchor = m.wreckerStart;
    rope.anchorB = m.wreckerPivot;
    rope.limitMin = 0.0f;
    rope.limitMax = glm::length(m.wreckerPivot - m.wreckerStart);
    Entity j = joint(world, m, m.wrecker, NullEntity, rope);
    addRope(world, m, j, m.wrecker, m.wreckerStart, NullEntity, m.wreckerPivot, 40);
    latchWrecker(world, m);
}

// =================================================================================================
// Actions
// =================================================================================================

uint32_t aimSkips(const World& world) { return runtime::physicsGroup(world, "small debris"); }

std::optional<runtime::QueryHit> aim(World& world, const Camera& camera, float distance = 200.0f) {
    return runtime::castRay(world, camera.position, camera.forward(), distance, 0xffffffffu, camera.walker, aimSkips(world));
}

// An explosion at `at`: every breakable surface within `radius` that a ray from the centre reaches is
// struck where the ray meets it, with the energy falling off with the square of the distance; two
// steps later, everything near (the shards included) is thrown outward; and TNT within reach goes off
// in turn, a moment later.
void explode(World& world, Mayhem& m, vec3 at, float energy, float radius, Entity ignore = NullEntity) {
    m.explosions++;
    std::map<Entity, runtime::QueryHit> nearest;
    for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++)
            for (int k = -1; k <= 1; k++) {
                if (i == 0 && j == 0 && k == 0) continue;
                vec3 dir = glm::normalize(vec3(i, j, k));
                auto hit = runtime::castRay(world, at, dir, radius, 0xffffffffu, ignore);
                if (!hit || hit->entity == NullEntity || hit->entity == m.floor) continue;
                auto [it, added] = nearest.try_emplace(hit->entity, *hit);
                if (!added && hit->distance < it->second.distance) it->second = *hit;
            }
    // Bodies the rays missed (inside the blast, or small) are struck at their middle.
    for (Entity e : runtime::overlapSphere(world, at, radius * 0.6f, runtime::layerBit(runtime::PhysicsLayer::Moving), ignore))
        if (!nearest.count(e) && world.all_of<projv::RigidBody>(e)) {
            runtime::QueryHit h;
            h.entity = e;
            h.position = runtime::bodyPose(world, e).centerOfMass;
            h.distance = glm::length(h.position - at);
            nearest[e] = h;
        }
    for (auto& [e, hit] : nearest) {
        if (m.tnt.count(e)) {
            m.fuses.push_back({e, 0.12f + 0.04f * float(m.fuses.size() % 4)});
            continue;
        }
        if (!runtime::canBreak(world, e)) continue;
        float falloff = std::max(0.0f, 1.0f - hit.distance / radius);
        runtime::Strike strike;
        strike.at = hit.position + glm::normalize(hit.position - at + vec3(1e-4f)) * 0.1f;
        strike.energy = energy * falloff * falloff;
        runtime::fracture(world, e, strike);
    }
    m.kicks.push_back({at, radius * 1.8f, 22.0f, 2});
    // A fireball, drawn only: grows and fades in half a second.
    m.flashes.push_back({decoration(world, m.fireball, at), 0.0f});
    // And the camera feels it.
    addShake(world, at, std::min(1.0f, energy / 250000.0f));
}

void kickTick(World& world, Mayhem& m) {
    for (Kick& k : m.kicks) {
        if (--k.steps != 0) continue;
        for (Entity e : runtime::overlapSphere(world, k.at, k.radius, runtime::layerBit(runtime::PhysicsLayer::Moving))) {
            if (!world.all_of<projv::RigidBody>(e) || world.get<projv::RigidBody>(e).motion != runtime::MotionType::Dynamic) continue;
            vec3 d = runtime::bodyPose(world, e).centerOfMass - k.at;
            float distance = glm::length(d);
            if (distance < 1e-3f) continue;
            float falloff = 1.0f - std::min(distance / k.radius, 1.0f);
            runtime::addVelocity(world, e, (d / distance * k.speed + vec3(0, 6, 0)) * falloff);
        }
    }
    std::erase_if(m.kicks, [](const Kick& k) { return k.steps <= 0; });
}

void detonate(World& world, Mayhem& m, Entity tnt) {
    if (!m.tnt.erase(tnt) || !world.valid(tnt)) return;
    vec3 at = runtime::hasBody(world, tnt) ? runtime::bodyPose(world, tnt).centerOfMass : world.get<projv::Transform>(tnt).position;
    explode(world, m, at, 250000.0f, 6.0f, tnt);
    world.destroy(tnt);
}

void fuseTick(World& world, Mayhem& m, float dt) {
    std::vector<Entity> due;
    for (Fuse& f : m.fuses)
        if ((f.seconds -= dt) <= 0.0f) due.push_back(f.tnt);
    std::erase_if(m.fuses, [](const Fuse& f) { return f.seconds <= 0.0f; });
    for (Entity e : due) detonate(world, m, e);
}

// The pickaxe: the engine's fracture just inside what the crosshair hits, breaking finer than the
// material would on its own; the shards, when they come back (BodyBroken), are thrown out of the face.
// Cut from one solid they sit flush and friction holds each in its socket, so for half a second they
// are frictionless: every palette entry's material swapped for its frictionless twin.
constexpr int SLIPPERY_STEPS = 30;
constexpr const char* THROWN = " (thrown)";
const char* const BREAKING_MATERIALS[] = {"ashlar", "brick", "planks", "fired brick", "sandstone", "timber", "chalk", "tower brick",
                                          "plaster", "concrete", "rebar", "wall brick", "mortar", "pine", "glass", "shale", "earth"};

void defineThrownTwins(World& world) {
    for (const char* name : BREAKING_MATERIALS)
        if (std::optional<runtime::PhysicsMaterial> mat = runtime::physicsMaterial(world, name)) {
            mat->friction = 0.0f;
            runtime::definePhysicsMaterial(world, std::string(name) + THROWN, *mat);
        }
}

void chip(World& world, Mayhem& m, const Camera& camera) {
    auto hit = aim(world, camera, 80.0f);
    if (!hit || hit->entity == NullEntity || hit->entity == m.floor) return;
    runtime::Strike strike;
    strike.at = hit->position + camera.forward() * 0.1f;
    strike.energy = 90000.0f;
    strike.shardSize = 0.35f;
    runtime::fracture(world, hit->entity, strike);
    m.chipped = true;
    m.chipTarget = hit->entity;
    m.chipAt = strike.at;
    m.chipDirection = camera.forward();
}

void slipChip(World& world, Mayhem& m, const projv::BodyBroken& e) {
    auto& scene = world.ctx().get<projv::Scene>();
    for (size_t i = 0; i < e.shards; i++) {
        Entity shard = e.pieces[i];
        const projv::RigidBody& rigid = world.get<projv::RigidBody>(shard);
        Slippery slip{shard, rigid.friction, rigid.material, {}, SLIPPERY_STEPS};
        const ComponentHandle h = world.get<projv::VoxelComponent>(shard).handle;
        const auto& palette = scene.components[h].materialPalette;
        for (size_t slot = 0; slot < palette.size(); slot++) {
            const std::string name = palette[slot].physics;
            if (name.empty() || !runtime::physicsMaterial(world, name + THROWN)) continue;
            slip.entries.push_back({uint8_t(slot), name});
            utils::setMaterialPhysics(scene, h, uint8_t(slot), name + THROWN);
        }
        std::string twin = rigid.material.empty() ? "" : rigid.material + THROWN;
        if (!twin.empty() && !runtime::physicsMaterial(world, twin)) twin.clear();
        m.slippery.push_back(std::move(slip));
        world.patch<projv::RigidBody>(shard, [&twin](projv::RigidBody& r) {
            r.friction = 0.0f;
            r.material = twin;
        });
        m.toThrow.push_back(shard);
    }
}

void throwChip(World& world, Mayhem& m) {
    const vec3 ahead = glm::normalize(m.chipDirection);
    for (Entity shard : m.toThrow) {
        if (!world.valid(shard) || !runtime::hasBody(world, shard)) continue;
        vec3 from = runtime::bodyPose(world, shard).centerOfMass - m.chipAt;
        vec3 across = from - ahead * glm::dot(from, ahead);
        float off = glm::length(across);
        vec3 spread = off > 1e-3f ? across / off : vec3(0.0f);
        runtime::setVelocity(world, shard, glm::normalize(ahead + 0.4f * spread) * 6.0f, glm::cross(ahead, spread) * 4.0f);
    }
    m.toThrow.clear();
}

void restoreChips(World& world, Mayhem& m) {
    auto& scene = world.ctx().get<projv::Scene>();
    for (Slippery& s : m.slippery) {
        if (--s.steps != 0 || !world.valid(s.piece) || !world.all_of<projv::RigidBody>(s.piece)) continue;
        if (const auto* link = world.try_get<projv::VoxelComponent>(s.piece); link && utils::isComponentAlive(scene, link->ref()))
            for (const auto& [slot, name] : s.entries) utils::setMaterialPhysics(scene, link->handle, slot, name);
        world.patch<projv::RigidBody>(s.piece, [&s](projv::RigidBody& r) {
            r.friction = s.friction;
            r.material = s.material;
        });
    }
    std::erase_if(m.slippery, [](const Slippery& s) { return s.steps <= 0; });
}

// The dig: removes every voxel within 1.5 m of where the crosshair hits, from whatever it hits -- a
// voxel edit, at the next step. What it cuts loose falls (a static collider that splits) or comes
// away (a moving body keeps its biggest part).
void dig(World& world, const Camera& camera) {
    auto hit = aim(world, camera, 80.0f);
    if (!hit || hit->entity == NullEntity) return;
    std::vector<projv::PendingVoxelOp> ops = runtime::voxelsInSphere(world, hit->entity, hit->position, 1.5f);
    if (!ops.empty()) runtime::editVoxels(world, hit->entity, ops);
}

void spawn(World& world, Mayhem& m, const Camera& camera) {
    const vec3 at = camera.position + camera.forward() * 4.0f, velocity = camera.forward() * 6.0f;
    const quat turn = glm::angleAxis(uniform(m, 0, 2 * PI), vec3(0, 1, 0));
    Entity e = NullEntity;
    switch (m.spawnKind) {
        case SpawnKind::Crate: e = place(world, m.crate, at, turn, body(30.0f)); break;
        case SpawnKind::Barrel: {
            projv::RigidBody r = body(60.0f);
            r.shape = projv::RigidBody::Shape::Capsule;   // fitted upright through its voxels
            e = place(world, m.barrel, at, turn, r);
            break;
        }
        case SpawnKind::Ball: e = place(world, m.balls[4], at, IDENTITY, ballBody(300.0f, 0.9f, "bouncy")); break;
        case SpawnKind::Glass: {
            projv::RigidBody r = body(40.0f);
            r.material = "glass";
            r.breaksOnImpact = true;
            r.minImpactSpeed = 2.0f;   // fragile
            e = place(world, m.glassBox, at, turn, r);
            break;
        }
        case SpawnKind::Balloon: {
            projv::RigidBody r = ballBody(40.0f, 0.5f, "balloon");
            r.gravityFactor = -0.6f;
            r.linearDamping = 0.8f;
            e = place(world, m.balloon, at, IDENTITY, r);
            break;
        }
        default: break;
    }
    if (e == NullEntity) return;
    runtime::setVelocity(world, e, velocity);
    keep(world, m.spawned, e, 150);
}

void use(World& world, Mayhem& m, const Camera& camera) {
    const vec3 forward = camera.forward();
    switch (m.tool) {
        case Tool::Cannon: {
            projv::RigidBody r = ballBody(7800.0f, 0.1f, "iron");
            r.quality = projv::RigidBody::Quality::Continuous;
            Entity e = place(world, m.cannonball, camera.position + forward * 2.0f, IDENTITY, r);
            runtime::setVelocity(world, e, forward * 50.0f);
            keep(world, m.cannonballs, e, 40);
            break;
        }
        case Tool::Blast:
            if (auto hit = aim(world, camera)) explode(world, m, hit->position - forward * 0.3f, 150000.0f, 4.5f);
            break;
        case Tool::Pickaxe: chip(world, m, camera); break;
        case Tool::Dig: dig(world, camera); break;
        case Tool::Spawn: spawn(world, m, camera); break;
        case Tool::Tnt: {
            Entity e = place(world, m.tntCrate, camera.position + forward * 2.5f, IDENTITY, tntBody());
            runtime::setVelocity(world, e, forward * 14.0f + vec3(0, 3, 0));
            m.tnt.insert(e);
            break;
        }
        case Tool::Orb: {
            projv::RigidBody rigid;
            rigid.motion = runtime::MotionType::Kinematic;
            rigid.shape = projv::RigidBody::Shape::Sphere;
            rigid.blockedByStatic = runtime::KinematicBlocking::Slide;   // rolls on along the floor
            Entity e = place(world, m.orb, camera.position + forward * 7.0f, IDENTITY, rigid);
            vec3 axis = glm::cross(forward, vec3(0, 1, 0));
            vec3 spin = glm::length(axis) > 1e-3f ? -glm::normalize(axis) * 3.0f : vec3(0);
            world.emplace<projv::ConstantVelocity>(e, projv::ConstantVelocity{forward * 25.0f, spin});
            m.orbs.push_back({e, 8.0f});
            break;
        }
        case Tool::Push: {
            // A shove: everything moving within 14 m, in a cone ahead.
            for (Entity e : runtime::overlapSphere(world, camera.position + forward * 7.0f, 8.0f,
                                                  runtime::layerBit(runtime::PhysicsLayer::Moving), camera.walker)) {
                if (!world.all_of<projv::RigidBody>(e) || world.get<projv::RigidBody>(e).motion != runtime::MotionType::Dynamic) continue;
                vec3 d = runtime::bodyPose(world, e).centerOfMass - camera.position;
                float distance = glm::length(d);
                if (distance < 1e-3f || glm::dot(d / distance, forward) < 0.5f) continue;
                runtime::addVelocity(world, e, forward * 18.0f * (1.0f - distance / 16.0f) + vec3(0, 3, 0));
            }
            break;
        }
    }
}

// =================================================================================================
// Systems
// =================================================================================================

// What the self-test watches over the whole run.
struct Watch {
    float elevatorHighest = 0.0f, gateWidest = 0.0f, ferrisSpeed = 0.0f;
    int balloonsAloft = 0;
};

// Right-click's hold: an impulse at the grabbed point toward its spot in front of the camera, capped,
// so joints and contacts still win.
constexpr float GRAB_FORCE = 30000.0f;
void dragTick(World& world, Mayhem& m, float dt) {
    Mayhem::Grab& grab = m.grab;
    if (grab.body == NullEntity) return;
    if (!world.valid(grab.body) || !runtime::hasBody(world, grab.body)) { grab.body = NullEntity; return; }
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

void setGate(World& world, Mayhem& m, bool open) {
    if (open == m.gateOpen || !world.valid(m.gate)) return;
    m.gateOpen = open;
    runtime::setJointMotor(world, m.gate, true, open ? 1.5f : 0.0f, runtime::MotorMode::Position);
}

void releaseWrecker(World& world, Mayhem& m) {
    if (world.valid(m.wreckerLatch)) {
        world.destroy(m.wreckerLatch);   // let go: it swings into the wall
        m.wreckerLatch = NullEntity;
        m.wreckerStruck = false;
        projv::core::info("mayhem: wrecking ball away");
    } else if (world.valid(m.wrecker) && m.relatchIn == 0) {
        runtime::teleport(world, m.wrecker, m.wreckerStart, IDENTITY);   // wound back
        runtime::setVelocity(world, m.wrecker, vec3(0.0f));
        m.relatchIn = 3;   // the latch is made once the teleport has happened
    }
}

void startDominoes(World& world, Mayhem& m) {
    if (m.dominoesStarted || m.dominoes.empty() || !world.valid(m.dominoes.front())) return;
    m.dominoesStarted = true;
    // Tipped forward along the path: turned about up x the path's direction.
    runtime::addAngularImpulse(world, m.dominoes.front(), glm::cross(vec3(0, 1, 0), m.dominoPush) * 30.0f);
    projv::core::info("mayhem: the dominoes are off");
}

// FixedUpdate: everything that happens over time, in fixed steps, so it plays the same at any frame rate.
void gameTick(projv::Application& app) {
    World& world = app.world;
    auto& m = world.ctx().get<Mayhem>();
    const float dt = app.time().fixedDelta;
    m.clock += dt;
    dragTick(world, m, dt);
    kickTick(world, m);
    fuseTick(world, m, dt);
    if (!m.toThrow.empty()) throwChip(world, m);
    if (!m.slippery.empty()) restoreChips(world, m);
    for (auto& [e, steps] : m.newborn)
        if (--steps == 0 && world.valid(e) && world.all_of<projv::CollisionGroups>(e))
            world.patch<projv::CollisionGroups>(e, [](projv::CollisionGroups& g) { g.family = 0; });
    std::erase_if(m.newborn, [](const auto& n) { return n.second <= 0; });
    if (m.relatchIn > 0 && --m.relatchIn == 0) latchWrecker(world, m);

    // The gate opens while anything stands on its plate (a sensor: what is touching it now).
    if (world.valid(m.gatePlate)) setGate(world, m, !runtime::touching(world, m.gatePlate).empty());

    // The elevator: up, then down, every seven seconds.
    if (world.valid(m.elevator) && (m.elevatorTimer -= dt) <= 0.0f) {
        m.elevatorTimer += 7.0f;
        m.elevatorUp = !m.elevatorUp;
        runtime::setJointMotor(world, m.elevator, true, m.elevatorUp ? 8.5f : 0.0f, runtime::MotorMode::Position);
    }
    // The fountain: a ball dropped over a jump pad every 2.5 s.
    if (!m.pads.empty() && (m.fountainTimer -= dt) <= 0.0f) {
        m.fountainTimer += 2.5f;
        auto pad = std::next(m.pads.begin(), long(m.random() % m.pads.size()));
        vec3 above = world.get<projv::Transform>(pad->first).position + vec3(1.5f, 6.0f, 1.5f);
        keep(world, m.fountain, place(world, m.balls[2 + int(m.random() % 3)], above, IDENTITY, ballBody(300.0f, 0.6f, "bouncy")), 30);
    }
    // A ball onto the bungee every 4 s.
    if (world.valid(m.bungeePlate) && (m.bungeeTimer -= dt) <= 0.0f) {
        m.bungeeTimer += 4.0f;
        vec3 above = vec3(-60.0f, 10.0f, 40.0f) + vec3(uniform(m, -0.8f, 0.8f), 0.0f, uniform(m, -0.8f, 0.8f));
        keep(world, m.bungeeDrops, place(world, m.balls[4], above, IDENTITY, ballBody(400.0f, 0.5f)), 8);
    }
    // The slides start again every 10 s.
    if (world.valid(m.slides[0].crate) && (m.slideTimer += dt) > 10.0f) {
        m.slideTimer = 0.0f;
        placeSlideCrates(world, m);
    }
    // Orbs fly for eight seconds.
    for (auto& [e, left] : m.orbs)
        if ((left -= dt) <= 0.0f && world.valid(e)) world.destroy(e);
    std::erase_if(m.orbs, [](const auto& o) { return o.second <= 0.0f; });
    if (m.bowlIn >= 0.0f && (m.bowlIn -= dt) < 0.0f) bowl(world, m);

    // For the self-test.
    if (Watch* w = world.ctx().find<Watch>()) {
        if (world.valid(m.elevatorPlatform)) w->elevatorHighest = std::max(w->elevatorHighest, runtime::bodyPose(world, m.elevatorPlatform).position.y);
        if (world.valid(m.gate)) w->gateWidest = std::max(w->gateWidest, runtime::jointState(world, m.gate).position);
        if (world.valid(m.ferris)) w->ferrisSpeed = runtime::bodyPose(world, m.ferris).angularVelocity.z;
    }
}

// FixedUpdate, after "physics": the step's impacts. TNT hit hard goes off; the hardest hit of the step,
// for where it is, shakes the camera by its energy on a log scale.
void impactTick(projv::Application& app) {
    World& world = app.world;
    auto& m = world.ctx().get<Mayhem>();
    float hardest = 0.0f;
    vec3 where{0.0f};
    for (const runtime::BodyImpact& hit : runtime::impacts(world)) {
        for (Entity e : {hit.a, hit.b})
            if (m.tnt.count(e) && hit.speed > 6.0f &&
                std::none_of(m.fuses.begin(), m.fuses.end(), [e](const Fuse& f) { return f.tnt == e; }))
                m.fuses.push_back({e, 0.0f});
        float strength = 0.7f * std::clamp(std::log10(hit.energy / SHAKE_FULL) + 1.0f, 0.0f, 1.0f);
        if (strength > hardest) { hardest = strength; where = hit.point; }
    }
    if (hardest > 0.0f) addShake(world, where, hardest);
}

// A jump pad throws what comes onto it up and on; the domino plate sets the dominoes off.
void onContact(World& world, const projv::ContactBegan& c) {
    auto& m = world.ctx().get<Mayhem>();
    auto& camera = world.ctx().get<Camera>();
    for (auto [sensor, other] : {std::pair{c.a, c.b}, std::pair{c.b, c.a}}) {
        if (!world.valid(other)) continue;
        if (sensor == m.dominoPlate) startDominoes(world, m);
        auto pad = m.pads.find(sensor);
        if (pad == m.pads.end()) continue;
        if (other == camera.walker) {
            camera.padJump = true;
            m.padLaunches++;
        } else if (world.all_of<projv::RigidBody>(other) && world.get<projv::RigidBody>(other).motion == runtime::MotionType::Dynamic &&
                   runtime::hasBody(world, other)) {
            runtime::BodyState s = runtime::bodyPose(world, other);
            runtime::setVelocity(world, other, vec3(s.linearVelocity.x * 0.3f, 0.0f, s.linearVelocity.z * 0.3f) + pad->second,
                                 s.angularVelocity);
            m.padLaunches++;
        }
    }
}

// A piece a break made of three voxels or fewer is "small debris": walked through, aimed through.
void markDebris(World& world, Entity piece) {
    const projv::Scene* scene = world.ctx().find<projv::Scene>();
    const projv::VoxelComponent* link = world.try_get<projv::VoxelComponent>(piece);
    if (!scene || !link) return;
    const uint64_t voxels = utils::getComponentVoxelCount(*scene, link->handle);
    const bool small = voxels <= SMALL_DEBRIS_VOXELS;
    const bool light = world.ctx().get<Mayhem>().map && voxels <= MAP_DEBRIS_VOXELS;
    projv::CollisionGroups groups = world.all_of<projv::CollisionGroups>(piece) ? world.get<projv::CollisionGroups>(piece)
                                                                               : projv::CollisionGroups{};
    groups.groups.clear();
    groups.ignores.clear();
    if (small || light) groups.groups.push_back("small debris");
    if (light) groups.ignores.push_back("small debris");
    world.emplace_or_replace<projv::CollisionGroups>(piece, std::move(groups));
    if (light && world.all_of<projv::RigidBody>(piece))
        world.patch<projv::RigidBody>(piece, [](projv::RigidBody& body) {
            body.shape = projv::RigidBody::Shape::Box;   // fitted to its voxels' content box
            body.halfExtents = vec3(0.0f);
            body.quality = projv::RigidBody::Quality::Discrete;
        });
}

void onBrokenBody(World& world, const projv::BodyBroken& e) {
    auto& m = world.ctx().get<Mayhem>();
    m.pieces += int(e.pieces.size());
    // A strike's shards are born flush against each other; one family for their first quarter second,
    // so the solver does not spend the break's first steps on every shard against its neighbours
    // (measured: with SHARD_FAMILY_STEPS and minImpactBreak 2, a cannonball through the castle wall
    // costs 40% less physics in its first half second).
    if (SHARD_FAMILY_STEPS > 0 && e.shards > 1) {
        static uint32_t nextFamily = 1000;
        const uint32_t family = nextFamily++;
        for (size_t i = 0; i < e.shards; i++)
            if (world.valid(e.pieces[i])) {
                projv::CollisionGroups g = world.all_of<projv::CollisionGroups>(e.pieces[i]) ? world.get<projv::CollisionGroups>(e.pieces[i]) : projv::CollisionGroups{};
                g.family = family;
                world.emplace_or_replace<projv::CollisionGroups>(e.pieces[i], g);
                m.newborn.push_back({e.pieces[i], SHARD_FAMILY_STEPS});
            }
    }
    for (Entity piece : e.pieces)
        if (world.valid(piece)) markDebris(world, piece);
    if (world.valid(e.entity) && world.all_of<projv::Piece>(e.entity)) markDebris(world, e.entity);
    // A thrown chip that breaks again while frictionless passes that on to its pieces.
    for (size_t k = 0, n = m.slippery.size(); k < n; k++)
        if (m.slippery[k].piece == e.entity)
            for (Entity piece : e.pieces) {
                Slippery copy = m.slippery[k];
                copy.piece = piece;
                copy.material = world.get<projv::RigidBody>(piece).material.empty() ? "" : copy.material;
                m.slippery.push_back(std::move(copy));
            }
    if (m.chipped && e.cause == projv::BreakCause::Strike && e.entity == m.chipTarget) {
        m.chipped = false;
        slipChip(world, m, e);
    }
    if (e.striker != NullEntity && e.striker == m.wrecker && !m.wreckerStruck) {
        m.wreckerStruck = true;
        projv::core::info("mayhem: the wrecking ball broke the wall: {} pieces, {:.0f} kJ", e.pieces.size(), e.energy / 1000.0f);
    }
}

// Update: the Blender's beam is placed by its Transform, which physics follows.
void moveBlender(projv::Application& app) {
    World& world = app.world;
    auto& m = world.ctx().get<Mayhem>();
    if (!world.valid(m.blender)) return;
    const float t = m.clock;
    world.patch<projv::Transform>(m.blender, [&](projv::Transform& tr) {
        tr.position = vec3(-60.0f, 0.6f, -28.0f) + vec3(std::cos(t * 0.35f) * 3.0f, 0.0f, std::sin(t * 0.35f) * 3.0f);
        tr.rotation = glm::angleAxis(t * 1.1f, vec3(0, 1, 0));
    });
}

// PostUpdate, after physics has written this frame's poses and before the Scene bridge reads them:
// the ropes strung between their ends, and the fireballs grown.
void presentDecorations(projv::Application& app) {
    World& world = app.world;
    auto& m = world.ctx().get<Mayhem>();
    for (Rope& r : m.ropes) {
        bool holds = world.valid(r.joint) && world.valid(r.a) && (r.b == NullEntity || world.valid(r.b)) &&
                     !(world.all_of<projv::PhysicsJoint>(r.joint) && world.get<projv::PhysicsJoint>(r.joint).broken);
        if (!holds) {
            for (Entity l : r.links) if (world.valid(l)) world.destroy(l);
            r.links.clear();
            continue;
        }
        const projv::Transform& ta = world.get<projv::Transform>(r.a);
        vec3 from = ta.position + ta.rotation * r.localA, to = r.localB;
        if (r.b != NullEntity) {
            const projv::Transform& tb = world.get<projv::Transform>(r.b);
            to = tb.position + tb.rotation * r.localB;
        }
        vec3 along = to - from;
        float length = glm::length(along);
        quat turn = length > 1e-3f ? quat(vec3(0, 1, 0), along / length) : IDENTITY;
        for (size_t k = 0; k < r.links.size(); k++) {
            float f = (float(k) + 0.5f) / float(r.links.size());
            world.emplace_or_replace<projv::Transform>(r.links[k], projv::Transform{from + along * f, turn});
        }
    }
    std::erase_if(m.ropes, [](const Rope& r) { return r.links.empty(); });
    for (auto& f : m.flashes) {
        f.age += app.time().delta;
        if (!world.valid(f.e)) continue;
        if (f.age > 0.5f) { world.destroy(f.e); continue; }
        world.patch<projv::Transform>(f.e, [&](projv::Transform& t) { t.scale = 0.6f + f.age * 9.0f; });
    }
    std::erase_if(m.flashes, [&](const auto& f) { return f.age > 0.5f || !world.valid(f.e); });
}

// PostUpdate: the camera is the walker's eyes, or chases the car.
void followCamera(projv::Application& app) {
    World& world = app.world;
    auto& camera = world.ctx().get<Camera>();
    auto& m = world.ctx().get<Mayhem>();
    if (camera.walker != NullEntity && world.valid(camera.walker)) {
        camera.position = world.get<projv::Transform>(camera.walker).position + vec3(0, EYE_HEIGHT, 0);
        return;
    }
    if (!camera.driving || !world.valid(m.car.chassis)) return;
    const projv::Transform& t = world.get<projv::Transform>(m.car.chassis);
    vec3 heading = t.rotation * vec3(0, 0, 1);
    heading.y = 0.0f;
    heading = glm::length(heading) > 1e-3f ? glm::normalize(heading) : vec3(0, 0, 1);
    vec3 wanted = t.position - heading * 9.0f + vec3(0, 3.5f, 0);
    camera.position = glm::mix(camera.position, wanted, 1.0f - std::exp(-6.0f * app.time().unscaledDelta));
    vec3 look = glm::normalize(t.position + vec3(0, 1, 0) - camera.position);
    camera.yaw = std::atan2(look.z, look.x);
    camera.pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
}

// =================================================================================================
// The renderer (examples/shared/voxelRenderer/, staged as voxelRenderer/): a G-buffer, AO, the shade, the
// voxel reconstruction, TAA and the display (docs/plans/voxel-renderer.md). Its uniforms are set here,
// every frame.
// =================================================================================================

// The renderer and its settings are shared with other examples: examples/shared/voxel_renderer.h.
using View = voxel_renderer::View;

// =================================================================================================
// Controls
// =================================================================================================

void printHelp() {
    std::printf(
        "\n"
        "  Voxel Mayhem -- every ProjectV physics feature in one park\n"
        "  -------------------------------------------------------------------------------\n"
        "  Mouse look, W A S D fly, Space/Shift up/down      Esc frees the cursor, a click takes it back\n"
        "  Tools (1-8 or the wheel), used with a click:\n"
        "    1 cannon  2 blast  3 pickaxe  4 dig  5 spawn (Q: what)  6 TNT  7 kinematic orb  8 push\n"
        "  Right-drag  hold and drag a body (the wheel: nearer / farther)\n"
        "  V walk / fly    E drive the car (W/S, A/D, Space brakes)   B wrecking ball   N bowl\n"
        "  G gravity: Earth / Moon / none    T slow motion   P pause   R rebuild the world\n"
        "  F5 snapshot the physics   F9 rewind to it (refused once anything has broken or been made)\n"
        "  H shadows  O ambient occlusion  I GI  K TAA\n"
        "  U debug view (AO / sun visibility / indirect light)\n"
        "  [ ] the sun   F1 this help\n"
        "  -------------------------------------------------------------------------------\n"
        "  South: ball pit (stand on the yellow plate), jump pads, balloons, slides, car and ramp\n"
        "  Centre: Ferris wheel   North: the castle, crane (B), composite wall, TNT stack\n"
        "  East: domino spiral (pink plate), pyramid, Jenga   West: bridge, chains, elevator, bungee, Blender\n"
        "  North-west: the quarry (dig the earth hill!)   North-east: bowling (N)\n\n");
    std::fflush(stdout);
}

void controls(projv::Application& app) {
    World& world = app.world;
    auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
    const auto& input = world.ctx().get<projv::Input>();
    auto& camera = world.ctx().get<Camera>();
    auto& m = world.ctx().get<Mayhem>();
    View& view = world.ctx().get<View>();
    projv::Time& time = app.time();
    if (m.unattended) return;

    const bool captured = input.cursorCaptured;   // the click that recaptures the cursor is not an action
    if (input.cursorCaptured && input.pressed(projv::Key::Escape)) projv::graphics::setCursorCaptured(app, renderInstance, false);
    else if (!input.cursorCaptured && input.pressed(projv::MouseButton::Left)) projv::graphics::setCursorCaptured(app, renderInstance, true);
    if (input.cursorCaptured && !camera.driving) {
        camera.yaw += input.cursorDelta.x * 0.0025f;
        camera.pitch = std::clamp(camera.pitch - input.cursorDelta.y * 0.0025f, -1.5f, 1.5f);
    }

    // Tools.
    for (int n = 1; n <= TOOL_COUNT; n++)
        if (input.pressed(projv::Key(int(projv::Key::Num0) + n))) m.tool = Tool(n);
    if (m.grab.body != NullEntity) m.grab.distance = std::clamp(m.grab.distance + input.scroll.y, 1.5f, 60.0f);
    else if (input.scroll.y != 0.0f) m.tool = Tool((int(m.tool) - 1 + (input.scroll.y < 0 ? 1 : TOOL_COUNT - 1)) % TOOL_COUNT + 1);
    if (input.pressed(projv::Key::Q)) {
        m.spawnKind = SpawnKind((int(m.spawnKind) + 1) % int(SpawnKind::Count));
        m.tool = Tool::Spawn;
    }
    if (captured && input.pressed(projv::MouseButton::Left) && !camera.driving) use(world, m, camera);
    if (captured && input.pressed(projv::MouseButton::Right)) {
        auto hit = runtime::castRay(world, camera.position, camera.forward(), 80.0f, 0xffffffffu, camera.walker);
        if (hit && world.valid(hit->entity) && world.all_of<projv::RigidBody>(hit->entity) &&
            world.get<projv::RigidBody>(hit->entity).motion == runtime::MotionType::Dynamic) {
            runtime::BodyState pose = runtime::bodyPose(world, hit->entity);
            m.grab = {hit->entity, glm::inverse(pose.rotation) * (hit->position - pose.position), hit->distance};
        }
    }
    if (input.released(projv::MouseButton::Right)) m.grab.body = NullEntity;

    // The set pieces.
    if (input.pressed(projv::Key::B)) releaseWrecker(world, m);
    if (input.pressed(projv::Key::N)) {
        rackPins(world, m);
        m.bowlIn = 1.0f;
    }
    if (input.pressed(projv::Key::R) && m.map) projv::core::info("mayhem: a map is not rebuilt -- start again to restore it");
    else if (input.pressed(projv::Key::R)) {
        clearWorld(world);
        buildWorld(world);
        projv::core::info("mayhem: the world, rebuilt");
    }
    if (input.pressed(projv::Key::G)) {
        static const vec3 gravities[3] = {{0, -9.81f, 0}, {0, -1.62f, 0}, {0, 0, 0}};
        static const char* names[3] = {"Earth", "the Moon", "none"};
        m.gravityMode = (m.gravityMode + 1) % 3;
        runtime::setGravity(world, gravities[m.gravityMode]);
        projv::core::info("mayhem: gravity: {}", names[m.gravityMode]);
    }
    if (input.pressed(projv::Key::F5)) {
        m.snapshot = runtime::savePhysics(world);
        projv::core::info("mayhem: physics snapshot taken ({} KB)", m.snapshot.size() / 1024);
    }
    if (input.pressed(projv::Key::F9) && !m.snapshot.empty())
        projv::core::info("mayhem: {}", runtime::restorePhysics(world, m.snapshot)
                                            ? "rewound to the snapshot"
                                            : "can't rewind: bodies were made or broken since the snapshot (no world rollback, by design)");
    if (input.pressed(projv::Key::T)) time.scale = time.scale == 1.0f ? 0.2f : 1.0f;
    if (input.pressed(projv::Key::P)) time.scale = time.scale == 0.0f ? 1.0f : 0.0f;
    if (input.pressed(projv::Key::H)) view.shadows = !view.shadows;
    if (input.pressed(projv::Key::O)) view.ambientOcclusion = !view.ambientOcclusion;
    if (input.pressed(projv::Key::K)) view.taa = !view.taa;
    if (input.pressed(projv::Key::I)) {
        view.gi = !view.gi;
        projv::core::info("mayhem: GI {}", view.gi ? "on" : "off (the sky ambient alone)");
    }
    if (input.pressed(projv::Key::U)) {
        view.debugView = view.debugView == 0 ? 2 : view.debugView == 2 ? 3 : view.debugView == 3 ? 7 : 0;
        static const char* names[8] = {"off", "", "AO only", "sun visibility", "", "", "", "the indirect light only"};
        projv::core::info("mayhem: debug view {}", names[view.debugView]);
    }
    if (input.down(projv::Key::LeftBracket)) view.sun -= 0.4f * time.unscaledDelta;
    if (input.down(projv::Key::RightBracket)) view.sun += 0.4f * time.unscaledDelta;
    if (input.pressed(projv::Key::F1)) printHelp();

    // Getting about: fly, walk (V) or drive (E, near the car).
    if (input.pressed(projv::Key::E)) {
        if (camera.driving) {
            camera.driving = false;
            driveCar(world, m, 0.0f, 0.0f, true);
        } else if (world.valid(m.car.chassis) &&
                   glm::length(runtime::bodyPose(world, m.car.chassis).position - camera.position) < 14.0f) {
            if (camera.walker != NullEntity) { world.destroy(camera.walker); camera.walker = NullEntity; }
            camera.driving = true;
        } else {
            projv::core::info("mayhem: the car is at ({:.0f}, {:.0f}); get within 14 m of it to drive",
                              runtime::bodyPose(world, m.car.chassis).position.x, runtime::bodyPose(world, m.car.chassis).position.z);
        }
    }
    const vec3 flat{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)}, right{-std::sin(camera.yaw), 0.0f, std::cos(camera.yaw)};
    if (input.pressed(projv::Key::V) && !camera.driving) {
        if (camera.walker == NullEntity) {
            camera.walker = world.create();
            world.emplace<projv::Transform>(camera.walker, projv::Transform{camera.position - vec3(0, EYE_HEIGHT, 0)});
            projv::CharacterController character;
            character.strength = 800.0f;
            world.emplace<projv::CharacterController>(camera.walker, character);
            world.emplace<projv::CollisionGroups>(camera.walker, projv::CollisionGroups{{"player"}, {"small debris"}, 0});
            camera.velocity.y = 0.0f;
        } else {
            world.destroy(camera.walker);
            camera.walker = NullEntity;
        }
    }
    if (camera.driving) {
        float throttle = (input.down(projv::Key::W) ? 1.0f : 0.0f) - (input.down(projv::Key::S) ? 0.6f : 0.0f);
        float steer = (input.down(projv::Key::D) ? 1.0f : 0.0f) - (input.down(projv::Key::A) ? 1.0f : 0.0f);
        driveCar(world, m, throttle, steer, input.down(projv::Key::Space));
    } else if (camera.walker != NullEntity) {
        vec3 walk(0.0f);
        if (input.down(projv::Key::W)) walk += flat;
        if (input.down(projv::Key::S)) walk -= flat;
        if (input.down(projv::Key::D)) walk += right;
        if (input.down(projv::Key::A)) walk -= right;
        if (glm::length(walk) > 0.0f) walk = glm::normalize(walk) * WALK_SPEED;
        bool grounded = runtime::characterState(world, camera.walker).onGround;
        camera.velocity += (walk - camera.velocity) * (1.0f - std::exp(-(grounded ? WALK_GRIP : AIR_GRIP) * time.unscaledDelta));
        const bool pad = camera.padJump;
        camera.padJump = false;
        runtime::setCharacterInput(world, camera.walker, camera.velocity, pad || input.pressed(projv::Key::Space), pad ? 20.0f : 6.0f);
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

void titleBar(projv::Application& app) {
    auto& m = app.world.ctx().get<Mayhem>();
    if ((m.titleTimer -= app.time().unscaledDelta) > 0.0f) return;
    m.titleTimer = 0.25f;
    const auto& stats = runtime::physicsWorld(app.world).stats();
    const auto& camera = app.world.ctx().get<Camera>();
    static const char* gravity[3] = {"Earth", "Moon", "zero-g"};
    std::string tool = m.tool == Tool::Spawn ? fmt::format("spawn: {}", spawnName(m.spawnKind)) : toolName(m.tool);
    std::string title = fmt::format("Voxel Mayhem  |  [{}] {}  |  {}  |  {} bodies ({} awake), {} pieces, {} booms  |  physics {:.1f} ms  |  {}{}  |  {} fps",
                                    int(m.tool), tool, camera.driving ? "driving" : camera.walker != NullEntity ? "walking" : "flying",
                                    stats.bodies, stats.awakeBodies, m.pieces, m.explosions, stats.lastStepMilliseconds,
                                    gravity[m.gravityMode],
                                    app.time().scale == 0.0f ? ", PAUSED" : app.time().scale < 1.0f ? ", slow motion" : "",
                                    int(1.0f / std::max(app.time().unscaledDelta, 1e-4f)));
    glfwSetWindowTitle(app.world.ctx().get<projv::graphics::RenderInstance>().window, title.c_str());
}


// =================================================================================================
// Bench: MAYHEM_BENCH=cannon (or blast) fires at the castle's front wall after 3 s and logs where each
// frame's time went until 4 s after -- the physics system (the fracture runs inside it), the break
// handler, the upload of new geometry and the render
// =================================================================================================

using BenchClock = std::chrono::steady_clock;
double msSince(BenchClock::time_point t) { return std::chrono::duration<double, std::milli>(BenchClock::now() - t).count(); }

struct Bench {
    std::string what;
    BenchClock::time_point frameStart = BenchClock::now(), stepStart;
    double physicsMs = 0.0, handlerMs = 0.0, flushMs = 0.0, renderMs = 0.0;
    int steps = 0;
    bool fired = false;
    struct Row { float t; double frame, physics, handler, flush, render; int steps; uint32_t bodies; int pieces; };
    std::vector<Row> before, after;
    // The barrage: shots fired on physics ticks, from the step hook, so every run breaks the same things
    // (the simulation is deterministic) and runs compare -- and what its steps cost from the first shot.
    uint64_t nextShot = 0;
    std::mt19937 aim{12345};   // its own: Mayhem's is drawn on by effects, frame by frame
    double   barrageMs = 0.0;
    int      barrageSteps = 0;
};

constexpr uint64_t BARRAGE_FIRST = 180, BARRAGE_LAST = 900, BARRAGE_END = 960, BARRAGE_EVERY = 24;   // ticks: 3 s, 15 s, 16 s, 0.4 s

// The break handler, timed.
void onBroken(World& world, const projv::BodyBroken& e) {
    const auto start = BenchClock::now();
    onBrokenBody(world, e);
    if (Bench* b = world.ctx().find<Bench>()) b->handlerMs += msSince(start);
}

// A barrage shot, when its tick comes: a cannonball every 0.4 s at a different place on the castle's walls
// (a map: blasts round MAYHEM_BENCH_SHOT's target), for 12 s.
void barrageShot(World& world, Bench& b) {
    const uint64_t tick = runtime::physicsWorld(world).tick();
    if (b.nextShot == 0) b.nextShot = BARRAGE_FIRST;
    if (tick < b.nextShot || tick > BARRAGE_LAST) return;
    b.nextShot += BARRAGE_EVERY;
    auto& m = world.ctx().get<Mayhem>();
    auto& camera = world.ctx().get<Camera>();
    auto pick = [&](float low, float high) { return std::uniform_real_distribution<float>(low, high)(b.aim); };
    if (m.map) {
        vec3 from(0.0f, 30.0f, 0.0f), at(0.0f);
        if (const char* shot = std::getenv("MAYHEM_BENCH_SHOT"))
            std::sscanf(shot, "%f,%f,%f,%f,%f,%f", &from.x, &from.y, &from.z, &at.x, &at.y, &at.z);
        camera.position = from;
        vec3 d = glm::normalize(at + vec3(pick(-8, 8), pick(-3, 3), pick(-8, 8)) - camera.position);
        camera.yaw = std::atan2(d.z, d.x);
        camera.pitch = std::asin(d.y);
        m.tool = Tool::Blast;
    } else {
        float a = pick(0.0f, 2.0f * PI);
        camera.position = CASTLE + vec3(std::cos(a) * 30.0f, 6.0f, std::sin(a) * 30.0f);
        vec3 d = glm::normalize(CASTLE + vec3(pick(-10, 10), pick(2, 9), pick(-10, 10)) - camera.position);
        camera.yaw = std::atan2(d.z, d.x);
        camera.pitch = std::asin(d.y);
        m.tool = Tool::Cannon;
    }
    use(world, m, camera);
}

void benchStepStart(projv::Application& app) {
    if (Bench* b = app.world.ctx().find<Bench>()) {
        if (b->what == "barrage") barrageShot(app.world, *b);
        b->stepStart = BenchClock::now();
    }
}
void benchStepEnd(projv::Application& app) {
    if (Bench* b = app.world.ctx().find<Bench>()) {
        const double ms = msSince(b->stepStart);
        b->physicsMs += ms;
        b->steps++;
        if (b->what == "barrage" && runtime::physicsWorld(app.world).tick() > BARRAGE_FIRST) {
            b->barrageMs += ms;
            b->barrageSteps++;
        }
    }
}

// Update, first: closes the last frame's row, and fires when it is time.
void benchFrame(projv::Application& app) {
    World& world = app.world;
    Bench* b = world.ctx().find<Bench>();
    if (!b) return;
    auto& m = world.ctx().get<Mayhem>();
    Bench::Row row{m.clock, msSince(b->frameStart), b->physicsMs, b->handlerMs, b->flushMs, b->renderMs, b->steps,
                   runtime::physicsWorld(world).stats().bodies, m.pieces};
    b->frameStart = BenchClock::now();
    b->physicsMs = b->handlerMs = b->flushMs = b->renderMs = 0.0;
    b->steps = 0;
    if (m.clock > 1.0f) (b->fired ? b->after : b->before).push_back(row);
    auto& camera = world.ctx().get<Camera>();
    if (b->what == "barrage") {
        if (runtime::physicsWorld(world).tick() < BARRAGE_END) return;
        double worstFrame = 0.0, frames = 0.0;
        uint32_t bodies = 0;
        int counted = 0;
        for (const auto& r : b->before) {
            if (r.t < 3.0f) continue;
            worstFrame = std::max(worstFrame, r.frame);
            frames += r.frame;
            counted++;
            bodies = std::max(bodies, r.bodies);
        }
        projv::core::info("BENCH SUMMARY barrage: physics {:.0f} ms over {} steps, {:.2f} ms a step; frames {:.1f} ms on average, "
                          "{:.0f} at worst; up to {} bodies, {} pieces and {} debris at the end; {} steps dropped",
                          b->barrageMs, b->barrageSteps, b->barrageSteps ? b->barrageMs / b->barrageSteps : 0.0,
                          counted ? frames / counted : 0.0, worstFrame, bodies, m.pieces, world.view<projv::Debris>().size(),
                          app.time().droppedFixedSteps);
        app.closeRequested = true;
        return;
    }
    if (!b->fired && m.clock > 3.0f) {
        b->fired = true;
        // MAYHEM_BENCH_SHOT=x,y,z,tx,ty,tz: from there, at that (a map has no castle wall to aim at).
        vec3 from(0, 7, 30), at(4, 6, 55.5f);
        if (const char* shot = std::getenv("MAYHEM_BENCH_SHOT"))
            std::sscanf(shot, "%f,%f,%f,%f,%f,%f", &from.x, &from.y, &from.z, &at.x, &at.y, &at.z);
        camera.position = from;
        vec3 d = glm::normalize(at - camera.position);
        camera.yaw = std::atan2(d.z, d.x);
        camera.pitch = std::asin(d.y);
        m.tool = b->what == "blast" ? Tool::Blast : Tool::Cannon;
        use(world, m, camera);
    }
    if (b->fired && m.clock > 7.0f) {
        double mean = 0.0;
        for (const auto& r : b->before) mean += r.frame;
        mean /= std::max<size_t>(b->before.size(), 1);
        projv::core::info("BENCH {} ({}): frames before firing {:.2f} ms on average", b->what,
                          (std::getenv("MAYHEM_ONLY") && *std::getenv("MAYHEM_ONLY")) ? "the castle alone" : "the whole park", mean);
        double spike = 0.0, worst = 0.0;
        for (const auto& r : b->after)
            if (r.t < 4.0f) { spike += r.physics; worst = std::max(worst, r.frame); }
        double later = 0.0, laterFrames = 0.0;
        for (const auto& r : b->after)
            if (r.t > 4.0f) { later += r.physics; laterFrames += 1.0; }
        projv::core::info("BENCH SUMMARY physics in the first 0.5 s: {:.0f} ms, worst frame {:.0f} ms, pieces {}, physics a frame after 1 s {:.1f} ms",
                          spike, worst, b->after.empty() ? 0 : b->after.back().pieces, laterFrames > 0.0 ? later / laterFrames : 0.0);
        projv::core::info("BENCH   t      frame  physics(steps) handler  flush  render  other  bodies pieces");
        for (const auto& r : b->after)
            if (r.frame > 2.0 * mean || r.physics > 8.0)
                projv::core::info("BENCH   {:5.2f} {:7.1f} {:7.1f}({}) {:7.1f} {:6.1f} {:6.1f} {:6.1f} {:7} {:6}", r.t, r.frame, r.physics, r.steps,
                                  r.handler, r.flush, r.render, r.frame - r.physics - r.flush - r.render, r.bodies, r.pieces);
        app.closeRequested = true;
    }
}

// =================================================================================================
// Self-test: MAYHEM_SELFTEST=1 sets everything off unattended and checks what happened
// =================================================================================================

struct SelfTest {
    int step = 0;
    bool pass = true;
    std::string capture;   // MAYHEM_CAPTURE: a screenshot prefix
    int shots = 0;
    std::vector<vec3> pinsAt, dominoesAt;
};

void check(SelfTest& test, bool ok, const std::string& what) {
    projv::core::info("MAYHEMTEST: {} | {}", what, ok ? "ok" : "WRONG");
    test.pass = test.pass && ok;
}

// MAYHEM_LOOK="x,y,z,tx,ty,tz": stands the camera at x,y,z looking at tx,ty,tz, saves a screenshot at 4 s
// (MAYHEM_CAPTURE=<file prefix>) and exits. MAYHEM_BOUNCE=0|1|2 and MAYHEM_DEBUG=0|2|4 set the look.
void lookTest(projv::Application& app) {
    const char* spec = std::getenv("MAYHEM_LOOK");
    if (!spec) return;
    World& world = app.world;
    auto& camera = world.ctx().get<Camera>();
    vec3 from, at;
    if (std::sscanf(spec, "%f,%f,%f,%f,%f,%f", &from.x, &from.y, &from.z, &at.x, &at.y, &at.z) != 6) return;
    camera.position = from;
    vec3 d = glm::normalize(at - from);
    camera.yaw = std::atan2(d.z, d.x);
    camera.pitch = std::asin(d.y);
    static bool shot = false, fired = false;
    float t = world.ctx().get<Mayhem>().clock;
    // MAYHEM_FIRE="x,y,z,tx,ty,tz": a cannonball from x,y,z at tx,ty,tz at 1 s, to have rubble to look at.
    vec3 gun, target;
    if (const char* fire = std::getenv("MAYHEM_FIRE"); !fired && t > 1.0f && fire &&
        std::sscanf(fire, "%f,%f,%f,%f,%f,%f", &gun.x, &gun.y, &gun.z, &target.x, &target.y, &target.z) == 6) {
        fired = true;
        Camera aimer = camera;
        aimer.position = gun;
        vec3 g = glm::normalize(target - gun);
        aimer.yaw = std::atan2(g.z, g.x);
        aimer.pitch = std::asin(g.y);
        auto& m = world.ctx().get<Mayhem>();
        m.tool = Tool::Cannon;
        use(world, m, aimer);
    }
    // MAYHEM_LOOK_AT=seconds: when the still is taken (default 4 s; earlier catches the rubble in flight).
    static const float shotAt = std::getenv("MAYHEM_LOOK_AT") ? float(std::atof(std::getenv("MAYHEM_LOOK_AT"))) : 4.0f;
    if (!shot && t > shotAt) {
        shot = true;
        if (const char* prefix = std::getenv("MAYHEM_CAPTURE")) bgfx::requestScreenShot(BGFX_INVALID_HANDLE, prefix);
    }
    if (t > 4.6f) app.closeRequested = true;
}

void selfTest(projv::Application& app) {
    World& world = app.world;
    SelfTest* test = world.ctx().find<SelfTest>();
    if (!test) return;
    auto& m = world.ctx().get<Mayhem>();
    auto& camera = world.ctx().get<Camera>();
    const float t = m.clock;
    auto look = [&](vec3 from, vec3 at) {
        camera.position = from;
        vec3 d = glm::normalize(at - from);
        camera.yaw = std::atan2(d.z, d.x);
        camera.pitch = std::asin(d.y);
    };
    auto shoot = [&](const char* name) {
        if (test->capture.empty()) return;
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, (test->capture + "-" + name).c_str());
        test->shots++;
    };
    auto positions = [&](const std::vector<Entity>& es) {
        std::vector<vec3> out;
        for (Entity e : es) out.push_back(world.valid(e) ? runtime::bodyPose(world, e).position : vec3(1e9f));
        return out;
    };
    // Each stage once its time has come, in simulated seconds.
    struct Stage { float at; std::function<void()> run; };
    const std::vector<Stage> stages = {
        {0.5f, [&] { look(vec3(0, 40, -120), vec3(0, 0, 10)); }},
        {1.5f, [&] {
             shoot("overview");
             test->pinsAt = positions(m.pins);
             test->dominoesAt = positions(m.dominoes);
             // Something on each plate, the wrecking ball away, a bowl, the cannon at the castle wall.
             const vec3 gatePlate = world.get<projv::Transform>(m.gatePlate).position + vec3(1.5f, 1.0f, 1.5f);
             keep(world, m.spawned, place(world, m.crate, gatePlate, IDENTITY, body(30.0f)), 150);
             const vec3 dominoPlate = world.get<projv::Transform>(m.dominoPlate).position + vec3(1.0f, 1.5f, 1.0f);
             keep(world, m.spawned, place(world, m.crate, dominoPlate, IDENTITY, body(30.0f)), 150);
             releaseWrecker(world, m);
             bowl(world, m);
             look(vec3(0, 7, 30), vec3(4, 6, 55.5f));
             m.tool = Tool::Cannon;
             use(world, m, camera);
         }},
        {2.5f, [&] {
             // The TNT stack, and a blast at the composite wall.
             if (!m.tnt.empty()) m.fuses.push_back({*m.tnt.begin(), 0.0f});
             look(vec3(28, 4, 50), vec3(28, 4, 60));
             m.tool = Tool::Blast;
             use(world, m, camera);
         }},
        {3.0f, [&] {
             // Dig under the earth hill's ledge, chip the shale.
             look(vec3(-64, 3, 34), vec3(-70, 5, 45));
             m.tool = Tool::Dig;
             for (int i = 0; i < 3; i++) { camera.pitch += 0.05f; use(world, m, camera); }
             look(vec3(-53, 4, 62), vec3(-54, 6, 70));
             m.tool = Tool::Pickaxe;
             use(world, m, camera);
         }},
        {4.0f, [&] { look(vec3(-8, 14, 30), vec3(0, 6, 60)); }},
        {4.5f, [&] { shoot("castle"); }},
        {5.0f, [&] { look(vec3(40, 18, -40), vec3(64, 0, -6)); }},
        {5.5f, [&] { shoot("stacks"); }},
        {6.0f, [&] { look(vec3(-35, 14, -10), vec3(-62, 4, 15)); }},
        {6.5f, [&] { shoot("machines"); }},
        {7.0f, [&] { look(vec3(-10, 10, -80), vec3(-10, 2, -45)); }},
        {7.5f, [&] { shoot("playground"); }},
        {8.0f, [&] { look(vec3(-60, 14, 25), vec3(-70, 4, 60)); }},
        {8.5f, [&] { shoot("quarry"); }},
        {9.0f, [&] { look(vec3(50, 12, 50), vec3(66, 2, 75)); }},
        {9.5f, [&] { shoot("bowling"); }},
        {14.0f, [&] {
             int pinsDown = 0;
             auto now = positions(m.pins);
             for (size_t i = 0; i < now.size() && i < test->pinsAt.size(); i++) pinsDown += glm::length(now[i] - test->pinsAt[i]) > 1.0f;
             int toppled = 0;
             for (Entity d : m.dominoes)
                 if (world.valid(d) && (runtime::bodyPose(world, d).rotation * vec3(0, 1, 0)).y < 0.8f) toppled++;
             int aloft = 0;
             for (Entity b : world.view<projv::RigidBody>())
                 if (world.get<projv::RigidBody>(b).gravityFactor < 0.0f && runtime::bodyPose(world, b).position.y > 3.0f) aloft++;
             const Watch& w = world.ctx().get<Watch>();
             check(*test, m.wreckerStruck, "the wrecking ball broke the castle wall");
             check(*test, pinsDown >= 5, fmt::format("the bowling ball knocked {} of 10 pins over", pinsDown));
             check(*test, toppled >= 5, fmt::format("the domino plate set the spiral off: {} toppled", toppled));
             check(*test, w.gateWidest > 1.0f, fmt::format("the plate opened the gate to {:.2f} rad", w.gateWidest));
             check(*test, w.elevatorHighest > 5.0f, fmt::format("the elevator rose to {:.1f} m", w.elevatorHighest));
             check(*test, std::abs(w.ferrisSpeed - 0.25f) < 0.08f, fmt::format("the Ferris wheel turns at {:.2f} rad/s", w.ferrisSpeed));
             check(*test, aloft >= 8, fmt::format("{} balloons aloft", aloft));
             check(*test, m.padLaunches > 0, fmt::format("the jump pads launched {} things", m.padLaunches));
             check(*test, m.explosions >= 4, fmt::format("{} explosions (the TNT stack went up in a chain)", m.explosions));
             check(*test, m.pieces > 100, fmt::format("{} pieces broke off", m.pieces));
             check(*test, m.fellThrough == 0, "nothing fell through the floor");
             bool finite = true;
             for (auto [e, b] : world.view<projv::PhysicsBody>().each()) finite = finite && std::isfinite(b.position.x + b.position.y + b.position.z);
             check(*test, finite, "every body is finite");
             const auto& stats = runtime::physicsWorld(world).stats();
             projv::core::info("MAYHEMTEST: {} bodies at the end, physics step {:.2f} ms", stats.bodies, stats.lastStepMilliseconds);
             projv::core::info("MAYHEMTEST: {}", test->pass ? "PASS" : "FAIL");
         }},
        {14.5f, [&] { app.closeRequested = true; }},
    };
    while (test->step < int(stages.size()) && t >= stages[size_t(test->step)].at) stages[size_t(test->step++)].run();
}

// =================================================================================================
// Startup, render, shutdown
// =================================================================================================

void makePrototypes(projv::Scene& scene, Mayhem& m) {
    m.crate = prototype(scene, "crate", ivec3(4), 0.25f, [](int x, int y, int z) {
        int edges = (x == 0 || x == 3) + (y == 0 || y == 3) + (z == 0 || z == 3);
        return edges >= 2 ? rgb(0.45f, 0.28f, 0.12f) : rgb(0.78f, 0.56f, 0.3f);
    });
    m.tntCrate = prototype(scene, "tnt", ivec3(8), 0.125f, [](int x, int y, int z) {
        int edges = (x == 0 || x == 7) + (y == 0 || y == 7) + (z == 0 || z == 7);
        if (edges >= 2) return rgb(0.25f, 0.08f, 0.06f);
        return y == 3 || y == 4 ? rgb(0.95f, 0.85f, 0.2f) : rgb(0.85f, 0.1f, 0.08f);
    });
    glow(scene, m.tntCrate.chunk, 0.25f);
    const int diameters[5] = {2, 3, 4, 6, 8};   // 0.25 m to 1 m, in 12.5 cm voxels
    for (int i = 0; i < 5; i++) {
        int n = diameters[i];
        vec3 colour = hsv(float(i) / 5.0f, 0.75f, 0.95f);
        m.balls[i] = prototype(scene, "ball", ivec3(n), 0.125f, [n, colour](int x, int y, int z) -> uint32_t {
            vec3 d = vec3(x, y, z) + vec3(0.5f) - vec3(float(n) * 0.5f);
            if (glm::length(d) > float(n) * 0.5f) return 0u;
            return std::abs(d.y) < float(n) * 0.15f ? rgb(0.95f, 0.95f, 0.95f) : rgb(colour);
        });
    }
    m.cannonball = prototype(scene, "cannonball", ivec3(8), 0.0625f, [](int x, int y, int z) -> uint32_t {
        vec3 d = vec3(x, y, z) + vec3(0.5f) - vec3(4.0f);
        return glm::length(d) <= 4.0f ? rgb(0.16f, 0.16f, 0.18f) : 0u;
    });
    m.domino = prototype(scene, "domino", ivec3(2, 16, 8), 0.125f, [](int x, int y, int z) {
        bool pip = x == 1 && ((y == 4 && z == 2) || (y == 4 && z == 5) || (y == 11 && z == 3) || (y == 12 && z == 4) || y == 8);
        return pip ? rgb(0.1f, 0.1f, 0.1f) : rgb(0.95f, 0.94f, 0.9f);
    });
    m.jenga = prototype(scene, "jenga", ivec3(24, 4, 8), 0.125f, [](int x, int y, int z) {
        return (x + y * 3 + z) % 5 ? rgb(0.86f, 0.7f, 0.48f) : rgb(0.8f, 0.63f, 0.42f);
    });
    m.plank = prototype(scene, "plank", ivec3(10, 1, 4), 0.125f, [](int x, int, int z) {
        return (x + z) % 3 ? rgb(0.66f, 0.46f, 0.26f) : rgb(0.58f, 0.39f, 0.21f);
    });
    m.link = prototype(scene, "link", ivec3(2, 6, 2), 0.125f, [](int, int y, int) {
        return y % 2 ? rgb(0.5f, 0.52f, 0.56f) : rgb(0.36f, 0.38f, 0.42f);
    });
    m.wheel = prototype(scene, "wheel", ivec3(3, 7, 7), 0.125f, [](int x, int y, int z) -> uint32_t {
        float r = glm::length(vec2(float(y) + 0.5f - 3.5f, float(z) + 0.5f - 3.5f));
        if (r > 3.5f) return 0u;
        return r < 1.6f ? rgb(0.75f, 0.75f, 0.78f) : (x == 1 ? rgb(0.12f, 0.12f, 0.13f) : rgb(0.18f, 0.18f, 0.2f));
    });
    m.knuckle = prototype(scene, "knuckle", ivec3(2), 0.125f, [](int, int, int) { return rgb(0.3f, 0.3f, 0.32f); });
    m.ropeLink = prototype(scene, "rope", ivec3(1, 4, 1), 0.06f, [](int, int y, int) {
        return y % 2 ? rgb(0.55f, 0.45f, 0.3f) : rgb(0.45f, 0.36f, 0.24f);
    });
    m.pin = prototype(scene, "pin", ivec3(8, 40, 8), 0.125f, [](int x, int y, int z) -> uint32_t {
        float h = float(y) / 39.0f;
        float radius = 2.0f + 2.0f * std::sin(PI * std::clamp(h * 1.15f, 0.0f, 1.0f)) * (h < 0.62f ? 1.0f : 0.55f) + (h > 0.8f ? 0.8f : 0.0f);
        if (glm::length(vec2(float(x), float(z)) + 0.5f - 4.0f) > std::min(radius, 4.0f)) return 0u;
        return (y == 27 || y == 28 || y == 30) ? rgb(0.85f, 0.1f, 0.1f) : rgb(0.96f, 0.95f, 0.92f);
    });
    m.bowlingBallProto = prototype(scene, "bowling ball", ivec3(24), 0.25f, [](int x, int y, int z) -> uint32_t {
        vec3 d = vec3(x, y, z) + vec3(0.5f) - vec3(12.0f);
        if (glm::length(d) > 12.0f) return 0u;
        bool hole = glm::length(d - vec3(0, 9, 4)) < 2.0f || glm::length(d - vec3(3, 9, -2)) < 1.8f || glm::length(d - vec3(-3, 9, -2)) < 1.8f;
        if (hole) return 0u;
        return (x * 7 + y * 3 + z * 5) % 9 == 0 ? rgb(0.35f, 0.15f, 0.55f) : rgb(0.22f, 0.08f, 0.38f);
    });
    m.barrel = prototype(scene, "barrel", ivec3(6, 10, 6), 0.125f, [](int x, int y, int z) -> uint32_t {
        if (glm::length(vec2(float(x), float(z)) + 0.5f - 3.0f) > 3.0f) return 0u;
        return y == 1 || y == 8 ? rgb(0.25f, 0.25f, 0.27f) : rgb(0.15f, 0.45f, 0.25f);
    });
    m.glassBox = prototype(scene, "glass box", ivec3(6), 0.125f, [](int x, int y, int z) {
        int edges = (x == 0 || x == 5) + (y == 0 || y == 5) + (z == 0 || z == 5);
        return edges >= 2 ? rgb(0.75f, 0.9f, 0.95f) : rgb(0.55f, 0.78f, 0.88f);
    });
    m.balloon = prototype(scene, "balloon", ivec3(10, 11, 10), 0.125f, [](int x, int y, int z) -> uint32_t {
        vec3 d = vec3(x, y, z) + vec3(0.5f) - vec3(5.0f, 6.0f, 5.0f);
        d.y *= 0.85f;
        bool knot = y == 0 && x >= 4 && x <= 5 && z >= 4 && z <= 5;
        if (glm::length(d) > 5.0f && !knot) return 0u;
        return knot ? rgb(0.7f, 0.15f, 0.2f) : rgb(0.95f, 0.3f, 0.4f);
    });
    m.orb = prototype(scene, "orb", ivec3(16), 0.25f, [](int x, int y, int z) -> uint32_t {
        vec3 c = vec3(float(x), float(y), float(z)) + vec3(0.5f) - vec3(8.0f);
        if (glm::length(c) > 8.0f) return 0u;
        return std::abs(c.y) < 1.0f ? rgb(0.95f, 0.45f, 0.1f) : rgb(0.88f, 0.88f, 0.86f);
    });
    glow(scene, m.orb.chunk, 0.3f);
    m.gondola = prototype(scene, "gondola", ivec3(20, 26, 16), 0.125f, [](int x, int y, int z) -> uint32_t {
        bool bucket = y < 16 && (y == 0 || x == 0 || x == 19 || z == 0 || z == 15) && (y < 8 || x == 0 || x == 19);
        bool bar = y >= 16 && x >= 9 && x <= 10 && z >= 7 && z <= 8;
        if (!bucket && !bar) return 0u;
        return bar ? rgb(0.6f, 0.6f, 0.65f) : rgb(0.2f, 0.6f, 0.9f);
    });
    m.wreckerProto = prototype(scene, "wrecking ball", ivec3(16), 0.125f, [](int x, int y, int z) -> uint32_t {
        vec3 d = vec3(x, y, z) + vec3(0.5f) - vec3(8.0f);
        return glm::length(d) <= 8.0f ? rgb(0.2f, 0.2f, 0.22f) : 0u;
    });
    m.fireball = prototype(scene, "fireball", ivec3(8), 0.25f, [](int x, int y, int z) -> uint32_t {
        vec3 d = vec3(x, y, z) + vec3(0.5f) - vec3(4.0f);
        float r = glm::length(d);
        if (r > 4.0f) return 0u;
        return r < 2.5f ? rgb(1.0f, 0.9f, 0.4f) : rgb(1.0f, 0.45f, 0.1f);
    });
    glow(scene, m.fireball.chunk, 12.0f);
    m.beam = prototype(scene, "beam", ivec3(48, 4, 3), 0.25f, [](int x, int, int) {
        return (x / 4) % 2 ? rgb(0.95f, 0.75f, 0.1f) : rgb(0.15f, 0.15f, 0.17f);
    });
}

// A map's bodies that overlap each other: what the Teardown converter leaves where the game had joints or
// rules it does not export -- a vehicle's wheels sit in its wheel wells (Teardown never collides a wheel
// with its own vehicle), a container's doors in its frame (hinged, in the game). Free, each pair shoves
// itself apart at every step. So each group of overlapping bodies is made one thing: one collision
// family (they never collide with each other), and every member welded to the group's largest with a
// fixed joint, which a hard enough hit breaks.
//
// MAYHEM_MAP_SCAN=1 also reports, for every body, the share of its voxels inside the static world or
// another body (worst first), and exits.
void joinOverlappingBodies(World& world, Mayhem& m, const projv::Scene& scene, bool report) {
    struct Placed { Entity e; ComponentHandle h; bool dynamic; std::vector<glm::ivec3> cells; };
    auto voxelSize = [&](ComponentHandle h) {
        for (const projv::Chunk& c : scene.chunks) {
            if (!c.alive) continue;
            ComponentHandle owner = c.gridIndex >= 0 ? scene.grids[c.gridIndex].componentHandle : c.componentHandle;
            // The chunk's world edge over its voxels across: header.voxelScale is not that for a loaded
            // .data chunk, and is not rescaled with the component
            if (owner == h) return c.header.resolution > 0 ? c.header.scale / float(c.header.resolution) : 0.0f;
        }
        return 0.0f;
    };
    std::vector<Placed> placed;
    float cell = 0.0f;   // the bin every voxel centre is put in: the finest voxel
    for (auto [e, body] : world.view<projv::RigidBody>().each()) {
        ComponentHandle h = runtime::voxelComponentOf(world, e);
        if (h != projv::INVALID_COMPONENT_HANDLE && body.motion == runtime::MotionType::Dynamic) placed.push_back({e, h, true, {}});
    }
    for (auto [e, collider] : world.view<projv::StaticCollider>().each()) {
        ComponentHandle h = runtime::voxelComponentOf(world, e);
        if (h != projv::INVALID_COMPONENT_HANDLE) placed.push_back({e, h, false, {}});
    }
    for (const Placed& p : placed) {
        const float v = voxelSize(p.h);
        if (v > 0.0f) cell = cell > 0.0f ? std::min(cell, v) : v;
    }
    if (cell <= 0.0f) return;
    auto key = [](glm::ivec3 c) { return (int64_t(c.x + 1048576) << 42) | (int64_t(c.y + 1048576) << 21) | int64_t(c.z + 1048576); };
    std::unordered_map<int64_t, int> bodyCells;   // cell -> the first body (index into placed) to claim it
    // The static voxels under a body: cell -> which static component, and the voxel there in its own space
    // (to carve it). Only those in a metre cell some body reaches are kept: the world has millions.
    struct StaticVoxel { int index; glm::ivec3 voxel; };
    std::unordered_map<int64_t, StaticVoxel> staticCells;
    std::unordered_set<int64_t> nearBodies;
    for (int pass = 0; pass < 2; pass++)   // the bodies first, then the static world round them
        for (size_t i = 0; i < placed.size(); i++) {
            Placed& p = placed[i];
            if (p.dynamic != (pass == 0)) continue;
            const projv::ComponentRecord& rec = scene.components[p.h];
            const float v = voxelSize(p.h);
            for (const projv::utils::ComponentVoxel& vox : projv::utils::componentVoxels(scene, p.h)) {
                const vec3 w = rec.localPosition + rec.localRotation * ((vec3(vox.position) + 0.5f) * v);
                const glm::ivec3 c = glm::ivec3(glm::floor(w / cell));
                if (p.dynamic) {
                    p.cells.push_back(c);
                    bodyCells.emplace(key(c), int(i));
                    nearBodies.insert(key(glm::ivec3(glm::floor(w))));
                } else if (nearBodies.count(key(glm::ivec3(glm::floor(w))))) {
                    staticCells.emplace(key(c), StaticVoxel{int(i), vox.position});
                }
            }
        }
    // Overlaps, pair by pair: voxels of body i in a cell body j claimed.
    std::map<std::pair<int, int>, int> shared;
    std::vector<int> inWorld(placed.size(), 0), inBodies(placed.size(), 0);
    std::map<int, std::vector<projv::PendingVoxelOp>> carve;   // static index -> its voxels a body stands in
    for (size_t i = 0; i < placed.size(); i++) {
        if (!placed[i].dynamic) continue;
        for (glm::ivec3 c : placed[i].cells) {
            if (auto it = staticCells.find(key(c)); it != staticCells.end()) {
                inWorld[i]++;
                carve[it->second.index].push_back({false, it->second.voxel, 0u});
                staticCells.erase(it);
            }
            auto it = bodyCells.find(key(c));
            if (it == bodyCells.end() || it->second == int(i)) continue;
            inBodies[i]++;
            shared[{std::min(int(i), it->second), std::max(int(i), it->second)}]++;
        }
    }
    if (report) {
        std::vector<size_t> order;
        for (size_t i = 0; i < placed.size(); i++) if (placed[i].dynamic && !placed[i].cells.empty()) order.push_back(i);
        auto share = [&](size_t i) { return float(inWorld[i] + inBodies[i]) / float(placed[i].cells.size()); };
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return share(a) > share(b); });
        int bad = 0;
        for (size_t i : order) {
            if (share(i) > 0.15f) bad++;
            const projv::ComponentRecord& rec = scene.components[placed[i].h];
            projv::core::info("SCAN body id {} ({} voxels) at ({:.1f}, {:.1f}, {:.1f}): {:.0f}% in the world, {:.0f}% in other bodies",
                              rec.localId, placed[i].cells.size(), rec.localPosition.x, rec.localPosition.y, rec.localPosition.z,
                              100.0f * inWorld[i] / placed[i].cells.size(), 100.0f * inBodies[i] / placed[i].cells.size());
        }
        projv::core::info("SCAN {} bodies, {} over 15% inside something", order.size(), bad);
    }

    // Groups: bodies joined by any real overlap (a few voxels' contact is not one).
    std::vector<int> parent(placed.size());   // (static entries stay alone: only bodies are grouped)
    for (size_t i = 0; i < parent.size(); i++) parent[i] = int(i);
    std::function<int(int)> find = [&](int i) { return parent[i] == i ? i : parent[i] = find(parent[i]); };
    for (const auto& [pair, n] : shared)
        if (n >= 8) parent[find(pair.first)] = find(pair.second);
    std::map<int, std::vector<int>> groups;
    for (size_t i = 0; i < placed.size(); i++)
        if (placed[i].dynamic) groups[find(int(i))].push_back(int(i));
    uint32_t family = 500000;   // clear of the shards' (from 1000) and the ferris wheel's (7)
    int joined = 0;
    for (auto& [root, members] : groups) {
        if (members.size() < 2) continue;
        family++;
        const int core = *std::max_element(members.begin(), members.end(),
                                           [&](int a, int b) { return placed[a].cells.size() < placed[b].cells.size(); });
        for (int i : members) {
            projv::CollisionGroups g = world.all_of<projv::CollisionGroups>(placed[i].e) ? world.get<projv::CollisionGroups>(placed[i].e) : projv::CollisionGroups{};
            g.family = family;
            world.emplace_or_replace<projv::CollisionGroups>(placed[i].e, g);
            if (i == core) continue;
            vec3 centre(0.0f);
            for (glm::ivec3 c : placed[i].cells) centre += (vec3(c) + 0.5f) * cell;
            projv::Joint weld;
            weld.type = runtime::JointType::Fixed;
            weld.anchor = centre / float(placed[i].cells.size());
            weld.breakForce = 4.0e4f;   // N: a wheel or a door stays on through a knock, not a cannonball
            weld.breakTorque = 2.0e4f;
            joint(world, m, placed[i].e, placed[core].e, weld);
            joined++;
        }
    }
    // A body inside the static world (a container sunk into the quay, where the level had a hole cut
    // for it that the converter does not carve: teardown-physics.md, A1) takes the space: the world's
    // voxels there go.
    size_t carved = 0;
    for (auto& [i, ops] : carve) {
        carved += ops.size();
        runtime::editVoxels(world, placed[i].e, ops);
    }
    projv::core::info("mayhem: {} overlapping bodies welded into {} groups; {} world voxels inside bodies cleared",
                      joined, family - 500000, carved);
}

void startup(projv::Application& app) {
    World& world = app.world;
    const fs::path here = projv::core::executableDirectory();
    const bool unattended = std::getenv("MAYHEM_SELFTEST") != nullptr || std::getenv("MAYHEM_BENCH") != nullptr ||
                            std::getenv("MAYHEM_LOOK") != nullptr;

    auto& renderInstance = world.ctx().emplace<projv::graphics::RenderInstance>();
    renderInstance.initialize(1600, 900, "Voxel Mayhem");
    projv::graphics::installPlatform(app, renderInstance);
    projv::graphics::setCursorCaptured(app, renderInstance, !unattended);

    world.ctx().emplace<Camera>();
    world.ctx().emplace<Shake>();
    View& look = world.ctx().emplace<View>();
    if (const char* v = std::getenv("MAYHEM_DEBUG")) look.debugView = std::atoi(v);
    if (const char* v = std::getenv("MAYHEM_AO")) look.ambientOcclusion = std::atoi(v) != 0;
    if (const char* v = std::getenv("MAYHEM_GI")) look.gi = std::atoi(v) != 0;
    world.ctx().emplace<Watch>();
    // MAYHEM_MAP=<folder>: an authored map's voxels in place of the park's floor.
    const char* mapFolder = std::getenv("MAYHEM_MAP");
    auto& scene = mapFolder ? world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(mapFolder))
                            : world.ctx().emplace<projv::Scene>();
    auto& m = world.ctx().emplace<Mayhem>();
    m.unattended = unattended;
    m.map = mapFolder != nullptr;
    vec3 mapMin{0.0f}, mapMax{0.0f};
    if (m.map) {
        // The Teardown imports are written ten times life size (the converter's --td-scale 10: a voxel a
        // world unit, where Teardown's are 10 cm), which to the physics is a level that falls slowly
        // and weighs a thousand times what it should. MAYHEM_MAP_SCALE (default 0.1) brings it back to
        // metres, the size Mayhem's tools are made for; 1 leaves it as written.
        const float rescale = std::getenv("MAYHEM_MAP_SCALE") ? float(std::atof(std::getenv("MAYHEM_MAP_SCALE"))) : 0.1f;
        if (rescale > 0.0f && rescale != 1.0f)
            for (ComponentHandle h : projv::utils::rootComponents(scene)) {
                const projv::ComponentRecord& c = scene.components[h];
                projv::utils::setComponentTransform(scene, h, c.localPosition * rescale, c.localRotation, c.localScale * rescale);
            }
        bool found = false;
        float finest = 0.0f;
        for (const projv::Chunk& chunk : scene.chunks) {
            if (!chunk.alive || chunk.header.scale <= 0.0f) continue;
            const vec3 lo = chunk.header.position, hi = chunk.header.position + vec3(chunk.header.scale);
            mapMin = found ? glm::min(mapMin, lo) : lo;
            mapMax = found ? glm::max(mapMax, hi) : hi;
            found = true;
            // the chunk's world edge over its voxels across (header.voxelScale is not that for a .data chunk)
            const float v = chunk.header.resolution > 0 ? chunk.header.scale / float(chunk.header.resolution) : 0.0f;
            if (v > 0.0f) finest = finest > 0.0f ? std::min(finest, v) : v;
        }
        if (!found) {
            projv::core::error("mayhem: no map in '{}' (a folder with a compose.json)", mapFolder);
            std::exit(1);
        }
        if (finest > 0.0f) look.finestVoxel = finest;
        look.hazeRadius = std::max(glm::length(mapMax - mapMin), 1.0f);
        projv::core::info("mayhem: map '{}', {:.0f} x {:.0f} x {:.0f} m, from ({:.0f}, {:.0f}, {:.0f})", mapFolder, mapMax.x - mapMin.x,
                          mapMax.y - mapMin.y, mapMax.z - mapMin.z, mapMin.x, mapMin.y, mapMin.z);
    }

    runtime::installSceneBridge(app);
    runtime::PhysicsConfig physics;
    physics.settings.worldMin = vec3(-1000.0f, -40.0f, -1000.0f);
    physics.settings.worldMax = vec3(1000.0f, 250.0f, 1000.0f);   // balloons that get away are gone at 250 m
    if (m.map) {   // the map's bounds, with room round them
        // A map's 10 cm chunks are far more detailed than the park's: its world collides as its voxels'
        // tile tables, read in place -- nothing to build at load, nothing approximated (MAYHEM_COLLISION=boxes:
        // as boxes, up to 16k a chunk before one collides as its content box, a solid block you cannot
        // walk into).
        const char* representation = std::getenv("MAYHEM_COLLISION");
        physics.collision.representation = representation && std::string(representation) == "boxes"
                                               ? projv::utils::CollisionRepresentation::Boxes
                                               : projv::utils::CollisionRepresentation::Tiles;
        physics.collision.pieceBudget = std::getenv("MAYHEM_MAP_BOXES") ? uint32_t(std::atoi(std::getenv("MAYHEM_MAP_BOXES"))) : 16384u;
        // A map's breaks make thousands of small pieces: up to MAP_DEBRIS_VOXELS, cheap debris -- no
        // bodies, a fall and a bounce against the world (MAYHEM_DEBRIS=0: bodies, as the park's are).
        physics.debris.maxVoxels = std::getenv("MAYHEM_DEBRIS") ? uint32_t(std::atoi(std::getenv("MAYHEM_DEBRIS"))) : MAP_DEBRIS_VOXELS;
        physics.settings.worldMin = glm::min(physics.settings.worldMin, mapMin - vec3(200.0f, 40.0f, 200.0f));
        physics.settings.worldMax = glm::max(physics.settings.worldMax, mapMax + vec3(200.0f, 250.0f, 200.0f));
    }
    physics.maxPieces = 2500;
    physics.dustVoxels = 1;   // a lone voxel a break leaves is dust: no body
    // A shard breaks again only on a hit that would break at least two of its material's shard sizes:
    // what keeps a big break from cascading into thousands of crumbs as its shards land.
    physics.fracture.minImpactBreak = 2.0f;
    // For measuring: the fracture limits, overridden.
    if (const char* v = std::getenv("MAYHEM_MAXSHARDS")) physics.fracture.maxShards = uint32_t(std::atoi(v));
    if (const char* v = std::getenv("MAYHEM_DUST")) physics.dustVoxels = uint32_t(std::atoi(v));
    if (const char* v = std::getenv("MAYHEM_MINBREAK")) physics.fracture.minImpactBreak = float(std::atof(v));
    runtime::installPhysics(app, physics);
    runtime::installImpactFracture(app);   // what is struck hard enough breaks
    runtime::registerFracturePattern(world, "slabs", slabs);   // before the library names it
    if (!runtime::loadPhysicsMaterials(world, (here / "physics_materials.json").string()))
        projv::core::error("mayhem: physics_materials.json did not load; materials will all behave alike");
    // The map's own materials (the Teardown imports' td_*), beside Mayhem's.
    if (m.map && !runtime::loadPhysicsMaterials(world, (fs::path(mapFolder) / "physics_materials.json").string()))
        projv::core::warn("mayhem: the map has no physics_materials.json; its voxels break as the default material");
    defineThrownTwins(world);
    for (const char* group : {"small debris", "player"}) runtime::definePhysicsGroup(world, group);

    // The floor: 220 m square of half-metre voxels, grass round a stone plaza, its top at y = 0.
    if (!m.map) {
        ComponentHandle floor = voxels(scene, "floor", ivec3(440, 2, 440), 0.5f, vec3(-FLOOR_HALF, -1.0f, -FLOOR_HALF), [](int x, int, int z) {
            float wx = float(x) * 0.5f - FLOOR_HALF, wz = float(z) * 0.5f - FLOOR_HALF;
            if (std::abs(wx) < 24.0f && std::abs(wz) < 26.0f)
                return ((x / 4 + z / 4) % 2) ? rgb(0.62f, 0.6f, 0.56f) : rgb(0.56f, 0.54f, 0.5f);
            int n = (x * 73 + z * 151 + (x / 7) * 31 + (z / 5) * 17) % 7;
            return n < 3 ? rgb(0.34f, 0.52f, 0.24f) : n < 5 ? rgb(0.31f, 0.49f, 0.22f) : rgb(0.37f, 0.55f, 0.26f);
        });
        m.floor = runtime::spawnComponent(world, floor);
        world.emplace<projv::StaticCollider>(m.floor);
    } else {
        // The map's entities.json: its static world and its breakable bodies.
        runtime::spawnEntities(world);
        // The level itself breaks like the park's walls: a hit hard enough breaks it, and what a break
        // or a dig leaves standing on nothing falls (held by the ground at the bottom of the level). Only
        // a fast hit is an impact on it -- a cannonball's 50 m/s, not debris landing -- or its soft
        // materials (dirt at 400) would break under every falling shard, and those shards break it again.
        for (auto [e, collider] : world.view<projv::StaticCollider>().each())
            world.patch<projv::StaticCollider>(e, [](projv::StaticCollider& c) {
                c.breaksOnImpact = true;
                c.minImpactSpeed = 12.0f;
                c.splits = true;
                c.anchorBase = true;
            });
        joinOverlappingBodies(world, m, scene, std::getenv("MAYHEM_MAP_SCAN") != nullptr);
        if (std::getenv("MAYHEM_MAP_SCAN")) std::exit(0);
        // Every body starts asleep, as a level's props do: costing nothing until something reaches it.
        // Awake, the bodies the converter left overlapping (docs/plans/teardown-physics.md, Part A)
        // shove each other apart at every step and the simulation falls behind.
        if (!std::getenv("MAYHEM_MAP_AWAKE"))
            for (auto [e, body, t] : world.view<projv::RigidBody, projv::Transform>().each())
                if (body.motion == runtime::MotionType::Dynamic)
                    runtime::setBodyState(world, e, t.position, t.rotation, vec3(0.0f), vec3(0.0f), true);
        auto& camera = world.ctx().get<Camera>();
        const vec3 centre = (mapMin + mapMax) * 0.5f, size = mapMax - mapMin;
        camera.position = vec3(centre.x, mapMin.y + std::min(size.y, 40.0f) + 10.0f, mapMin.z - 10.0f);
        const vec3 d = glm::normalize(vec3(centre.x, mapMin.y, centre.z) - camera.position);
        camera.yaw = std::atan2(d.z, d.x);
        camera.pitch = std::asin(d.y);
    }

    makePrototypes(scene, m);

    app.events().on<projv::BodyBroken>([&world](const projv::BodyBroken& e) { onBroken(world, e); });
    app.events().on<projv::ContactBegan>([&world](const projv::ContactBegan& c) { onContact(world, c); });
    app.events().on<projv::JointBroke>([&world](const projv::JointBroke& e) {
        if (!e.bodyGone) world.ctx().get<Mayhem>().jointsBroken++;
    });
    app.events().on<projv::BodyEmptied>([&world](const projv::BodyEmptied& e) {
        if (world.valid(e.entity)) world.destroy(e.entity);
    });
    app.events().on<projv::PiecesCleared>([&world](const projv::PiecesCleared& e) {
        world.ctx().get<Mayhem>().piecesCleared += int(e.entities.size());
    });
    app.events().on<projv::BodyLeftWorld>([&world](const projv::BodyLeftWorld& e) {
        if (!world.ctx().get<Mayhem>().map && e.position.y < 0.0f && std::abs(e.position.x) < FLOOR_HALF - 2.0f && std::abs(e.position.z) < FLOOR_HALF - 2.0f) {
            world.ctx().get<Mayhem>().fellThrough++;
            projv::core::error("mayhem: a body fell through the floor at ({:.1f}, {:.1f})", e.position.x, e.position.z);
        }
    });
    app.events().on<projv::SimulationSlow>([](const projv::SimulationSlow& e) {
        static bool said = false;
        if (!said) projv::core::warn("mayhem: the simulation is running slower than real time ({} steps dropped)", e.droppedSteps);
        said = true;
    });

    // The renderer, at half the window's width and height; the reconstruction and TAA
    // rebuild the full frame.
    if (!(restir_test::wanted() && restir_test::install(renderInstance, here)))
        voxel_renderer::install(world, renderInstance, here);
    else
        world.ctx().emplace<restir_test::State>();
    world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));

    if (!m.map) buildWorld(world);
    if (auto* restir = world.ctx().find<restir_test::State>(); restir && m.map) restir->ground = mapMin.y;
    if (const char* what = std::getenv("MAYHEM_BENCH")) {
        world.ctx().emplace<Bench>().what = what;
    } else if (std::getenv("MAYHEM_SELFTEST")) {
        SelfTest& test = world.ctx().emplace<SelfTest>();
        if (const char* prefix = std::getenv("MAYHEM_CAPTURE")) test.capture = prefix;
    } else if (!unattended) {
        printHelp();
    }
}

void render(projv::Application& app) {
    World& world = app.world;
    auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
    auto& scene = world.ctx().get<projv::Scene>();
    auto& gpuData = world.ctx().get<projv::GPUData>();
    const auto& camera = world.ctx().get<Camera>();
    const auto flushStart = BenchClock::now();
    projv::graphics::flushSceneUpdates(scene, gpuData);
    Bench* bench = world.ctx().find<Bench>();
    if (bench) bench->flushMs += msSince(flushStart);
    const auto renderStart = BenchClock::now();
    vec3 position = camera.position, direction = camera.forward();
    // The shake is drawn only: aiming and clicks use the steady camera.
    Shake& shake = world.ctx().get<Shake>();
    shake.clock += app.time().unscaledDelta;
    shake.trauma = std::max(0.0f, shake.trauma - SHAKE_DECAY * app.time().unscaledDelta);
    if (shake.trauma > 0.0f) {
        const float amount = shake.trauma * shake.trauma, t = shake.clock;
        auto noise = [t](float seed) {
            return 0.5f * std::sin(t * 37.0f + seed) + 0.3f * std::sin(t * 61.0f + 2.1f * seed) + 0.2f * std::sin(t * 97.0f + 3.7f * seed);
        };
        position += amount * SHAKE_MOVE * vec3(noise(1.0f), noise(2.0f), noise(3.0f));
        Camera shaken = camera;
        shaken.yaw += amount * SHAKE_TURN * noise(4.0f);
        shaken.pitch += amount * SHAKE_TURN * noise(5.0f);
        direction = shaken.forward();
    }
    if (restir_test::State* restir = world.ctx().find<restir_test::State>()) {
        const View& view = world.ctx().get<View>();
        restir_test::upload(*restir, world, renderInstance.getActiveRenderer(), renderInstance.getWindowResolution(), position, direction,
                            view.sun, view.finestVoxel, view.hazeRadius);
    } else {
        voxel_renderer::upload(world, renderInstance, position, direction);
    }
    projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpuData);
    if (bench) bench->renderMs += msSince(renderStart);
    // MAYHEM_GPU=1: each pass's GPU time, averaged over frames 120..360, printed once (needs the profiler).
    if (std::getenv("MAYHEM_GPU")) {
        static double passMs[16] = {}, frameMs = 0.0;
        static int samples = 0;
        if (app.frameCount == 1) bgfx::setDebug(BGFX_DEBUG_PROFILER);
        const bgfx::Stats* stats = bgfx::getStats();
        if (app.frameCount > 120 && app.frameCount <= 360 && stats && stats->gpuTimerFreq > 0) {
            const double toMs = 1000.0 / double(stats->gpuTimerFreq);
            frameMs += double(stats->gpuTimeEnd - stats->gpuTimeBegin) * toMs;
            for (uint16_t v = 0; v < stats->numViews; v++)
                if (stats->viewStats[v].view < 16)
                    passMs[stats->viewStats[v].view] += double(stats->viewStats[v].gpuTimeEnd - stats->viewStats[v].gpuTimeBegin) * toMs;
            samples++;
        }
        if (app.frameCount == 361 && samples > 0) {
            static const char* names[] = {"gbuffer", "probes", "pack", "ao", "shade", "reconstruct", "taa", "display"};
            std::string line = fmt::format("GPU frame {:.2f} ms |", frameMs / samples);
            for (int v = 0; v < 8; v++) line += fmt::format(" {} {:.2f}", names[v], passMs[v] / samples);
            projv::core::info("{}", line);
        }
    }
    if (renderInstance.shouldClose) app.closeRequested = true;
}

void shutdown(projv::Application& app) {
    projv::graphics::destroyGPUData(app.world.ctx().get<projv::GPUData>());
    // Stopped here rather than left to process exit: bgfx's render thread still
    // presenting while the driver's memory is freed under it crashed the NVIDIA driver at exit.
    bgfx::shutdown();
    glfwTerminate();
    if (const SelfTest* test = app.world.ctx().find<SelfTest>(); test && !test->pass) std::exit(1);
}

}  // namespace

int main() {
    projv::Application app;
    app.addSystem(projv::Stage::Startup,     "startup",      startup);
    // "physics" is added at startup, between whatever names it in `before` and in `after`: so those
    // before it must be added ahead of those after it, or the two contradict and physics goes last.
    app.addSystem(projv::Stage::FixedUpdate, "bench: step start", benchStepStart, {.before = "physics"});
    app.addSystem(projv::Stage::FixedUpdate, "mayhem",       gameTick);
    app.addSystem(projv::Stage::FixedUpdate, "impacts",      impactTick, {.after = "physics"});
    app.addSystem(projv::Stage::FixedUpdate, "bench: step end",   benchStepEnd, {.after = "physics"});
    app.addSystem(projv::Stage::Update,      "bench",        benchFrame, {.before = "controls"});
    app.addSystem(projv::Stage::Update,      "controls",     controls);
    app.addSystem(projv::Stage::Update,      "blender",      moveBlender);
    app.addSystem(projv::Stage::Update,      "self-test",    selfTest);
    app.addSystem(projv::Stage::PostUpdate,  "look test",    lookTest, {.after = "follow camera"});
    app.addSystem(projv::Stage::Update,      "title bar",    titleBar);
    app.addSystem(projv::Stage::PostUpdate,  "follow camera", followCamera, {.after = "physics: present"});
    app.addSystem(projv::Stage::PostUpdate,  "decorations",  presentDecorations, {.before = "scene bridge", .after = "physics: present"});
    app.addSystem(projv::Stage::Render,      "render",       render);
    app.addSystem(projv::Stage::Shutdown,    "shutdown",     shutdown);
    app.run();
    return 0;
}
