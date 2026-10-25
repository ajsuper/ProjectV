#ifndef PROJECTV_BLOB_VOXELS_H
#define PROJECTV_BLOB_VOXELS_H

#include <cstdint>
#include <memory>
#include <vector>

#include "data_structures/scene.h"
#include "utils/tile_table.h"

// A geometry blob's voxels, in either of the two forms the engine has: its tile table
// (utils/tile_table.h) -- what the GPU marches, what edits change in place and what physics reads --
// or its tree64 and material bytes, the form a v1 .data stores and the brick-map authoring path builds.
//
// A blob holds whichever form was written last, and derives the other the first time it is asked for
// after a change (keyed by GeometryBlob::contentStamp, so a stale copy is never returned). Tiles derived
// from a tree replace it: from then on the blob holds tiles, and a tree asked for is built again. **Read
// through these, write through these**: the blob's fields are the storage behind them, and reading one
// directly can hand back the form that was not written last.
namespace projv::utils {
    // The voxels as a tile table.
    const TileTable& blobTiles(const GeometryBlob& blob);

    // The same table, shared: it stays as it is now, whatever is done to the blob after (an edit copies
    // the blob's first). What a physics shape reads its voxels from. Null when the blob is empty.
    std::shared_ptr<const TileTable> shareBlobTiles(const GeometryBlob& blob);

    // The voxels as a tree64 (3 words a node, root first) and the material bytes its leaves index.
    const std::vector<uint32_t>& blobTree(const GeometryBlob& blob);
    const std::vector<uint8_t>& blobTreeMaterials(const GeometryBlob& blob);

    // Replace the voxels with this tree and its materials / this tile table. Each takes a fresh
    // content stamp (markBlobContentChanged), as every write that replaces a blob's voxels must.
    void setBlobTree(GeometryBlob& blob, std::vector<uint32_t> tree, std::vector<uint8_t> materials);
    void setBlobTiles(GeometryBlob& blob, TileTable tiles);

    // The voxels as a tile table, to change in place (applyTileEdits): the tile form becomes the one the
    // blob holds, and its content stamp is taken fresh. `resolution` is the chunk's, for a blob still empty.
    TileTable& editBlobTiles(GeometryBlob& blob, uint32_t resolution);

    // The material bytes for a blob just interned from a chunk's tree (internChunkGeometry moves only the
    // tree): the authoring path's baked materials, joined to it. Before anything reads the blob: once its
    // tiles exist the tree is gone, and this refuses (and logs).
    void setBlobTreeMaterials(GeometryBlob& blob, std::vector<uint8_t> materials);

    // The material bytes of whichever form the blob holds -- to keep, and put back through
    // editBlobMaterials, across something that changes only slots (a palette remap and its undo).
    const std::vector<uint8_t>& blobMaterials(const GeometryBlob& blob);

    // The material bytes of whichever form the blob holds, to change in place -- a palette slot remap.
    // The voxels' content stamp is taken fresh first, so nothing derived from the old bytes survives.
    std::vector<uint8_t>& editBlobMaterials(GeometryBlob& blob);

    // Whether the blob holds no voxels, whichever form it is in.
    bool blobEmpty(const GeometryBlob& blob);

    // The bytes its voxels take, in the form it holds (the derived form is a cache, not counted).
    size_t blobVoxelBytes(const GeometryBlob& blob);

    // replaceChunkGeometry (scene.h) with a tile table: the chunk's voxels replaced -- in place when it is
    // the blob's only owner, else on a fresh blob it no longer shares -- at its own resolution, or at
    // `resolution` when given (a refine: regenerated finer, as replaceChunkGeometry's newResolution).
    void replaceChunkTiles(Scene& scene, ChunkHandle handle, TileTable tiles, uint32_t resolution = 0);

    // A tree64 and its materials from a tile table: the inverse of buildTileTable, for what still reads
    // trees (the animation envelope bake).
    void treeFromTileTable(const TileTable& tiles, std::vector<uint32_t>& tree, std::vector<uint8_t>& materials);
}

#endif
