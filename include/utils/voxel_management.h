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
     * Generates an octree following a modified version of the version from Nvidia's paper "Effecient Sparse Voxel Octrees"
     * following this format [23-bit relative child pointer][8-bit valid mask][1-bit leaf flag]
     * @param voxels An std::vector<std::vector<std::vector<voxel>>>& containing the scene to be converted to a sparse voxel octree.
     * @param voxelWholeResolution
     * @param octreeID 
     * @return Returns an std::vector<uint32_t> containing the entire octree structure.
     */
    std::vector<uint32_t> createOctree(VoxelGrid& voxels, int voxelWholeResolution);

    /**
     * Generates voxel type data that stores 8 bytes of data per voxel. Currently, a color and 3 8 bit sections of extra data.
     * @param voxels The 3D voxel std::vector that contains all of the voxels in the scene.
     * @param voxelWholeResolution The resolution of the entire 3D voxel vector.
     * @param voxelCount Optional paramater that reserves memory space to potentially decrease generation times from re-reserving during creation. 
     */
    std::vector<uint32_t> createVoxelTypeData(VoxelGrid& voxels);

    /**
     * Adds a voxel to a VoxelGrid. Expensive because it has to insert it in the correct sorted order.
     * @param voxels The VoxelGrid to add the voxel too.
     * @param position The position at which the voxel resides in the VoxelGrid
     * @param color The color of the voxel in the VoxelGrid
     */
    void addVoxelToVoxelGrid(VoxelGrid& voxels, core::ivec3 position, Color color);

    /**
     * Adds a VoxelBatch to a VoxelGrid. Faster than addVoxelToVoxels for large amounts of voxels, but slower for small amounts of voxels.
     * @param voxels VoxelGrid to add VoxelBatch to.
     * @param voxelBatch voxelBatch to add to VoxelGrid
     */
    void addVoxelBatchToVoxelGrid(VoxelGrid& voxels, VoxelBatch& voxelBatch);

    /**
     * Creates a VoxelBatch
     * @return Returns an empty VoxelBatch
     */
    VoxelBatch createVoxelBatch();

    /**
     * Creates a ChunkHeader based on a position, voxel scale, and resolution.
     * @param position The world-space position of the chunk.
     * @param voxelScale The size of a single voxel in world units.
     * @param resolutionPowOf2 The resolution of the chunk, given as a power of 2 (e.g., 7 for 128^3 voxels).
     * @return A populated ChunkHeader with the specified parameters.
     */
    ChunkHeader createChunkHeader(core::vec3 position, float voxelScale, int resolutionPowOf2);

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

    /**
     * Adds a voxel to a voxel batch.
     * @param voxel The voxel to add.
     * @param voxelBatch The target VoxelBatch that the voxel will be added to.
     */
    void addVoxelToVoxelBatch(Voxel& voxel, VoxelBatch& voxelBatch);

    /**
     * Copies all voxels from a VoxelBatch to a Chunk.
     * @param voxelBatch The source VoxelBatch containing voxel data.
     * @param chunk The destination Chunk to receive the voxel data.
     */
    void copyVoxelBatchToChunk(VoxelBatch& voxelBatch, Chunk& chunk);

    /**
     * Retrieves a VoxelBatch representing the contents of a Chunk.
     * @param chunk The source chunk.
     * @param convertCompressedData Whether to decompress chunk data before converting it to a VoxelBatch.
     * @return A VoxelBatch containing the voxel data from the chunk.
     */
    VoxelBatch getChunkVoxelBatch(Chunk& chunk, bool convertCompressedData = true);

    /**
     * Merges voxel data from voxelBatchA into voxelBatchB with an optional positional offset.
     * @param voxelBatchA The VoxelBatch to merge from.
     * @param voxelBatchB The VoxelBatch to merge into.
     * @param voxelBatchAPosition Optional offset to apply to voxelBatchA's data during the merge.
     */
    void addVoxelBatchAToVoxelBatchB(VoxelBatch& voxelBatchA, VoxelBatch& voxelBatchB, core::ivec3 voxelBatchAPosition = {0, 0, 0});

    /**
     * Updates a Chunk's internal voxel representation using its current VoxelBatch.
     * @param chunk The Chunk to update.
     * @param clearBatch Whether to clear the VoxelBatch after the update.
     */
    void updateChunkFromItsVoxelBatch(Chunk& chunk, bool clearBatch = true);

    /**
     * Removes voxel data from voxelBatchB that overlaps with voxelBatchA, using an optional position offset.
     * @param voxelBatchA The VoxelBatch containing voxels to remove.
     * @param voxelBatchB The VoxelBatch from which voxels will be removed.
     * @param positionOffset An optional positional offset to apply when comparing voxels.
     */
    void removeVoxelBatchAFromVoxelBatchB(VoxelBatch& voxelBatchA, VoxelBatch& voxelBatchB, core::ivec3 positionOffset = {0, 0, 0});
}

#endif