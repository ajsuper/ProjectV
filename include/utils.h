
#ifndef PROJECTV_UTILS_H
#define PROJECTV_UTILS_H

#include <iostream>
#include <fstream>
#include <vector>
#include <array>
#include <stdint.h>
#include <bitset>
#include <math.h>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <json.hpp>

#include "data_structures/color.h"
#include "data_structures/voxel.h"
#include "data_structures/scene.h"

namespace projv{
     /**
     * Initiates an empty grid of voxel's of size resolution.
     * @param size A power of 2 size of the voxel grid.
     * @return A grid with xyz dimensions of size and all voxel initialized to white and empty.
     */
    std::vector<std::vector<std::vector<voxel>>> createVoxelGrid(int size);

    /**
     * Converts a 3D vector to a single uint32_t storing that position.
     * @param x, y, z The 3d vector to convert to a single uint32_t.
     * @return A single uint32_t storing the 3D vector.
     */
    uint32_t convertVec3ToHeaderPosition(uint32_t x, uint32_t y, uint32_t z);

    /**
     * Converts a single uint32_t storing a 3D vector to an std::array<int, 3>.
     * @param headerPosition A single uint32_t storing a 3D vector.
     * @return An std::array<int, 3> containing the 3D vector.
     */
    std::array<int, 3> convertHeaderPositionToVec3(uint32_t headerPosition);

    /**
     * Writes an std::vector<uint32_t> to a file directory.
     * @param vector An std::vector<uint32_t> to be written to the file in fileDirectory.
     * @param fileDirectory An std::string containgin the directory to write the file.
     */
    void writeUint32Vector(std::vector<uint32_t> vector, std::string fileDirectory);

    /**
     * Reads a vector of uint32_t's from a file directory.
     * @param fileDirectory An std::string containing the directory of the file to be read.
     * @return An std::vector<uint32_t> containing the uint32_t's in order from the file.
     */
    std::vector<uint32_t> readUint32Vector(std::string fileDirectory);

    /**
     * Writes the headers for a scene into a file directory.
     * @param chunkHeaders An std::vector<chunkHeader> containing the headers to be written.
     * @param fileDirectory An std::string containing the parent directory to write the headers to.
     */
    void writeHeadersJSON(const std::vector<chunkHeader>& chunkHeaders, const std::string& fileDirectory);

    /**
     * Reads the headers for a scene from a file directory.
     * @param fileDirectory An std::string containing the directory of the file to be read.
     * @return An std::vector<chunkHeader> containing the chunk headers in order from the file.
     */
    std::vector<chunkHeader> readHeadersJSON(const std::string& fileDirectory);
    /**
     * Writes a scene to a file directory.
     * @param scene The scene to be written to the file.
     * @param fileDirectory An std::string containing the directory to write the scene to.
     */
    void writeScene(scene scene, std::string fileDirectory);

    /**
     * Reads a scene from a file directory.
     * @param fileDirectory An std::string containing the directory of the file to be read.
     * @return A scene containing the data from the file.
     */
    scene readScene(std::string fileDirectory);

    /**
     * Finds how many filled voxels are within boundsMin and boundsMax in the 3D voxel vector.
     * @param voxels A 3D vector of voxel's.
     * @param voxelWholeResolution The resolution of the entire voxels vector.
     * @param boundsMin The smaller of the 2 bounds to check against.
     * @param boundsMax The larger of the 2 bounds to check against.
     * @return Returns the number of voxels within the bounds.
     */
    int voxInBounds(std::vector<std::vector<std::vector<voxel>>>& voxels, int wholeVoxelResolution, int boundsMin[3], int boundsMax[3]);

    /**
     * Calculates the masks for a section of voxels based off of the minimum xyz. The depthResolution/2 gives the size of the child nodes and is used to create the octants corresponding to their parent.
     * @param voxels A 3D vector of voxel's.
     * @param depthResolution The resolution of the voxel grid at a certain depth. Depth 1 is 2x2x2, depth 2 is 4x4x4. Essentially pow(2, depth)
     * @param wholeVoxelResolution The resolution of the voxel grid at its highest resolution. A voxel grid of 256^3 would be 256^3.
     * @return A 9 bit mask stored in a uint16_t. The rightmost bit is to indicate if the nodes are leaf nodes. The 8 to the left of that indicate whether they are non-empty nodes.
     */
    uint16_t buildMask(std::vector<std::vector<std::vector<voxel>>>& voxels, const int& checkX, const int& checkY, const int& checkZ, const int& depthResolution, int wholeVoxelResolution);

    /**
     * Creates the Z-Order Index of a point in 3D space with a certain bit depth.
     * @param x, y, z Position in 3D space to calculate Z-Order of.
     * @param bitDepth How many bits the index takes up.
     * @return Retruns the Z-Order
     */
    uint64_t createZOrderIndex(const int& x, const int& y, const int& z, const int& bitDepth);

    /**
     * Creates a 3D point from a Z-Order index and the number of bits the index takes up.
     * @param z_order The Z-Order of the point
     * @param bitDepth The number of bits the index takes up.
     * @return Returns an std::array<int, 3> containg the 3D point given from the Z-Order index.
     */
    std::array<int, 3> reverseZOrderIndex(uint64_t z_order, int bitDepth);

    /**
     * Build all of the masks for the current depth in the octree. Does not return anything, but modifies the octree parameter to add the masks.
     * @param octree An empty std::vector<uint32_t> that contains the nodes of our octree.
     * @param voxels A 3D vector of voxel's to generate the masks from.
     * @param depthInOctree The depth at which we are building masks.
     * @param voxelWholeResolution The resolution of the entire 3D voxel vector.
     */
    void buildMasksForWholeDepth(std::vector<uint32_t>& octree, std::vector<std::vector<std::vector<voxel>>>& voxels, int depthInOctree, int voxelWholeResolution);

    /**
     * Generate the relative child pointers and combine them with a partially constructed octree containing only the masks.
     * @param octree An std::vector<uint32_t> that has all of it's masks to which we are going to be adding the pointers.
     * @return Returns an optional reference to the octree.
     */
    std::vector<uint32_t> addPointers(std::vector<uint32_t>& octree);

    /**
     * Generates an octree following a modified version of the version from Nvidia's paper "Effecient Sparse Voxel Octrees"
     * following this format [23-bit relative child pointer][8-bit valid mask][1-bit leaf flag]
     * @param voxels An std::vector<std::vector<std::vector<voxel>>>& containing the scene to be converted to a sparse voxel octree.
     * @param voxelWholeResolution
     * @param octreeID 
     * @return Returns an std::vector<uint32_t> containing the entire octree structure.
     */
    std::vector<uint32_t> createOctree(std::vector<std::vector<std::vector<voxel>>>& voxels, int voxelWholeResolution, uint32_t octreeID);

    /**
     * Generates voxel type data that stores 8 bytes of data per voxel. Currently, a color and 3 8 bit sections of extra data.
     * @param voxels The 3D voxel std::vector that contains all of the voxels in the scene.
     * @param voxelWholeResolution The resolution of the entire 3D voxel vector.
     * @param voxelCount Optional paramater that reserves memory space to potentially decrease generation times from re-reserving during creation. 
     */
    std::vector<uint32_t> createVoxelTypeData(std::vector<std::vector<std::vector<voxel>>>& voxels, int voxelWholeResolution, int voxelCount = -1);

    //TEMPORARY
    bool isInSierpinskiTriangle(int x, int y, int z);

    //TEMPORARY
    bool isInSierpinskiCube(int x, int y, int z);
}

#endif
