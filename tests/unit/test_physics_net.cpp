// Ready for networking (the physics plan, A7, V2 and V4): recordings that replay exactly, NetIds
// that make two peers' simulations agree whatever their local order, stamped commands applied by
// stamp rather than arrival, and a runtime snapshot that rewinds exactly.

#include "doctest/doctest.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

#include "core/application.h"
#include "runtime/net.h"
#include "runtime/physics.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

namespace {
    using namespace projv::runtime;
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;
    constexpr float DT = 1.0f / 60.0f;

    // A recorded scene with a bit of everything: voxel and primitive bodies, a static floor, impulses,
    // velocities, teleports, destruction and creation mid-run, a gravity change, a restore.
    PhysicsLog recordBusyScene() {
        PhysicsSettings settings;
        settings.record = true;
        PhysicsWorld physics(settings);
        projv::Scene scene;
        ComponentHandle h = projv::utils::addComponent(scene, projv::ComponentKind::Chunk, "c",
                                                       projv::INVALID_COMPONENT_HANDLE, 16, 1.0f);
        std::vector<projv::PendingVoxelOp> ops;
        for (int x = 0; x < 4; x++) for (int y = 0; y < 2; y++) ops.push_back({true, ivec3(x, y, 0), 0x3FFFFFFFu});
        ops.push_back({true, ivec3(0, 2, 0), 0x3FFFFFFFu});
        projv::utils::queueVoxelAdd(scene, h, ops);
        projv::utils::updateScene(scene);
        const projv::GeometryBlob& blob =
            scene.geometryPool[size_t(scene.chunks[scene.components[h].chunkHandle].geometryPoolIndex)];
        CollisionShapeRef shape = physics.voxelShape(blob, 16);

        BodyDesc floor;
        floor.shape = PhysicsShape::box({30, 1, 30});
        floor.motion = MotionType::Static;
        floor.layer = PhysicsLayer::Static;
        floor.position = {0, -1, 0};
        physics.createBody(floor);
        std::vector<BodyId> bodies;
        std::mt19937 random(8);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        for (int i = 0; i < 40; i++) {
            BodyDesc d;
            d.shape = i % 2 ? PhysicsShape::sphere(0.4f) : PhysicsShape::fromVoxels(shape, 0.5f);
            d.position = vec3(unit(random) * 6, 2 + i * 0.6f, unit(random) * 6);
            d.restitution = 0.3f;
            bodies.push_back(physics.createBody(d));
        }
        std::vector<uint8_t> snapshot;
        for (int tick = 0; tick < 400; tick++) {
            if (tick % 17 == 0) physics.addImpulse(bodies[size_t(tick) % bodies.size()], vec3(unit(random) * 200, 300, 0));
            if (tick % 23 == 0) physics.setVelocity(bodies[size_t(tick * 3) % bodies.size()], vec3(0, 5, 0), vec3(1, 0, 0));
            if (tick % 41 == 0) physics.setPose(bodies[size_t(tick * 7) % bodies.size()], vec3(0, 12, 0), quat(1, 0, 0, 0));
            if (tick % 31 == 0) {
                size_t i = size_t(tick * 5) % bodies.size();
                physics.destroyBody(bodies[i]);
                BodyDesc d;
                d.shape = PhysicsShape::box(vec3(0.3f, 0.6f, 0.3f));
                d.position = vec3(unit(random) * 4, 10, unit(random) * 4);
                bodies[i] = physics.createBody(d);
            }
            if (tick == 150) physics.setGravity(vec3(0, -4, 0));
            if (tick == 200) snapshot = physics.saveState();
            if (tick == 260) physics.restoreState(snapshot);
            physics.step(DT);
        }
        return physics.recording();
    }
}

TEST_CASE("a recording replays exactly, step for step") {
    PhysicsLog log = recordBusyScene();
    CHECK(log.steps == 400);
    ReplayResult result = PhysicsWorld::replay(log);
    CAPTURE(result.problem);
    CHECK(result.matched);
    CHECK(result.stepsReplayed == 400);
    CHECK(result.firstDivergentStep == 0);
}

TEST_CASE("a replay finds the first step that differs, and says when a recording is broken") {
    PhysicsLog log = recordBusyScene();
    // The last record is the last step, its state hash last: change that and only step 400 differs.
    PhysicsLog tampered = log;
    tampered.bytes.back() ^= 0x40;
    ReplayResult result = PhysicsWorld::replay(tampered);
    CHECK_FALSE(result.matched);
    CHECK(result.firstDivergentStep == 400);
    CHECK_FALSE(result.problem.empty());

    // Lengthen the first step (the first Step record: its op byte, then dt = 1/60): every state
    // from there on differs, and the replay names step 1.
    tampered = log;
    float dt = DT;
    std::vector<uint8_t> pattern(1 + sizeof(float));
    pattern[0] = 10;   // Op::Step
    std::memcpy(pattern.data() + 1, &dt, sizeof(float));
    auto at = std::search(tampered.bytes.begin(), tampered.bytes.end(), pattern.begin(), pattern.end());
    REQUIRE(at != tampered.bytes.end());
    float longer = DT * 1.5f;
    std::memcpy(&*(at + 1), &longer, sizeof(float));
    result = PhysicsWorld::replay(tampered);
    CHECK_FALSE(result.matched);
    CHECK(result.firstDivergentStep == 1);

    PhysicsLog truncated = log;
    truncated.bytes.resize(log.bytes.size() - 5);
    result = PhysicsWorld::replay(truncated);
    CHECK_FALSE(result.matched);
    CHECK(result.problem.find("truncated") != std::string::npos);

    CHECK_FALSE(PhysicsWorld::replay(PhysicsLog{}).matched);
}

TEST_CASE("a recording survives a trip through a file") {
    PhysicsLog log = recordBusyScene();
    std::string path = (std::filesystem::temp_directory_path() / "projv_physics_test.pjvl").string();
    REQUIRE(log.saveToFile(path));
    PhysicsLog loaded;
    REQUIRE(PhysicsLog::loadFromFile(path, loaded));
    std::remove(path.c_str());
    CHECK(loaded.bytes == log.bytes);
    CHECK(PhysicsWorld::replay(loaded).matched);
}

namespace {
    struct App {
        projv::Application app;
        projv::Scene& scene;
        explicit App(bool record = false) : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            installSceneBridge(app);
            PhysicsConfig config;
            config.settings.record = record;
            installPhysics(app, config);
            app.runStartup();
            app.runFrame();
        }
        double now = 0.0;
        void frames(int n) { for (int i = 0; i < n; i++) { now += DT; app.runFrame(); } }
        Entity ball(vec3 at, uint32_t netId = 0) {
            Entity e = app.world.create();
            app.world.emplace<projv::Transform>(e, projv::Transform{at});
            if (netId) app.world.emplace<projv::NetId>(e, projv::NetId{netId});
            projv::RigidBody rigid;
            rigid.shape = projv::RigidBody::Shape::Sphere;
            rigid.radius = 0.5f;
            rigid.restitution = 0.4f;
            app.world.emplace<projv::RigidBody>(e, rigid);
            return e;
        }
        Entity floor() {
            Entity e = app.world.create();
            app.world.emplace<projv::Transform>(e, projv::Transform{vec3(0, -1, 0)});
            projv::RigidBody rigid;
            rigid.motion = MotionType::Static;
            rigid.shape = projv::RigidBody::Shape::Box;
            rigid.halfExtents = vec3(20, 1, 20);
            rigid.layer = PhysicsLayer::Static;
            app.world.emplace<projv::RigidBody>(e, rigid);
            return e;
        }
    };
}

TEST_CASE("the runtime's whole physics pipeline replays exactly") {
    App a(true);
    a.floor();
    std::vector<Entity> balls;
    for (int i = 0; i < 30; i++) balls.push_back(a.ball(vec3(float(i % 6) - 3, 1 + float(i / 6) * 1.2f, 0)));
    for (int f = 0; f < 300; f++) {
        if (f % 20 == 0) addImpulse(a.app.world, balls[size_t(f / 20) % balls.size()], vec3(0, 400, 100));
        if (f == 100) { a.app.world.destroy(balls[3]); balls[3] = a.ball(vec3(0, 8, 0)); }
        if (f == 150) setGravity(a.app.world, vec3(0, -20, 0));
        a.frames(1);
    }
    PhysicsLog log = physicsWorld(a.app.world).recording();
    CHECK(log.steps >= 300);
    ReplayResult result = PhysicsWorld::replay(log);
    CAPTURE(result.problem);
    CHECK(result.matched);
}

TEST_CASE("two peers with the same NetIds agree, whatever order their entities were made in") {
    App a, b;
    a.floor();
    b.floor();
    // The same twenty balls with the same NetIds -- made forwards in one world, backwards in the other.
    for (int i = 0; i < 20; i++) a.ball(vec3(float(i % 5) - 2, 1 + float(i / 5), 0), uint32_t(100 + i));
    for (int i = 19; i >= 0; i--) b.ball(vec3(float(i % 5) - 2, 1 + float(i / 5), 0), uint32_t(100 + i));
    for (int f = 0; f < 240; f++) {
        // A command named by NetId, from a "server", to both.
        if (f % 30 == 0) {
            PhysicsCommand c;
            c.kind = PhysicsCommand::Kind::AddVelocity;
            c.target = projv::NetId{uint32_t(100 + (f / 30) % 20)};
            c.a = vec3(0, 6, 2);
            c.tick = nextPhysicsTick(a.app.world);
            c.issuer = 1;
            submitCommand(a.app.world, c);
            c.tick = nextPhysicsTick(b.app.world);
            submitCommand(b.app.world, c);
        }
        a.frames(1);
        b.frames(1);
        REQUIRE(physicsStateHash(a.app.world) == physicsStateHash(b.app.world));
    }
}

TEST_CASE("commands apply by stamp, not by arrival") {
    auto run = [](bool reversed) {
        App a;
        Entity e = a.ball(vec3(0, 30, 0), 7);
        a.frames(1);
        PhysicsCommand set;
        set.kind = PhysicsCommand::Kind::SetVelocity;
        set.target = projv::NetId{7};
        set.a = vec3(0, 0, 0);
        set.tick = nextPhysicsTick(a.app.world);
        set.issuer = 1;
        PhysicsCommand add = set;
        add.kind = PhysicsCommand::Kind::AddVelocity;
        add.a = vec3(5, 0, 0);
        add.issuer = 2;   // after issuer 1, whatever order they arrive in
        if (reversed) { submitCommand(a.app.world, add); submitCommand(a.app.world, set); }
        else { submitCommand(a.app.world, set); submitCommand(a.app.world, add); }
        a.frames(1);
        return bodyPose(a.app.world, e).linearVelocity.x;
    };
    float forwards = run(false), backwards = run(true);
    CHECK(forwards == doctest::Approx(5.0f).epsilon(0.01));   // set to zero, then plus five
    CHECK(backwards == forwards);
}

TEST_CASE("a command stamped for a step already run applies at the next one, and is counted") {
    App a;
    Entity e = a.ball(vec3(0, 30, 0), 9);
    a.frames(5);
    PhysicsCommand c;
    c.kind = PhysicsCommand::Kind::SetVelocity;
    c.target = projv::NetId{9};
    c.a = vec3(3, 0, 0);
    c.tick = 2;   // long gone
    submitCommand(a.app.world, c);
    a.frames(1);
    CHECK(lateCommands(a.app.world) == 1);
    CHECK(bodyPose(a.app.world, e).linearVelocity.x == doctest::Approx(3.0f).epsilon(0.01));

    // And one for a later step waits for it.
    c.tick = nextPhysicsTick(a.app.world) + 3;
    c.a = vec3(-8, 0, 0);
    submitCommand(a.app.world, c);
    a.frames(1);
    CHECK(bodyPose(a.app.world, e).linearVelocity.x > 0.0f);
    a.frames(3);
    CHECK(bodyPose(a.app.world, e).linearVelocity.x == doctest::Approx(-8.0f).epsilon(0.01));
}

TEST_CASE("the runtime rewinds exactly: restore, step on, same states") {
    App a;
    a.floor();
    for (int i = 0; i < 25; i++) a.ball(vec3(float(i % 5) - 2, 1 + float(i / 5) * 1.1f, 0), uint32_t(10 + i));
    a.frames(60);
    // A command waiting for later is part of the state too.
    PhysicsCommand later;
    later.kind = PhysicsCommand::Kind::AddVelocity;
    later.target = projv::NetId{12};
    later.a = vec3(0, 9, 0);
    later.tick = nextPhysicsTick(a.app.world) + 20;
    submitCommand(a.app.world, later);
    std::vector<uint8_t> snapshot = savePhysics(a.app.world);
    std::vector<uint64_t> first;
    for (int f = 0; f < 90; f++) { a.frames(1); first.push_back(physicsStateHash(a.app.world)); }
    REQUIRE(restorePhysics(a.app.world, snapshot));
    for (int f = 0; f < 90; f++) {
        a.frames(1);
        REQUIRE(physicsStateHash(a.app.world) == first[size_t(f)]);
    }
    // A different set of bodies is refused, with nothing changed.
    a.ball(vec3(0, 20, 0), 99);
    a.frames(1);
    uint64_t before = physicsStateHash(a.app.world);
    CHECK_FALSE(restorePhysics(a.app.world, snapshot));
    CHECK(physicsStateHash(a.app.world) == before);
}

TEST_CASE("NetIds: assigned past every id in use, found by value, saved in entities.json") {
    projv::World world;
    installNetIds(world);
    Entity authored = world.create();
    world.emplace<projv::NetId>(authored, projv::NetId{40});
    Entity spawned = world.create();
    CHECK(assignNetId(world, spawned).value == 41);
    CHECK(assignNetId(world, spawned).value == 41);   // keeps the one it has
    CHECK(entityForNetId(world, projv::NetId{40}) == authored);
    CHECK(entityForNetId(world, projv::NetId{41}) == spawned);
    world.destroy(authored);
    CHECK(entityForNetId(world, projv::NetId{40}) == projv::Entity(projv::NullEntity));
    CHECK(assignNetId(world, world.create()).value == 42);   // never reused

    using Traits = ComponentTraits<projv::NetId>;
    auto back = Traits::load(Traits::save(projv::NetId{77}), 1);
    REQUIRE(back);
    CHECK(back->value == 77);
    CHECK_FALSE(Traits::load(nlohmann::json{{"id", 0}}, 1));
}
