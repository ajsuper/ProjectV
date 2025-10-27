#include "utils/lod.h" // Ensure RuntimeChunkData is included

namespace projv::utils {
    void updateLOD(Chunk& chunk, uint32_t targetLOD, const std::string& sceneFilePath, bool forceReload) {
        auto start = std::chrono::high_resolution_clock::now();
        core::info("[updateLOD] Updating LOD of chunk {} to {}...", chunk.header.chunkID, targetLOD);

        // Exits if the chunk is already at the targetLOD and forceReload is false.
        if (chunk.LOD == targetLOD && !forceReload) {
            core::warn("[updateLOD] Chunk already at target LOD.");
            return;
        }

        if (targetLOD == 0) {
            chunk = loadChunkFromDisk(sceneFilePath, chunk.header);
            return;
        }

        // Loads the chunk if the targetLOD is a higher resolution (lower value) than the current LOD.
        Chunk chunkToBeChanged;
        if (chunk.LOD > targetLOD || forceReload) {
            chunkToBeChanged = loadChunkFromDisk(sceneFilePath, chunk.header);
        } else {
            chunkToBeChanged = chunk;
        }

        if (chunkToBeChanged.geometryData.empty()) {
            core::error("[updateLOD] Chunk geometry data is empty. Cannot update LOD.");
            chunkToBeChanged.LOD = targetLOD;
            chunk = chunkToBeChanged;
            return;
        }

        // Traverses the octree to find the parents of the leaves and removes them.
        int depthToTheParentsOfTheLeaves = log2(chunkToBeChanged.header.resolution / pow(2, targetLOD));
        int parentsOfTheParentsOfTheLeavesStartIndex = 0;
        int parentsOfTheLeavesStartIndex;
        int currentNodeIndex = 0;
        uint32_t currentNodeData = chunkToBeChanged.geometryData[currentNodeIndex];
        for (int i = 0; i < depthToTheParentsOfTheLeaves; i++) { // Tree traversal to find the parents of the leaves.
            int currentNodePointer = (currentNodeData >> 9) & 0b11111111111111111111111;
            currentNodeIndex += currentNodePointer;
            currentNodeData = chunkToBeChanged.geometryData[currentNodeIndex];
            if (i < depthToTheParentsOfTheLeaves - 1) {
                parentsOfTheParentsOfTheLeavesStartIndex = currentNodeIndex;
            }
        }

        // Makes the previous leaf grandparents into leaf parents.
        parentsOfTheLeavesStartIndex = currentNodeIndex;
        chunkToBeChanged.geometryData.erase(chunkToBeChanged.geometryData.begin() + parentsOfTheLeavesStartIndex, chunkToBeChanged.geometryData.end()); // Deletes the parents of the leaves
        for (size_t i = parentsOfTheParentsOfTheLeavesStartIndex; i < chunkToBeChanged.geometryData.size(); i++) { // Loops over the parents of the parents of the leaves
            uint32_t currentNodeData = chunkToBeChanged.geometryData[i];
            currentNodeData = currentNodeData & 0b111111111; // Clears the child pointer
            currentNodeData = currentNodeData | 0b1; // Sets the leaf flag to 1.
            chunkToBeChanged.geometryData[i] = currentNodeData;
        }

        // Scales the voxel type data down to the new resolution.
        uint32_t lastZOrder = 0;
        std::vector<uint32_t> newVoxelTypeData;
        int resolutionScale = pow(2, targetLOD - chunkToBeChanged.LOD);
        for (size_t i = 0; i < chunkToBeChanged.voxelTypeData.size(); i += 3) {
            chunkToBeChanged.voxelTypeData[i] /= (resolutionScale * resolutionScale * resolutionScale);

            uint32_t currentVoxelTypeDataZOrder = chunkToBeChanged.voxelTypeData[i];
            if (currentVoxelTypeDataZOrder == lastZOrder && i != 0) {
                continue;
            } else {
                newVoxelTypeData.push_back(currentVoxelTypeDataZOrder);
                newVoxelTypeData.push_back(chunkToBeChanged.voxelTypeData[i + 1]);
                newVoxelTypeData.push_back(chunkToBeChanged.voxelTypeData[i + 2]);
            }
            lastZOrder = currentVoxelTypeDataZOrder;
        }

        // Updates the chunk with the new data
        chunkToBeChanged.voxelTypeData = newVoxelTypeData;
        chunkToBeChanged.LOD = targetLOD;
        chunk = chunkToBeChanged;

        auto end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
        core::info("[updateLOD] Updated in {} ms", elapsed);

        return;
    }
}
