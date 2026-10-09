// Slot reuse (Scene::slots): freed component and chunk rows are handed out again, a ComponentRef
// tells a component from a later occupant of its row, and the root index stays exact.

#include "doctest/doctest.h"

#include <algorithm>
#include <filesystem>
#include <random>
#include <set>
#include <vector>

#include "core/application.h"
#include "runtime/entities.h"
#include "runtime/scene_bridge.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

namespace {
    using projv::ChunkHandle;
    using projv::ComponentHandle;
    using projv::ComponentKind;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::ivec3;
    namespace utils = projv::utils;
    namespace runtime = projv::runtime;

    ComponentHandle solid(projv::Scene& scene, const char* name, ComponentHandle parent = INVALID_COMPONENT_HANDLE) {
        ComponentHandle h = utils::addComponent(scene, ComponentKind::Chunk, name, parent, 16, 0.5f);
        utils::queueVoxelAdd(scene, h, {{true, ivec3(1, 2, 3), 0x3FFFFFFFu}});
        utils::updateScene(scene);
        return h;
    }

    // Every live top-level component, found the slow way.
    std::vector<ComponentHandle> scannedRoots(const projv::Scene& scene) {
        std::vector<ComponentHandle> roots;
        for (ComponentHandle h = 0; h < scene.components.size(); h++) {
            if (scene.components[h].parent == INVALID_COMPONENT_HANDLE && utils::isComponentAlive(scene, h)) roots.push_back(h);
        }
        return roots;
    }

    void checkRootIndex(const projv::Scene& scene) {
        std::vector<ComponentHandle> indexed = utils::rootComponents(scene);
        std::vector<ComponentHandle> scanned = scannedRoots(scene);
        std::sort(indexed.begin(), indexed.end());
        CHECK(indexed == scanned);
        std::set<uint32_t> ids;
        for (ComponentHandle h : scanned) ids.insert(scene.components[h].localId);
        CHECK(ids.size() == scanned.size());   // ids unique among the roots
    }

    // Each live chunk component's row points back at it, and each live loose chunk is listed once.
    void checkLinks(const projv::Scene& scene) {
        std::set<ChunkHandle> loose(scene.looseChunks.begin(), scene.looseChunks.end());
        CHECK(loose.size() == scene.looseChunks.size());
        for (ComponentHandle h = 0; h < scene.components.size(); h++) {
            if (!utils::isComponentAlive(scene, h)) continue;
            const projv::ComponentRecord& record = scene.components[h];
            for (ComponentHandle child : record.children) CHECK(scene.components[child].parent == h);
            if (record.parent != INVALID_COMPONENT_HANDLE) {
                const auto& siblings = scene.components[record.parent].children;
                CHECK(std::count(siblings.begin(), siblings.end(), h) == 1);
            }
            if (record.kind != ComponentKind::Chunk) continue;
            REQUIRE(record.chunkHandle < scene.chunks.size());
            const projv::Chunk& chunk = scene.chunks[record.chunkHandle];
            CHECK(chunk.alive);
            CHECK(chunk.componentHandle == h);
            CHECK(loose.count(record.chunkHandle) == 1);
        }
    }
}

TEST_CASE("a deleted component's rows are reused, and a ref to it stops resolving") {
    projv::Scene scene;
    ComponentHandle a = solid(scene, "a");
    ChunkHandle aChunk = scene.components[a].chunkHandle;
    projv::ComponentRef ref = utils::refOf(scene, a);
    CHECK(utils::resolve(scene, ref) == a);

    utils::deleteComponent(scene, a);
    CHECK(utils::resolve(scene, ref) == INVALID_COMPONENT_HANDLE);
    CHECK(scene.components[a].materialPalette.empty());   // emptied while it waits

    ComponentHandle b = solid(scene, "b");
    CHECK(b == a);                                          // the same row...
    CHECK(scene.components[b].chunkHandle == aChunk);       // ...and the same chunk row
    CHECK(utils::resolve(scene, ref) == INVALID_COMPONENT_HANDLE);   // ...but not the same component
    CHECK(utils::resolve(scene, utils::refOf(scene, b)) == b);
    CHECK(scene.chunks[aChunk].headerDirty);
    CHECK(scene.chunks[aChunk].header.chunkID == aChunk);
    checkLinks(scene);
}

TEST_CASE("with recycling off, deleted rows stay tombstones, and refs still notice") {
    projv::Scene scene;
    scene.slots.enabled = false;
    ComponentHandle a = solid(scene, "a");
    projv::ComponentRef ref = utils::refOf(scene, a);
    utils::deleteComponent(scene, a);
    ComponentHandle b = solid(scene, "b");
    CHECK(b != a);
    CHECK(scene.components[b].chunkHandle != scene.components[a].chunkHandle);
    CHECK(scene.components[a].name == "__deleted__");
    CHECK(utils::resolve(scene, ref) == INVALID_COMPONENT_HANDLE);
}

TEST_CASE("spawning and destroying for a long time keeps every table the size of what is alive") {
    projv::Scene scene;
    std::vector<ComponentHandle> alive;
    for (int i = 0; i < 2000; i++) {
        alive.push_back(solid(scene, "body"));
        if (alive.size() > 10) {
            utils::deleteComponent(scene, alive.front());
            alive.erase(alive.begin());
        }
    }
    CHECK(scene.components.size() <= 12);
    CHECK(scene.chunks.size() <= 12);
    CHECK(scene.geometryPool.size() <= 24);
    CHECK(utils::rootComponents(scene).size() == alive.size());
    checkRootIndex(scene);
    checkLinks(scene);
}

TEST_CASE("the root index matches a scan through every kind of change") {
    projv::Scene scene;
    std::mt19937 random(5);
    std::vector<ComponentHandle> assets;
    for (int step = 0; step < 600; step++) {
        std::vector<ComponentHandle> live;
        for (ComponentHandle h = 0; h < scene.components.size(); h++) if (utils::isComponentAlive(scene, h)) live.push_back(h);
        std::vector<ComponentHandle> liveAssets;
        for (ComponentHandle h : live) if (scene.components[h].kind == ComponentKind::Asset) liveAssets.push_back(h);
        auto pick = [&](const std::vector<ComponentHandle>& from) { return from[random() % from.size()]; };

        switch (random() % 6) {
            case 0: utils::addComponent(scene, ComponentKind::Asset, "folder", INVALID_COMPONENT_HANDLE, 4, 1.0f); break;
            case 1:
                if (!liveAssets.empty()) utils::addComponent(scene, ComponentKind::Asset, "inner", pick(liveAssets), 4, 1.0f);
                break;
            case 2: if (live.size() > 3) utils::deleteComponent(scene, pick(live)); break;
            case 3:   // to the top
                if (!live.empty()) utils::setComponentParent(scene, pick(live), INVALID_COMPONENT_HANDLE);
                break;
            case 4:   // into a folder
                if (!live.empty() && !liveAssets.empty()) utils::setComponentParent(scene, pick(live), pick(liveAssets));
                break;
            case 5: {  // a row appended by hand, the way examples build scenes
                ComponentHandle h = static_cast<ComponentHandle>(scene.components.size());
                scene.components.push_back(projv::ComponentRecord{});
                scene.components.back().kind = ComponentKind::Asset;
                scene.components.back().name = "by hand";
                utils::ensureUniqueLocalId(scene, h);
                break;
            }
        }
        checkRootIndex(scene);
    }
}

TEST_CASE("grafting a prefab into a scene with holes maps every row it brings") {
    std::filesystem::path folder = std::filesystem::temp_directory_path() / "projv_unit_slot_prefab";
    std::filesystem::remove_all(folder);
    {
        projv::Scene source;
        ComponentHandle part = utils::addComponent(source, ComponentKind::Asset, "Part", INVALID_COMPONENT_HANDLE, 4, 1.0f);
        solid(source, "left", part);
        solid(source, "right", part);
        solid(source, "lid");
        REQUIRE(utils::saveComposeToDisk(source, INVALID_COMPONENT_HANDLE, folder.string()));
    }

    projv::Scene scene;
    std::vector<ComponentHandle> made;
    for (int i = 0; i < 8; i++) made.push_back(solid(scene, "filler"));
    for (int i = 0; i < 8; i += 2) utils::deleteComponent(scene, made[i]);   // holes in both tables
    size_t rowsBefore = scene.components.size();

    ComponentHandle first = utils::instantiateComposeInto(scene, folder.string(), INVALID_COMPONENT_HANDLE,
                                                          projv::core::vec3(0), projv::core::quat(1, 0, 0, 0), 1.0f);
    ComponentHandle second = utils::instantiateComposeInto(scene, folder.string(), INVALID_COMPONENT_HANDLE,
                                                           projv::core::vec3(5, 0, 0), projv::core::quat(1, 0, 0, 0), 1.0f);
    REQUIRE(first != INVALID_COMPONENT_HANDLE);
    REQUIRE(second != INVALID_COMPONENT_HANDLE);
    CHECK(scene.components.size() < rowsBefore + 2 * 5);   // some rows came from the holes
    CHECK(scene.components[first].children.size() == 2);   // Part and lid
    checkLinks(scene);
    checkRootIndex(scene);
    std::filesystem::remove_all(folder);
}

TEST_CASE("a link to a deleted component is never mistaken for a link to its row's next occupant") {
    projv::Application app;
    projv::Scene& scene = app.world.ctx().emplace<projv::Scene>();
    runtime::installSceneBridge(app);
    app.runStartup();
    projv::World& world = app.world;

    ComponentHandle a = solid(scene, "a");
    projv::Entity holder = runtime::spawnComponent(world, a, projv::LinkMode::Root, projv::OnUnlink::Keep);
    REQUIRE(holder != projv::NullEntity);

    // Deleted from the Scene side, and the row reused before the bridge has run.
    utils::deleteComponent(scene, a);
    ComponentHandle b = solid(scene, "b");
    REQUIRE(b == a);
    CHECK(runtime::entityFor(world, b) == projv::NullEntity);
    projv::Entity fresh = runtime::spawnComponent(world, b, projv::LinkMode::Root, projv::OnUnlink::Keep);
    CHECK(fresh != projv::NullEntity);

    // The bridge drops the stale link, and only that one.
    app.runFrame();
    CHECK_FALSE(world.all_of<projv::VoxelComponent>(holder));
    CHECK(world.all_of<projv::VoxelComponent>(fresh));
    CHECK(runtime::entityFor(world, b) == fresh);
}

TEST_CASE("prefab instances share one geometry blob, an edit forks just that instance, and the cache lets go") {
    std::filesystem::path folder = std::filesystem::temp_directory_path() / "projv_unit_prefab_cache";
    std::filesystem::remove_all(folder);
    {
        projv::Scene source;
        solid(source, "body");
        REQUIRE(utils::saveComposeToDisk(source, INVALID_COMPONENT_HANDLE, folder.string()));
    }
    projv::Application app;
    projv::Scene& scene = app.world.ctx().emplace<projv::Scene>();
    runtime::installSceneBridge(app);
    app.runStartup();
    projv::World& world = app.world;

    auto bodyOf = [&](projv::Entity e) {
        ComponentHandle root = world.get<projv::VoxelComponent>(e).handle;
        REQUIRE(scene.components[root].children.size() == 1);
        return scene.components[root].children[0];
    };
    auto blobOf = [&](ComponentHandle body) { return scene.chunks[scene.components[body].chunkHandle].geometryPoolIndex; };

    projv::Entity a = runtime::instantiatePrefab(world, folder.string(), projv::core::vec3(0));
    projv::Entity b = runtime::instantiatePrefab(world, folder.string(), projv::core::vec3(3, 0, 0));
    REQUIRE(a != projv::NullEntity);
    REQUIRE(b != projv::NullEntity);
    int32_t shared = blobOf(bodyOf(a));
    CHECK(blobOf(bodyOf(b)) == shared);
    CHECK(scene.geometryPool[shared].refCount == 3);   // two instances and the cache's pin

    // Editing one instance gives it its own copy; the other keeps the prefab's.
    utils::queueVoxelAdd(scene, bodyOf(a), {{true, ivec3(7, 7, 7), 0x3FFFFFFFu}});
    utils::updateScene(scene);
    CHECK(blobOf(bodyOf(a)) != shared);
    CHECK(blobOf(bodyOf(b)) == shared);
    CHECK(scene.geometryPool[shared].refCount == 2);

    world.destroy(a);
    world.destroy(b);
    CHECK(scene.geometryPool[shared].refCount == 1);   // only the pin: still there for the next spawn
    projv::Entity c = runtime::instantiatePrefab(world, folder.string(), projv::core::vec3(0));
    CHECK(blobOf(bodyOf(c)) == shared);
    world.destroy(c);

    runtime::clearPrefabCache(world);
    CHECK(scene.geometryPool[shared].refCount == 0);
    std::filesystem::remove_all(folder);
}
