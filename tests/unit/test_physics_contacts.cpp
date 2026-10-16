// Contact and sensor events (the physics plan, R6): bodies that ask are told when they begin and stop
// touching another body, one change per pair however many parts touch, kept through sleep; sensors
// report what enters and leaves them without being collided with, sensing the layers they are told
// to. Deterministic: the same changes whatever the threads, through a replay and a restore.

#include "doctest/doctest.h"

#include <vector>

#include "core/application.h"
#include "core/events.h"
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

    BodyId addFloor(PhysicsWorld& physics) {
        BodyDesc floor;
        floor.shape = PhysicsShape::box({20.0f, 0.5f, 20.0f});
        floor.motion = MotionType::Static;
        floor.layer = PhysicsLayer::Static;
        floor.position = {0, -0.5f, 0};
        return physics.createBody(floor);
    }
    BodyDesc crateAt(vec3 position, bool reports = false) {
        BodyDesc d;
        d.shape = PhysicsShape::box({0.5f, 0.5f, 0.5f});
        d.position = position;
        d.reportContacts = reports;
        return d;
    }
    BodyDesc sensorAt(vec3 position, vec3 half, MotionType motion = MotionType::Static) {
        BodyDesc d;
        d.shape = PhysicsShape::box(half);
        d.position = position;
        d.motion = motion;
        d.layer = PhysicsLayer::Sensor;
        return d;
    }

    // Every change, over many steps, with the tick it came in.
    struct Change { uint64_t tick; BodyId a, b; bool began; };
    std::vector<Change> run(PhysicsWorld& physics, int n) {
        std::vector<Change> out;
        for (int i = 0; i < n; i++) {
            physics.step(DT);
            for (const ContactChange& c : physics.contactChanges()) out.push_back({physics.tick(), c.a, c.b, c.began});
        }
        return out;
    }
    int count(const std::vector<Change>& changes, BodyId body, bool began) {
        int n = 0;
        for (const Change& c : changes) n += (c.a == body || c.b == body) && c.began == began;
        return n;
    }
}

TEST_CASE("contacts: a body that reports begins touching the floor once, and stays touching asleep") {
    PhysicsWorld physics;
    BodyId floor = addFloor(physics);
    BodyId crate = physics.createBody(crateAt({0, 2, 0}, true));
    BodyId quiet = physics.createBody(crateAt({3, 2, 0}));
    std::vector<Change> changes = run(physics, 60);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].began);
    CHECK(((changes[0].a == floor && changes[0].b == crate) || (changes[0].a == crate && changes[0].b == floor)));
    CHECK(count(changes, quiet, true) == 0);

    // It sleeps (Jolt drops a sleeping body's contacts); it has not left the floor.
    changes = run(physics, 5 * 60);
    CHECK_FALSE(physics.bodyState(crate).awake);
    CHECK(changes.empty());
    CHECK(physics.contactPairs().size() == 1);

    // Lifted off: it ends, once.
    physics.setPose(crate, {0, 5, 0}, quat(1, 0, 0, 0));
    changes = run(physics, 2);
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].began);
    CHECK(physics.contactPairs().empty());
    // And lands again.
    changes = run(physics, 90);
    CHECK(count(changes, crate, true) == 1);
    CHECK(count(changes, crate, false) == 0);
}

TEST_CASE("contacts: the change carries where they touched, its normal from a towards b") {
    PhysicsWorld physics;
    addFloor(physics);
    BodyId crate = physics.createBody(crateAt({1, 2, -1}, true));
    for (int i = 0; i < 60 && physics.contactChanges().empty(); i++) physics.step(DT);
    REQUIRE(physics.contactChanges().size() == 1);
    const ContactChange& c = physics.contactChanges()[0];
    CHECK(c.point.y == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(c.point.x == doctest::Approx(1.0f).epsilon(0.1));
    CHECK(c.point.z == doctest::Approx(-1.0f).epsilon(0.1));
    const float up = c.a == crate ? -1.0f : 1.0f;   // a towards b: the crate is above the floor
    CHECK(c.normal.y == doctest::Approx(up).epsilon(0.01));
    CHECK_FALSE(c.sensor);
}

TEST_CASE("contacts: destroying a body ends its pairs at the next step") {
    PhysicsWorld physics;
    addFloor(physics);
    BodyId crate = physics.createBody(crateAt({0, 0.5f, 0}, true));
    run(physics, 10);
    REQUIRE(physics.contactPairs().size() == 1);
    physics.destroyBody(crate);
    CHECK(physics.contactPairs().empty());
    std::vector<Change> changes = run(physics, 2);
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].began);
    CHECK((changes[0].a == crate || changes[0].b == crate));
}

TEST_CASE("contacts: a voxel body touching in many places is one pair") {
    PhysicsWorld physics;
    addFloor(physics);
    // Two feet, one body: a compound of two boxes standing apart.
    BodyDesc table;
    table.shape = PhysicsShape::compound({{PhysicsShape::box({0.2f, 0.5f, 0.2f}), {-1, 0, 0}, quat(1, 0, 0, 0)},
                                          {PhysicsShape::box({0.2f, 0.5f, 0.2f}), {1, 0, 0}, quat(1, 0, 0, 0)}});
    table.position = {0, 1, 0};
    table.reportContacts = true;
    BodyId id = physics.createBody(table);
    std::vector<Change> changes = run(physics, 120);
    CHECK(count(changes, id, true) == 1);
    CHECK(count(changes, id, false) == 0);
}

TEST_CASE("contacts: setReportContacts finds what a body already touches, and ends it when turned off") {
    PhysicsWorld physics;
    addFloor(physics);
    BodyId crate = physics.createBody(crateAt({0, 0.5f, 0}));
    run(physics, 4 * 60);   // asleep on the floor, unreported
    REQUIRE_FALSE(physics.bodyState(crate).awake);
    physics.setReportContacts(crate, true);
    CHECK(physics.reportsContacts(crate));
    std::vector<Change> changes = run(physics, 3);
    CHECK(count(changes, crate, true) == 1);
    physics.setReportContacts(crate, false);
    changes = run(physics, 1);
    CHECK(count(changes, crate, false) == 1);
    CHECK(physics.contactPairs().empty());
}

TEST_CASE("contacts: a sensor reports what passes through it, and is not collided with") {
    PhysicsWorld physics;
    addFloor(physics);
    BodyId sensor = physics.createBody(sensorAt({0, 3, 0}, {2, 0.5f, 2}));
    CHECK(physics.reportsContacts(sensor));
    BodyId ball = physics.createBody(crateAt({0, 6, 0}));
    std::vector<Change> changes = run(physics, 2 * 60);
    CHECK(count(changes, ball, true) == 1);
    CHECK(count(changes, ball, false) == 1);
    CHECK(physics.bodyState(ball).position.y == doctest::Approx(0.5f).epsilon(0.05));   // fell through it to the floor
    for (const Change& c : changes) CHECK((c.a == sensor || c.b == sensor));         // never the floor
    bool sensorFlag = false;
    physics.setPose(ball, {0, 3, 0}, quat(1, 0, 0, 0));
    physics.step(DT);
    for (const ContactChange& c : physics.contactChanges()) sensorFlag = sensorFlag || c.sensor;
    CHECK(sensorFlag);
}

TEST_CASE("contacts: a body asleep in a sensor is still in it, until it is moved out or destroyed") {
    PhysicsWorld physics;
    addFloor(physics);
    BodyId plate = physics.createBody(sensorAt({0, 0.5f, 0}, {1, 0.5f, 1}));
    BodyId crate = physics.createBody(crateAt({0, 2, 0}));
    BodyId other = physics.createBody(crateAt({0.2f, 4, 0}));
    std::vector<Change> changes = run(physics, 6 * 60);
    CHECK(count(changes, crate, true) == 1);
    CHECK(count(changes, other, true) == 1);
    CHECK(count(changes, plate, false) == 0);
    CHECK_FALSE(physics.bodyState(crate).awake);
    CHECK(physics.contactPairs().size() == 2);

    // Moved out while asleep: setPose wakes it, and the step it is awake through ends the pair.
    physics.setPose(other, {5, 0.5f, 0}, quat(1, 0, 0, 0));
    changes = run(physics, 3);
    CHECK(count(changes, other, false) == 1);
    CHECK(count(changes, crate, false) == 0);
    physics.destroyBody(crate);
    changes = run(physics, 1);
    CHECK(count(changes, crate, false) == 1);
    CHECK(physics.contactPairs().empty());
}

TEST_CASE("contacts: a sensor senses only the layers it is told to") {
    PhysicsWorld physics;
    BodyDesc onlyCharacters = sensorAt({0, 0, 0}, {2, 2, 2});
    onlyCharacters.senses = layerBit(PhysicsLayer::Character);
    BodyId sensor = physics.createBody(onlyCharacters);
    BodyId crate = physics.createBody(crateAt({0, 0, 0}));
    std::vector<Change> changes = run(physics, 10);
    CHECK(changes.empty());

    CharacterDesc walker;
    walker.position = {0, -0.5f, 0};
    CharacterId character = physics.createCharacter(walker);
    REQUIRE(character.valid());
    physics.setCharacterInput(character, {0.5f, 0, 0}, false);   // moving, so its inner body is awake
    changes = run(physics, 10);
    CHECK(count(changes, physics.characterBody(character), true) == 1);
    CHECK(count(changes, crate, true) == 0);
    (void)sensor;
}

TEST_CASE("contacts: a moving sensor senses static bodies when told to") {
    PhysicsWorld physics;
    BodyId floor = addFloor(physics);
    BodyDesc probe = sensorAt({0, 3, 0}, {0.5f, 0.5f, 0.5f}, MotionType::Kinematic);
    probe.senses |= layerBit(PhysicsLayer::Static);
    BodyId sensor = physics.createBody(probe);
    BodyDesc blind = sensorAt({4, 3, 0}, {0.5f, 0.5f, 0.5f}, MotionType::Kinematic);
    BodyId other = physics.createBody(blind);
    for (int i = 1; i <= 60; i++) {
        physics.moveKinematic(sensor, {0, 3.0f - 0.05f * float(i), 0}, quat(1, 0, 0, 0), DT);
        physics.moveKinematic(other, {4, 3.0f - 0.05f * float(i), 0}, quat(1, 0, 0, 0), DT);
        physics.step(DT);
        for (const ContactChange& c : physics.contactChanges()) {
            CHECK((c.a == sensor || c.b == sensor));
            CHECK((c.a == floor || c.b == floor));
        }
    }
    REQUIRE(physics.contactPairs().size() == 1);
    (void)other;
}

namespace {
    // A scene with things entering and leaving sensors, sleeping on reporting floors, and destroyed.
    // With `save`: saved `stepsBefore` steps after the destroy, and `after` is every change since.
    std::vector<Change> scenario(PhysicsWorld& physics, int stepsBefore = 0, std::vector<uint8_t>* save = nullptr,
                                 std::vector<Change>* after = nullptr) {
        addFloor(physics);
        physics.createBody(sensorAt({0, 2, 0}, {3, 1, 3}));
        std::vector<BodyId> crates;
        for (int i = 0; i < 12; i++)
            crates.push_back(physics.createBody(crateAt({float(i % 4) - 1.5f, 3.0f + 1.1f * float(i / 4), 0.1f * float(i)}, i % 3 == 0)));
        std::vector<Change> out = run(physics, 40);
        physics.destroyBody(crates[5]);
        physics.addImpulse(crates[2], {0, 20, 3});
        if (save) {
            run(physics, stepsBefore);
            *save = physics.saveState();
        }
        std::vector<Change> rest = run(physics, 200);
        if (after) *after = rest;
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }
    bool same(const std::vector<Change>& x, const std::vector<Change>& y) {
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); i++)
            if (x[i].tick != y[i].tick || x[i].a != y[i].a || x[i].b != y[i].b || x[i].began != y[i].began) return false;
        return true;
    }
}

TEST_CASE("contacts: the same changes on any number of threads") {
    PhysicsSettings one;
    one.threads = 0;
    PhysicsSettings many;
    many.threads = 4;
    PhysicsWorld a(one), b(many);
    std::vector<Change> x = scenario(a), y = scenario(b);
    CHECK(x.size() > 12);
    CHECK(same(x, y));
}

TEST_CASE("contacts: reports are recorded and replayed, and a restore carries who touches whom") {
    PhysicsSettings settings;
    settings.record = true;
    PhysicsWorld physics(settings);
    scenario(physics);
    physics.setReportContacts(BodyId{}, true);   // a stale id: recorded, ignored
    ReplayResult replayed = PhysicsWorld::replay(physics.recording());
    CHECK(replayed.matched);
    CHECK(replayed.problem.empty());

    // Saved midway, run on, restored, run on again: the same changes from the save on.
    PhysicsWorld world;
    std::vector<uint8_t> snapshot;
    std::vector<Change> first;
    scenario(world, 10, &snapshot, &first);
    REQUIRE(world.restoreState(snapshot));
    std::vector<Change> again = run(world, 200);
    CHECK_FALSE(first.empty());
    CHECK(same(first, again));
}

// ---- Entities ------------------------------------------------------------------------------------

namespace {
    struct Fixture {
        projv::Application app;
        projv::Scene& scene;
        double now = 0.0;
        std::vector<projv::ContactBegan> began;
        std::vector<projv::ContactEnded> ended;
        Fixture() : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            installSceneBridge(app);
            installPhysics(app);
            app.events().on<projv::ContactBegan>([this](const projv::ContactBegan& e) { began.push_back(e); });
            app.events().on<projv::ContactEnded>([this](const projv::ContactEnded& e) { ended.push_back(e); });
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
        Entity floor() {
            Entity e = spawnComponent(app.world, block("floor", vec3(-5, -1, -5), ivec3(40, 4, 40), 0.25f));
            app.world.emplace<projv::StaticCollider>(e);
            return e;
        }
        Entity crate(vec3 corner) {
            Entity e = spawnComponent(app.world, block("crate", corner, ivec3(4), 0.25f), projv::LinkMode::Root,
                                      projv::OnUnlink::Destroy);
            app.world.emplace<projv::RigidBody>(e);
            return e;
        }
        // A static sensor: a slab of voxels on the sensor layer.
        Entity plate(vec3 corner, ivec3 size) {
            Entity e = spawnComponent(app.world, block("plate", corner, size, 0.25f));
            projv::StaticCollider sensor;
            sensor.layer = PhysicsLayer::Sensor;
            app.world.emplace<projv::StaticCollider>(e, sensor);
            return e;
        }
        int began_(Entity e) const {
            int n = 0;
            for (const auto& b : began) n += b.a == e || b.b == e;
            return n;
        }
        int ended_(Entity e) const {
            int n = 0;
            for (const auto& b : ended) n += b.a == e || b.b == e;
            return n;
        }
    };
}

TEST_CASE("contacts: an entity that asks is told it landed, once, and what it touches") {
    Fixture f;
    projv::World& world = f.app.world;
    Entity floor = f.floor();
    Entity crate = f.crate(vec3(0, 1, 0));
    Entity quiet = f.crate(vec3(3, 1, 0));
    world.emplace<projv::ContactReports>(crate);
    bool seenInStep = false;
    f.app.addSystem(projv::Stage::FixedUpdate, "test: contacts", [&](projv::Application& app) {
        for (const BodyContact& c : contacts(app.world)) seenInStep = seenInStep || (c.began && (c.a == crate || c.b == crate));
    }, {.after = "physics"});
    f.frames(5 * 60);
    REQUIRE(f.began.size() == 1);
    CHECK(((f.began[0].a == floor && f.began[0].b == crate) || (f.began[0].a == crate && f.began[0].b == floor)));
    CHECK(f.began[0].point.y == doctest::Approx(0.0f).epsilon(0.05));
    CHECK_FALSE(f.began[0].sensor);
    CHECK(seenInStep);
    CHECK(f.ended.empty());
    CHECK(f.began_(quiet) == 0);
    CHECK(touching(world, crate) == std::vector<Entity>{floor});
    CHECK(touching(world, floor) == std::vector<Entity>{crate});

    // Its body rebuilt under the contact (a changed RigidBody): nothing to say.
    world.patch<projv::RigidBody>(crate, [](projv::RigidBody& b) { b.friction = 0.9f; });
    f.frames(10);
    CHECK(f.began.size() == 1);
    CHECK(f.ended.empty());

    // Destroyed: it ended.
    world.destroy(crate);
    f.frames(2);
    REQUIRE(f.ended.size() == 1);
    CHECK((f.ended[0].a == crate || f.ended[0].b == crate));
    CHECK(touching(world, floor).empty());
}

TEST_CASE("contacts: asking and no longer asking, on a body already resting") {
    Fixture f;
    projv::World& world = f.app.world;
    Entity floor = f.floor();
    Entity crate = f.crate(vec3(0, 0, 0));
    f.frames(4 * 60);
    CHECK(f.began.empty());
    world.emplace<projv::ContactReports>(crate);
    f.frames(3);
    CHECK(f.began_(crate) == 1);
    CHECK(touching(world, crate) == std::vector<Entity>{floor});
    world.remove<projv::ContactReports>(crate);
    f.frames(2);
    CHECK(f.ended_(crate) == 1);
    CHECK(touching(world, crate).empty());
}

TEST_CASE("contacts: a sensor collider reports what passes through, and `only` picks whom") {
    Fixture f;
    projv::World& world = f.app.world;
    definePhysicsGroup(world, "player");
    f.floor();
    Entity trigger = f.plate(vec3(-1, 2, -1), ivec3(16, 2, 16));
    Entity crate = f.crate(vec3(0, 4, 0));
    f.frames(3 * 60);
    CHECK(f.began_(crate) == 1);
    CHECK(f.ended_(crate) == 1);
    CHECK(f.began.size() == 1);   // the sensor never reports the floor
    CHECK(f.began[0].sensor);
    CHECK(bodyPose(world, crate).position.y < 0.5f);   // fell through it

    // Only players, now: a crate that is not one passes unreported; one in the group is reported.
    world.emplace<projv::ContactReports>(trigger, projv::ContactReports{{"player"}});
    Entity other = f.crate(vec3(2, 4, 0));
    Entity player = f.crate(vec3(-0.5f, 4, 2));
    world.emplace<projv::CollisionGroups>(player, projv::CollisionGroups{{"player"}, {}, 0});
    f.frames(3 * 60);
    CHECK(f.began_(other) == 0);
    CHECK(f.began_(player) == 1);
    CHECK(f.ended_(player) == 1);
    (void)trigger;
}

TEST_CASE("contacts: a crate settled on a pressure plate stays on it, and a restore keeps it there") {
    Fixture f;
    projv::World& world = f.app.world;
    f.floor();
    Entity plate = f.plate(vec3(-1, 0, -1), ivec3(12, 2, 12));
    Entity crate = f.crate(vec3(0, 1, 0));
    f.frames(6 * 60);
    CHECK_FALSE(bodyPose(world, crate).awake);
    CHECK(touching(world, plate) == std::vector<Entity>{crate});
    CHECK(f.ended.empty());

    std::vector<uint8_t> saved = savePhysics(world);
    teleport(world, crate, vec3(3, 3, 3), quat(1, 0, 0, 0));
    f.frames(3);
    CHECK(f.ended_(crate) == 1);
    CHECK(touching(world, plate).empty());
    REQUIRE(restorePhysics(world, saved));
    CHECK(touching(world, plate) == std::vector<Entity>{crate});
    const size_t before = f.began.size() + f.ended.size();
    f.frames(30);
    CHECK(f.began.size() + f.ended.size() == before);   // nothing new: it is where it was
}

TEST_CASE("contacts: a sensor reports a character walking into it") {
    Fixture f;
    projv::World& world = f.app.world;
    f.floor();
    Entity trigger = f.plate(vec3(2, 0, -1), ivec3(8, 8, 8));
    Entity walker = world.create();
    world.emplace<projv::Transform>(walker, projv::Transform{vec3(0, 0, 0)});
    world.emplace<projv::CharacterController>(walker);
    f.frames(5);
    setCharacterInput(world, walker, vec3(2, 0, 0));
    f.frames(90);
    CHECK(f.began_(walker) == 1);
    CHECK(touching(world, trigger) == std::vector<Entity>{walker});
}

TEST_CASE("contacts: ContactReports saves as physics.contacts, and senses by name") {
    using projv::runtime::ComponentTraits;
    projv::ContactReports r{{"player", "npc"}};
    nlohmann::json j = ComponentTraits<projv::ContactReports>::save(r);
    CHECK(j["only"] == nlohmann::json{"player", "npc"});
    auto back = ComponentTraits<projv::ContactReports>::load(j, 1);
    REQUIRE(back);
    CHECK(back->only == r.only);
    CHECK(ComponentTraits<projv::ContactReports>::save(projv::ContactReports{}).empty());
    CHECK(ComponentTraits<projv::ContactReports>::load(nlohmann::json::object(), 1));
    CHECK_FALSE(ComponentTraits<projv::ContactReports>::load(nlohmann::json{{"only", "player"}}, 1));

    projv::StaticCollider sensor;
    sensor.layer = PhysicsLayer::Sensor;
    sensor.senses = layerBit(PhysicsLayer::Character);
    j = ComponentTraits<projv::StaticCollider>::save(sensor);
    CHECK(j["senses"] == nlohmann::json{"character"});
    auto loaded = ComponentTraits<projv::StaticCollider>::load(j, 1);
    REQUIRE(loaded);
    CHECK(loaded->senses == sensor.senses);
    CHECK_FALSE(ComponentTraits<projv::StaticCollider>::save(projv::StaticCollider{}).contains("senses"));
    CHECK_FALSE(ComponentTraits<projv::RigidBody>::load(nlohmann::json{{"senses", {"nowhere"}}}, 1));
    projv::RigidBody probe;
    probe.senses |= layerBit(PhysicsLayer::Static);
    auto rigid = ComponentTraits<projv::RigidBody>::load(ComponentTraits<projv::RigidBody>::save(probe), 1);
    REQUIRE(rigid);
    CHECK(rigid->senses == probe.senses);
}
