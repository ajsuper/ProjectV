#ifndef PROJECTV_VOXEL_MANAGEMENT_H
#define PROJECTV_VOXEL_MANAGEMENT_H


#include <memory>
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
    void brickMapSetVoxel(VoxelBrickMap& map, int x, int y, int z, uint8_t materialID);

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
     * Rebuild the editable brick map from a baked tree64 and its material bytes — the inverse of
     * buildTree64FromBrickMap + bakeMaterialsFromBrickMap, and the reason a .data needs to store
     * nothing but those two arrays.
     *
     * Called on the first edit of a blob loaded from disk (see ensureBrickMapExists). Voxel positions
     * are recovered from the descent path rather than stored: a child's Z-order within its parent is
     * its bit index in the parent's 64-bit mask, so accumulating `parentZOrder * 64 + childBit` down
     * to the leaves reproduces each voxel's chunk-space Z-order exactly. Material bytes are read
     * through each leaf's own offset, honouring the uniform-leaf flag.
     *
     * Slot numbering is preserved as-is: the bytes name slots in the component's palette, and this
     * does not touch the palette or intern anything.
     *
     * @param geometry The tree64 array (3 uint32s per node, root first).
     * @param materialIDs The baked material bytes the leaves index into. May be empty, in which case
     *                    every voxel is rebuilt with slot 0.
     * @param resolution The chunk's edge resolution, which fixes the brick dimensions and depth.
     * @return The rebuilt brick map, or an empty map when the geometry is malformed.
     */
    std::unique_ptr<VoxelBrickMap> brickMapFromTree64(const std::vector<uint32_t>& geometry,
                                                      const std::vector<uint8_t>& materialIDs,
                                                      uint32_t resolution);

    /**
     * Build a sorted VoxelGrid (by chunk-space Z-order) from a brick map.
     */
    VoxelGrid buildVoxelGridFromBrickMap(const VoxelBrickMap& map);

    /**
     * Build a tree64 directly from brick map bitmasks, skipping the intermediate VoxelGrid.
     * The brick's per-row mask[] IS the leaf-level tree64 bitmask — no bit walking needed.
     * Uses the existing aggregateLevelTree64 for higher levels.
     */
    std::vector<uint32_t> buildTree64FromBrickMap(const VoxelBrickMap& map, int resolution);

    /**
     * Update a chunk's geometryData from a brick map.
     * Calls buildTree64FromBrickMap.
     * Sets LOD to 0.
     * Materials are baked separately via bakeMaterialsFromBrickMap.
     */
    void updateChunkFromBrickMap(Chunk& chunk, const VoxelBrickMap& map);

    /**
     * Deep-clone a VoxelBrickMap (for COW fork). Copies all bricks and
     * their hash map contents.
     */
    std::unique_ptr<VoxelBrickMap> cloneBrickMap(const VoxelBrickMap& src);

    /**
     * Coarsen a serialized tree64 by dropping `lodSteps` levels off the bottom.
     *
     * A tree64 stores its levels root-first and contiguously (buildTree64FromBrickMap) and its
     * child pointers are relative (addPointersTree64), so coarsening is a tail truncation: the
     * retained prefix is byte-identical and its pointers stay valid. The new last level's nodes
     * are re-stamped as leaves, their 64 mask bits reinterpreted from "which children exist" to
     * "which voxels are solid", and given a representative material per set bit.
     *
     * Each step is 4x coarser per axis. The caller must present the matching reduced resolution
     * to the shader -- use resolutionAtLOD(resolution, returnValue).
     *
     * Do NOT re-run addPointersTree64 on the output: it ORs pointer bits in rather than assigning,
     * so a second pass would corrupt the already-pointered prefix.
     *
     * The inputs are left untouched; outputs are written to caller-owned buffers so a per-upload
     * scratch can be reused. Returns the number of levels actually dropped, clamped so the root
     * level always survives (so it can be less than lodSteps on a shallow tree).
     */
    uint32_t downsampleTree64(const std::vector<uint32_t>& geometry,
                              const std::vector<uint8_t>&  materialIDs,
                              uint32_t lodSteps,
                              std::vector<uint32_t>& outGeometry,
                              std::vector<uint8_t>&  outMaterialIDs);
}

#endif
