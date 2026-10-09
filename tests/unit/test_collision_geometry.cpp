// What voxels collide as (utils/collision_geometry.h), checked exactly against the voxels.

#include "doctest/doctest.h"

#include <random>
#include <set>
#include <tuple>
#include <vector>

#include "utils/collision_geometry.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

namespace {
    using projv::ComponentHandle;
    using projv::core::ivec3;
    namespace utils = projv::utils;

    struct Less {
        bool operator()(ivec3 a, ivec3 b) const { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); }
    };
    using VoxelSet = std::set<ivec3, Less>;

    ComponentHandle chunkWith(projv::Scene& scene, const VoxelSet& voxels, uint32_t resolution) {
        ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "c",
                                                projv::INVALID_COMPONENT_HANDLE, resolution, 1.0f);
        std::vector<projv::PendingVoxelOp> ops;
        for (ivec3 v : voxels) ops.push_back({true, v, 0x3FFFFFFFu});
        utils::queueVoxelAdd(scene, h, ops);
        utils::updateScene(scene);
        return h;
    }

    const projv::GeometryBlob& blobOf(const projv::Scene& scene, ComponentHandle h) {
        return scene.geometryPool[scene.chunks[scene.components[h].chunkHandle].geometryPoolIndex];
    }

    VoxelSet randomVoxels(std::mt19937& random, int resolution, float density) {
        // Clumps rather than noise: what real geometry looks like, and what makes merging matter.
        std::uniform_int_distribution<int> cell(0, resolution - 1), size(1, resolution / 3);
        std::uniform_real_distribution<float> chance(0.0f, 1.0f);
        VoxelSet voxels;
        for (int blob = 0; blob < 6; blob++) {
            ivec3 low(cell(random), cell(random), cell(random));
            ivec3 high = glm::min(low + ivec3(size(random), size(random), size(random)), ivec3(resolution - 1));
            for (int z = low.z; z <= high.z; z++)
                for (int y = low.y; y <= high.y; y++)
                    for (int x = low.x; x <= high.x; x++)
                        if (chance(random) < density) voxels.insert({x, y, z});
        }
        return voxels;
    }

    // Every voxel the pieces cover, and whether any voxel is covered twice.
    VoxelSet covered(const utils::CollisionPieces& pieces, bool& overlap) {
        VoxelSet out;
        overlap = false;
        for (const utils::CollisionPiece& p : pieces.pieces)
            for (int z = p.voxelMin.z; z <= p.voxelMax.z; z++)
                for (int y = p.voxelMin.y; y <= p.voxelMax.y; y++)
                    for (int x = p.voxelMin.x; x <= p.voxelMax.x; x++)
                        if (!out.insert({x, y, z}).second) overlap = true;
        return out;
    }
}

TEST_CASE("boxes cover the voxels exactly, without overlapping") {
    std::mt19937 random(7);
    for (int trial = 0; trial < 12; trial++) {
        int resolution = trial % 2 ? 64 : 16;
        VoxelSet voxels = randomVoxels(random, resolution, trial % 3 == 0 ? 1.0f : 0.7f);
        projv::Scene scene;
        ComponentHandle h = chunkWith(scene, voxels, uint32_t(resolution));
        utils::CollisionParams params;
        params.pieceBudget = 1u << 20;   // this test is about exactness, not the budget
        utils::CollisionPieces pieces = utils::buildCollisionPieces(blobOf(scene, h), uint32_t(resolution), params);

        CAPTURE(trial);
        REQUIRE(pieces.fallback == utils::CollisionFallback::None);
        bool overlap = false;
        CHECK(covered(pieces, overlap) == voxels);
        CHECK_FALSE(overlap);
        CHECK(pieces.solidVoxels == voxels.size());
        for (const utils::CollisionPiece& p : pieces.pieces) {
            CHECK(p.min == projv::core::vec3(p.voxelMin));
            CHECK(p.max == projv::core::vec3(p.voxelMax + ivec3(1)));
        }
        // Merging is doing its job: a solid clump is far fewer boxes than voxels.
        if (trial % 3 == 0) CHECK(pieces.pieces.size() * 8 < voxels.size());
    }
}

TEST_CASE("the same voxels give the same pieces in the same order") {
    std::mt19937 random(11);
    VoxelSet voxels = randomVoxels(random, 64, 0.8f);
    projv::Scene a, b;
    ComponentHandle ha = chunkWith(a, voxels, 64), hb = chunkWith(b, voxels, 64);
    utils::CollisionParams params;
    params.pieceBudget = 1u << 20;
    utils::CollisionPieces first = utils::buildCollisionPieces(blobOf(a, ha), 64, params);
    utils::CollisionPieces again = utils::buildCollisionPieces(blobOf(a, ha), 64, params);
    utils::CollisionPieces other = utils::buildCollisionPieces(blobOf(b, hb), 64, params);
    projv::GeometryBlob copy = blobOf(a, ha);
    utils::CollisionPieces copied = utils::buildCollisionPieces(copy, 64, params);
    for (const utils::CollisionPieces* p : {&again, &other, &copied}) {
        REQUIRE(p->pieces.size() == first.pieces.size());
        for (size_t i = 0; i < first.pieces.size(); i++) {
            CHECK(p->pieces[i].voxelMin == first.pieces[i].voxelMin);
            CHECK(p->pieces[i].voxelMax == first.pieces[i].voxelMax);
        }
    }
}

TEST_CASE("past the piece budget a blob collides as its content box, and says so") {
    // A checkerboard: no two voxels can merge, so it needs one box per voxel.
    VoxelSet checker;
    for (int z = 0; z < 8; z++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                if ((x + y + z) % 2 == 0) checker.insert({x + 2, y + 3, z + 4});
    projv::Scene scene;
    ComponentHandle h = chunkWith(scene, checker, 16);
    utils::CollisionParams params;
    params.pieceBudget = 100;
    utils::CollisionPieces pieces = utils::buildCollisionPieces(blobOf(scene, h), 16, params);
    CHECK(pieces.fallback == utils::CollisionFallback::OverBudget);
    REQUIRE(pieces.pieces.size() == 1);
    CHECK(pieces.pieces[0].voxelMin == ivec3(2, 3, 4));
    CHECK(pieces.pieces[0].voxelMax == ivec3(9, 10, 11));
    CHECK(pieces.solidVoxels == checker.size());

    params.pieceBudget = 256;   // exactly enough
    pieces = utils::buildCollisionPieces(blobOf(scene, h), 16, params);
    CHECK(pieces.fallback == utils::CollisionFallback::None);
    CHECK(pieces.pieces.size() == checker.size());

    params.maxDecodedCells = 100;   // smaller than the 8x8x8 box
    pieces = utils::buildCollisionPieces(blobOf(scene, h), 16, params);
    CHECK(pieces.fallback == utils::CollisionFallback::TooLarge);
    CHECK(pieces.pieces.size() == 1);
}

TEST_CASE("an empty blob has no pieces") {
    projv::GeometryBlob empty;
    utils::CollisionPieces pieces = utils::buildCollisionPieces(empty, 16);
    CHECK(pieces.pieces.empty());
    CHECK(pieces.fallback == utils::CollisionFallback::None);
}

TEST_CASE("a blob's content stamp changes with its voxels and nothing else") {
    projv::Scene scene;
    ComponentHandle h = chunkWith(scene, {{1, 1, 1}, {2, 1, 1}}, 16);
    uint64_t before = blobOf(scene, h).contentStamp;

    projv::GeometryBlob copy = blobOf(scene, h);
    CHECK(copy.contentStamp == before);          // same voxels, same stamp
    projv::GeometryBlob fresh;
    CHECK(fresh.contentStamp != before);         // a new blob is new content

    utils::queueVoxelAdd(scene, h, {{true, ivec3(5, 5, 5), 0x3FFFFFFFu}});
    utils::updateScene(scene);
    CHECK(blobOf(scene, h).contentStamp != before);
    CHECK(blobOf(scene, h).contentStamp != fresh.contentStamp);
}

TEST_CASE("collision parameters that differ have different keys") {
    utils::CollisionParams a, b;
    CHECK(a.key() == b.key());
    b.pieceBudget = 2048;
    CHECK(a.key() != b.key());
}
