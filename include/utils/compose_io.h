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
     * Reads a .data (PVDT) container from disk into a DataFile. Reads the whole file; for per-block
     * seeking/streaming, see readDataFileHeader + readDataBlock below.
     * @param path The path of the .data file to read.
     * @return A DataFile containing all blocks. Empty on error.
     */
    DataFile readDataFile(const std::string& path);

    /**
     * Reads only the header + block table of a .data (PVDT) container — every block's grid coord and
     * byte offset/length — WITHOUT reading any geometry.
     * @param path The path of the .data file to read.
     * @return A DataFileHeader. Empty on error.
     */
    DataFileHeader readDataFileHeader(const std::string& path);

    /**
     * Reads exactly one block's arrays from a .data container by seeking to its recorded offsets.
     * @param path The path of the .data file.
     * @param entry The block-table entry (from readDataFileHeader) locating the block in the file.
     * @return The DataBlock. Empty geometry on error.
     */
    DataBlock readDataBlock(const std::string& path, const BlockEntry& entry);

    /**
     * Reads one block by index (reads the block table first, then the block). Prefer the BlockEntry
     * overload in a hot loop, where the table is already cached.
     * @param path The path of the .data file.
     * @param blockIndex The block's index in the file's block table.
     * @return The DataBlock. Empty on error or out-of-range index.
     */
    DataBlock readDataBlock(const std::string& path, uint32_t blockIndex);

    /**
     * Parses a compose.json file into a ComposeDoc. Line/block comments are permitted.
     * @param composeJsonPath The path of the compose.json file to parse.
     * @return A ComposeDoc. Empty (version 0) on error.
     */
    ComposeDoc parseComposeJson(const std::string& composeJsonPath);

    /**
     * Loads a Compose scene folder (containing a compose.json) and flattens it into a Scene.
     * Recursively expands `asset` references, bakes world transforms into each chunk, and
     * emits one Chunk per .data block.
     * @param folderPath The path of the folder containing the root compose.json.
     * @return A Scene with all geometry loaded eagerly.
     */
    Scene loadComposeFromDisk(const std::string& folderPath);
}

#endif
