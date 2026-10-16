// World loading in the runtime (runtime/loading.h): entities come and go with the blocks they are
// linked to, or by their own bounds. CPU only: an Application stepped by hand, no window.

#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/application.h"
#include "runtime/entities.h"
#include "runtime/loading.h"
#include "runtime/net.h"
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

    struct Tag {
        int value = 0;
    };
}

template<> struct projv::runtime::ComponentTraits<Tag> {
    static constexpr const char* key = "test.tag";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const Tag& t) { return nlohmann::json{{"value", t.value}}; }
    static std::optional<Tag> load(const nlohmann::json& j, uint32_t) { return Tag{j.value("value", 0)}; }
};

namespace {
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

    ComponentHandle named(const projv::Scene& scene, const std::string& name) {
        for (ComponentHandle h = 0; h < scene.components.size(); h++)
            if (utils::isComponentAlive(scene, h) && scene.components[h].name == name) return h;
        return INVALID_COMPONENT_HANDLE;
    }

    // A floor of 8 x 8 cells of 8 m from the origin, a crate on its far corner, and four entities:
    // the crate's (linked, NetId 42), a beacon over the far corner (unlinked, with bounds), the rules
    // (unlinked, no bounds) and the player (a viewer), at the near corner.
    void writeWorld(const std::filesystem::path& folder) {
        projv::Scene scene;
        ComponentHandle floor = utils::addComponent(scene, projv::ComponentKind::Chunk, "floor", INVALID_COMPONENT_HANDLE, 16, 0.5f);
        utils::queueVoxelAdd(scene, floor, box({0, 0, 0}, {127, 0, 127}));
        ComponentHandle crate = utils::addComponent(scene, projv::ComponentKind::Chunk, "crate", INVALID_COMPONENT_HANDLE, 16, 0.25f);
        utils::queueVoxelAdd(scene, crate, box({0, 0, 0}, {15, 15, 15}));
        utils::updateScene(scene);
        utils::setComponentTransform(scene, crate, vec3(56.0f, 1.0f, 56.0f), projv::core::quat(1, 0, 0, 0), 1.0f);
        REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder.string()));

        uint32_t crateId = utils::loadComposeFromDisk(folder.string()).components[1].localId;
        std::ofstream(folder / "entities.json") << R"({ "version": 1, "entities": [
            { "name": "Crate", "link": )" << crateId << R"(,
              "components": { "test.tag": { "v": 1, "value": 7 }, "projv.netid": { "v": 1, "id": 42 } } },
            { "name": "Beacon", "components": { "test.tag": { "v": 1, "value": 3 },
                "projv.transform": { "v": 1, "position": [60, 1, 60] },
                "projv.loadbounds": { "v": 1, "halfExtents": [1, 1, 1] } } },
            { "name": "Rules", "components": { "test.tag": { "v": 1, "value": 1 } } },
            { "name": "Player", "components": { "projv.transform": { "v": 1, "position": [4, 1, 4] },
                "projv.loadviewer": { "v": 1, "id": 1 } } }
        ] })";
    }

    Entity byName(projv::World& world, const std::string& name) {
        for (auto [e, a] : world.view<runtime::Authored>().each())
            if (a.name == name) return e;
        return projv::NullEntity;
    }

    struct Harness {
        projv::Application app;
        std::vector<projv::EntityUnloading> unloading;
        std::vector<projv::EntityReloaded> reloaded;

        explicit Harness(const std::filesystem::path& folder) {
            runtime::WorldLoadingConfig config;
            config.loader.threaded = false;
            config.policy = utils::loadNear(10.0f, 20.0f);
            runtime::installNetIds(app.world);
            REQUIRE(runtime::openWorld(app, folder.string(), config));
            runtime::registerComponent<Tag>(app.world);
            app.events().on<projv::EntityUnloading>([this](const projv::EntityUnloading& e) {
                unloading.push_back(e);
                // Delivered while it still exists.
                CHECK(app.world.valid(e.entity));
            });
            app.events().on<projv::EntityReloaded>([this](const projv::EntityReloaded& e) { reloaded.push_back(e); });
            runtime::spawnEntities(app.world);
        }
        projv::Scene& scene() { return app.world.ctx().get<projv::Scene>(); }
        void walkTo(vec3 at) {
            app.world.patch<projv::Transform>(byName(app.world, "Player"), [&](projv::Transform& t) { t.position = at; });
            runtime::settleWorld(app.world);
            app.runFrame();   // the bridge's sync and the event pump
        }
    };
}

TEST_CASE("world loading: entities linked to far blocks are unloaded with them, and come back as they were") {
    TempDir dir("world_loading_linked");
    writeWorld(dir.path);
    Harness h(dir.path);
    projv::World& world = h.app.world;

    // At the near corner: the crate and the beacon are far; the rules and the player stay.
    runtime::settleWorld(world);
    h.app.runFrame();
    CHECK(runtime::entityForNetId(world, projv::NetId{42}) == projv::NullEntity);
    CHECK(byName(world, "Crate") == projv::NullEntity);
    CHECK(byName(world, "Beacon") == projv::NullEntity);
    CHECK(byName(world, "Rules") != projv::NullEntity);
    CHECK(byName(world, "Player") != projv::NullEntity);
    CHECK(runtime::unloadedEntityCount(world) == 2);
    REQUIRE(h.unloading.size() == 2);
    CHECK(h.unloading[0].netId.value == 42);
    // Its component stayed in the Scene, unloaded.
    ComponentHandle crate = named(h.scene(), "crate");
    REQUIRE(crate != INVALID_COMPONENT_HANDLE);
    CHECK(utils::hasUnloadedBlocks(h.scene(), crate));

    // Walk over: both come back.
    h.walkTo(vec3(54.0f, 1.0f, 54.0f));
    Entity e = runtime::entityForNetId(world, projv::NetId{42});
    REQUIRE(e != projv::NullEntity);
    CHECK(world.get<Tag>(e).value == 7);
    CHECK(world.get<projv::VoxelComponent>(e).handle == crate);
    CHECK(world.get<projv::LoadState>(e).presence == utils::Presence::Active);
    CHECK(byName(world, "Beacon") != projv::NullEntity);
    CHECK(runtime::unloadedEntityCount(world) == 0);
    CHECK(h.reloaded.size() == 2);

    // Change it -- its data and where it is -- and walk away: it keeps both.
    world.get<Tag>(e).value = 99;
    world.patch<projv::Transform>(e, [](projv::Transform& t) { t.position.x = 50.0f; });
    h.app.runFrame();   // the bridge writes the move into the component
    h.walkTo(vec3(4.0f, 1.0f, 4.0f));
    CHECK(runtime::entityForNetId(world, projv::NetId{42}) == projv::NullEntity);
    CHECK(h.scene().components[crate].localPosition.x == doctest::Approx(50.0f));
    h.walkTo(vec3(48.0f, 1.0f, 54.0f));
    e = runtime::entityForNetId(world, projv::NetId{42});
    REQUIRE(e != projv::NullEntity);
    CHECK(world.get<Tag>(e).value == 99);
    CHECK(world.get<projv::Transform>(e).position.x == doctest::Approx(50.0f));
    CHECK(world.get<runtime::Authored>(e).name == "Crate");
}

TEST_CASE("world loading: a viewer entity, Kept and Active, and a deleted component's entity") {
    TempDir dir("world_loading_states");
    writeWorld(dir.path);
    Harness h(dir.path);
    projv::World& world = h.app.world;

    // Within the kept band of the crate but not the active one.
    h.walkTo(vec3(56.0f, 1.0f, 41.0f));
    Entity e = runtime::entityForNetId(world, projv::NetId{42});
    REQUIRE(e != projv::NullEntity);
    CHECK(world.get<projv::LoadState>(e).presence == utils::Presence::Kept);
    h.walkTo(vec3(56.0f, 1.0f, 52.0f));
    CHECK(world.get<projv::LoadState>(e).presence == utils::Presence::Active);

    // The player is never unloaded, wherever it goes.
    h.walkTo(vec3(500.0f, 1.0f, 500.0f));
    CHECK(byName(world, "Player") != projv::NullEntity);
    CHECK(utils::residentVoxelBytes(h.scene()) == 0);

    // A component deleted while its entity is out: the entity does not come back.
    REQUIRE(runtime::unloadedEntityCount(world) == 2);
    utils::deleteComponent(h.scene(), named(h.scene(), "crate"));
    h.walkTo(vec3(54.0f, 1.0f, 54.0f));
    CHECK(runtime::entityForNetId(world, projv::NetId{42}) == projv::NullEntity);
    CHECK(runtime::unloadedEntityCount(world) == 0);
    CHECK(byName(world, "Beacon") != projv::NullEntity);
}

TEST_CASE("world loading: setViewers adds the program's own, and a new rule applies at once") {
    TempDir dir("world_loading_viewers");
    writeWorld(dir.path);
    Harness h(dir.path);
    projv::World& world = h.app.world;
    runtime::settleWorld(world);
    REQUIRE(byName(world, "Crate") == projv::NullEntity);

    runtime::setViewers(world, {{vec3(58.0f, 3.0f, 58.0f), 7}});   // a free camera by the crate
    runtime::settleWorld(world);
    CHECK(byName(world, "Crate") != projv::NullEntity);

    runtime::setViewers(world, {});
    runtime::setLoadRule(world, utils::loadEverything());
    runtime::settleWorld(world);
    CHECK(byName(world, "Crate") != projv::NullEntity);
    CHECK(byName(world, "Beacon") != projv::NullEntity);
    CHECK(runtime::worldLoader(world).stats().loaded == 65);
}
