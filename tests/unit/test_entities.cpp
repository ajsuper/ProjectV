// Authored entities (runtime/entities.h): entities.json beside compose.json, ECS components saved as
// themselves, linked to components by their local id.

#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "core/application.h"
#include "runtime/entities.h"
#include "utils/compose_io.h"
#include "utils/scene_query.h"

namespace {
    struct Speed { float value = 0.0f; };
    struct Label { std::string text; };
}

template<> struct projv::runtime::ComponentTraits<Speed> {
    static constexpr const char* key = "test.speed";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const Speed& s) { return nlohmann::json{{"value", s.value}}; }
    static std::optional<Speed> load(const nlohmann::json& j, uint32_t) {
        if (!j.contains("value") || !j["value"].is_number()) return std::nullopt;
        return Speed{j["value"].get<float>()};
    }
};
template<> struct projv::runtime::ComponentTraits<Label> {
    static constexpr const char* key = "test.label";
    static constexpr uint32_t version = 2;
    static nlohmann::json save(const Label& l) { return nlohmann::json{{"text", l.text}}; }
    static std::optional<Label> load(const nlohmann::json& j, uint32_t) { return Label{j.at("text").get<std::string>()}; }
};

namespace {
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::vec3;
    namespace utils = projv::utils;
    namespace runtime = projv::runtime;

    struct TempDir {
        std::filesystem::path path;
        explicit TempDir(const std::string& name) {
            path = std::filesystem::temp_directory_path() / ("projv_unit_" + name);
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
        }
        ~TempDir() { std::error_code e; std::filesystem::remove_all(path, e); }
    };

    void writeFile(const std::filesystem::path& path, const std::string& text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << text;
    }

    nlohmann::json readJson(const std::filesystem::path& path) {
        std::ifstream in(path);
        return nlohmann::json::parse(in);
    }

    // An Application whose Scene is loaded from `folder`, with the bridge and the test components.
    struct Fixture {
        projv::Application app;
        projv::Scene* scene = nullptr;
        explicit Fixture(const std::filesystem::path& folder) {
            scene = &app.world.ctx().emplace<projv::Scene>(utils::loadComposeFromDisk(folder.string()));
            runtime::installSceneBridge(app);
            runtime::registerComponent<Speed>(app.world);
            runtime::registerComponent<Label>(app.world);
            app.runStartup();
        }
        ComponentHandle named(const std::string& name) const {
            for (ComponentHandle h = 0; h < scene->components.size(); h++) {
                if (scene->components[h].name == name && utils::isComponentAlive(*scene, h)) return h;
            }
            return INVALID_COMPONENT_HANDLE;
        }
        Entity entityNamed(const std::string& name) {
            for (auto [entity, authored] : app.world.view<runtime::Authored>().each()) {
                if (authored.name == name) return entity;
            }
            return projv::NullEntity;
        }
    };

    vec3 g_seenAt{0.0f};
    void recordPosition(projv::World& world, Entity e) { g_seenAt = world.get<projv::Transform>(e).position; }

    // A top-level folder with two assets, House (id 1, at x = 10) and Shed (id 2).
    void writeTown(const std::filesystem::path& dir, const std::string& entities) {
        writeFile(dir / "House" / "compose.json", R"({"version": 1, "name": "House", "components": []})");
        writeFile(dir / "Shed" / "compose.json", R"({"version": 1, "name": "Shed", "components": []})");
        writeFile(dir / "compose.json", R"({"version": 1, "name": "Town", "components": [
            { "type": "asset", "source": "House", "id": 1, "position": [10, 0, 0] },
            { "type": "asset", "source": "Shed",  "id": 2 } ]})");
        writeFile(dir / "entities.json", entities);
    }
}

TEST_CASE("entities link by id, stand alone, and carry their components as written") {
    TempDir dir("entities_basic");
    writeTown(dir.path, R"({"version": 1, "entities": [
        { "name": "door",  "link": 1, "components": { "test.speed": { "v": 1, "value": 3.5 } } },
        { "name": "rules",            "components": { "test.label": { "v": 2, "text": "hello" } } },
        { "name": "top",   "link": "document" }
    ]})");
    Fixture f(dir.path);
    runtime::SpawnedEntities spawned = runtime::spawnEntities(f.app.world);
    REQUIRE(spawned.entities.size() == 3);

    projv::World& w = f.app.world;
    Entity door = f.entityNamed("door");
    REQUIRE(door != projv::NullEntity);
    REQUIRE(w.all_of<projv::VoxelComponent>(door));
    CHECK(w.get<projv::VoxelComponent>(door).handle == f.named("House"));
    CHECK(w.get<projv::VoxelComponent>(door).mode == projv::LinkMode::Root);
    CHECK(w.get<projv::Transform>(door).position == vec3(10, 0, 0));   // seeded from the component
    CHECK(w.get<Speed>(door).value == doctest::Approx(3.5f));

    Entity rules = f.entityNamed("rules");
    CHECK_FALSE(w.all_of<projv::VoxelComponent>(rules));
    CHECK(w.get<Label>(rules).text == "hello");
    // The top-level folder has no node, so "document" there is no link at all.
    CHECK_FALSE(w.all_of<projv::VoxelComponent>(f.entityNamed("top")));
}

TEST_CASE("a nested folder's entities link inside it, and an outer file adds to and overrides them") {
    TempDir dir("entities_nested");
    writeFile(dir.path / "House" / "Door" / "compose.json", R"({"version": 1, "name": "Door", "components": []})");
    writeFile(dir.path / "House" / "compose.json", R"({"version": 1, "name": "House", "components": [
        { "type": "asset", "source": "Door", "id": 4, "position": [0, 2, 0] } ]})");
    writeFile(dir.path / "House" / "entities.json", R"({"version": 1, "entities": [
        { "name": "door",  "link": 4,          "components": { "test.speed": { "v": 1, "value": 1 } } },
        { "name": "house", "link": "document", "components": { "test.speed": { "v": 1, "value": 2 },
                                                              "test.label": { "v": 2, "text": "inner" } } }
    ]})");
    writeFile(dir.path / "compose.json", R"({"version": 1, "name": "Town", "components": [
        { "type": "asset", "source": "House", "id": 1 } ]})");
    writeFile(dir.path / "entities.json", R"({"version": 1, "entities": [
        { "name": "house from outside", "link": 1, "components": { "test.label": { "v": 2, "text": "outer" } } }
    ]})");

    Fixture f(dir.path);
    runtime::SpawnedEntities spawned = runtime::spawnEntities(f.app.world);
    projv::World& w = f.app.world;
    CHECK(spawned.entities.size() == 2);   // the outer record merged into the house entity

    Entity door = f.entityNamed("door");
    REQUIRE(door != projv::NullEntity);
    CHECK(w.get<projv::VoxelComponent>(door).mode == projv::LinkMode::Part);
    CHECK(w.get<projv::VoxelComponent>(door).handle == f.named("Door"));

    Entity house = f.entityNamed("house");
    REQUIRE(house != projv::NullEntity);
    CHECK(w.get<projv::VoxelComponent>(house).handle == f.named("House"));
    CHECK(w.get<Speed>(house).value == doctest::Approx(2.0f));   // from the inner file
    CHECK(w.get<Label>(house).text == "outer");                  // overridden by the outer one
}

TEST_CASE("unknown and refused components are kept, and written back by saveEntities") {
    TempDir dir("entities_unknown");
    writeTown(dir.path, R"({"version": 1, "entities": [
        { "name": "door", "link": 1, "components": {
            "test.speed":     { "v": 1, "value": "fast" },
            "othergame.ai":   { "v": 3, "brain": [1, 2, 3] } } }
    ]})");
    Fixture f(dir.path);
    runtime::spawnEntities(f.app.world);
    projv::World& w = f.app.world;
    Entity door = f.entityNamed("door");
    CHECK_FALSE(w.all_of<Speed>(door));                         // refused: "fast" is not a number
    REQUIRE(w.all_of<runtime::UnknownComponents>(door));
    CHECK(w.get<runtime::UnknownComponents>(door).byKey.count("othergame.ai") == 1);
    CHECK(w.get<runtime::UnknownComponents>(door).byKey.count("test.speed") == 1);

    TempDir out("entities_unknown_out");
    REQUIRE(runtime::saveEntities(w, INVALID_COMPONENT_HANDLE, out.path.string()));
    nlohmann::json saved = readJson(out.path / "entities.json");
    REQUIRE(saved["entities"].size() == 1);
    CHECK(saved["entities"][0]["components"]["othergame.ai"] == nlohmann::json::parse(R"({ "v": 3, "brain": [1, 2, 3] })"));
    CHECK(saved["entities"][0]["components"]["test.speed"]["value"] == "fast");
}

TEST_CASE("saveEntities round-trips links, components and an unlinked entity's transform") {
    TempDir dir("entities_save");
    writeTown(dir.path, R"({"version": 1, "entities": []})");
    Fixture f(dir.path);
    runtime::spawnEntities(f.app.world);
    projv::World& w = f.app.world;

    // Made in code, then saved: a linked entity and a standalone one.
    Entity shed = runtime::spawnComponent(w, f.named("Shed"), projv::LinkMode::Root, projv::OnUnlink::Destroy);
    w.emplace<Speed>(shed, 9.0f);
    Entity marker = w.create();
    w.emplace<projv::Transform>(marker, projv::Transform{vec3(1, 2, 3), projv::core::quat(1, 0, 0, 0), 1.0f});
    w.emplace<Label>(marker, "spawn point");
    w.emplace<runtime::Authored>(marker, runtime::Authored{INVALID_COMPONENT_HANDLE, "marker"});

    REQUIRE(runtime::saveEntities(w, INVALID_COMPONENT_HANDLE, dir.path.string()));
    nlohmann::json saved = readJson(dir.path / "entities.json");
    REQUIRE(saved["entities"].size() == 2);

    // Reload the folder into a fresh application: the same entities come back.
    Fixture again(dir.path);
    runtime::spawnEntities(again.app.world);
    projv::World& w2 = again.app.world;
    Entity shed2 = runtime::entityFor(w2, again.named("Shed"));
    REQUIRE(shed2 != projv::NullEntity);
    CHECK(w2.get<Speed>(shed2).value == doctest::Approx(9.0f));
    CHECK(w2.get<projv::VoxelComponent>(shed2).onUnlink == projv::OnUnlink::Destroy);
    Entity marker2 = again.entityNamed("marker");
    REQUIRE(marker2 != projv::NullEntity);
    CHECK(w2.get<projv::Transform>(marker2).position == vec3(1, 2, 3));
    CHECK(w2.get<Label>(marker2).text == "spawn point");
    // The linked entity's transform lives in compose.json, not here.
    for (const auto& record : saved["entities"]) {
        if (record.contains("link")) CHECK_FALSE(record["components"].contains("projv.transform"));
    }
}

TEST_CASE("a link to an id the folder does not have spawns unlinked, and keeps the link on save") {
    TempDir dir("entities_missing");
    writeTown(dir.path, R"({"version": 1, "entities": [ { "name": "ghost", "link": 99 } ]})");
    Fixture f(dir.path);
    runtime::spawnEntities(f.app.world);
    Entity ghost = f.entityNamed("ghost");
    REQUIRE(ghost != projv::NullEntity);
    CHECK_FALSE(f.app.world.all_of<projv::VoxelComponent>(ghost));
    TempDir out("entities_missing_out");
    REQUIRE(runtime::saveEntities(f.app.world, INVALID_COMPONENT_HANDLE, out.path.string()));
    CHECK(readJson(out.path / "entities.json")["entities"][0]["link"] == 99);
}

TEST_CASE("a prefab instantiated twice gives two independent entities, each owning its voxels") {
    TempDir prefab("entities_prefab");
    writeFile(prefab.path / "Part" / "compose.json", R"({"version": 1, "name": "Part", "components": []})");
    writeFile(prefab.path / "compose.json", R"({"version": 1, "name": "Crate", "components": [
        { "type": "asset", "source": "Part", "id": 1 } ]})");
    writeFile(prefab.path / "entities.json", R"({"version": 1, "entities": [
        { "link": "document", "components": { "test.speed": { "v": 1, "value": 4 } } }
    ]})");
    TempDir empty("entities_prefab_scene");
    writeFile(empty.path / "compose.json", R"({"version": 1, "name": "Empty", "components": []})");

    Fixture f(empty.path);
    projv::World& w = f.app.world;
    Entity a = runtime::instantiatePrefab(w, prefab.path.string(), vec3(1, 0, 0));
    Entity b = runtime::instantiatePrefab(w, prefab.path.string(), vec3(-1, 0, 0));
    REQUIRE(a != projv::NullEntity);
    REQUIRE(b != projv::NullEntity);
    CHECK(a != b);
    CHECK(w.get<Speed>(a).value == doctest::Approx(4.0f));
    CHECK(w.get<Speed>(b).value == doctest::Approx(4.0f));
    ComponentHandle rootA = w.get<projv::VoxelComponent>(a).handle;
    ComponentHandle rootB = w.get<projv::VoxelComponent>(b).handle;
    CHECK(rootA != rootB);
    CHECK(w.get<projv::Transform>(a).position == vec3(1, 0, 0));

    w.destroy(a);   // OnUnlink::Destroy by default: its voxels go with it
    CHECK_FALSE(utils::isComponentAlive(*f.scene, rootA));
    CHECK(utils::isComponentAlive(*f.scene, rootB));
}

TEST_CASE("a component's on_construct sees the linked Transform, for setup that depends on it") {
    TempDir dir("entities_construct");
    writeTown(dir.path, R"({"version": 1, "entities": [
        { "name": "door", "link": 1, "components": { "test.speed": { "v": 1, "value": 1 } } } ]})");
    Fixture f(dir.path);
    g_seenAt = vec3(-1);
    f.app.world.on_construct<Speed>().connect<&recordPosition>();
    runtime::spawnEntities(f.app.world);
    CHECK(g_seenAt == vec3(10, 0, 0));
}

TEST_CASE("an entity made in code is saved in exactly one file: the one its component is an entry of") {
    TempDir dir("entities_owner");
    writeFile(dir.path / "House" / "Door" / "compose.json", R"({"version": 1, "name": "Door", "components": []})");
    writeFile(dir.path / "House" / "compose.json", R"({"version": 1, "name": "House", "components": [
        { "type": "asset", "source": "Door", "id": 1 } ]})");
    writeFile(dir.path / "compose.json", R"({"version": 1, "name": "Town", "components": [
        { "type": "asset", "source": "House", "id": 1 } ]})");
    Fixture f(dir.path);
    projv::World& w = f.app.world;
    ComponentHandle house = f.named("House");
    w.emplace<Speed>(runtime::spawnComponent(w, house), 1.0f);                                  // an entry of the top
    w.emplace<Speed>(runtime::spawnComponent(w, f.named("Door"), projv::LinkMode::Part), 2.0f);  // an entry of House

    TempDir top("entities_owner_top"), inner("entities_owner_inner");
    REQUIRE(runtime::saveEntities(w, INVALID_COMPONENT_HANDLE, top.path.string()));
    REQUIRE(runtime::saveEntities(w, house, inner.path.string()));
    nlohmann::json topFile = readJson(top.path / "entities.json");
    nlohmann::json innerFile = readJson(inner.path / "entities.json");
    REQUIRE(topFile["entities"].size() == 1);
    REQUIRE(innerFile["entities"].size() == 1);
    CHECK(topFile["entities"][0]["link"] == 1);
    CHECK(topFile["entities"][0]["components"]["test.speed"]["value"] == 1.0);
    CHECK(innerFile["entities"][0]["link"] == 1);
    CHECK(innerFile["entities"][0]["components"]["test.speed"]["value"] == 2.0);
}
