#ifndef PROJV_UTILS_SCENE_LOADING_H
#define PROJV_UTILS_SCENE_LOADING_H

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "data_structures/scene.h"
#include "utils/compose_io.h"

// Loading part of a scene: blocks (one chunk, or one grid cell) come into memory and go again, as a
// rule the program writes decides. The plan: docs/plans/world-loading.md.
//
//     utils::SceneLoader loader;
//     loader.open(scene, "scenes/Island");
//     loader.setRule(utils::loadNear(64.0f, 192.0f));      // or loadEverything(), loadInside(box), a lambda
//     // each frame:
//     loader.setViewers({{cameraPosition}});
//     loader.update(scene);
//
// A scene opened here has its whole component tree -- names, transforms, palettes, attachments --
// and no voxels. While a block is out:
//   - a grid cell reads CELL_NOT_LOADED (empty, to everything that draws or collides);
//   - a single-block component's chunkHandle is INVALID_CHUNK_HANDLE;
//   - edits to it are dropped (logged), and saveComposeToDisk and duplicateComponent refuse it.
// A block changed since it was read is kept when unloaded (in memory, then in a session file past a
// limit) and comes back as it was left. Cells an edit makes are blocks too, kept the same way.
//
// No EnTT, no GPU: the renderer and physics see blocks come and go as chunks being made and freed.
namespace projv::utils {
    using BlockId = uint32_t;   // a block's place in SceneBlocks, fixed for as long as it is open

    struct WorldBox {
        core::vec3 min{0.0f};
        core::vec3 max{0.0f};
        bool empty() const { return !(min.x < max.x && min.y < max.y && min.z < max.z); }
    };
    // 0 inside.
    float distanceToBox(const WorldBox& box, core::vec3 point);

    // ---- Blocks ---------------------------------------------------------------------------------

    // The blocks of a scene opened lazily, each loaded or not. The mechanism SceneLoader drives; usable
    // alone to load blocks by hand.
    class SceneBlocks {
    public:
        // Opens the compose folder into `scene`, replacing what it held, with no block loaded.
        // False (and `scene` empty) if the folder did not load.
        bool open(Scene& scene, const std::string& folder);

        size_t size() const { return blocks_.size(); }
        // Where the block is on disk. `path` is empty for a block made at runtime (adopt()).
        const SceneBlockSource& source(BlockId id) const { return sources_[id]; }
        bool loaded(BlockId id) const { return blocks_[id].chunk != INVALID_CHUNK_HANDLE; }
        ChunkHandle chunk(BlockId id) const { return blocks_[id].chunk; }   // or INVALID_CHUNK_HANDLE
        // Changed since it was read from disk (or made at runtime), so unloading keeps it.
        bool edited(const Scene& scene, BlockId id) const;
        // The block's whole cube in world space, loaded or not, from its component's transform as it
        // is now. Empty once its component is deleted.
        WorldBox bounds(const Scene& scene, BlockId id) const;

        // Whether loading it reads its .data: not loaded, not kept, not shared with a loaded copy.
        bool needsRead(const Scene& scene, BlockId id) const;
        // Reads the block (or takes back the kept copy) and makes its chunk. True when it is loaded
        // afterwards; false when its component was deleted, its place is taken, or the read failed.
        bool load(Scene& scene, BlockId id);
        // load() with the read done elsewhere (a worker thread): `data` is what readDataBlock gave for
        // source(id), or null when needsRead was false.
        bool install(Scene& scene, BlockId id, DataBlock* data);
        // Frees the block's chunk and its voxels, keeping them first if edited. A block whose
        // component was deleted is forgotten.
        void unload(Scene& scene, BlockId id);
        void loadAll(Scene& scene);

        // Cells of the scene's lazily opened grids that are not blocks yet -- an edit made them --
        // become blocks, kept when they unload. Scans `maxCells` cells from where the last call
        // stopped, so a caller can spread the scan over frames. Returns the blocks added.
        std::vector<BlockId> adopt(const Scene& scene, size_t maxCells = SIZE_MAX);

        // Edited blocks held while unloaded. Past `memoryBytes` the oldest go to files in `folder`
        // (made on demand, removed with this object). Defaults: 256 MB, a folder under the system temp.
        void setKeptLimit(size_t memoryBytes, const std::string& folder = "");
        size_t keptBlocks() const { return kept_.size(); }
        size_t keptBytes() const;        // in memory
        size_t spilledBlocks() const;    // in files

        SceneBlocks() = default;
        SceneBlocks(const SceneBlocks&) = delete;
        SceneBlocks& operator=(const SceneBlocks&) = delete;
        ~SceneBlocks();

    private:
        struct Block {
            ComponentRef component;
            ChunkHandle  chunk = INVALID_CHUNK_HANDLE;
            uint64_t     readStamp = 0;   // the blob's content stamp as loaded
            bool         edited = false;  // came back from kept_, or made at runtime: stays edited
        };
        // A block's voxels, already in the pool for another instance of the same source.
        struct Shared {
            int32_t  pool = -1;
            uint64_t stamp = 0;
        };
        struct Kept {
            GeometryBlob blob;   // empty when spilled
            std::string  file;   // the spill file, or empty
            size_t       bytes = 0;
        };

        bool alive(const Scene& scene, BlockId id) const;
        bool takeKept(BlockId id, GeometryBlob& out);
        void keep(BlockId id, GeometryBlob&& blob);
        void spill();

        std::vector<SceneBlockSource>           sources_;
        std::vector<Block>                      blocks_;
        std::unordered_map<std::string, Shared> shared_;
        std::map<BlockId, Kept>                 kept_;
        std::list<BlockId>                      keptOrder_;   // oldest first, the in-memory ones
        std::unordered_map<ChunkHandle, BlockId> byChunk_;
        size_t                                  keptLimit_ = size_t(256) << 20;
        std::string                             spillFolder_;
        bool                                    spillFolderMade_ = false;
        size_t                                  adoptGrid_ = 0, adoptCell_ = 0;
    };

    // ---- The rule ---------------------------------------------------------------------------------

    // What the rule wants for a block. Without physics, Kept and Active both mean loaded; the runtime
    // gives them their meaning for bodies (world-loading.md).
    enum class Presence : uint8_t { Absent, Kept, Active };

    struct LoadCandidate {
        BlockId         id = 0;
        WorldBox        bounds;
        ComponentHandle component = INVALID_COMPONENT_HANDLE;
        core::ivec3     cell{0};                     // grid coordinate; (0,0,0) for a single block
        Presence        current = Presence::Absent;  // what the rule said last time: for hysteresis
        uint32_t        entity = 0xFFFFFFFFu;        // an entity being judged (runtime/loading.h), as its
                                                     // integer id; ~0u for a block
    };
    // Where the world is seen from: a camera, a player, a client. The program numbers them.
    struct Viewer {
        core::vec3 position{0.0f};
        uint32_t   id = 0;
    };

    using LoadRule = std::function<Presence(const LoadCandidate&, std::span<const Viewer>)>;
    // radius > 0: a block farther than this from every viewer is Absent without calling the rule.
    struct LoadHint {
        float radius = 0.0f;
    };
    struct LoadPolicy {
        LoadRule rule;
        LoadHint hint;
    };

    // Everything Active: the default, and what loadComposeFromDisk does.
    LoadPolicy loadEverything();
    // Active within `active` metres of a viewer, Kept within `kept`, Absent beyond. A block keeps what
    // it has until it is `margin` (a fraction) further out, so one on the edge does not flicker.
    LoadPolicy loadNear(float active, float kept, float margin = 0.1f);
    // Blocks touching `box` get `inside`; the rest are Absent.
    LoadPolicy loadInside(WorldBox box, Presence inside = Presence::Active);

    // ---- The loader -------------------------------------------------------------------------------

    struct LoaderConfig {
        uint32_t maxLoadsPerUpdate = 32;     // chunks made per update()
        uint32_t maxUnloadsPerUpdate = 64;   // chunks freed per update()
        uint32_t maxPendingReads = 64;       // reads queued on the worker at once
        uint32_t recheckUpdates = 30;        // every block is asked again within this many updates
        uint32_t adoptCellsPerUpdate = 4096; // grid cells scanned per update for ones an edit made
        float    viewerMoveRecheck = 2.0f;   // a viewer this far (m) from where blocks were last asked
                                             // from has them all asked again (within the hint)
        float    indexCell = 64.0f;          // the spatial index's cell, metres
        bool     threaded = true;            // reads on a worker thread; false: in update()
        size_t   keptMemoryBytes = size_t(256) << 20;
        std::string spillFolder;             // empty: under the system temp folder
    };

    struct LoaderStats {
        size_t blocks = 0;
        size_t loaded = 0;
        size_t wanted = 0;          // the rule says Kept or Active
        size_t pendingReads = 0;
        size_t kept = 0, spilled = 0;
        uint64_t ruleCalls = 0;     // since open
    };

    // Runs the rule over the blocks and loads and unloads them, a budget at a time.
    //
    // Which blocks are asked: on the first update after open, setRule or refreshBounds, and whenever
    // a viewer comes or goes or moves viewerMoveRecheck from where they were last asked from, all of
    // them (within the hint); otherwise a slice per update, so each is asked again within
    // recheckUpdates; and with a hint, any block a viewer has just come within range of, at once.
    // Loads go nearest viewer first. A block the rule stops wanting is unloaded even if its read is
    // still pending.
    class SceneLoader {
    public:
        explicit SceneLoader(const LoaderConfig& config = {});
        ~SceneLoader();
        SceneLoader(const SceneLoader&) = delete;
        SceneLoader& operator=(const SceneLoader&) = delete;

        bool open(Scene& scene, const std::string& folder);
        void setRule(LoadPolicy policy);
        void setRule(LoadRule rule, LoadHint hint = {}) { setRule(LoadPolicy{std::move(rule), hint}); }
        void setViewers(std::vector<Viewer> viewers) { viewers_ = std::move(viewers); }
        // Re-reads every block's bounds: after moving components that have blocks.
        void refreshBounds(const Scene& scene);

        // One frame's worth: ask the rule, start reads, make and free chunks within the budget.
        void update(Scene& scene);
        // Updates until every wanted block is loaded and every unwanted one gone (a loading screen,
        // a test). Waits on the worker between updates.
        void settle(Scene& scene);

        Presence presence(BlockId id) const { return want_[id]; }
        LoaderStats stats() const;
        SceneBlocks&       blocks() { return blocks_; }
        const SceneBlocks& blocks() const { return blocks_; }

    private:
        struct Worker;
        void evaluate(const Scene& scene, BlockId id);
        void index(BlockId id);
        void reindex(const Scene& scene);
        // Blocks within the hint of a viewer, each with whether it was not at the last update.
        void nearBlocks(std::vector<std::pair<BlockId, bool>>& out);

        LoaderConfig            config_;
        SceneBlocks             blocks_;
        LoadPolicy              policy_ = loadEverything();
        std::vector<Viewer>     viewers_;
        std::vector<Viewer>     askedFrom_;   // the viewers at the last full pass
        std::vector<Presence>   want_;
        std::vector<WorldBox>   bounds_;
        std::vector<uint8_t>    reading_;     // a read is queued or done and not installed
        std::vector<uint32_t>   readSerial_;  // bumped when a read is no longer wanted
        std::vector<uint32_t>   nearStamp_;   // the update a block was last found within the hint
        std::vector<uint8_t>    failed_;      // its load failed: not tried again until setRule
        std::vector<BlockId>    prevNear_;    // within the hint at the last update
        std::unordered_map<uint64_t, std::vector<BlockId>> grid_;
        uint32_t                updateCount_ = 0;
        size_t                  sliceCursor_ = 0;
        bool                    fullPass_ = true;
        uint64_t                ruleCalls_ = 0;
        std::unique_ptr<Worker> worker_;
    };

    // Bytes of voxel data in the scene's geometry pool (geometry, material ids, envelope): what
    // loading and unloading move.
    size_t residentVoxelBytes(const Scene& scene);
}

#endif
