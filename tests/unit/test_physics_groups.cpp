// Collision groups (the physics plan, R4): which bodies pass through which, as data -- groups a body
// is in and groups it ignores, and families whose members never collide -- for bodies, characters
// and queries; changed at run time without rebuilding a body; recorded, replayed and restored.

#include "doctest/doctest.h"

#include <vector>

#include "core/application.h"
#include "core/events.h"
#include "runtime/physics.h"
#include "runtime/physics/physics_world.h"
#include "utils/editing.h"
#include "utils/material.h"
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
    constexpr uint32_t A = 1u << 0, B = 1u << 1;

    BodyId addFloor(PhysicsWorld& physics) {
        BodyDesc floor;
        floor.shape = PhysicsShape::box({20.0f, 0.5f, 20.0f});
        floor.motion = MotionType::Static;
        floor.layer = PhysicsLayer::Static;
        floor.position = {0, -0.5f, 0};
        return physics.createBody(floor);
    }
    BodyDesc crateAt(vec3 position, CollisionFilter filter = {}) {
        BodyDesc d;
        d.shape = PhysicsShape::box({0.5f, 0.5f, 0.5f});
        d.position = position;
        d.filter = filter;
        return d;
    }
    void steps(PhysicsWorld& physics, int n) { for (int i = 0; i < n; i++) physics.step(DT); }
    float heightOf(PhysicsWorld& physics, BodyId id) { return physics.bodyState(id).position.y; }
}

TEST_CASE("groups: a body passes through the groups it ignores, and lands on everything else") {
    PhysicsWorld physics;
    addFloor(physics);
    BodyId below = physics.createBody(crateAt({0, 0.5f, 0}, {.groups = A}));
    BodyId ignoring = physics.createBody(crateAt({0, 3.0f, 0}, {.ignores = A}));
    BodyId plain = physics.createBody(crateAt({3, 0.5f, 0}, {.groups = A}));
    BodyId onTop = physics.createBody(crateAt({3, 3.0f, 0}));
    steps(physics, 4 * 60);
    CHECK(heightOf(physics, below) == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(heightOf(physics, ignoring) == doctest::Approx(0.5f).epsilon(0.02));   // inside it, on the floor
    CHECK(heightOf(physics, plain) == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(heightOf(physics, onTop) == doctest::Approx(1.5f).epsilon(0.02));      // stacked

    // Either side ignoring is enough, and a group a body is not in changes nothing.
    CHECK_FALSE(filtersCollide({.groups = A}, {.ignores = A}));
    CHECK_FALSE(filtersCollide({.ignores = A}, {.groups = A | B}));
    CHECK(filtersCollide({.groups = A}, {.ignores = B}));
    CHECK(filtersCollide({}, {.ignores = A | B}));
    CHECK(filtersCollide({}, {}));
}

TEST_CASE("groups: bodies of one family pass through each other, and nothing else") {
    PhysicsWorld physics;
    addFloor(physics);
    BodyId low = physics.createBody(crateAt({0, 0.5f, 0}, {.family = 7}));
    BodyId high = physics.createBody(crateAt({0, 3.0f, 0}, {.family = 7}));
    BodyId other = physics.createBody(crateAt({0, 6.0f, 0}, {.family = 8}));
    steps(physics, 5 * 60);
    CHECK(heightOf(physics, low) == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(heightOf(physics, high) == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(heightOf(physics, other) == doctest::Approx(1.5f).epsilon(0.02));   // on the pair of them
    CHECK_FALSE(filtersCollide({.family = 3}, {.family = 3}));
    CHECK(filtersCollide({.family = 0}, {.family = 0}));   // no family is not a family
}

TEST_CASE("groups: a filter changed mid-run ends a resting contact, waking what it must") {
    PhysicsWorld physics;
    addFloor(physics);
    BodyId below = physics.createBody(crateAt({0, 0.5f, 0}, {.groups = A}));
    BodyId above = physics.createBody(crateAt({0, 1.5f, 0}));
    steps(physics, 3 * 60);
    REQUIRE(heightOf(physics, above) == doctest::Approx(1.5f).epsilon(0.02));
    REQUIRE_FALSE(physics.bodyState(above).awake);   // asleep on it

    physics.setFilter(above, {.ignores = A});
    CHECK(physics.filter(above) == CollisionFilter{.ignores = A});
    steps(physics, 2 * 60);
    CHECK(heightOf(physics, above) == doctest::Approx(0.5f).epsilon(0.02));   // through it, onto the floor

    // And back: the one on top was not sleeping inside, so it is pushed out rather than staying.
    physics.setFilter(above, {});
    steps(physics, 2 * 60);
    CHECK(glm::distance(physics.bodyState(above).position, physics.bodyState(below).position) > 0.9f);
    CHECK(physics.filter(BodyId{}) == CollisionFilter{});   // a stale id: the default
}

TEST_CASE("groups: a character walks through what it ignores, and is stopped by the rest") {
    auto walk = [](CollisionFilter character, CollisionFilter wall) {
        PhysicsWorld physics;
        addFloor(physics);
        BodyDesc w;
        w.shape = PhysicsShape::box({0.2f, 1.5f, 3.0f});
        w.motion = MotionType::Static;
        w.layer = PhysicsLayer::Static;
        w.position = {2.0f, 1.5f, 0.0f};
        w.filter = wall;
        physics.createBody(w);
        CharacterDesc desc;
        desc.position = {0, 0.05f, 0};
        desc.filter = character;
        CharacterId c = physics.createCharacter(desc);
        steps(physics, 20);
        physics.setCharacterInput(c, {2.0f, 0, 0}, false);
        steps(physics, 3 * 60);
        return physics.characterState(c).position.x;
    };
    CHECK(walk({}, {.groups = A}) < 1.8f);                  // stopped at the wall
    CHECK(walk({.ignores = A}, {.groups = A}) > 4.0f);      // through it
    CHECK(walk({.groups = B}, {.ignores = B}) > 4.0f);      // the wall ignoring it is the same
    CHECK(walk({.family = 2}, {.family = 2}) > 4.0f);

    // Its inner body, which other bodies meet, has its filter: a crate dropped on a character that
    // ignores crates falls through it.
    PhysicsWorld physics;
    addFloor(physics);
    CharacterDesc desc;
    desc.position = {0, 0.05f, 0};
    desc.filter = {.ignores = A};
    CharacterId c = physics.createCharacter(desc);
    BodyId crate = physics.createBody(crateAt({0, 3.0f, 0}, {.groups = A}));
    steps(physics, 3 * 60);
    CHECK(heightOf(physics, crate) == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(physics.filter(physics.characterBody(c)) == desc.filter);
    // Changed later: crates land on it again.
    physics.setCharacterFilter(c, {});
    physics.setPose(crate, {0, 4.0f, 0}, quat(1, 0, 0, 0));
    steps(physics, 3 * 60);
    CHECK(heightOf(physics, crate) > 1.5f);
}

TEST_CASE("groups: a query skips the groups it is told to") {
    PhysicsWorld physics;
    BodyDesc near = crateAt({0, 0, 3}, {.groups = A});
    near.motion = MotionType::Static;
    near.layer = PhysicsLayer::Static;
    BodyDesc far = crateAt({0, 0, 6});
    far.motion = MotionType::Static;
    far.layer = PhysicsLayer::Static;
    BodyId a = physics.createBody(near), b = physics.createBody(far);
    physics.step(DT);
    auto hit = physics.castRay({0, 0, 0}, {0, 0, 1}, 20.0f);
    REQUIRE(hit);
    CHECK(hit->body == a);
    QueryFilter skip;
    skip.skipGroups = A;
    hit = physics.castRay({0, 0, 0}, {0, 0, 1}, 20.0f, skip);
    REQUIRE(hit);
    CHECK(hit->body == b);
    CHECK(physics.castSphere({0, 0, 0}, 0.2f, {0, 0, 1}, 20.0f, skip)->body == b);
    CHECK(physics.overlapSphere({0, 0, 3}, 0.6f, skip).empty());
    CHECK(physics.overlapSphere({0, 0, 3}, 0.6f).size() == 1);
}

TEST_CASE("groups: filters are recorded, replayed, and part of a snapshot") {
    PhysicsSettings settings;
    settings.record = true;
    PhysicsWorld physics(settings);
    addFloor(physics);
    BodyId below = physics.createBody(crateAt({0, 0.5f, 0}, {.groups = A}));
    BodyId above = physics.createBody(crateAt({0, 2.5f, 0}, {.family = 4}));
    CharacterDesc desc;
    desc.position = {3, 0.05f, 0};
    desc.filter = {.groups = B};
    CharacterId c = physics.createCharacter(desc);
    steps(physics, 60);
    const std::vector<uint8_t> saved = physics.saveState();
    physics.setFilter(above, {.ignores = A});
    physics.setCharacterFilter(c, {.ignores = A});
    steps(physics, 60);
    CHECK(heightOf(physics, above) < 1.0f);
    ReplayResult replayed = PhysicsWorld::replay(physics.recording());
    CAPTURE(replayed.problem);
    CHECK(replayed.matched);
    CHECK(replayed.stepsReplayed == 120);

    REQUIRE(physics.restoreState(saved));
    CHECK(physics.filter(above) == CollisionFilter{.family = 4});
    CHECK(physics.filter(below) == CollisionFilter{.groups = A});
    CHECK(physics.filter(physics.characterBody(c)) == CollisionFilter{.groups = B});
}

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
            utils::setComponentTransform(scene, h, corner, quat(1, 0, 0, 0), 1.0f);
            std::vector<projv::PendingVoxelOp> ops;
            for (int z = 0; z < size.z; z++)
                for (int y = 0; y < size.y; y++)
                    for (int x = 0; x < size.x; x++) ops.push_back({true, ivec3(x, y, z), 0x3FFFFFFFu});
            utils::queueVoxelAdd(scene, h, ops);
            utils::updateScene(scene);
            return h;
        }
        Entity crate(vec3 corner) {
            Entity e = spawnComponent(app.world, block("crate", corner, ivec3(4), 0.25f), projv::LinkMode::Root,
                                      projv::OnUnlink::Destroy);
            app.world.emplace<projv::RigidBody>(e);
            return e;
        }
        CollisionFilter filterOf(Entity e) { return physicsWorld(app.world).filter(app.world.get<projv::PhysicsBody>(e).id); }
    };
}

TEST_CASE("groups: names get bits in definition order, 32 of them") {
    Fixture f;
    projv::World& world = f.app.world;
    CHECK(definePhysicsGroup(world, "shards") == A);
    CHECK(definePhysicsGroup(world, "player") == B);
    CHECK(definePhysicsGroup(world, "shards") == A);   // again: the same bit
    CHECK(physicsGroup(world, "player") == B);
    CHECK(physicsGroup(world, "nobody") == 0);
    CHECK(definePhysicsGroup(world, "") == 0);
    for (uint32_t i = 2; i < MAX_COLLISION_GROUPS; i++) CHECK(definePhysicsGroup(world, "g" + std::to_string(i)) == (1u << i));
    CHECK(definePhysicsGroup(world, "one too many") == 0);
}

TEST_CASE("groups: CollisionGroups filters an entity's body, and changing it does not rebuild the body") {
    Fixture f;
    projv::World& world = f.app.world;
    const uint32_t shards = definePhysicsGroup(world, "shards");
    Entity floor = spawnComponent(world, f.block("floor", vec3(-5, -1, -5), ivec3(40, 4, 40), 0.25f));
    world.emplace<projv::StaticCollider>(floor);
    Entity crate = f.crate(vec3(0, 0, 0));
    world.emplace<projv::CollisionGroups>(crate, projv::CollisionGroups{{"shards"}, {"later"}, 0});
    f.frames(2);
    CHECK(f.filterOf(crate) == CollisionFilter{.groups = shards});   // "later" is not defined yet
    const BodyId id = world.get<projv::PhysicsBody>(crate).id;

    // Defining the name it waited for reaches it at the next step.
    const uint32_t later = definePhysicsGroup(world, "later");
    f.frames(1);
    CHECK(f.filterOf(crate) == CollisionFilter{.groups = shards, .ignores = later});

    world.patch<projv::CollisionGroups>(crate, [](projv::CollisionGroups& g) { g.family = 9; });
    f.frames(1);
    CHECK(f.filterOf(crate) == CollisionFilter{.groups = shards, .ignores = later, .family = 9});
    CHECK(world.get<projv::PhysicsBody>(crate).id == id);   // the same body

    world.remove<projv::CollisionGroups>(crate);
    f.frames(1);
    CHECK(f.filterOf(crate) == CollisionFilter{});
    CHECK(world.get<projv::PhysicsBody>(crate).id == id);

    // A floor that ignores shards: a crate in that group falls through it.
    world.emplace<projv::CollisionGroups>(floor, projv::CollisionGroups{{}, {"shards"}, 0});
    world.emplace<projv::CollisionGroups>(crate, projv::CollisionGroups{{"shards"}, {}, 0});
    f.frames(60);
    CHECK(bodyPose(world, crate).position.y < -2.0f);
}

TEST_CASE("groups: pieces copy their body's groups, and a BodyBroken handler can give a break's shards a family") {
    Fixture f;
    projv::World& world = f.app.world;
    definePhysicsGroup(world, "rubble");
    PhysicsMaterial stone{2400.0f, 0.7f, 0.0f};
    stone.strength = 2e4f;
    stone.shardSize = 0.5f;
    definePhysicsMaterial(world, "stone", stone);
    ComponentHandle wall = f.block("wall", vec3(-2, 0, 2), ivec3(16, 12, 2), 0.25f);
    for (size_t s = 0; s < f.scene.components[wall].materialPalette.size(); s++)
        utils::setMaterialPhysics(f.scene, wall, uint8_t(s), "stone");
    Entity wallEntity = spawnComponent(world, wall);
    projv::StaticCollider collider;
    collider.splits = true;
    collider.anchorBase = true;
    world.emplace<projv::StaticCollider>(wallEntity, collider);
    world.emplace<projv::CollisionGroups>(wallEntity, projv::CollisionGroups{{"rubble"}, {}, 0});
    f.frames(1);

    std::vector<Entity> shards;
    f.app.events().on<projv::BodyBroken>([&](const projv::BodyBroken& e) {
        for (size_t i = 0; i < e.shards; i++) {
            world.patch<projv::CollisionGroups>(e.pieces[i], [](projv::CollisionGroups& g) { g.family = 77; });
            shards.push_back(e.pieces[i]);
        }
    });
    fracture(world, wallEntity, vec3(0.0f, 1.5f, 2.0f), 4e4f, vec3(0, 0, 2));
    f.frames(1);
    REQUIRE(shards.size() > 2);
    const uint32_t rubble = physicsGroup(world, "rubble");
    for (Entity e : shards) {
        REQUIRE(world.all_of<projv::PhysicsBody>(e));
        CHECK(f.filterOf(e) == CollisionFilter{.groups = rubble, .family = 77});   // made with it, not a step later
    }
}

TEST_CASE("groups: CollisionGroups saves as physics.groups and loads back") {
    using Traits = ComponentTraits<projv::CollisionGroups>;
    projv::CollisionGroups g{{"shards", "glass"}, {"player"}, 12};
    nlohmann::json j = Traits::save(g);
    CHECK(j.dump() == R"({"family":12,"groups":["shards","glass"],"ignores":["player"]})");
    auto back = Traits::load(j, 1);
    REQUIRE(back);
    CHECK(back->groups == g.groups);
    CHECK(back->ignores == g.ignores);
    CHECK(back->family == 12);
    CHECK(Traits::save(projv::CollisionGroups{}).dump() == "{}");
    CHECK_FALSE(Traits::load(nlohmann::json{{"groups", "shards"}}, 1));       // not a list
    CHECK_FALSE(Traits::load(nlohmann::json{{"ignores", {1, 2}}}, 1));        // not names

    // The debris layer is gone: a file naming it is refused, and kept as written.
    CHECK_FALSE(ComponentTraits<projv::RigidBody>::load(nlohmann::json{{"layer", "debris"}}, 1));
}
