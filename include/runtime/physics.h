#ifndef PROJECTV_RUNTIME_PHYSICS_H
#define PROJECTV_RUNTIME_PHYSICS_H

#include <optional>
#include <string>

#include "nlohmann/json.hpp"
#include "core/application.h"
#include "core/world.h"
#include "runtime/entities.h"
#include "runtime/net.h"
#include "runtime/physics/physics_world.h"
#include "runtime/scene_bridge.h"

// Physics for entities: rigid bodies made from the voxels an entity links, stepped in FixedUpdate.
//
//     projv::runtime::installSceneBridge(app);
//     projv::runtime::installPhysics(app);
//     // entities.json:  "physics.rigidbody": { "v": 1, "motion": "dynamic", "density": 600 }
//     //                 "physics.static":    { "v": 1 }            -- the floor; nothing is solid unasked
//     projv::runtime::addImpulse(app.world, crate, {0, 40, 0});
//
// **Nothing collides that was not asked to.** A RigidBody (something that moves, or is moved) or a
// StaticCollider (the floor, a wall) on an entity is what makes it solid; installing physics changes
// nothing in a scene until a file says so.
//
// **The shape is the linked component's voxels** -- every chunk under it, grid cells included,
// merged into boxes (utils::buildCollisionPieces) and shared by every body made from the same blob
// content. A RigidBody may ask for a sphere, box or capsule instead, fitted to the voxels' bounds
// unless sized outright: steadier and cheaper where the shape allows it (a ball).
//
// **One writer per pose.** A dynamic body is moved by the simulation, and the simulation writes its
// Transform: patching that Transform does not move the body (and says so in the log, once) -- use
// teleport. A kinematic body follows its Transform, pushing what it meets. A static collider stays
// where its component is.
//
// **Everything happens at tick boundaries.** Bodies are made at the start of the next fixed step
// after their component appears, in entity order; commands (impulses, velocities, teleports, gravity)
// apply at the start of the next fixed step, in the order they were issued; destroyed entities' bodies
// go then too. So the simulation is a function of the commands and the ticks they landed on, which
// is what replays and networking build on (see the physics plan).
//
// **What is drawn is interpolated.** The simulation runs at Time::fixedDelta; in PostUpdate, before
// the Scene bridge, each moving body's Transform is set between its last two fixed poses by
// Time::fixedAlpha. Gameplay that needs where a body *is* reads bodyPose.
namespace projv {
    // A body that moves: dynamic (by the simulation) or kinematic (by its Transform).
    struct RigidBody {
        enum class Shape : uint8_t { Voxels, Sphere, Box, Capsule };
        enum class Quality : uint8_t {
            Auto,        // continuous collision for small bodies (under a metre across), discrete otherwise
            Discrete,
            Continuous
        };
        runtime::MotionType   motion = runtime::MotionType::Dynamic;
        Shape                 shape = Shape::Voxels;
        // Primitive sizes; 0 fits them to the voxels' content box (a sphere to its largest half
        // extent, a capsule upright through it).
        float                 radius = 0.0f;
        core::vec3            halfExtents{0.0f};
        float                 halfHeight = 0.0f;
        float                 density = 1000.0f;   // kg/m^3
        float                 mass = 0.0f;         // > 0 overrides density
        float                 friction = 0.6f;
        float                 restitution = 0.0f;    // bounce is asked for, not assumed
        float                 linearDamping = 0.05f;
        float                 angularDamping = 0.05f;
        float                 gravityFactor = 1.0f;
        runtime::PhysicsLayer layer = runtime::PhysicsLayer::Moving;
        Quality               quality = Quality::Auto;
    };

    // Solid, immovable geometry: the linked component's voxels.
    struct StaticCollider {
        float                 friction = 0.6f;
        float                 restitution = 0.0f;
        runtime::PhysicsLayer layer = runtime::PhysicsLayer::Static;
        // Whether pieces cut loose by an edit fall away as bodies of their own (destruction, M5 of
        // the physics plan). Saved, not acted on yet.
        bool                  splits = false;
    };

    // One chunk's part of a voxel body: which chunk, the shared shape built from it, and where it
    // sits in the body's frame. What a query hit is traced back to a voxel through.
    struct PhysicsVoxelPart {
        ChunkHandle                chunk = 0;
        runtime::CollisionShapeRef shape;
        core::vec3                 position{0.0f};
        core::quat                 rotation{1.0f, 0.0f, 0.0f, 0.0f};
        float                      voxelSize = 1.0f;
    };

    // The simulation's side of a RigidBody or StaticCollider. Made and removed by physics; never
    // saved, never written by anything else.
    struct PhysicsBody {
        runtime::BodyId id;
        runtime::MotionType motion = runtime::MotionType::Static;
        // The last two fixed poses, which presentation interpolates between.
        core::vec3 previousPosition{0.0f}, position{0.0f};
        core::quat previousRotation{1.0f, 0.0f, 0.0f, 0.0f}, rotation{1.0f, 0.0f, 0.0f, 0.0f};
        // The pose last written to Transform, so a body at rest is not rewritten every frame.
        core::vec3 presentedPosition{0.0f};
        core::quat presentedRotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool       presented = false;
        // The voxels the shape was built from: each chunk and its blob's content stamp. When they
        // differ from what the Scene holds now, the shape is rebuilt at the next step.
        std::vector<std::pair<ChunkHandle, uint64_t>> sources;
        // The shape's voxel parts, in the order the shape holds them; empty for a primitive.
        std::vector<PhysicsVoxelPart> voxelParts;
    };

    // A walking character (runtime::CharacterDesc): an upright capsule, its Transform's position at
    // its feet, moved by input (runtime::setCharacterInput) rather than by forces. Physics writes the
    // Transform's position; its rotation (where the character faces) is the game's.
    struct CharacterController {
        float                 radius = 0.3f;
        float                 height = 1.8f;
        float                 maxSlopeDegrees = 50.0f;
        float                 mass = 70.0f;
        float                 strength = 300.0f;     // N: how hard it pushes what it walks into
        float                 stepHeight = 0.4f;
        runtime::PhysicsLayer layer = runtime::PhysicsLayer::Character;
    };

    // The simulation's side of a CharacterController. Made and removed by physics; never saved.
    struct PhysicsCharacter {
        runtime::CharacterId id;
        core::vec3 previousPosition{0.0f}, position{0.0f};
        core::vec3 presentedPosition{0.0f};
        bool       presented = false;
    };

    // A joint (runtime::JointDesc) between two bodies, named by NetId (0 for `b`: the world), on an
    // entity of its own. Made at the first fixed step at which both bodies exist, placed in world
    // space as they are then. Saved in entities.json like any component, by NetId -- so the bodies it
    // joins need NetIds; runtime::connectBodies gives them one.
    struct Joint {
        runtime::JointType type = runtime::JointType::Point;
        NetId      a, b;
        core::vec3 anchor{0.0f}, anchorB{0.0f};
        core::vec3 axis{0.0f, 1.0f, 0.0f};
        float      limitMin = 0.0f, limitMax = 0.0f;
        float      springFrequency = 0.0f, springDamping = 0.0f;
        float      friction = 0.0f;
        bool       motor = false;
        float      motorVelocity = 0.0f, motorLimit = 0.0f;
        float      breakForce = 0.0f, breakTorque = 0.0f;
    };

    // The simulation's side of a Joint. `broken` once it has broken or lost a body: it is not made
    // again (remove and re-add the Joint for a new one).
    struct PhysicsJoint {
        runtime::JointId id;
        bool broken = false;
    };

    // ---- Events ---------------------------------------------------------------------------------

    // An entity asked for a body and could not have one; the reason is in the message and the log.
    struct PhysicsBodyRefused { Entity entity; std::string reason; };

    // A body went outside PhysicsSettings::worldMin/worldMax -- thrown over a wall and falling, say.
    // With PhysicsConfig::destroyBodiesThatLeaveTheWorld (the default) its entity is destroyed in
    // the same step, so `entity` is no longer valid when this arrives; `position` is where it was.
    struct BodyLeftWorld { Entity entity; core::vec3 position; };

    // A dynamic body's voxels were cut into separate pieces by an edit, and each piece cut loose is
    // now a body of its own: a new entity, linked to a new top-level component, with the parent's
    // RigidBody (its mass shared out by voxel count, if it named one), a NetId if the parent had one,
    // and the parent's motion at that point. The parent keeps the largest piece. Gameplay copies what
    // else the pieces should carry.
    struct BodySplit { Entity parent; std::vector<Entity> pieces; };

    // An edit took away every voxel a body was made from: it has no body any more, and nothing is
    // drawn for it. The entity stays; whether it goes (it usually should) is the game's call.
    struct BodyEmptied { Entity entity; };

    // A joint is gone: it broke under more force than its breakForce or breakTorque, or a body it
    // joined was destroyed. The entity and its Joint stay; `force` and `torque` are what it held last.
    struct JointBroke { Entity entity; bool bodyGone; float force; float torque; };

    // Time dropped fixed steps (Time::maxFixedStepsPerFrame) because the frames could not keep up:
    // the simulation is running slower than real time. `droppedSteps` is the total so far.
    struct SimulationSlow { uint64_t droppedSteps; };
}

namespace projv::runtime {
    struct PhysicsConfig {
        PhysicsSettings        settings;
        utils::CollisionParams collision;
        // A body that leaves the world bounds is falling forever or flung away; by default its
        // entity goes (after BodyLeftWorld is sent). False leaves the decision to the game.
        bool                   destroyBodiesThatLeaveTheWorld = true;
    };

    // Creates the physics world (in world.ctx()), registers RigidBody and StaticCollider with
    // entities.json, and adds the two systems: "physics" in FixedUpdate and "physics: present" in
    // PostUpdate, ordered before "scene bridge". Needs the Scene bridge for anything voxel-shaped.
    // Idempotent.
    void installPhysics(Application& app, const PhysicsConfig& config = {});

    // The simulation itself, for what the calls below do not cover. Mutating it directly bypasses
    // the tick ordering everything else here keeps, so a direct change lands mid-frame rather than
    // at a tick boundary, and a replay will not see it.
    PhysicsWorld&       physicsWorld(World& world);
    const PhysicsWorld& physicsWorld(const World& world);

    // ---- Commands: applied at the start of the next fixed step, in the order issued -------------
    // An entity with no body by then (not physical, refused, destroyed) is skipped.
    void addImpulse(World& world, Entity entity, core::vec3 impulse);
    void addAngularImpulse(World& world, Entity entity, core::vec3 impulse);
    void addVelocity(World& world, Entity entity, core::vec3 linear);   // whatever the mass
    void setVelocity(World& world, Entity entity, core::vec3 linear, core::vec3 angular = core::vec3(0.0f));
    // Moves a body without sweeping it; velocity kept. Its Transform follows, uninterpolated.
    void teleport(World& world, Entity entity, core::vec3 position, core::quat rotation);
    void setGravity(World& world, core::vec3 gravity);
    // What a character does from the next step on, until changed: walk at `velocity` (horizontal
    // only), and jump once at `jumpSpeed` the next time it is standing if `jump`. An input is a
    // command like any other -- stamped, ordered, and sent by NetId when it comes from elsewhere.
    void setCharacterInput(World& world, Entity entity, core::vec3 velocity, bool jump = false, float jumpSpeed = 5.0f);
    // A joint entity between `a` and `b` (NullEntity: the world), giving each a NetId if it has none.
    // `joint`'s a and b are filled in from them.
    Entity connectBodies(World& world, Entity a, Entity b, Joint joint);
    // Turns a hinge's or slider's motor on or off, at `velocity`, from the next step.
    void setJointMotor(World& world, Entity joint, bool on, float velocity);
    // What the joint is doing, as of the last step (alive false if not made yet, or gone).
    JointState jointState(const World& world, Entity joint);

    // Where the character is, as of the last step: feet, velocity, and what it stands on (groundBody
    // an id the queries' entities resolve). Default for an entity with no character.
    CharacterState characterState(const World& world, Entity entity);
    // Voxel edits to the component `entity` links (see voxelComponentOf), applied at the start of the next fixed step
    // (utils::queueVoxelAdd/Remove + updateScene), in the order issued -- so every peer edits on
    // the same step. Edits made directly through utils work too; the physics notices them at the
    // next step either way, rebuilding shapes and splitting what was cut loose.
    void editVoxels(World& world, Entity entity, const std::vector<PendingVoxelOp>& ops);

    // The component voxel edits to `entity` go to: the one it links when that is a chunk or a grid,
    // or the single chunk inside the asset it links (a prefab). INVALID for anything else.
    ComponentHandle voxelComponentOf(const World& world, Entity entity);
    // Removal ops for every voxel of that component whose centre lies within `radius` of the world
    // point `centre` -- what a blast carves. Coordinates in the component's own voxel space, ready
    // for editVoxels. Empty if the entity has no voxel component.
    std::vector<PendingVoxelOp> voxelsInSphere(const World& world, Entity entity, core::vec3 centre, float radius);

    // ---- Commands from elsewhere: a server, a peer, a replay --------------------------------------
    // The same commands, named by NetId rather than Entity, and stamped with the fixed step they
    // belong to. A command applies just before the step that takes PhysicsWorld::tick() to `tick`,
    // and the commands of a step apply in (tick, issuer, sequence) order -- never in the order they
    // happened to arrive. Local commands (the calls above) are stamped for the next step with
    // issuer 0 and a running sequence. One stamped for a step already run applies at the next one,
    // and is counted (lateCommands): a networked game that sees them is falling behind.
    struct PhysicsCommand {
        // CharacterInput: `a` is the walking velocity, `jump` asks for a jump at `b.x` m/s.
        // JointMotor: `jump` turns it on or off, `a.x` is the velocity; `target` names the joint's entity.
        enum class Kind : uint8_t { Impulse, AngularImpulse, AddVelocity, SetVelocity, Teleport, Gravity, CharacterInput, JointMotor };
        Kind       kind = Kind::Impulse;
        NetId      target;                  // ignored for Gravity
        core::vec3 a{0.0f}, b{0.0f};        // impulse / velocity / position; b: angular velocity
        core::quat q{1.0f, 0.0f, 0.0f, 0.0f};   // Teleport
        uint64_t   tick = 0;
        uint16_t   issuer = 0;
        uint32_t   sequence = 0;
        bool       jump = false;
    };
    void submitCommand(World& world, const PhysicsCommand& command);
    // The step a command issued now lands on.
    uint64_t nextPhysicsTick(const World& world);
    uint64_t lateCommands(const World& world);

    // ---- State, for comparing and for rewinding ---------------------------------------------------
    // A hash of every body's pose and velocities, keyed by NetId (bodies without one follow, in
    // entity order): two peers with the same NetIds and the same simulation hash the same, whatever
    // their local ids are.
    uint64_t physicsStateHash(const World& world);
    // Everything the physics runtime would need to rewind to now: the simulation, each body's
    // presentation poses, and the commands waiting for later steps. Restore needs the same bodies
    // (by NetId, or entity for those without), and changes nothing when it refuses.
    std::vector<uint8_t> savePhysics(const World& world);
    bool restorePhysics(World& world, const std::vector<uint8_t>& snapshot);

    // ---- Queries: against the simulation as of the last fixed step ----------------------------------
    // What a query struck. `normal` is the collision shape's (see PhysicsHit). For a voxel body the
    // hit is traced to the voxel: `chunk` and `voxel` (in that chunk's own voxel space; a grid's
    // cells are chunks of their own), the `component` that chunk belongs to, and its palette slot.
    struct QueryHit {
        Entity          entity = NullEntity;
        NetId           netId;
        core::vec3      position{0.0f};
        core::vec3      normal{0.0f};
        float           distance = 0.0f;
        bool            onVoxel = false;
        ComponentHandle component = INVALID_COMPONENT_HANDLE;
        ChunkHandle     chunk = 0;
        core::ivec3     voxel{0};
        uint8_t         materialSlot = 0;
    };
    // `layers`: a bit per PhysicsLayer (QueryFilter::only). `ignore`: an entity whose body to skip.
    std::optional<QueryHit> castRay(const World& world, core::vec3 origin, core::vec3 direction, float maxDistance,
                                    uint32_t layers = 0xffffffffu, Entity ignore = NullEntity);
    std::optional<QueryHit> castSphere(const World& world, core::vec3 origin, float radius, core::vec3 direction,
                                       float maxDistance, uint32_t layers = 0xffffffffu, Entity ignore = NullEntity);
    // Entities whose bodies overlap the shape: NetId order, then entity order for the rest.
    std::vector<Entity> overlapSphere(const World& world, core::vec3 centre, float radius,
                                      uint32_t layers = 0xffffffffu, Entity ignore = NullEntity);
    std::vector<Entity> overlapBox(const World& world, core::vec3 centre, core::vec3 halfExtents, core::quat rotation,
                                   uint32_t layers = 0xffffffffu, Entity ignore = NullEntity);

    // Where the simulation has the body: the end of the last fixed step, not the interpolated pose
    // that is drawn. Default (and `awake` false) for an entity with no body.
    BodyState bodyPose(const World& world, Entity entity);
    bool      hasBody(const World& world, Entity entity);
}

// ---- Saved in entities.json --------------------------------------------------------------------

template<> struct projv::runtime::ComponentTraits<projv::RigidBody> {
    static constexpr const char* key = "physics.rigidbody";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::RigidBody& body);
    static std::optional<projv::RigidBody> load(const nlohmann::json& json, uint32_t fileVersion);
};

template<> struct projv::runtime::ComponentTraits<projv::CharacterController> {
    static constexpr const char* key = "physics.character";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::CharacterController& c);
    static std::optional<projv::CharacterController> load(const nlohmann::json& json, uint32_t fileVersion);
};

template<> struct projv::runtime::ComponentTraits<projv::Joint> {
    static constexpr const char* key = "physics.joint";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::Joint& j);
    static std::optional<projv::Joint> load(const nlohmann::json& json, uint32_t fileVersion);
};

template<> struct projv::runtime::ComponentTraits<projv::StaticCollider> {
    static constexpr const char* key = "physics.static";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::StaticCollider& collider);
    static std::optional<projv::StaticCollider> load(const nlohmann::json& json, uint32_t fileVersion);
};

#endif
