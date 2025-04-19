#include "utils/voxel_management.h"

namespace projv::utils {
    VoxelGrid createVoxelGrid() {
        // Initialize the 3D vector with `voxel` instances.
        VoxelGrid voxels;
        voxels.max = 0;
        return voxels;
    }

    // Generate the relative child pointers and combine them with a partially constructed octree containing only the masks.
    std::vector<uint32_t> addPointers(std::vector<uint32_t>& octree){
        // TODO: Remove the return of the reference and make it only modify the octree directly.
        uint16_t vldMaskMask = 0b111111110;
        uint16_t lefMaskMask = 0b000000001;
        int parentCount = 0;
        int childCounter = 0;
        for(int address = 0; address < octree.size(); address++){
            uint32_t* current = &octree[address];
            uint32_t childPointer;
            uint8_t validMask = (*current & vldMaskMask) >> 1;
            uint8_t leafMask = (*current & lefMaskMask);
            if(validMask != 0 && leafMask == 0){ 
                childPointer = childCounter-address+1;    
                
                parentCount += 1;
                for(uint8_t i = 0; i < 8; i++){
                    if((validMask & (1 << i)) != 0){
                        childCounter += 1;
                    }
                }
            } else {
                childPointer = 0;
            }
            if(childPointer >= 0b11111111111111111111111){
                std::cerr << "[addPointers] Child pointer too large! Likely too much data in octree or corrupt octree" << std::bitset<32>(childPointer) << std::endl;
            }
            octree[address] = (*current & 0b111111111) | (childPointer << 9);
        }
        return octree;
    }

    std::vector<nodeStructure> agregateLevel(std::vector<nodeStructure>& oldLevel, bool childParent = false) {
        std::vector<nodeStructure> newLevel;
        std::unordered_map<int, int> indexMap; // Maps newIndex to index in newLevel
    
        for (const auto& node : oldLevel) {
            int newIndex = node.ZOrderIndex / 8;
            uint32_t relativeZOrder = node.ZOrderIndex % 8;
            uint32_t bitToSet = (1 << (8 - relativeZOrder));
    
            if (indexMap.find(newIndex) != indexMap.end()) {
                // Update existing node
                newLevel[indexMap[newIndex]].standardNode |= bitToSet;
            } else {
                // Create new node
                nodeStructure newNode = {0, newIndex};
                newNode.standardNode |= bitToSet;
                newNode.standardNode &= 0b111111110; // Clear leaf bit if needed
                if(childParent) newNode.standardNode |= 0b1;
                newLevel.push_back(newNode);
                indexMap[newIndex] = newLevel.size() - 1;
            }
        }
    
        return newLevel;
    }

    std::vector<nodeStructure> convertVoxelsToGeometry(VoxelGrid& voxels) {
        std::vector<nodeStructure> nodes;
        for(int i = 0; i < voxels.voxels.size(); i++) {
            nodeStructure node;
            node.ZOrderIndex = voxels.voxels[i].ZOrderPosition;
            auto position = reverseZOrderIndex(node.ZOrderIndex, 15);
            nodes.emplace_back(node);
        }
        return nodes;
    }
    
    std::vector<uint32_t> createOctree(VoxelGrid& voxels, int voxelWholeResolution){
        std::chrono::high_resolution_clock::time_point startWhole = std::chrono::high_resolution_clock::now();
        std::cout << "[createOctree] Octree generation started with size of " << voxelWholeResolution << std::endl;
        int levelsOfDepth = int(log2(voxelWholeResolution));
        std::vector<nodeStructure> octree;
        std::vector<nodeStructure> levelInProgress;

        levelInProgress = convertVoxelsToGeometry(voxels); // Builds mask for the lowest resolution. GOOD
        std::reverse(levelInProgress.begin(), levelInProgress.end());
        levelInProgress = agregateLevel(levelInProgress, true);

        for(int i = 0; i < levelInProgress.size(); i++){ // Puts it on the octree in reverse order since were starting at the lowest level.
            octree.push_back(levelInProgress[i]); // Puts our data on the octree
        }

        for(int i = 0; i < levelsOfDepth - 1; i++){ // Loops over all the levels of depth
            levelInProgress = agregateLevel(levelInProgress); // Agregates the previous level. GOOD
    
            for(int j = 0; j < levelInProgress.size(); j++){
                levelInProgress[j].standardNode &= 0b111111110; // Removes the leaf flag from the node.
                octree.push_back(levelInProgress[j]);
            }
        }

        std::vector<uint32_t> octreeSimplified;
        for(int i = octree.size() - 1; i >= 0; i--){
            octreeSimplified.push_back(octree[i].standardNode);
        }

        octreeSimplified = addPointers(octreeSimplified);
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end - startWhole).count();
        std::cerr << "[createOctree] Octree finished in: " << elapsed << " ms\n" << std::endl;
        return octreeSimplified;
    }

    std::vector<uint32_t> createVoxelTypeData(VoxelGrid& voxels) {
        std::vector<uint32_t> voxelTypeData;
        for(int i = 0; i < voxels.voxels.size(); i++){
            Voxel voxel = voxels.voxels[i];
            //std::cout << i << "ZOrderIndex" << std::endl;
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

    void addVoxelToVoxelGrid(VoxelGrid& voxels, std::array<int, 3> position, Color color) {
        Voxel voxel;
        voxel.ZOrderPosition = createZOrderIndex(
            std::clamp(position[0], 1, 511),
            std::clamp(position[1], 1, 511),
            std::clamp(position[2], 1, 511),
            15
        );
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
        for(int i = 0; i < voxelBatch.size(); i++){
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

    void addVoxelToBatch(Voxel voxel, VoxelBatch& voxelBatch) {
        voxelBatch.emplace_back(voxel);
        return;
    }
}