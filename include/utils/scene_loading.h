#ifndef PROJV_UTILS_SCENE_LOADING_H
#define PROJV_UTILS_SCENE_LOADING_H

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "data_structures/scene.h"
#include "utils/compose_io.h"

// Loading part of a scene: blocks (one chunk, or one grid cell) come into memory and go again.
// The world-loading plan (docs/plans/world-loading.md) builds the rule that decides which on this.
//
// A scene opened here has its whole component tree -- names, transforms, palettes, attachments --
// and no voxels. Each block is then loaded or unloaded on its own. While a block is out:
//   - a grid cell reads CELL_NOT_LOADED (empty, to everything that draws or collides);
//   - a single-block component's chunkHandle is INVALID_CHUNK_HANDLE;
//   - edits to it are dropped (logged), and saveComposeToDisk refuses the scene.
// A block changed since it was read is kept in memory when unloaded, and comes back as it was left.
//
// No EnTT, no GPU: the renderer and physics see blocks come and go as chunks being made and freed.
namespace projv::utils {
    using BlockId = uint32_t;   // a block's place in SceneBlocks, fixed from open()

    struct WorldBox {
        core::vec3 min{0.0f};
        core::vec3 max{0.0f};
    };

    class SceneBlocks {
    public:
        // Opens the compose folder into `scene`, replacing what it held, with no block loaded.
        // False (and `scene` empty) if the folder did not load.
        bool open(Scene& scene, const std::string& folder);

        size_t size() const { return blocks_.size(); }
        const SceneBlockSource& source(BlockId id) const { return sources_[id]; }
        bool loaded(BlockId id) const { return blocks_[id].chunk != INVALID_CHUNK_HANDLE; }
        // The block's chunk, or INVALID_CHUNK_HANDLE.
        ChunkHandle chunk(BlockId id) const { return blocks_[id].chunk; }
        // Changed since it was read from disk, so unloading keeps it.
        bool edited(const Scene& scene, BlockId id) const;
        // The block's whole cube in world space, loaded or not: what a loading rule judges it by.
        // Empty (min == max) once its component is deleted.
        WorldBox bounds(const Scene& scene, BlockId id) const;

        // Reads the block (or takes back the kept copy) and makes its chunk. True when it is loaded
        // afterwards. False when its component was deleted or the file could not be read.
        bool load(Scene& scene, BlockId id);
        // Frees the block's chunk and its voxels, keeping them first if edited. A block whose
        // component was deleted is forgotten.
        void unload(Scene& scene, BlockId id);
        void loadAll(Scene& scene);

        // Edited blocks held while unloaded, and the bytes they take.
        size_t keptBlocks() const { return kept_.size(); }
        size_t keptBytes() const;

    private:
        struct Block {
            ComponentRef component;
            ChunkHandle  chunk = INVALID_CHUNK_HANDLE;
            uint64_t     readStamp = 0;   // the blob's content stamp as loaded
            bool         edited = false;  // came back from kept_: stays edited
        };
        // A block's voxels, already in the pool for another instance of the same source.
        struct Shared {
            int32_t  pool = -1;
            uint64_t stamp = 0;
        };

        bool alive(const Scene& scene, BlockId id) const;

        std::vector<SceneBlockSource>           sources_;
        std::vector<Block>                      blocks_;
        std::unordered_map<std::string, Shared> shared_;
        std::map<BlockId, GeometryBlob>         kept_;
    };

    // Bytes of voxel data in the scene's geometry pool (geometry, material ids, envelope): what
    // loading and unloading move.
    size_t residentVoxelBytes(const Scene& scene);
}

#endif
