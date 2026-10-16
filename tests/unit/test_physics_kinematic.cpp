// Kinematic bodies that static geometry stops (the physics plan, R7), asked for per body: a move into
// a static body ends where it touches (Stop), or goes on along it (Slide). Sensors, filtered pairs and
// moving bodies do not stop it; a body already touching moves along and out freely. Recorded and
// replayed like every move.

#include "doctest/doctest.h"

#include <vector>

#include "core/application.h"
#include "runtime/physics.h"
#include "runtime/physics/physics_world.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

namespace {
    using namespace projv::runtime;
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;
    namespace utils = projv::utils;
    constexpr float DT = 1.0f / 60.0f;
    const quat ID(1, 0, 0, 0);

    BodyId addStatic(PhysicsWorld& physics, vec3 centre, vec3 half, PhysicsLayer layer = PhysicsLayer::Static,
                     CollisionFilter filter = {}) {
        BodyDesc d;
        d.shape = PhysicsShape::box(half);
        d.motion = MotionType::Static;
        d.layer = layer;
        d.position = centre;
        d.filter = filter;
        return physics.createBody(d);
    }
    BodyId addFloor(PhysicsWorld& physics) { return addStatic(physics, {0, -0.5f, 0}, {20, 0.5f, 20}); }
    BodyId kinematicBox(PhysicsWorld& physics, vec3 at, KinematicBlocking blocking, CollisionFilter filter = {}) {
        BodyDesc d;
        d.shape = PhysicsShape::box({0.5f, 0.5f, 0.5f});
        d.motion = MotionType::Kinematic;
        d.position = at;
        d.blockedByStatic = blocking;
        d.filter = filter;
        return physics.createBody(d);
    }
    // Drives a kinematic body at `velocity` for n steps, from where it is each step.
    void drive(PhysicsWorld& physics, BodyId id, vec3 velocity, int n) {
        for (int i = 0; i < n; i++) {
            physics.moveKinematic(id, physics.bodyState(id).position + velocity * DT, ID, DT);
            physics.step(DT);
        }
    }
    vec3 at(PhysicsWorld& physics, BodyId id) { return physics.bodyState(id).position; }
}

TEST_CASE("kinematic: by default it goes through the floor; Stop ends the move where it touches") {
    PhysicsWorld physics;
    addFloor(physics);
    BodyId through = kinematicBox(physics, {-3, 3, 0}, KinematicBlocking::None);
    BodyId stops = kinematicBox(physics, {3, 3, 0}, KinematicBlocking::Stop);
    drive(physics, through, {0, -30, 0}, 30);
    drive(physics, stops, {0, -30, 0}, 30);
    CHECK(at(physics, through).y < -5.0f);
    CHECK(at(physics, stops).y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(at(physics, stops).y >= 0.5f);
    CHECK(at(physics, stops).x == doctest::Approx(3.0f));
    // Stopped, not slid: a move into the floor at a slant goes nowhere along it either.
    drive(physics, stops, {10, -10, 0}, 20);
    CHECK(at(physics, stops).x == doctest::Approx(3.0f).epsilon(0.01));
    // Along it, or away from it, it moves freely.
    drive(physics, stops, {6, 0, 0}, 30);
    CHECK(at(physics, stops).x == doctest::Approx(6.0f).epsilon(0.02));
    drive(physics, stops, {0, 6, 0}, 30);
    CHECK(at(physics, stops).y == doctest::Approx(3.5f).epsilon(0.02));
}

TEST_CASE("kinematic: Slide carries the rest of a move on along the floor, and into a corner") {
    PhysicsWorld physics;
    addFloor(physics);
    addStatic(physics, {6, 2, 0}, {0.5f, 2, 5});   // a wall at x = 5.5
    BodyId box = kinematicBox(physics, {0, 2, 0}, KinematicBlocking::Slide);
    drive(physics, box, {3, -3, 0}, 30);           // 1.5 m down, 1.5 m along
    CHECK(at(physics, box).y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(at(physics, box).x == doctest::Approx(1.5f).epsilon(0.05));
    // Down and into the wall at once: it ends in the corner, on the floor against the wall, and
    // slides along the wall's length.
    drive(physics, box, {12, -6, 3}, 60);
    CHECK(at(physics, box).y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(at(physics, box).x == doctest::Approx(5.0f).epsilon(0.02));
    CHECK(at(physics, box).x <= 5.0f);
    CHECK(at(physics, box).z > 2.5f);
}

TEST_CASE("kinematic: sensors, bodies it passes through, and moving bodies do not stop it") {
    PhysicsWorld physics;
    addStatic(physics, {-4, 0, 0}, {1, 1, 1}, PhysicsLayer::Sensor);
    addStatic(physics, {0, 0, 0}, {1, 1, 1}, PhysicsLayer::Static, {.groups = 1});
    BodyDesc crate;
    crate.shape = PhysicsShape::box({0.5f, 0.5f, 0.5f});
    crate.position = {4, 0, 0};
    crate.gravityFactor = 0.0f;
    BodyId pushed = physics.createBody(crate);
    BodyId box = kinematicBox(physics, {-8, 0, 0}, KinematicBlocking::Stop, {.ignores = 1});
    drive(physics, box, {8, 0, 0}, 90);   // 12 m: through the sensor, the ignored block, into the crate
    CHECK(at(physics, box).x == doctest::Approx(4.0f).epsilon(0.02));
    CHECK(at(physics, pushed).x > 4.5f);   // shoved aside
}

TEST_CASE("kinematic: a blocked body touching the floor reports the contact") {
    PhysicsWorld physics;
    BodyId floor = addFloor(physics);
    BodyDesc d;
    d.shape = PhysicsShape::box({0.5f, 0.5f, 0.5f});
    d.motion = MotionType::Kinematic;
    d.position = {0, 2, 0};
    d.blockedByStatic = KinematicBlocking::Stop;
    d.reportContacts = true;
    BodyId box = physics.createBody(d);
    int began = 0;
    for (int i = 0; i < 40; i++) {
        physics.moveKinematic(box, at(physics, box) + vec3(0, -6, 0) * DT, ID, DT);
        physics.step(DT);
        for (const ContactChange& c : physics.contactChanges())
            began += c.began && (c.a == floor || c.b == floor) && (c.a == box || c.b == box);
    }
    CHECK(began == 1);
}

TEST_CASE("kinematic: blocked moves are recorded and replay the same") {
    PhysicsSettings settings;
    settings.record = true;
    PhysicsWorld physics(settings);
    addFloor(physics);
    addStatic(physics, {3, 1, 0}, {0.5f, 1, 3});
    BodyId a = kinematicBox(physics, {0, 3, 0}, KinematicBlocking::Slide);
    BodyId b = kinematicBox(physics, {0, 3, 3}, KinematicBlocking::Stop);
    BodyDesc crate;
    crate.shape = PhysicsShape::box({0.3f, 0.3f, 0.3f});
    crate.position = {1, 0.3f, 0};
    physics.createBody(crate);
    for (int i = 0; i < 120; i++) {
        physics.moveKinematic(a, at(physics, a) + vec3(4, -3, 1) * DT, ID, DT);
        physics.moveKinematic(b, at(physics, b) + vec3(5, -4, 0) * DT, ID, DT);
        physics.step(DT);
    }
    ReplayResult replayed = PhysicsWorld::replay(physics.recording());
    CHECK(replayed.matched);
    CHECK(replayed.problem.empty());
}

// ---- Entities ------------------------------------------------------------------------------------

namespace {
    struct Fixture {
        projv::Application app;
        projv::Scene& scene;
        double now = 0.0;
        Fixture() : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            installSceneBridge(app);
            installPhysics(app);
            app.runStartup();
            app.runFrame();
        }
        void frames(int n) { for (int i = 0; i < n; i++) { now += DT; app.runFrame(); } }
        ComponentHandle block(const char* name, vec3 corner, ivec3 size, float voxel) {
            ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, name, INVALID_COMPONENT_HANDLE, 16, voxel);
            utils::setComponentTransform(scene, h, corner, ID, 1.0f);
            std::vector<projv::PendingVoxelOp> ops;
            for (int z = 0; z < size.z; z++)
                for (int y = 0; y < size.y; y++)
                    for (int x = 0; x < size.x; x++) ops.push_back({true, ivec3(x, y, z), 0x3FFFFFFFu});
            utils::queueVoxelAdd(scene, h, ops);
            utils::updateScene(scene);
            return h;
        }
        Entity floor() {
            Entity e = spawnComponent(app.world, block("floor", vec3(-10, -1, -10), ivec3(80, 4, 80), 0.25f));
            app.world.emplace<projv::StaticCollider>(e);
            return e;
        }
        // A 1 m kinematic ball, blocked by static geometry as asked.
        Entity ball(vec3 corner, KinematicBlocking blocking) {
            Entity e = spawnComponent(app.world, block("ball", corner, ivec3(4), 0.25f), projv::LinkMode::Root,
                                      projv::OnUnlink::Destroy);
            projv::RigidBody rigid;
            rigid.motion = MotionType::Kinematic;
            rigid.shape = projv::RigidBody::Shape::Sphere;
            rigid.blockedByStatic = blocking;
            app.world.emplace<projv::RigidBody>(e, rigid);
            return e;
        }
    };
}

TEST_CASE("kinematic: a ball flying on a ConstantVelocity into the floor rolls on along it") {
    Fixture f;
    projv::World& world = f.app.world;
    f.floor();
    Entity sliding = f.ball(vec3(0, 3, 0), KinematicBlocking::Slide);
    Entity through = f.ball(vec3(0, 3, 4), KinematicBlocking::None);
    world.emplace<projv::ConstantVelocity>(sliding, projv::ConstantVelocity{vec3(2, -4, 0)});
    world.emplace<projv::ConstantVelocity>(through, projv::ConstantVelocity{vec3(2, -4, 0)});
    f.frames(90);   // 1.5 s: 6 m down, 3 m along
    // Its position is the voxels' corner: the sphere's centre is half a metre in from it.
    vec3 ball = bodyPose(world, sliding).position;
    CHECK(ball.y == doctest::Approx(0.0f).epsilon(0.01));    // resting on the floor
    CHECK(ball.x == doctest::Approx(3.0f).epsilon(0.01));    // the full 3 m along
    CHECK(bodyPose(world, through).position.y < -2.0f);
    // Physics writes its Transform, as it does a dynamic body's.
    CHECK(world.get<projv::Transform>(sliding).position.y == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("kinematic: a blocked body sent into a wall by its Transform stops there, and holds") {
    Fixture f;
    projv::World& world = f.app.world;
    f.floor();
    Entity wall = spawnComponent(world, f.block("wall", vec3(4, 0, -2), ivec3(4, 8, 16), 0.25f));
    world.emplace<projv::StaticCollider>(wall);
    Entity ball = f.ball(vec3(0, 0.25f, 0), KinematicBlocking::Stop);
    f.frames(3);
    world.patch<projv::Transform>(ball, [](projv::Transform& t) { t.position = vec3(8, 0.75f, 0.5f); });   // beyond the wall
    f.frames(3);
    vec3 stopped = bodyPose(world, ball).position;
    CHECK(stopped.x == doctest::Approx(3.0f).epsilon(0.01));   // its corner; the sphere touching x = 4
    CHECK(stopped.x < 3.0f);
    CHECK(world.get<projv::Transform>(ball).position.x == doctest::Approx(stopped.x).epsilon(0.01));
    f.frames(60);   // left as physics wrote it: it stays
    CHECK(bodyPose(world, ball).position.x == doctest::Approx(stopped.x));
    // Sent back: it goes.
    world.patch<projv::Transform>(ball, [](projv::Transform& t) { t.position = vec3(1, 0.75f, 0.5f); });
    f.frames(2);
    CHECK(bodyPose(world, ball).position.x == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("kinematic: blockedByStatic saves by name") {
    using projv::runtime::ComponentTraits;
    projv::RigidBody b;
    b.motion = MotionType::Kinematic;
    b.blockedByStatic = KinematicBlocking::Slide;
    nlohmann::json j = ComponentTraits<projv::RigidBody>::save(b);
    CHECK(j["blockedByStatic"] == "slide");
    auto back = ComponentTraits<projv::RigidBody>::load(j, 1);
    REQUIRE(back);
    CHECK(back->blockedByStatic == KinematicBlocking::Slide);
    CHECK_FALSE(ComponentTraits<projv::RigidBody>::save(projv::RigidBody{}).contains("blockedByStatic"));
    CHECK_FALSE(ComponentTraits<projv::RigidBody>::load(nlohmann::json{{"blockedByStatic", "bounce"}}, 1));
}
