#include "graphics/scene.h"

namespace projv::graphics {
    void passSceneToOpenGL(Scene& sceneToRender) {
        std::vector<GPUChunkHeader> chunkHeaders;
        std::vector<uint32_t> serializedGeometry;
        std::vector<uint32_t> serializedVoxelTypeData;
                    
        chunkHeaders.reserve(sceneToRender.chunks.size());
        size_t totalGeometrySize = 0;
        size_t totalVoxelTypeDataSize = 0;
    
        // Precompute total sizes to reserve memory
        for (const auto &chunk : sceneToRender.chunks) {
            totalGeometrySize += chunk.geometryData.size();
            totalVoxelTypeDataSize += chunk.voxelTypeData.size();
        }
        serializedGeometry.reserve(totalGeometrySize);
        serializedVoxelTypeData.reserve(totalVoxelTypeDataSize);
    
        // Gather data and compute chunk header indices
        for (const auto &chunk : sceneToRender.chunks) {
            if(chunk.geometryData.empty() || chunk.voxelTypeData.empty()) {
                core::warn("Chunk {} has empty geometry or voxel type data. Skipping.", chunk.header.chunkID);
                continue; // Skip empty chunks
            }
            GPUChunkHeader shaderChunkHeader;
            shaderChunkHeader.positionX = chunk.header.position.x;
            shaderChunkHeader.positionY = chunk.header.position.y;
            shaderChunkHeader.positionZ = chunk.header.position.z;
            shaderChunkHeader.scale = chunk.header.scale;
            shaderChunkHeader.resolution = chunk.header.resolution / pow(2, chunk.LOD);
            shaderChunkHeader.geometryStartIndex = serializedGeometry.size();
            shaderChunkHeader.voxelTypeDataStartIndex = serializedVoxelTypeData.size();
    
            serializedGeometry.insert(serializedGeometry.end(), chunk.geometryData.begin(), chunk.geometryData.end());
            serializedVoxelTypeData.insert(serializedVoxelTypeData.end(), chunk.voxelTypeData.begin(), chunk.voxelTypeData.end());
    
            shaderChunkHeader.geometryEndIndex = serializedGeometry.size();
            shaderChunkHeader.voxelTypeDataEndIndex = serializedVoxelTypeData.size();
    
            chunkHeaders.emplace_back(shaderChunkHeader);
        }
        
        // Use static variables for persistent mapped buffers and capacity tracking.
        static GLuint chunkHeaderSSBO = 0, geometrySSBO = 0, voxelTypeDataSSBO = 0;
        static void* chunkHeaderMappedPtr = nullptr;
        static void* geometryMappedPtr = nullptr;
        static void* voxelTypeDataMappedPtr = nullptr;
        static size_t chunkHeaderCapacity = 0;
        static size_t geometryCapacity = 0;
        static size_t voxelTypeDataCapacity = 0;
        
        // Define persistent mapping access flags.
        const GLbitfield storageFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        
        // Helper lambda to update or allocate a buffer with persistent mapping.
        auto updateBuffer = [&](GLuint &ssbo, void* &mappedPtr, size_t &capacity, size_t requiredSize,
                                  GLenum bindingPoint, const void* data) {
            if (ssbo == 0 || capacity < requiredSize) {
                core::info("Resizing SSBO to accommodate {} bytes.", requiredSize);
                // If already created but not large enough, delete it.
                if (ssbo != 0) {
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
                    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);  // Unmap before deletion.
                    glDeleteBuffers(1, &ssbo);
                }
                glGenBuffers(1, &ssbo);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
                capacity = requiredSize * 1.5;
                // Allocate persistent storage once with no initial data.
                glBufferStorage(GL_SHADER_STORAGE_BUFFER, capacity, nullptr, storageFlags);
                // Map the entire buffer persistently.
                mappedPtr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, capacity, storageFlags);
                if (!mappedPtr) {
                    core::error("Failed to map buffer persistently!");
                    // Handle error appropriately.
                }
            } else {
                // Bind if no reallocation is needed.
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
            }
            // Update the buffer memory using memcpy.
            std::memcpy(mappedPtr, data, requiredSize);
            // Bind the buffer to the desired binding point.
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, bindingPoint, ssbo);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        };
        
        // Calculate sizes (in bytes) for each buffer.
        size_t chunkHeaderSize = chunkHeaders.size() * sizeof(GPUChunkHeader);
        size_t geometrySize = serializedGeometry.size() * sizeof(uint32_t);
        size_t voxelTypeDataSize = serializedVoxelTypeData.size() * sizeof(uint32_t);
        
        // Update each SSBO with the new data using persistent mapping.
        updateBuffer(chunkHeaderSSBO, chunkHeaderMappedPtr, chunkHeaderCapacity,
                    chunkHeaderSize, 3, chunkHeaders.data());
        updateBuffer(geometrySSBO, geometryMappedPtr, geometryCapacity,
                     geometrySize, 4, serializedGeometry.data());
        updateBuffer(voxelTypeDataSSBO, voxelTypeDataMappedPtr, voxelTypeDataCapacity,
                     voxelTypeDataSize, 5, serializedVoxelTypeData.data());
    }
}