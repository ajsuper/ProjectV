#ifndef PROJECTV_RUNTIME_PHYSICS_H
#define PROJECTV_RUNTIME_PHYSICS_H

#include <optional>
#include <string>

#include "nlohmann/json.hpp"
#include "core/application.h"
#include "core/world.h"
#include "runtime/entities.h"
#include "runtime/fracture.h"
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
        // A name in the physics material library (runtime::definePhysicsMaterial). When set, its
        // density, friction and restitution are the body's instead of the three fields above
        // (`mass` still overrides): the material of a primitive, and of every voxel whose palette
        // entry names none. A palette entry that names one wins for its voxels.
        std::string           material;
        // Whether a hit can break its voxels (see runtime::fracture: "On impact"). Asked for, not
        // assumed: a material with a strength breaks only where a strike is made, and this says an
        // impact may make one. Only voxels whose material has a strength break at all; shards a
        // fracture throws off have this off, so debris does not shatter again on landing.
        bool                  breaksOnImpact = false;
    };

    // A kinematic body that flies on by itself: each physics step moves it `velocity` (m/s) and turns
    // it `spin` (rad/s, about world axes) from where the simulation has it. No gravity, no drag, and
    // nothing pushes it back -- it shoves whatever it meets aside. Needs a kinematic RigidBody that
    // isn't a Part; its Transform follows, as any moving body's does.
    struct ConstantVelocity {
        core::vec3 velocity{0.0f};
        core::vec3 spin{0.0f};
    };

    // Solid, immovable geometry: the linked component's voxels.
    struct StaticCollider {
        float                 friction = 0.6f;
        float                 restitution = 0.0f;
        runtime::PhysicsLayer layer = runtime::PhysicsLayer::Static;
        // Whether what an edit or a fracture leaves unsupported falls. After its voxels change, every
        // face-connected island of them that no longer reaches the component's ground -- its own
        // y = 0 and below, so build a wall standing on its y = 0 -- becomes a dynamic body of its
        // own (BodySplit), of this collider's friction, restitution and material. Off by default
        // (the physics plan, D4): floating terrain falling is a choice. Checked only after a change.
        bool                  splits = false;
        // As RigidBody::material: friction and restitution from the library when set.
        std::string           material;
        // As RigidBody::breaksOnImpact. The pieces that fall when support goes (splits) take it on.
        bool                  breaksOnImpact = false;
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
        // The physics materials the body was built with (its own, its palettes' and the library's
        // version), 0 for none. When it differs from what they say now, the body is rebuilt.
        uint64_t materialKey = 0;
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
        runtime::MotorMode motorMode = runtime::MotorMode::Velocity;
        float      motorVelocity = 0.0f, motorTarget = 0.0f, motorLimit = 0.0f, motorFrequency = 0.0f;
        float      breakForce = 0.0f, breakTorque = 0.0f;
        bool       collideConnected = false;   // see runtime::JointDesc
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
    // else the pieces should carry. Also sent when a static collider that splits loses support
    // (StaticCollider::splits): its pieces start at rest, made of what it was.
    struct BodySplit { Entity parent; std::vector<Entity> pieces; };

    // A strike broke a body (runtime::fracture): `shards` are the pieces it threw off, each a new
    // entity linked to a new top-level component holding exactly those voxels, with their palette
    // entries, given a RigidBody and moving as that part of the body was, plus the strike's push.
    // A dynamic body's RigidBody is theirs too (its mass shared out by voxel count, if it named one);
    // a static collider's shards take its friction, restitution and material. What is left of the
    // body is rebuilt, and pieces of it that the strike cut loose split as any edit's do (BodySplit).
    // A shard a fracture threw off: debris. Physics puts it on each shard (with the step it was made)
    // and clears the oldest debris when there is more than PhysicsConfig::maxDebris. Remove it from
    // a shard to keep that shard for good. Not saved.
    struct Debris { uint64_t madeAt = 0; };

    // `striker`: what hit it (Strike::striker: an impact's other body); NullEntity for none.
    // `energy`: the strike's, J (0 for a strike by reach).
    struct BodyFractured {
        Entity entity;
        std::vector<Entity> shards;
        core::vec3 at;
        Entity striker = NullEntity;
        float energy = 0.0f;
    };

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
        // The most shards (Debris) there may be at once. Past it, the oldest go -- their entities
        // destroyed, at the step that made the newest -- in (madeAt, NetId, entity) order, so peers
        // clear the same ones. Thousands of shards are thousands of bodies: what they cost is the
        // step's. 0: no limit.
        uint32_t               maxDebris = 3000;
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

    // ---- Physics materials ---------------------------------------------------------------------------
    // The project's library: named materials (stone, wood, ice) that palette entries
    // (Material::physics) and bodies (RigidBody::material) name. Many palette entries can name one
    // material -- three colours of granite are one stone to physics, and merge into the same boxes --
    // and two entries that look alike can name two. A body whose voxels mix materials weighs what
    // each part is made of, and each contact uses the material of the voxels touched.
    //
    // Indices follow definition order, so peers that load the same library agree on them; the
    // library is part of the physics state hash. Defining a name again changes it in place, and every
    // body using it is rebuilt at the next step.
    // The index (1..), or 0 when refused (logged): a bad name, unusable values, the library full.
    uint16_t definePhysicsMaterial(World& world, const std::string& name, const PhysicsMaterial& material);
    // 0 when there is none of that name.
    uint16_t physicsMaterialIndex(const World& world, const std::string& name);
    std::optional<PhysicsMaterial> physicsMaterial(const World& world, const std::string& name);
    // A library file, defined in file order:
    //     { "v": 1, "materials": [ { "name": "stone", "density": 2400, "friction": 0.8, "restitution": 0.1,
    //                                "strength": 2e5, "fracture": "shatter", "shardSize": 0.5 }, ... ] }
    // ("grain": [0, 1, 0] too, for "splinter").
    // Missing values are PhysicsMaterial's defaults. False (logged) if the file cannot be read or
    // parsed; materials before a bad entry stay defined.
    bool loadPhysicsMaterials(World& world, const std::string& path);

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
    // Turns a hinge's or slider's motor on or off from the next step: driving at `value` (rad/s or
    // m/s), or in Position mode holding `value` (rad or m) -- steering a wheel, say.
    void setJointMotor(World& world, Entity joint, bool on, float value,
                       runtime::MotorMode mode = runtime::MotorMode::Velocity);
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

    // A strike (runtime::fracture): where, how hard, and what becomes of the pieces. Every field but
    // `at` and one of `energy` / `reach` has a default that leaves it out.
    struct Strike {
        core::vec3 at{0.0f};   // world point
        // Joules: the reach is the radius whose sphere holds energy / strength cubic metres, of the
        // struck voxel's material.
        float energy = 0.0f;
        // Metres: when > 0, the reach itself, whatever the energy (runtime::fractureWithin) -- for a
        // game with its own model of how far a blow breaks.
        float reach = 0.0f;
        // m/s, at the strike, falling off to nothing at the edge of the break.
        core::vec3 push{0.0f};
        // The motion the shards start from, before `push`; unset, the body's own as it is now.
        std::optional<core::vec3> bodyVelocity;
        // In shard sizes of the struck material: a smaller break breaks nothing (a knock, not a break).
        float minimumBreak = 0.0f;
        // What struck, reported in BodyFractured; NullEntity for none.
        Entity striker = NullEntity;
        // When the struck body does not move and the striker is a dynamic body: the striker's velocity
        // is set to `strikerVelocity` times the share of the energy the break did not use (breaking a
        // voxel costs its material's strength times its volume), square-rooted -- it carries on through.
        bool returnUnusedEnergy = false;
        core::vec3 strikerVelocity{0.0f};
        // Whether the shards break on impact themselves; unset, as the body: a dynamic body's shards
        // copy its whole RigidBody (damping, gravity factor, layer and this too), a static collider's
        // take its friction, restitution, material and breaksOnImpact.
        std::optional<bool> shardsBreakOnImpact;
    };

    // Breaks the voxel body `entity` at the start of the next fixed step, in order with editVoxels.
    // The voxel nearest `strike.at` (within three voxels) is what was struck; its physics material's
    // strength turns the energy into a reach, and its fracture pattern sets which voxels go together
    // (runtime/fracture.h). Voxels whose material has no strength never break; neither does anything
    // when the struck one's has none. Each shard is a body of its own (BodyFractured) and debris
    // (Debris). Static colliders break too; their shards are dynamic.
    //
    // Works on what voxelComponentOf finds: a chunk, a grid, or a prefab of one chunk. The reach
    // is capped at 64 voxels.
    //
    // Nothing calls this on impact by itself: installImpactFracture adds the system that does.
    void fracture(World& world, Entity entity, const Strike& strike);
    // The same, with `energy` joules and `push`.
    void fracture(World& world, Entity entity, core::vec3 at, float energy, core::vec3 push = core::vec3(0.0f));
    // The same, breaking out to `radius` metres whatever the material's strength.
    void fractureWithin(World& world, Entity entity, core::vec3 at, float radius, core::vec3 push = core::vec3(0.0f));
    // Whether anything in the voxel body `entity` can break: its own material, or one its palette
    // names, has a strength. Cheap: no voxels are read.
    bool canBreak(World& world, Entity entity);

    // A collision of the last physics step, between two entities: one per pair, the fastest of its
    // contacts, closing faster than PhysicsSettings::minImpactSpeed.
    struct BodyImpact {
        Entity     a = NullEntity, b = NullEntity;
        core::vec3 point{0.0f};               // world space, between the two surfaces
        core::vec3 normal{0.0f, 1.0f, 0.0f};  // from a towards b
        float      speed = 0.0f;              // closing speed along the normal, m/s
        // What the collision can take out of the motion: half the pair's reduced mass times the
        // closing speed squared (the whole of the striker's when it strikes something static), J.
        float      energy = 0.0f;
        core::vec3 velocityA{0.0f}, velocityB{0.0f};   // each body's linear velocity, before
    };
    // The last step's impacts, in the order every peer agrees on. Read it from a FixedUpdate system
    // ordered after "physics": it is replaced at every step. What breaking on impact, sounds and
    // damage are built on.
    std::vector<BodyImpact> impacts(const World& world);

    // Breaking on impact: what installImpactFracture does with each impact. The defaults are what the
    // demos and tests were built with.
    struct ImpactFractureConfig {
        // In shard sizes of the struck material: an impact that would break less breaks nothing, so a
        // crate set down on a stone floor does not chip it.
        float minimumBreak = 1.0f;
        // When what was struck does not move, the striker gets back the energy the break did not use,
        // along the way it was going: a boulder that breaks a hole in a wall carries on through it.
        bool  returnUnusedEnergy = true;
        // The shards take on the striker's motion at the contact, fading to the struck body's own
        // (from before the hit) at the edge of the break: what is in a boulder's way goes on ahead of
        // it, and a brick that lands hard stops where it hit and falls on further out. Off, they start
        // with the struck body's motion from before the hit.
        bool  shardsTakeStrikerMotion = true;
        // Whether shards break again where they land: off, debris does not shatter on landing.
        bool  shardsBreakOnImpact = false;
    };
    // Adds "physics: impact fracture" to FixedUpdate, after "physics": every impact on a voxel body
    // with breaksOnImpact (RigidBody, StaticCollider) and a material that can break strikes it at the
    // contact, at the next step, with the collision's energy. Built only on impacts() and fracture(),
    // so a game can leave it out and write its own. Needs installPhysics first. Idempotent.
    void installImpactFracture(Application& app, const ImpactFractureConfig& config = {});

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
        // JointMotor: `jump` turns it on or off, `a.x` is the velocity or target, `b.x` the MotorMode;
        // `target` names the joint's entity.
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
