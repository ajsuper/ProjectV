// --td-verify: reads a converted folder back through the engine, the way a game would, and decodes
// every block's tree64 to count what it holds.

#include "utils/tile_table.h"
#include "verify.h"

#include <bit>
#include <filesystem>
#include <map>

#include "common.h"
#include "data_structures/compose.h"
#include "utils/compose_io.h"
#include "utils/voxel_management.h"

namespace fs = std::filesystem;

namespace teardown {

VerifyResult verifyFolder(const std::string& folder, bool print) {
    VerifyResult result;
    projv::ComposeDoc doc = projv::utils::parseComposeJson((fs::path(folder) / "compose.json").string());
    if (doc.version == 0) {
        std::printf("%s: no readable compose.json\n", folder.c_str());
        return result;
    }

    // Every .data the document names, decoded block by block.
    bool ok = true;
    size_t voxels = 0, blocks = 0, files = 0;
    std::map<std::string, size_t> perFile;
    for (const projv::ComposeComponent& c : doc.components) {
        if (c.type != projv::ComponentType::Data) continue;
        const std::string path = (fs::path(folder) / c.source).string();
        auto [it, fresh] = perFile.try_emplace(path, 0);
        if (fresh) {
            projv::DataFile data = projv::utils::readDataFile(path);
            if (data.resolution == 0 || data.blocks.empty()) {
                std::printf("  %s: unreadable or empty\n", c.source.c_str());
                ok = false;
                continue;
            }
            files++;
            for (const projv::DataBlock& b : data.blocks) {
                // A v4 block is its tile table; an older one is a tree, read as the table built from it.
                const projv::utils::TileTable tiles = b.tiles.texels.empty()
                    ? projv::utils::buildTileTable(b.geometry, b.materialIDs, data.resolution) : b.tiles;
                size_t n = 0;
                uint8_t maxSlot = 0;
                projv::utils::forEachTileVoxel(tiles, [&](projv::core::ivec3, uint8_t slot) {
                    n++;
                    maxSlot = std::max(maxSlot, slot);
                });
                if (n == 0 || maxSlot >= c.palette.size()) {
                    std::printf("  %s block (%d,%d,%d): %zu voxels, slot %u of a %zu-entry palette\n", c.source.c_str(), b.gridX,
                                b.gridY, b.gridZ, n, maxSlot, c.palette.size());
                    ok = false;
                }
                it->second += n;
                blocks++;
            }
        }
        voxels += it->second;
        if (fresh && g_verbosity > 1) std::printf("   file %s %zu\n", c.source.c_str(), it->second);
    }

    // And the whole folder through the loader, as a program would open it.
    projv::Scene scene = projv::utils::loadComposeFromDisk(folder);
    size_t chunks = 0;
    for (const auto& ch : scene.chunks) chunks += ch.alive ? 1 : 0;
    float cell = 0.0f;
    for (const auto& g : scene.grids) cell = std::max(cell, g.cellSize);
    if (print && cell > 0.0f) std::printf("%s: largest grid cell %.2f world units\n", folder.c_str(), cell);
    if (print)
        std::printf("%s: %zu components, %zu files, %zu blocks, %zu voxels placed; loader: %zu chunks, %zu grids, %zu loose\n",
                    folder.c_str(), doc.components.size(), files, blocks, voxels, chunks, scene.grids.size(), scene.looseChunks.size());
    if (scene.components.size() < doc.components.size()) {
        std::printf("  the loader made %zu components of %zu\n", scene.components.size(), doc.components.size());
        ok = false;
    }
    result.ok = ok;
    result.components = doc.components.size();
    result.files = files;
    result.blocks = blocks;
    result.voxels = voxels;
    result.chunks = chunks;
    result.grids = scene.grids.size();
    result.loose = scene.looseChunks.size();
    return result;
}

} // namespace teardown
