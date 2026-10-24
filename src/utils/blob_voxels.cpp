#include "utils/blob_voxels.h"

#include "core/log.h"

#include "utils/block_table.h"
#include "utils/material.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

namespace projv::utils {
    namespace {
        // The form that was not written last, brought up to date with the one that was. Tiles derived
        // from a tree replace it: the tree is the form the engine reads least (only a tree asked for
        // again rebuilds it), and keeping both would hold every streamed or authored blob twice.
        void deriveTiles(const GeometryBlob& blob) {
            if (blob.voxelForm == GeometryBlob::VoxelForm::Tiles) return;
            blob.tiles = buildTileTable(blob.treeNodes, blob.treeMaterials, tree64Resolution(blob.treeNodes));
            std::vector<uint32_t>().swap(blob.treeNodes);
            std::vector<uint8_t>().swap(blob.treeMaterials);
            blob.voxelForm = GeometryBlob::VoxelForm::Tiles;
            blob.derivedStamp = 0;   // no stamp is 0: the tree is not current
        }
        void deriveTree(const GeometryBlob& blob) {
            if (blob.voxelForm == GeometryBlob::VoxelForm::Tree || blob.derivedStamp == blob.contentStamp) return;
            treeFromTileTable(blob.tiles, blob.treeNodes, blob.treeMaterials);
            blob.derivedStamp = blob.contentStamp;
        }
    }

    const TileTable& blobTiles(const GeometryBlob& blob) {
        deriveTiles(blob);
        return blob.tiles;
    }

    const std::vector<uint32_t>& blobTree(const GeometryBlob& blob) {
        deriveTree(blob);
        return blob.treeNodes;
    }

    const std::vector<uint8_t>& blobTreeMaterials(const GeometryBlob& blob) {
        deriveTree(blob);
        return blob.treeMaterials;
    }

    void setBlobTree(GeometryBlob& blob, std::vector<uint32_t> tree, std::vector<uint8_t> materials) {
        blob.treeNodes = std::move(tree);
        blob.treeMaterials = std::move(materials);
        blob.tiles = TileTable{};   // derived again when asked for
        blob.voxelForm = GeometryBlob::VoxelForm::Tree;
        markBlobContentChanged(blob);
    }

    void setBlobTiles(GeometryBlob& blob, TileTable tiles) {
        blob.tiles = std::move(tiles);
        blob.treeNodes.clear();
        blob.treeMaterials.clear();
        blob.voxelForm = GeometryBlob::VoxelForm::Tiles;
        markBlobContentChanged(blob);
    }

    TileTable& editBlobTiles(GeometryBlob& blob, uint32_t resolution) {
        deriveTiles(blob);
        if (blob.tiles.resolution == 0) blob.tiles.resolution = resolution;   // an empty tree has none
        blob.treeNodes.clear();
        blob.treeMaterials.clear();
        blob.voxelForm = GeometryBlob::VoxelForm::Tiles;
        markBlobContentChanged(blob);
        return blob.tiles;
    }

    void setBlobTreeMaterials(GeometryBlob& blob, std::vector<uint8_t> materials) {
        if (blob.voxelForm != GeometryBlob::VoxelForm::Tree) {
            // Its tiles were made from the tree before these bytes joined it: they and anything built
            // from them name the wrong slots. Materials go on before anything reads the blob.
            core::error("setBlobTreeMaterials: the blob's tree was already read as tiles; materials not set");
            return;
        }
        blob.treeMaterials = std::move(materials);
        markBlobContentChanged(blob);
    }

    const std::vector<uint8_t>& blobMaterials(const GeometryBlob& blob) {
        return blob.voxelForm == GeometryBlob::VoxelForm::Tiles ? blob.tiles.materials : blob.treeMaterials;
    }

    std::vector<uint8_t>& editBlobMaterials(GeometryBlob& blob) {
        markBlobContentChanged(blob);   // the other form, derived from the old bytes, is stale now
        return blob.voxelForm == GeometryBlob::VoxelForm::Tiles ? blob.tiles.materials : blob.treeMaterials;
    }

    bool blobEmpty(const GeometryBlob& blob) {
        return blob.voxelForm == GeometryBlob::VoxelForm::Tiles ? blob.tiles.texels.empty() : blob.treeNodes.empty();
    }

    size_t blobVoxelBytes(const GeometryBlob& blob) {
        if (blob.voxelForm == GeometryBlob::VoxelForm::Tiles)
            return blob.tiles.texels.size() * sizeof(uint32_t) + blob.tiles.materials.size();
        return blob.treeNodes.size() * sizeof(uint32_t) + blob.treeMaterials.size();
    }

    void replaceChunkTiles(Scene& scene, ChunkHandle handle, TileTable tiles, uint32_t resolution) {
        if (handle >= scene.chunks.size()) return;
        // The pool bookkeeping is replaceChunkGeometry's: an empty tree puts the chunk on a blob of its own.
        replaceChunkGeometry(scene, handle, {}, {}, resolution ? resolution : scene.chunks[handle].header.resolution);
        const int32_t pool = scene.chunks[handle].geometryPoolIndex;
        if (pool >= 0 && size_t(pool) < scene.geometryPool.size()) setBlobTiles(scene.geometryPool[size_t(pool)], std::move(tiles));
    }

    void treeFromTileTable(const TileTable& tiles, std::vector<uint32_t>& tree, std::vector<uint8_t>& materials) {
        tree.clear();
        materials.clear();
        if (tiles.texels.empty() || tiles.resolution < 4) return;
        // Through a brick map, the authoring path's own tree builder and material bake: the trees this
        // gives are exactly the ones the engine has always built.
        std::unique_ptr<VoxelBrickMap> map = createVoxelBrickMap(computeBrickDims(tiles.resolution));
        forEachTileVoxel(tiles, [&](core::ivec3 p, uint8_t slot) { brickMapSetVoxel(*map, p.x, p.y, p.z, slot); });
        tree = buildTree64FromBrickMap(*map, int(tiles.resolution));
        bakeMaterialsFromBrickMap(tree, materials, *map);
    }
}
