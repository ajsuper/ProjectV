// Tile tables (utils/tile_table.h): what the GPU marches in place of the tree64. Every test looks up
// each voxel through the table -- region, block, then a tile or a leaf, the shader's reads -- and
// compares its occupancy and palette slot with the tree the table was built from.

#include "doctest/doctest.h"

#include <filesystem>
#include <random>
#include <vector>

#include "data_structures/compose.h"
#include "data_structures/scene.h"
#include "utils/blob_voxels.h"
#include "utils/compose_io.h"
#include "utils/block_table.h"
#include "utils/material.h"
#include "utils/tile_table.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

using projv::core::ivec3;

namespace {
    struct BuiltTree {
        std::vector<uint32_t> geometry;
        std::vector<uint8_t> materialIDs;
    };

    // A tree the way the engine builds one: a brick map, compressed by updateChunkFromBrickMap. `slot`
    // gives each solid voxel its palette slot.
    template <typename Solid, typename Slot>
    BuiltTree buildTree(int resolution, Solid solid, Slot slot) {
        BuiltTree t;
        auto map = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(uint32_t(resolution)));
        for (int z = 0; z < resolution; ++z)
            for (int y = 0; y < resolution; ++y)
                for (int x = 0; x < resolution; ++x)
                    if (solid(x, y, z)) projv::utils::brickMapSetVoxel(*map, x, y, z, uint8_t(slot(x, y, z)));
        projv::Chunk chunk;
        chunk.header.resolution = uint32_t(resolution);
        projv::utils::updateChunkFromBrickMap(chunk, *map);
        t.geometry = chunk.geometryData;
        projv::utils::bakeMaterialsFromBrickMap(t.geometry, t.materialIDs, *map);
        return t;
    }
    template <typename Solid>
    BuiltTree buildTree(int resolution, Solid solid) {
        return buildTree(resolution, solid, [](int x, int y, int z) { return (x + y + z) % 3; });
    }

    // The tree's own answer for voxel v, through its block table (itself checked against the voxels in
    // test_block_table.cpp): -1 empty, else its palette slot.
    int treeSlot(const BuiltTree& tree, const std::vector<uint32_t>& blockTable, int resolution, ivec3 v) {
        uint32_t leaf = 0, z = 0;
        if (!projv::utils::blockTableVoxel(blockTable, tree.geometry, uint32_t(resolution), v, &leaf, &z)) return -1;
        const uint32_t high = tree.geometry[leaf * 3], low = tree.geometry[leaf * 3 + 1], data3 = tree.geometry[leaf * 3 + 2];
        uint32_t offset = projv::tree64LeafMatOffset(data3);
        if (!projv::tree64LeafIsUniform(data3)) {
            const uint64_t mask = (uint64_t(high) << 32) | low;
            offset += z == 0 ? 0u : uint32_t(__builtin_popcountll(mask >> (64u - z)));
        }
        return offset < tree.materialIDs.size() ? tree.materialIDs[offset] : -2;
    }

    // Every voxel: the table's occupancy and slot against the tree's.
    int mismatches(const BuiltTree& tree, int resolution, projv::utils::TileTable* out = nullptr) {
        uint32_t stray = 0;
        projv::utils::TileTable table = projv::utils::buildTileTable(tree.geometry, tree.materialIDs, uint32_t(resolution), &stray);
        CHECK(stray == 0);
        const std::vector<uint32_t> blockTable = projv::utils::buildBlockTable(tree.geometry, uint32_t(resolution));
        int wrong = 0;
        for (int z = 0; z < resolution; ++z)
            for (int y = 0; y < resolution; ++y)
                for (int x = 0; x < resolution; ++x) {
                    uint8_t slot = 0;
                    const int got = projv::utils::tileTableVoxel(table, {x, y, z}, &slot) ? int(slot) : -1;
                    if (got != treeSlot(tree, blockTable, resolution, {x, y, z})) ++wrong;
                }
        if (out) *out = std::move(table);
        return wrong;
    }
}

TEST_CASE("tile table: its index covers only the box its content spans") {
    using namespace projv::utils;
    // One voxel deep in a 1024^3 chunk: one region and one block, not 4096 and 262,144.
    TileTable t;
    t.resolution = 1024;
    applyTileEdits(t, {{{900, 10, 500}, true, 3}});
    TileLayout L = tileTableLayout(t);
    CHECK(L.regionOrigin == projv::core::ivec3(14, 0, 7));
    CHECK(L.regionDims == projv::core::ivec3(1));
    CHECK(L.blockOrigin == projv::core::ivec3(56, 0, 31));
    CHECK(L.blockDims == projv::core::ivec3(1));
    CHECK(t.leafStart == 1 + 1 + 1 + 1 + 1);   // header, region masks, region starts, block masks, block starts
    CHECK(tileTableWellFormed(t));
    uint8_t slot = 0;
    CHECK(tileTableVoxel(t, {900, 10, 500}, &slot));
    CHECK(slot == 3);
    CHECK_FALSE(tileTableVoxel(t, {0, 0, 0}));
    CHECK_FALSE(tileTableVoxel(t, {1023, 1023, 1023}));

    // A second voxel far off grows both boxes to span the two; taking it away shrinks them back.
    applyTileEdits(t, {{{20, 1000, 30}, true, 5}});
    L = tileTableLayout(t);
    CHECK(L.regionOrigin == projv::core::ivec3(0, 0, 0));
    CHECK(L.regionDims == projv::core::ivec3(15, 16, 8));
    CHECK(L.blockOrigin == projv::core::ivec3(1, 0, 1));
    CHECK(L.blockDims == projv::core::ivec3(56, 63, 31));
    CHECK(tileTableVoxel(t, {20, 1000, 30}, &slot));
    CHECK(slot == 5);
    CHECK(tileTableVoxel(t, {900, 10, 500}, &slot));
    CHECK(slot == 3);
    CHECK(tileTableVoxelCount(t) == 2);
    applyTileEdits(t, {{{20, 1000, 30}, false, 0}});
    L = tileTableLayout(t);
    CHECK(L.blockDims == projv::core::ivec3(1));
    CHECK(t.leafStart == 5);

    // A full 64^3 chunk: one region, 64 blocks, all tiles.
    TileTable full;
    full.resolution = 64;
    std::vector<TileEdit> fill;
    for (int z = 0; z < 64; ++z)
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) fill.push_back({{x, y, z}, true, 1});
    applyTileEdits(full, fill);
    CHECK(full.leafStart == 1 + 1 + 1 + 64 + 32);
    CHECK(full.fullBlocks == 64);
}

TEST_CASE("tile table: a box that leaves the chunk, or a block box outside its region box, is malformed") {
    using namespace projv::utils;
    TileTable t;
    t.resolution = 256;
    applyTileEdits(t, {{{100, 100, 100}, true, 0}, {{101, 100, 100}, true, 1}});
    REQUIRE(tileTableWellFormed(t));
    TileTable beyond = t;
    beyond.texels[0] = 4u;   // region box origin x = 4, past the chunk's four regions
    CHECK_FALSE(tileTableWellFormed(beyond));
    TileTable apart = t;
    apart.texels[2] = 0u;    // block box at the origin, outside region (1, 1, 1)
    CHECK_FALSE(tileTableWellFormed(apart));
    TileTable huge = t;
    huge.texels[3] = 0x3FFFFFFFu;   // a block box 1024 a side
    CHECK_FALSE(tileTableWellFormed(huge));
}

TEST_CASE("tile table: finds exactly the tree's voxels and materials, at every resolution") {
    for (int resolution : {4, 16, 64}) {
        CAPTURE(resolution);
        std::mt19937 random(uint32_t(resolution) * 7919u);
        std::vector<uint8_t> solid(size_t(resolution) * resolution * resolution);
        for (auto& s : solid) s = (random() % 7) == 0;
        auto at = [&](int x, int y, int z) { return solid[size_t(x) + size_t(resolution) * (size_t(y) + size_t(resolution) * z)] != 0; };
        CHECK(mismatches(buildTree(resolution, at), resolution) == 0);
    }
}

TEST_CASE("tile table: a sparse 256^3 tree, with whole empty regions") {
    auto at = [](int x, int y, int z) {
        auto ball = [](int x, int y, int z, int cx, int cy, int cz, int r) {
            int dx = x - cx, dy = y - cy, dz = z - cz;
            return dx * dx + dy * dy + dz * dz < r * r;
        };
        return y < 3 || ball(x, y, z, 70, 40, 90, 20) || ball(x, y, z, 200, 150, 30, 9) || (x == 255 && y == 255 && z == 255);
    };
    CHECK(mismatches(buildTree(256, at), 256) == 0);
}

TEST_CASE("tile table: empty and full trees") {
    for (int resolution : {4, 16, 64}) {
        CAPTURE(resolution);
        projv::utils::TileTable full;
        CHECK(mismatches(buildTree(resolution, [](int, int, int) { return true; }, [](int, int, int) { return 2; }), resolution, &full) == 0);
        CHECK(full.partialBricks == 0);   // one material, wholly solid: tiles all the way down
        CHECK(mismatches(buildTree(resolution, [](int, int, int) { return false; }), resolution) == 0);
    }
}

TEST_CASE("tile table: a solid interior is tiles, its surface is leaves") {
    // A solid ball of one material, its outermost voxels another -- the case the format is for.
    const int resolution = 256;
    auto inside = [](int x, int y, int z) {
        const int dx = x - 128, dy = y - 120, dz = z - 132;
        return dx * dx + dy * dy + dz * dz < 100 * 100;
    };
    auto slot = [&](int x, int y, int z) {
        return (inside(x + 1, y, z) && inside(x - 1, y, z) && inside(x, y + 1, z) && inside(x, y - 1, z)) ? 1 : 2;
    };
    BuiltTree tree = buildTree(resolution, inside, slot);
    projv::utils::TileTable table;
    CHECK(mismatches(tree, resolution, &table) == 0);
    CHECK(table.fullBlocks > 500);    // ~690 of the ball's 16^3 blocks lie wholly inside its surface layer
    CHECK(table.fullBricks > 1000);
    // Far less than a leaf per brick, which is what the tree pays.
    size_t treeLeaves = 0;
    for (size_t i = 2; i < tree.geometry.size(); i += 3) treeLeaves += projv::tree64IsLeaf(tree.geometry[i]) ? 1 : 0;
    CHECK(table.partialBricks * 4 < treeLeaves);
}

TEST_CASE("tile table: bricks that are full but of mixed materials stay leaves") {
    const int resolution = 64;
    BuiltTree tree = buildTree(resolution, [](int, int y, int) { return y < 40; }, [](int x, int, int) { return x % 2; });
    projv::utils::TileTable table;
    CHECK(mismatches(tree, resolution, &table) == 0);
    CHECK(table.fullBricks == 0);
    CHECK(table.fullBlocks == 0);
}

TEST_CASE("tile table: storage LOD -- built from the downsampled tree, at the reduced resolution") {
    const int resolution = 64;
    BuiltTree tree = buildTree(resolution, [](int x, int y, int z) { return (x / 5 + y / 7 + z / 3) % 4 == 0 && y < 40; });
    for (uint32_t steps : {1u, 2u}) {
        CAPTURE(steps);
        BuiltTree coarse;
        uint32_t dropped = projv::utils::downsampleTree64(tree.geometry, tree.materialIDs, steps, coarse.geometry, coarse.materialIDs);
        const int coarseResolution = int(projv::resolutionAtLOD(resolution, dropped));
        CHECK(mismatches(coarse, coarseResolution) == 0);
    }
}

// ---- Edits in place (applyTileEdits), and walking a table ------------------------------------------

namespace {
    // A table and the voxels it should hold (-1 empty, else the slot), kept side by side.
    struct Reference {
        int resolution;
        std::vector<int> cells;
        explicit Reference(int r) : resolution(r), cells(size_t(r) * r * r, -1) {}
        int& at(ivec3 v) { return cells[size_t(v.x) + size_t(resolution) * (size_t(v.y) + size_t(resolution) * v.z)]; }
    };

    int differences(const projv::utils::TileTable& table, Reference& ref) {
        int wrong = 0;
        for (int z = 0; z < ref.resolution; ++z)
            for (int y = 0; y < ref.resolution; ++y)
                for (int x = 0; x < ref.resolution; ++x) {
                    uint8_t slot = 0;
                    const int got = projv::utils::tileTableVoxel(table, {x, y, z}, &slot) ? int(slot) : -1;
                    if (got != ref.at({x, y, z})) ++wrong;
                }
        return wrong;
    }

    void edit(projv::utils::TileTable& table, Reference& ref, const std::vector<projv::utils::TileEdit>& edits) {
        projv::utils::applyTileEdits(table, edits);
        for (const auto& e : edits) ref.at(e.voxel) = e.add ? int(e.slot) : -1;
    }
}

TEST_CASE("tile edits: random adds and removes match a voxel grid, at every resolution") {
    for (int resolution : {4, 16, 64, 256}) {
        CAPTURE(resolution);
        std::mt19937 random(uint32_t(resolution) * 31u + 7u);
        projv::utils::TileTable table;
        table.resolution = uint32_t(resolution);
        Reference ref(resolution);
        for (int round = 0; round < 6; ++round) {
            std::vector<projv::utils::TileEdit> edits;
            for (int i = 0; i < 400; ++i) {
                projv::utils::TileEdit e;
                // Clustered, so some bricks and blocks fill and empty, not just scattered voxels.
                const int c = resolution > 16 ? int(random() % uint32_t(resolution - 8)) : 0;
                e.voxel = {std::min(c + int(random() % 8u), resolution - 1), std::min(int(random() % 8u), resolution - 1),
                           std::min(c / 2 + int(random() % 8u), resolution - 1)};
                e.add = (random() % 4) != 0;
                e.slot = uint8_t(random() % 3);
                edits.push_back(e);
            }
            edit(table, ref, edits);
            CHECK(differences(table, ref) == 0);
        }
    }
}

TEST_CASE("tile edits: a block filled with one material becomes a tile; cut into, it becomes leaves") {
    projv::utils::TileTable table;
    table.resolution = 64;
    Reference ref(64);
    std::vector<projv::utils::TileEdit> fill;
    for (int z = 16; z < 32; ++z)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) fill.push_back({{x, y, z}, true, 5});
    edit(table, ref, fill);
    CHECK(differences(table, ref) == 0);
    CHECK(table.fullBlocks == 1);
    CHECK(table.partialBricks == 0);
    CHECK(projv::utils::tileTableVoxelCount(table) == 4096);

    edit(table, ref, {{{3, 3, 20}, false, 0}, {{12, 9, 30}, true, 2}});   // a hole, and one other material
    CHECK(differences(table, ref) == 0);
    CHECK(table.fullBlocks == 0);
    CHECK(table.partialBricks == 2);   // only the two bricks the edits went through
    CHECK(table.fullBricks == 62);
    CHECK(projv::utils::tileTableVoxelCount(table) == 4095);
}

TEST_CASE("tile edits: a table edited down to nothing is empty; edited back up, it holds what was added") {
    projv::utils::TileTable table;
    table.resolution = 16;
    Reference ref(16);
    edit(table, ref, {{{1, 2, 3}, true, 1}, {{15, 15, 15}, true, 4}});
    CHECK(differences(table, ref) == 0);
    edit(table, ref, {{{1, 2, 3}, false, 0}, {{15, 15, 15}, false, 0}});
    CHECK(table.texels.empty());
    CHECK(differences(table, ref) == 0);
    edit(table, ref, {{{7, 0, 9}, true, 3}});
    CHECK(differences(table, ref) == 0);
    CHECK(projv::utils::tileTableVoxelCount(table) == 1);
}

TEST_CASE("tile edits: on a table built from a tree, the edited table equals one built from the edited tree") {
    const int resolution = 64;
    auto solid = [](int x, int y, int z) { return y < 20 || (x - 32) * (x - 32) + (z - 32) * (z - 32) < 64; };
    BuiltTree tree = buildTree(resolution, solid, [](int, int y, int) { return y < 10 ? 0 : 1; });
    projv::utils::TileTable table = projv::utils::buildTileTable(tree.geometry, tree.materialIDs, uint32_t(resolution));
    std::vector<projv::utils::TileEdit> edits;
    for (int x = 10; x < 30; ++x) edits.push_back({{x, 19, 40}, false, 0});
    for (int y = 20; y < 30; ++y) edits.push_back({{5, y, 5}, true, 2});
    projv::utils::applyTileEdits(table, edits);
    auto after = [&](int x, int y, int z) {
        if (x == 5 && z == 5 && y >= 20 && y < 30) return true;
        if (y == 19 && z == 40 && x >= 10 && x < 30) return false;
        return solid(x, y, z);
    };
    auto slotAfter = [&](int x, int y, int z) { return (x == 5 && z == 5 && y >= 20 && y < 30) ? 2 : (y < 10 ? 0 : 1); };
    BuiltTree edited = buildTree(resolution, after, slotAfter);
    projv::utils::TileTable fresh = projv::utils::buildTileTable(edited.geometry, edited.materialIDs, uint32_t(resolution));
    CHECK(table.texels == fresh.texels);
    CHECK(table.materials == fresh.materials);
}

TEST_CASE("tile table: walking it visits each voxel once with its slot; count, bounds and the tree round trip") {
    const int resolution = 64;
    auto solid = [](int x, int y, int z) { return (x * 7 + y * 3 + z * 5) % 9 == 0 || (y > 40 && y < 58); };
    BuiltTree tree = buildTree(resolution, solid, [](int x, int, int) { return x / 22; });
    projv::utils::TileTable table = projv::utils::buildTileTable(tree.geometry, tree.materialIDs, uint32_t(resolution));
    std::vector<int> seen(size_t(resolution) * resolution * resolution, -1);
    uint64_t visits = 0;
    projv::utils::forEachTileVoxel(table, [&](ivec3 p, uint8_t slot) {
        int& s = seen[size_t(p.x) + size_t(resolution) * (size_t(p.y) + size_t(resolution) * p.z)];
        CHECK(s == -1);
        s = slot;
        ++visits;
    });
    uint64_t expected = 0;
    ivec3 wantLow(resolution), wantHigh(-1);
    for (int z = 0; z < resolution; ++z)
        for (int y = 0; y < resolution; ++y)
            for (int x = 0; x < resolution; ++x)
                if (solid(x, y, z)) {
                    ++expected;
                    wantLow = glm::min(wantLow, ivec3(x, y, z));
                    wantHigh = glm::max(wantHigh, ivec3(x, y, z));
                    CHECK(seen[size_t(x) + size_t(resolution) * (size_t(y) + size_t(resolution) * z)] == x / 22);
                }
    CHECK(visits == expected);
    CHECK(projv::utils::tileTableVoxelCount(table) == expected);
    ivec3 low, high;
    REQUIRE(projv::utils::tileTableBounds(table, low, high));
    CHECK(low == wantLow);
    CHECK(high == wantHigh);

    BuiltTree back;
    projv::utils::treeFromTileTable(table, back.geometry, back.materialIDs);
    CHECK(mismatches(back, resolution) == 0);   // the round-tripped tree's voxels and slots
    projv::utils::TileTable again = projv::utils::buildTileTable(back.geometry, back.materialIDs, uint32_t(resolution));
    CHECK(again.texels == table.texels);
    CHECK(again.materials == table.materials);
}

// ---- .data v4: tile tables on disk ------------------------------------------------------------------

TEST_CASE(".data v4: a block given as a tree is written as its tile table and read back as one") {
    const int resolution = 64;
    BuiltTree tree = buildTree(resolution, [](int x, int y, int z) { return y < 12 || (x + z) % 9 == 0; },
                               [](int x, int y, int) { return y < 6 ? 0 : x % 3; });
    projv::DataFile file;
    file.resolution = uint32_t(resolution);
    file.voxelScale = 0.25f;
    projv::DataBlock block;
    block.geometry = tree.geometry;
    block.materialIDs = tree.materialIDs;
    file.blocks.push_back(block);
    const std::string path = (std::filesystem::temp_directory_path() / "projv_tiles_v4_tree.data").string();
    REQUIRE(projv::utils::writeDataFile(path, file));
    projv::DataFile back = projv::utils::readDataFile(path);
    std::filesystem::remove(path);
    CHECK(back.version == 4);
    REQUIRE(back.blocks.size() == 1);
    CHECK(back.blocks[0].geometry.empty());
    const projv::utils::TileTable expected = projv::utils::buildTileTable(tree.geometry, tree.materialIDs, uint32_t(resolution));
    const projv::utils::TileTable& got = back.blocks[0].tiles;
    CHECK(got.resolution == uint32_t(resolution));
    CHECK(got.leafStart == expected.leafStart);
    CHECK(got.texels == expected.texels);
    CHECK(got.materials == expected.materials);
    CHECK(got.fullBlocks == expected.fullBlocks);
    CHECK(got.fullBricks == expected.fullBricks);
    CHECK(got.partialBricks == expected.partialBricks);
}

TEST_CASE(".data v4: a tile table round-trips byte for byte, through the whole file and one block") {
    const int resolution = 256;
    BuiltTree tree = buildTree(resolution, [](int x, int y, int z) {
        const int dx = x - 120, dy = y - 100, dz = z - 140;
        return dx * dx + dy * dy + dz * dz < 70 * 70 || y < 2;
    }, [](int, int y, int) { return y < 2 ? 4 : 1; });
    projv::DataFile file;
    file.resolution = uint32_t(resolution);
    file.voxelScale = 1.0f;
    projv::DataBlock block;
    block.tiles = projv::utils::buildTileTable(tree.geometry, tree.materialIDs, uint32_t(resolution));
    REQUIRE(block.tiles.fullBlocks > 0);
    file.blocks.push_back(block);
    const std::string path = (std::filesystem::temp_directory_path() / "projv_tiles_v4_tiles.data").string();
    REQUIRE(projv::utils::writeDataFile(path, file));
    projv::DataFile back = projv::utils::readDataFile(path);
    projv::DataBlock one = projv::utils::readDataBlock(path, 0u);
    std::filesystem::remove(path);
    REQUIRE(back.blocks.size() == 1);
    CHECK(back.blocks[0].tiles.texels == block.tiles.texels);
    CHECK(back.blocks[0].tiles.materials == block.tiles.materials);
    CHECK(one.tiles.texels == block.tiles.texels);
    CHECK(one.tiles.materials == block.tiles.materials);
    CHECK(one.tiles.fullBlocks == block.tiles.fullBlocks);
}

TEST_CASE(".data v3 still reads: as a tree, which builds the same voxels as tiles") {
    projv::DataFile file = projv::utils::readDataFile((std::filesystem::path(PROJV_TEST_DATA_DIR) / "v3_pylon.data").string());
    CHECK(file.version == 3);
    REQUIRE(!file.blocks.empty());
    const projv::DataBlock& b = file.blocks[0];
    CHECK(b.tiles.texels.empty());
    REQUIRE(!b.geometry.empty());
    const projv::utils::TileTable tiles = projv::utils::buildTileTable(b.geometry, b.materialIDs, file.resolution);
    BuiltTree tree{b.geometry, b.materialIDs};
    CHECK(mismatches(tree, int(file.resolution)) == 0);
    CHECK(projv::utils::tileTableVoxelCount(tiles) > 0);
}

TEST_CASE("tile table: well-formed is every table built, and none whose offsets leave it") {
    const int resolution = 64;
    BuiltTree tree = buildTree(resolution, [](int x, int y, int z) { return y < 20 || (x * z) % 7 == 0; },
                               [](int x, int, int) { return x % 4; });
    const projv::utils::TileTable good = projv::utils::buildTileTable(tree.geometry, tree.materialIDs, uint32_t(resolution));
    CHECK(projv::utils::tileTableWellFormed(good));
    CHECK(projv::utils::tileTableWellFormed(projv::utils::TileTable{}));

    projv::utils::TileTable shortMaterials = good;
    shortMaterials.materials.resize(shortMaterials.materials.size() / 2);
    CHECK_FALSE(projv::utils::tileTableWellFormed(shortMaterials));

    projv::utils::TileTable missingLeaves = good;
    missingLeaves.texels.resize(missingLeaves.texels.size() - 4 * 8);
    CHECK_FALSE(projv::utils::tileTableWellFormed(missingLeaves));

    projv::utils::TileTable wrongResolution = good;
    wrongResolution.resolution = 16;   // its boxes leave a 16^3 chunk (at 256 they fit, and it reads the same)
    CHECK_FALSE(projv::utils::tileTableWellFormed(wrongResolution));

    projv::utils::TileTable fullOutsideOccupied = good;
    fullOutsideOccupied.texels[4] = 0;   // region 0 (texel 1, after the header) occupies nothing, but its full bits stay
    fullOutsideOccupied.texels[5] = 0;
    fullOutsideOccupied.texels[6] |= 1u;
    CHECK_FALSE(projv::utils::tileTableWellFormed(fullOutsideOccupied));

    projv::utils::TileTable badLeaf = good;
    badLeaf.texels[size_t(badLeaf.leafStart) * 4 + 2] = projv::encodeLeafNode(uint32_t(good.materials.size()), true);
    CHECK_FALSE(projv::utils::tileTableWellFormed(badLeaf));
}

TEST_CASE("tile table: palette slot 255 survives the build and edits") {
    // The dense block grid holds slot + 1: a byte would wrap 255 to empty.
    const int resolution = 16;
    BuiltTree tree = buildTree(resolution, [](int x, int y, int) { return x < 3 && y < 3; }, [](int, int, int) { return 255; });
    projv::utils::TileTable t = projv::utils::buildTileTable(tree.geometry, tree.materialIDs, uint32_t(resolution));
    CHECK(projv::utils::tileTableVoxelCount(t) == 3 * 3 * 16);
    uint8_t slot = 0;
    CHECK(projv::utils::tileTableVoxel(t, {1, 1, 1}, &slot));
    CHECK(slot == 255);
    projv::utils::applyTileEdits(t, {{{10, 10, 10}, true, 255}});
    CHECK(projv::utils::tileTableVoxelCount(t) == 3 * 3 * 16 + 1);
    CHECK(projv::utils::tileTableVoxel(t, {10, 10, 10}, &slot));
    CHECK(slot == 255);
}

TEST_CASE("tile table: storage LOD from the table is the table of the downsampled tree, byte for byte") {
    for (int resolution : {16, 64, 256, 1024}) {
        CAPTURE(resolution);
        std::mt19937 random(uint32_t(resolution) * 17u + 3u);
        projv::utils::TileTable table;
        table.resolution = uint32_t(resolution);
        std::vector<projv::utils::TileEdit> edits;
        // A solid cube of one slot and another of two (tiles, and full blocks of mixed bricks), then
        // clustered voxels of three slots scattered over the whole cube (leaves, far-apart regions).
        const int cube = std::min(resolution / 2, 32);
        for (int z = 0; z < cube; ++z)
            for (int y = 0; y < cube; ++y)
                for (int x = 0; x < cube; ++x) {
                    edits.push_back({{x, y, z}, true, 7});
                    edits.push_back({{resolution - 1 - x, y, z}, true, uint8_t(x / 4 % 2 ? 255 : 9)});
                }
        for (int i = 0; i < 3000; ++i) {
            const ivec3 c{int(random() % uint32_t(resolution)), int(random() % uint32_t(resolution)), int(random() % uint32_t(resolution))};
            const ivec3 v = glm::min(c + ivec3(int(random() % 6u), int(random() % 6u), int(random() % 6u)), ivec3(resolution - 1));
            edits.push_back({v, (random() % 5) != 0, uint8_t(random() % 3)});
        }
        projv::utils::applyTileEdits(table, edits);

        std::vector<uint32_t> tree;
        std::vector<uint8_t> treeMaterials;
        projv::utils::treeFromTileTable(table, tree, treeMaterials);
        CHECK(projv::utils::downsampleTileTable(table, 0).texels == table.texels);
        for (uint32_t steps = 1; steps <= 6; ++steps) {
            CAPTURE(steps);
            std::vector<uint32_t> coarse;
            std::vector<uint8_t> coarseMaterials;
            const uint32_t dropped = projv::utils::downsampleTree64(tree, treeMaterials, steps, coarse, coarseMaterials);
            const projv::utils::TileTable viaTree = projv::utils::buildTileTable(coarse, coarseMaterials, projv::resolutionAtLOD(uint32_t(resolution), dropped));
            const projv::utils::TileTable direct = projv::utils::downsampleTileTable(table, steps);
            CHECK(direct.resolution == viaTree.resolution);
            CHECK(direct.texels == viaTree.texels);
            CHECK(direct.materials == viaTree.materials);
            CHECK(direct.fullBlocks == viaTree.fullBlocks);
            CHECK(direct.partialBricks == viaTree.partialBricks);
            CHECK(projv::utils::tileTableWellFormed(direct));
        }
    }
}

TEST_CASE("blob voxels: tiles made from a tree replace it; a refine installs a table at its new resolution") {
    projv::Scene scene;
    projv::ChunkHeader header;
    header.resolution = 64;
    scene.chunks.push_back(projv::utils::createChunk(header));
    projv::Chunk& chunk = scene.chunks.back();
    chunk.alive = true;
    BuiltTree tree = buildTree(64, [](int x, int y, int z) { return y < 20 + (x + z) % 7; }, [](int x, int, int) { return x / 16; });
    chunk.geometryData = tree.geometry;
    const int32_t pool = projv::internChunkGeometry(scene, chunk);
    REQUIRE(pool >= 0);
    projv::GeometryBlob& blob = scene.geometryPool[size_t(pool)];
    projv::utils::setBlobTreeMaterials(blob, tree.materialIDs);
    CHECK(blob.voxelForm == projv::GeometryBlob::VoxelForm::Tree);

    // Read as tiles: the table is the tree's, and the tree is gone.
    const projv::utils::TileTable expected = projv::utils::buildTileTable(tree.geometry, tree.materialIDs, 64);
    CHECK(projv::utils::blobTiles(blob).texels == expected.texels);
    CHECK(blob.voxelForm == projv::GeometryBlob::VoxelForm::Tiles);
    CHECK(blob.treeNodes.empty());
    CHECK(projv::utils::blobVoxelBytes(blob) == expected.texels.size() * 4 + expected.materials.size());
    // A tree asked for again is built from the tiles, and holds the same voxels.
    const std::vector<uint32_t> again = projv::utils::blobTree(blob);
    CHECK(projv::utils::buildTileTable(again, projv::utils::blobTreeMaterials(blob), 64).texels == expected.texels);
    // Materials for a tree already read as tiles are refused, not joined to a tree they were not baked for.
    projv::utils::setBlobTreeMaterials(blob, std::vector<uint8_t>(tree.materialIDs.size(), 9));
    CHECK(projv::utils::blobTiles(blob).materials == expected.materials);

    // A refine: regenerated at 256^3, installed as a table, the chunk's resolution raised with it.
    projv::utils::TileTable finer;
    finer.resolution = 256;
    projv::utils::applyTileEdits(finer, {{{200, 3, 100}, true, 4}, {{7, 250, 7}, true, 1}});
    blob.renderLOD = 1;
    projv::utils::replaceChunkTiles(scene, 0, finer, 256);
    const projv::GeometryBlob& refined = scene.geometryPool[size_t(scene.chunks[0].geometryPoolIndex)];
    CHECK(scene.chunks[0].header.resolution == 256);
    CHECK(refined.renderLOD == 0);
    CHECK(projv::utils::blobTiles(refined).texels == finer.texels);
    // Without a resolution, the chunk keeps its own.
    projv::utils::replaceChunkTiles(scene, 0, finer);
    CHECK(scene.chunks[0].header.resolution == 256);
}
