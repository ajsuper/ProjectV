#ifndef PROJECTV_VOXEL_MANAGEMENT_H
#define PROJECTV_VOXEL_MANAGEMENT_H

#include <vector>
#include <stdint.h>
#include <iostream>
#include <math.h>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <bitset>
#include <unordered_set>

#include "data_structures/voxel.h"
#include "data_structures/nodeStructure.h"
#include "core/math.h"
#include "core/log.h"
#include "data_structures/scene.h"
#include "voxel_math.h"

namespace projv::utils {
    /**
     * Initiates an empty grid of voxel's of size resolution.
     * @param size A power of 2 size of the voxel grid.
     * @return A grid with xyz dimensions of size and all voxel initialized to white and empty.
     */
    VoxelGrid createVoxelGrid();

    /**
     * Creates a Chunk object using an existing ChunkHeader.
     * @param chunkHeader The metadata describing the chunk's location, scale, and resolution.
     * @return A new Chunk initialized with the given header.
     */
    Chunk createChunk(ChunkHeader chunkHeader);

    /**
     * Calculates the world-space size (scale) of a chunk from its voxel scale and resolution.
     * @param voxelScale The size of a single voxel in world units.
     * @param resolutionPowerOf2 The resolution of the chunk as a power of 2.
     * @return The total size (scale) of the chunk in world units.
     */
    float createChunkScaleFromVoxelScaleAndResolution(float voxelScale, int resolutionPowerOf2);

    // ---- Brick Map API ----

    /**
     * Create an empty VoxelBrickMap for the given brick dimensions.
     * All bricks are initially absent (brickMask all zeros).
     */
    std::unique_ptr<VoxelBrickMap> createVoxelBrickMap(const core::ivec3& brickDims);

    /**
     * Set a voxel in the brick map at integer chunk-space position (x, y, z).
     * Creates the brick if it doesn't exist. O(1) average.
     */
    void brickMapSetVoxel(VoxelBrickMap& map, int x, int y, int z, Color color);

    /**
     * Remove a voxel from the brick map at integer chunk-space position (x, y, z).
     * No-op if the voxel doesn't exist. O(1) average.
     */
    void brickMapClearVoxel(VoxelBrickMap& map, int x, int y, int z);

    /**
     * Check if a voxel exists at integer chunk-space position (x, y, z). O(1).
     */
    bool brickMapHasVoxel(const VoxelBrickMap& map, int x, int y, int z);

    /**
     * Get the color of a voxel at integer chunk-space position (x, y, z).
     * Returns black if the voxel doesn't exist. O(1) average.
     */
    Color brickMapGetColor(const VoxelBrickMap& map, int x, int y, int z);

    /**
     * Populate a brick map from an existing voxelTypeData array (3 u32s per voxel).
     * Used on first edit of a disk-loaded blob. The brick map must already
     * have the correct brickDims for the chunk's resolution.
     */
    void brickMapFromVoxelTypeData(VoxelBrickMap& map,
                                   const std::vector<uint32_t>& voxelTypeData);

    /**
     * Build a sorted VoxelGrid (by chunk-space Z-order) from a brick map.
     * Used as input to createTree64 for tree64 generation.
     * Note: updateChunkFromBrickMap no longer calls this; prefer buildTree64FromBrickMap.
     */
    VoxelGrid buildVoxelGridFromBrickMap(const VoxelBrickMap& map);

    /**
     * Build voxelTypeData (3 u32s per voxel) directly from a brick map,
     * in chunk-space Z-order. Faster than going through VoxelGrid.
     * Note: updateChunkFromBrickMap no longer calls this; prefer buildVoxelTypeDataFromBrickMapFast.
     */
    std::vector<uint32_t> buildVoxelTypeDataFromBrickMap(const VoxelBrickMap& map);

    /**
     * Build a tree64 directly from brick map bitmasks, skipping the intermediate VoxelGrid.
     * The brick's per-row mask[] IS the leaf-level tree64 bitmask — no bit walking needed.
     * Uses the existing aggregateLevelTree64 for higher levels.
     */
    std::vector<uint32_t> buildTree64FromBrickMap(const VoxelBrickMap& map, int resolution);

    /**
     * Build voxelTypeData directly from brick map, computing chunk-space Z-order
     * via direct composition: chunkZOrder = (brickZOrder << 18) | localZOrder.
     * Iterates bits MSB-first to produce ascending Z-order output without sorting.
     */
    std::vector<uint32_t> buildVoxelTypeDataFromBrickMapFast(const VoxelBrickMap& map);

    /**
     * Update a chunk's geometryData, voxelTypeData, and header from a brick map.
     * Calls buildTree64FromBrickMap + buildVoxelTypeDataFromBrickMapFast.
     * Sets LOD to 0.
     */
    void updateChunkFromBrickMap(Chunk& chunk, const VoxelBrickMap& map);

    /**
     * Deep-clone a VoxelBrickMap (for COW fork). Copies all bricks and
     * their hash map contents.
     */
    std::unique_ptr<VoxelBrickMap> cloneBrickMap(const VoxelBrickMap& src);
}

#endif
