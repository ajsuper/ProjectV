// Physics for entities (runtime/physics.h): RigidBody and StaticCollider on linked components, run
// through real Application frames with a clock the test owns.

#include "doctest/doctest.h"

#include <cmath>
#include <string>
#include <vector>

#include "core/application.h"
#include "runtime/physics.h"
#include "runtime/scene_bridge.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

namespace {
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;
    namespace utils = projv::utils;
    namespace runtime = projv::runtime;

    constexpr double DT = 1.0 / 60.0;

    struct Fixture {
        projv::Application app;
        projv::Scene& scene;
        double now = 0.0;
        std::vector<projv::PhysicsBodyRefused> refused;
        explicit Fixture(bool physicsFirst = false) : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            if (physicsFirst) runtime::installPhysics(app);
            runtime::installSceneBridge(app);
            if (!physicsFirst) runtime::installPhysics(app);
            app.events().on<projv::PhysicsBodyRefused>([this](const projv::PhysicsBodyRefused& e) { refused.push_back(e); });
            app.runStartup();
            app.runFrame();   // the first frame only starts the clock
        }
        void frames(int n, double each = DT) {
            for (int i = 0; i < n; i++) {
                now += each;
                app.runFrame();
            }
        }
        // A component of voxels filling [low, high], at `position`, `voxel` metres each. Started as a
        // 16-voxel chunk: anything larger becomes a grid, as continuous geometry should.
        ComponentHandle voxels(const char* name, ivec3 low, ivec3 high, vec3 position, float voxel = 0.5f,
                               ComponentHandle parent = INVALID_COMPONENT_HANDLE) {
            ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, name, parent, 16, voxel);
            utils::setComponentTransform(scene, h, position, quat(1, 0, 0, 0), 1.0f);
            std::vector<projv::PendingVoxelOp> ops;
            for (int z = low.z; z <= high.z; z++)
                for (int y = low.y; y <= high.y; y++)
                    for (int x = low.x; x <= high.x; x++) ops.push_back({true, ivec3(x, y, z), 0x3FFFFFFFu});
            utils::queueVoxelAdd(scene, h, ops);
            utils::updateScene(scene);
            return h;
        }
        // A 40 m floor whose top is at y = 0: 80 x 2 x 80 half-metre voxels, so a grid of cells.
        ComponentHandle floor() { return voxels("floor", {0, 0, 0}, {79, 1, 79}, vec3(-20, -1, -20)); }

        Entity linked(ComponentHandle h) { return runtime::spawnComponent(app.world, h); }
        Entity crate(vec3 at) {
            // 2 m: 4 voxels a side, its corner at the component's origin.
            Entity e = linked(voxels("crate", {0, 0, 0}, {3, 3, 3}, at));
            app.world.emplace<projv::RigidBody>(e);
            return e;
        }
        Entity solidFloor() {
            Entity e = linked(floor());
            app.world.emplace<projv::StaticCollider>(e);
            return e;
        }
        const projv::Transform& transform(Entity e) { return app.world.get<projv::Transform>(e); }
    };
}

TEST_CASE("a voxel crate falls onto a voxel floor and rests on it, drawn where it is") {
    Fixture f;
    Entity floor = f.solidFloor();
    ComponentHandle floorComponent = f.app.world.get<projv::VoxelComponent>(floor).handle;
    CHECK(f.scene.components[floorComponent].kind == projv::ComponentKind::Grid);   // really a grid
    Entity crate = f.crate({-1, 6, -1});
    f.frames(4 * 60);

    REQUIRE(runtime::hasBody(f.app.world, crate));
    runtime::BodyState pose = runtime::bodyPose(f.app.world, crate);
    CHECK(pose.position.y == doctest::Approx(0.0f).epsilon(0.02));   // its corner on the floor's top
    CHECK_FALSE(pose.awake);
    // Drawn there too: the Transform, and the component the bridge wrote it into.
    CHECK(f.transform(crate).position.y == doctest::Approx(pose.position.y).epsilon(1e-4));
    ComponentHandle h = f.app.world.get<projv::VoxelComponent>(crate).handle;
    CHECK(utils::getComponentWorldPosition(f.scene, h).y == doctest::Approx(pose.position.y).epsilon(1e-4));
    CHECK(f.refused.empty());
    CHECK(runtime::physicsWorld(f.app.world).stats().stepsWithDroppedContacts == 0);
}

TEST_CASE("nothing collides that was not asked to") {
    Fixture f;
    f.linked(f.floor());   // linked, but no StaticCollider
    Entity crate = f.crate({-1, 6, -1});
    f.frames(2 * 60);
    CHECK(runtime::bodyPose(f.app.world, crate).position.y < -5.0f);

    // And a voxel component with neither component is left alone entirely.
    Fixture g;
    ComponentHandle h = g.voxels("prop", {0, 0, 0}, {1, 1, 1}, vec3(0, 3, 0));
    Entity prop = g.linked(h);
    g.frames(60);
    CHECK_FALSE(runtime::hasBody(g.app.world, prop));
    CHECK(g.transform(prop).position == vec3(0, 3, 0));
}

TEST_CASE("presentation interpolates between the last two fixed poses") {
    Fixture f;
    Entity crate = f.crate({0, 50, 0});
    f.frames(30);
    // Half a fixed step into the next one: the Transform is halfway between the two poses.
    f.frames(1, DT * 0.5);
    const projv::PhysicsBody& body = f.app.world.get<projv::PhysicsBody>(crate);
    float alpha = f.app.time().fixedAlpha;
    CHECK(alpha == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(f.transform(crate).position.y ==
          doctest::Approx(body.previousPosition.y + (body.position.y - body.previousPosition.y) * alpha).epsilon(1e-4));
    CHECK(body.position.y < body.previousPosition.y);   // falling
}

TEST_CASE("commands apply at the next fixed step, in the order issued") {
    Fixture f;
    Entity crate = f.crate({0, 50, 0});
    f.frames(1);
    REQUIRE(runtime::hasBody(f.app.world, crate));
    runtime::setGravity(f.app.world, vec3(0));
    runtime::setVelocity(f.app.world, crate, vec3(0));
    runtime::addVelocity(f.app.world, crate, vec3(3, 0, 0));
    runtime::addVelocity(f.app.world, crate, vec3(0, 0, 4));
    CHECK(runtime::bodyPose(f.app.world, crate).linearVelocity.x == doctest::Approx(0.0f));   // not yet
    f.frames(1);
    vec3 v = runtime::bodyPose(f.app.world, crate).linearVelocity;
    CHECK(v.x == doctest::Approx(3.0f).epsilon(0.02));
    CHECK(v.z == doctest::Approx(4.0f).epsilon(0.02));
    CHECK(std::abs(v.y) < 0.01f);

    // An impulse is a velocity change over the mass: 2 m of crate at 1000 kg/m^3 is 8000 kg.
    runtime::setVelocity(f.app.world, crate, vec3(0));
    runtime::addImpulse(f.app.world, crate, vec3(0, 8000, 0));
    f.frames(1);
    CHECK(runtime::bodyPose(f.app.world, crate).linearVelocity.y == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("a teleport places the body, and is drawn as a jump") {
    Fixture f;
    Entity crate = f.crate({0, 50, 0});
    f.frames(10);
    runtime::teleport(f.app.world, crate, vec3(7, 20, 7), quat(1, 0, 0, 0));
    f.frames(1, DT * 0.5);   // no whole step yet: nothing has happened
    CHECK(f.transform(crate).position.x == doctest::Approx(0.0f));
    f.frames(1, DT * 0.5);
    runtime::BodyState pose = runtime::bodyPose(f.app.world, crate);
    CHECK(pose.position.x == doctest::Approx(7.0f));
    // Not interpolated from where it was: the drawn pose is near the new place, not halfway.
    CHECK(f.transform(crate).position.x == doctest::Approx(7.0f).epsilon(0.01));
}

TEST_CASE("patching a dynamic body's Transform does not move it") {
    Fixture f;
    f.solidFloor();
    Entity crate = f.crate({-1, 0.5f, -1});
    f.frames(90);
    vec3 resting = runtime::bodyPose(f.app.world, crate).position;
    f.app.world.patch<projv::Transform>(crate, [](projv::Transform& t) { t.position.y = 30.0f; });
    f.frames(30);
    CHECK(runtime::bodyPose(f.app.world, crate).position.y == doctest::Approx(resting.y).epsilon(0.01));
}

TEST_CASE("destroying the entity destroys its body; a deleted component takes the body with it") {
    Fixture f;
    f.solidFloor();
    Entity a = f.crate({-5, 4, -5});
    Entity b = f.crate({5, 4, 5});
    f.frames(2);
    const runtime::PhysicsWorld& sim = runtime::physicsWorld(f.app.world);
    CHECK(sim.stats().bodies == 3);
    f.app.world.destroy(a);
    f.frames(1);
    CHECK(sim.stats().bodies == 2);

    utils::deleteComponent(f.scene, f.app.world.get<projv::VoxelComponent>(b).handle);
    f.frames(2);   // the bridge notices the deletion, then physics drops the body
    CHECK(sim.stats().bodies == 1);
    CHECK_FALSE(runtime::hasBody(f.app.world, b));
    CHECK(f.app.world.valid(b));   // the entity is the game's to keep or not
}

TEST_CASE("a sphere fitted to a prefab-shaped asset sits on its voxels' centre") {
    // A prefab: an asset node at the ball's centre, its chunk offset by minus half its size -- how
    // examples/16-sandbox writes them.
    Fixture f;
    f.solidFloor();
    ComponentHandle node = utils::addComponent(f.scene, projv::ComponentKind::Asset, "ball", INVALID_COMPONENT_HANDLE, 4, 1.0f);
    f.voxels("shape", {0, 0, 0}, {5, 5, 5}, vec3(-1.5f), 0.5f, node);
    utils::setComponentTransform(f.scene, node, vec3(3, 10, 3), quat(1, 0, 0, 0), 1.0f);
    Entity ball = f.linked(node);
    projv::RigidBody rigid;
    rigid.shape = projv::RigidBody::Shape::Sphere;
    f.app.world.emplace<projv::RigidBody>(ball, rigid);
    f.frames(4 * 60);
    runtime::BodyState pose = runtime::bodyPose(f.app.world, ball);
    CHECK(pose.position.y == doctest::Approx(1.5f).epsilon(0.02));   // radius 1.5, fitted
    // A sphere rolls; nothing about a fitted sphere should leave it hovering or sunk.
    CHECK(f.refused.empty());
}

TEST_CASE("a kinematic body follows its Transform and pushes what it meets") {
    Fixture f;
    f.solidFloor();
    Entity pusher = f.linked(f.voxels("pusher", {0, 0, 0}, {1, 3, 7}, vec3(-6, 0.05f, -2)));
    projv::RigidBody rigid;
    rigid.motion = projv::runtime::MotionType::Kinematic;
    f.app.world.emplace<projv::RigidBody>(pusher, rigid);
    Entity crate = f.crate({0, 0.05f, -1});
    f.frames(30);
    float before = runtime::bodyPose(f.app.world, crate).position.x;
    for (int i = 0; i < 120; i++) {
        f.app.world.patch<projv::Transform>(pusher, [](projv::Transform& t) { t.position.x += 0.05f; });
        f.frames(1);
    }
    CHECK(f.transform(pusher).position.x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(runtime::bodyPose(f.app.world, pusher).position.x == doctest::Approx(0.0f).epsilon(0.05));
    // The pusher's front face (1 m thick, from x = -5) ends at x = 1: the crate, which started at 0,
    // is pushed along to it -- not through it, not left behind.
    CHECK(before == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(runtime::bodyPose(f.app.world, crate).position.x == doctest::Approx(1.0f).epsilon(0.03));
}

TEST_CASE("bodies that cannot be made are refused, with a reason, and nothing else breaks") {
    Fixture f;
    // A dynamic part inside an asset.
    ComponentHandle house = utils::addComponent(f.scene, projv::ComponentKind::Asset, "house", INVALID_COMPONENT_HANDLE, 4, 1.0f);
    ComponentHandle door = f.voxels("door", {0, 0, 0}, {1, 3, 0}, vec3(0), 0.5f, house);
    Entity e = f.app.world.create();
    REQUIRE(runtime::linkComponent(f.app.world, e, door, projv::LinkMode::Part));
    f.app.world.emplace<projv::RigidBody>(e);
    // A voxel shape with no link, and an empty component.
    Entity loose = f.app.world.create();
    f.app.world.emplace<projv::RigidBody>(loose);
    Entity empty = f.linked(utils::addComponent(f.scene, projv::ComponentKind::Chunk, "empty", INVALID_COMPONENT_HANDLE, 16, 0.5f));
    f.app.world.emplace<projv::StaticCollider>(empty);
    Entity fine = f.crate({0, 5, 0});
    f.frames(2);
    CHECK(f.refused.size() == 3);
    CHECK_FALSE(runtime::hasBody(f.app.world, e));
    CHECK_FALSE(runtime::hasBody(f.app.world, loose));
    CHECK_FALSE(runtime::hasBody(f.app.world, empty));
    CHECK(runtime::hasBody(f.app.world, fine));
}

TEST_CASE("an unlinked entity with a sized primitive is a body on its own") {
    Fixture f;
    f.solidFloor();
    Entity e = f.app.world.create();
    f.app.world.emplace<projv::Transform>(e, projv::Transform{vec3(0, 5, 0)});
    projv::RigidBody rigid;
    rigid.shape = projv::RigidBody::Shape::Box;
    rigid.halfExtents = vec3(0.5f);
    f.app.world.emplace<projv::RigidBody>(e, rigid);
    f.frames(3 * 60);
    CHECK(f.transform(e).position.y == doctest::Approx(0.5f).epsilon(0.02));
}

TEST_CASE("physics presents before the bridge syncs, whichever was installed first") {
    for (bool physicsFirst : {false, true}) {
        Fixture f(physicsFirst);
        const auto& names = f.app.systemNames(projv::Stage::PostUpdate);
        auto at = [&](const char* name) { return std::find(names.begin(), names.end(), name) - names.begin(); };
        CHECK(at("physics: present") < at("scene bridge"));
        // And the effect: what the Scene holds is this frame's pose, not last frame's.
        Entity crate = f.crate({0, 50, 0});
        f.frames(20);
        ComponentHandle h = f.app.world.get<projv::VoxelComponent>(crate).handle;
        CHECK(utils::getComponentWorldPosition(f.scene, h).y == doctest::Approx(f.transform(crate).position.y));
    }
}

TEST_CASE("RigidBody and StaticCollider round-trip through entities.json") {
    using Traits = runtime::ComponentTraits<projv::RigidBody>;
    projv::RigidBody b;
    b.motion = projv::runtime::MotionType::Kinematic;
    b.shape = projv::RigidBody::Shape::Capsule;
    b.radius = 0.4f;
    b.halfHeight = 0.9f;
    b.mass = 80.0f;
    b.friction = 0.9f;
    b.layer = projv::runtime::PhysicsLayer::Character;
    b.quality = projv::RigidBody::Quality::Continuous;
    nlohmann::json j = Traits::save(b);
    auto back = Traits::load(j, 1);
    REQUIRE(back);
    CHECK(back->motion == b.motion);
    CHECK(back->shape == b.shape);
    CHECK(back->radius == doctest::Approx(0.4f));
    CHECK(back->halfHeight == doctest::Approx(0.9f));
    CHECK(back->mass == doctest::Approx(80.0f));
    CHECK(back->friction == doctest::Approx(0.9f));
    CHECK(back->layer == b.layer);
    CHECK(back->quality == b.quality);
    // Defaults are not written, and an unknown word is refused rather than guessed at.
    CHECK(Traits::save(projv::RigidBody{}) == nlohmann::json{{"motion", "dynamic"}});
    CHECK_FALSE(Traits::load(nlohmann::json{{"motion", "floaty"}}, 1));
    CHECK_FALSE(Traits::load(nlohmann::json{{"layer", "nope"}}, 1));

    using StaticTraits = runtime::ComponentTraits<projv::StaticCollider>;
    projv::StaticCollider c;
    c.splits = true;
    c.restitution = 0.7f;
    auto cb = StaticTraits::load(StaticTraits::save(c), 1);
    REQUIRE(cb);
    CHECK(cb->splits);
    CHECK(cb->restitution == doctest::Approx(0.7f));
}
