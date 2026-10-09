#ifndef PROJECTV_PHYSICS_WORLD_H
#define PROJECTV_PHYSICS_WORLD_H

#include <cstdint>
#include <memory>
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
        struct Native;                            // the Jolt shape; defined where Jolt is
        std::shared_ptr<Native>  native;
    };
    using CollisionShapeRef = std::shared_ptr<const CollisionShape>;

    struct PhysicsShape {
        enum class Kind : uint8_t { Box, Sphere, Capsule, Voxels };
        Kind       kind = Kind::Box;
        core::vec3 halfExtents{0.5f};   // Box
        float      radius = 0.5f;       // Sphere, Capsule
        float      halfHeight = 0.5f;   // Capsule: half the length of the cylinder part, along Y
        // Voxels: the shape, and the world size of one voxel (the chunk's voxel size times its
        // uniform scale). The body's position is the voxel space origin -- the chunk's corner.
        CollisionShapeRef voxels;
        float             voxelSize = 1.0f;

        static PhysicsShape box(core::vec3 halfExtents) { return {Kind::Box, halfExtents, 0.0f, 0.0f, {}, 1.0f}; }
        static PhysicsShape sphere(float radius) { return {Kind::Sphere, core::vec3(0.0f), radius, 0.0f, {}, 1.0f}; }
        static PhysicsShape capsule(float halfHeight, float radius) {
            return {Kind::Capsule, core::vec3(0.0f), radius, halfHeight, {}, 1.0f};
        }
        static PhysicsShape fromVoxels(CollisionShapeRef shape, float voxelSize) {
            return {Kind::Voxels, core::vec3(0.0f), 0.0f, 0.0f, std::move(shape), voxelSize};
        }
    };

    struct BodyDesc {
        PhysicsShape shape;
        MotionType   motion = MotionType::Dynamic;
        PhysicsLayer layer = PhysicsLayer::Moving;
        core::vec3   position{0.0f};
        core::quat   rotation{1.0f, 0.0f, 0.0f, 0.0f};
        core::vec3   linearVelocity{0.0f};
        core::vec3   angularVelocity{0.0f};
        // Mass: from the shape's volume at `density`, unless `mass` is > 0, which sets it outright
        // (the inertia keeps the shape's distribution, scaled to that mass).
        float        density = 1000.0f;
        float        mass = 0.0f;
        float        friction = 0.6f;
        float        restitution = 0.2f;
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
        uint32_t   maxBodies = 65536;
        uint32_t   maxBodyPairs = 65536;
        uint32_t   maxContactConstraints = 32768;
        core::vec3 gravity{0.0f, -9.81f, 0.0f};
    };

    struct PhysicsStats {
        uint64_t steps = 0;
        uint32_t bodies = 0;
        uint32_t awakeBodies = 0;
        // Steps in which Jolt ran out of room for contacts and ignored some. Each one is an error in
        // the log too; a nonzero count means the capacities above are too small for the scene.
        uint64_t stepsWithDroppedContacts = 0;
        uint64_t refusedBodies = 0;     // createBody calls refused (bad input or a full table)
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

        const PhysicsStats& stats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}

#endif
