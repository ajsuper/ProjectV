// Loading part of a scene (utils/scene_loading.h): a scene opens with its tree and no voxels, and
// each block -- one chunk, or one grid cell -- loads and unloads on its own. CPU only.

#include "doctest/doctest.h"

#include <filesystem>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/scene_loading.h"
#include "utils/scene_query.h"

namespace {
    using projv::ComponentHandle;
    using projv::INVALID_CHUNK_HANDLE;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::ivec3;
    using projv::core::vec3;
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

    std::vector<projv::PendingVoxelOp> box(ivec3 low, ivec3 high, bool add, uint32_t colour = 0x3FFFFFFFu) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) ops.push_back({add, ivec3(x, y, z), colour});
        return ops;
    }

    ComponentHandle named(const projv::Scene& scene, const std::string& name) {
        for (ComponentHandle h = 0; h < scene.components.size(); h++)
            if (utils::isComponentAlive(scene, h) && scene.components[h].name == name) return h;
        return INVALID_COMPONENT_HANDLE;
    }

    // A grid floor of 3x1x3 cells, moved, turned and scaled; a crate in a moved folder; and a copy
    // of the crate, which shares its .data.
    void writeScene(const std::filesystem::path& folder) {
        projv::Scene scene;
        ComponentHandle ground = utils::addComponent(scene, projv::ComponentKind::Chunk, "ground", INVALID_COMPONENT_HANDLE, 16, 0.5f);
        utils::queueVoxelAdd(scene, ground, box({0, 0, 0}, {39, 1, 39}, true));
        utils::queueVoxelAdd(scene, ground, box({20, 2, 20}, {22, 5, 22}, true, 0x0FF003FFu));
        utils::updateScene(scene);
        REQUIRE(scene.components[ground].kind == projv::ComponentKind::Grid);
        utils::setComponentTransform(scene, ground, vec3(10.0f, 0.0f, -5.0f),
                                     glm::angleAxis(glm::radians(30.0f), vec3(0, 1, 0)), 2.0f);

        ComponentHandle props = utils::addComponent(scene, projv::ComponentKind::Asset, "props", INVALID_COMPONENT_HANDLE, 4, 1.0f);
        utils::setComponentTransform(scene, props, vec3(1.0f, 0.0f, 0.0f), projv::core::quat(1, 0, 0, 0), 1.0f);
        ComponentHandle crate = utils::addComponent(scene, projv::ComponentKind::Chunk, "crate", props, 16, 0.25f);
        utils::queueVoxelAdd(scene, crate, box({2, 2, 2}, {5, 5, 5}, true));
        utils::updateScene(scene);
        utils::setComponentTransform(scene, crate, vec3(0.0f, 4.0f, 0.0f), projv::core::quat(1, 0, 0, 0), 1.0f);
        ComponentHandle copy = utils::duplicateComponent(scene, crate, props);
        REQUIRE(copy != INVALID_COMPONENT_HANDLE);
        scene.components[copy].name = "crate copy";
        utils::setComponentTransform(scene, copy, vec3(3.0f, 4.0f, 0.0f), projv::core::quat(1, 0, 0, 0), 1.0f);

        REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder.string()));
    }

    const projv::GeometryBlob& blobOf(const projv::Scene& scene, projv::ChunkHandle chunk) {
        return scene.geometryPool[size_t(scene.chunks[chunk].geometryPoolIndex)];
    }

    void checkSameChunk(const projv::Scene& a, projv::ChunkHandle ca, const projv::Scene& b, projv::ChunkHandle cb) {
        const projv::ChunkHeader& x = a.chunks[ca].header;
        const projv::ChunkHeader& y = b.chunks[cb].header;
        CHECK(glm::distance(x.position, y.position) < 1e-4f);
        CHECK(std::abs(glm::dot(x.rotation, y.rotation)) > 1.0f - 1e-5f);
        CHECK(x.scale == doctest::Approx(y.scale));
        CHECK(x.voxelScale == doctest::Approx(y.voxelScale));
        CHECK(x.resolution == y.resolution);
        CHECK(a.chunks[ca].nativeScale == doctest::Approx(b.chunks[cb].nativeScale));
        CHECK(a.chunks[ca].mutability == b.chunks[cb].mutability);
        CHECK(blobOf(a, ca).geometry == blobOf(b, cb).geometry);
        CHECK(blobOf(a, ca).materialIDs == blobOf(b, cb).materialIDs);
    }

    // The block of `component` (the cell at `coord`, for a grid).
    utils::BlockId blockOf(const utils::SceneBlocks& blocks, ComponentHandle component, ivec3 coord = ivec3(0)) {
        for (utils::BlockId id = 0; id < blocks.size(); id++) {
            const utils::SceneBlockSource& s = blocks.source(id);
            if (s.component == component && ivec3(s.entry.gridX, s.entry.gridY, s.entry.gridZ) == coord) return id;
        }
        FAIL("no such block");
        return 0;
    }
}

TEST_CASE("scene loading: a scene opens with its tree and no voxels") {
    TempDir dir("scene_loading_open");
    writeScene(dir.path);

    projv::Scene eager = utils::loadComposeFromDisk(dir.path.string());
    projv::Scene scene;
    utils::SceneBlocks blocks;
    REQUIRE(blocks.open(scene, dir.path.string()));

    CHECK(utils::residentVoxelBytes(scene) == 0);
    CHECK(blocks.size() == 9 + 2);   // nine floor cells, two crates
    for (const char* name : {"ground", "props", "crate", "crate copy"}) {
        ComponentHandle a = named(eager, name), b = named(scene, name);
        REQUIRE(b != INVALID_COMPONENT_HANDLE);
        CHECK(scene.components[b].kind == eager.components[a].kind);
        CHECK(scene.components[b].materialPalette.size() == eager.components[a].materialPalette.size());
        CHECK(glm::distance(utils::getComponentWorldPosition(scene, b), utils::getComponentWorldPosition(eager, a)) < 1e-5f);
    }
    const projv::SceneGrid& grid = scene.grids[size_t(scene.components[named(scene, "ground")].gridIndex)];
    CHECK(std::count(grid.cellToChunk.begin(), grid.cellToChunk.end(), projv::CELL_NOT_LOADED) == 9);
    CHECK(scene.components[named(scene, "crate")].chunkHandle == INVALID_CHUNK_HANDLE);
    CHECK(scene.looseChunks.empty());
    CHECK(utils::hasUnloadedBlocks(scene, named(scene, "ground")));
    CHECK(utils::getComponentVoxelCount(scene, named(scene, "ground")) == 0);
}

TEST_CASE("scene loading: loading every block gives what loadComposeFromDisk does") {
    TempDir dir("scene_loading_all");
    writeScene(dir.path);

    projv::Scene eager = utils::loadComposeFromDisk(dir.path.string());
    projv::Scene scene;
    utils::SceneBlocks blocks;
    REQUIRE(blocks.open(scene, dir.path.string()));
    blocks.loadAll(scene);

    for (const char* name : {"crate", "crate copy"}) {
        ComponentHandle a = named(eager, name), b = named(scene, name);
        REQUIRE(scene.components[b].chunkHandle < scene.chunks.size());
        checkSameChunk(eager, eager.components[a].chunkHandle, scene, scene.components[b].chunkHandle);
    }
    const projv::SceneGrid& ge = eager.grids[size_t(eager.components[named(eager, "ground")].gridIndex)];
    const projv::SceneGrid& gl = scene.grids[size_t(scene.components[named(scene, "ground")].gridIndex)];
    REQUIRE(ge.cellToChunk.size() == gl.cellToChunk.size());
    for (size_t i = 0; i < ge.cellToChunk.size(); i++) {
        REQUIRE((ge.cellToChunk[i] >= 0) == (gl.cellToChunk[i] >= 0));
        if (ge.cellToChunk[i] >= 0) checkSameChunk(eager, ge.cellToChunk[i], scene, gl.cellToChunk[i]);
    }
    CHECK(utils::residentVoxelBytes(scene) == utils::residentVoxelBytes(eager));
    CHECK(scene.looseChunks.size() == eager.looseChunks.size());
    CHECK_FALSE(utils::hasUnloadedBlocks(scene, named(scene, "ground")));

    // The copy shares the crate's voxels, as the eager loader shares them.
    ComponentHandle crate = named(scene, "crate"), copy = named(scene, "crate copy");
    CHECK(scene.chunks[scene.components[crate].chunkHandle].geometryPoolIndex ==
          scene.chunks[scene.components[copy].chunkHandle].geometryPoolIndex);
}

TEST_CASE("scene loading: one cell loads, unloads and loads again; its memory goes with it") {
    TempDir dir("scene_loading_cell");
    writeScene(dir.path);
    projv::Scene scene;
    utils::SceneBlocks blocks;
    REQUIRE(blocks.open(scene, dir.path.string()));

    ComponentHandle ground = named(scene, "ground");
    utils::BlockId middle = blockOf(blocks, ground, ivec3(1, 0, 1));
    REQUIRE(blocks.load(scene, middle));
    const size_t bytes = utils::residentVoxelBytes(scene);
    CHECK(bytes > 0);
    const std::vector<uint32_t> geometry = blobOf(scene, blocks.chunk(middle)).geometry;
    const uint32_t voxels = utils::getComponentVoxelCount(scene, ground);
    CHECK(voxels == 16 * 2 * 16 + 9 * 4);   // a slab of the floor and the whole pillar

    // Only that cell: the rest of the grid is still on disk.
    const projv::SceneGrid& grid = scene.grids[size_t(scene.components[ground].gridIndex)];
    CHECK(std::count(grid.cellToChunk.begin(), grid.cellToChunk.end(), projv::CELL_NOT_LOADED) == 8);

    blocks.unload(scene, middle);
    CHECK_FALSE(blocks.loaded(middle));
    CHECK(utils::residentVoxelBytes(scene) == 0);
    CHECK(std::count(grid.cellToChunk.begin(), grid.cellToChunk.end(), projv::CELL_NOT_LOADED) == 9);
    CHECK(blocks.keptBlocks() == 0);   // not edited: nothing kept

    REQUIRE(blocks.load(scene, middle));
    CHECK(utils::residentVoxelBytes(scene) == bytes);
    CHECK(blobOf(scene, blocks.chunk(middle)).geometry == geometry);

    // A hundred times round leaves memory where one load left it: the pool slot is reused.
    for (int i = 0; i < 100; i++) {
        blocks.unload(scene, middle);
        REQUIRE(blocks.load(scene, middle));
    }
    CHECK(utils::residentVoxelBytes(scene) == bytes);
    CHECK(scene.geometryPool.size() <= 2);
}

TEST_CASE("scene loading: a block's bounds are known before it loads, and are its chunk's cube") {
    TempDir dir("scene_loading_bounds");
    writeScene(dir.path);
    projv::Scene scene;
    utils::SceneBlocks blocks;
    REQUIRE(blocks.open(scene, dir.path.string()));

    for (utils::BlockId id = 0; id < blocks.size(); id++) {
        utils::WorldBox before = blocks.bounds(scene, id);
        CHECK(glm::all(glm::lessThan(before.min, before.max)));
        REQUIRE(blocks.load(scene, id));
        const projv::ChunkHeader& h = scene.chunks[blocks.chunk(id)].header;
        // The chunk's own corners, turned: the box must hold every one and touch them.
        vec3 low(1e30f), high(-1e30f);
        for (int c = 0; c < 8; c++) {
            vec3 at = h.position + glm::mat3_cast(h.rotation) * (vec3(c & 1, (c >> 1) & 1, (c >> 2) & 1) * h.scale);
            low = glm::min(low, at);
            high = glm::max(high, at);
        }
        CHECK(glm::distance(low, before.min) < 1e-4f);
        CHECK(glm::distance(high, before.max) < 1e-4f);
    }
}

TEST_CASE("scene loading: an edited block keeps its edit across an unload") {
    TempDir dir("scene_loading_edit");
    writeScene(dir.path);
    projv::Scene scene;
    utils::SceneBlocks blocks;
    REQUIRE(blocks.open(scene, dir.path.string()));

    ComponentHandle crate = named(scene, "crate"), copy = named(scene, "crate copy");
    utils::BlockId crateBlock = blockOf(blocks, crate), copyBlock = blockOf(blocks, copy);
    REQUIRE(blocks.load(scene, crateBlock));
    REQUIRE(blocks.load(scene, copyBlock));
    CHECK_FALSE(blocks.edited(scene, crateBlock));

    utils::queueVoxelRemove(scene, crate, box({2, 2, 2}, {5, 5, 2}, false));   // one face off
    utils::updateScene(scene);
    CHECK(utils::getComponentVoxelCount(scene, crate) == 4 * 4 * 3);
    CHECK(blocks.edited(scene, crateBlock));
    CHECK_FALSE(blocks.edited(scene, copyBlock));   // its copy shared the voxels, and keeps the old ones

    blocks.unload(scene, crateBlock);
    CHECK(blocks.keptBlocks() == 1);
    CHECK(blocks.keptBytes() > 0);
    CHECK(blocks.edited(scene, crateBlock));

    REQUIRE(blocks.load(scene, crateBlock));
    CHECK(utils::getComponentVoxelCount(scene, crate) == 4 * 4 * 3);
    CHECK(utils::getComponentVoxelCount(scene, copy) == 4 * 4 * 4);
    CHECK(blocks.keptBlocks() == 0);
    CHECK(blocks.edited(scene, crateBlock));   // still not what is on disk

    // And round again: still edited, still kept.
    blocks.unload(scene, crateBlock);
    CHECK(blocks.keptBlocks() == 1);
    REQUIRE(blocks.load(scene, crateBlock));
    CHECK(utils::getComponentVoxelCount(scene, crate) == 4 * 4 * 3);
}

TEST_CASE("scene loading: edits to blocks that are not loaded are dropped, and a save is refused") {
    TempDir dir("scene_loading_refuse");
    writeScene(dir.path);
    projv::Scene scene;
    utils::SceneBlocks blocks;
    REQUIRE(blocks.open(scene, dir.path.string()));

    ComponentHandle crate = named(scene, "crate"), ground = named(scene, "ground");
    utils::queueVoxelAdd(scene, crate, box({0, 0, 0}, {0, 0, 0}, true));
    utils::updateScene(scene);
    CHECK(scene.components[crate].chunkHandle == INVALID_CHUNK_HANDLE);
    CHECK(scene.components[crate].kind == projv::ComponentKind::Chunk);

    // The middle cell loaded, its neighbours not: an add in a neighbour is dropped, in the middle kept.
    REQUIRE(blocks.load(scene, blockOf(blocks, ground, ivec3(1, 0, 1))));
    const uint32_t before = utils::getComponentVoxelCount(scene, ground);
    utils::queueVoxelAdd(scene, ground, box({0, 5, 0}, {0, 5, 0}, true));     // cell (0, 0, 0)
    utils::queueVoxelAdd(scene, ground, box({17, 5, 17}, {17, 5, 17}, true)); // cell (1, 0, 1)
    utils::updateScene(scene);
    CHECK(utils::getComponentVoxelCount(scene, ground) == before + 1);
    const projv::SceneGrid& grid = scene.grids[size_t(scene.components[ground].gridIndex)];
    CHECK(grid.cellToChunk[0] == projv::CELL_NOT_LOADED);

    TempDir out("scene_loading_refuse_out");
    CHECK_FALSE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, out.path.string()));
    CHECK_FALSE(std::filesystem::exists(out.path / "compose.json"));

    blocks.loadAll(scene);
    CHECK(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, out.path.string()));
}

TEST_CASE("scene loading: a deleted component's blocks are forgotten") {
    TempDir dir("scene_loading_delete");
    writeScene(dir.path);
    projv::Scene scene;
    utils::SceneBlocks blocks;
    REQUIRE(blocks.open(scene, dir.path.string()));

    ComponentHandle crate = named(scene, "crate");
    utils::BlockId id = blockOf(blocks, crate);
    REQUIRE(blocks.load(scene, id));
    utils::deleteComponent(scene, crate);
    // Its row may be reused by anything now; unloading must not touch it.
    ComponentHandle other = utils::addComponent(scene, projv::ComponentKind::Chunk, "other", INVALID_COMPONENT_HANDLE, 16, 0.25f);
    utils::queueVoxelAdd(scene, other, box({0, 0, 0}, {1, 1, 1}, true));
    utils::updateScene(scene);
    blocks.unload(scene, id);
    CHECK(utils::getComponentVoxelCount(scene, other) == 8);
    CHECK_FALSE(blocks.load(scene, id));
    CHECK(blocks.bounds(scene, id).min == blocks.bounds(scene, id).max);
}
