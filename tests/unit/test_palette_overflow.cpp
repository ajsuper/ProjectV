// A palette of 256 entries (MAX_PALETTE_ENTRIES is 255): slot 255 is INVALID_MATERIAL, so a file
// that used it -- the Teardown converter's, before it knew -- has its last entry folded into the
// nearest other as it loads, and the voxels on it moved there. Before, an edit copying such a voxel
// (a break cutting it into a piece) asked for "no slot", and found the palette full.

#include "doctest/doctest.h"

#include <filesystem>

#include "utils/blob_voxels.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/picking.h"
#include "utils/scene_query.h"
#include "utils/tile_table.h"

namespace {
    using projv::ComponentHandle;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::Material;
    using projv::core::ivec3;
    namespace utils = projv::utils;

    Material entry(uint32_t r, uint32_t g, uint32_t b, const char* physics = "") {
        Material m;
        m.packedColor = (r << 20) | (g << 10) | b;
        m.physics = physics;
        return m;
    }

    // 256 greys, slot 255 a red of "metal", with a red of no material and a blue of "metal" to choose from.
    std::vector<Material> fullPalette() {
        std::vector<Material> p;
        for (uint32_t i = 0; i < 256; i++) p.push_back(entry(i * 4, i * 4, i * 4));
        p[10] = entry(1000, 0, 0);
        p[20] = entry(0, 0, 1000, "metal");
        p[255] = entry(1000, 10, 10, "metal");
        return p;
    }
}

TEST_CASE("palette overflow: the 256th entry folds into the nearest of the same material, else the nearest colour") {
    std::vector<Material> p = fullPalette();
    CHECK(utils::foldOverflowSlot(p) == 20);   // the metal one, though the red is nearer in colour
    CHECK(p.size() == projv::MAX_PALETTE_ENTRIES);

    std::vector<Material> plain = fullPalette();
    plain[20].physics.clear();
    plain[255].physics.clear();
    CHECK(utils::foldOverflowSlot(plain) == 10);   // no metal left: the nearest colour

    std::vector<Material> within(255, entry(1, 2, 3));
    CHECK(utils::foldOverflowSlot(within) == projv::INVALID_MATERIAL);
    CHECK(within.size() == 255);

    std::vector<uint8_t> bytes{0, 255, 7, 255};
    utils::remapSlotBytes(bytes, 255, 20);
    CHECK(bytes == std::vector<uint8_t>{0, 20, 7, 20});
}

TEST_CASE("palette overflow: a component saved with 256 entries loads with 255, its slot-255 voxels on the fold") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "projv_unit_palette_overflow";
    std::filesystem::remove_all(dir);
    {
        projv::Scene scene;
        ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "wall", INVALID_COMPONENT_HANDLE, 16, 0.5f);
        utils::queueVoxelAdd(scene, h, {{true, ivec3(1, 1, 1), 0x3FFFFFFFu}, {true, ivec3(2, 1, 1), 0x3FFFFFFFu}});
        utils::updateScene(scene);
        scene.components[h].materialPalette = fullPalette();   // as a converter wrote it
        projv::GeometryBlob& blob = scene.geometryPool[size_t(scene.chunks[scene.components[h].chunkHandle].geometryPoolIndex)];
        utils::applyTileEdits(utils::editBlobTiles(blob, 16), {{ivec3(2, 1, 1), true, 255}});
        REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, dir.string()));
    }
    projv::Scene loaded = utils::loadComposeFromDisk(dir.string());
    REQUIRE(loaded.components.size() == 1);
    CHECK(loaded.components[0].materialPalette.size() == projv::MAX_PALETTE_ENTRIES);
    const projv::Chunk& chunk = loaded.chunks[loaded.components[0].chunkHandle];
    const projv::GeometryBlob& blob = loaded.geometryPool[size_t(chunk.geometryPoolIndex)];
    uint8_t slot = 0;
    REQUIRE(utils::queryVoxelMaterial(blob, 16, ivec3(2, 1, 1), slot));
    CHECK(slot == 20);
    REQUIRE(utils::queryVoxelMaterial(blob, 16, ivec3(1, 1, 1), slot));
    CHECK(slot != 255);

    // The streamed loader is told the same.
    std::vector<utils::SceneBlockSource> blocks;
    utils::openComposeFromDisk(dir.string(), blocks);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].foldedSlot == 20);
    std::filesystem::remove_all(dir);
}
