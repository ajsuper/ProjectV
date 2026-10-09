#ifndef PROJECTV_RUNTIME_PHYSICS_H
#define PROJECTV_RUNTIME_PHYSICS_H

#include <optional>
#include <string>

#include "nlohmann/json.hpp"
#include "core/application.h"
#include "core/world.h"
#include "runtime/entities.h"
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
    };

    // ---- Events ---------------------------------------------------------------------------------

    // An entity asked for a body and could not have one; the reason is in the message and the log.
    struct PhysicsBodyRefused { Entity entity; std::string reason; };

    // A body went outside PhysicsSettings::worldMin/worldMax -- thrown over a wall and falling, say.
    // With PhysicsConfig::destroyBodiesThatLeaveTheWorld (the default) its entity is destroyed in
    // the same step, so `entity` is no longer valid when this arrives; `position` is where it was.
    struct BodyLeftWorld { Entity entity; core::vec3 position; };

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

template<> struct projv::runtime::ComponentTraits<projv::StaticCollider> {
    static constexpr const char* key = "physics.static";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::StaticCollider& collider);
    static std::optional<projv::StaticCollider> load(const nlohmann::json& json, uint32_t fileVersion);
};

#endif
