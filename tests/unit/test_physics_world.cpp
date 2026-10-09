// The physics core (runtime/physics/physics_world.h): it settles things, it is deterministic, it
// restores exactly, and it refuses what it cannot simulate.

#include "doctest/doctest.h"

#include <cmath>
#include <vector>

#include "runtime/physics/physics_world.h"

using namespace projv::runtime;
using projv::core::vec3;

namespace {
    constexpr float DT = 1.0f / 60.0f;

    // A floor whose top is at y = 0.
    BodyId addFloor(PhysicsWorld& physics) {
        BodyDesc floor;
        floor.shape = PhysicsShape::box({50.0f, 1.0f, 50.0f});
        floor.motion = MotionType::Static;
        floor.layer = PhysicsLayer::Static;
        floor.position = {0.0f, -1.0f, 0.0f};
        return physics.createBody(floor);
    }

    BodyDesc crateAt(vec3 position) {
        BodyDesc crate;
        crate.shape = PhysicsShape::box({0.5f, 0.5f, 0.5f});
        crate.position = position;
        return crate;
    }

    // A scene with enough going on that a determinism bug would show: a tumbling pile of boxes and
    // spheres, thrown in with spin, landing on each other.
    std::vector<BodyId> buildPile(PhysicsWorld& physics) {
        std::vector<BodyId> bodies;
        addFloor(physics);
        for (int i = 0; i < 60; i++) {
            BodyDesc d = crateAt({float(i % 5) * 1.1f - 2.2f, 2.0f + float(i / 5) * 1.3f, float(i % 3) * 0.7f});
            if (i % 2) d.shape = PhysicsShape::sphere(0.45f);
            d.angularVelocity = {0.3f * float(i % 7), 0.1f * float(i % 4), -0.2f * float(i % 5)};
            d.linearVelocity = {0.5f * float(i % 3) - 0.5f, 0.0f, 0.25f * float(i % 4)};
            bodies.push_back(physics.createBody(d));
        }
        return bodies;
    }
}

TEST_CASE("a box dropped on a floor comes to rest on it and falls asleep") {
    PhysicsWorld physics;
    addFloor(physics);
    BodyId crate = physics.createBody(crateAt({0.0f, 5.0f, 0.0f}));
    REQUIRE(crate.valid());

    for (int i = 0; i < 5 * 60; i++) physics.step(DT);

    BodyState s = physics.bodyState(crate);
    CHECK(s.position.y == doctest::Approx(0.5f).epsilon(0.02));   // half a box above the floor's top
    CHECK(std::abs(s.position.x) < 0.01f);
    CHECK(std::abs(s.position.z) < 0.01f);
    CHECK(glm::length(s.linearVelocity) < 1e-3f);
    CHECK_FALSE(s.awake);
    CHECK(physics.stats().stepsWithDroppedContacts == 0);
}

TEST_CASE("the same calls give the same state, bit for bit, every tick") {
    PhysicsWorld a, b;
    buildPile(a);
    buildPile(b);
    for (int i = 0; i < 600; i++) {
        a.step(DT);
        b.step(DT);
        REQUIRE(a.stateHash() == b.stateHash());
    }
    // And something actually happened: the pile is not where it started.
    PhysicsWorld fresh;
    buildPile(fresh);
    CHECK(fresh.stateHash() != a.stateHash());
}

TEST_CASE("restoring a snapshot and stepping on gives the same states as never restoring") {
    PhysicsWorld physics;
    buildPile(physics);
    for (int i = 0; i < 100; i++) physics.step(DT);
    std::vector<uint8_t> snapshot = physics.saveState();
    uint64_t hashAt100 = physics.stateHash();

    std::vector<uint64_t> firstRun;
    for (int i = 0; i < 100; i++) {
        physics.step(DT);
        firstRun.push_back(physics.stateHash());
    }
    CHECK(physics.tick() == 200);

    REQUIRE(physics.restoreState(snapshot));
    CHECK(physics.tick() == 100);
    CHECK(physics.stateHash() == hashAt100);
    for (int i = 0; i < 100; i++) {
        physics.step(DT);
        REQUIRE(physics.stateHash() == firstRun[size_t(i)]);
    }
}

TEST_CASE("a snapshot restores into another world that made the same bodies") {
    // What a late-joining peer, or a replay, does: build the same bodies, then take the state.
    PhysicsWorld source;
    buildPile(source);
    for (int i = 0; i < 150; i++) source.step(DT);
    std::vector<uint8_t> snapshot = source.saveState();

    PhysicsWorld joiner;
    buildPile(joiner);
    REQUIRE(joiner.restoreState(snapshot));
    CHECK(joiner.stateHash() == source.stateHash());
    for (int i = 0; i < 120; i++) {
        source.step(DT);
        joiner.step(DT);
        REQUIRE(joiner.stateHash() == source.stateHash());
    }
}

TEST_CASE("a restore with a different set of bodies is refused and changes nothing") {
    PhysicsWorld physics;
    std::vector<BodyId> bodies = buildPile(physics);
    for (int i = 0; i < 50; i++) physics.step(DT);
    std::vector<uint8_t> snapshot = physics.saveState();

    physics.destroyBody(bodies[3]);
    for (int i = 0; i < 10; i++) physics.step(DT);
    uint64_t before = physics.stateHash();
    uint64_t tickBefore = physics.tick();
    CHECK_FALSE(physics.restoreState(snapshot));
    CHECK(physics.stateHash() == before);
    CHECK(physics.tick() == tickBefore);

    // Garbage, and a truncated snapshot, likewise.
    CHECK_FALSE(physics.restoreState({1, 2, 3}));
    std::vector<uint8_t> truncated(snapshot.begin(), snapshot.begin() + long(snapshot.size() / 2));
    CHECK_FALSE(physics.restoreState(truncated));
    CHECK(physics.stateHash() == before);
}

TEST_CASE("a destroyed body's id stays dead even when its slot is reused") {
    PhysicsWorld physics;
    BodyId first = physics.createBody(crateAt({0, 0, 0}));
    REQUIRE(physics.isAlive(first));
    physics.destroyBody(first);
    CHECK_FALSE(physics.isAlive(first));
    physics.destroyBody(first);   // twice is harmless

    BodyId second = physics.createBody(crateAt({3, 0, 0}));
    REQUIRE(physics.isAlive(second));
    CHECK_FALSE(physics.isAlive(first));
    CHECK(physics.bodyState(first).position == vec3(0.0f));   // default state, not the new body's
    CHECK(physics.bodyState(second).position.x == doctest::Approx(3.0f));
    CHECK_FALSE(physics.isAlive(BodyId{}));
}

TEST_CASE("unusable bodies are refused, not simulated") {
    PhysicsWorld physics;
    BodyDesc nan = crateAt({std::nanf(""), 0, 0});
    CHECK_FALSE(physics.createBody(nan).valid());

    BodyDesc flat = crateAt({0, 0, 0});
    flat.shape = PhysicsShape::box({1.0f, 0.0f, 1.0f});
    CHECK_FALSE(physics.createBody(flat).valid());

    BodyDesc noRadius = crateAt({0, 0, 0});
    noRadius.shape = PhysicsShape::sphere(-1.0f);
    CHECK_FALSE(physics.createBody(noRadius).valid());

    BodyDesc zeroRotation = crateAt({0, 0, 0});
    zeroRotation.rotation = projv::core::quat(0, 0, 0, 0);
    CHECK_FALSE(physics.createBody(zeroRotation).valid());

    BodyDesc noDensity = crateAt({0, 0, 0});
    noDensity.density = 0.0f;
    CHECK_FALSE(physics.createBody(noDensity).valid());

    CHECK(physics.stats().refusedBodies == 5);

    // A rotation that is merely unnormalised is fixed, not refused.
    BodyDesc scaled = crateAt({0, 0, 0});
    scaled.rotation = projv::core::quat(2, 0, 0, 0);
    BodyId ok = physics.createBody(scaled);
    REQUIRE(ok.valid());
    CHECK(physics.bodyState(ok).rotation.w == doctest::Approx(1.0f));
}

TEST_CASE("a full body table refuses creation instead of failing later") {
    PhysicsSettings settings;
    settings.maxBodies = 4;
    PhysicsWorld physics(settings);
    for (int i = 0; i < 4; i++) REQUIRE(physics.createBody(crateAt({float(i) * 2, 0, 0})).valid());
    CHECK_FALSE(physics.createBody(crateAt({10, 0, 0})).valid());
    CHECK(physics.stats().refusedBodies == 1);
    physics.step(DT);   // still steps normally
}

TEST_CASE("layers: debris ignores debris but lands on the floor") {
    PhysicsWorld physics;
    addFloor(physics);
    BodyDesc low = crateAt({0, 0.5f, 0});
    low.layer = PhysicsLayer::Debris;
    BodyDesc high = crateAt({0, 3.0f, 0});
    high.layer = PhysicsLayer::Debris;
    BodyId a = physics.createBody(low), b = physics.createBody(high);
    for (int i = 0; i < 4 * 60; i++) physics.step(DT);
    // Both rest on the floor, one inside the other, rather than stacked.
    CHECK(physics.bodyState(a).position.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(physics.bodyState(b).position.y == doctest::Approx(0.5f).epsilon(0.02));

    CHECK(layersCollide(PhysicsLayer::Moving, PhysicsLayer::Debris));
    CHECK_FALSE(layersCollide(PhysicsLayer::Static, PhysicsLayer::Static));
    CHECK_FALSE(layersCollide(PhysicsLayer::Sensor, PhysicsLayer::Static));
    CHECK(layersCollide(PhysicsLayer::Character, PhysicsLayer::Sensor));
    CHECK(layersCollide(PhysicsLayer::Sensor, PhysicsLayer::Moving));
    for (int i = 0; i < int(PhysicsLayer::Count); i++)
        for (int j = 0; j < int(PhysicsLayer::Count); j++)
            CHECK(layersCollide(PhysicsLayer(i), PhysicsLayer(j)) == layersCollide(PhysicsLayer(j), PhysicsLayer(i)));
}

TEST_CASE("a small fast body with continuous collision does not pass through a thin wall") {
    PhysicsWorld physics;
    BodyDesc wall;
    wall.shape = PhysicsShape::box({5.0f, 5.0f, 0.05f});   // 10 cm thick
    wall.motion = MotionType::Static;
    wall.layer = PhysicsLayer::Static;
    wall.position = {0, 0, 0};
    physics.createBody(wall);

    BodyDesc bullet;
    bullet.shape = PhysicsShape::sphere(0.05f);
    bullet.position = {0, 0, -5};
    bullet.linearVelocity = {0, 0, 300};   // 5 m per step: twenty-five times its own size
    bullet.gravityFactor = 0.0f;
    bullet.continuous = true;
    BodyId id = physics.createBody(bullet);
    for (int i = 0; i < 30; i++) physics.step(DT);
    CHECK(physics.bodyState(id).position.z < 0.0f);

    // Not a control case: the same bullet *without* continuous collision is stopped too, because
    // Jolt's speculative contacts already reach as far as a body moves in one step. Continuous
    // collision is still what the plan prescribes for small fast bodies -- it is the guarantee,
    // where speculative contacts are a margin -- but this wall alone cannot tell the two apart.
}

// ---- Voxel shapes ---------------------------------------------------------------------------------

#include <set>

#include "utils/editing.h"
#include "utils/scene_query.h"

namespace {
    using projv::ComponentHandle;
    using projv::core::ivec3;

    ComponentHandle voxelChunk(projv::Scene& scene, const std::vector<ivec3>& voxels, uint32_t resolution = 16) {
        ComponentHandle h = projv::utils::addComponent(scene, projv::ComponentKind::Chunk, "c",
                                                       projv::INVALID_COMPONENT_HANDLE, resolution, 1.0f);
        std::vector<projv::PendingVoxelOp> ops;
        for (ivec3 v : voxels) ops.push_back({true, v, 0x3FFFFFFFu});
        projv::utils::queueVoxelAdd(scene, h, ops);
        projv::utils::updateScene(scene);
        return h;
    }
    const projv::GeometryBlob& blobOf(const projv::Scene& scene, ComponentHandle h) {
        return scene.geometryPool[scene.chunks[scene.components[h].chunkHandle].geometryPoolIndex];
    }
    std::vector<ivec3> solidBox(ivec3 low, ivec3 high) {
        std::vector<ivec3> out;
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) out.push_back({x, y, z});
        return out;
    }
}

TEST_CASE("a voxel body falls and rests on the floor at its true height") {
    projv::Scene scene;
    // A 4x2x4 slab of voxels sitting 3 voxels up inside its chunk, at half a metre per voxel.
    ComponentHandle h = voxelChunk(scene, solidBox({0, 3, 0}, {3, 4, 3}));
    PhysicsWorld physics;
    addFloor(physics);
    CollisionShapeRef shape = physics.voxelShape(blobOf(scene, h), 16);
    REQUIRE(shape);
    CHECK(shape->pieces == 1);
    CHECK(shape->volume == doctest::Approx(32.0f));

    BodyDesc d;
    d.shape = PhysicsShape::fromVoxels(shape, 0.5f);
    d.position = {0, 6, 0};
    d.density = 100.0f;
    BodyId body = physics.createBody(d);
    REQUIRE(body.valid());
    CHECK(physics.bodyMass(body) == doctest::Approx(32.0f * 0.125f * 100.0f));
    for (int i = 0; i < 5 * 60; i++) physics.step(DT);

    // The voxels start 3 voxels (1.5 m) above the body's origin, so resting on y = 0 puts the
    // origin 1.5 m below the floor's top.
    BodyState s = physics.bodyState(body);
    CHECK(s.position.y == doctest::Approx(-1.5f).epsilon(0.02));
    CHECK_FALSE(s.awake);
}

TEST_CASE("a voxel cup holds a ball: voxel shapes are not convex hulls") {
    projv::Scene scene;
    // A 6x4x6 cup: a floor and four walls, open at the top, hollow inside.
    std::vector<ivec3> cup;
    for (ivec3 v : solidBox({0, 0, 0}, {5, 3, 5}))
        if (v.y == 0 || v.x == 0 || v.x == 5 || v.z == 0 || v.z == 5) cup.push_back(v);
    ComponentHandle h = voxelChunk(scene, cup);
    PhysicsWorld physics;
    CollisionShapeRef shape = physics.voxelShape(blobOf(scene, h), 16);
    REQUIRE(shape);
    CHECK(shape->pieces > 1);

    BodyDesc cupBody;
    cupBody.shape = PhysicsShape::fromVoxels(shape, 1.0f);
    cupBody.motion = MotionType::Static;
    cupBody.layer = PhysicsLayer::Static;
    physics.createBody(cupBody);

    BodyDesc ball;
    ball.shape = PhysicsShape::sphere(0.8f);
    ball.position = {3.0f, 8.0f, 3.0f};
    BodyId id = physics.createBody(ball);
    for (int i = 0; i < 4 * 60; i++) physics.step(DT);
    BodyState s = physics.bodyState(id);
    // Resting on the cup's floor (top at y = 1), inside the walls: a hull would have held it at
    // the rim, y = 4.8.
    CHECK(s.position.y == doctest::Approx(1.8f).epsilon(0.03));
    CHECK(s.position.x > 1.0f);
    CHECK(s.position.x < 5.0f);
}

TEST_CASE("voxel shapes are shared by content and rebuilt after an edit") {
    projv::Scene scene;
    ComponentHandle h = voxelChunk(scene, solidBox({0, 0, 0}, {2, 2, 2}));
    PhysicsWorld physics;
    CollisionShapeRef first = physics.voxelShape(blobOf(scene, h), 16);
    CollisionShapeRef again = physics.voxelShape(blobOf(scene, h), 16);
    CHECK(first == again);
    projv::GeometryBlob copy = blobOf(scene, h);
    CHECK(physics.voxelShape(copy, 16) == first);   // an instance's copy of the same voxels
    CHECK(physics.cachedShapeCount() == 1);

    projv::utils::queueVoxelAdd(scene, h, {{true, ivec3(8, 8, 8), 0x3FFFFFFFu}});
    projv::utils::updateScene(scene);
    CollisionShapeRef edited = physics.voxelShape(blobOf(scene, h), 16);
    REQUIRE(edited);
    CHECK(edited != first);
    CHECK(edited->pieces == 2);

    // Different parameters are a different shape.
    projv::utils::CollisionParams params;
    params.pieceBudget = 1;
    CollisionShapeRef budgeted = physics.voxelShape(blobOf(scene, h), 16, params);
    REQUIRE(budgeted);
    CHECK(budgeted->fallback == projv::utils::CollisionFallback::OverBudget);
    CHECK(physics.cachedShapeCount() == 3);
}

TEST_CASE("unused voxel shapes are pruned; shapes a body uses are kept") {
    projv::Scene scene;
    ComponentHandle a = voxelChunk(scene, solidBox({0, 0, 0}, {1, 1, 1}));
    ComponentHandle b = voxelChunk(scene, solidBox({0, 0, 0}, {3, 0, 0}));
    PhysicsWorld physics;
    BodyId body;
    {
        CollisionShapeRef used = physics.voxelShape(blobOf(scene, a), 16);
        physics.voxelShape(blobOf(scene, b), 16);   // asked for, then dropped
        BodyDesc d;
        d.shape = PhysicsShape::fromVoxels(used, 0.25f);   // scaled: the body holds a wrapper
        body = physics.createBody(d);
        REQUIRE(body.valid());
    }
    CHECK(physics.cachedShapeCount() == 2);
    physics.pruneShapeCache();
    CHECK(physics.cachedShapeCount() == 1);       // b went; a's body still uses its shape
    physics.destroyBody(body);
    physics.pruneShapeCache();
    CHECK(physics.cachedShapeCount() == 0);

    // An empty blob has no shape, and a body cannot be made from none.
    projv::GeometryBlob empty;
    CHECK_FALSE(physics.voxelShape(empty, 16));
    BodyDesc none;
    none.shape = PhysicsShape::fromVoxels(nullptr, 1.0f);
    CHECK_FALSE(physics.createBody(none).valid());
}

TEST_CASE("a simulation with voxel bodies is deterministic and restorable") {
    projv::Scene scene;
    std::vector<ivec3> ell = solidBox({0, 0, 0}, {3, 0, 0});
    for (ivec3 v : solidBox({0, 1, 0}, {0, 3, 0})) ell.push_back(v);
    ComponentHandle h = voxelChunk(scene, ell);
    auto build = [&](PhysicsWorld& physics) {
        addFloor(physics);
        CollisionShapeRef shape = physics.voxelShape(blobOf(scene, h), 16);
        for (int i = 0; i < 20; i++) {
            BodyDesc d;
            d.shape = PhysicsShape::fromVoxels(shape, 0.5f);
            d.position = {float(i % 4) * 2.5f - 4.0f, 1.0f + float(i / 4) * 2.2f, float(i % 3) * 0.4f};
            d.angularVelocity = {0.4f * float(i % 3), 0.2f, -0.3f * float(i % 2)};
            physics.createBody(d);
        }
    };
    PhysicsWorld a, b;
    build(a);
    build(b);
    for (int i = 0; i < 120; i++) { a.step(DT); b.step(DT); }
    REQUIRE(a.stateHash() == b.stateHash());
    std::vector<uint8_t> snapshot = a.saveState();
    std::vector<uint64_t> hashes;
    for (int i = 0; i < 120; i++) {
        a.step(DT);
        b.step(DT);
        REQUIRE(a.stateHash() == b.stateHash());
        hashes.push_back(a.stateHash());
    }
    REQUIRE(a.restoreState(snapshot));
    for (int i = 0; i < 120; i++) {
        a.step(DT);
        REQUIRE(a.stateHash() == hashes[size_t(i)]);
    }
}
