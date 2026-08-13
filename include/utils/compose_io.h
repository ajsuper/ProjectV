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
     * @return False if the file could not be opened, or the write failed part way and what is on disk
     *         is incomplete. **Check it.** This returned void until it was found that the one caller
     *         that matters went on to write a compose.json referencing a .data that had never been
     *         written -- a save that reported success and had quietly lost the geometry.
     */
    bool writeDataFile(const std::string& path, const DataFile& data);

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

    /**
     * Grafts a compose folder into a scene that is already open, as a subtree under `parent`.
     *
     * loadComposeFromDisk builds a whole Scene and is the only way to get a compose folder in memory;
     * placing an asset needs the same walk landing on a branch of a scene the user is working in.
     * This is that: the folder is loaded into a temporary Scene and then appended, with every handle
     * it carries remapped -- component handles, chunk handles, grid indices, geometry pool indices,
     * and the loose list.
     *
     * The folder's top-level components become children of one new Asset node named after the folder,
     * so the result is a single component the caller can place, name and delete as a unit. Its
     * `op` fields survive the trip, so an assembly written to disk comes back as the editable stack
     * that produced it rather than as inert geometry.
     *
     * Geometry is copied rather than shared with anything already in the scene: the two Scenes have
     * separate geometry pools and no common blob identity, so deduplication against components
     * already loaded is not attempted. Instancing *within* the imported folder is preserved.
     *
     * @param scene The scene to graft into.
     * @param folderPath The folder containing the compose.json to instantiate.
     * @param parent The Asset component to attach under, or INVALID_COMPONENT_HANDLE for a root.
     * @param localPosition Local position of the new node, relative to `parent`.
     * @param localRotation Local rotation of the new node.
     * @param localScale Uniform local scale of the new node.
     * @return The handle of the new Asset node, or INVALID_COMPONENT_HANDLE on failure.
     */
    ComponentHandle instantiateComposeInto(Scene& scene, const std::string& folderPath,
                                           ComponentHandle parent,
                                           core::vec3 localPosition = core::vec3(0.0f),
                                           core::quat localRotation = core::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                           float localScale = 1.0f);

    /**
     * Writes a ComposeDoc to a compose.json. The exact inverse of parseComposeJson: rotation goes out
     * as a 4-array [x, y, z, w] and scale as a single number when uniform, both of which the parser
     * accepts. Comments are not preserved -- the parser tolerates them, but nothing carries them into
     * a ComposeDoc to write back.
     * @param composeJsonPath The path (including the compose.json filename) to write.
     * @param doc The document to serialize.
     * @return True on success.
     */
    bool writeComposeJson(const std::string& composeJsonPath, const ComposeDoc& doc);

    /**
     * Extracts one Chunk or Grid component's live voxels as a DataFile ready for writeDataFile.
     *
     * The geometry pool is the source of truth, not the chunk: after an edit, applyEditsToChunk moves
     * the rebuilt tree64 into the blob and leaves Chunk::geometryData empty. Both arrays the format
     * stores -- the tree64 and the material bytes -- are copied straight out of the blob, because they
     * are already exactly what the file holds and what the GPU reads. Nothing is re-serialized per
     * voxel, and the palette they index is written by saveComposeToDisk into compose.json.
     *
     * `voxelScale` is recovered from Chunk::nativeScale, which is transform-independent by
     * construction -- reading it off header.voxelScale instead would bake the component's world scale
     * into the file, so a save/load/save cycle would compound that scale every pass.
     *
     * @param scene The scene owning the component.
     * @param handle The Chunk or Grid component to extract. Asset components have no geometry.
     * @return A DataFile; `resolution == 0` with no blocks when there is nothing to write.
     */
    DataFile dataFileFromComponent(const Scene& scene, ComponentHandle handle);

    /**
     * Writes a component subtree to disk as a compose folder -- the inverse of loadComposeFromDisk.
     *
     * Emits `folderPath/compose.json` describing the subtree's immediate children, one `<name>.data`
     * per Chunk/Grid child, and one recursively-written subfolder per Asset child. Every child's
     * local transform is written as it stands, so a load/save round trip is a fixed point.
     *
     * Chunk components sharing a geometry pool entry share one .data on disk, so an instanced scene
     * does not multiply on save the way one file per component would.
     *
     * @param scene The scene to write from.
     * @param root The Asset component whose children become this folder's compose.json, or
     *             INVALID_COMPONENT_HANDLE to write the scene's root components (the whole scene).
     * @param folderPath The folder to write into. Created if absent. Files whose names collide are
     *                   overwritten; unrelated files already in the folder are left alone.
     * @return True when the compose.json and every .data beneath it were written.
     */
    bool saveComposeToDisk(const Scene& scene, ComponentHandle root, const std::string& folderPath);
}

#endif
