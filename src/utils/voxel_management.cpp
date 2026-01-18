#include "utils/voxel_management.h"

namespace projv::utils {
    VoxelGrid createVoxelGrid() {
        // Initialize the 3D vector with `voxel` instances.
        VoxelGrid voxels;
        voxels.max = 0;
        return voxels;
    }

    // Generate the relative child pointers and combine them with a partially constructed octree containing only the masks.
    void addPointers(std::vector<uint32_t>& octree) {
        uint16_t vldMaskMask = 0b111111110;
        uint16_t lefMaskMask = 0b000000001;
        int childCounter = 0;
        bool childPointerTooLarge = false;
        for(size_t address = 0; address < octree.size(); address++){
            uint32_t* current = &octree[address];
            uint32_t childPointer;
            uint8_t validMask = (*current & vldMaskMask) >> 1;
            uint8_t leafMask = (*current & lefMaskMask);
            if(validMask != 0 && leafMask == 0){ 
                childPointer = childCounter-address+1;    
                
                for(uint8_t i = 0; i < 8; i++){
                    if((validMask & (1 << i)) != 0){
                        childCounter += 1;
                    }
                }
            } else {
                childPointer = 0;
            }
            if(childPointer >= 0b11111111111111111111111){
                childPointerTooLarge = true;
            }
            octree[address] = (*current & 0b111111111) | (childPointer << 9);
        }

        if(childPointerTooLarge) {
            core::error("addPointers: Child pointer too large (exceeds 21 bits)! Octree may be corrupted. Consider reducing data size or increasing resolution levels");
        }
    }

    std::vector<nodeStructure> agregateLevel(std::vector<nodeStructure>& oldLevel, bool childParent = false) {
        std::vector<nodeStructure> newLevel;

        for(size_t i = 0; i < oldLevel.size(); i++) { // Looping over verey voxel
            nodeStructure& node = oldLevel[i];
            uint32_t newIndex = node.ZOrderIndex / 8;
            uint32_t relativeZOrder = node.ZOrderIndex % 8;
            uint32_t bitToSet = (1 << (8 - relativeZOrder));
            bool parentExists = false;
            int parentIndex = -1;

            // Check the last node to see if it is the parent for this node.
            if(newLevel.size() > 0 && newLevel[newLevel.size()-1].ZOrderIndex == newIndex) {
                parentExists = true;
                parentIndex = newLevel.size()-1;
            }

            if (parentExists) {
                // Update existing node
                newLevel[parentIndex].standardNode |= bitToSet;
            } else {
                // Create new node
                nodeStructure newNode = {0, newIndex};
                newNode.standardNode |= bitToSet;
                newNode.standardNode &= 0b111111110; // Clear leaf bit if needed
                if(childParent) newNode.standardNode |= 0b1; // Set leaf mask if this node is the parent of leaf's
                newLevel.push_back(newNode);
            }
        }
    
        return newLevel;
    }

    std::vector<nodeStructure> agregateLevelFromVoxelGrid(VoxelGrid& oldLevel, bool childParent = false) {
        std::vector<nodeStructure> newLevel;
        newLevel.reserve(oldLevel.voxels.size());
        for(int i = oldLevel.voxels.size() - 1; i >= 0; i--) { // Looping over every voxel in reverse
            Voxel& voxel = oldLevel.voxels[i];
            uint32_t newIndex = voxel.ZOrderPosition / 8;
            uint32_t relativeZOrder = voxel.ZOrderPosition % 8;
            uint32_t bitToSet = (1 << (8 - relativeZOrder));
            bool parentExists = false;
            int parentIndex = -1;

            // Check the last node to see if it is the parent for this node.
            if(newLevel.size() > 0 && newLevel[newLevel.size()-1].ZOrderIndex == newIndex) {
                parentExists = true;
                parentIndex = newLevel.size()-1;
            }

            if (parentExists) {
                // Update existing node
                newLevel[parentIndex].standardNode |= bitToSet;
            } else {
                // Create new node
                nodeStructure newNode = {0, newIndex};
                newNode.standardNode |= bitToSet;
                newNode.standardNode &= 0b111111110; // Clear leaf bit if needed
                if(childParent) newNode.standardNode |= 0b1; // Set leaf mask if this node is the parent of leaf's
                newLevel.emplace_back(newNode);
            }
        }
        return newLevel;
    }

    
    std::vector<uint32_t> createOctree(VoxelGrid& voxels, int voxelWholeResolution){
        std::chrono::high_resolution_clock::time_point startWhole = std::chrono::high_resolution_clock::now();
        core::info("createOctree: Starting octree generation with resolution {}x{}x{} ({} voxels total)", voxelWholeResolution, voxelWholeResolution, voxelWholeResolution, voxelWholeResolution * voxelWholeResolution * voxelWholeResolution);
        int levelsOfDepth = int(log2(voxelWholeResolution));
        std::vector<nodeStructure> octree;
        std::vector<nodeStructure> levelInProgress;

        levelInProgress = agregateLevelFromVoxelGrid(voxels, true);
        octree = levelInProgress; // Puts our data on the octree

        for(int i = 0; i < levelsOfDepth - 1; i++){ // Loops over all the levels of depth
            levelInProgress = agregateLevel(levelInProgress); // Agregates the previous level.

            for(size_t j = 0; j < levelInProgress.size(); j++){
                levelInProgress[j].standardNode &= 0b111111110; // Removes the leaf flag from the node.
                octree.emplace_back(levelInProgress[j]);
            }
        }

        std::vector<uint32_t> octreeSimplified;
        octreeSimplified.reserve(octree.size()); // Reserve memory to avoid reallocations
        for (auto it = octree.rbegin(); it != octree.rend(); ++it) { // Use reverse iterator for efficiency
            octreeSimplified.emplace_back(it->standardNode);
        }

        addPointers(octreeSimplified);

        auto endWhole = std::chrono::high_resolution_clock::now();
        double elapsedWhole = std::chrono::duration<double, std::milli>(endWhole - startWhole).count();

        core::info("createOctree: Completed octree generation in {:.2f}ms for {} voxels", elapsedWhole, voxelWholeResolution * voxelWholeResolution * voxelWholeResolution);

        return octreeSimplified;
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

    void addVoxelToVoxelGrid(VoxelGrid& voxels, core::ivec3 position, Color color) {
        Voxel voxel;
        voxel.ZOrderPosition = createZOrderIndex(position);
        voxel.color = color;
    
        int beginIndex = 0;
        int endIndex = voxels.voxels.size();
    
        while (beginIndex < endIndex) {
            int middleIndex = (beginIndex + endIndex) / 2;
            uint64_t midZ = voxels.voxels[middleIndex].ZOrderPosition;
    
            if (midZ < voxel.ZOrderPosition) {
                beginIndex = middleIndex + 1;
            } else if (midZ > voxel.ZOrderPosition) {
                endIndex = middleIndex;
            } else {
                // Match found: update color and exit
                voxels.voxels[middleIndex].color = voxel.color;
                return;
            }
        }
    
        // Not found: insert at correct position to maintain sort
        voxels.voxels.insert(voxels.voxels.begin() + beginIndex, voxel);
    }

    void addVoxelBatchToVoxelGrid(VoxelGrid& voxels, VoxelBatch& voxelBatch) {
        for(size_t i = 0; i < voxelBatch.size(); i++) {
            voxels.voxels.emplace_back(voxelBatch[i]);
        }

        std::sort(voxels.voxels.begin(), voxels.voxels.end(), [](const Voxel& a, const Voxel& b) {
            return a.ZOrderPosition < b.ZOrderPosition;
        });
    }

    VoxelBatch createVoxelBatch() {
        VoxelBatch voxelBatch;
        return voxelBatch;
    }

    float createChunkScaleFromVoxelScaleAndResolution(float voxelScale, int resolutionPowerOf2) {
        return resolutionPowerOf2 * (voxelScale * 0.0390625); // * 0.0390625 is to adjust it so that a voxel size of 1 and a resolution of 512 results in a chunk size of roughly 20. This is done for precision reasons.
    }

    ChunkHeader createChunkHeader(std::vector<ChunkHeader>& sceneChunkHeaders, core::vec3 position, float voxelScale, int resolutionPowOf2) {
        if(resolutionPowOf2 > 512) {
            core::warn("createChunkHeader: Resolution {} exceeds recommended maximum of 256 (may impact performance)", resolutionPowOf2);
        }
        int accuratePowerOf2 = std::pow(2, std::ceil(std::log2(resolutionPowOf2)));
        if(core::fract(log2(resolutionPowOf2)) != 0) {
            core::warn("createChunkHeader: Resolution {} is not power of 2, rounding up to {}", resolutionPowOf2, accuratePowerOf2);
        }
        float chunkScale = createChunkScaleFromVoxelScaleAndResolution(voxelScale, resolutionPowOf2);
        if(chunkScale < 3) {
            core::warn("createChunkHeader: Chunk scale {:.2f} is below 3.0 (may cause floating point precision issues)", chunkScale);
        }

        projv::ChunkHeader chunkHeader;
        chunkHeader.position = position;
        chunkHeader.scale = chunkScale;
        chunkHeader.voxelScale = voxelScale;
        chunkHeader.resolution = accuratePowerOf2;
        
        // Convert our existing chunk id's into an unorderd_set for fast look up to see if the newly generated random ID exists or not.
        std::unordered_set<uint32_t> existingIDs;
        for(size_t i = 0; i < sceneChunkHeaders.size(); i++) {
            existingIDs.insert(sceneChunkHeaders[i].chunkID);
        }

        // Generate a unique ID.
        uint32_t randomID = std::rand();
        while(existingIDs.find(randomID) != existingIDs.end()) {
            chunkHeader.chunkID = randomID;
            randomID = std::rand();
        }
        return chunkHeader;
    }

    Chunk createChunk(ChunkHeader chunkHeader) {
        Chunk chunk;
        chunk.header = chunkHeader;
        chunk.LOD = 0;
        return chunk;
    }


    void addVoxelToVoxelBatch(Voxel& voxel, VoxelBatch& voxelBatch) {
        voxelBatch.emplace_back(voxel);
        return;
    }

    void moveVoxelBatchToChunk(VoxelBatch& voxelBatch, Chunk& chunk) {
        chunk.chunkQueue = std::move(voxelBatch);
        return;
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

    VoxelBatch getChunkVoxelBatch(Chunk& chunk, bool convertCompressedData) {
        if(!convertCompressedData) {
            return chunk.chunkQueue;
        }

        VoxelBatch decompressedVoxels;
        size_t count = chunk.voxelTypeData.size() / 3;
        decompressedVoxels.resize(count);
        
        core::info("getChunkVoxelBatch: Decompressing {} voxels from chunk", count);
        for (size_t i = 0; i < count; ++i) {
            uint32_t ZOrderPosition = chunk.voxelTypeData[i * 3];
            uint32_t SerializedColor = chunk.voxelTypeData[i * 3 + 1];
            //uint32_t SerializedNormal = chunk.voxelTypeData[i * 3 + 2];
    
            Voxel voxel;
            voxel.ZOrderPosition = ZOrderPosition;
    
            // Deserialize color.
            voxel.color = unserializeColor(SerializedColor);
        
            decompressedVoxels[i] = voxel;
        }

        core::info("getChunkVoxelBatch: Decompressed {} voxels from chunk storage", decompressedVoxels.size());

        return decompressedVoxels;
    }
    
    void addVoxelBatchAToVoxelBatchB(VoxelBatch& voxelBatchA, VoxelBatch& voxelBatchB, core::ivec3 voxelBatchAPosition) {
        for(size_t i = 0; i < voxelBatchA.size(); i++) {
            core::ivec3 currentPosition = reverseZOrderIndex(voxelBatchA[i].ZOrderPosition);
            core::ivec3 newPosition = currentPosition + voxelBatchAPosition;
            newPosition.x = std::clamp(newPosition.x, 0, 511);
            newPosition.y = std::clamp(newPosition.y, 0, 511);
            newPosition.z = std::clamp(newPosition.z, 0, 511);
            uint64_t newZOrderPosition = createZOrderIndex(newPosition);

            projv::Voxel copiedVoxel = voxelBatchA[i];
            copiedVoxel.ZOrderPosition = newZOrderPosition;
            voxelBatchB.emplace_back(copiedVoxel);
        }
    }

    void sortVoxelBatch(VoxelBatch& voxelBatch) {
        std::sort(voxelBatch.begin(), voxelBatch.end(), [](const Voxel& a, const Voxel& b) {
            return a.ZOrderPosition < b.ZOrderPosition;
        });
        return;
    }

    VoxelGrid createVoxelGridFromChunksQueue(const Chunk& chunk) {
        VoxelGrid voxelGrid = {};

        // Copy chunkQueue to work on it directly
        std::vector<Voxel> voxels = chunk.chunkQueue;

        // Sort by ZOrderPosition (we don't care which duplicate is kept)
        std::sort(voxels.begin(), voxels.end(), [](const Voxel& a, const Voxel& b) {
            return a.ZOrderPosition < b.ZOrderPosition;
        });

        // Remove duplicates: keep first occurrence after sorting
        auto last = std::unique(voxels.begin(), voxels.end(), [](const Voxel& a, const Voxel& b) {
            return a.ZOrderPosition == b.ZOrderPosition;
        });
        voxels.erase(last, voxels.end());

        // Assign to VoxelGrid
        voxelGrid.voxels = std::move(voxels);
        return voxelGrid;
    }

    void updateChunkFromItsVoxelBatch(Chunk& chunk, bool clearBatch) {
        // Get farthest voxel.
        auto start = std::chrono::high_resolution_clock::now();
        VoxelGrid voxelGrid = createVoxelGridFromChunksQueue(chunk);
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
        core::info("createVoxelGridFromChunksQueue: Processed chunk queue in {:.2f}ms", elapsed);

        // Compute resolution.
        Voxel farthestVoxel = voxelGrid.voxels[voxelGrid.voxels.size() - 1];
        core::ivec3 position = reverseZOrderIndex(farthestVoxel.ZOrderPosition);
        int farthestCoordinate = std::max({position.x, position.y, position.z});
        int resolutionToTheNearestPowOfTwo = std::pow(2, std::ceil(std::log2(farthestCoordinate + 1)));
        if(resolutionToTheNearestPowOfTwo > 256) {
            core::warn("updateChunkFromItsVoxelBatch: Chunk {} resolution {} exceeds recommended 256 (voxel positions too large)", chunk.header.chunkID, chunk.header.resolution);
        }

        // Update the chunk.
        chunk.geometryData = createOctree(voxelGrid, resolutionToTheNearestPowOfTwo);
        chunk.voxelTypeData = createVoxelTypeData(voxelGrid);
        chunk.LOD = 0;

        chunk.header.resolution = resolutionToTheNearestPowOfTwo;
        chunk.header.scale = createChunkScaleFromVoxelScaleAndResolution(chunk.header.voxelScale, resolutionToTheNearestPowOfTwo);

        // Clear our queue.
        if(clearBatch) {
            VoxelBatch emptyChunkQueue;
            chunk.chunkQueue = emptyChunkQueue;
        }
        return;
    }

    void removeVoxelBatchAFromVoxelBatchB(VoxelBatch& voxelBatchA, VoxelBatch& voxelBatchB, core::ivec3 positionOffset) {
        auto start = std::chrono::high_resolution_clock::now();
    
        constexpr size_t ZORDER_RANGE = 512 * 512 * 512; // 134217728
        std::vector<bool> zOrderMask(ZORDER_RANGE, false); // Bitmask for A's Z-order positions
    
        // Preprocessing voxelBatchA
        auto loopStart = std::chrono::high_resolution_clock::now();
    
        for (const auto& voxelA : voxelBatchA) {
            core::ivec3 adjustedPosition = reverseZOrderIndex(voxelA.ZOrderPosition);
            uint32_t adjustedZOrderIndex = createZOrderIndex(adjustedPosition + positionOffset);
            zOrderMask[adjustedZOrderIndex] = true; // Mark this index as to-remove
        }
    
        auto loopEnd = std::chrono::high_resolution_clock::now();
        double loopElapsed = std::chrono::duration<double, std::milli>(loopEnd - loopStart).count();
        core::info("Time spent processing voxelBatchA: " + std::to_string(loopElapsed) + "ms");
    
        // filtering voxelBatchB
        auto filterStart = std::chrono::high_resolution_clock::now();
    
        size_t index = 0;
        for (size_t i = 0; i < voxelBatchB.size(); ++i) {
            uint32_t zIndex = voxelBatchB[i].ZOrderPosition;
            if (zIndex >= ZORDER_RANGE || !zOrderMask[zIndex]) {
                voxelBatchB[index++] = voxelBatchB[i];
            }
        }
        voxelBatchB.resize(index);
    
        auto filterEnd = std::chrono::high_resolution_clock::now();
        double filterElapsed = std::chrono::duration<double, std::milli>(filterEnd - filterStart).count();
        core::info("Time spent filtering voxelBatchB: " + std::to_string(filterElapsed) + "ms");
    
        auto end = std::chrono::high_resolution_clock::now();
        double totalElapsed = std::chrono::duration<double, std::milli>(end - start).count();
        core::info("Function: removeVoxelBatchAFromVoxelBatchB. Time taken: " + std::to_string(totalElapsed) + "ms");
    }
    
    Voxel createVoxel(Color color, core::ivec3 position) {
        Voxel voxel;
        voxel.color = color;
        voxel.ZOrderPosition = createZOrderIndex(position);
        return voxel;
    }
}
