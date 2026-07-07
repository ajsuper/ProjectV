#ifndef PROJECTV_COMPOSE_IO_H
#define PROJECTV_COMPOSE_IO_H

#include <vector>
#include <string>

#include "core/log.h"
#include "data_structures/scene.h"
#include "data_structures/compose.h"

namespace projv::utils {
    /**
     * Writes a DataFile to a .data (PVDT) container on disk.
     * @param path The path (including .data filename) to write to.
     * @param data The DataFile to serialize.
     */
    void writeDataFile(const std::string& path, const DataFile& data);

    /**
     * Reads a .data (PVDT) container from disk into a DataFile. Reads the whole file;
     * per-block seeking/streaming is not yet implemented.
     * @param path The path of the .data file to read.
     * @return A DataFile containing all blocks. Empty on error.
     */
    DataFile readDataFile(const std::string& path);

    /**
     * Parses a compose.json file into a ComposeDoc. Line/block comments are permitted.
     * @param composeJsonPath The path of the compose.json file to parse.
     * @return A ComposeDoc. Empty (version 0) on error.
     */
    ComposeDoc parseComposeJson(const std::string& composeJsonPath);

    /**
     * Loads a Compose scene folder (containing a compose.json) and flattens it into a Scene.
     * Recursively expands `asset` references, bakes world transforms into each chunk, and
     * emits one Chunk per .data block. A drop-in alternative to loadSceneFromDisk.
     * @param folderPath The path of the folder containing the root compose.json.
     * @return A Scene with all chunks placed in world space.
     */
    Scene loadComposeFromDisk(const std::string& folderPath);
}

#endif
