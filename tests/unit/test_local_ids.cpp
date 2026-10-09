// Persistent local component ids (ComponentRecord::localId): unique among siblings, saved as the
// entry's "id", and what entity files link by.

#include "doctest/doctest.h"

#include <filesystem>
#include <algorithm>
#include <fstream>

#include "nlohmann/json.hpp"

#include "utils/compose_io.h"
#include "utils/scene_query.h"

namespace {
    using projv::ComponentHandle;
    using projv::INVALID_COMPONENT_HANDLE;
    namespace utils = projv::utils;

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

    ComponentHandle folder(projv::Scene& scene, const char* name, ComponentHandle parent = INVALID_COMPONENT_HANDLE) {
        return utils::addComponent(scene, projv::ComponentKind::Asset, name, parent, 4, 1.0f);
    }

    ComponentHandle named(const projv::Scene& scene, const std::string& name) {
        for (ComponentHandle h = 0; h < scene.components.size(); h++) {
            if (scene.components[h].name == name) return h;
        }
        return INVALID_COMPONENT_HANDLE;
    }
}

TEST_CASE("addComponent numbers siblings, and each parent's children separately") {
    projv::Scene scene;
    ComponentHandle a = folder(scene, "A");
    ComponentHandle b = folder(scene, "B");
    ComponentHandle a1 = folder(scene, "A1", a);
    ComponentHandle a2 = folder(scene, "A2", a);
    ComponentHandle b1 = folder(scene, "B1", b);
    CHECK(scene.components[a].localId == 1);
    CHECK(scene.components[b].localId == 2);
    CHECK(scene.components[a1].localId == 1);
    CHECK(scene.components[a2].localId == 2);
    CHECK(scene.components[b1].localId == 1);

    CHECK(utils::findComponentByLocalId(scene, INVALID_COMPONENT_HANDLE, 2) == b);
    CHECK(utils::findComponentByLocalId(scene, a, 2) == a2);
    CHECK(utils::findComponentByLocalId(scene, b, 2) == INVALID_COMPONENT_HANDLE);
    utils::deleteComponent(scene, a2);
    CHECK(utils::findComponentByLocalId(scene, a, 2) == INVALID_COMPONENT_HANDLE);   // dead is not found
}

TEST_CASE("ids are saved, and survive a reload and a reorder of the file") {
    TempDir dir("ids_roundtrip");
    {
        projv::Scene scene;
        folder(scene, "First");
        folder(scene, "Second");
        folder(scene, "Third");
        REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, dir.path.string()));
    }
    // Reverse the entries by hand, as an editor of the file might.
    std::ifstream in(dir.path / "compose.json");
    nlohmann::json doc = nlohmann::json::parse(in);
    in.close();
    std::reverse(doc["components"].begin(), doc["components"].end());
    std::ofstream(dir.path / "compose.json") << doc.dump(2);

    projv::Scene loaded = utils::loadComposeFromDisk(dir.path.string());
    CHECK(loaded.components[named(loaded, "First")].localId == 1);
    CHECK(loaded.components[named(loaded, "Second")].localId == 2);
    CHECK(loaded.components[named(loaded, "Third")].localId == 3);
    CHECK(loaded.documentPath == std::filesystem::weakly_canonical(dir.path).string());
}

TEST_CASE("missing ids are assigned after the file's own, and a duplicate is renumbered") {
    TempDir dir("ids_assign");
    for (const char* name : {"NoId", "One", "DupA", "DupB"}) {
        writeFile(dir.path / name / "compose.json", std::string(R"({"version": 1, "name": ")") + name + R"(", "components": []})");
    }
    writeFile(dir.path / "compose.json", R"({
        "version": 1, "name": "Doc",
        "components": [
            { "type": "asset", "source": "NoId" },
            { "type": "asset", "source": "One",  "id": 1 },
            { "type": "asset", "source": "DupA", "id": 7 },
            { "type": "asset", "source": "DupB", "id": 7 }
        ]
    })");
    projv::Scene scene = utils::loadComposeFromDisk(dir.path.string());
    CHECK(scene.components[named(scene, "One")].localId == 1);    // the file's own wins
    CHECK(scene.components[named(scene, "DupA")].localId == 7);   // the first of a clash keeps it
    uint32_t noId = scene.components[named(scene, "NoId")].localId;
    uint32_t dupB = scene.components[named(scene, "DupB")].localId;
    CHECK(noId != 0);
    CHECK(dupB != 0);
    for (uint32_t id : {noId, dupB}) { CHECK(id != 1); CHECK(id != 7); }
    CHECK(noId != dupB);
}

TEST_CASE("a hand-built record with no id is numbered in the file it is saved to") {
    TempDir dir("ids_handbuilt");
    projv::Scene scene;
    folder(scene, "Numbered");
    scene.components[0].localId = 0;   // as a record pushed by hand would be
    REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, dir.path.string()));
    std::ifstream in(dir.path / "compose.json");
    nlohmann::json doc = nlohmann::json::parse(in);
    CHECK(doc["components"][0]["id"] == 1);
}

TEST_CASE("a duplicate gets a new id beside its original; its children keep theirs") {
    projv::Scene scene;
    ComponentHandle house = folder(scene, "House");
    ComponentHandle door = folder(scene, "Door", house);
    ComponentHandle window = folder(scene, "Window", house);
    ComponentHandle copy = utils::duplicateComponent(scene, house);
    REQUIRE(copy != INVALID_COMPONENT_HANDLE);
    CHECK(scene.components[copy].localId != scene.components[house].localId);
    ComponentHandle copiedDoor = utils::findComponentByLocalId(scene, copy, scene.components[door].localId);
    ComponentHandle copiedWindow = utils::findComponentByLocalId(scene, copy, scene.components[window].localId);
    REQUIRE(copiedDoor != INVALID_COMPONENT_HANDLE);
    REQUIRE(copiedWindow != INVALID_COMPONENT_HANDLE);
    CHECK(copiedDoor != door);
}

TEST_CASE("a reparented component is renumbered only if it clashes") {
    projv::Scene scene;
    ComponentHandle a = folder(scene, "A");
    ComponentHandle b = folder(scene, "B");
    ComponentHandle a1 = folder(scene, "A1", a);         // id 1 under A
    ComponentHandle b1 = folder(scene, "B1", b);         // id 1 under B
    ComponentHandle a2 = folder(scene, "A2", a);         // id 2 under A
    REQUIRE(utils::setComponentParent(scene, a2, b));    // 2 is free under B: kept
    CHECK(scene.components[a2].localId == 2);
    REQUIRE(utils::setComponentParent(scene, a1, b));    // 1 is B1's: renumbered
    CHECK(scene.components[a1].localId == 3);
    CHECK(scene.components[b1].localId == 1);
}

TEST_CASE("a folder grafted twice keeps its ids in each copy, under nodes that remember the folder") {
    TempDir dir("ids_graft");
    writeFile(dir.path / "Inner" / "compose.json", R"({"version": 1, "name": "Inner", "components": []})");
    writeFile(dir.path / "compose.json", R"({"version": 1, "name": "Prefab",
        "components": [ { "type": "asset", "source": "Inner", "id": 5 } ]})");
    projv::Scene scene;
    ComponentHandle first = utils::instantiateComposeInto(scene, dir.path.string(), INVALID_COMPONENT_HANDLE);
    ComponentHandle second = utils::instantiateComposeInto(scene, dir.path.string(), INVALID_COMPONENT_HANDLE);
    REQUIRE(first != INVALID_COMPONENT_HANDLE);
    REQUIRE(second != INVALID_COMPONENT_HANDLE);
    CHECK(scene.components[first].localId != scene.components[second].localId);
    CHECK(utils::findComponentByLocalId(scene, first, 5) != INVALID_COMPONENT_HANDLE);
    CHECK(utils::findComponentByLocalId(scene, second, 5) != INVALID_COMPONENT_HANDLE);
    CHECK(utils::findComponentByLocalId(scene, first, 5) != utils::findComponentByLocalId(scene, second, 5));
    CHECK(scene.components[first].sourcePath == std::filesystem::weakly_canonical(dir.path).string());
}
