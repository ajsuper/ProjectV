// Characters (runtime::CharacterController, PhysicsWorld characters): standing, walking, stairs,
// walls, slopes, jumping, pushing, being landed on, moving ground -- and the same determinism,
// rewind and replay guarantees as bodies.

#include "doctest/doctest.h"

#include <cmath>
#include <vector>

#include "core/application.h"
#include "runtime/physics.h"
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
    constexpr double DT = 1.0 / 60.0;

    struct Fixture {
        projv::Application app;
        projv::Scene& scene;
        double now = 0.0;
        explicit Fixture(bool record = false) : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            installSceneBridge(app);
            PhysicsConfig config;
            config.settings.record = record;
            installPhysics(app, config);
            app.runStartup();
            app.runFrame();
        }
        void frames(int n) { for (int i = 0; i < n; i++) { now += DT; app.runFrame(); } }
        // Voxel ground of 0.25 m voxels: `height(x, z)` voxels high, over 80 x 80 (20 m), corner at
        // (-10, 0, -10).
        template <typename Height> ComponentHandle ground(Height height) {
            ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "ground", INVALID_COMPONENT_HANDLE, 16, 0.25f);
            utils::setComponentTransform(scene, h, vec3(-10, 0, -10), quat(1, 0, 0, 0), 1.0f);
            std::vector<projv::PendingVoxelOp> ops;
            for (int z = 0; z < 80; z++)
                for (int x = 0; x < 80; x++)
                    for (int y = 0; y < height(x, z); y++) ops.push_back({true, ivec3(x, y, z), 0x3FFFFFFFu});
            utils::queueVoxelAdd(scene, h, ops);
            utils::updateScene(scene);
            Entity e = spawnComponent(app.world, h);
            app.world.emplace<projv::StaticCollider>(e);
            return h;
        }
        Entity character(vec3 feet, uint32_t netId = 0) {
            Entity e = app.world.create();
            app.world.emplace<projv::Transform>(e, projv::Transform{feet});
            if (netId) app.world.emplace<projv::NetId>(e, projv::NetId{netId});
            app.world.emplace<projv::CharacterController>(e);
            return e;
        }
        CharacterState state(Entity e) { return characterState(app.world, e); }
    };
    // Flat ground one voxel (0.25 m) thick: its top at y = 0.25.
    int flat(int, int) { return 1; }
}

TEST_CASE("a character stands on voxel ground, and walks at the speed it is given") {
    Fixture f;
    f.ground(flat);
    Entity c = f.character(vec3(0, 1, 0));
    f.frames(60);
    CharacterState s = f.state(c);
    CHECK(s.onGround);
    CHECK(s.position.y == doctest::Approx(0.25f).epsilon(0.02));
    CHECK(s.groundNormal.y == doctest::Approx(1.0f).epsilon(0.01));

    setCharacterInput(f.app.world, c, vec3(3, 0, 0));
    f.frames(120);   // two seconds at 3 m/s
    s = f.state(c);
    CHECK(s.position.x == doctest::Approx(6.0f).epsilon(0.05));
    CHECK(s.position.y == doctest::Approx(0.25f).epsilon(0.02));
    CHECK(s.onGround);
    // Drawn there: physics writes the Transform's position.
    CHECK(f.app.world.get<projv::Transform>(c).position.x == doctest::Approx(s.position.x).epsilon(0.02));
}

TEST_CASE("a character walks up steps it can step, and stops at a wall it cannot") {
    Fixture f;
    // Steps of 0.25 m from x = 2 m, every half metre, up to 1.75 m; then a wall from x = 7 m standing
    // 2 m above the top step.
    f.ground([](int x, int) {
        if (x >= 68) return 15;                                 // the wall, 3.75 m
        if (x >= 48) return 1 + std::min((x - 48) / 2 + 1, 6);  // stairs up to 1.75 m
        return 1;
    });
    Entity c = f.character(vec3(-5, 0.5f, 0));
    f.frames(30);
    setCharacterInput(f.app.world, c, vec3(2.5f, 0, 0));
    f.frames(8 * 60);
    CharacterState s = f.state(c);
    CHECK(s.position.y == doctest::Approx(1.75f).epsilon(0.03));   // up all six steps
    CHECK(s.position.x < 7.0f);                                    // and not through the wall
    CHECK(s.position.x > 6.0f);                                    // which it walked right up to
}

TEST_CASE("a slope steeper than the character's limit cannot be walked up") {
    Fixture f;
    // One voxel up per voxel along (45 degrees, in quarter-metre steps it can step), from x = 0; then
    // from x = 3 m four up per voxel (76 degrees, in 1 m steps it cannot). A voxel slope is a
    // staircase, so what limits a character on one is the step height as much as the angle.
    f.ground([](int x, int) {
        if (x < 40) return 1;
        if (x < 52) return 1 + (x - 40);
        return std::min(13 + (x - 52) * 4, 60);
    });
    Entity c = f.character(vec3(-2, 0.5f, 0));
    f.frames(30);
    setCharacterInput(f.app.world, c, vec3(2, 0, 0));
    f.frames(10 * 60);
    CharacterState s = f.state(c);
    CHECK(s.position.y > 2.5f);                  // climbed the 45 degree part
    CHECK(s.position.y < 3.5f);                  // but not the steep part: the 45 degree top is 3.25 m
    CHECK(s.position.x < 3.0f + 0.1f);
}

TEST_CASE("a character jumps once, comes back down, and lands") {
    Fixture f;
    f.ground(flat);
    Entity c = f.character(vec3(0, 0.5f, 0));
    f.frames(60);
    setCharacterInput(f.app.world, c, vec3(0), true, 5.0f);
    float highest = 0.0f;
    for (int i = 0; i < 90; i++) {
        f.frames(1);
        highest = std::max(highest, f.state(c).position.y);
    }
    // v^2 / 2g at 5 m/s: 1.27 m above the ground.
    CHECK(highest - 0.25f == doctest::Approx(25.0f / (2.0f * 9.81f)).epsilon(0.05));
    CHECK(f.state(c).onGround);
    CHECK(f.state(c).position.y == doctest::Approx(0.25f).epsilon(0.02));
    f.frames(60);   // the request was used up: no second jump
    CHECK(f.state(c).position.y == doctest::Approx(0.25f).epsilon(0.02));
}

TEST_CASE("a character pushes a crate it walks into, and a dropped ball does not pass through it") {
    Fixture f;
    f.ground(flat);
    Entity c = f.character(vec3(-3, 0.5f, 0));
    // A light crate in its way.
    Entity crate = f.app.world.create();
    f.app.world.emplace<projv::Transform>(crate, projv::Transform{vec3(0, 0.75f, 0)});
    projv::RigidBody box;
    box.shape = projv::RigidBody::Shape::Box;
    box.halfExtents = vec3(0.5f);
    box.mass = 20.0f;
    f.app.world.emplace<projv::RigidBody>(crate, box);
    f.frames(30);
    setCharacterInput(f.app.world, c, vec3(2, 0, 0));
    f.frames(3 * 60);
    CHECK(bodyPose(f.app.world, crate).position.x > 1.5f);   // shoved along ahead of it

    // A ball dropped on its head rests on it or rolls off; it does not fall through to the floor.
    setCharacterInput(f.app.world, c, vec3(0));
    f.frames(30);
    vec3 head = f.state(c).position + vec3(0, 1.8f, 0);
    Entity ball = f.app.world.create();
    f.app.world.emplace<projv::Transform>(ball, projv::Transform{head + vec3(0, 2, 0)});
    projv::RigidBody sphere;
    sphere.shape = projv::RigidBody::Shape::Sphere;
    sphere.radius = 0.2f;
    f.app.world.emplace<projv::RigidBody>(ball, sphere);
    float lowestWhileAbove = 100.0f;
    for (int i = 0; i < 60; i++) {
        f.frames(1);
        vec3 p = bodyPose(f.app.world, ball).position;
        vec3 feet = f.state(c).position;
        if (std::abs(p.x - feet.x) < 0.2f && std::abs(p.z - feet.z) < 0.2f) lowestWhileAbove = std::min(lowestWhileAbove, p.y);
    }
    CHECK(lowestWhileAbove > head.y - 0.3f);
}

TEST_CASE("a character standing on a moving platform is carried with it") {
    Fixture f;
    f.ground(flat);
    Entity platform = f.app.world.create();
    f.app.world.emplace<projv::Transform>(platform, projv::Transform{vec3(0, 1, 0)});
    projv::RigidBody rigid;
    rigid.motion = MotionType::Kinematic;
    rigid.shape = projv::RigidBody::Shape::Box;
    rigid.halfExtents = vec3(2, 0.25f, 2);
    f.app.world.emplace<projv::RigidBody>(platform, rigid);
    Entity c = f.character(vec3(0, 2, 0));
    f.frames(60);
    REQUIRE(f.state(c).onGround);
    CHECK(f.state(c).position.y == doctest::Approx(1.25f).epsilon(0.02));
    for (int i = 0; i < 120; i++) {
        f.app.world.patch<projv::Transform>(platform, [](projv::Transform& t) { t.position.x += 0.02f; });
        f.frames(1);
    }
    CHECK(f.state(c).position.x == doctest::Approx(2.4f).epsilon(0.05));   // 1.2 m/s for 2 s
    CHECK(f.state(c).onGround);
}

TEST_CASE("characters are deterministic: input by NetId, rewind, and replay") {
    Fixture f(true);
    f.ground([](int x, int z) { return 1 + ((x / 8 + z / 8) % 3 == 0 ? 1 : 0); });
    Entity a = f.character(vec3(-3, 1, -3), 10);
    Entity b = f.character(vec3(3, 1, 3), 11);
    f.frames(20);
    std::vector<uint8_t> snapshot;
    std::vector<uint64_t> hashes;
    for (int frame = 0; frame < 300; frame++) {
        if (frame % 40 == 0) {
            PhysicsCommand walk;
            walk.kind = PhysicsCommand::Kind::CharacterInput;
            walk.target = projv::NetId{10};
            walk.a = vec3(std::sin(float(frame)) * 3, 0, std::cos(float(frame)) * 3);
            walk.jump = frame % 80 == 0;
            walk.b = vec3(4, 0, 0);
            walk.tick = nextPhysicsTick(f.app.world);
            walk.issuer = 3;
            submitCommand(f.app.world, walk);
            setCharacterInput(f.app.world, b, vec3(-2, 0, -1), frame % 120 == 0);
        }
        if (frame == 100) snapshot = savePhysics(f.app.world);
        f.frames(1);
        if (frame >= 100) hashes.push_back(physicsStateHash(f.app.world));
    }
    CHECK(glm::length(f.state(a).position - vec3(-3, 0.25f, -3)) > 1.0f);   // it went somewhere

    ReplayResult replay = PhysicsWorld::replay(physicsWorld(f.app.world).recording());
    CAPTURE(replay.problem);
    CHECK(replay.matched);

    // Rewound to frame 100, the same inputs again give the same states. Inputs were applied by then
    // only up to frame 80; the later ones are re-sent, as a networked client would after a rollback.
    REQUIRE(restorePhysics(f.app.world, snapshot));
    for (int frame = 100; frame < 300; frame++) {
        if (frame % 40 == 0) {
            PhysicsCommand walk;
            walk.kind = PhysicsCommand::Kind::CharacterInput;
            walk.target = projv::NetId{10};
            walk.a = vec3(std::sin(float(frame)) * 3, 0, std::cos(float(frame)) * 3);
            walk.jump = frame % 80 == 0;
            walk.b = vec3(4, 0, 0);
            walk.tick = nextPhysicsTick(f.app.world);
            walk.issuer = 3;
            submitCommand(f.app.world, walk);
            setCharacterInput(f.app.world, b, vec3(-2, 0, -1), frame % 120 == 0);
        }
        f.frames(1);
        REQUIRE(physicsStateHash(f.app.world) == hashes[size_t(frame - 100)]);
    }
}

TEST_CASE("CharacterController round-trips through entities.json") {
    using Traits = ComponentTraits<projv::CharacterController>;
    projv::CharacterController c;
    c.radius = 0.4f;
    c.height = 2.2f;
    c.strength = 900.0f;
    auto back = Traits::load(Traits::save(c), 1);
    REQUIRE(back);
    CHECK(back->radius == doctest::Approx(0.4f));
    CHECK(back->height == doctest::Approx(2.2f));
    CHECK(back->strength == doctest::Approx(900.0f));
    CHECK(Traits::save(projv::CharacterController{}).empty());
}



TEST_CASE("a query from a character's eyes skips the character when told to ignore it") {
    // A player's crosshair: the ray starts inside the character's own capsule. Named as `ignore`, the
    // character is skipped, and the ray goes on to what it is aimed at.
    Fixture f;
    f.ground(flat);
    Entity c = f.character(vec3(0, 1, 0));
    f.frames(30);
    const vec3 eye = f.state(c).position + vec3(0, 1.6f, 0);
    auto down = castRay(f.app.world, eye, vec3(0, -1, 0), 10.0f, 0xffffffffu, c);
    REQUIRE(down);
    CHECK(down->entity != c);
    CHECK(down->distance == doctest::Approx(1.6f).epsilon(0.02));   // the ground under its feet
    CHECK(castSphere(f.app.world, eye, 0.1f, vec3(0, -1, 0), 10.0f, 0xffffffffu, c)->entity != c);
    for (Entity e : overlapSphere(f.app.world, eye, 0.2f, 0xffffffffu, c)) CHECK(e != c);
    // Not told to: the ray starts inside it, and stops on it at once.
    auto plain = castRay(f.app.world, eye, vec3(0, -1, 0), 10.0f);
    REQUIRE(plain);
    CHECK(plain->entity == c);
}
