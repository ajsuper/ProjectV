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
    void addVoxelToVoxelGrid(VoxelGrid& voxels, std::array<int, 3> position, Color color);

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
     * Adds a voxel to a voxel batch
     * @param voxel Voxel to add to VoxelBatch
     * @param voxelBatch VoxelBatch to have the voxel added too.
     */
    void addVoxelToBatch(Voxel voxel, VoxelBatch& voxelBatch);
}

#endif