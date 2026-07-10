#ifndef PROJECTV_COMPOSE_IO_H
#define PROJECTV_COMPOSE_IO_H

#include <vector>
#include <string>

#include "core/log.h"
#include "data_structures/scene.h"
#include "data_structures/compose.h"

namespace projv::utils {
    // Forward declaration: the streaming variant of loadComposeFromDisk defers grid volumes into a
    // StreamingContext (defined in utils/streaming.h) rather than loading their geometry. compose_io
    // itself only fills the struct through a pointer, so it does not depend on streaming.h.
    struct StreamingContext;

    /**
     * Writes a DataFile to a .data (PVDT) container on disk.
     * @param path The path (including .data filename) to write to.
     * @param data The DataFile to serialize.
     */
    void writeDataFile(const std::string& path, const DataFile& data);

    /**
     * Reads a .data (PVDT) container from disk into a DataFile. Reads the whole file; for per-block
     * seeking/streaming (the cheap path used by the streaming residency layer), see
     * readDataFileHeader + readDataBlock below.
     * @param path The path of the .data file to read.
     * @return A DataFile containing all blocks. Empty on error.
     */
    DataFile readDataFile(const std::string& path);

    /**
     * Reads only the header + block table of a .data (PVDT) container — every block's grid coord and
     * byte offset/length — WITHOUT reading any geometry. The cheap index that drives per-block streaming.
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
     * overload in a hot streaming loop, where the table is already cached.
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
     * @param streaming When non-null, grid volumes (multi-block .data) are NOT loaded: only their
     *        SceneGrid descriptor is built (all cells empty) and a per-grid streaming record is pushed
     *        into `streaming`, so a residency policy can materialize cells on demand. Single-block
     *        (loose) assets still load eagerly. When null, everything loads eagerly (default).
     * @return A Scene with loose chunks placed in world space; grid cells resident only if streamed in.
     */
    Scene loadComposeFromDisk(const std::string& folderPath, StreamingContext* streaming = nullptr);

    /**
     * Returns the geometry-pool index whose GeometryBlob the caller may safely mutate, honoring the
     * chunk's mutability (copy-on-write gate):
     *   - Locked/Direct: the existing shared index (edits are shared across same-policy instances of
     *     the source; never across policies, which the loader already keeps in separate pool entries).
     *   - Copy: on first call, forks a private duplicate blob (setting chunk.copyDiverged) and returns
     *     its index; idempotent thereafter.
     * @param scene The scene owning the geometry pool.
     * @param chunk The chunk whose geometry is about to be edited. Must reference the pool (index >= 0).
     * @return A pool index safe to mutate, or -1 if the chunk is not pooled.
     */
    int32_t makeChunkGeometryWritable(Scene& scene, Chunk& chunk);

    /**
     * Regenerates a pooled chunk's geometry from its VoxelBatch (chunk.chunkQueue), writing the result
     * into the policy-resolved pool blob (see makeChunkGeometryWritable). Full regeneration from the
     * queue, mirroring updateChunkFromItsVoxelBatch. No-op if the chunk is not pooled or the queue is empty.
     * @param scene The scene owning the geometry pool.
     * @param chunk The pooled chunk to edit; its chunkQueue holds the voxels to bake.
     */
    void editChunkVoxels(Scene& scene, Chunk& chunk);

    /**
     * Persists a pooled chunk's current geometry to disk per its mutability:
     *   - Locked: no-op (returns false).
     *   - Direct: rewrites the chunk's block in place in its source .data.
     *   - Copy: writes a new .data (source with this block replaced) and repoints the (forked) blob at
     *     it; the original file is left untouched. Subsequent Copy persists write in place to the new file.
     * @param scene The scene owning the geometry pool.
     * @param chunk The pooled chunk to persist.
     * @param copyDestPath For Copy, the destination .data path; auto-derived when empty.
     * @return true if anything was written to disk.
     */
    bool persistChunkData(Scene& scene, Chunk& chunk, const std::string& copyDestPath = "");
}

#endif
