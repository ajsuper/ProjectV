#ifndef PROJECTV_MATERIAL_H
#define PROJECTV_MATERIAL_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

#include "data_structures/color.h"
#include "data_structures/scene.h"
#include "data_structures/voxel.h"

namespace projv::utils {

uint8_t internMaterial(GeometryBlob& blob, const std::string& name, uint32_t packedColor);
uint8_t findMaterialByName(const GeometryBlob& blob, const std::string& name);
uint8_t findMaterialByColor(const GeometryBlob& blob, uint32_t packedColor);

void brickMapSetMaterial(BrickData& brick, uint32_t localZOrder, uint8_t materialID);
uint8_t brickMapGetMaterial(const BrickData& brick, uint32_t localZOrder);

void bakeMaterialsFromBrickMap(std::vector<uint32_t>& geometry,
                                GeometryBlob& blob,
                                const VoxelBrickMap& map);

} // namespace projv::utils

#endif