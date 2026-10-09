#ifndef PROJECTV_PHYSICS_WORLD_H
#define PROJECTV_PHYSICS_WORLD_H

#include <cstdint>
#include <memory>
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

    // What collides with what. A fixed list, so the collision matrix is a few lines of code rather
    // than data: everything collides with everything except that static ignores static, debris
    // ignores debris (piles of fragments are the expensive case and nobody watches them touch), and
    // a sensor touches only things that move under their own power (moving bodies and characters).
    enum class PhysicsLayer : uint8_t {
        Static,
        Moving,
        Debris,
        Sensor,
        Character,
        Count
    };
    bool layersCollide(PhysicsLayer a, PhysicsLayer b);

    // A shape built from voxels (PhysicsWorld::voxelShape): the blob's collision pieces as one Jolt
    // shape, in the blob's voxel space. Shared: every body made from the same blob content uses the
    // same one, and holding a ref keeps it alive. Opaque; the fields are for diagnostics and tests.
    struct CollisionShape {
        uint32_t                 pieces = 0;
        float                    volume = 0.0f;   // in voxels (cubic voxel units)
        utils::CollisionFallback fallback = utils::CollisionFallback::None;
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

    struct BodyDesc {
        PhysicsShape shape;
        MotionType   motion = MotionType::Dynamic;
        PhysicsLayer layer = PhysicsLayer::Moving;
        core::vec3   position{0.0f};
        core::quat   rotation{1.0f, 0.0f, 0.0f, 0.0f};
        core::vec3   linearVelocity{0.0f};
        core::vec3   angularVelocity{0.0f};
        // Mass: the shape's volume times `density`, unless `mass` is > 0, which sets it outright.
        // Either way the inertia is the shape's, scaled to that mass.
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
    };

    struct BodyId {
        uint32_t value = 0xffffffffu;
        bool valid() const { return value != 0xffffffffu; }
        bool operator==(const BodyId&) const = default;
    };

    struct BodyState {
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
        uint32_t   maxBodies = 65536;
        uint32_t   maxBodyPairs = 65536;
        uint32_t   maxContactConstraints = 32768;
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
        // Removes and destroys a body. A stale or invalid id is ignored.
        void destroyBody(BodyId id);
        bool isAlive(BodyId id) const;

        // Default state for an id that is not alive.
        BodyState bodyState(BodyId id) const;
        // The body's mass, 0 for a static body or an id that is not alive.
        float bodyMass(BodyId id) const;

        // ---- Voxel shapes ------------------------------------------------------------------------
        // The shape a blob collides as (utils::buildCollisionPieces), built once per blob content and
        // parameters and shared after that: two calls for the same content return the same shape,
        // and an edit -- which gives the blob a new content stamp -- gets a new one. Null when the
        // blob has no voxels (nothing to collide with) or its pieces could not be made into a shape.
        CollisionShapeRef voxelShape(const GeometryBlob& blob, uint32_t resolution,
                                     const utils::CollisionParams& params = {});
        // The same, from pieces already built, and not cached.
        CollisionShapeRef shapeFromPieces(const utils::CollisionPieces& pieces);
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
        // so what it pushes is pushed with the right speed.
        void moveKinematic(BodyId id, core::vec3 position, core::quat rotation, float dt);
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

    private:
        void sanityPass(float dt);
        BodyId createBodyUnrecorded(const BodyDesc& desc);
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}

#endif
