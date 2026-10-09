// Reliability (the physics plan, A8 and V3): the cases that break physics engines in games -- fast
// small things, tall stacks, big piles, bodies deleted mid-contact, solver blow-ups, things flung out
// of the world -- each held to a hard pass/fail. The soak at the end runs only with PROJV_SOAK set
// (ctest -L soak).

#include "doctest/doctest.h"

#include <cmath>
#include <cstdlib>
#include <random>
#include <vector>

#include "core/application.h"
#include "runtime/physics.h"
#include "runtime/physics/physics_world.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

namespace {
    using namespace projv::runtime;
    using projv::ComponentHandle;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;
    constexpr float DT = 1.0f / 60.0f;

    // Voxel geometry for these tests: a component filled over [low, high] at `resolution`.
    struct Voxels {
        projv::Scene scene;
        const projv::GeometryBlob& box(ivec3 low, ivec3 high, uint32_t resolution) {
            ComponentHandle h = projv::utils::addComponent(scene, projv::ComponentKind::Chunk, "v",
                                                           projv::INVALID_COMPONENT_HANDLE, resolution, 1.0f);
            std::vector<projv::PendingVoxelOp> ops;
            for (int z = low.z; z <= high.z; z++)
                for (int y = low.y; y <= high.y; y++)
                    for (int x = low.x; x <= high.x; x++) ops.push_back({true, ivec3(x, y, z), 0x3FFFFFFFu});
            projv::utils::queueVoxelAdd(scene, h, ops);
            projv::utils::updateScene(scene);
            const projv::Chunk& chunk = scene.chunks[scene.components[h].chunkHandle];
            return scene.geometryPool[size_t(chunk.geometryPoolIndex)];
        }
    };

    BodyId addStatic(PhysicsWorld& physics, CollisionShapeRef shape, float voxel, vec3 at) {
        BodyDesc d;
        d.shape = PhysicsShape::fromVoxels(shape, voxel);
        d.motion = MotionType::Static;
        d.layer = PhysicsLayer::Static;
        d.position = at;
        return physics.createBody(d);
    }

    // A 1 m crate of 4 x 4 x 4 quarter-metre voxels, and a 40 m voxel floor whose top is y = 0.
    struct Yard {
        Voxels voxels;
        CollisionShapeRef crate, floor;
        explicit Yard(PhysicsWorld& physics) {
            crate = physics.voxelShape(voxels.box({0, 0, 0}, {3, 3, 3}, 16), 16);
            floor = physics.voxelShape(voxels.box({0, 0, 0}, {63, 1, 63}, 64), 64);
            addStatic(physics, floor, 0.625f, vec3(-20.0f, -1.25f, -20.0f));
        }
        BodyId addCrate(PhysicsWorld& physics, vec3 corner, PhysicsLayer layer = PhysicsLayer::Moving) {
            BodyDesc d;
            d.shape = PhysicsShape::fromVoxels(crate, 0.25f);
            d.position = corner;
            d.layer = layer;
            d.density = 500.0f;
            return physics.createBody(d);
        }
    };

    std::vector<uint64_t> runPile(int threads, int ticks) {
        PhysicsSettings settings;
        settings.threads = threads;
        PhysicsWorld physics(settings);
        Yard yard(physics);
        for (int i = 0; i < 120; i++) {
            BodyId id = yard.addCrate(physics, vec3(float(i % 6) * 1.05f - 3.0f, 0.5f + float(i / 36) * 1.2f,
                                                    float((i / 6) % 6) * 1.05f - 3.0f));
            physics.setVelocity(id, vec3(0.1f * float(i % 5), 0, -0.1f * float(i % 3)), vec3(0.2f * float(i % 4), 0, 0));
        }
        std::vector<uint64_t> hashes;
        for (int i = 0; i < ticks; i++) {
            physics.step(DT);
            hashes.push_back(physics.stateHash());
        }
        return hashes;
    }
}

TEST_CASE("the results do not depend on the number of threads") {
    std::vector<uint64_t> single = runPile(0, 300);
    std::vector<uint64_t> four = runPile(4, 300);
    std::vector<uint64_t> seven = runPile(7, 300);
    REQUIRE(single.size() == four.size());
    for (size_t i = 0; i < single.size(); i++) {
        CAPTURE(i);
        REQUIRE(single[i] == four[i]);
        REQUIRE(single[i] == seven[i]);
    }
}

TEST_CASE("tunnel: a thousand one-voxel balls at 200 m/s do not pass a one-voxel wall") {
    PhysicsWorld physics;
    Voxels voxels;
    // A wall 6.4 m square and one 10 cm voxel thick, its face at z = 0.
    CollisionShapeRef wall = physics.voxelShape(voxels.box({0, 0, 0}, {63, 63, 0}, 64), 64);
    REQUIRE(wall);
    addStatic(physics, wall, 0.1f, vec3(-3.2f, -3.2f, 0.0f));
    std::mt19937 random(5);
    std::uniform_real_distribution<float> across(-3.0f, 3.0f), back(-8.0f, -2.0f);
    std::vector<BodyId> balls;
    for (int i = 0; i < 1000; i++) {
        BodyDesc ball;
        ball.shape = PhysicsShape::sphere(0.05f);   // one voxel across
        ball.layer = PhysicsLayer::Debris;          // they pass through each other, not the wall
        ball.position = vec3(across(random), across(random), back(random));
        ball.linearVelocity = vec3(0, 0, 200);      // 3.3 m a step: 33 times its own size
        ball.gravityFactor = 0.0f;
        ball.continuous = true;                     // what RigidBody::Quality::Auto picks for it
        balls.push_back(physics.createBody(ball));
    }
    for (int i = 0; i < 30; i++) physics.step(DT);
    int through = 0;
    for (BodyId id : balls) through += physics.bodyState(id).position.z > 0.1f;
    CHECK(through == 0);
}

TEST_CASE("stack: a ten-crate tower settles, sleeps within 5 s, and then stays put") {
    PhysicsWorld physics;
    Yard yard(physics);
    std::vector<BodyId> tower;
    for (int i = 0; i < 10; i++) tower.push_back(yard.addCrate(physics, vec3(0, 0.02f + float(i) * 1.02f, 0)));
    for (int i = 0; i < 5 * 60; i++) physics.step(DT);
    std::vector<vec3> settled;
    for (BodyId id : tower) {
        CHECK_FALSE(physics.bodyState(id).awake);
        settled.push_back(physics.bodyState(id).position);
    }
    CHECK(settled.back().y == doctest::Approx(9.0f).epsilon(0.01));   // still a tower, ten high
    for (int i = 0; i < 5 * 60; i++) physics.step(DT);
    for (size_t i = 0; i < tower.size(); i++)
        CHECK(glm::length(physics.bodyState(tower[i]).position - settled[i]) < 0.125f);   // half a voxel
}

TEST_CASE("stack: 200-crate piles come to rest and stay at rest") {
    // Several layouts, and some with a little bounce, so this cannot pass by luck: a pile is chaotic,
    // and one arrangement settling says little about the next.
    struct Layout { float gap, drop, jitter, restitution; unsigned seed; };
    for (const Layout& layout : {Layout{0.10f, 0.15f, 0.00f, 0.0f, 1}, Layout{0.05f, 0.30f, 0.04f, 0.0f, 2},
                                 Layout{0.20f, 0.10f, 0.08f, 0.0f, 3}, Layout{0.10f, 0.15f, 0.02f, 0.2f, 4},
                                 Layout{0.12f, 0.25f, 0.05f, 0.3f, 5}}) {
        CAPTURE(layout.seed);
        PhysicsWorld physics;
        Yard yard(physics);
        std::mt19937 random(layout.seed);
        std::uniform_real_distribution<float> jitter(-layout.jitter, layout.jitter);
        std::vector<BodyId> pile;
        float pitch = 1.0f + layout.gap, rise = 1.0f + layout.drop;
        for (int i = 0; i < 200; i++) {
            vec3 at(float(i % 5) * pitch - 2.75f + jitter(random), 0.3f + float(i / 25) * rise,
                    float((i / 5) % 5) * pitch - 2.75f + jitter(random));
            BodyDesc d;
            d.shape = PhysicsShape::fromVoxels(yard.crate, 0.25f);
            d.position = at;
            d.density = 500.0f;
            d.restitution = layout.restitution;
            pile.push_back(physics.createBody(d));
        }
        int ticks = 0;
        do { physics.step(DT); ticks++; } while (physics.stats().awakeBodies > 0 && ticks < 15 * 60);
        CAPTURE(ticks);
        CHECK(physics.stats().awakeBodies == 0);
        CHECK(ticks <= 8 * 60);
        std::vector<vec3> settled;
        for (BodyId id : pile) settled.push_back(physics.bodyState(id).position);
        for (int i = 0; i < 3 * 60; i++) physics.step(DT);
        int drifted = 0, below = 0;
        for (size_t i = 0; i < pile.size(); i++) {
            vec3 p = physics.bodyState(pile[i]).position;
            drifted += glm::length(p - settled[i]) > 0.125f;   // half a voxel
            below += p.y < -0.05f;                              // pushed into the floor
        }
        CHECK(drifted == 0);
        CHECK(below == 0);
        CHECK(physics.stats().stepsWithDroppedContacts == 0);
    }
}

TEST_CASE("delete during contact: a body destroyed every tick for 1000 ticks leaves nothing behind") {
    PhysicsWorld physics;
    Yard yard(physics);
    std::mt19937 random(3);
    std::vector<BodyId> bodies;
    for (int i = 0; i < 100; i++)
        bodies.push_back(yard.addCrate(physics, vec3(float(i % 10) * 1.1f - 5.5f, 0.2f + float(i / 10) * 1.1f, 0)));
    for (int tick = 0; tick < 1000; tick++) {
        size_t victim = std::uniform_int_distribution<size_t>(0, bodies.size() - 1)(random);
        physics.destroyBody(bodies[victim]);
        bodies[victim] = yard.addCrate(physics, vec3(float(tick % 9) - 4.5f, 12.0f, float(tick % 5) - 2.5f));
        physics.step(DT);
    }
    CHECK(physics.stats().bodies == 101);   // the hundred and the floor
    CHECK(physics.stats().stepsWithDroppedContacts == 0);
    CHECK(physics.stats().nonFiniteRecoveries == 0);
    for (BodyId id : bodies) CHECK(physics.isAlive(id));
}

TEST_CASE("a body that blows up is put back where it last was, at rest, and nothing else is touched") {
    PhysicsWorld physics;
    Yard yard(physics);
    BodyId falling = yard.addCrate(physics, vec3(0, 20, 0));
    BodyId other = yard.addCrate(physics, vec3(5, 20, 0));
    for (int i = 0; i < 10; i++) physics.step(DT);
    BodyState before = physics.bodyState(falling);
    physics.corruptBodyForTesting(falling);
    physics.step(DT);
    BodyState after = physics.bodyState(falling);
    CHECK(physics.stats().nonFiniteRecoveries == 1);
    CHECK(after.position == before.position);
    CHECK(glm::length(after.linearVelocity) == 0.0f);
    CHECK_FALSE(after.awake);
    CHECK(physics.bodyState(other).awake);   // still falling
    physics.step(DT);
    CHECK(physics.stats().nonFiniteRecoveries == 1);   // once, not every step after
}

TEST_CASE("a body that leaves the world is reported once per excursion") {
    PhysicsSettings settings;
    settings.worldMin = vec3(-100.0f, -20.0f, -100.0f);
    PhysicsWorld physics(settings);
    BodyDesc d;
    d.shape = PhysicsShape::sphere(0.5f);
    d.position = vec3(0, -15, 0);
    BodyId id = physics.createBody(d);
    std::vector<BodyId> reported;
    for (int i = 0; i < 120; i++) {
        physics.step(DT);
        for (BodyId b : physics.takeBodiesThatLeftTheWorld()) reported.push_back(b);
    }
    REQUIRE(reported.size() == 1);
    CHECK(reported[0] == id);
    CHECK(physics.isAlive(id));   // reported, not deleted: that is the game's call
    CHECK(physics.stats().leftWorld == 1);
}

TEST_CASE("the runtime retires a body that leaves the world, and says where it went") {
    projv::Application app;
    double now = 0.0;
    app.clock = [&] { return now; };
    app.world.ctx().emplace<projv::Scene>();
    installSceneBridge(app);
    PhysicsConfig config;
    config.settings.worldMin = vec3(-100.0f, -10.0f, -100.0f);
    installPhysics(app, config);
    std::vector<projv::BodyLeftWorld> left;
    app.events().on<projv::BodyLeftWorld>([&](const projv::BodyLeftWorld& e) { left.push_back(e); });
    app.runStartup();
    app.runFrame();
    projv::Entity e = app.world.create();
    app.world.emplace<projv::Transform>(e, projv::Transform{vec3(0, 0, 0)});
    projv::RigidBody rigid;
    rigid.shape = projv::RigidBody::Shape::Sphere;
    rigid.radius = 0.5f;
    app.world.emplace<projv::RigidBody>(e, rigid);
    for (int i = 0; i < 120; i++) { now += DT; app.runFrame(); }
    REQUIRE(left.size() == 1);
    CHECK(left[0].entity == e);
    CHECK(left[0].position.y < -10.0f);
    CHECK_FALSE(app.world.valid(e));
    CHECK(physicsWorld(app.world).stats().bodies == 0);
}

TEST_CASE("a sliver of voxels gets a mass and inertia the solver can handle") {
    PhysicsWorld physics;
    Voxels voxels;
    CollisionShapeRef sliver = physics.voxelShape(voxels.box({0, 0, 0}, {0, 0, 0}, 16), 16);   // one voxel
    BodyDesc d;
    d.shape = PhysicsShape::fromVoxels(sliver, 0.01f);   // a centimetre: 1 g of water, at most
    d.density = 1.0f;                                    // ...and far lighter than that
    BodyId id = physics.createBody(d);
    REQUIRE(id.valid());
    CHECK(physics.bodyMass(id) == doctest::Approx(physics.bodyMass(id) < 0.0101f ? 0.01f : physics.bodyMass(id)));
    CHECK(physics.bodyMass(id) >= 0.01f);
    // Spun hard and stepped, it stays finite and under the speed limit.
    physics.setVelocity(id, vec3(0), vec3(1000, 0, 0));
    for (int i = 0; i < 60; i++) physics.step(DT);
    BodyState s = physics.bodyState(id);
    CHECK(std::isfinite(s.position.y));
    CHECK(glm::length(s.angularVelocity) <= 47.2f);
    CHECK(physics.stats().nonFiniteRecoveries == 0);
}

// ---- Soak ----------------------------------------------------------------------------------------
// PROJV_SOAK=1 (ctest -L soak). PROJV_SOAK_SECONDS sets the simulated length (default 600: ten
// minutes) and PROJV_SOAK_BODIES the population (default 2000).

TEST_CASE("soak: thousands of bodies, minutes of churn and explosions, and nothing goes wrong" *
          doctest::skip(std::getenv("PROJV_SOAK") == nullptr)) {
    const int seconds = std::getenv("PROJV_SOAK_SECONDS") ? std::atoi(std::getenv("PROJV_SOAK_SECONDS")) : 600;
    const int population = std::getenv("PROJV_SOAK_BODIES") ? std::atoi(std::getenv("PROJV_SOAK_BODIES")) : 2000;
    PhysicsWorld physics;
    Voxels voxels;
    // A closed 64 m box: floor, ceiling at 30 m and four walls up to it, all half-metre voxels. Closed,
    // so nothing can leave it honestly: anything found outside went through something.
    CollisionShapeRef crate = physics.voxelShape(voxels.box({0, 0, 0}, {3, 3, 3}, 16), 16);
    CollisionShapeRef slab = physics.voxelShape(voxels.box({0, 0, 0}, {127, 1, 127}, 256), 256);
    CollisionShapeRef wallX = physics.voxelShape(voxels.box({0, 0, 0}, {1, 63, 127}, 256), 256);
    CollisionShapeRef wallZ = physics.voxelShape(voxels.box({0, 0, 0}, {127, 63, 1}, 256), 256);
    addStatic(physics, slab, 0.5f, vec3(-32, -1, -32));
    addStatic(physics, slab, 0.5f, vec3(-32, 30, -32));
    addStatic(physics, wallX, 0.5f, vec3(-33, 0, -32));
    addStatic(physics, wallX, 0.5f, vec3(32, 0, -32));
    addStatic(physics, wallZ, 0.5f, vec3(-32, 0, -33));
    addStatic(physics, wallZ, 0.5f, vec3(-32, 0, 32));

    std::mt19937 random(99);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    auto spawn = [&]() {
        BodyDesc d;
        if (unit(random) < 0.5f) {
            d.shape = PhysicsShape::fromVoxels(crate, 0.25f + 0.25f * unit(random));
        } else {
            d.shape = PhysicsShape::sphere(0.3f + 0.6f * unit(random));
        }
        d.position = vec3(unit(random) * 56 - 28, 4 + unit(random) * 20, unit(random) * 56 - 28);
        d.linearVelocity = vec3(unit(random) * 20 - 10, unit(random) * 5, unit(random) * 20 - 10);
        d.angularVelocity = vec3(unit(random) * 6 - 3, unit(random) * 6 - 3, unit(random) * 6 - 3);
        d.continuous = d.shape.kind == PhysicsShape::Kind::Sphere && d.shape.radius < 0.5f;
        return physics.createBody(d);
    };
    std::vector<BodyId> bodies;
    for (int i = 0; i < population; i++) bodies.push_back(spawn());

    int escaped = 0;
    for (int tick = 0; tick < seconds * 60; tick++) {
        // Churn: twenty bodies replaced a step.
        for (int k = 0; k < 20; k++) {
            size_t i = std::uniform_int_distribution<size_t>(0, bodies.size() - 1)(random);
            physics.destroyBody(bodies[i]);
            bodies[i] = spawn();
        }
        // An explosion every two seconds: everything within 8 m flung away.
        if (tick % 120 == 0) {
            vec3 centre(unit(random) * 40 - 20, 1, unit(random) * 40 - 20);
            for (BodyId id : bodies) {
                vec3 d = physics.bodyState(id).position - centre;
                float distance = glm::length(d);
                if (distance < 8.0f && distance > 0.01f)
                    physics.addVelocity(id, d / distance * (8.0f - distance) * 4.0f + vec3(0, 6, 0));
            }
        }
        physics.step(DT);
        if (tick % 60 == 59) {
            for (BodyId id : bodies) {
                vec3 p = physics.bodyState(id).position;
                if (p.y < -1.0f || p.y > 31.0f || std::abs(p.x) > 33.0f || std::abs(p.z) > 33.0f) escaped++;
            }
        }
    }
    const PhysicsStats& stats = physics.stats();
    MESSAGE("soak: " << seconds << " s, " << population << " bodies, " << stats.steps << " steps, "
                     << stats.threads << " threads, last step " << stats.lastStepMilliseconds << " ms");
    CHECK(escaped == 0);
    CHECK(stats.nonFiniteRecoveries == 0);
    CHECK(stats.stepsWithDroppedContacts == 0);
    CHECK(stats.bodies == uint32_t(population + 6));
    CHECK(physics.cachedShapeCount() <= 4);   // memory does not grow with churn
}


TEST_CASE("rest damping leaves alone what is really moving: a rolling ball, a spinning top") {
    PhysicsWorld physics;
    Yard yard(physics);
    BodyDesc ball;
    ball.shape = PhysicsShape::sphere(0.5f);
    ball.position = vec3(-15, 0.5f, 0);
    ball.linearVelocity = vec3(0.3f, 0, 0);       // slow: 5 cm takes a sixth of a second
    ball.angularVelocity = vec3(0, 0, -0.6f);     // already rolling, not sliding
    ball.linearDamping = 0.0f;
    ball.angularDamping = 0.0f;
    BodyId rolling = physics.createBody(ball);

    BodyDesc top;
    top.shape = PhysicsShape::box(vec3(0.5f));
    top.position = vec3(10, 30, 0);
    top.gravityFactor = 0.0f;
    top.angularVelocity = vec3(0, 1.0f, 0);       // turning in place: the angle gives it away
    top.angularDamping = 0.0f;
    BodyId spinning = physics.createBody(top);

    for (int i = 0; i < 5 * 60; i++) physics.step(DT);
    BodyState r = physics.bodyState(rolling), t = physics.bodyState(spinning);
    CHECK(r.awake);
    CHECK(r.position.x > -15.0f + 1.2f);          // covered most of 1.5 m in 5 s
    CHECK(t.awake);
    CHECK(glm::length(t.angularVelocity) == doctest::Approx(1.0f).epsilon(0.02));
}
