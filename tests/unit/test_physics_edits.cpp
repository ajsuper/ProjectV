// Edits and destruction (the physics plan, A5 and M5): voxel islands, splitting a body cut in
// pieces, shapes rebuilt when voxels change -- and the cases that break games when they are not
// handled: the floor carved out from under a sleeping pile, voxels added inside a resting body, a
// floor deleted under bodies asleep on it.

#include "doctest/doctest.h"

#include <cmath>
#include <set>
#include <tuple>
#include <vector>

#include "core/application.h"
#include "runtime/physics.h"
#include "utils/editing.h"
#include "utils/picking.h"
#include "utils/scene_query.h"
#include "utils/voxel_islands.h"

namespace {
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;
    namespace utils = projv::utils;
    namespace runtime = projv::runtime;
    constexpr double DT = 1.0 / 60.0;

    std::vector<projv::PendingVoxelOp> fill(ivec3 low, ivec3 high, uint32_t colour = 0x3FFFFFFFu) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) ops.push_back({true, ivec3(x, y, z), colour});
        return ops;
    }

    ComponentHandle chunkOf(projv::Scene& scene, const std::vector<projv::PendingVoxelOp>& ops, vec3 at = vec3(0),
                            float voxel = 0.25f, ComponentHandle parent = INVALID_COMPONENT_HANDLE) {
        ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "c", parent, 16, voxel);
        utils::setComponentTransform(scene, h, at, quat(1, 0, 0, 0), 1.0f);
        utils::queueVoxelAdd(scene, h, ops);
        utils::updateScene(scene);
        return h;
    }

    const projv::GeometryBlob& blobOf(const projv::Scene& scene, ComponentHandle h) {
        return scene.geometryPool[size_t(scene.chunks[scene.components[h].chunkHandle].geometryPoolIndex)];
    }

    struct Fixture {
        projv::Application app;
        projv::Scene& scene;
        double now = 0.0;
        std::vector<projv::BodySplit> splits;
        explicit Fixture(bool record = false) : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            runtime::installSceneBridge(app);
            runtime::PhysicsConfig config;
            config.settings.record = record;
            runtime::installPhysics(app, config);
            app.events().on<projv::BodySplit>([this](const projv::BodySplit& e) { splits.push_back(e); });
            app.runStartup();
            app.runFrame();
        }
        void frames(int n) { for (int i = 0; i < n; i++) { now += DT; app.runFrame(); } }
        // A 20 m floor of half-metre voxels, top at y = 0: a grid, solid.
        ComponentHandle floorComponent = INVALID_COMPONENT_HANDLE;
        Entity floor() {
            floorComponent = utils::addComponent(scene, projv::ComponentKind::Chunk, "floor", INVALID_COMPONENT_HANDLE, 16, 0.5f);
            utils::setComponentTransform(scene, floorComponent, vec3(-10, -1, -10), quat(1, 0, 0, 0), 1.0f);
            utils::queueVoxelAdd(scene, floorComponent, fill({0, 0, 0}, {39, 1, 39}));
            utils::updateScene(scene);
            Entity e = runtime::spawnComponent(app.world, floorComponent);
            app.world.emplace<projv::StaticCollider>(e);
            return e;
        }
        Entity body(const std::vector<projv::PendingVoxelOp>& voxels, vec3 at, float mass = 0.0f) {
            Entity e = runtime::spawnComponent(app.world, chunkOf(scene, voxels, at), projv::LinkMode::Root,
                                               projv::OnUnlink::Destroy);
            projv::RigidBody rigid;
            rigid.mass = mass;
            app.world.emplace<projv::RigidBody>(e, rigid);
            return e;
        }
        runtime::BodyState pose(Entity e) { return runtime::bodyPose(app.world, e); }
        ComponentHandle componentOf(Entity e) { return app.world.get<projv::VoxelComponent>(e).handle; }
    };
}

TEST_CASE("islands: face-connected groups, edge contact does not join them, a fixed order") {
    projv::Scene scene;
    std::vector<projv::PendingVoxelOp> ops = fill({0, 0, 0}, {1, 1, 1});    // 8 voxels
    for (auto op : fill({4, 0, 0}, {4, 2, 0})) ops.push_back(op);           // 3 voxels
    for (auto op : fill({2, 2, 0}, {2, 2, 0})) ops.push_back(op);           // touches the first only along an edge
    ComponentHandle h = chunkOf(scene, ops);
    auto islands = utils::voxelIslands(blobOf(scene, h), 16);
    REQUIRE(islands.size() == 3);
    CHECK(islands[0].size() == 8);   // scan order: the block at the origin first
    CHECK(islands[1].size() == 3);   // then (4,0,0)...
    CHECK(islands[2].size() == 1);   // and the edge-touching voxel, alone
    std::set<std::tuple<int, int, int>> all;
    for (const auto& island : islands) for (ivec3 v : island) all.insert({v.x, v.y, v.z});
    CHECK(all.size() == 12);
    CHECK(utils::voxelIslands(projv::GeometryBlob{}, 16).empty());
}

TEST_CASE("a split keeps the largest island, and each piece keeps its voxels' materials and place") {
    projv::Scene scene;
    std::vector<projv::PendingVoxelOp> ops = fill({0, 0, 0}, {3, 3, 3}, 0x3FF00000u);   // red, 64
    for (auto op : fill({8, 0, 0}, {9, 1, 1}, 0x000FFC00u)) ops.push_back(op);           // green, 8
    ComponentHandle h = chunkOf(scene, ops, vec3(5, 6, 7));
    utils::IslandSplit split = utils::splitVoxelIslands(scene, h, true);
    REQUIRE(split.pieces.size() == 1);
    CHECK(split.voxels == std::vector<uint64_t>{64, 8});
    ComponentHandle piece = split.pieces[0];
    CHECK(scene.components[piece].parent == INVALID_COMPONENT_HANDLE);
    CHECK(utils::getComponentWorldPosition(scene, piece) == utils::getComponentWorldPosition(scene, h));

    auto islandsOf = [&](ComponentHandle c) { return utils::voxelIslands(blobOf(scene, c), 16); };
    REQUIRE(islandsOf(h).size() == 1);
    CHECK(islandsOf(h)[0].size() == 64);
    REQUIRE(islandsOf(piece).size() == 1);
    CHECK(islandsOf(piece)[0].size() == 8);
    // The piece's voxels still read as green through its own palette.
    uint8_t slot = 0;
    REQUIRE(utils::queryVoxelMaterial(blobOf(scene, piece), 16, ivec3(8, 0, 0), slot));
    CHECK(scene.components[piece].materialPalette[slot].packedColor == 0x000FFC00u);
    REQUIRE(utils::queryVoxelMaterial(blobOf(scene, h), 16, ivec3(0, 0, 0), slot));
    CHECK(scene.components[h].materialPalette[slot].packedColor == 0x3FF00000u);

    // One island: nothing to do.
    CHECK(utils::splitVoxelIslands(scene, h, true).pieces.empty());
}

TEST_CASE("edit under load: carve the floor from under a sleeping crate and it falls through the hole") {
    Fixture f;
    f.floor();
    Entity crate = f.body(fill({0, 0, 0}, {3, 3, 3}), vec3(-0.5f, 0.0f, -0.5f));   // 1 m, at the centre
    f.frames(3 * 60);
    REQUIRE_FALSE(f.pose(crate).awake);
    // A 3 m hole through the floor under it.
    std::vector<projv::PendingVoxelOp> hole;
    for (auto op : fill({17, 0, 17}, {22, 1, 22})) hole.push_back({false, op.position, 0});
    utils::queueVoxelRemove(f.scene, f.floorComponent, hole);
    utils::updateScene(f.scene);
    f.frames(2 * 60);
    CHECK(f.pose(crate).position.y < -3.0f);
}

TEST_CASE("edit under load: voxels added inside a resting crate push it out, without exploding it") {
    Fixture f;
    f.floor();
    Entity crate = f.body(fill({0, 0, 0}, {3, 3, 3}), vec3(-0.5f, 0.0f, -0.5f));
    f.frames(3 * 60);
    // A half-metre step in the floor, right where the crate sits: the crate overlaps it by half a metre.
    utils::queueVoxelAdd(f.scene, f.floorComponent, fill({19, 2, 19}, {20, 2, 20}));
    utils::updateScene(f.scene);
    float fastest = 0.0f;
    for (int i = 0; i < 4 * 60; i++) {
        f.frames(1);
        fastest = std::max(fastest, glm::length(f.pose(crate).linearVelocity));
    }
    runtime::BodyState end = f.pose(crate);
    CHECK(fastest < 10.0f);                 // pushed, not launched
    CHECK(end.position.y > -0.01f);         // not left inside the floor
    CHECK(end.position.y < 1.0f);           // and still on it
    CHECK_FALSE(end.awake);                 // and it settles
    CHECK(runtime::physicsWorld(f.app.world).stats().nonFiniteRecoveries == 0);
}

TEST_CASE("deleting a floor wakes what sleeps on it") {
    Fixture f;
    Entity floor = f.floor();
    Entity crate = f.body(fill({0, 0, 0}, {3, 3, 3}), vec3(-0.5f, 0.0f, -0.5f));
    f.frames(3 * 60);
    REQUIRE_FALSE(f.pose(crate).awake);
    f.app.world.remove<projv::StaticCollider>(floor);
    f.frames(60);
    CHECK(f.pose(crate).position.y < -1.0f);
}

TEST_CASE("destruction: a crate cut in two becomes two bodies, sharing its mass and its motion") {
    Fixture f;
    runtime::setGravity(f.app.world, vec3(0));
    Entity crate = f.body(fill({0, 0, 0}, {7, 3, 3}), vec3(0, 10, 0), 32.0f);   // 2 x 1 x 1 m, 32 kg
    f.frames(2);
    runtime::setVelocity(f.app.world, crate, vec3(2, 0, 0));
    f.frames(2);
    // A lopsided cut: the slab at x = 5 goes, leaving five slabs on one side and two on the other.
    std::vector<projv::PendingVoxelOp> cut;
    for (auto op : fill({5, 0, 0}, {5, 3, 3})) cut.push_back({false, op.position, 0});
    runtime::editVoxels(f.app.world, crate, cut);
    f.frames(2);
    REQUIRE(f.splits.size() == 1);
    CHECK(f.splits[0].parent == crate);
    REQUIRE(f.splits[0].pieces.size() == 1);
    Entity piece = f.splits[0].pieces[0];
    REQUIRE(runtime::hasBody(f.app.world, piece));
    const runtime::PhysicsWorld& sim = runtime::physicsWorld(f.app.world);
    float bigger = sim.bodyMass(f.app.world.get<projv::PhysicsBody>(crate).id);
    float smaller = sim.bodyMass(f.app.world.get<projv::PhysicsBody>(piece).id);
    CHECK(bigger == doctest::Approx(32.0f * 80.0f / 112.0f).epsilon(0.01));   // 5 of 7 slabs
    CHECK(smaller == doctest::Approx(32.0f * 32.0f / 112.0f).epsilon(0.01));  // 2 of 7
    CHECK(f.pose(crate).linearVelocity.x == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(f.pose(piece).linearVelocity.x == doctest::Approx(2.0f).epsilon(0.02));
    // The piece is where its voxels were, and moves on with them.
    float gap = f.pose(piece).position.x - f.pose(crate).position.x;
    CHECK(std::abs(gap) < 0.01f);   // both components have the same origin; the voxels differ
    float start = f.pose(piece).position.x;
    f.frames(30);
    CHECK(f.pose(piece).position.x - start == doctest::Approx(1.0f).epsilon(0.03));   // 2 m/s for half a second
}

TEST_CASE("destruction works on prefab-shaped bodies: an asset node with one chunk inside") {
    Fixture f;
    f.floor();
    ComponentHandle node = utils::addComponent(f.scene, projv::ComponentKind::Asset, "prefab", INVALID_COMPONENT_HANDLE, 4, 1.0f);
    std::vector<projv::PendingVoxelOp> twoBlocks = fill({0, 0, 0}, {3, 3, 3});
    for (auto op : fill({4, 0, 0}, {7, 3, 3})) twoBlocks.push_back(op);
    chunkOf(f.scene, twoBlocks, vec3(-1, 0, -0.5f), 0.25f, node);
    utils::setComponentTransform(f.scene, node, vec3(0, 0.02f, 0), quat(1, 0, 0, 0), 1.0f);
    Entity e = runtime::spawnComponent(f.app.world, node, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    f.app.world.emplace<projv::RigidBody>(e);
    f.frames(60);
    // Cut the join: a one-voxel slot down the middle.
    std::vector<projv::PendingVoxelOp> cut;
    for (auto op : fill({3, 0, 0}, {4, 3, 3})) cut.push_back({false, op.position, 0});
    ComponentHandle chunk = f.scene.components[node].children[0];
    utils::queueVoxelRemove(f.scene, chunk, cut);
    utils::updateScene(f.scene);
    f.frames(60);
    REQUIRE(f.splits.size() == 1);
    REQUIRE(f.splits[0].pieces.size() == 1);
    Entity piece = f.splits[0].pieces[0];
    CHECK(runtime::hasBody(f.app.world, piece));
    // Both halves rest on the floor, side by side, and neither moved far.
    CHECK(f.pose(e).position.y == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(std::abs(f.pose(piece).position.y - (-0.0f)) < 0.05f);
    // Destroying the parent's entity takes only its own voxels.
    f.app.world.destroy(e);
    f.frames(2);
    CHECK(runtime::hasBody(f.app.world, piece));
}

TEST_CASE("a voxel edit command applies at the next step, not before") {
    Fixture f;
    f.floor();
    Entity crate = f.body(fill({0, 0, 0}, {3, 3, 3}), vec3(-0.5f, 0.0f, -0.5f));
    f.frames(10);
    ComponentHandle h = f.componentOf(crate);
    uint64_t before = blobOf(f.scene, h).contentStamp;
    runtime::editVoxels(f.app.world, crate, {{true, ivec3(5, 0, 0), 0x3FFFFFFFu}});
    CHECK(blobOf(f.scene, h).contentStamp == before);   // queued, not applied
    f.frames(1);
    CHECK(blobOf(f.scene, h).contentStamp != before);
}

TEST_CASE("a session with edits, rebuilds and splits replays exactly") {
    Fixture f(true);
    f.floor();
    std::vector<Entity> crates;
    for (int i = 0; i < 6; i++) {
        std::vector<projv::PendingVoxelOp> bar = fill({0, 0, 0}, {11, 1, 1});
        crates.push_back(f.body(bar, vec3(float(i) * 1.5f - 4.0f, 2.0f + float(i), -1.0f), 10.0f));
    }
    for (int frame = 0; frame < 240; frame++) {
        if (frame == 60 || frame == 120) {
            for (size_t i = 0; i < crates.size(); i++) {
                int x = frame == 60 ? 4 : 8;
                std::vector<projv::PendingVoxelOp> cut;
                for (auto op : fill({x, 0, 0}, {x, 1, 1})) cut.push_back({false, op.position, 0});
                runtime::editVoxels(f.app.world, crates[i], cut);
            }
        }
        if (frame == 90) {
            std::vector<projv::PendingVoxelOp> hole;
            for (auto op : fill({0, 0, 15}, {39, 1, 16})) hole.push_back({false, op.position, 0});
            utils::queueVoxelRemove(f.scene, f.floorComponent, hole);
            utils::updateScene(f.scene);
        }
        f.frames(1);
    }
    CHECK(f.splits.size() >= 6);
    runtime::ReplayResult result = runtime::PhysicsWorld::replay(runtime::physicsWorld(f.app.world).recording());
    CAPTURE(result.problem);
    CHECK(result.matched);
}

TEST_CASE("a body carved down to nothing is reported as emptied, not refused") {
    Fixture f;
    f.floor();
    Entity crate = f.body(fill({0, 0, 0}, {3, 3, 3}), vec3(-0.5f, 0.0f, -0.5f));
    std::vector<projv::BodyEmptied> emptied;
    std::vector<projv::PhysicsBodyRefused> refused;
    f.app.events().on<projv::BodyEmptied>([&](const projv::BodyEmptied& e) { emptied.push_back(e); });
    f.app.events().on<projv::PhysicsBodyRefused>([&](const projv::PhysicsBodyRefused& e) { refused.push_back(e); });
    f.frames(10);
    std::vector<projv::PendingVoxelOp> all;
    for (auto op : fill({0, 0, 0}, {3, 3, 3})) all.push_back({false, op.position, 0});
    runtime::editVoxels(f.app.world, crate, all);
    f.frames(2);
    REQUIRE(emptied.size() == 1);
    CHECK(emptied[0].entity == crate);
    CHECK(refused.empty());
    CHECK_FALSE(runtime::hasBody(f.app.world, crate));
    CHECK(f.app.world.valid(crate));   // the game's to destroy
}

TEST_CASE("a body carved to nothing before its first step is emptied too, not refused") {
    Fixture f;
    f.floor();
    std::vector<projv::BodyEmptied> emptied;
    std::vector<projv::PhysicsBodyRefused> refused;
    f.app.events().on<projv::BodyEmptied>([&](const projv::BodyEmptied& e) { emptied.push_back(e); });
    f.app.events().on<projv::PhysicsBodyRefused>([&](const projv::PhysicsBodyRefused& e) { refused.push_back(e); });
    // Spawned and blasted away in the same frame: its body was never made.
    Entity crate = f.body(fill({0, 0, 0}, {1, 1, 1}), vec3(0, 3, 0));
    std::vector<projv::PendingVoxelOp> all;
    for (auto op : fill({0, 0, 0}, {1, 1, 1})) all.push_back({false, op.position, 0});
    runtime::editVoxels(f.app.world, crate, all);
    f.frames(2);
    REQUIRE(emptied.size() == 1);
    CHECK(emptied[0].entity == crate);
    CHECK(refused.empty());

    // An entity linked to a component that never had any geometry is still a refusal: a mistake.
    ComponentHandle never = utils::addComponent(f.scene, projv::ComponentKind::Chunk, "never", INVALID_COMPONENT_HANDLE, 16, 0.5f);
    Entity mistake = runtime::spawnComponent(f.app.world, never);
    f.app.world.emplace<projv::RigidBody>(mistake);
    f.frames(2);
    CHECK(refused.size() == 1);
    CHECK(emptied.size() == 1);
}

TEST_CASE("a copy of a prefab-shaped prototype, cracked through, splits") {
    // How examples/17-physics-demos builds its bricks: one prototype asset with a chunk inside, and
    // every brick a duplicateComponent of it, sharing its geometry until edited.
    Fixture f;
    f.floor();
    ComponentHandle proto = utils::addComponent(f.scene, projv::ComponentKind::Asset, "brick", INVALID_COMPONENT_HANDLE, 4, 1.0f);
    chunkOf(f.scene, fill({0, 0, 0}, {7, 3, 3}), vec3(-0.5f, -0.25f, -0.25f), 0.125f, proto);
    utils::setComponentTransform(f.scene, proto, vec3(0, -1000, 0), quat(1, 0, 0, 0), 1.0f);
    ComponentHandle copy = utils::duplicateComponent(f.scene, proto);
    utils::setComponentTransform(f.scene, copy, vec3(0, 0.3f, 0), quat(1, 0, 0, 0), 1.0f);
    Entity brick = runtime::spawnComponent(f.app.world, copy, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    f.app.world.emplace<projv::RigidBody>(brick, projv::RigidBody{.mass = 30.0f});
    f.frames(30);
    std::vector<projv::PendingVoxelOp> cracks;
    for (int z = 0; z < 4; z++)
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 8; x++)
                if (x == 2 || x == 5 || y == 2) cracks.push_back({false, ivec3(x, y, z), 0});
    runtime::editVoxels(f.app.world, brick, cracks);
    f.frames(3);
    REQUIRE(f.splits.size() == 1);
    CHECK(f.splits[0].pieces.size() == 5);   // six pieces: the parent keeps one
}
