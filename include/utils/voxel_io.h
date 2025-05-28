#ifndef PROJECTV_VOXEL_IO_H
#define PROJECTV_VOXEL_IO_H

#include <vector>
#include <string>
#include <stdint.h>
#include <fstream>
#include <iostream>
#include <filesystem>

#include "core/log.h"
#include "nlohmann/json.hpp"
#include "data_structures/scene.h"
#include "voxel_math.h"

namespace projv::utils {
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
    void writeHeadersJSON(const std::vector<ChunkHeader>& chunkHeaders, const std::string& fileDirectory);

    /**
     * Reads the headers for a scene from a file directory.
     * @param fileDirectory An std::string containing the directory of the file to be read.
     * @return An std::vector<chunkHeader> containing the chunk headers in order from the file.
     */
    std::vector<ChunkHeader> readHeadersJSON(const std::string& fileDirectory);

    /**
     * Loads a chunk from disk given the scene file directory and chunk header.
     * @param sceneFileDirectory The directory of the scene file.
     * @param chunkHeader The header of the chunk to be loaded.
     * @return A chunkData object containing the loaded chunk data.
     */
    Chunk loadChunkFromDisk(std::string sceneFileDirectory, ChunkHeader chunkHeader);

    /**
     * Writes a chunk to disk given the scene file directory and chunk data.
     * @param sceneFileDirectory The directory of the scene file.
     * @param chunk The chunk data to be written.
     */
    void writeChunkToDisk(std::string sceneFileDirectory, Chunk chunk);

    /**
     * Loads a scene from disk given the scene file directory.
     * @param sceneFileDirectory The directory of the scene file.
     * @return A scene object containing the loaded scene data.
     */
    Scene loadSceneFromDisk(std::string sceneFileDirectory);

    /**
     * Writes a scene to disk given the scene file directory and scene data.
     * @param sceneFileDirectory The directory of the scene file.
     * @param scene The scene data to be written.
     */
    void writeSceneToDisk(std::string sceneFileDirectory, Scene& scene);

    std::vector<ChunkHeader> loadChunkHeadersFromDisk(std::string sceneFileDirectory);

    std::vector<ChunkHeader> getChunkHeadersFromScene(Scene& scene);
}

#endif