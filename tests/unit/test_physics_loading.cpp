// Physics under world loading (runtime/loading.h, world-loading.md L3): Kept bodies frozen, colliders
// rebuilt as their blocks come and go, and support that does not drop what it cannot see. CPU only:
// Applications stepped by a clock the test owns, no window.

#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/application.h"
#include "runtime/entities.h"
#include "runtime/loading.h"
#include "runtime/net.h"
#include "runtime/physics.h"
#include "runtime/physics/physics_world.h"
#include "runtime/scene_bridge.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

namespace {
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::ivec3;
    using projv::core::vec3;
    namespace utils = projv::utils;
    namespace runtime = projv::runtime;

    constexpr double DT = 1.0 / 60.0;

    struct TempDir {
        std::filesystem::path path;
        explicit TempDir(const std::string& name) {
            path = std::filesystem::temp_directory_path() / ("projv_unit_" + name);
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
        }
        ~TempDir() { std::error_code e; std::filesystem::remove_all(path, e); }
    };

    std::vector<projv::PendingVoxelOp> box(ivec3 low, ivec3 high) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) ops.push_back({true, ivec3(x, y, z), 0x3FFFFFFFu});
        return ops;
    }

    // Components on disk, each with an entities.json entry carrying `components` (by name).
    struct Part {
        const char* name;
        ivec3 low, high;
        vec3 at;
        float voxel;
        const char* components;
    };
    void writeWorld(const std::filesystem::path& folder, const std::vector<Part>& parts) {
        projv::Scene scene;
        for (const Part& p : parts) {
            ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, p.name, INVALID_COMPONENT_HANDLE, 16, p.voxel);
            utils::queueVoxelAdd(scene, h, box(p.low, p.high));
            utils::updateScene(scene);
            utils::setComponentTransform(scene, h, p.at, projv::core::quat(1, 0, 0, 0), 1.0f);
        }
        REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder.string()));
        projv::Scene saved = utils::loadComposeFromDisk(folder.string());
        std::ofstream out(folder / "entities.json");
        out << R"({ "version": 1, "entities": [)";
        for (size_t i = 0; i < parts.size(); i++) {
            out << (i ? "," : "") << R"({ "name": ")" << parts[i].name << R"(", "link": )"
                << saved.components[i].localId << R"(, "components": )" << parts[i].components << "}";
        }
        out << "] }";
    }

    struct World {
        projv::Application app;
        double now = 0.0;
        std::vector<projv::BodyBroken> broken;
        std::vector<projv::PhysicsBodyRefused> refused;

        World(const std::filesystem::path& folder, utils::LoadPolicy policy) {
            app.clock = [this] { return now; };
            runtime::installNetIds(app.world);
            runtime::WorldLoadingConfig config;
            config.loader.threaded = false;
            config.policy = std::move(policy);
            REQUIRE(runtime::openWorld(app, folder.string(), config));
            runtime::installPhysics(app);
            app.events().on<projv::BodyBroken>([this](const projv::BodyBroken& e) { broken.push_back(e); });
            app.events().on<projv::PhysicsBodyRefused>([this](const projv::PhysicsBodyRefused& e) { refused.push_back(e); });
            runtime::spawnEntities(app.world);
            app.runStartup();
            app.runFrame();
        }
        void frames(int n) {
            for (int i = 0; i < n; i++) {
                now += DT;
                app.runFrame();
            }
        }
        void viewFrom(vec3 at) {
            runtime::setViewers(app.world, {{at, 0}});
            runtime::settleWorld(app.world);
        }
        Entity named(const std::string& name) {
            for (auto [e, a] : app.world.view<runtime::Authored>().each())
                if (a.name == name) return e;
            return projv::NullEntity;
        }
        projv::Scene& scene() { return app.world.ctx().get<projv::Scene>(); }
    };

    // A floor of 8 x 8 cells of 8 m, its top at y = 1, and a 1 m crate above its far corner.
    std::vector<Part> floorAndCrate() {
        return {{"floor", {0, 0, 0}, {127, 1, 127}, vec3(0.0f), 0.5f, R"({ "physics.static": { "v": 1 } })"},
                {"crate", {0, 0, 0}, {3, 3, 3}, vec3(56.0f, 3.0f, 56.0f), 0.25f,
                 R"({ "physics.rigidbody": { "v": 1 }, "projv.netid": { "v": 1, "id": 42 } })"}};
    }
}

TEST_CASE("physics loading: a body in the kept band is frozen where it is, and thaws with its motion") {
    TempDir dir("physics_loading_freeze");
    writeWorld(dir.path, floorAndCrate());
    World w(dir.path, utils::loadNear(12.0f, 30.0f));
    projv::World& world = w.app.world;

    w.viewFrom(vec3(58.0f, 2.0f, 58.0f));
    w.frames(150);   // it falls onto the floor and settles
    Entity crate = w.named("crate");
    REQUIRE(crate != projv::NullEntity);
    REQUIRE(runtime::hasBody(world, crate));
    CHECK(runtime::bodyPose(world, crate).position.y == doctest::Approx(1.0f).epsilon(0.02));

    // Sliding when the viewer steps back far enough that it is only Kept.
    runtime::setVelocity(world, crate, vec3(3.0f, 0.0f, 0.0f), vec3(0.0f));
    w.frames(1);
    w.viewFrom(vec3(58.0f, 2.0f, 32.0f));
    w.frames(1);
    REQUIRE(world.get<projv::LoadState>(crate).presence == utils::Presence::Kept);
    REQUIRE(world.all_of<projv::FrozenMotion>(crate));
    CHECK(world.get<projv::FrozenMotion>(crate).linear.x > 1.0f);
    const projv::runtime::BodyState held = runtime::bodyPose(world, crate);
    CHECK(glm::length(held.linearVelocity) == 0.0f);
    w.frames(120);
    const projv::runtime::BodyState later = runtime::bodyPose(world, crate);
    CHECK(later.position == held.position);
    CHECK(later.rotation == held.rotation);

    // Near again: it slides on from where it was, as fast as it was going.
    w.viewFrom(vec3(58.0f, 2.0f, 58.0f));
    w.frames(1);
    CHECK_FALSE(world.all_of<projv::FrozenMotion>(crate));
    CHECK(runtime::bodyPose(world, crate).linearVelocity.x > 1.0f);
    CHECK(w.refused.empty());
}

TEST_CASE("physics loading: a moving body that unloads comes back with its motion") {
    TempDir dir("physics_loading_unload");
    writeWorld(dir.path, floorAndCrate());
    World w(dir.path, utils::loadNear(12.0f, 30.0f));
    projv::World& world = w.app.world;
    w.viewFrom(vec3(58.0f, 2.0f, 58.0f));
    w.frames(150);
    Entity crate = runtime::entityForNetId(world, projv::NetId{42});
    REQUIRE(crate != projv::NullEntity);
    const vec3 rested = runtime::bodyPose(world, crate).position;

    // Straight from Active to gone: no Kept step in between.
    runtime::setVelocity(world, crate, vec3(0.0f, 0.0f, 2.5f), vec3(0.0f));
    w.frames(1);
    w.viewFrom(vec3(-200.0f, 2.0f, -200.0f));
    w.frames(1);
    REQUIRE(runtime::entityForNetId(world, projv::NetId{42}) == projv::NullEntity);

    w.viewFrom(vec3(58.0f, 2.0f, 58.0f));
    w.frames(1);
    crate = runtime::entityForNetId(world, projv::NetId{42});
    REQUIRE(crate != projv::NullEntity);
    REQUIRE(runtime::hasBody(world, crate));
    CHECK(runtime::bodyPose(world, crate).linearVelocity.z > 1.0f);
    CHECK(glm::distance(runtime::bodyPose(world, crate).position, rested) < 0.2f);
    CHECK(w.refused.empty());
}

TEST_CASE("physics loading: the floor streams under a walking character, who never falls") {
    TempDir dir("physics_loading_walk");
    writeWorld(dir.path, {floorAndCrate()[0]});
    World w(dir.path, utils::loadNear(10.0f, 18.0f));
    projv::World& world = w.app.world;

    Entity walker = world.create();
    world.emplace<projv::Transform>(walker, projv::Transform{vec3(4.0f, 1.0f, 4.0f)});
    world.emplace<projv::CharacterController>(walker);
    world.emplace<projv::LoadViewer>(walker);
    runtime::settleWorld(world);
    w.frames(30);

    float lowest = 1e9f;
    for (int i = 0; i < 60 * 9; i++) {
        runtime::setCharacterInput(world, walker, vec3(6.0f, 0.0f, 6.0f));
        w.frames(1);
        runtime::settleWorld(world);   // a block per frame is the budget; settle stands for a fast disk
        lowest = std::min(lowest, world.get<projv::Transform>(walker).position.y);
    }
    const vec3 end = world.get<projv::Transform>(walker).position;
    CHECK(end.x > 50.0f);
    CHECK(end.z > 50.0f);
    CHECK(lowest > 0.9f);
    // Only the blocks near it are loaded, and the floor is still one body.
    const auto stats = runtime::worldLoader(world).stats();
    CHECK(stats.loaded < stats.blocks / 2);
    CHECK(runtime::hasBody(world, w.named("floor")));
    CHECK(w.refused.empty());
}

TEST_CASE("physics loading: support counts what is not loaded as holding up what touches it") {
    // A pillar at x = 0 holding up a beam 64 m long, splitting and standing on its base. Seen from the
    // beam's far end, the pillar's cells are not loaded.
    TempDir dir("physics_loading_support");
    writeWorld(dir.path, {{"bridge", {0, 0, 0}, {3, 21, 3}, vec3(0.0f), 0.5f,
                           R"({ "physics.static": { "v": 1, "splits": true, "anchorBase": true } })"}});
    {
        // The beam, added to the same component.
        projv::Scene scene = utils::loadComposeFromDisk(dir.path.string());
        utils::queueVoxelAdd(scene, 0, box({0, 20, 0}, {127, 21, 3}));
        utils::updateScene(scene);
        REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, dir.path.string()));
    }
    World w(dir.path, utils::loadNear(10.0f, 16.0f));
    projv::World& world = w.app.world;
    w.viewFrom(vec3(63.0f, 10.0f, 1.0f));
    w.frames(2);
    Entity bridge = w.named("bridge");
    REQUIRE(bridge != projv::NullEntity);
    ComponentHandle h = runtime::voxelComponentOf(world, bridge);
    REQUIRE(utils::hasUnloadedBlocks(w.scene(), h));
    const uint32_t before = utils::getComponentVoxelCount(w.scene(), h);

    // A notch in the far end: the loaded part reaches no anchor of its own, but it is held.
    runtime::editVoxels(world, bridge, {{false, ivec3(125, 21, 1), 0}});
    w.frames(2);
    CHECK(w.broken.empty());
    CHECK(utils::getComponentVoxelCount(w.scene(), h) == before - 1);

    // With everything loaded, a cut right through the beam does bring the far part down.
    runtime::setLoadRule(world, utils::loadEverything());
    runtime::settleWorld(world);
    w.frames(2);
    std::vector<projv::PendingVoxelOp> cut;
    for (int y = 20; y <= 21; y++)
        for (int z = 0; z <= 3; z++) cut.push_back({false, ivec3(60, y, z), 0});
    runtime::editVoxels(world, bridge, cut);
    w.frames(2);
    CHECK(w.broken.size() == 1);
}

TEST_CASE("physics loading: a frozen body survives snapshot, restore and replay") {
    using namespace projv::runtime;
    PhysicsSettings settings;
    settings.record = true;
    PhysicsWorld physics(settings);
    BodyDesc floor;
    floor.shape = PhysicsShape::box({50.0f, 1.0f, 50.0f});
    floor.motion = MotionType::Static;
    floor.position = vec3(0.0f, -1.0f, 0.0f);
    physics.createBody(floor);
    BodyDesc crate;
    crate.shape = PhysicsShape::box({0.5f, 0.5f, 0.5f});
    crate.motion = MotionType::Dynamic;
    crate.position = vec3(0.0f, 3.0f, 0.0f);
    BodyId id = physics.createBody(crate);
    for (int i = 0; i < 20; i++) physics.step(float(DT));

    physics.setFrozen(id, true);
    CHECK(physics.frozen(id));
    const vec3 frozenAt = physics.bodyState(id).position;
    for (int i = 0; i < 30; i++) physics.step(float(DT));
    CHECK(physics.bodyState(id).position == frozenAt);   // in mid-air, and stays there
    std::vector<uint8_t> snapshot = physics.saveState();

    physics.setFrozen(id, false);
    for (int i = 0; i < 30; i++) physics.step(float(DT));
    CHECK(physics.bodyState(id).position.y < frozenAt.y - 0.5f);   // fell

    REQUIRE(physics.restoreState(snapshot));
    CHECK(physics.frozen(id));
    CHECK(physics.bodyState(id).position == frozenAt);
    for (int i = 0; i < 10; i++) physics.step(float(DT));
    CHECK(physics.bodyState(id).position == frozenAt);

    physics.setFrozen(id, false);
    for (int i = 0; i < 10; i++) physics.step(float(DT));
    ReplayResult replay = PhysicsWorld::replay(physics.recording());
    CHECK(replay.matched);
}
