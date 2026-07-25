#include "utils/material.h"
#include "core/log.h"

namespace projv::utils {

uint8_t internMaterial(ComponentRecord& comp, const std::string& name, uint32_t packedColor) {
    if (!name.empty()) {
        for (size_t i = 0; i < comp.materialPalette.size(); ++i) {
            if (comp.materialPalette[i].name == name) {
                return static_cast<uint8_t>(i);
            }
        }
    }
    for (size_t i = 0; i < comp.materialPalette.size(); ++i) {
        if (comp.materialPalette[i].packedColor == packedColor) {
            return static_cast<uint8_t>(i);
        }
    }
    // Hard cap: material IDs are uint8_t, and 255 is INVALID_MATERIAL. Past this point the
    // static_cast below would wrap and the new material would silently alias palette slot
    // (index & 255), corrupting the colors of whatever already owns that slot. Fail loudly
    // instead -- this used to be an assert, which is compiled out in release builds.
    if (comp.materialPalette.size() >= MAX_MATERIALS_PER_COMPONENT - 1) {
        core::error("internMaterial: palette full ({} entries) -- cannot add '{}' (0x{:06X}). "
                    "Material IDs are uint8_t; quantize or reduce the color set.",
                    comp.materialPalette.size(), name, packedColor);
        return INVALID_MATERIAL;
    }
    Material mat;
    mat.name = name;
    mat.packedColor = packedColor;
    comp.materialPalette.push_back(mat);
    comp.paletteVersion++;
    return static_cast<uint8_t>(comp.materialPalette.size() - 1);
}

uint8_t findMaterialByName(const ComponentRecord& comp, const std::string& name) {
    for (size_t i = 0; i < comp.materialPalette.size(); ++i) {
        if (comp.materialPalette[i].name == name) {
            return static_cast<uint8_t>(i);
        }
    }
    return INVALID_MATERIAL;
}

uint8_t findMaterialByColor(const ComponentRecord& comp, uint32_t packedColor) {
    for (size_t i = 0; i < comp.materialPalette.size(); ++i) {
        if (comp.materialPalette[i].packedColor == packedColor) {
            return static_cast<uint8_t>(i);
        }
    }
    return INVALID_MATERIAL;
}

void brickMapSetMaterial(BrickData& brick, uint32_t localZOrder, uint8_t materialID) {
    brick.materials[localZOrder] = materialID;
}

uint8_t brickMapGetMaterial(const BrickData& brick, uint32_t localZOrder) {
    auto it = brick.materials.find(localZOrder);
    return (it != brick.materials.end()) ? it->second : 0;
}

void bakeMaterialsFromBrickMap(std::vector<uint32_t>& geometry,
                                std::vector<uint8_t>& materialIDs,
                                const VoxelBrickMap& map) {
    materialIDs.clear();
    size_t estimatedVoxels = 0;
    for (uint32_t bz = 0; bz < map.totalBricks; ++bz) {
        if (!map.bricks[bz]) continue;
        for (uint32_t row = 0; row < BRICK_MASK_ROWS; ++row) {
            estimatedVoxels += static_cast<size_t>(
                __builtin_popcountll(map.bricks[bz]->mask[row]));
        }
    }
    materialIDs.reserve(estimatedVoxels);

    size_t nodeCount = geometry.size() / 3;
    int32_t cursorBrick = 0;
    int32_t cursorRow = -1;

    auto advanceCursor = [&]() -> bool {
        while (cursorBrick < static_cast<int32_t>(map.totalBricks)) {
            ++cursorRow;
            if (cursorRow >= static_cast<int32_t>(BRICK_MASK_ROWS)) {
                ++cursorBrick;
                if (cursorBrick < static_cast<int32_t>(map.totalBricks)) {
                    cursorRow = -1;
                    continue; // skip the mask check this pass; next ++cursorRow -> 0
                } else break;
            }
            if (map.bricks[cursorBrick] &&
                map.bricks[cursorBrick]->mask[cursorRow] != 0) {
                return true;
            }
        }
        return false;
    };

    for (size_t n = 0; n < nodeCount; ++n) {
        uint32_t ptrFlag = geometry[n * 3 + 2];
        bool isLeaf = (ptrFlag & 1u) != 0;
        if (!isLeaf) continue;

        uint32_t materialOffset = static_cast<uint32_t>(materialIDs.size());

        if (!advanceCursor()) {
            core::error("bakeMaterialsFromBrickMap: cursor exhausted before leaf nodes");
            break;
        }

        const BrickData& brick = *map.bricks[cursorBrick];
        uint64_t rowBits = brick.mask[cursorRow];

        size_t runStart = materialIDs.size();
        bool uniform = true;
        uint8_t firstID = 0;
        bool haveFirst = false;

        while (rowBits) {
            int leadingZeros = __builtin_clzll(rowBits);
            uint32_t localZOrder = static_cast<uint32_t>(cursorRow) * 64 + leadingZeros;

            auto mit = brick.materials.find(localZOrder);
            uint8_t matID = (mit != brick.materials.end()) ? mit->second : 0;
            materialIDs.push_back(matID);
            if (!haveFirst) { firstID = matID; haveFirst = true; }
            else if (matID != firstID) uniform = false;

            rowBits &= ~(1ull << (63 - leadingZeros));
        }

        // Uniform-leaf compression. Terrain-like content fills whole leaves with one material
        // (measured ~96% of leaves), so collapse those to a single byte and let the shader skip
        // the per-voxel index. Random access is preserved -- the shader just ignores `above`.
        if (uniform && materialIDs.size() > runStart + 1) {
            materialIDs.resize(runStart + 1);
        }

        geometry[n * 3 + 2] = encodeLeafNode(materialOffset, uniform);
    }
}

} // namespace projv::utils