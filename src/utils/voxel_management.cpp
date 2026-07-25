#include "utils/voxel_management.h"
#include "utils/material.h"
#include "data_structures/nodeStructure.h"
#include <numeric>

namespace projv {
    GeometryBlob::GeometryBlob(const GeometryBlob& rhs)
        : geometry(rhs.geometry)
        , materialIDs(rhs.materialIDs)
        , voxelTypeData(rhs.voxelTypeData)
        , brickMap(rhs.brickMap ? utils::cloneBrickMap(*rhs.brickMap) : nullptr)
        , sourceDataPath(rhs.sourceDataPath)
        , sourceBlockCoord(rhs.sourceBlockCoord)
        , ownsSourceFile(rhs.ownsSourceFile)
        , refCount(rhs.refCount)
        , dirty(rhs.dirty)
    {}

    GeometryBlob& GeometryBlob::operator=(const GeometryBlob& rhs) {
        if (this == &rhs) return *this;
        geometry       = rhs.geometry;
        materialIDs    = rhs.materialIDs;
        voxelTypeData  = rhs.voxelTypeData;
        brickMap       = rhs.brickMap ? utils::cloneBrickMap(*rhs.brickMap) : nullptr;
        sourceDataPath = rhs.sourceDataPath;
        sourceBlockCoord = rhs.sourceBlockCoord;
        ownsSourceFile = rhs.ownsSourceFile;
        refCount       = rhs.refCount;
        dirty          = rhs.dirty;
        return *this;
    }
} // namespace projv

namespace projv::utils {
    VoxelGrid createVoxelGrid() {
        // Initialize the 3D vector with `voxel` instances.
        VoxelGrid voxels;
        voxels.max = 0;
        return voxels;
    }

    void addPointersTree64(std::vector<uint32_t>& tree64) {
        uint32_t pointerMask = 0b11111111111111111111111111111110;
        int childCounter = 0;
        bool childPointerTooLarge = false;
        assert(tree64.size() % 3 == 0);
        // Divide by 3 because each node takes up 3 indecies.
        for(size_t address = 0; address < tree64.size() / 3; address++){
            uint32_t validMask1 = tree64[address * 3];
            uint32_t validMask2 = tree64[address * 3 + 1];
            //uint32_t childPointer = tree64[address * 3 + 2] & pointerMask;
            uint32_t childPointer = 0;
            uint32_t leafFlag = tree64[address * 3 + 2] & 0b1;
            if((validMask1 != 0 || validMask2 != 0) && leafFlag == 0){ // If it children and they are not leaves.
                childPointer = childCounter - (address) + 1;
                
                for(uint32_t i = 0; i < 64; i++){
                    uint64_t combinedValidMask = (uint64_t(validMask1) << 32) | uint64_t(validMask2);
                    if((combinedValidMask & (1ull << i)) != 0){
                        childCounter += 1;
                    }
                }
            } else { // If it has no children or it is a leaf.
                childPointer = 0;
            }
            if(childPointer > pointerMask){
                childPointerTooLarge = true;
            }
            tree64[address * 3 + 2] |= childPointer << 1;
        }

        if(childPointerTooLarge) {
            core::error("addPointersTree64: Child pointer too large (exceeds 31 bits)! Tree64 may be corrupted. Consider reducing data size or increasing resolution levels");
        }
    }

    std::vector<nodeStructureTree64> aggregateLevelTree64(std::vector<nodeStructureTree64> oldLevel, bool childParent = false) {
        std::vector<nodeStructureTree64> newLevel;

        for(size_t i = 0; i < oldLevel.size(); i++) { // Looping over verey voxel
            nodeStructureTree64& node = oldLevel[i];
            uint32_t newIndex = node.ZOrderIndex / 64;
            uint32_t relativeZOrder = node.ZOrderIndex % 64;
            uint64_t bitToSet = (1ull << (63 - relativeZOrder));
            bool parentExists = false;
            int parentIndex = -1;

            // Check the last node to see if it is the parent for this node.
            if(newLevel.size() > 0) {
                if (newLevel[newLevel.size()-1].ZOrderIndex == newIndex) {
                    parentExists = true;
                    parentIndex = newLevel.size()-1;
                }
            }

            if (parentExists) {
                // Update existing node
                if (newLevel.empty()) core::warn("New level is empty!");
                uint32_t firstMask = uint32_t((bitToSet >> 32) & 0b11111111111111111111111111111111);
                uint32_t secondMask = uint32_t((bitToSet) & 0b11111111111111111111111111111111);
                newLevel.back().mask1 |= firstMask;
                newLevel.back().mask2 |= secondMask;
            } else {
                // Create new node
                nodeStructureTree64 newNode = {0, 0, 0, newIndex};

                newNode.mask1 |= uint32_t((bitToSet >> 32) & 0b11111111111111111111111111111111);
                newNode.mask2 |= uint32_t((bitToSet) & 0b11111111111111111111111111111111);
                newNode.pointerAndLeafFlag = 0; // Clear leaf bit if needed
                if(childParent) newNode.pointerAndLeafFlag |= 0b1; // Set leaf mask if this node is the parent of leaf's
                newLevel.emplace_back(newNode);
            }
        }
    
        return newLevel;
    }

    std::vector<nodeStructureTree64> aggregateLevelFromVoxelGridTree64(VoxelGrid& oldLevel, bool childParent = false) {
        std::vector<nodeStructureTree64> newLevel;
        //newLevel.reserve(oldLevel.voxels.size());
        for(int i = oldLevel.voxels.size() - 1; i >= 0; i--) { // Looping over every voxel in reverse
            Voxel voxel = oldLevel.voxels[i];
            uint32_t newIndex = voxel.ZOrderPosition / 64;
            uint32_t relativeZOrder = (voxel.ZOrderPosition % 64);
            uint64_t bitToSet = (1ull << (63 - relativeZOrder));
            bool parentExists = false;

            // Check the last node to see if it is the parent for this node.
            if(newLevel.size() > 0) {
                if(newLevel[newLevel.size()-1].ZOrderIndex == newIndex) {
                    parentExists = true;
                }
            }

            if (parentExists) {
                 // Update existing node
                // We shift it right for the first one because leftmost bit is 0.
                newLevel.back().mask1 |= uint32_t((bitToSet >> 32u) & 0b11111111111111111111111111111111);
                newLevel.back().mask2 |= uint32_t((bitToSet) & 0b11111111111111111111111111111111);
           } else {
                // Create new node
                nodeStructureTree64 newNode = {0, 0, 0, newIndex};
                // Update existing node
                newNode.mask1 |= uint32_t((bitToSet >> 32u) & 0b11111111111111111111111111111111);
                newNode.mask2 |= uint32_t((bitToSet) & 0b11111111111111111111111111111111);

                newNode.pointerAndLeafFlag = 0; // Clear leaf bit if needed
                if(childParent) newNode.pointerAndLeafFlag |= 0b1; // Set leaf mask if this node is the parent of leaf's
                newLevel.emplace_back(newNode);
            }
        }
        return newLevel;
    }

    std::vector<uint32_t> createTree64(VoxelGrid& voxels, int gridResolution) {
        std::chrono::high_resolution_clock::time_point startWhole = std::chrono::high_resolution_clock::now();
        core::perf("createTree64: Starting tree64 generation with resolution {}x{}x{} ({} voxels total)", gridResolution, gridResolution, gridResolution, gridResolution * gridResolution * gridResolution);
        int levelsOfDepth = int(log10(gridResolution)/log10(4));
        std::vector<nodeStructureTree64> tree64;
        std::vector<nodeStructureTree64> levelInProgress;

        levelInProgress = aggregateLevelFromVoxelGridTree64(voxels, true);
        tree64 = levelInProgress; // Puts our data on the tree64
        std::cout << "Levels of depth: " << levelsOfDepth << std::endl;

        for(int i = 0; i < levelsOfDepth - 1; i++){ // Loops over all the levels of depth
            levelInProgress = aggregateLevelTree64(levelInProgress); // Agregates the previous level.

            for(size_t j = 0; j < levelInProgress.size(); j++){
                levelInProgress[j].pointerAndLeafFlag &= 0b11111111111111111111111111111110; // Removes the leaf flag from the node.
                tree64.emplace_back(levelInProgress[j]);
            }
        }

        // The tree64 is in reverse order. So we loop over it in reverse order when emplacing our uint32_t's to the actually tree64Simplified storing the serialized structure.
        std::vector<uint32_t> tree64Simplified;
        tree64Simplified.reserve(tree64.size()); // Reserve memory to avoid reallocations
        for (auto it = tree64.rbegin(); it != tree64.rend(); ++it) { // Use reverse iterator for efficiency
            tree64Simplified.emplace_back(it->mask1);
            tree64Simplified.emplace_back(it->mask2);
            tree64Simplified.emplace_back(it->pointerAndLeafFlag);
        }

        addPointersTree64(tree64Simplified);

        for (size_t i = 0; i < tree64Simplified.size(); i++) {
            //std::cout << std::bitset<32>(tree64Simplified[i]) << "|" << i % 3 << std::endl;
        }

        auto endWhole = std::chrono::high_resolution_clock::now();
        double elapsedWhole = std::chrono::duration<double, std::milli>(endWhole - startWhole).count();

        core::perf("createTree64: Completed tree-64 generation in {:.2f}ms for {} voxels", elapsedWhole, gridResolution * gridResolution * gridResolution);
        core::perf("createTree64: resolution={} voxels={} levels={}: {:.2f}ms", gridResolution, gridResolution * gridResolution * gridResolution, levelsOfDepth, elapsedWhole);

        return tree64Simplified;
    }

    std::vector<uint32_t> createVoxelTypeData(VoxelGrid& voxels) {
        std::vector<uint32_t> voxelTypeData;
        for(size_t i = 0; i < voxels.voxels.size(); i++){
            Voxel voxel = voxels.voxels[i];
            //core::debug("{} ZOrderIndex", i);
            voxelTypeData.emplace_back(voxel.ZOrderPosition);
            int R10 = (voxel.color.r)*4;
            int G10 = (voxel.color.g)*4;
            int B10 = (voxel.color.b)*4;
            float normalX = 0;
            float normalY = 0;
            float normalZ = 0;
            uint8_t normalXSign = 1;
            uint8_t normalYSign = 1;
            uint8_t normalZSign = 1;
            if(normalX < 0) normalXSign = 0;
            if(normalY < 0) normalYSign = 0;
            if(normalZ < 0) normalZSign = 0;
            int normalX9 = int(abs(normalX)*511) & 0x1FF;
            int normalY9 = int(abs(normalY)*511) & 0x1FF;
            int normalZ9 = int(abs(normalZ)*511) & 0x1FF;
            uint32_t SerializedColor = uint32_t(R10 << 20 | G10 << 10 | B10);
            uint32_t SerializedNormal = uint32_t((normalXSign << 29) | (normalX9 << 20) | (normalYSign << 19) | (normalY9 << 10) | (normalZSign << 9) | normalZ9);
            voxelTypeData.emplace_back(SerializedColor);
            voxelTypeData.emplace_back(SerializedNormal);
        }
        return voxelTypeData;
    }

    float createChunkScaleFromVoxelScaleAndResolution(float voxelScale, int resolutionPowerOf2) {
        return resolutionPowerOf2 * (voxelScale);    
    }

    Chunk createChunk(ChunkHeader chunkHeader) {
        Chunk chunk;
        chunk.header = chunkHeader;
        chunk.LOD = 0;
        return chunk;
    }

    Color unserializeColor(uint32_t serializedColor) {
        uint32_t R10 = (serializedColor >> 20) & 0x3FF; // 10 bits
        uint32_t G10 = (serializedColor >> 10) & 0x3FF; // 10 bits
        uint32_t B10 = serializedColor & 0x3FF;         // 10 bits
    
        projv::Color color;
        color.r = static_cast<uint8_t>(R10 / 4); // Convert back from 10-bit to 8-bit
        color.g = static_cast<uint8_t>(G10 / 4);
        color.b = static_cast<uint8_t>(B10 / 4);
    
        return color;
    }

    // ---- Brick Map Implementation ----

    std::unique_ptr<VoxelBrickMap> createVoxelBrickMap(const core::ivec3& brickDims) {
        auto map = std::make_unique<VoxelBrickMap>();
        map->brickDims = brickDims;
        map->totalBricks = static_cast<uint32_t>(brickDims.x * brickDims.y * brickDims.z);
        size_t maskWords = (static_cast<size_t>(map->totalBricks) + 63) / 64;
        map->brickMask.assign(maskWords, 0);
        map->bricks.resize(map->totalBricks);
        return map;
    }

    void brickMapSetVoxel(VoxelBrickMap& map, int x, int y, int z, uint8_t materialID) {
        core::ivec3 brickCoord = computeBrickCoord(x, y, z);
        core::ivec3 localPos = computeBrickLocalPos(x, y, z);

        // Check bounds
        if (brickCoord.x < 0 || brickCoord.y < 0 || brickCoord.z < 0 ||
            brickCoord.x >= map.brickDims.x ||
            brickCoord.y >= map.brickDims.y ||
            brickCoord.z >= map.brickDims.z) {
            return;
        }

        uint32_t brickZOrder = computeBrickZOrder(brickCoord, map.brickDims);
        uint32_t localZOrder = computeLocalZOrder(localPos);

        // Ensure brick exists
        if (!map.bricks[brickZOrder]) {
            map.bricks[brickZOrder] = std::make_unique<BrickData>();
            map.brickMask[brickZOrder / 64] |= (1ull << (brickZOrder % 64));
        }

        BrickData& brick = *map.bricks[brickZOrder];
        uint32_t row = localZOrder / 64;
        // Tree64 convention: bit 63 = Z-order position 0, bit 0 = Z-order position 63
        uint32_t bit = 63 - (localZOrder % 64);

        brick.mask[row] |= (1ull << bit);
        brick.materials[localZOrder] = materialID;
    }

    void brickMapClearVoxel(VoxelBrickMap& map, int x, int y, int z) {
        core::ivec3 brickCoord = computeBrickCoord(x, y, z);

        if (brickCoord.x < 0 || brickCoord.y < 0 || brickCoord.z < 0 ||
            brickCoord.x >= map.brickDims.x ||
            brickCoord.y >= map.brickDims.y ||
            brickCoord.z >= map.brickDims.z) {
            return;
        }

        uint32_t brickZOrder = computeBrickZOrder(brickCoord, map.brickDims);
        if (!map.bricks[brickZOrder]) return;

        core::ivec3 localPos = computeBrickLocalPos(x, y, z);
        uint32_t localZOrder = computeLocalZOrder(localPos);
        BrickData& brick = *map.bricks[brickZOrder];
        uint32_t row = localZOrder / 64;
        uint32_t bit = 63 - (localZOrder % 64);

        if (brick.mask[row] & (1ull << bit)) {
            brick.mask[row] &= ~(1ull << bit);
            brick.materials.erase(localZOrder);
        }
    }

    bool brickMapHasVoxel(const VoxelBrickMap& map, int x, int y, int z) {
        core::ivec3 brickCoord = computeBrickCoord(x, y, z);
        if (brickCoord.x < 0 || brickCoord.y < 0 || brickCoord.z < 0 ||
            brickCoord.x >= map.brickDims.x ||
            brickCoord.y >= map.brickDims.y ||
            brickCoord.z >= map.brickDims.z) {
            return false;
        }

        uint32_t brickZOrder = computeBrickZOrder(brickCoord, map.brickDims);
        if (!map.bricks[brickZOrder]) return false;

        core::ivec3 localPos = computeBrickLocalPos(x, y, z);
        uint32_t localZOrder = computeLocalZOrder(localPos);
        const BrickData& brick = *map.bricks[brickZOrder];
        uint32_t row = localZOrder / 64;
        uint32_t bit = 63 - (localZOrder % 64);

        return (brick.mask[row] & (1ull << bit)) != 0;
    }

    void brickMapFromVoxelTypeData(VoxelBrickMap& map,
                                    const std::vector<uint32_t>& voxelTypeData,
                                    ComponentRecord& comp) {
        size_t count = voxelTypeData.size() / 3;
        for (size_t i = 0; i < count; ++i) {
            uint32_t zorder = voxelTypeData[i * 3];
            uint32_t serializedColor = voxelTypeData[i * 3 + 1];

            core::ivec3 pos = reverseZOrderIndex(zorder);
            Color c = unserializeColor(serializedColor);
            uint32_t packed = packColor(c);
            uint8_t matID = internMaterial(comp, "", packed);
            brickMapSetVoxel(map, pos.x, pos.y, pos.z, matID);
        }
        if (count > 0) {
            map.defaultNormal = voxelTypeData[count * 3 - 1];
        }
    }

    std::vector<uint32_t> buildTree64FromBrickMap(const VoxelBrickMap& map, int resolution) {
        int levelsOfDepth = int(log10(resolution) / log10(4));
        std::vector<nodeStructureTree64> tree64;
        std::vector<nodeStructureTree64> levelInProgress;

        for (uint32_t bz = map.totalBricks; bz > 0; --bz) {
            uint32_t brickIdx = bz - 1;
            if (!map.bricks[brickIdx]) continue;

            const BrickData& brick = *map.bricks[brickIdx];
            uint32_t baseZOrder = brickIdx * BRICK_MASK_ROWS;

            for (uint32_t row = BRICK_MASK_ROWS; row > 0; --row) {
                uint32_t r = row - 1;
                uint64_t rowBits = brick.mask[r];
                if (rowBits == 0) continue;

                nodeStructureTree64 node;
                node.mask1 = uint32_t(rowBits >> 32);
                node.mask2 = uint32_t(rowBits & 0xFFFFFFFF);
                node.pointerAndLeafFlag = 1;
                node.ZOrderIndex = baseZOrder + r;
                levelInProgress.emplace_back(node);
            }
        }

        tree64 = levelInProgress;

        for (int i = 0; i < levelsOfDepth - 1; ++i) {
            levelInProgress = aggregateLevelTree64(levelInProgress);
            for (size_t j = 0; j < levelInProgress.size(); ++j) {
                levelInProgress[j].pointerAndLeafFlag &= 0xFFFFFFFE;
                tree64.emplace_back(levelInProgress[j]);
            }
        }

        std::vector<uint32_t> tree64Simplified;
        tree64Simplified.reserve(tree64.size());
        for (auto it = tree64.rbegin(); it != tree64.rend(); ++it) {
            tree64Simplified.emplace_back(it->mask1);
            tree64Simplified.emplace_back(it->mask2);
            tree64Simplified.emplace_back(it->pointerAndLeafFlag);
        }

        addPointersTree64(tree64Simplified);
        return tree64Simplified;
    }

    void updateChunkFromBrickMap(Chunk& chunk, const VoxelBrickMap& map) {
        auto t0 = std::chrono::high_resolution_clock::now();

        int resolution = chunk.header.resolution;
        if (resolution == 0) {
            resolution = static_cast<int>(map.brickDims.x * BRICK_SIZE);
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        chunk.geometryData = buildTree64FromBrickMap(map, resolution);
        auto t2 = std::chrono::high_resolution_clock::now();
        chunk.LOD = 0;

        double treeMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
        double totalMs = std::chrono::duration<double, std::milli>(t2 - t0).count();
        core::perf("updateChunkFromBrickMap: tree64={:.2f}ms total={:.2}ms",
                   treeMs, totalMs);
    }

    std::unique_ptr<VoxelBrickMap> cloneBrickMap(const VoxelBrickMap& src) {
        auto dst = std::make_unique<VoxelBrickMap>();
        dst->brickDims = src.brickDims;
        dst->totalBricks = src.totalBricks;
        dst->brickMask = src.brickMask;
        dst->bricks.resize(src.bricks.size());
        dst->defaultNormal = src.defaultNormal;

        for (size_t i = 0; i < src.bricks.size(); ++i) {
            if (src.bricks[i]) {
                auto brickClone = std::make_unique<BrickData>();
                std::copy(std::begin(src.bricks[i]->mask),
                          std::end(src.bricks[i]->mask),
                          std::begin(brickClone->mask));
                brickClone->materials = src.bricks[i]->materials;
                dst->bricks[i] = std::move(brickClone);
            }
        }

        return dst;
    }
}
