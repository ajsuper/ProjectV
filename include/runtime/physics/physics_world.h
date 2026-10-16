#ifndef PROJECTV_PHYSICS_WORLD_H
#define PROJECTV_PHYSICS_WORLD_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/math.h"
#include "utils/collision_geometry.h"

// The physics core: a Jolt simulation behind an interface that names no Jolt type.
//
//     projv::runtime::PhysicsWorld physics;
//     auto floor = physics.createBody({.shape = PhysicsShape::box({50, 1, 50}),
//                                      .motion = MotionType::Static, .layer = PhysicsLayer::Static});
//     auto crate = physics.createBody({.shape = PhysicsShape::box({1, 1, 1}), .position = {0, 10, 0}});
//     for (int i = 0; i < 600; i++) physics.step(1.0f / 60.0f);
//     physics.bodyState(crate).position;   // resting on the floor
//
// This is the layer the ECS side (RigidBody components, the command queue, FixedUpdate) is built
// on; see the physics plan. It is deliberately small, and three properties hold for everything in
// it, because the rest of the design rests on them:
//
// **Deterministic.** The same sequence of calls gives bit-identical results, on every platform
// Jolt's cross-platform-deterministic build supports. Nothing here reads a clock, a thread count
// or an iteration order that could differ between runs. stateHash() is how a test, a replay or two
// networked peers check it.
//
// **Restorable.** saveState() captures everything step() reads; restoreState() puts it back, and
// stepping on from there gives the same results as if the save had never happened. Body creation
// and destruction are not part of the snapshot: a restore needs the same set of bodies to exist as
// when it was saved, and refuses -- changing nothing -- when they do not.
//
// **Refuses bad input rather than simulating it.** A non-finite position, a shape with no size, a
// full body table: each is logged and refused (an invalid BodyId), never passed to the solver.
//
// Body ids, like ComponentHandles, are local to this process: never persist one or send one over a
// network. A destroyed body's id stays invalid even after its slot is reused (Jolt's ids carry a
// sequence number), so a stale id is always caught.
namespace projv::runtime {

    enum class MotionType : uint8_t {
        Static,     // never moves
        Kinematic,  // moved by the game; pushes dynamic bodies, is not pushed back
        Dynamic     // moved by the simulation
    };

    // What collides with what, coarsely: a fixed list, so the collision matrix is a few lines of code
    // rather than data. Everything collides with everything except that static ignores static, and
    // sensors ignore sensors. Which bodies pass through which beyond that is data: CollisionFilter,
    // and a sensor's BodyDesc::senses.
    //
    // A body on the Sensor layer is a sensor: nothing collides with it, and it reports what enters and
    // leaves it (PhysicsWorld::contactChanges) -- a trigger volume.
    enum class PhysicsLayer : uint8_t {
        Static,
        Moving,
        Sensor,
        Character,
        Count
    };
    bool layersCollide(PhysicsLayer a, PhysicsLayer b);
    constexpr uint32_t layerBit(PhysicsLayer layer) { return 1u << uint32_t(layer); }

    // Which bodies pass through which, finer than layers (the physics plan, R4), checked for each pair
    // the layers let meet. `groups` and `ignores` are bits, one per named group (a game names them:
    // runtime::definePhysicsGroup); two bodies pass through each other when either ignores a group
    // the other is in. And two bodies of the same nonzero `family` never collide: the shards of one
    // break, a ragdoll's limbs, a vehicle's parts -- as many families as a game likes, where groups
    // are 32. The default is in no group and no family, and collides as the layers say.
    struct CollisionFilter {
        uint32_t groups = 0;
        uint32_t ignores = 0;
        uint32_t family = 0;
        bool operator==(const CollisionFilter&) const = default;
    };
    constexpr uint32_t MAX_COLLISION_GROUPS = 32;
    bool filtersCollide(const CollisionFilter& a, const CollisionFilter& b);

    // A shape built from voxels (PhysicsWorld::voxelShape): the blob's collision pieces as one Jolt
    // shape, in the blob's voxel space. Shared: every body made from the same blob content uses the
    // same one, and holding a ref keeps it alive. Opaque; the fields are for diagnostics and tests.
    struct CollisionShape {
        uint32_t                 pieces = 0;
        float                    volume = 0.0f;   // in voxels (cubic voxel units)
        utils::CollisionFallback fallback = utils::CollisionFallback::None;
        // Some piece names a physics material: the shape carries each piece's density (its
        // material's, or otherDensity for material 0) and a body's mass comes from it.
        bool                     hasMaterials = false;
        float                    otherDensity = 1000.0f;
        // What it was built from, kept so a recording can carry the shape as data.
        std::shared_ptr<const utils::CollisionPieces> source;
        struct Native;                            // the Jolt shape; defined where Jolt is
        std::shared_ptr<Native>  native;
    };
    using CollisionShapeRef = std::shared_ptr<const CollisionShape>;

    struct PhysicsShapePart;

    struct PhysicsShape {
        enum class Kind : uint8_t { Box, Sphere, Capsule, Voxels, Compound };
        Kind       kind = Kind::Box;
        core::vec3 halfExtents{0.5f};   // Box
        float      radius = 0.5f;       // Sphere, Capsule
        float      halfHeight = 0.5f;   // Capsule: half the length of the cylinder part, along Y
        // Voxels: the shape, and the world size of one voxel (the chunk's voxel size times its
        // uniform scale). The shape's origin is the voxel space origin -- the chunk's corner.
        CollisionShapeRef voxels;
        float             voxelSize = 1.0f;
        // Compound: shapes placed in this one's space. Any kind may be a part, compounds included.
        std::vector<PhysicsShapePart> parts;

        static PhysicsShape box(core::vec3 halfExtents);
        static PhysicsShape sphere(float radius);
        static PhysicsShape capsule(float halfHeight, float radius);
        static PhysicsShape fromVoxels(CollisionShapeRef shape, float voxelSize);
        static PhysicsShape compound(std::vector<PhysicsShapePart> parts);

        // Cubic metres. What a body's mass is computed from, with its density.
        float volume() const;
    };

    struct PhysicsShapePart {
        PhysicsShape shape;
        core::vec3   position{0.0f};
        core::quat   rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    inline PhysicsShape PhysicsShape::box(core::vec3 h) {
        PhysicsShape s; s.kind = Kind::Box; s.halfExtents = h; return s;
    }
    inline PhysicsShape PhysicsShape::sphere(float r) {
        PhysicsShape s; s.kind = Kind::Sphere; s.radius = r; return s;
    }
    inline PhysicsShape PhysicsShape::capsule(float halfHeight, float r) {
        PhysicsShape s; s.kind = Kind::Capsule; s.halfHeight = halfHeight; s.radius = r; return s;
    }
    inline PhysicsShape PhysicsShape::fromVoxels(CollisionShapeRef shape, float voxelSize) {
        PhysicsShape s; s.kind = Kind::Voxels; s.voxels = std::move(shape); s.voxelSize = voxelSize; return s;
    }
    inline PhysicsShape PhysicsShape::compound(std::vector<PhysicsShapePart> parts) {
        PhysicsShape s; s.kind = Kind::Compound; s.parts = std::move(parts); return s;
    }

    // ---- Materials ------------------------------------------------------------------------------
    // What a voxel is made of, as physics sees it: how heavy, how grippy, how bouncy. A table of
    // them, indexed from 1 (0 is "none": the body's own BodyDesc values). Voxel shapes name one per
    // piece (utils::SlotMaterials); a contact uses each side's -- the material of the piece touched,
    // or the body's values where it has none -- combined as Jolt combines bodies: friction the
    // geometric mean, restitution the larger.
    //
    // How it breaks (runtime::fracture) is part of it too, though the simulation itself never reads
    // it: a strike breaks the voxels within a reach set by its energy and the struck material's
    // strength, into shards laid out by the material's fracture pattern.
    struct PhysicsMaterial {
        float density = 1000.0f;    // kg/m^3
        float friction = 0.6f;
        float restitution = 0.0f;   // 0..1
        // Joules to break a cubic metre: a strike of energy E breaks out to the radius whose sphere
        // holds E / strength. 0: it does not break.
        float strength = 0.0f;
        // How it breaks: a pattern registered by name (runtime::registerFracturePattern). Built in:
        // "shatter" (stone, glass), "splinter" (wood, along `grain`), "crumble" (plaster, earth),
        // "seams" (brick: apart at palette-entry boundaries first).
        std::string fracture = "shatter";
        float shardSize = 0.5f;     // m: about how big a shard is
        // The direction splinters run along, in the voxels' own frame (a component's local axes).
        core::vec3 grain{0.0f, 1.0f, 0.0f};
        // Holds up what it is part of: in a static collider that splits, a piece with a voxel of an
        // anchor material in it stands, whatever is cut away under it (bedrock, a foundation).
        bool anchor = false;
        bool operator==(const PhysicsMaterial&) const = default;
    };
    constexpr uint16_t MAX_PHYSICS_MATERIALS = 1024;   // indices 1..1023

    // What static geometry does to a kinematic body (BodyDesc::blockedByStatic). Nothing, by default:
    // a kinematic body goes where it is moved, through floors and walls. Stop: a move that would take
    // it into a static body ends where it touches. Slide: what is left of the move after the touch
    // goes on along the surface (up to three surfaces a step) -- a ball rolling on along the floor it
    // was driven into, a platform along a wall. Asked for, as everything is.
    enum class KinematicBlocking : uint8_t { None, Stop, Slide };

    struct BodyDesc {
        PhysicsShape shape;
        MotionType   motion = MotionType::Dynamic;
        PhysicsLayer layer = PhysicsLayer::Moving;
        core::vec3   position{0.0f};
        core::quat   rotation{1.0f, 0.0f, 0.0f, 0.0f};
        core::vec3   linearVelocity{0.0f};
        core::vec3   angularVelocity{0.0f};
        // Mass: the shape's volume times `density`, unless `mass` is > 0, which sets it outright.
        // Either way the inertia is the shape's, scaled to that mass. A voxel shape with materials
        // weighs what its pieces' densities say instead, and its centre of mass is theirs (material-0
        // pieces at the otherDensity it was built with; a primitive beside it at `density`). `mass`
        // still scales the whole.
        float        density = 1000.0f;
        float        mass = 0.0f;
        float        friction = 0.6f;
        // 0, Jolt's default: bounce is something a body asks for. Restitution in a resting stack
        // is what starts the rocking described at PhysicsSettings::velocitySteps.
        float        restitution = 0.0f;
        float        linearDamping = 0.05f;
        float        angularDamping = 0.05f;
        float        gravityFactor = 1.0f;
        // Continuous collision (a swept test each step) instead of discrete. For bodies small or
        // fast enough to pass through something in one step.
        bool         continuous = false;
        CollisionFilter filter;
        // Report when it begins and stops touching another body (PhysicsWorld::contactChanges). A
        // sensor always does.
        bool         reportContacts = false;
        // A sensor's (layer Sensor): the layers of the bodies it senses, layerBit()s. What a static
        // sensor senses only while it moves -- a sleeping body stays sensed, but is not found anew --
        // and a static sensor never senses static bodies; a kinematic one does, at a cost.
        uint32_t     senses = layerBit(PhysicsLayer::Moving) | layerBit(PhysicsLayer::Character);
        // A kinematic body's: whether static bodies (not sensors, and only those its layer and filter
        // collide with) stop it. It touches them too, so its contacts with them are reported
        // (reportContacts). Swept along the move in the rotation it starts the step in; the turn is
        // applied where it ends, unswept.
        KinematicBlocking blockedByStatic = KinematicBlocking::None;
    };

    struct BodyId {
        uint32_t value = 0xffffffffu;
        bool valid() const { return value != 0xffffffffu; }
        bool operator==(const BodyId&) const = default;
    };

    struct BodyState {
        core::vec3 centerOfMass{0.0f};   // world space
        core::vec3 position{0.0f};
        core::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        core::vec3 linearVelocity{0.0f};
        core::vec3 angularVelocity{0.0f};
        bool       awake = false;
    };

    struct PhysicsSettings {
        // Capacities. Jolt allocates these up front; a full body table refuses creation, and a full
        // pair or contact buffer drops contacts for that step, which step() reports (PhysicsStats).
        // Crossing 80% of maxBodies is warned about, once.
        // The pairs and contacts are sized for fracture: shards are made packed face to face, each
        // touching many others, and thirty blasts in the demos' brick yard (~6,000 shards) filled
        // 65,536 pairs and 32,768 contacts.
        uint32_t   maxBodies = 65536;
        uint32_t   maxBodyPairs = 262144;
        uint32_t   maxContactConstraints = 131072;
        core::vec3 gravity{0.0f, -9.81f, 0.0f};

        // ---- Guards (the physics plan, A8) ----
        // A body outside this box after a step has left the world: it is reported
        // (takeBodiesThatLeftTheWorld) for the game to deal with, never moved or deleted here.
        core::vec3 worldMin{-10000.0f}, worldMax{10000.0f};
        // Jolt clamps every body to these, every step.
        float      maxLinearSpeed = 500.0f;     // m/s
        float      maxAngularSpeed = 47.1f;     // rad/s, Jolt's default (450 rpm)
        // Floors on what a body's mass properties may be: a sliver of voxels can otherwise be a
        // body light enough, or with little enough inertia about some axis, for the solver to spin
        // it up without limit. Inertia is floored at mass * minGyrationRadius^2 about every axis.
        float      minMass = 0.01f;             // kg
        float      minGyrationRadius = 0.05f;   // m

        // Rest damping. A dynamic body that stays awake but within restRadius and restAngle of one
        // pose for restSeconds is going nowhere -- typically a stack rocking in a slow circle the
        // solver never damps, which Jolt's speed-based sleep test then never ends (the 200-crate
        // pile tests: towers swaying for good at 0.2 m/s). Its velocities are multiplied by
        // restDamping every step after that, until it sleeps. A body rolling or spinning leaves
        // the radius or the angle quickly and is never touched. restSeconds 0 turns it off.
        // Sleep: a body sleeps once every point of it has moved slower than sleepSpeed for
        // sleepSeconds (Jolt's test, and Jolt's defaults). It errs towards sleeping: a body that
        // starts to slide or tip from rest gathers speed slowly, and one still under 3 cm/s after
        // half a second sleeps where it is -- a brick creeping off a ledge round a hole in demo 9,
        // a box on a slope just steeper than its friction -- until something wakes it. A lower
        // speed (5 mm/s) lets those go on, but keeps big piles awake far longer (demo 1's 2,700
        // crates, a thousand still awake at the self-test's check). The project's call: over-
        // sleeping is the lesser fault. Lower it for a game where slow slides must finish.
        float      sleepSpeed = 0.03f;    // m/s
        float      sleepSeconds = 0.5f;
        float      restRadius = 0.05f;    // m
        float      restAngle = 0.1f;      // rad
        float      restSeconds = 1.0f;
        float      restDamping = 0.9f;

        // Solver iterations per step. 15 velocity steps, not Jolt's 10: stacks are stiffer and
        // settle sooner. With 10, the first 200-crate pile test (tests/unit/
        // test_physics_reliability.cpp) left two 8-high columns rocking at 0.2 m/s after 20 s; with
        // 15 they slept within 4 s. Other layouts still rocked at 15, which is what rest damping
        // above is for: the iterations make it rarer, the damping makes it end. Roughly a third
        // more solver time.
        int        velocitySteps = 15;
        int        positionSteps = 2;
        // The same for joints, which take more: a ten-link chain dropped from level stretched 3% at
        // its worst with the contact counts (tests/unit/test_physics_joints.cpp, the chain).
        int        jointVelocitySteps = 30;
        int        jointPositionSteps = 6;

        // Contacts that begin closing faster than this are impacts (PhysicsWorld::impacts): what
        // breaks voxels on impact, and what a game hangs sounds and damage on.
        float      minImpactSpeed = 1.0f;     // m/s

        // Record every call that changes the simulation, from construction on, with the state hash
        // after each step: PhysicsWorld::recording() returns it, and PhysicsWorld::replay plays it
        // into a fresh world and says whether every step came out the same. A determinism check
        // end to end, and a file a physics bug can be reported with.
        bool       record = false;

        // Worker threads for the step: 0 runs it on the calling thread alone, -1 picks from the
        // machine (one fewer than it has cores, at most 7). Results do not depend on the count --
        // a test holds that -- so this is a speed setting only.
        int        threads = -1;
    };

    // Two bodies that began touching in a step, closing at `speed` or more (PhysicsSettings::
    // minImpactSpeed): one per pair of bodies -- the fastest of their contacts -- with `a` the
    // lower id. Speeds and velocities are the bodies' as the step began, before the solver met the
    // contact, so what the collision took out can be given back (a boulder through a wall).
    struct Impact {
        BodyId     a, b;
        core::vec3 point{0.0f};               // world space, between the two surfaces
        core::vec3 normal{0.0f, 1.0f, 0.0f};  // from a towards b
        float      speed = 0.0f;              // closing speed along the normal, m/s
        // What the collision can take out of the motion: half the pair's reduced mass times the
        // closing speed squared (the whole of the striker's when it strikes something static), J.
        float      energy = 0.0f;
        core::vec3 velocityA{0.0f}, velocityB{0.0f};   // each body's linear velocity, before
    };

    // Two bodies began or stopped touching in a step (PhysicsWorld::contactChanges), one of them a
    // body that reports contacts (BodyDesc::reportContacts) or a sensor. One per pair of bodies,
    // however many of their parts touch, with `a` the lower id. A pair that falls asleep touching is
    // still touching: it ends when they part, or when either body is destroyed (or stops reporting,
    // with the other not reporting either). Every begin has its end.
    struct ContactChange {
        BodyId     a, b;
        bool       began = true;
        bool       sensor = false;                // either is a sensor
        // Began only: where they first touched (world space) and the normal there, from a towards b.
        core::vec3 point{0.0f};
        core::vec3 normal{0.0f, 1.0f, 0.0f};
    };

    struct PhysicsStats {
        uint64_t steps = 0;
        uint32_t bodies = 0;
        uint32_t awakeBodies = 0;
        // Steps in which Jolt ran out of room for contacts and ignored some. Each one is an error in
        // the log too; a nonzero count means the capacities above are too small for the scene.
        uint64_t stepsWithDroppedContacts = 0;
        uint64_t refusedBodies = 0;     // createBody calls refused (bad input or a full table)
        // Bodies found with a non-finite pose or velocity after a step, and put back where they last
        // were, at rest. Each is logged; a nonzero count is a bug to report, not a state to live in.
        uint64_t nonFiniteRecoveries = 0;
        uint64_t leftWorld = 0;         // reports of bodies outside the world bounds
        double   lastStepMilliseconds = 0.0;   // wall time of the last step(), for profiling only
        int      threads = 0;           // worker threads the step uses (0: the calling thread)
    };

    // ---- Characters -------------------------------------------------------------------------------
    // A walking character: an upright capsule moved by its own controller (Jolt's CharacterVirtual)
    // rather than by the solver -- it walks up steps and slopes up to maxSlope, sticks to the floor
    // going down them, stands on moving bodies, and pushes dynamic bodies with at most maxStrength.
    // An inner body of the same shape is what other bodies collide with.
    struct CharacterDesc {
        float        radius = 0.3f;
        float        height = 1.8f;               // total, feet to top
        core::vec3   position{0.0f};              // the feet
        float        maxSlope = 0.87f;            // radians (50 degrees)
        float        mass = 70.0f;
        float        maxStrength = 300.0f;        // N: how hard it can push
        float        stepUp = 0.4f;               // m: the highest step it walks up
        float        stickDown = 0.5f;            // m: how far down it follows the floor
        PhysicsLayer layer = PhysicsLayer::Character;
        // What it walks through, and what passes through it (its inner body has the same filter).
        CollisionFilter filter;
    };

    struct CharacterId {
        uint32_t value = 0xffffffffu;
        bool valid() const { return value != 0xffffffffu; }
        bool operator==(const CharacterId&) const = default;
    };

    struct CharacterState {
        core::vec3 position{0.0f};                // the feet
        core::vec3 velocity{0.0f};
        bool       onGround = false;              // standing on walkable ground
        bool       onSteepGround = false;         // touching ground too steep to stand on
        core::vec3 groundNormal{0.0f};
        BodyId     groundBody;
    };

    // ---- Joints -----------------------------------------------------------------------------------
    // A constraint between two bodies, or a body and the world. Placed in world space as the bodies
    // are when it is made: it holds them in that relation from then on (within its freedoms).
    enum class JointType : uint8_t {
        Fixed,      // welded: no relative motion at all
        Point,      // ball and socket: turns any way about `anchor`
        Hinge,      // turns about `axis` through `anchor`; limits are angles (radians)
        Slider,     // slides along `axis`; limits are distances (metres)
        Distance    // keeps `anchor` (on a) and `anchorB` (on b) between limitMin and limitMax apart: a rope,
                    // a rod, or with a spring, a spring
    };

    enum class MotorMode : uint8_t {
        Velocity,   // drive at motorVelocity (rad/s or m/s)
        Position    // hold motorTarget (an angle in rad, or an offset in m), like a servo
    };

    struct JointDesc {
        JointType  type = JointType::Point;
        BodyId     a;
        BodyId     b;                          // invalid: attached to the world
        core::vec3 anchor{0.0f};               // world space
        core::vec3 anchorB{0.0f};              // Distance only: the point on b
        core::vec3 axis{0.0f, 1.0f, 0.0f};     // Hinge, Slider
        // Hinge/Slider: limits, none when both are 0. Distance: the range; both 0 (or negative): the
        // distance the anchors are apart when it is made.
        float      limitMin = 0.0f, limitMax = 0.0f;
        float      springFrequency = 0.0f;     // Distance: Hz; 0 is rigid
        float      springDamping = 0.0f;       // 0..1
        float      friction = 0.0f;            // Hinge: N m, Slider: N -- what turning it takes
        // Hinge/Slider: a motor driving it at motorVelocity (rad/s or m/s), or holding motorTarget
        // (rad or m) in Position mode, with at most motorLimit (N m or N; 0: unlimited). A position
        // motor is a spring of motorFrequency Hz (0: Jolt's default, 2 Hz), critically damped.
        bool       motor = false;
        MotorMode  motorMode = MotorMode::Velocity;
        float      motorVelocity = 0.0f;
        float      motorTarget = 0.0f;
        float      motorLimit = 0.0f;
        float      motorFrequency = 0.0f;
        // It breaks -- is removed, and reported (takeBrokenJoints) -- when holding the bodies together
        // takes more than this force (N) or torque (N m). 0: never. What broke it carries on: the
        // impulse beyond what it could hold in its last step is given back to the bodies.
        float      breakForce = 0.0f;
        float      breakTorque = 0.0f;
        // Whether the two bodies it joins collide with each other. Off by default, as in most
        // engines: a wheel and the chassis it hangs from, the links of a chain, are meant to overlap
        // or touch, and colliding they fight their own joint. Once it breaks, they collide again.
        bool       collideConnected = false;
    };

    struct JointId {
        uint32_t value = 0xffffffffu;
        bool valid() const { return value != 0xffffffffu; }
        bool operator==(const JointId&) const = default;
    };

    struct JointState {
        bool  alive = false;
        float position = 0.0f;   // Hinge: angle (rad); Slider: offset (m); Distance: length (m)
        float force = 0.0f;      // what it held with in the last step (N)
        float torque = 0.0f;     // (N m)
    };

    // ---- Queries ----------------------------------------------------------------------------------
    // Which bodies a query sees: a bit per PhysicsLayer (all by default), one body to skip (the
    // caster's own, typically), and the collision groups whose bodies it skips (CollisionFilter bits).
    struct QueryFilter {
        uint32_t layers = 0xffffffffu;
        BodyId   ignore;
        uint32_t skipGroups = 0;
        static uint32_t only(PhysicsLayer layer) { return 1u << uint32_t(layer); }
    };

    // Where a query struck a body. `normal` is the collision shape's, pointing out of the body --
    // never one derived from voxel coordinates, so a different collision representation changes it
    // everywhere at once. `path` names the sub-shape: the index taken at each compound on the way
    // down from the body's shape to the one struck (see runtime::castRay for what it means).
    struct PhysicsHit {
        BodyId                body;
        core::vec3            position{0.0f};
        core::vec3            normal{0.0f};
        float                 distance = 0.0f;
        std::vector<uint32_t> path;
    };

    // Everything a simulation was given, in order: what replay needs to run it again.
    struct PhysicsLog {
        std::vector<uint8_t> bytes;
        uint64_t             steps = 0;
        bool saveToFile(const std::string& path) const;
        static bool loadFromFile(const std::string& path, PhysicsLog& out);
    };

    struct ReplayResult {
        bool        matched = false;      // every step's state hash equal to the recording's
        uint64_t    stepsReplayed = 0;
        uint64_t    firstDivergentStep = 0;   // 1-based; 0 when none diverged
        std::string problem;              // why it did not match, for a log
    };

    class PhysicsWorld {
    public:
        explicit PhysicsWorld(const PhysicsSettings& settings = {});
        ~PhysicsWorld();
        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;
        PhysicsWorld(PhysicsWorld&&) = delete;
        PhysicsWorld& operator=(PhysicsWorld&&) = delete;

        // Creates and adds a body. An invalid id, logged, when the description is unusable or the
        // body table is full. A dynamic or kinematic body starts awake.
        BodyId createBody(const BodyDesc& desc);
        // Removes and destroys a body, first waking every body its bounds touch -- so what rested on
        // it falls, rather than sleeping in mid-air. A stale or invalid id is ignored.
        void destroyBody(BodyId id);
        bool isAlive(BodyId id) const;

        // Default state for an id that is not alive.
        BodyState bodyState(BodyId id) const;
        // The body's mass, 0 for a static body or an id that is not alive.
        float bodyMass(BodyId id) const;

        // Changes which bodies it passes through (BodyDesc::filter), from the next step, and wakes
        // it and what touches it -- so a contact that should end does, and one that should begin
        // does. A stale id is ignored.
        void setFilter(BodyId id, const CollisionFilter& filter);
        // Default for an id that is not alive.
        CollisionFilter filter(BodyId id) const;

        // BodyDesc::reportContacts, from the next step: on, it wakes the body so what it is already
        // touching is found and reported as begun; off, its pairs end (unless the other body reports).
        // A sensor reports whatever this says. A stale id is ignored.
        void setReportContacts(BodyId id, bool on);
        bool reportsContacts(BodyId id) const;

        // Wakes every body whose bounds touch this one's: what an edit to its shape must do, so
        // bodies resting on (or now inside) the old shape notice the new one.
        void wakeBodiesAround(BodyId id);

        // ---- Materials ---------------------------------------------------------------------------
        // Defines (or redefines) material `index`, 1..MAX_PHYSICS_MATERIALS-1. False, logged, for an
        // index out of range or values that are not usable (density not positive, friction
        // negative, strength negative, shard size not positive, a zero grain, anything non-finite);
        // restitution is clamped to 0..1 and the grain normalized. Friction and restitution
        // apply from the next step to every contact; a density is baked into voxel shapes, so it
        // reaches bodies made after the change.
        bool setMaterial(uint16_t index, const PhysicsMaterial& material);
        // Default values for an index never defined.
        PhysicsMaterial material(uint16_t index) const;

        // ---- Voxel shapes ------------------------------------------------------------------------
        // The shape a blob collides as (utils::buildCollisionPieces), built once per blob content and
        // parameters and shared after that: two calls for the same content return the same shape,
        // and an edit -- which gives the blob a new content stamp -- gets a new one. Null when the
        // blob has no voxels (nothing to collide with) or its pieces could not be made into a shape.
        // `materials` gives each palette slot's physics material; pieces of material 0 weigh
        // `otherDensity`. Shapes with different materials, densities or table versions are cached
        // apart.
        CollisionShapeRef voxelShape(const GeometryBlob& blob, uint32_t resolution,
                                     const utils::CollisionParams& params = {},
                                     const utils::SlotMaterials* materials = nullptr, float otherDensity = 1000.0f);
        // The same, from pieces already built, and not cached.
        CollisionShapeRef shapeFromPieces(const utils::CollisionPieces& pieces, float otherDensity = 1000.0f);
        // Drops cached shapes nothing uses any more: no ref held outside the cache, no body built
        // from it. step() does this once a second of ticks; call it to release memory sooner.
        void pruneShapeCache();
        size_t cachedShapeCount() const;

        // ---- Moving bodies -------------------------------------------------------------------------
        // All of these wake the body, and ignore a body that is not alive or cannot move.
        void addImpulse(BodyId id, core::vec3 impulse);              // at the centre of mass, N s
        void addAngularImpulse(BodyId id, core::vec3 impulse);       // N m s
        void addVelocity(BodyId id, core::vec3 linear);              // a velocity change, whatever the mass
        void setVelocity(BodyId id, core::vec3 linear, core::vec3 angular);
        // Places the body; velocities are kept. A teleport: nothing is swept along the way.
        void setPose(BodyId id, core::vec3 position, core::quat rotation);
        // A kinematic body: moves it to the pose over the next step, with the velocity that takes,
        // so what it pushes is pushed with the right speed. One blocked by static bodies
        // (BodyDesc::blockedByStatic) moves as far as they let it instead; the position it heads for
        // is returned (the one asked for, unblocked; the body's own for one that cannot move).
        core::vec3 moveKinematic(BodyId id, core::vec3 position, core::quat rotation, float dt);
        void setGravity(core::vec3 gravity);
        core::vec3 gravity() const;

        // Advances the simulation by `dt` seconds, in one collision step. Callers step at a fixed
        // rate (FixedUpdate); a varying dt is allowed but gives up determinism across runs that
        // varied it differently.
        void step(float dt);

        // Steps run since construction (or since the step a restored snapshot was saved at).
        uint64_t tick() const;

        // The whole simulation state: tick, every body, the contact cache (so warm starting
        // resumes exactly). Opaque bytes, meaningful only to restoreState in a world with the same
        // bodies.
        std::vector<uint8_t> saveState() const;
        // False, with nothing changed, if the bytes are not a snapshot or the set of live bodies is
        // not the one that was saved.
        bool restoreState(const std::vector<uint8_t>& snapshot);

        // A hash of every body's position, rotation and velocities (their exact bits), in body id
        // order, plus the tick. Equal hashes: the same simulation state, for every practical purpose.
        uint64_t stateHash() const;

        // ---- Characters ----------------------------------------------------------------------------
        CharacterId createCharacter(const CharacterDesc& desc);
        void destroyCharacter(CharacterId id);
        bool isAlive(CharacterId id) const;
        // What the character tries to do from the next step on, until changed: walk at
        // `horizontal` (its vertical part is ignored), and jump at `jumpSpeed` if `jump` -- a jump
        // happens once, on the next step it is standing.
        void setCharacterInput(CharacterId id, core::vec3 horizontal, bool jump, float jumpSpeed = 5.0f);
        // Places it; velocity kept.
        void setCharacterPosition(CharacterId id, core::vec3 feet);
        // CharacterDesc::filter, for it and its inner body, from the next step.
        void setCharacterFilter(CharacterId id, const CollisionFilter& filter);
        CharacterState characterState(CharacterId id) const;
        // The inner body other bodies collide with, so queries and filters can name it.
        BodyId characterBody(CharacterId id) const;

        // ---- Joints --------------------------------------------------------------------------------
        // An invalid id, logged, when a body is not alive or the description is unusable. Joints go
        // with their bodies: destroying either body removes every joint on it.
        JointId createJoint(const JointDesc& desc);
        void destroyJoint(JointId id);
        bool isAlive(JointId id) const;
        void setJointMotor(JointId id, bool on, float velocity);   // velocity mode
        void setJointMotor(JointId id, bool on, MotorMode mode, float value);
        JointState jointState(JointId id) const;
        // Joints that broke (breakForce, breakTorque) since the last call, in slot order, with what
        // they were holding when they did. Each is gone already; its id is no longer alive.
        struct BrokenJoint { JointId id; float force = 0.0f, torque = 0.0f; };
        std::vector<BrokenJoint> takeBrokenJoints();

        // ---- Queries, against the state at the end of the last step ------------------------------
        // The nearest body a ray from `origin` along the unit `direction` meets within maxDistance.
        // A ray starting inside a body hits it at distance 0.
        std::optional<PhysicsHit> castRay(core::vec3 origin, core::vec3 direction, float maxDistance,
                                          const QueryFilter& filter = {}) const;
        // The first body a sphere swept from `origin` along `direction` touches.
        std::optional<PhysicsHit> castSphere(core::vec3 origin, float radius, core::vec3 direction, float maxDistance,
                                             const QueryFilter& filter = {}) const;
        // Every body overlapping the shape, in body id order.
        std::vector<BodyId> overlapSphere(core::vec3 centre, float radius, const QueryFilter& filter = {}) const;
        std::vector<BodyId> overlapBox(core::vec3 centre, core::vec3 halfExtents, core::quat rotation,
                                       const QueryFilter& filter = {}) const;

        // What has been recorded so far (PhysicsSettings::record), or an empty log.
        PhysicsLog recording() const;
        // Runs a recording in a fresh world made with the recorded settings, and compares every
        // step's state hash with the one recorded.
        static ReplayResult replay(const PhysicsLog& log);

        // Bodies found outside the world bounds since the last call, in body id order. Each is
        // reported once per excursion: again only after it has come back inside.
        std::vector<BodyId> takeBodiesThatLeftTheWorld();

        // **Tests only.** Makes the next step's check find this body non-finite, as a solver blow-up
        // would leave it, so the recovery can be exercised. (Real NaN cannot be written through
        // Jolt's API: its asserts reject it, which is the first line of defence in dev builds.)
        void corruptBodyForTesting(BodyId id);

        const PhysicsStats& stats() const;
        // The impacts of the last step, ordered by (a, b): the same on every machine, whatever
        // thread the solver met each contact on.
        const std::vector<Impact>& impacts() const;
        // The contacts that began and ended in the last step, ordered by (a, b), ends first; pairs
        // a destroyBody (or setReportContacts off) ended since the step before are among them.
        const std::vector<ContactChange>& contactChanges() const;
        // Every pair touching now that a change was or would be reported for, ordered by (a, b):
        // what a contactChanges reader rebuilds its view from after restoreState.
        std::vector<ContactChange> contactPairs() const;

    private:
        void snapshotVelocities();
        void orderImpacts();
        void trackContacts();
        core::vec3 sweepAgainstStatic(BodyId id, core::vec3 target, bool slide) const;
        void endContactsOf(BodyId body, bool unlessReported);
        void sanityPass(float dt);
        void updateCharacters(float dt);
        void checkJoints(float dt);
        void removeJointsOf(BodyId body);
        BodyId createBodyUnrecorded(const BodyDesc& desc);
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}

#endif
