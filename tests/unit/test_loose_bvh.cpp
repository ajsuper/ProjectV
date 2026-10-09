// The loose-chunk BVH (utils/loose_bvh.h), checked against brute force on random scenes.

#include "doctest/doctest.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <random>
#include <set>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "utils/editing.h"
#include "utils/loose_bvh.h"
#include "utils/scene_query.h"

namespace {
    using projv::ChunkHandle;
    using projv::ComponentHandle;
    using projv::core::ivec3;
    using projv::core::vec3;
    namespace utils = projv::utils;

    ComponentHandle chunkWith(projv::Scene& scene, const std::vector<ivec3>& voxels, uint32_t resolution = 16) {
        ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "c",
                                                projv::INVALID_COMPONENT_HANDLE, resolution, 0.5f);
        std::vector<projv::PendingVoxelOp> ops;
        for (ivec3 v : voxels) ops.push_back({true, v, 0x3FFFFFFFu});
        utils::queueVoxelAdd(scene, h, ops);
        utils::updateScene(scene);
        return h;
    }

    // A scene of `count` small chunks, each a few voxels somewhere in its cube, scattered and turned.
    projv::Scene randomScene(int count, unsigned seed) {
        std::mt19937 random(seed);
        std::uniform_real_distribution<float> place(-60.0f, 60.0f), angle(-3.14f, 3.14f), unit(-1.0f, 1.0f);
        std::uniform_int_distribution<int> cell(0, 15);
        projv::Scene scene;
        for (int i = 0; i < count; i++) {
            std::vector<ivec3> voxels;
            ivec3 seedVoxel(cell(random), cell(random), cell(random));
            for (int k = 0; k < 3; k++) voxels.push_back(glm::clamp(seedVoxel + ivec3(k, 0, k % 2), ivec3(0), ivec3(15)));
            ComponentHandle h = chunkWith(scene, voxels);
            vec3 axis = glm::normalize(vec3(unit(random), unit(random), unit(random)) + vec3(0.001f));
            utils::setComponentTransform(scene, h, vec3(place(random), place(random) * 0.3f, place(random)),
                                         glm::angleAxis(angle(random), axis), 1.0f);
        }
        return scene;
    }

    struct Box { ChunkHandle chunk; vec3 minimum, maximum; };
    std::vector<Box> bruteBoxes(const projv::Scene& scene) {
        std::vector<Box> boxes;
        for (ChunkHandle h : scene.looseChunks) {
            Box b{h, vec3(0), vec3(0)};
            if (utils::looseChunkWorldBounds(scene, h, b.minimum, b.maximum)) boxes.push_back(b);
        }
        return boxes;
    }

    int depthOf(const utils::LooseBVH& bvh, uint32_t node) {
        const utils::LooseBVHNode& n = bvh.nodes[node];
        return n.leaf() ? 1 : 1 + std::max(depthOf(bvh, n.a), depthOf(bvh, n.b));
    }
}

TEST_CASE("content bounds are the voxels' own box, cached, and refreshed when the geometry changes") {
    projv::Scene scene;
    ComponentHandle h = chunkWith(scene, {ivec3(2, 3, 4), ivec3(5, 3, 1)});
    const projv::GeometryBlob& blob = scene.geometryPool[scene.chunks[scene.components[h].chunkHandle].geometryPoolIndex];
    ivec3 low, high;
    REQUIRE(utils::blobContentBounds(blob, low, high));
    CHECK(low == ivec3(2, 3, 1));
    CHECK(high == ivec3(5, 3, 4));
    CHECK(blob.contentBoundsValid);

    // An edit replaces the geometry; the cache must not survive it.
    utils::queueVoxelAdd(scene, h, {{true, ivec3(9, 9, 9), 0x3FFFFFFFu}});
    utils::updateScene(scene);
    const projv::GeometryBlob& edited = scene.geometryPool[scene.chunks[scene.components[h].chunkHandle].geometryPoolIndex];
    REQUIRE(utils::blobContentBounds(edited, low, high));
    CHECK(high == ivec3(9, 9, 9));
}

TEST_CASE("a chunk's world bounds follow its transform, and an empty chunk has none") {
    projv::Scene scene;
    ComponentHandle h = chunkWith(scene, {ivec3(0, 0, 0)});   // one voxel, 0.5 units, at the corner
    utils::setComponentTransform(scene, h, vec3(10, 0, 0), projv::core::quat(1, 0, 0, 0), 1.0f);
    vec3 low, high;
    REQUIRE(utils::looseChunkWorldBounds(scene, scene.components[h].chunkHandle, low, high));
    CHECK(low.x == doctest::Approx(10.0f));
    CHECK(high.x == doctest::Approx(10.5f));
    CHECK(high.y == doctest::Approx(0.5f));

    // Turned a quarter about y: the voxel's x extent becomes a z extent.
    utils::setComponentTransform(scene, h, vec3(10, 0, 0), glm::angleAxis(1.5707963f, vec3(0, 1, 0)), 1.0f);
    REQUIRE(utils::looseChunkWorldBounds(scene, scene.components[h].chunkHandle, low, high));
    CHECK(high.x - low.x == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(high.z - low.z == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(low.z == doctest::Approx(-0.5f).epsilon(1e-3));

    ComponentHandle empty = utils::addComponent(scene, projv::ComponentKind::Chunk, "empty",
                                                projv::INVALID_COMPONENT_HANDLE, 16, 0.5f);
    CHECK_FALSE(utils::looseChunkWorldBounds(scene, scene.components[empty].chunkHandle, low, high));
    utils::LooseBVH bvh = utils::buildLooseBVH(scene, scene.looseChunks);
    CHECK(std::find(bvh.chunks.begin(), bvh.chunks.end(), scene.components[empty].chunkHandle) == bvh.chunks.end());
}

TEST_CASE("every chunk is in exactly one leaf, and the tree fits the shader's stack") {
    projv::Scene scene = randomScene(300, 7);
    utils::LooseBVH bvh = utils::buildLooseBVH(scene, scene.looseChunks);
    REQUIRE_FALSE(bvh.empty());
    std::set<ChunkHandle> unique(bvh.chunks.begin(), bvh.chunks.end());
    CHECK(unique.size() == bvh.chunks.size());
    CHECK(bvh.chunks.size() == bruteBoxes(scene).size());
    CHECK(depthOf(bvh, 0) <= 31);
    // Every node's box contains its children's.
    for (const utils::LooseBVHNode& node : bvh.nodes) {
        if (node.leaf()) continue;
        for (uint32_t child : {node.a, node.b}) {
            CHECK(glm::all(glm::lessThanEqual(node.minimum, bvh.nodes[child].minimum)));
            CHECK(glm::all(glm::greaterThanEqual(node.maximum, bvh.nodes[child].maximum)));
        }
    }
}

TEST_CASE("ray traversal finds the same nearest box as brute force, and never misses one it should visit") {
    projv::Scene scene = randomScene(250, 11);
    utils::LooseBVH bvh = utils::buildLooseBVH(scene, scene.looseChunks);
    std::vector<Box> boxes = bruteBoxes(scene);
    std::mt19937 random(3);
    std::uniform_real_distribution<float> coordinate(-80.0f, 80.0f);
    int checked = 0;
    for (int r = 0; r < 2000; r++) {
        vec3 origin(coordinate(random), coordinate(random) * 0.5f, coordinate(random));
        vec3 target(coordinate(random), coordinate(random) * 0.2f, coordinate(random));
        vec3 direction = glm::normalize(target - origin);
        vec3 inverse = vec3(1.0f) / direction;

        float bruteNearest = std::numeric_limits<float>::max();
        std::set<ChunkHandle> bruteHit;
        for (const Box& b : boxes) {
            float entry, exit;
            if (utils::rayBoxInterval(origin, inverse, b.minimum, b.maximum, entry, exit)) {
                bruteHit.insert(b.chunk);
                bruteNearest = std::min(bruteNearest, entry);
            }
        }

        // With no pruning, the traversal visits every chunk the ray's line enters.
        std::set<ChunkHandle> visited;
        utils::traverseLooseBVH(bvh, origin, direction, std::numeric_limits<float>::max(),
                                [&](ChunkHandle c, float, float maxD) { visited.insert(c); return maxD; });
        for (ChunkHandle c : bruteHit) CHECK(visited.count(c) == 1);

        // Pruned by the nearest entry so far, it still finds the nearest box.
        float nearest = std::numeric_limits<float>::max();
        utils::traverseLooseBVH(bvh, origin, direction, std::numeric_limits<float>::max(),
                                [&](ChunkHandle c, float, float maxD) {
                                    for (const Box& b : boxes) {
                                        if (b.chunk != c) continue;
                                        float entry, exit;
                                        if (utils::rayBoxInterval(origin, inverse, b.minimum, b.maximum, entry, exit)) {
                                            nearest = std::min(nearest, entry);
                                        }
                                    }
                                    return std::min(maxD, nearest);
                                });
        CHECK(nearest == doctest::Approx(bruteNearest));
        if (!bruteHit.empty()) checked++;
    }
    CHECK(checked > 100);   // enough rays actually hit something for the comparison to mean anything
}

TEST_CASE("overlap queries match brute force exactly") {
    projv::Scene scene = randomScene(200, 5);
    utils::LooseBVH bvh = utils::buildLooseBVH(scene, scene.looseChunks);
    std::vector<Box> boxes = bruteBoxes(scene);
    std::mt19937 random(9);
    std::uniform_real_distribution<float> coordinate(-70.0f, 70.0f), size(0.5f, 25.0f);
    for (int q = 0; q < 500; q++) {
        vec3 low(coordinate(random), coordinate(random) * 0.3f, coordinate(random));
        vec3 high = low + vec3(size(random), size(random), size(random));
        std::set<ChunkHandle> brute, found;
        for (const Box& b : boxes) {
            bool apart = b.maximum.x < low.x || b.minimum.x > high.x || b.maximum.y < low.y ||
                         b.minimum.y > high.y || b.maximum.z < low.z || b.minimum.z > high.z;
            if (!apart) brute.insert(b.chunk);
        }
        utils::overlapLooseBVH(bvh, low, high, [&](ChunkHandle c) { found.insert(c); });
        CHECK(found == brute);
    }
}

TEST_CASE("an empty set of loose chunks builds an empty tree") {
    projv::Scene scene;
    CHECK(utils::buildLooseBVH(scene, scene.looseChunks).empty());
    int visits = 0;
    utils::traverseLooseBVH(utils::LooseBVH{}, vec3(0), vec3(1, 0, 0), 100.0f,
                            [&](ChunkHandle, float, float d) { visits++; return d; });
    CHECK(visits == 0);
}
