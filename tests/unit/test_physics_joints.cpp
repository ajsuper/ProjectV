// Joints (PhysicsWorld::createJoint, runtime::Joint): each type doing what it says -- against the
// physics it should obey where there is a closed form -- breaking when it should, going with its
// bodies, and keeping determinism, replay and rewind.

#include "doctest/doctest.h"

#include <cmath>
#include <vector>

#include "core/application.h"
#include "runtime/physics.h"
#include "runtime/physics/physics_world.h"

namespace {
    using namespace projv::runtime;
    using projv::Entity;
    using projv::core::quat;
    using projv::core::vec3;
    constexpr float DT = 1.0f / 60.0f;
    constexpr float PI = 3.14159265f;

    BodyId ball(PhysicsWorld& physics, vec3 at, float radius = 0.2f, float mass = 1.0f) {
        BodyDesc d;
        d.shape = PhysicsShape::sphere(radius);
        d.position = at;
        d.mass = mass;
        d.linearDamping = d.angularDamping = 0.0f;
        return physics.createBody(d);
    }
    BodyId box(PhysicsWorld& physics, vec3 at, vec3 half, float mass = 10.0f) {
        BodyDesc d;
        d.shape = PhysicsShape::box(half);
        d.position = at;
        d.mass = mass;
        return physics.createBody(d);
    }
}

TEST_CASE("a pendulum on a rod (a distance joint) keeps its length and swings at 2 pi sqrt(L/g)") {
    PhysicsWorld physics;
    const float length = 2.0f;
    // Hung from the world at the origin, pulled 10 degrees aside: a small swing, close to the ideal.
    float start = 10.0f * PI / 180.0f;
    BodyId bob = ball(physics, vec3(std::sin(start) * length, -std::cos(start) * length, 0), 0.05f, 1.0f);
    JointDesc d;
    d.type = JointType::Distance;   // a rod: the distance held, turning free
    d.a = bob;
    d.anchor = physics.bodyState(bob).position;
    d.anchorB = vec3(0);
    JointId rod = physics.createJoint(d);
    REQUIRE(rod.valid());
    // Time the swings by the sign changes of x.
    int crossings = 0;
    float firstCrossing = 0.0f, lastCrossing = 0.0f, previousX = physics.bodyState(bob).position.x;
    float worst = 0.0f;
    for (int i = 1; i <= 12 * 60; i++) {
        physics.step(DT);
        vec3 p = physics.bodyState(bob).position;
        worst = std::max(worst, std::abs(glm::length(p) - length));
        if ((p.x < 0.0f) != (previousX < 0.0f)) {
            if (crossings == 0) firstCrossing = float(i) * DT;
            lastCrossing = float(i) * DT;
            crossings++;
        }
        previousX = p.x;
    }
    REQUIRE(crossings >= 6);   // 2.84 s a swing: eight half-swings in 12 s
    float period = 2.0f * (lastCrossing - firstCrossing) / float(crossings - 1);
    float ideal = 2.0f * PI * std::sqrt(length / 9.81f);
    CHECK(period == doctest::Approx(ideal).epsilon(0.03));
    CHECK(worst < 0.01f * length);   // the rod does not stretch
}

TEST_CASE("a hinge turns only about its axis, within its limits, and its motor drives it") {
    PhysicsWorld physics;
    physics.setGravity(vec3(0));
    // A door: 1 m wide, hinged to the world along its left edge, which runs up the y axis.
    BodyId door = box(physics, vec3(0.5f, 1, 0), vec3(0.5f, 1, 0.05f));
    JointDesc d;
    d.type = JointType::Hinge;
    d.a = door;
    d.anchor = vec3(0, 1, 0);
    d.axis = vec3(0, 1, 0);
    d.limitMin = -PI * 0.5f;
    d.limitMax = 0.0f;
    JointId hinge = physics.createJoint(d);
    REQUIRE(hinge.valid());
    // Shoved hard: it swings open until the stop, and the hinge edge stays put.
    physics.addImpulse(door, vec3(0, 0, 40));
    float widest = 0.0f;
    for (int i = 0; i < 120; i++) {
        physics.step(DT);
        widest = std::min(widest, physics.jointState(hinge).position);
    }
    CHECK(widest > -PI * 0.5f - 0.06f);   // a stop is reached within a step: up to half a step's travel past it
    CHECK(widest < -0.3f);   // it did swing
    BodyState s = physics.bodyState(door);
    vec3 edge = s.position + s.rotation * vec3(-0.5f, 0, 0);
    CHECK(glm::length(vec3(edge.x, 0, edge.z)) < 0.01f);
    CHECK(std::abs(s.position.y - 1.0f) < 0.01f);

    // A motor on a free hinge: up to speed, and held there.
    BodyId wheel = box(physics, vec3(5, 0, 0), vec3(1, 0.1f, 0.1f));
    JointDesc m;
    m.type = JointType::Hinge;
    m.a = wheel;
    m.anchor = vec3(5, 0, 0);
    m.axis = vec3(0, 0, 1);
    m.motor = true;
    m.motorVelocity = 3.0f;
    JointId motor = physics.createJoint(m);
    for (int i = 0; i < 60; i++) physics.step(DT);
    CHECK(physics.bodyState(wheel).angularVelocity.z == doctest::Approx(3.0f).epsilon(0.02));
    physics.setJointMotor(motor, true, -1.0f);
    for (int i = 0; i < 60; i++) physics.step(DT);
    CHECK(physics.bodyState(wheel).angularVelocity.z == doctest::Approx(-1.0f).epsilon(0.02));
}

TEST_CASE("a slider moves only along its axis, and stops at its limit") {
    PhysicsWorld physics;
    physics.setGravity(vec3(3, -9.81f, 0));   // pulling along +x, and down
    BodyId block = box(physics, vec3(0, 0, 0), vec3(0.25f));
    JointDesc d;
    d.type = JointType::Slider;
    d.a = block;
    d.anchor = vec3(0);
    d.axis = vec3(1, 0, 0);
    d.limitMin = -1.0f;
    d.limitMax = 2.0f;
    JointId slider = physics.createJoint(d);
    for (int i = 0; i < 4 * 60; i++) physics.step(DT);
    BodyState s = physics.bodyState(block);
    CHECK(s.position.x == doctest::Approx(2.0f).epsilon(0.01));   // at the stop
    CHECK(std::abs(s.position.y) < 0.01f);                         // not falling
    CHECK(physics.jointState(slider).position == doctest::Approx(2.0f).epsilon(0.01));
}

TEST_CASE("a fixed joint welds two bodies: they fall and tumble as one") {
    PhysicsWorld physics;
    BodyDesc floor;
    floor.shape = PhysicsShape::box(vec3(20, 1, 20));
    floor.motion = MotionType::Static;
    floor.layer = PhysicsLayer::Static;
    floor.position = vec3(0, -1, 0);
    physics.createBody(floor);
    BodyId a = box(physics, vec3(0, 4, 0), vec3(0.5f)), b = box(physics, vec3(1.0f, 4.5f, 0), vec3(0.5f));
    JointDesc d;
    d.type = JointType::Fixed;
    d.a = a;
    d.b = b;
    d.anchor = vec3(0.5f, 4.25f, 0);
    physics.createJoint(d);
    physics.setVelocity(a, vec3(1, 0, 0), vec3(0, 0, 2));
    vec3 offset0 = physics.bodyState(b).position - physics.bodyState(a).position;
    for (int i = 0; i < 3 * 60; i++) physics.step(DT);
    BodyState sa = physics.bodyState(a), sb = physics.bodyState(b);
    vec3 local = glm::conjugate(sa.rotation) * (sb.position - sa.position);
    CHECK(glm::length(local - offset0) < 0.02f);   // b where it was relative to a
    CHECK(std::abs(glm::dot(sa.rotation, sb.rotation)) > 0.9999f);
}

TEST_CASE("a rope holds at its length; a spring stretches by the weight it holds") {
    PhysicsWorld physics;
    BodyId hanging = ball(physics, vec3(0, -1, 0), 0.1f, 2.0f);
    JointDesc rope;
    rope.type = JointType::Distance;
    rope.a = hanging;
    rope.anchor = vec3(0, -1, 0);
    rope.anchorB = vec3(0, 0, 0);
    rope.limitMin = 0.0f;
    rope.limitMax = 1.5f;   // slack: it falls half a metre, then the rope catches it
    JointId r = physics.createJoint(rope);
    for (int i = 0; i < 4 * 60; i++) physics.step(DT);
    CHECK(physics.bodyState(hanging).position.y == doctest::Approx(-1.5f).epsilon(0.01));
    CHECK(physics.jointState(r).position == doctest::Approx(1.5f).epsilon(0.01));

    // A spring: k = m (2 pi f)^2 = 1 * (2 pi)^2 = 39.5 N/m holding 1 kg stretches 0.248 m.
    BodyId bob = ball(physics, vec3(5, -1, 0), 0.1f, 1.0f);
    JointDesc spring;
    spring.type = JointType::Distance;
    spring.a = bob;
    spring.anchor = vec3(5, -1, 0);
    spring.anchorB = vec3(5, 0, 0);
    spring.limitMin = spring.limitMax = 1.0f;
    spring.springFrequency = 1.0f;
    spring.springDamping = 1.0f;   // critically damped: it settles rather than bouncing for ever
    physics.createJoint(spring);
    for (int i = 0; i < 6 * 60; i++) physics.step(DT);
    float stretch = -physics.bodyState(bob).position.y - 1.0f;
    CHECK(stretch == doctest::Approx(9.81f / (4.0f * PI * PI)).epsilon(0.05));
}

TEST_CASE("a joint breaks when it holds more than its breakForce, and only then") {
    PhysicsWorld physics;
    // Two 50 kg boxes hung from the world: about 490 N each. One joint can take 400 N, one 600 N.
    BodyId weak = box(physics, vec3(0, 0, 0), vec3(0.25f), 50.0f), strong = box(physics, vec3(3, 0, 0), vec3(0.25f), 50.0f);
    JointDesc d;
    d.type = JointType::Point;
    d.a = weak;
    d.anchor = vec3(0, 0.25f, 0);
    d.breakForce = 400.0f;
    JointId weakJoint = physics.createJoint(d);
    d.a = strong;
    d.anchor = vec3(3, 0.25f, 0);
    d.breakForce = 600.0f;
    JointId strongJoint = physics.createJoint(d);
    std::vector<PhysicsWorld::BrokenJoint> broken;
    for (int i = 0; i < 120; i++) {
        physics.step(DT);
        for (auto b : physics.takeBrokenJoints()) broken.push_back(b);
    }
    REQUIRE(broken.size() == 1);
    CHECK(broken[0].id == weakJoint);
    CHECK(broken[0].force > 400.0f);
    CHECK_FALSE(physics.isAlive(weakJoint));
    CHECK(physics.isAlive(strongJoint));
    CHECK(physics.jointState(strongJoint).force == doctest::Approx(50.0f * 9.81f).epsilon(0.02));
    CHECK(physics.bodyState(weak).position.y < -5.0f);    // fell
    CHECK(std::abs(physics.bodyState(strong).position.y) < 0.01f);
}

TEST_CASE("destroying a body removes its joints") {
    PhysicsWorld physics;
    BodyId a = ball(physics, vec3(0)), b = ball(physics, vec3(1, 0, 0));
    JointDesc d;
    d.type = JointType::Point;
    d.a = a;
    d.b = b;
    d.anchor = vec3(0.5f, 0, 0);
    JointId j = physics.createJoint(d);
    REQUIRE(physics.isAlive(j));
    physics.destroyBody(b);
    CHECK_FALSE(physics.isAlive(j));
    for (int i = 0; i < 30; i++) physics.step(DT);   // and nothing refers to it any more
    CHECK(physics.bodyState(a).position.y < -0.5f);
    // Unusable joints are refused.
    d.b = b;   // gone
    CHECK_FALSE(physics.createJoint(d).valid());
    d.b = a;   // to itself
    CHECK_FALSE(physics.createJoint(d).valid());
}

namespace {
    // A ten-link chain hung from the world, kicked: something for determinism to get wrong.
    struct Chain {
        std::vector<BodyId> links;
        explicit Chain(PhysicsWorld& physics) {
            BodyId previous;
            for (int i = 0; i < 10; i++) {
                BodyId link = box(physics, vec3(float(i) * 0.5f + 0.25f, 5, 0), vec3(0.25f, 0.05f, 0.05f), 1.0f);
                JointDesc d;
                d.type = JointType::Point;
                d.a = link;
                d.b = previous;
                d.anchor = vec3(float(i) * 0.5f, 5, 0);
                physics.createJoint(d);
                links.push_back(link);
                previous = link;
            }
        }
    };
}

TEST_CASE("a chain of joints is stable, deterministic, replays and rewinds") {
    PhysicsSettings settings;
    settings.record = true;
    PhysicsWorld physics(settings);
    Chain chain(physics);
    physics.addImpulse(chain.links.back(), vec3(0, 0, 3));
    for (int i = 0; i < 120; i++) physics.step(DT);
    std::vector<uint8_t> snapshot = physics.saveState();
    std::vector<uint64_t> hashes;
    for (int i = 0; i < 240; i++) {
        physics.step(DT);
        hashes.push_back(physics.stateHash());
    }
    // Hanging, and never longer than it is: the last link's centre is 4.75 m along the chain from
    // where it hangs, and no further away than that at any moment.
    float farthest = 0.0f;
    BodyState last;
    for (int i = 0; i < 120; i++) {
        physics.step(DT);
        last = physics.bodyState(chain.links.back());
        farthest = std::max(farthest, glm::length(last.position - vec3(0, 5, 0)));
    }
    CHECK(last.position.y < 4.0f);
    CHECK(farthest < 4.75f * 1.01f);
    REQUIRE(physics.restoreState(snapshot));
    for (int i = 0; i < 240; i++) {
        physics.step(DT);
        REQUIRE(physics.stateHash() == hashes[size_t(i)]);
    }
    ReplayResult replay = PhysicsWorld::replay(physics.recording());
    CAPTURE(replay.problem);
    CHECK(replay.matched);
}

TEST_CASE("runtime joints: made when both bodies exist, broken and reported, motors by command") {
    projv::Application app;
    double now = 0.0;
    app.clock = [&] { return now; };
    app.world.ctx().emplace<projv::Scene>();
    installSceneBridge(app);
    installPhysics(app);
    std::vector<projv::JointBroke> broke;
    app.events().on<projv::JointBroke>([&](const projv::JointBroke& e) { broke.push_back(e); });
    app.runStartup();
    app.runFrame();
    auto frames = [&](int n) { for (int i = 0; i < n; i++) { now += DT; app.runFrame(); } };
    auto boxEntity = [&](vec3 at, float mass) {
        Entity e = app.world.create();
        app.world.emplace<projv::Transform>(e, projv::Transform{at});
        projv::RigidBody r;
        r.shape = projv::RigidBody::Shape::Box;
        r.halfExtents = vec3(0.25f);
        r.mass = mass;
        app.world.emplace<projv::RigidBody>(e, r);
        return e;
    };
    Entity heavy = boxEntity(vec3(0, 3, 0), 80.0f);
    projv::Joint hang;
    hang.type = JointType::Point;
    hang.anchor = vec3(0, 3.25f, 0);
    hang.breakForce = 2000.0f;   // holds 80 kg (785 N), not much more
    Entity joint = connectBodies(app.world, heavy, projv::NullEntity, hang);
    frames(30);
    CHECK(jointState(app.world, joint).alive);
    CHECK(bodyPose(app.world, heavy).position.y == doctest::Approx(3.0f).epsilon(0.01));
    // A hard yank: more than it can hold.
    setVelocity(app.world, heavy, vec3(0, -30, 0));
    frames(2);
    REQUIRE(broke.size() == 1);
    CHECK(broke[0].entity == joint);
    CHECK_FALSE(broke[0].bodyGone);
    CHECK(broke[0].force > 2000.0f);
    CHECK_FALSE(jointState(app.world, joint).alive);
    // ...and the yank carries on: the joint kept only what it could hold (2000 N for a step: about
    // 0.4 m/s of 30 on 80 kg), not the whole of it.
    CHECK(bodyPose(app.world, heavy).linearVelocity.y < -25.0f);

    // A motor turned on and off by command.
    Entity wheel = boxEntity(vec3(5, 3, 0), 5.0f);
    projv::Joint axle;
    axle.type = JointType::Hinge;
    axle.anchor = vec3(5, 3, 0);
    axle.axis = vec3(0, 0, 1);
    Entity motor = connectBodies(app.world, wheel, projv::NullEntity, axle);
    frames(5);
    setJointMotor(app.world, motor, true, 4.0f);
    frames(60);
    CHECK(bodyPose(app.world, wheel).angularVelocity.z == doctest::Approx(4.0f).epsilon(0.02));
    // The wheel destroyed: its joint goes, reported as such.
    app.world.destroy(wheel);
    frames(2);
    REQUIRE(broke.size() == 2);
    CHECK(broke[1].entity == motor);
    CHECK(broke[1].bodyGone);

    using Traits = ComponentTraits<projv::Joint>;
    projv::Joint j;
    j.type = JointType::Hinge;
    j.a = projv::NetId{4};
    j.b = projv::NetId{9};
    j.anchor = vec3(1, 2, 3);
    j.axis = vec3(0, 0, 1);
    j.limitMin = -1.0f;
    j.limitMax = 0.5f;
    j.motor = true;
    j.motorVelocity = 2.0f;
    j.breakTorque = 300.0f;
    auto back = Traits::load(Traits::save(j), 1);
    REQUIRE(back);
    CHECK(back->type == JointType::Hinge);
    CHECK(back->a.value == 4);
    CHECK(back->b.value == 9);
    CHECK(back->anchor == vec3(1, 2, 3));
    CHECK(back->limitMin == doctest::Approx(-1.0f));
    CHECK(back->motor);
    CHECK(back->motorVelocity == doctest::Approx(2.0f));
    CHECK(back->breakTorque == doctest::Approx(300.0f));
}

TEST_CASE("a motor jammed for a while starts again when the jam clears") {
    // Jolt lets a body sleep once it stops moving; a motor that stalls would then sleep for good.
    PhysicsWorld physics;
    physics.setGravity(vec3(0));
    BodyId arm = box(physics, vec3(1, 0, 0), vec3(1, 0.1f, 0.1f));
    JointDesc m;
    m.type = JointType::Hinge;
    m.a = arm;
    m.anchor = vec3(0);
    m.axis = vec3(0, 0, 1);
    m.motor = true;
    m.motorVelocity = 2.0f;
    m.motorLimit = 50.0f;   // weak: a wall stops it
    physics.createJoint(m);
    BodyDesc wall;
    wall.shape = PhysicsShape::box(vec3(0.2f, 0.2f, 1));
    wall.motion = MotionType::Kinematic;      // so it can be moved away without waking anything
    wall.position = vec3(1.5f, 0.45f, 0);   // in the arm's path, just past where it starts
    BodyId stop = physics.createBody(wall);
    for (int i = 0; i < 4 * 60; i++) physics.step(DT);   // long enough to have slept
    CHECK(std::abs(physics.bodyState(arm).angularVelocity.z) < 0.1f);
    physics.setPose(stop, vec3(50, 0, 0), quat(1, 0, 0, 0));   // the jam clears; nothing else is touched
    for (int i = 0; i < 60; i++) physics.step(DT);
    CHECK(physics.bodyState(arm).angularVelocity.z == doctest::Approx(2.0f).epsilon(0.03));
}
