// Component attachments (utils/attachments.h): program data the engine stores, copies and saves
// without interpreting.
//
// Every scene here is built from Asset components, which save and load without any voxel data, so
// the tests exercise the attachment paths and nothing else.

#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "utils/attachments.h"
#include "utils/compose_io.h"
#include "utils/scene_query.h"

namespace {
    // A typed attachment, Copy on duplicate. Records the version its last load was handed.
    struct Tag {
        std::string value;
    };
    uint32_t g_lastTagVersion = 0;

    // A second typed attachment, Drop on duplicate.
    struct Scratch {
        int count = 0;
    };
}

template<> struct projv::utils::AttachmentTraits<Tag> {
    static constexpr const char* key = "test.tag";
    static constexpr uint32_t version = 2;
    static constexpr projv::OnDuplicate onDuplicate = projv::OnDuplicate::Copy;
    static nlohmann::json save(const Tag& tag) { return nlohmann::json{{"value", tag.value}}; }
    static std::optional<Tag> load(const nlohmann::json& json, uint32_t fileVersion) {
        g_lastTagVersion = fileVersion;
        if (!json.is_object() || !json.contains("value") || !json["value"].is_string()) return std::nullopt;
        return Tag{json["value"].get<std::string>()};
    }
};

template<> struct projv::utils::AttachmentTraits<Scratch> {
    static constexpr const char* key = "test.scratch";
    static constexpr uint32_t version = 1;
    static constexpr projv::OnDuplicate onDuplicate = projv::OnDuplicate::Drop;
    static nlohmann::json save(const Scratch& scratch) { return nlohmann::json{{"count", scratch.count}}; }
    static std::optional<Scratch> load(const nlohmann::json& json, uint32_t) {
        return Scratch{json.at("count").get<int>()};   // Throws on a bad shape; the engine catches it.
    }
};

namespace {
    using projv::AttachmentScope;
    using projv::ComponentHandle;
    using projv::INVALID_COMPONENT_HANDLE;
    namespace utils = projv::utils;

    // A fresh directory per test, removed afterwards.
    struct TempDir {
        std::filesystem::path path;
        explicit TempDir(const std::string& name) {
            path = std::filesystem::temp_directory_path() / ("projv_unit_" + name);
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
        }
        ~TempDir() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
        std::string str() const { return path.string(); }
    };

    void writeFile(const std::filesystem::path& path, const std::string& text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << text;
    }

    nlohmann::json readJson(const std::filesystem::path& path) {
        std::ifstream in(path);
        return nlohmann::json::parse(in);
    }

    ComponentHandle addFolder(projv::Scene& scene, const std::string& name,
                              ComponentHandle parent = INVALID_COMPONENT_HANDLE) {
        return utils::addComponent(scene, projv::ComponentKind::Asset, name, parent, 4, 1.0f);
    }

    ComponentHandle findByName(const projv::Scene& scene, const std::string& name) {
        for (ComponentHandle handle = 0; handle < scene.components.size(); handle++) {
            if (scene.components[handle].name == name) return handle;
        }
        return INVALID_COMPONENT_HANDLE;
    }
}

TEST_CASE("typed attachments can be set, read, replaced and removed") {
    projv::Scene scene;
    ComponentHandle a = addFolder(scene, "A");
    ComponentHandle b = addFolder(scene, "B");

    CHECK(utils::getAttachment<Tag>(scene, a) == nullptr);

    utils::setAttachment(scene, a, Tag{"first"});
    REQUIRE(utils::getAttachment<Tag>(scene, a) != nullptr);
    CHECK(utils::getAttachment<Tag>(scene, a)->value == "first");
    CHECK(utils::getAttachment<Tag>(scene, b) == nullptr);
    CHECK(utils::hasAttachment(scene, a, "test.tag"));

    utils::setAttachment(scene, a, Tag{"second"});
    const projv::Scene& readOnly = scene;
    REQUIRE(utils::getAttachment<Tag>(readOnly, a) != nullptr);
    CHECK(utils::getAttachment<Tag>(readOnly, a)->value == "second");

    // The two scopes are separate: a document attachment on `a` is not a component attachment.
    utils::setAttachment(scene, a, Tag{"folder"}, AttachmentScope::Document);
    CHECK(utils::getAttachment<Tag>(scene, a)->value == "second");
    CHECK(utils::getAttachment<Tag>(scene, a, AttachmentScope::Document)->value == "folder");

    utils::removeAttachment<Tag>(scene, a);
    CHECK(utils::getAttachment<Tag>(scene, a) == nullptr);
    CHECK(utils::getAttachment<Tag>(scene, a, AttachmentScope::Document) != nullptr);
}

TEST_CASE("raw attachments decode on first typed access, with the version they were written at") {
    projv::Scene scene;
    ComponentHandle a = addFolder(scene, "A");
    ComponentHandle b = addFolder(scene, "B");

    // One entry at a time, so the version each was handed can be read off on its own.
    utils::attachRaw(scene, a, AttachmentScope::Component, {{"test.tag", R"({"v": 7, "value": "seven"})"}});
    g_lastTagVersion = 0;
    REQUIRE(utils::getAttachment<Tag>(scene, a) != nullptr);
    CHECK(utils::getAttachment<Tag>(scene, a)->value == "seven");
    CHECK(g_lastTagVersion == 7);

    // A raw entry arriving at a key that is already bound is still decoded on the next read.
    utils::attachRaw(scene, b, AttachmentScope::Component, {{"test.tag", R"({"value": "unversioned"})"}});
    REQUIRE(utils::getAttachment<Tag>(scene, b) != nullptr);
    CHECK(utils::getAttachment<Tag>(scene, b)->value == "unversioned");
    CHECK(g_lastTagVersion == 1);   // The file said nothing: 1.

    // Once typed, an entry is written through the traits, at the type's own version.
    auto texts = utils::attachmentsForSave(scene, a, AttachmentScope::Component);
    REQUIRE(texts.count("test.tag") == 1);
    nlohmann::json saved = nlohmann::json::parse(texts["test.tag"]);
    CHECK(saved["v"] == 2);
    CHECK(saved["value"] == "seven");
}

TEST_CASE("an entry its type refuses is kept, and written back exactly as read") {
    projv::Scene scene;
    ComponentHandle a = addFolder(scene, "A");
    ComponentHandle b = addFolder(scene, "B");
    const std::string refused = R"({"v":1,"value":42})";      // value is not a string
    const std::string throws  = R"({"v":1,"wrong":true})";    // Scratch::load's at() throws

    utils::attachRaw(scene, a, AttachmentScope::Component, {{"test.tag", refused}});
    utils::attachRaw(scene, b, AttachmentScope::Component, {{"test.scratch", throws}});

    CHECK(utils::getAttachment<Tag>(scene, a) == nullptr);
    CHECK(utils::getAttachment<Scratch>(scene, b) == nullptr);
    CHECK(utils::hasAttachment(scene, a, "test.tag"));
    CHECK(utils::attachmentsForSave(scene, a, AttachmentScope::Component)["test.tag"] == refused);
    CHECK(utils::attachmentsForSave(scene, b, AttachmentScope::Component)["test.scratch"] == throws);

    // Setting a typed value replaces the refused one.
    utils::setAttachment(scene, a, Tag{"fixed"});
    CHECK(utils::getAttachment<Tag>(scene, a)->value == "fixed");
}

TEST_CASE("a key no program asks for survives load and save unchanged, in both scopes") {
    TempDir source("unknown_source");
    TempDir output("unknown_output");

    writeFile(source.path / "compose.json", R"({
        "version": 1, "name": "Doc",
        "attachments": { "mygame.level": { "v": 3, "gravity": [0, -9.8, 0], "notes": "keep me" } },
        "components": [
            { "type": "asset", "source": "Child",
              "attachments": { "mygame.spawn": { "v": 1, "kind": "door", "weights": [1, 2, 3] },
                               "other.flag": true } }
        ]
    })");
    writeFile(source.path / "Child" / "compose.json", R"({
        "version": 1, "name": "Child",
        "attachments": { "mygame.folder": { "v": 1, "deep": { "nested": [ { "a": 1 } ] } } },
        "components": []
    })");

    projv::Scene scene = utils::loadComposeFromDisk(source.str());
    REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, output.str()));

    nlohmann::json originalTop = readJson(source.path / "compose.json");
    nlohmann::json savedTop = readJson(output.path / "compose.json");
    CHECK(savedTop["attachments"] == originalTop["attachments"]);
    REQUIRE(savedTop["components"].size() == 1);
    CHECK(savedTop["components"][0]["attachments"] == originalTop["components"][0]["attachments"]);

    nlohmann::json originalChild = readJson(source.path / "Child" / "compose.json");
    nlohmann::json savedChild = readJson(output.path / "Child" / "compose.json");
    CHECK(savedChild["attachments"] == originalChild["attachments"]);
}

TEST_CASE("typed attachments round-trip through disk") {
    TempDir folder("typed_roundtrip");
    {
        projv::Scene scene;
        ComponentHandle parent = addFolder(scene, "Parent");
        ComponentHandle child = addFolder(scene, "Child", parent);
        utils::setAttachment(scene, parent, Tag{"on parent"});
        utils::setAttachment(scene, child, Tag{"on child"});
        utils::setAttachment(scene, child, Scratch{5});
        utils::setAttachment(scene, INVALID_COMPONENT_HANDLE, Tag{"top level"}, AttachmentScope::Document);
        utils::setAttachment(scene, parent, Tag{"parent folder"}, AttachmentScope::Document);
        REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder.str()));
    }

    projv::Scene loaded = utils::loadComposeFromDisk(folder.str());
    ComponentHandle parent = findByName(loaded, "Parent");
    ComponentHandle child = findByName(loaded, "Child");
    REQUIRE(parent != INVALID_COMPONENT_HANDLE);
    REQUIRE(child != INVALID_COMPONENT_HANDLE);

    REQUIRE(utils::getAttachment<Tag>(loaded, parent) != nullptr);
    CHECK(utils::getAttachment<Tag>(loaded, parent)->value == "on parent");
    CHECK(utils::getAttachment<Tag>(loaded, child)->value == "on child");
    REQUIRE(utils::getAttachment<Scratch>(loaded, child) != nullptr);
    CHECK(utils::getAttachment<Scratch>(loaded, child)->count == 5);

    // Document blocks: the top-level folder has no node, a nested folder's is its Asset node.
    REQUIRE(utils::getAttachment<Tag>(loaded, INVALID_COMPONENT_HANDLE, AttachmentScope::Document) != nullptr);
    CHECK(utils::getAttachment<Tag>(loaded, INVALID_COMPONENT_HANDLE, AttachmentScope::Document)->value == "top level");
    REQUIRE(utils::getAttachment<Tag>(loaded, parent, AttachmentScope::Document) != nullptr);
    CHECK(utils::getAttachment<Tag>(loaded, parent, AttachmentScope::Document)->value == "parent folder");

    // Saving one node writes that node's document block as the folder's own.
    TempDir subtree("typed_roundtrip_subtree");
    REQUIRE(utils::saveComposeToDisk(loaded, parent, subtree.str()));
    nlohmann::json written = readJson(subtree.path / "compose.json");
    CHECK(written["attachments"]["test.tag"]["value"] == "parent folder");
}

TEST_CASE("a document with no attachments writes no attachments key") {
    TempDir folder("no_attachments");
    projv::Scene scene;
    ComponentHandle parent = addFolder(scene, "Parent");
    addFolder(scene, "Child", parent);
    REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder.str()));

    CHECK_FALSE(readJson(folder.path / "compose.json").contains("attachments"));
    CHECK_FALSE(readJson(folder.path / "compose.json")["components"][0].contains("attachments"));
    CHECK_FALSE(readJson(folder.path / "Parent" / "compose.json").contains("attachments"));
    CHECK_FALSE(readJson(folder.path / "Parent" / "compose.json")["components"][0].contains("attachments"));

    // Setting None-equivalent state -- removing the only attachment -- leaves nothing behind either.
    utils::setAttachment(scene, parent, Tag{"x"});
    utils::removeAttachment<Tag>(scene, parent);
    TempDir again("no_attachments_again");
    REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, again.str()));
    CHECK_FALSE(readJson(again.path / "compose.json")["components"][0].contains("attachments"));
}

TEST_CASE("duplicateComponent follows each key's OnDuplicate, through the whole subtree") {
    projv::Scene scene;
    ComponentHandle parent = addFolder(scene, "Parent");
    ComponentHandle child = addFolder(scene, "Child", parent);
    utils::setAttachment(scene, parent, Tag{"parent"});
    utils::setAttachment(scene, parent, Scratch{1});
    utils::setAttachment(scene, child, Tag{"child"});
    utils::setAttachment(scene, parent, Tag{"parent folder"}, AttachmentScope::Document);
    utils::attachRaw(scene, child, AttachmentScope::Component, {{"unknown.key", R"({"v":1})"}});

    ComponentHandle copy = utils::duplicateComponent(scene, parent);
    REQUIRE(copy != INVALID_COMPONENT_HANDLE);
    REQUIRE(scene.components[copy].children.size() == 1);
    ComponentHandle copiedChild = scene.components[copy].children[0];

    REQUIRE(utils::getAttachment<Tag>(scene, copy) != nullptr);
    CHECK(utils::getAttachment<Tag>(scene, copy)->value == "parent");
    CHECK(utils::getAttachment<Scratch>(scene, copy) == nullptr);          // Drop
    CHECK(utils::getAttachment<Tag>(scene, copiedChild)->value == "child");
    CHECK(utils::hasAttachment(scene, copiedChild, "unknown.key"));        // raw defaults to Copy
    CHECK(utils::getAttachment<Tag>(scene, copy, AttachmentScope::Document)->value == "parent folder");

    // The copy is independent of the source.
    utils::setAttachment(scene, copy, Tag{"changed"});
    CHECK(utils::getAttachment<Tag>(scene, parent)->value == "parent");
}

TEST_CASE("clearAttachments removes everything on a component, in both scopes") {
    projv::Scene scene;
    ComponentHandle a = addFolder(scene, "A");
    ComponentHandle b = addFolder(scene, "B");
    utils::setAttachment(scene, a, Tag{"a"});
    utils::setAttachment(scene, a, Tag{"a folder"}, AttachmentScope::Document);
    utils::attachRaw(scene, a, AttachmentScope::Component, {{"unknown.key", "1"}});
    utils::setAttachment(scene, b, Tag{"b"});

    utils::clearAttachments(scene, a);
    CHECK(utils::getAttachment<Tag>(scene, a) == nullptr);
    CHECK(utils::getAttachment<Tag>(scene, a, AttachmentScope::Document) == nullptr);
    CHECK_FALSE(utils::hasAttachment(scene, a, "unknown.key"));
    CHECK(utils::getAttachment<Tag>(scene, b)->value == "b");
}

TEST_CASE("copying and moving a Scene keeps its attachments") {
    projv::Scene scene;
    ComponentHandle a = addFolder(scene, "A");
    utils::setAttachment(scene, a, Tag{"typed"});
    utils::setAttachment(scene, a, Tag{"folder"}, AttachmentScope::Document);
    utils::attachRaw(scene, a, AttachmentScope::Component, {{"unknown.key", R"({"v":1})"}});

    projv::Scene copied(scene);
    CHECK(utils::getAttachment<Tag>(copied, a)->value == "typed");
    CHECK(utils::getAttachment<Tag>(copied, a, AttachmentScope::Document)->value == "folder");
    CHECK(utils::hasAttachment(copied, a, "unknown.key"));

    projv::Scene assigned;
    assigned = scene;
    CHECK(utils::getAttachment<Tag>(assigned, a)->value == "typed");

    projv::Scene moved(std::move(copied));
    CHECK(utils::getAttachment<Tag>(moved, a)->value == "typed");
    CHECK(utils::hasAttachment(moved, a, "unknown.key"));

    projv::Scene moveAssigned;
    moveAssigned = std::move(assigned);
    CHECK(utils::getAttachment<Tag>(moveAssigned, a, AttachmentScope::Document)->value == "folder");
}

TEST_CASE("instantiateComposeInto carries attachments, and the grafted folder's block lands on its node") {
    TempDir folder("graft_source");
    writeFile(folder.path / "compose.json", R"({
        "version": 1, "name": "Graft",
        "attachments": { "test.tag": { "v": 2, "value": "graft folder" } },
        "components": [
            { "type": "asset", "source": "Inner",
              "attachments": { "test.tag": { "v": 2, "value": "inner entry" }, "unknown.key": [1, 2] } }
        ]
    })");
    writeFile(folder.path / "Inner" / "compose.json",
              R"({ "version": 1, "name": "Inner", "components": [] })");

    projv::Scene scene;
    ComponentHandle existing = addFolder(scene, "Existing");
    utils::setAttachment(scene, existing, Tag{"existing"});
    utils::setAttachment(scene, INVALID_COMPONENT_HANDLE, Tag{"host document"}, AttachmentScope::Document);

    ComponentHandle root = utils::instantiateComposeInto(scene, folder.str(), INVALID_COMPONENT_HANDLE,
                                                         projv::core::vec3(0.0f),
                                                         projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f);
    REQUIRE(root != INVALID_COMPONENT_HANDLE);
    ComponentHandle inner = findByName(scene, "Inner");
    REQUIRE(inner != INVALID_COMPONENT_HANDLE);

    CHECK(utils::getAttachment<Tag>(scene, inner)->value == "inner entry");
    CHECK(utils::hasAttachment(scene, inner, "unknown.key"));
    REQUIRE(utils::getAttachment<Tag>(scene, root, AttachmentScope::Document) != nullptr);
    CHECK(utils::getAttachment<Tag>(scene, root, AttachmentScope::Document)->value == "graft folder");

    // Nothing that was already there moved or was overwritten.
    CHECK(utils::getAttachment<Tag>(scene, existing)->value == "existing");
    CHECK(utils::getAttachment<Tag>(scene, INVALID_COMPONENT_HANDLE, AttachmentScope::Document)->value
          == "host document");
}

TEST_CASE("a legacy top-level op becomes the editor's attachment, and is saved there") {
    TempDir source("legacy_op_source");
    TempDir output("legacy_op_output");
    writeFile(source.path / "compose.json", R"({
        "version": 1, "name": "Legacy",
        "components": [
            { "type": "asset", "source": "Cut",    "op": "subtract" },
            { "type": "asset", "source": "Placed", "op": "none" },
            { "type": "asset", "source": "Typo",   "op": "subtrakt" },
            { "type": "asset", "source": "Both",   "op": "union",
              "attachments": { "projv.editor.csg": { "v": 1, "op": "intersect" } } }
        ]
    })");
    for (const char* name : {"Cut", "Placed", "Typo", "Both"}) {
        writeFile(source.path / name / "compose.json",
                  std::string(R"({ "version": 1, "name": ")") + name + R"(", "components": [] })");
    }

    projv::ComposeDoc doc = utils::parseComposeJson((source.path / "compose.json").string());
    REQUIRE(doc.components.size() == 4);
    auto csg = [](const projv::ComposeComponent& entry) -> nlohmann::json {
        auto it = entry.attachments.find("projv.editor.csg");
        return it == entry.attachments.end() ? nlohmann::json() : nlohmann::json::parse(it->second);
    };
    CHECK(csg(doc.components[0]) == nlohmann::json({{"v", 1}, {"op", "subtract"}}));
    CHECK(csg(doc.components[1]).is_null());                                    // none: nothing
    CHECK(csg(doc.components[2]) == nlohmann::json({{"v", 1}, {"op", "subtrakt"}}));  // moved verbatim
    CHECK(csg(doc.components[3])["op"] == "intersect");                         // attachment wins

    projv::Scene scene = utils::loadComposeFromDisk(source.str());
    REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, output.str()));
    nlohmann::json saved = readJson(output.path / "compose.json");
    REQUIRE(saved["components"].size() == 4);
    for (const nlohmann::json& entry : saved["components"]) CHECK_FALSE(entry.contains("op"));
    CHECK(saved["components"][0]["attachments"]["projv.editor.csg"]["op"] == "subtract");
    CHECK_FALSE(saved["components"][1].contains("attachments"));
}
