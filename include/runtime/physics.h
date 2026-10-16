#ifndef PROJECTV_RUNTIME_PHYSICS_H
#define PROJECTV_RUNTIME_PHYSICS_H

#include <optional>
#include <string>
#include <vector>

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
        // On the sensor layer, the body is a sensor (a trigger volume that moves): it is collided with
        // by nothing, and reports what enters and leaves (ContactBegan). These are the layers it
        // senses, runtime::layerBit()s; "senses" in the file, by name. Static: only a kinematic sensor.
        uint32_t              senses = runtime::layerBit(runtime::PhysicsLayer::Moving) |
                                       runtime::layerBit(runtime::PhysicsLayer::Character);
        // A kinematic body's: whether static geometry stops it (runtime::KinematicBlocking) -- by
        // default nothing does, and it goes through floors and walls. Stop ends a move where it
        // touches; Slide carries the rest of the move on along the surface. "blockedByStatic":
        // "stop" or "slide" in the file. The simulation then writes its Transform, as for a dynamic
        // body: a Transform the game sets is where it heads for this step, and one left as physics
        // wrote it holds it where it is -- so set where it should go, or fly it on a
        // ConstantVelocity, rather than adding to the Transform drawn (that is a step behind). Not
        // for a Part, whose pose the Scene owns.
        runtime::KinematicBlocking blockedByStatic = runtime::KinematicBlocking::None;
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
        // face-connected island of them with nothing to hold it up becomes a dynamic body of its own
        // (BodyBroken), of this collider's friction, restitution, material and breaksOnImpact. What
        // holds up is data: voxels of an anchor material (PhysicsMaterial::anchor), and with
        // anchorBase the component's own y = 0 and below. Off by default (the physics plan, D4):
        // floating terrain falling is a choice. Checked only after a change. A collider with no
        // anchor of either kind drops nothing, and says so.
        bool                  splits = false;
        // The component's own y = 0 and below holds it up (build a wall standing on its y = 0): for a
        // collider with no anchor material under it.
        bool                  anchorBase = false;
        // As RigidBody::material: friction and restitution from the library when set.
        std::string           material;
        // As RigidBody::breaksOnImpact. The pieces that fall when support goes (splits) take it on.
        bool                  breaksOnImpact = false;
        // As RigidBody::senses, on the sensor layer: a trigger volume that stays put. It senses a body
        // as it comes in moving; one that falls asleep inside stays sensed.
        uint32_t              senses = runtime::layerBit(runtime::PhysicsLayer::Moving) |
                                       runtime::layerBit(runtime::PhysicsLayer::Character);
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

    // Which bodies this entity's body (RigidBody, StaticCollider or CharacterController) passes
    // through, beyond what layers say (runtime::CollisionFilter): the named groups it is in, the
    // named groups it passes through, and a family -- bodies of the same nonzero family never
    // collide. Group names are defined by the game (runtime::definePhysicsGroup); a name not defined
    // is left out, with a warning once. Changing it changes the body's filter at the next fixed step,
    // without rebuilding the body; removing it puts the body back in no group. Pieces that come away
    // from a body copy its CollisionGroups, as they copy its RigidBody.
    struct CollisionGroups {
        std::vector<std::string> groups;
        std::vector<std::string> ignores;
        uint32_t                 family = 0;
    };

    // Report when this entity's body (RigidBody, StaticCollider or CharacterController) begins and stops
    // touching another (ContactBegan, ContactEnded). Asked for, as everything is: without it a body
    // reports nothing -- except a sensor, which reports whatever touches it, and takes `only` from
    // this when it has one. `only`: collision group names (definePhysicsGroup); when set, only
    // contacts with a body in one of them are reported -- a pressure plate that counts players.
    // Changing `only` applies to contacts that begin after. Pieces of a body do not copy it.
    // A character's contacts are its inner body's: moving bodies and sensors, not the static ground
    // it walks on (characterState says what it stands on).
    struct ContactReports {
        std::vector<std::string> only;
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

    // Why a body came apart (BodyBroken): an edit cut a moving body in pieces, a static collider
    // lost the support of what fell (StaticCollider::splits), or a strike broke it -- one a game
    // made (runtime::fracture), or an impact's (installImpactFracture).
    enum class BreakCause : uint8_t { Edit, Support, Strike, Impact };

    // A voxel body came apart, and `pieces` came away from it. Whatever the cause, it is one pass,
    // run after a body's voxels change: its face-connected islands are found across all its chunks,
    // and each stays or comes away -- a moving body keeps its largest; a static collider that splits
    // keeps what an anchor holds up; a struck body's shards (the groups its material's fracture
    // pattern made) always come away. Each piece is a new entity linked to a new top-level component
    // holding exactly those voxels, with their palette entries, a NetId if the body had one, a
    // Piece, and a RigidBody that takes after the body: a moving body's own (its mass shared out by
    // voxel count, if it named one), or one of a static collider's friction, restitution, material
    // and breaksOnImpact. A piece moves as that part of the body was, plus a strike's push.
    // `entity` keeps the rest; it is rebuilt.
    //
    // Delivered at once, inside the physics step, before the pieces' bodies are made: a handler can
    // shape them in the step that made them -- change a piece's RigidBody (its layer, damping,
    // breaksOnImpact) or CollisionGroups (one family for a break's shards, so they pass through each
    // other), set the velocity it starts with (setVelocity), or destroy it. Like everything
    // that feeds a step, what a handler does must be the same on every peer.
    struct BodyBroken {
        Entity              entity;
        // A strike's shards first (the first `shards` of them), then the islands that came away.
        std::vector<Entity> pieces;
        size_t              shards = 0;
        BreakCause          cause = BreakCause::Edit;
        // A strike's: where, what struck (Strike::striker; NullEntity for none), and its energy, J
        // (0 for a strike by reach). Zero for an edit or support.
        core::vec3          at{0.0f};
        Entity              striker = NullEntity;
        float               energy = 0.0f;
    };

    // What a break made (BodyBroken): a shard, or an island that came away. Physics puts it on every
    // piece, with the step it was made, and clears the oldest when there are more than
    // PhysicsConfig::maxPieces. Remove it from a piece to keep that piece for good. Not saved.
    struct Piece { uint64_t madeAt = 0; };

    // The piece budget is about to clear `entities`, the oldest pieces, in the order it chose them
    // (PhysicsConfig::maxPieces). Delivered at once, as BodyBroken is, so a handler sees them whole:
    // it can take Piece off one to keep it, or put something in its place. Every entity still with
    // Piece after the handlers is destroyed. Handlers run inside the physics step.
    struct PiecesCleared { std::vector<Entity> entities; };

    // Two entities' bodies began touching (one per pair, however many parts touch), where one asked
    // (ContactReports) or is a sensor. `a` and `b` in the order every peer agrees on (NetId, then
    // entity); `point` where they first touched and `normal` there, from a towards b. Queued: it
    // arrives after the step, like the events below; a FixedUpdate system after "physics" can read
    // the step's own with runtime::contacts.
    struct ContactBegan {
        Entity     a = NullEntity, b = NullEntity;
        core::vec3 point{0.0f};
        core::vec3 normal{0.0f, 1.0f, 0.0f};
        bool       sensor = false;   // either is a sensor
    };
    // They stopped: they parted, or one was destroyed (so `a` or `b` may no longer be valid), or the
    // one that asked stopped asking. Every ContactBegan has its ContactEnded. Asleep touching is
    // still touching: a crate that settles on a pressure plate stays on it.
    struct ContactEnded {
        Entity a = NullEntity, b = NullEntity;
        bool   sensor = false;
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
        // The most pieces (Piece) there may be at once. Past it, the oldest go -- their entities
        // destroyed, at the step that made the newest -- in (madeAt, NetId, entity) order, so peers
        // clear the same ones; PiecesCleared names them first. Thousands of pieces are thousands of
        // bodies: what they cost is the step's. 0: no limit.
        uint32_t               maxPieces = 3000;
        // The smallest piece: an island of this many voxels or fewer that would come away is removed
        // instead, as dust -- no body, no entity. The same for shards and fallen pieces. 0: none.
        uint32_t               dustVoxels = 0;
        // Guards on a strike (runtime::fracture), like maxLinearSpeed on motion: what a game may
        // change, never what it may turn off. Out of range, installPhysics clamps them and says so.
        struct FractureLimits {
            // How far from the strike point, in voxels, the voxel it struck may be (0.5 .. 16).
            float    searchVoxels = 3.0f;
            // The most a reach may be, in voxels of the struck body (1 .. 256): a bigger one is cut
            // to this, and logged.
            float    maxReachVoxels = 64.0f;
            // The most shards one strike makes (1 .. 4096); the rest of what would break stays.
            uint32_t maxShards = 512;
            // The least an impact breaks (Strike::cause Impact), in shard sizes of the struck
            // material (0.5 .. 16): a hit that would break less breaks nothing, and a body that fits
            // in a cube of this many shard sizes is not broken by a hit at all. What keeps pieces
            // that break on impact from cascading: every break makes smaller pieces, and one below
            // the least break cannot be broken by a hit.
            float    minImpactBreak = 1.0f;
        } fracture;
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

    // ---- Collision groups ---------------------------------------------------------------------------
    // Names for CollisionFilter's 32 group bits, which CollisionGroups and queries name. Bits follow
    // definition order, so peers that define the same names in the same order agree on them (the
    // names are part of the physics state hash). Defining a name again returns its bit. Entities that
    // named a group before it was defined take it on at the next fixed step.
    // The group's bit, or 0 when refused (logged): a bad name (1 to 64 characters), all 32 taken.
    uint32_t definePhysicsGroup(World& world, const std::string& name);
    // 0 when there is none of that name.
    uint32_t physicsGroup(const World& world, const std::string& name);

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
        // Metres: the size this strike breaks to, instead of the struck material's shardSize -- so a
        // game can break a small thing finer than a big one. 0: the material's.
        float shardSize = 0.0f;
        // Reported in BodyBroken. Impact (what installImpactFracture strikes with) is also held to
        // PhysicsConfig::fracture.minImpactBreak.
        BreakCause cause = BreakCause::Strike;
        // What struck, reported in BodyBroken; NullEntity for none.
        Entity striker = NullEntity;
        // When the struck body does not move and the striker is a dynamic body: the striker's velocity
        // is set to `strikerVelocity` times the share of the energy the break did not use (breaking a
        // voxel costs its material's strength times its volume), square-rooted -- it carries on through.
        bool returnUnusedEnergy = false;
        core::vec3 strikerVelocity{0.0f};
    };

    // Breaks the voxel body `entity` at the start of the next fixed step, in order with editVoxels.
    // The voxel nearest `strike.at` (within PhysicsConfig::fracture.searchVoxels, three by default) is
    // what was struck; its physics material's
    // strength turns the energy into a reach, and its fracture pattern sets which voxels go together
    // (runtime/fracture.h). Voxels whose material has no strength never break; neither does anything
    // when the struck one's has none. Each voxel breaks only within the reach the energy has in its
    // own material, so something stronger than what was struck -- a steel bar in concrete -- breaks
    // only nearer the strike (a strike by reach breaks everything breakable within it). The shards
    // come away through the same pass as any island (BodyBroken), taking after the body as every
    // piece does: a dynamic body's shards copy its whole RigidBody (damping, gravity factor, layer,
    // breaksOnImpact), a static collider's take its friction, restitution, material and
    // breaksOnImpact; both copy its CollisionGroups. Static colliders break too; their shards are
    // dynamic.
    //
    // Works on what voxelComponentOf finds: a chunk, a grid, or a prefab of one chunk. The reach
    // is capped (PhysicsConfig::fracture.maxReachVoxels, 64 by default), and so is the number of
    // shards (maxShards, 512).
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

    // A contact of the last physics step that began or ended (ContactBegan, ContactEnded), in the
    // order every peer agrees on. Replaced at every step; for a FixedUpdate system after "physics".
    struct BodyContact {
        Entity     a = NullEntity, b = NullEntity;
        bool       began = true;
        bool       sensor = false;
        core::vec3 point{0.0f};                   // began only
        core::vec3 normal{0.0f, 1.0f, 0.0f};
    };
    std::vector<BodyContact> contacts(const World& world);
    // The entities `entity` is touching now, among the contacts reported (ContactReports, sensors):
    // what is on a pressure plate, in a trigger.
    std::vector<Entity> touching(const World& world, Entity entity);

    // Breaking on impact: what installImpactFracture does with each impact. The defaults are what the
    // demos and tests were built with.
    struct ImpactFractureConfig {
        // When what was struck does not move, the striker gets back the energy the break did not use,
        // along the way it was going: a boulder that breaks a hole in a wall carries on through it.
        bool  returnUnusedEnergy = true;
        // The shards take on the striker's motion at the contact, fading to the struck body's own
        // (from before the hit) at the edge of the break: what is in a boulder's way goes on ahead of
        // it, and a brick that lands hard stops where it hit and falls on further out. Off, they start
        // with the struck body's motion from before the hit.
        bool  shardsTakeStrikerMotion = true;
    };
    // Adds "physics: impact fracture" to FixedUpdate, after "physics": every impact on a voxel body
    // with breaksOnImpact (RigidBody, StaticCollider) and a material that can break strikes it at the
    // contact, at the next step, with the collision's energy (BreakCause::Impact, so held to
    // PhysicsConfig::fracture.minImpactBreak: a crate set down on a stone floor does not chip it). Built only on impacts() and fracture(),
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
    // (by NetId, or entity for those without), and changes nothing when it refuses. So it does not
    // rewind across a break, an edit that splits, or anything else that makes or removes bodies --
    // by design (the physics plan, D9): the world is the server's, and a client predicts and
    // rewinds only what is its own.
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
    // `layers`: a bit per PhysicsLayer (QueryFilter::only). `ignore`: an entity whose body to skip --
    // a character's too, so a ray from its eyes does not stop inside it.
    // `skipGroups`: group bits (physicsGroup) whose bodies to skip -- a bullet through shards.
    std::optional<QueryHit> castRay(const World& world, core::vec3 origin, core::vec3 direction, float maxDistance,
                                    uint32_t layers = 0xffffffffu, Entity ignore = NullEntity, uint32_t skipGroups = 0);
    std::optional<QueryHit> castSphere(const World& world, core::vec3 origin, float radius, core::vec3 direction,
                                       float maxDistance, uint32_t layers = 0xffffffffu, Entity ignore = NullEntity,
                                       uint32_t skipGroups = 0);
    // Entities whose bodies overlap the shape: NetId order, then entity order for the rest.
    std::vector<Entity> overlapSphere(const World& world, core::vec3 centre, float radius,
                                      uint32_t layers = 0xffffffffu, Entity ignore = NullEntity, uint32_t skipGroups = 0);
    std::vector<Entity> overlapBox(const World& world, core::vec3 centre, core::vec3 halfExtents, core::quat rotation,
                                   uint32_t layers = 0xffffffffu, Entity ignore = NullEntity, uint32_t skipGroups = 0);

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

template<> struct projv::runtime::ComponentTraits<projv::CollisionGroups> {
    static constexpr const char* key = "physics.groups";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::CollisionGroups& g);
    static std::optional<projv::CollisionGroups> load(const nlohmann::json& json, uint32_t fileVersion);
};

template<> struct projv::runtime::ComponentTraits<projv::ContactReports> {
    static constexpr const char* key = "physics.contacts";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::ContactReports& r);
    static std::optional<projv::ContactReports> load(const nlohmann::json& json, uint32_t fileVersion);
};

template<> struct projv::runtime::ComponentTraits<projv::StaticCollider> {
    static constexpr const char* key = "physics.static";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::StaticCollider& collider);
    static std::optional<projv::StaticCollider> load(const nlohmann::json& json, uint32_t fileVersion);
};

#endif
