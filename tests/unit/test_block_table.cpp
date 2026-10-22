// Block tables (utils/block_table.h): the dense top of a tree64 the GPU march walks instead of the tree's
// upper levels. Every test compares the table's lookup -- region, block, leaf, the shader's three
// reads -- against the voxels the tree was built from.

#include "doctest/doctest.h"

#include <random>
#include <vector>

#include "data_structures/scene.h"
#include "utils/block_table.h"
#include "utils/material.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

using projv::core::ivec3;

namespace {
    struct BuiltTree {
        std::vector<uint32_t> geometry;
        std::vector<uint8_t> materialIDs;
        std::unique_ptr<projv::VoxelBrickMap> map;
    };

    // A tree the way the engine builds one: a brick map, compressed by updateChunkFromBrickMap.
    template <typename Solid>
    BuiltTree buildTree(int resolution, Solid solid) {
        BuiltTree t;
        t.map = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(uint32_t(resolution)));
        for (int z = 0; z < resolution; ++z)
            for (int y = 0; y < resolution; ++y)
                for (int x = 0; x < resolution; ++x)
                    if (solid(x, y, z)) projv::utils::brickMapSetVoxel(*t.map, x, y, z, uint8_t((x + y + z) % 3));
        projv::Chunk chunk;
        chunk.header.resolution = uint32_t(resolution);
        projv::utils::updateChunkFromBrickMap(chunk, *t.map);
        t.geometry = chunk.geometryData;
        projv::utils::bakeMaterialsFromBrickMap(t.geometry, t.materialIDs, *t.map);
        return t;
    }

    // Every voxel of a resolution-R tree, table lookup against `expected`.
    template <typename Expected>
    int mismatches(const std::vector<uint32_t>& table, const std::vector<uint32_t>& geometry, int resolution, Expected expected) {
        int wrong = 0;
        for (int z = 0; z < resolution; ++z)
            for (int y = 0; y < resolution; ++y)
                for (int x = 0; x < resolution; ++x)
                    if (projv::utils::blockTableVoxel(table, geometry, uint32_t(resolution), {x, y, z}) != expected(x, y, z)) ++wrong;
        return wrong;
    }
}

TEST_CASE("block table: its size is the region and block arrays") {
    CHECK(projv::utils::blockTableTexels(4) == 2);
    CHECK(projv::utils::blockTableTexels(16) == 2);
    CHECK(projv::utils::blockTableTexels(64) == 1 + 64);
    CHECK(projv::utils::blockTableTexels(256) == 64 + 4096);
}

TEST_CASE("block table: finds exactly the tree's voxels, at every resolution") {
    for (int resolution : {4, 16, 64}) {
        CAPTURE(resolution);
        std::mt19937 random(uint32_t(resolution) * 7919u);
        std::vector<uint8_t> solid(size_t(resolution) * resolution * resolution);
        for (auto& s : solid) s = (random() % 7) == 0;
        auto at = [&](int x, int y, int z) { return solid[size_t(x) + size_t(resolution) * (size_t(y) + size_t(resolution) * z)] != 0; };
        BuiltTree tree = buildTree(resolution, at);
        uint32_t stray = 0;
        auto table = projv::utils::buildBlockTable(tree.geometry, uint32_t(resolution), &stray);
        CHECK(stray == 0);
        REQUIRE(table.size() == size_t(projv::utils::blockTableTexels(uint32_t(resolution))) * 4);
        CHECK(mismatches(table, tree.geometry, resolution, at) == 0);
    }
}

TEST_CASE("block table: a sparse 256^3 tree, with whole empty regions") {
    const int resolution = 256;
    auto at = [](int x, int y, int z) {
        // two blobs and a floor, leaving most 64^3 regions and 16^3 blocks empty
        auto ball = [](int x, int y, int z, int cx, int cy, int cz, int r) {
            int dx = x - cx, dy = y - cy, dz = z - cz;
            return dx * dx + dy * dy + dz * dz < r * r;
        };
        return y < 3 || ball(x, y, z, 70, 40, 90, 20) || ball(x, y, z, 200, 150, 30, 9) || (x == 255 && y == 255 && z == 255);
    };
    BuiltTree tree = buildTree(resolution, at);
    uint32_t stray = 0;
    auto table = projv::utils::buildBlockTable(tree.geometry, resolution, &stray);
    CHECK(stray == 0);
    CHECK(mismatches(table, tree.geometry, resolution, at) == 0);
}

TEST_CASE("block table: empty and full trees") {
    for (int resolution : {4, 16, 64}) {
        CAPTURE(resolution);
        BuiltTree full = buildTree(resolution, [](int, int, int) { return true; });
        auto table = projv::utils::buildBlockTable(full.geometry, uint32_t(resolution));
        CHECK(mismatches(table, full.geometry, resolution, [](int, int, int) { return true; }) == 0);

        BuiltTree empty = buildTree(resolution, [](int, int, int) { return false; });
        auto emptyTable = projv::utils::buildBlockTable(empty.geometry, uint32_t(resolution));
        CHECK(mismatches(emptyTable, empty.geometry, resolution, [](int, int, int) { return false; }) == 0);
    }
}

TEST_CASE("block table: the hit names the leaf and the voxel's Z-order in it") {
    const int resolution = 64;
    auto at = [](int x, int y, int z) { return (x * 3 + y * 5 + z * 7) % 11 == 0; };
    BuiltTree tree = buildTree(resolution, at);
    auto table = projv::utils::buildBlockTable(tree.geometry, resolution);
    int checked = 0;
    for (int z = 0; z < resolution; z += 3)
        for (int y = 0; y < resolution; y += 3)
            for (int x = 0; x < resolution; ++x) {
                if (!at(x, y, z)) continue;
                uint32_t leaf = 0, inLeaf = 0;
                REQUIRE(projv::utils::blockTableVoxel(table, tree.geometry, resolution, {x, y, z}, &leaf, &inLeaf));
                // The named leaf is a leaf, and its bit for the voxel is the voxel's bit.
                CHECK(projv::tree64IsLeaf(tree.geometry[leaf * 3 + 2]));
                CHECK(inLeaf == (projv::utils::computeLocalZOrder({x % 4, y % 4, z % 4}) & 63u));
                ++checked;
            }
    CHECK(checked > 100);
}

TEST_CASE("block table: storage LOD -- built from the downsampled tree, at the reduced resolution") {
    const int resolution = 64;
    auto at = [](int x, int y, int z) { return (x / 5 + y / 7 + z / 3) % 4 == 0 && y < 40; };
    BuiltTree tree = buildTree(resolution, at);
    for (uint32_t steps : {1u, 2u}) {
        CAPTURE(steps);
        std::vector<uint32_t> coarse;
        std::vector<uint8_t> coarseMaterials;
        uint32_t dropped = projv::utils::downsampleTree64(tree.geometry, tree.materialIDs, steps, coarse, coarseMaterials);
        const int coarseResolution = int(projv::resolutionAtLOD(resolution, dropped));
        uint32_t stray = 0;
        auto table = projv::utils::buildBlockTable(coarse, uint32_t(coarseResolution), &stray);
        CHECK(stray == 0);
        // A coarse voxel is set when any voxel of the block it covers was.
        const int cell = resolution / coarseResolution;
        auto expected = [&](int x, int y, int z) {
            for (int k = 0; k < cell; ++k)
                for (int j = 0; j < cell; ++j)
                    for (int i = 0; i < cell; ++i)
                        if (at(x * cell + i, y * cell + j, z * cell + k)) return true;
            return false;
        };
        CHECK(mismatches(table, coarse, coarseResolution, expected) == 0);
    }
}

TEST_CASE("block table: a tree's resolution is read off its depth") {
    for (int resolution : {4, 16, 64, 256}) {
        CAPTURE(resolution);
        BuiltTree tree = buildTree(resolution, [&](int x, int y, int z) { return x == resolution - 1 && y == 0 && z == resolution / 2; });
        CHECK(projv::utils::tree64Resolution(tree.geometry) == uint32_t(resolution));
    }
    BuiltTree empty = buildTree(16, [](int, int, int) { return false; });
    CHECK(projv::utils::tree64Resolution(empty.geometry) == 0u);
    CHECK(projv::utils::tree64Resolution({}) == 0u);

    // A downsampled tree reports the reduced resolution the header advertises for it.
    BuiltTree tree = buildTree(64, [](int x, int y, int z) { return (x + y + z) % 5 == 0; });
    std::vector<uint32_t> coarse;
    std::vector<uint8_t> coarseMaterials;
    uint32_t dropped = projv::utils::downsampleTree64(tree.geometry, tree.materialIDs, 1, coarse, coarseMaterials);
    CHECK(projv::utils::tree64Resolution(coarse) == projv::resolutionAtLOD(64, dropped));
}
