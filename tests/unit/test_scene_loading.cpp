// Loading part of a scene (utils/scene_loading.h): a scene opens with its tree and no voxels, and
// each block -- one chunk, or one grid cell -- loads and unloads on its own. CPU only.

#include "doctest/doctest.h"

#include <filesystem>
#include <set>
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

TEST_CASE("scene loading: a partly loaded component is not duplicated") {
    TempDir dir("scene_loading_duplicate");
    writeScene(dir.path);
    projv::Scene scene;
    utils::SceneBlocks blocks;
    REQUIRE(blocks.open(scene, dir.path.string()));
    ComponentHandle ground = named(scene, "ground");
    REQUIRE(blocks.load(scene, blockOf(blocks, ground, ivec3(1, 0, 1))));
    CHECK(utils::duplicateComponent(scene, ground) == INVALID_COMPONENT_HANDLE);
    CHECK(utils::duplicateComponent(scene, named(scene, "props")) == INVALID_COMPONENT_HANDLE);
    blocks.loadAll(scene);
    CHECK(utils::duplicateComponent(scene, ground) != INVALID_COMPONENT_HANDLE);
}

// ---- The loader: the rule, budgets, the worker, kept blocks spilled to files -------------------

namespace {
    // A flat floor of 8 x 1 x 8 cells, 8 m each, from the world origin: 64 blocks over 64 m.
    void writeFloor(const std::filesystem::path& folder) {
        projv::Scene scene;
        ComponentHandle floor = utils::addComponent(scene, projv::ComponentKind::Chunk, "floor", INVALID_COMPONENT_HANDLE, 16, 0.5f);
        utils::queueVoxelAdd(scene, floor, box({0, 0, 0}, {127, 0, 127}, true));
        utils::updateScene(scene);
        REQUIRE(scene.grids[size_t(scene.components[floor].gridIndex)].dims == ivec3(8, 1, 8));
        REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder.string()));
    }

    // Every block loaded exactly when `expect` says it should be.
    template <typename Expect>
    void checkLoaded(const projv::Scene& scene, const utils::SceneLoader& loader, Expect expect) {
        for (utils::BlockId id = 0; id < loader.blocks().size(); id++) {
            utils::WorldBox b = loader.blocks().bounds(scene, id);
            INFO("block " << id << " at " << b.min.x << ", " << b.min.z);
            CHECK(loader.blocks().loaded(id) == expect(b));
        }
    }

    utils::LoaderConfig unthreaded() {
        utils::LoaderConfig config;
        config.threaded = false;
        return config;
    }
}

TEST_CASE("loader: by default everything loads, a budget at a time, and matches the eager loader") {
    TempDir dir("loader_default");
    writeFloor(dir.path);
    utils::LoaderConfig config = unthreaded();
    config.maxLoadsPerUpdate = 5;
    utils::SceneLoader loader(config);
    projv::Scene scene;
    REQUIRE(loader.open(scene, dir.path.string()));

    loader.update(scene);
    CHECK(loader.stats().loaded == 5);
    CHECK(loader.stats().wanted == 64);
    loader.settle(scene);
    CHECK(loader.stats().loaded == 64);

    projv::Scene eager = utils::loadComposeFromDisk(dir.path.string());
    CHECK(utils::residentVoxelBytes(scene) == utils::residentVoxelBytes(eager));
    CHECK(utils::getComponentVoxelCount(scene, named(scene, "floor")) == 128 * 128);
}

TEST_CASE("loader: loadNear loads what is near the viewer, nearest first, and follows it") {
    TempDir dir("loader_near");
    writeFloor(dir.path);
    utils::LoaderConfig config = unthreaded();
    config.maxLoadsPerUpdate = 3;
    utils::SceneLoader loader(config);
    projv::Scene scene;
    REQUIRE(loader.open(scene, dir.path.string()));
    loader.setRule(utils::loadNear(6.0f, 14.0f));

    vec3 at(4.0f, 0.25f, 4.0f);   // over the middle of the corner cell
    loader.setViewers({{at}});
    loader.update(scene);
    // The three nearest: the cell underneath first, then two of its neighbours.
    REQUIRE(loader.stats().loaded == 3);
    CHECK(loader.blocks().loaded(0));
    loader.settle(scene);
    checkLoaded(scene, loader, [&](const utils::WorldBox& b) { return utils::distanceToBox(b, at) <= 14.0f; });
    for (utils::BlockId id = 0; id < loader.blocks().size(); id++) {
        float d = utils::distanceToBox(loader.blocks().bounds(scene, id), at);
        if (d <= 6.0f) CHECK(loader.presence(id) == utils::Presence::Active);
        else if (d <= 14.0f) CHECK(loader.presence(id) == utils::Presence::Kept);
    }
    size_t nearCorner = loader.stats().loaded;
    CHECK(nearCorner < 64);

    at = vec3(60.0f, 0.25f, 60.0f);   // the far corner
    loader.setViewers({{at}});
    loader.settle(scene);
    checkLoaded(scene, loader, [&](const utils::WorldBox& b) { return utils::distanceToBox(b, at) <= 14.0f; });
    CHECK(loader.stats().loaded == nearCorner);
    CHECK(utils::residentVoxelBytes(scene) > 0);

    loader.setViewers({});   // no viewer: nothing is near anything
    loader.settle(scene);
    CHECK(loader.stats().loaded == 0);
    CHECK(utils::residentVoxelBytes(scene) == 0);
}

TEST_CASE("loader: a block on the edge does not flicker") {
    TempDir dir("loader_margin");
    writeFloor(dir.path);
    utils::LoaderConfig config = unthreaded();
    config.viewerMoveRecheck = 0.1f;   // every step below is asked about at once
    utils::SceneLoader loader(config);
    projv::Scene scene;
    REQUIRE(loader.open(scene, dir.path.string()));
    loader.setRule(utils::loadNear(0.0f, 10.0f, 0.1f));

    // Block 0 is the cell at the origin, x in [0, 8]. Walk away from it along -x.
    REQUIRE(loader.blocks().source(0).entry.gridX == 0);
    REQUIRE(loader.blocks().source(0).entry.gridZ == 0);
    auto standAt = [&](float distance) {
        loader.setViewers({{vec3(-distance, 0.25f, 4.0f)}});
        loader.settle(scene);
        return loader.blocks().loaded(0);
    };
    CHECK(standAt(9.5f));
    CHECK(standAt(10.5f));    // past the radius, within the margin: stays
    CHECK(standAt(9.9f));
    CHECK(standAt(10.9f));
    CHECK_FALSE(standAt(11.2f));
    CHECK_FALSE(standAt(10.5f));   // coming back, the margin does not apply: not yet
    CHECK(standAt(9.9f));
}

TEST_CASE("loader: with a hint the rule is never asked about blocks beyond it") {
    TempDir dir("loader_hint");
    writeFloor(dir.path);
    utils::SceneLoader loader(unthreaded());
    projv::Scene scene;
    REQUIRE(loader.open(scene, dir.path.string()));
    vec3 at(4.0f, 0.25f, 4.0f);
    float farthest = 0.0f;
    std::set<utils::BlockId> asked;
    loader.setRule([&](const utils::LoadCandidate& c, std::span<const utils::Viewer> viewers) {
        farthest = std::max(farthest, utils::distanceToBox(c.bounds, viewers[0].position));
        asked.insert(c.id);
        return utils::Presence::Active;
    }, utils::LoadHint{12.0f});
    loader.setViewers({{at}});
    for (int i = 0; i < 100; i++) loader.update(scene);
    CHECK(farthest <= 12.0f);
    CHECK(asked.size() == loader.stats().loaded);
    CHECK(asked.size() < 16);
    // Asked again now and then, not every update: 100 updates, recheck every 30.
    CHECK(loader.stats().ruleCalls < asked.size() * 8);
}

TEST_CASE("loader: without a hint each block is asked again within recheckUpdates") {
    TempDir dir("loader_recheck");
    writeFloor(dir.path);
    utils::LoaderConfig config = unthreaded();
    config.recheckUpdates = 10;
    utils::SceneLoader loader(config);
    projv::Scene scene;
    REQUIRE(loader.open(scene, dir.path.string()));
    bool wanted = true;
    loader.setRule([&](const utils::LoadCandidate&, std::span<const utils::Viewer>) {
        return wanted ? utils::Presence::Active : utils::Presence::Absent;
    });
    loader.settle(scene);
    REQUIRE(loader.stats().loaded == 64);
    uint64_t calls = loader.stats().ruleCalls;
    wanted = false;
    for (int i = 0; i < 10; i++) loader.update(scene);
    CHECK(loader.stats().ruleCalls - calls == 70);   // a slice of 7 a time: every block, six twice
    CHECK(loader.stats().loaded == 0);
}

TEST_CASE("loader: reads on the worker thread give what reads in update give") {
    TempDir dir("loader_threaded");
    writeFloor(dir.path);
    projv::Scene eager = utils::loadComposeFromDisk(dir.path.string());

    utils::SceneLoader loader;   // threaded by default
    projv::Scene scene;
    REQUIRE(loader.open(scene, dir.path.string()));
    loader.settle(scene);
    CHECK(loader.stats().loaded == 64);
    CHECK(loader.stats().pendingReads == 0);
    CHECK(utils::residentVoxelBytes(scene) == utils::residentVoxelBytes(eager));
    const projv::SceneGrid& ge = eager.grids[0];
    const projv::SceneGrid& gl = scene.grids[0];
    for (size_t i = 0; i < ge.cellToChunk.size(); i++)
        CHECK(blobOf(eager, ge.cellToChunk[i]).geometry == blobOf(scene, gl.cellToChunk[i]).geometry);

    // Reads pending when the rule changes its mind land and are dropped.
    utils::SceneLoader second;
    projv::Scene other;
    REQUIRE(second.open(other, dir.path.string()));
    second.update(other);
    CHECK(second.stats().pendingReads > 0);
    second.setRule(utils::loadInside({vec3(1000.0f), vec3(1001.0f)}));
    second.settle(other);
    CHECK(second.stats().loaded == 0);
    CHECK(second.stats().pendingReads == 0);
    CHECK(utils::residentVoxelBytes(other) == 0);
}

TEST_CASE("loader: edited blocks spill to files past the limit, and come back") {
    TempDir dir("loader_spill");
    TempDir spill("loader_spill_files");
    writeFloor(dir.path);
    utils::LoaderConfig config = unthreaded();
    config.keptMemoryBytes = 0;   // every kept block to a file
    config.spillFolder = (spill.path / "kept").string();
    projv::Scene scene;
    {
        utils::SceneLoader loader(config);
        REQUIRE(loader.open(scene, dir.path.string()));
        loader.setRule(utils::loadNear(4.0f, 4.0f));
        loader.setViewers({{vec3(4.0f, 0.25f, 4.0f)}});
        loader.settle(scene);
        ComponentHandle floor = named(scene, "floor");
        utils::queueVoxelAdd(scene, floor, box({2, 1, 2}, {5, 4, 5}, true));   // a post on the corner cell
        utils::updateScene(scene);
        CHECK(utils::getComponentVoxelCount(scene, floor) == 16 * 16 * loader.stats().loaded + 64);

        loader.setViewers({{vec3(60.0f, 0.25f, 60.0f)}});
        loader.settle(scene);
        CHECK(loader.stats().kept == 1);
        CHECK(loader.stats().spilled == 1);
        CHECK(loader.blocks().keptBytes() == 0);
        CHECK(std::distance(std::filesystem::directory_iterator(config.spillFolder), {}) == 1);

        loader.setViewers({{vec3(4.0f, 0.25f, 4.0f)}});
        loader.settle(scene);
        CHECK(loader.stats().kept == 0);
        CHECK(std::distance(std::filesystem::directory_iterator(config.spillFolder), {}) == 0);
        CHECK(utils::getComponentVoxelCount(scene, floor) == 16 * 16 * loader.stats().loaded + 64);

        loader.setViewers({{vec3(60.0f, 0.25f, 60.0f)}});
        loader.settle(scene);
        CHECK(loader.stats().spilled == 1);
    }
    // The loader is gone, and its files with it.
    CHECK_FALSE(std::filesystem::exists(config.spillFolder));
}

TEST_CASE("loader: a cell an edit makes is a block, kept when it unloads") {
    TempDir dir("loader_adopt");
    writeFloor(dir.path);
    utils::SceneLoader loader(unthreaded());
    projv::Scene scene;
    REQUIRE(loader.open(scene, dir.path.string()));
    loader.settle(scene);
    ComponentHandle floor = named(scene, "floor");

    // Below the grid's low corner: the grid grows down, and every cell index moves.
    utils::queueVoxelAdd(scene, floor, box({3, -6, 3}, {3, -3, 3}, true));
    utils::updateScene(scene);
    REQUIRE(scene.grids[0].originCellCoord.y == -1);
    const uint32_t voxels = utils::getComponentVoxelCount(scene, floor);
    CHECK(voxels == 128 * 128 + 4);

    loader.update(scene);
    CHECK(loader.stats().blocks == 65);
    CHECK(loader.blocks().edited(scene, 64));

    loader.setRule(utils::loadInside({vec3(1000.0f), vec3(1001.0f)}));
    loader.settle(scene);
    CHECK(loader.stats().loaded == 0);
    CHECK(loader.stats().kept == 1);   // only the new cell: the rest are on disk as they were
    CHECK(utils::getComponentVoxelCount(scene, floor) == 0);

    loader.setRule(utils::loadEverything());
    loader.settle(scene);
    CHECK(utils::getComponentVoxelCount(scene, floor) == voxels);
    CHECK(loader.stats().kept == 0);
}
