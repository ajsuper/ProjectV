// Cheap debris (PhysicsConfig::debris; docs/plans/cheap-debris.md): a break's small pieces are not
// bodies, but fall, land and rest on the static world, go after their seconds or past the count, and
// fall again when the ground under them is cut away. A networked body's are bodies still.

#include "doctest/doctest.h"

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
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;
    namespace utils = projv::utils;
    constexpr double DT = 1.0 / 60.0;

    std::vector<projv::PendingVoxelOp> fill(ivec3 low, ivec3 high, uint32_t colour = 0x3FFFFFFFu) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) ops.push_back({true, ivec3(x, y, z), colour});
        return ops;
    }

    struct Fixture {
        projv::Application app;
        projv::Scene& scene;
        double now = 0.0;
        std::vector<projv::BodyBroken> breaks;
        ComponentHandle floorComponent = INVALID_COMPONENT_HANDLE;
        explicit Fixture(PhysicsConfig::DebrisConfig debris) : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            installSceneBridge(app);
            PhysicsConfig config;
            config.debris = debris;
            installPhysics(app, config);
            app.events().on<projv::BodyBroken>([this](const projv::BodyBroken& e) { breaks.push_back(e); });
            app.runStartup();
            app.runFrame();
            // A 20 m floor of half-metre voxels, top at y = 0.
            floorComponent = utils::addComponent(scene, projv::ComponentKind::Chunk, "floor", INVALID_COMPONENT_HANDLE, 16, 0.5f);
            utils::setComponentTransform(scene, floorComponent, vec3(-10, -1, -10), quat(1, 0, 0, 0), 1.0f);
            utils::queueVoxelAdd(scene, floorComponent, fill({0, 0, 0}, {39, 1, 39}));
            utils::updateScene(scene);
            app.world.emplace<projv::StaticCollider>(spawnComponent(app.world, floorComponent));
        }
        void frames(int n) { for (int i = 0; i < n; i++) { now += DT; app.runFrame(); } }
        // A 1 m crate of quarter-metre voxels with a nub (`nub`^3 voxels) on a one-voxel bridge, up at `at`.
        struct Crate { Entity entity; ComponentHandle component; };
        Crate crate(vec3 at, int nub = 2) {
            std::vector<projv::PendingVoxelOp> ops = fill({0, 0, 0}, {3, 3, 3});
            for (auto op : fill({4, 1, 1}, {4, 1, 1})) ops.push_back(op);   // the bridge
            for (auto op : fill({5, 0, 0}, {4 + nub, nub - 1, nub - 1})) ops.push_back(op);   // the nub
            ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "crate", INVALID_COMPONENT_HANDLE, 16, 0.25f);
            utils::setComponentTransform(scene, h, at, quat(1, 0, 0, 0), 1.0f);
            utils::queueVoxelAdd(scene, h, ops);
            utils::updateScene(scene);
            Entity e = spawnComponent(app.world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
            app.world.emplace<projv::RigidBody>(e);
            return {e, h};
        }
        // Cuts the bridge: the nub comes away.
        void cut(const Crate& c) {
            utils::queueVoxelRemove(scene, c.component, {{false, ivec3(4, 1, 1), 0}});
            utils::updateScene(scene);
        }
        std::vector<Entity> debris() const {
            std::vector<Entity> out;
            for (Entity e : app.world.view<projv::Debris>()) out.push_back(e);
            return out;
        }
    };

    PhysicsConfig::DebrisConfig on(uint32_t maxVoxels = 8) {
        PhysicsConfig::DebrisConfig d;
        d.maxVoxels = maxVoxels;
        return d;
    }
}

TEST_CASE("debris: a break's small piece is no body; it falls, lands on the voxel floor and rests there") {
    Fixture f(on());
    Fixture::Crate c = f.crate(vec3(-0.5f, 2.0f, -0.5f));
    f.frames(2);
    f.cut(c);
    f.frames(1);
    REQUIRE(f.breaks.size() == 1);
    CHECK(f.breaks[0].pieces.empty());
    REQUIRE(f.breaks[0].debris.size() == 1);
    const Entity d = f.breaks[0].debris[0];
    CHECK(f.app.world.all_of<projv::Debris>(d));
    CHECK_FALSE(f.app.world.any_of<projv::RigidBody, projv::PhysicsBody, projv::Piece>(d));
    f.frames(4 * 60);
    REQUIRE(f.app.world.valid(d));
    const projv::Debris& debris = f.app.world.get<projv::Debris>(d);
    CHECK(debris.resting);
    CHECK(debris.radius == doctest::Approx(0.25f));
    const vec3 centre = debris.position + debris.rotation * debris.centre;
    CHECK(centre.y == doctest::Approx(0.25f).epsilon(0.1));   // a sphere of 0.25 m on the floor's top
    // Drawn there: its Transform, and so its component.
    CHECK(glm::distance(f.app.world.get<projv::Transform>(d).position, debris.position) < 1e-3f);
    // The crate is still a body, without its nub.
    CHECK(f.app.world.all_of<projv::PhysicsBody>(c.entity));
    CHECK(utils::getComponentVoxelCount(f.scene, c.component) == 64);
}

TEST_CASE("debris: it lasts its seconds, and past the count the oldest goes, its component with it") {
    PhysicsConfig::DebrisConfig config = on();
    config.seconds = 1.0f;
    Fixture f(config);
    Fixture::Crate c = f.crate(vec3(-0.5f, 2.0f, -0.5f));
    f.frames(2);
    f.cut(c);
    f.frames(1);
    REQUIRE(f.debris().size() == 1);
    const ComponentHandle piece = f.app.world.get<projv::VoxelComponent>(f.debris()[0]).handle;
    f.frames(90);
    CHECK(f.debris().empty());
    CHECK_FALSE(utils::isComponentAlive(f.scene, piece));

    PhysicsConfig::DebrisConfig one = on();
    one.maxCount = 1;
    Fixture g(one);
    Fixture::Crate a = g.crate(vec3(-4, 2, 0)), b = g.crate(vec3(4, 2, 0));
    g.frames(2);
    g.cut(a);
    g.frames(5);
    g.cut(b);
    g.frames(1);
    REQUIRE(g.breaks.size() == 2);
    REQUIRE(g.debris().size() == 1);
    CHECK(g.debris()[0] == g.breaks[1].debris[0]);   // the newer
}

TEST_CASE("debris: resting debris falls again when the ground under it is cut away") {
    Fixture f(on());
    Fixture::Crate c = f.crate(vec3(-0.5f, 2.0f, -0.5f));
    f.frames(2);
    f.cut(c);
    f.frames(3 * 60);
    REQUIRE(f.debris().size() == 1);
    const Entity d = f.debris()[0];
    REQUIRE(f.app.world.get<projv::Debris>(d).resting);
    const projv::Debris& debris = f.app.world.get<projv::Debris>(d);
    const vec3 at = debris.position + debris.rotation * debris.centre;
    // A 3 m hole through the floor under it.
    const ivec3 under = ivec3(glm::floor((at - vec3(-10, -1, -10)) / 0.5f));
    std::vector<projv::PendingVoxelOp> hole;
    for (auto op : fill({under.x - 3, 0, under.z - 3}, {under.x + 3, 1, under.z + 3})) hole.push_back({false, op.position, 0});
    utils::queueVoxelRemove(f.scene, f.floorComponent, hole);
    utils::updateScene(f.scene);
    f.frames(60);
    REQUIRE(f.app.world.valid(d));
    CHECK_FALSE(f.app.world.get<projv::Debris>(d).resting);
    CHECK(f.app.world.get<projv::Debris>(d).position.y < -1.5f);
}

TEST_CASE("debris: debris pressed into a corner it overlaps comes to rest, and gathers no speed") {
    // A shard's sphere sits at its voxels' centre, so one cut from a wall can start inside what is left
    // of it: it comes out, and rests in the corner. Debris met by two surfaces in one step slides along
    // the crease between them or stops (stepDebris). What made that necessary -- debris in a big level's
    // grooves and corners gathering speed while it stood still, 60 m/s, never resting -- this does not
    // reproduce (the code before passes it too): a barrage on that level left 205 debris of 1,300 still
    // moving 10 s on, then none (docs/plans/cheap-debris.md).
    Fixture f(on());
    // A wall on the floor, its face at x = 2.
    utils::queueVoxelAdd(f.scene, f.floorComponent, fill({24, 2, 0}, {25, 5, 39}));
    utils::updateScene(f.scene);
    f.frames(2);
    for (vec3 at : {vec3(1.8f, 0.2f, 0.0f), vec3(1.9f, 0.15f, 1.0f), vec3(1.82f, 0.4f, 2.0f)}) {   // into the wall and the floor
        Entity e = f.app.world.create();
        f.app.world.emplace<projv::Transform>(e, projv::Transform{at});
        projv::Debris d;
        d.position = d.previousPosition = at;
        d.radius = 0.25f;
        d.velocity = vec3(1.0f, -1.0f, 0.0f);
        f.app.world.emplace<projv::Debris>(e, d);
    }
    float fastest = 0.0f;
    for (int i = 0; i < 3 * 60; i++) {
        f.frames(1);
        for (auto [e, d] : f.app.world.view<projv::Debris>().each()) fastest = std::max(fastest, glm::length(d.velocity));
    }
    int resting = 0;
    for (auto [e, d] : f.app.world.view<projv::Debris>().each()) resting += d.resting;
    CHECK(resting == 3);
    CHECK(fastest < 3.0f);
}

TEST_CASE("debris: debris that lands sliding starts to roll, then comes to rest") {
    // Friction acts on the slip where the sphere meets the ground, its spin counted, so what it takes from
    // the sliding it gives to the turning: debris rolls and tumbles rather than skating along unturned.
    // (Rolling at 4 m/s it turns at 16 rad/s: the limit is raised past that.)
    PhysicsConfig::DebrisConfig config = on();
    config.maxAngularSpeed = 100.0f;
    Fixture f(config);
    f.frames(2);
    Entity e = f.app.world.create();
    const vec3 at(-5.0f, 0.3f, 0.0f);
    f.app.world.emplace<projv::Transform>(e, projv::Transform{at});
    projv::Debris d;
    d.position = d.previousPosition = at;
    d.radius = 0.25f;
    d.velocity = vec3(4.0f, 0.0f, 0.0f);   // sliding, not turning
    f.app.world.emplace<projv::Debris>(e, d);
    float rolling = 0.0f;   // the most |omega| r / |v| seen while it moved: 1 is rolling without slipping
    for (int i = 0; i < 60; i++) {
        f.frames(1);
        const projv::Debris& now = f.app.world.get<projv::Debris>(e);
        const float speed = glm::length(now.velocity);
        if (speed > 0.5f) rolling = std::max(rolling, glm::length(now.angularVelocity) * now.radius / speed);
    }
    CHECK(rolling > 0.9f);
    CHECK(rolling < 1.1f);
    const projv::Debris& turned = f.app.world.get<projv::Debris>(e);
    CHECK(glm::angle(turned.rotation) > 1.0f);   // it turned as it went
    f.frames(8 * 60);
    CHECK(f.app.world.get<projv::Debris>(e).resting);
}

TEST_CASE("debris: it turns no faster than maxAngularSpeed -- small debris rolling fast slides instead") {
    for (float limit : {10.0f, 0.0f}) {
        CAPTURE(limit);
        PhysicsConfig::DebrisConfig config = on();
        config.maxAngularSpeed = limit;
        Fixture f(config);
        f.frames(2);
        Entity e = f.app.world.create();
        const vec3 at(-5.0f, 0.06f, 0.0f);
        f.app.world.emplace<projv::Transform>(e, projv::Transform{at});
        projv::Debris d;
        d.position = d.previousPosition = at;
        d.radius = 0.05f;
        d.velocity = vec3(6.0f, 0.0f, 0.0f);         // rolling, it would turn at 120 rad/s
        d.angularVelocity = vec3(0.0f, 0.0f, 300.0f);  // and it starts spinning far past that
        f.app.world.emplace<projv::Debris>(e, d);
        float fastest = 0.0f;
        for (int i = 0; i < 120; i++) {
            f.frames(1);
            fastest = std::max(fastest, glm::length(f.app.world.get<projv::Debris>(e).angularVelocity));
        }
        CHECK(fastest <= limit + 1e-3f);
        f.frames(8 * 60);
        CHECK(f.app.world.get<projv::Debris>(e).resting);
    }
}

TEST_CASE("debris: debris meets moving bodies too -- a crate stops it, and it falls when the crate is gone") {
    // Debris met static bodies only: a cannonball through a crate threw its debris on through the crate's
    // far side and the wall beyond, both of them bodies that can move.
    Fixture f(on());
    auto crateAt = [&](vec3 at) {
        ComponentHandle h = utils::addComponent(f.scene, projv::ComponentKind::Chunk, "box", INVALID_COMPONENT_HANDLE, 16, 0.25f);
        utils::setComponentTransform(f.scene, h, at, quat(1, 0, 0, 0), 1.0f);
        utils::queueVoxelAdd(f.scene, h, fill({0, 0, 0}, {7, 7, 7}));   // 2 m
        utils::updateScene(f.scene);
        Entity e = spawnComponent(f.app.world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        f.app.world.emplace<projv::RigidBody>(e);
        return e;
    };
    Entity wall = crateAt(vec3(3, 0, -1));   // its near face at x = 3
    Entity table = crateAt(vec3(-6, 0, -1));
    f.frames(60);
    auto debrisAt = [&](vec3 at, vec3 velocity) {
        Entity e = f.app.world.create();
        f.app.world.emplace<projv::Transform>(e, projv::Transform{at});
        projv::Debris d;
        d.position = d.previousPosition = at;
        d.radius = 0.15f;
        d.velocity = velocity;
        f.app.world.emplace<projv::Debris>(e, d);
        return e;
    };
    Entity thrown = debrisAt(vec3(0, 1, 0), vec3(12, 0, 0));
    Entity lying = debrisAt(vec3(-5, 2.5f, 0), vec3(0));   // dropped on the table's top, at y = 2
    float furthest = 0.0f;
    for (int i = 0; i < 90; i++) {
        f.frames(1);
        furthest = std::max(furthest, f.app.world.get<projv::Debris>(thrown).position.x);
    }
    CHECK(furthest < 3.0f);   // stopped at the crate's face, not through it
    REQUIRE(f.app.world.get<projv::Debris>(lying).resting);
    CHECK(f.app.world.get<projv::Debris>(lying).position.y > 1.9f);
    f.app.world.destroy(table);
    f.frames(45);
    CHECK(f.app.world.get<projv::Debris>(lying).position.y < 1.0f);   // the table gone, it fell
    (void)wall;
}

TEST_CASE("debris: a networked body's small pieces are bodies still") {
    Fixture f(on());
    Fixture::Crate c = f.crate(vec3(-0.5f, 2.0f, -0.5f));
    assignNetId(f.app.world, c.entity);
    f.frames(2);
    f.cut(c);
    f.frames(2);
    REQUIRE(f.breaks.size() == 1);
    CHECK(f.breaks[0].debris.empty());
    REQUIRE(f.breaks[0].pieces.size() == 1);
    CHECK(f.app.world.all_of<projv::PhysicsBody>(f.breaks[0].pieces[0]));
    CHECK(f.debris().empty());
}

TEST_CASE("debris: off (the default), a small piece is a body") {
    Fixture f(PhysicsConfig::DebrisConfig{});
    Fixture::Crate c = f.crate(vec3(-0.5f, 2.0f, -0.5f));
    f.frames(2);
    f.cut(c);
    f.frames(2);
    REQUIRE(f.breaks.size() == 1);
    CHECK(f.breaks[0].debris.empty());
    CHECK(f.breaks[0].pieces.size() == 1);
}
