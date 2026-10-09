// utils::deleteComponent / isComponentAlive: deleting a component and its subtree in the engine.

#include "doctest/doctest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>

#include "utils/attachments.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

namespace {
    using projv::ComponentHandle;
    using projv::INVALID_COMPONENT_HANDLE;
    namespace utils = projv::utils;

    struct Mark { int value = 0; };
}

template<> struct projv::utils::AttachmentTraits<Mark> {
    static constexpr const char* key = "test.mark";
    static constexpr uint32_t version = 1;
    static constexpr projv::OnDuplicate onDuplicate = projv::OnDuplicate::Copy;
    static nlohmann::json save(const Mark& m) { return nlohmann::json{{"value", m.value}}; }
    static std::optional<Mark> load(const nlohmann::json& j, uint32_t) { return Mark{j.at("value").get<int>()}; }
};

namespace {
    // A Chunk component with a few voxels in it, so it owns a geometry blob.
    ComponentHandle solidChunk(projv::Scene& scene, const char* name,
                               ComponentHandle parent = INVALID_COMPONENT_HANDLE) {
        ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, name, parent, 4, 1.0f);
        std::vector<projv::PendingVoxelOp> voxels;
        for (int x = 0; x < 2; x++) voxels.push_back({true, projv::core::ivec3(x, 0, 0), 0x3FFFFFFFu});
        utils::queueVoxelAdd(scene, h, voxels);
        utils::updateScene(scene);
        return h;
    }

    bool isLoose(const projv::Scene& scene, projv::ChunkHandle chunk) {
        return std::find(scene.looseChunks.begin(), scene.looseChunks.end(), chunk) != scene.looseChunks.end();
    }
}

TEST_CASE("deleting a chunk kills it, releases its geometry and tombstones the record") {
    projv::Scene scene;
    ComponentHandle h = solidChunk(scene, "Box");
    projv::ChunkHandle chunk = scene.components[h].chunkHandle;
    int32_t blob = scene.chunks[chunk].geometryPoolIndex;
    REQUIRE(blob >= 0);
    REQUIRE(scene.geometryPool[blob].refCount == 1);
    REQUIRE(isLoose(scene, chunk));
    CHECK(utils::isComponentAlive(scene, h));

    std::vector<ComponentHandle> removed = utils::deleteComponent(scene, h);
    CHECK(removed == std::vector<ComponentHandle>{h});
    CHECK_FALSE(utils::isComponentAlive(scene, h));
    CHECK_FALSE(scene.chunks[chunk].alive);
    CHECK_FALSE(isLoose(scene, chunk));
    CHECK(scene.geometryPool[blob].refCount == 0);
    CHECK(scene.deletions == 1);
    CHECK(h < scene.components.size());     // the slot survives
}

TEST_CASE("deleting a grid empties every cell") {
    projv::Scene scene;
    ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "Wide", INVALID_COMPONENT_HANDLE, 4, 1.0f);
    // Voxels past the chunk's resolution turn it into a grid of several cells.
    std::vector<projv::PendingVoxelOp> voxels = {
        {true, projv::core::ivec3(0, 0, 0), 0x3FFFFFFFu},
        {true, projv::core::ivec3(9, 0, 0), 0x3FFFFFFFu},
    };
    utils::queueVoxelAdd(scene, h, voxels);
    utils::updateScene(scene);
    REQUIRE(scene.components[h].kind == projv::ComponentKind::Grid);
    const projv::SceneGrid& grid = scene.grids[scene.components[h].gridIndex];
    std::vector<int32_t> cells;
    for (int32_t cell : grid.cellToChunk) if (cell >= 0) cells.push_back(cell);
    REQUIRE(cells.size() >= 2);

    // Read before deleting: the freed row is emptied, gridIndex included.
    const int32_t gridIndex = scene.components[h].gridIndex;
    utils::deleteComponent(scene, h);
    const projv::SceneGrid& emptied = scene.grids[gridIndex];
    CHECK(std::all_of(emptied.cellToChunk.begin(), emptied.cellToChunk.end(), [](int32_t c) { return c < 0; }));
    CHECK(emptied.componentHandle == INVALID_COMPONENT_HANDLE);
    for (int32_t cell : cells) CHECK_FALSE(scene.chunks[cell].alive);
}

TEST_CASE("deleting an asset tombstones its whole subtree, leaves first, and clears attachments") {
    projv::Scene scene;
    ComponentHandle root = utils::addComponent(scene, projv::ComponentKind::Asset, "House", INVALID_COMPONENT_HANDLE, 4, 1.0f);
    ComponentHandle room = utils::addComponent(scene, projv::ComponentKind::Asset, "Room", root, 4, 1.0f);
    ComponentHandle wall = solidChunk(scene, "Wall", room);
    ComponentHandle other = solidChunk(scene, "Shed");
    for (ComponentHandle h : {root, room, wall, other}) utils::setAttachment(scene, h, Mark{int(h)});
    utils::setAttachment(scene, room, Mark{99}, projv::AttachmentScope::Document);

    std::vector<ComponentHandle> removed = utils::deleteComponent(scene, root);
    CHECK(removed == std::vector<ComponentHandle>{wall, room, root});
    for (ComponentHandle h : {root, room, wall}) {
        CHECK_FALSE(utils::isComponentAlive(scene, h));
        CHECK(scene.components[h].children.empty());
        CHECK(scene.components[h].parent == INVALID_COMPONENT_HANDLE);
        CHECK(utils::getAttachment<Mark>(scene, h) == nullptr);
    }
    CHECK(utils::getAttachment<Mark>(scene, room, projv::AttachmentScope::Document) == nullptr);
    CHECK(utils::isComponentAlive(scene, other));
    CHECK(utils::getAttachment<Mark>(scene, other)->value == int(other));
    CHECK(scene.deletions == 1);              // one call, however many nodes
}

TEST_CASE("deleting a child detaches it from its parent and leaves its siblings") {
    projv::Scene scene;
    ComponentHandle root = utils::addComponent(scene, projv::ComponentKind::Asset, "Root", INVALID_COMPONENT_HANDLE, 4, 1.0f);
    ComponentHandle a = solidChunk(scene, "A", root);
    ComponentHandle b = solidChunk(scene, "B", root);
    utils::deleteComponent(scene, a);
    CHECK(scene.components[root].children == std::vector<ComponentHandle>{b});
    CHECK(utils::isComponentAlive(scene, b));
}

TEST_CASE("deleting what is already deleted, or out of range, does nothing") {
    projv::Scene scene;
    ComponentHandle h = solidChunk(scene, "Box");
    utils::deleteComponent(scene, h);
    CHECK(utils::deleteComponent(scene, h).empty());
    CHECK(utils::deleteComponent(scene, 12345).empty());
    CHECK(utils::deleteComponent(scene, INVALID_COMPONENT_HANDLE).empty());
    CHECK(scene.deletions == 1);
    CHECK_FALSE(utils::isComponentAlive(scene, 12345));
}

TEST_CASE("a deleted component is not saved") {
    auto folder = std::filesystem::temp_directory_path() / "projv_unit_delete_save";
    std::filesystem::remove_all(folder);
    projv::Scene scene;
    ComponentHandle kept = utils::addComponent(scene, projv::ComponentKind::Asset, "Kept", INVALID_COMPONENT_HANDLE, 4, 1.0f);
    ComponentHandle gone = utils::addComponent(scene, projv::ComponentKind::Asset, "Gone", INVALID_COMPONENT_HANDLE, 4, 1.0f);
    (void)kept;
    utils::deleteComponent(scene, gone);
    REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder.string()));
    std::ifstream in(folder / "compose.json");
    nlohmann::json doc = nlohmann::json::parse(in);
    REQUIRE(doc["components"].size() == 1);
    CHECK(doc["components"][0]["name"] == "Kept");
    std::filesystem::remove_all(folder);
}
