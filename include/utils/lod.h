#ifndef PROJECTV_LOD_H
#define PROJECTV_LOD_H

#include <stdint.h>
#include <string>
#include <iostream>
#include <chrono>
#include <math.h>

#include "data_structures/scene.h"
#include "utils/voxel_io.h"

namespace projv::utils {
    /**
     * Updates the LOD of a chunk and re-reads the full resolution version if necessary.
     * @param chunk The chunk data to be updated.
     * @param targetLOD The target LOD to be set.
     * @param sceneFilePath The file path of the scene.
     * @param forceReload Whether to force reload the chunk from disk. (false by default)
     */
    void updateLOD(Chunk& chunk, uint32_t targetLOD, const std::string& sceneFilePath, bool forceReload = false); // LOD
}

#endif