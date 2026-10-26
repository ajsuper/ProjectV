#include "utils/blob_voxels.h"
#include "utils/material.h"
#include "utils/scene_loading.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_set>

#include <glm/gtc/quaternion.hpp>

#include "core/log.h"
#include "utils/scene_query.h"
#include "utils/voxel_management.h"

namespace projv::utils {
    namespace {
        // The key the eager loader shares blobs by: one source block, one mutability.
        std::string poolKey(const SceneBlockSource& s) {
            return s.path + "#" + std::to_string(s.entry.gridX) + "_" + std::to_string(s.entry.gridY) + "_" +
                   std::to_string(s.entry.gridZ) + "#" + std::to_string(static_cast<int>(s.mutability)) + "#" +
                   std::to_string(int(s.foldedSlot));
        }

        size_t blobBytes(const GeometryBlob& b) {
            return blobVoxelBytes(b) + b.envelope.size() * sizeof(uint32_t) + b.envelopeMotion.size();
        }

        // Where a block's chunk goes and how it is placed. The same numbers loadComposeFromDisk's
        // makeChunk computes, from the component's transform as it is now.
        struct Placement {
            core::vec3 corner{0.0f};
            core::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
            float      scale = 0.0f;        // world edge of the block
            float      nativeScale = 0.0f;  // edge before any transform
            float      uniformScale = 1.0f;
            int32_t    cell = -1;           // grid cell index, or -1 for a single-block component
            bool       valid = false;
        };

        Placement placement(const Scene& scene, const SceneBlockSource& s) {
            Placement p;
            const ComponentRecord& rec = scene.components[s.component];
            p.nativeScale = createChunkScaleFromVoxelScaleAndResolution(s.voxelScale, static_cast<int>(s.resolution));
            if (rec.kind == ComponentKind::Grid) {
                if (rec.gridIndex < 0 || size_t(rec.gridIndex) >= scene.grids.size()) return p;
                const SceneGrid& grid = scene.grids[size_t(rec.gridIndex)];
                core::ivec3 local = core::ivec3(s.entry.gridX, s.entry.gridY, s.entry.gridZ) - grid.originCellCoord;
                if (glm::any(glm::lessThan(local, core::ivec3(0))) || glm::any(glm::greaterThanEqual(local, grid.dims))) return p;
                p.cell = local.x + grid.dims.x * (local.y + grid.dims.y * local.z);
                p.rotation = grid.rotation;
                p.scale = grid.cellSize;
                p.uniformScale = grid.nativeCellSize > 0.0f ? grid.cellSize / grid.nativeCellSize : 1.0f;
                p.corner = grid.origin + glm::mat3_cast(grid.rotation) * (core::vec3(local) * grid.cellSize);
            } else if (rec.kind == ComponentKind::Chunk) {
                core::mat4 world = getComponentWorldMatrix(scene, s.component);
                core::vec3 col0(world[0]), col1(world[1]), col2(world[2]);
                float scale = core::length(col0);
                if (scale < 1e-8f) return p;
                p.uniformScale = scale;
                p.scale = p.nativeScale * scale;
                p.rotation = glm::quat_cast(core::mat3(col0 / scale, col1 / core::length(col1), col2 / core::length(col2)));
                p.corner = core::vec3(world[3]);
            } else {
                return p;
            }
            p.valid = true;
            return p;
        }
    }

    float distanceToBox(const WorldBox& box, core::vec3 point) {
        return glm::length(glm::max(glm::max(box.min - point, point - box.max), core::vec3(0.0f)));
    }

    // ---- SceneBlocks ----------------------------------------------------------------------------

    SceneBlocks::~SceneBlocks() {
        if (spillFolderMade_) {
            std::error_code e;
            std::filesystem::remove_all(spillFolder_, e);
        }
    }

    bool SceneBlocks::open(Scene& scene, const std::string& folder) {
        sources_.clear();
        blocks_.clear();
        shared_.clear();
        for (auto& [id, kept] : kept_) {
            if (!kept.file.empty()) { std::error_code e; std::filesystem::remove(kept.file, e); }
        }
        kept_.clear();
        keptOrder_.clear();
        byChunk_.clear();
        adoptGrid_ = adoptCell_ = 0;
        scene = openComposeFromDisk(folder, sources_);
        if (scene.components.empty()) return false;
        blocks_.resize(sources_.size());
        for (size_t i = 0; i < sources_.size(); i++) blocks_[i].component = refOf(scene, sources_[i].component);
        return true;
    }

    bool SceneBlocks::alive(const Scene& scene, BlockId id) const {
        return id < blocks_.size() && resolve(scene, blocks_[id].component) != INVALID_COMPONENT_HANDLE;
    }

    bool SceneBlocks::edited(const Scene& scene, BlockId id) const {
        if (id >= blocks_.size()) return false;
        if (!loaded(id)) return kept_.count(id) != 0;
        const Block& b = blocks_[id];
        if (b.edited) return true;
        int32_t pool = scene.chunks[b.chunk].geometryPoolIndex;
        return pool < 0 || size_t(pool) >= scene.geometryPool.size() || scene.geometryPool[size_t(pool)].contentStamp != b.readStamp;
    }

    WorldBox SceneBlocks::bounds(const Scene& scene, BlockId id) const {
        if (!alive(scene, id)) return {};
        Placement p = placement(scene, sources_[id]);
        if (!p.valid) return {};
        core::mat3 axes = glm::mat3_cast(p.rotation) * p.scale;
        WorldBox box{core::vec3(std::numeric_limits<float>::max()), core::vec3(std::numeric_limits<float>::lowest())};
        for (int corner = 0; corner < 8; corner++) {
            core::vec3 at = p.corner + axes * core::vec3(float(corner & 1), float((corner >> 1) & 1), float((corner >> 2) & 1));
            box.min = glm::min(box.min, at);
            box.max = glm::max(box.max, at);
        }
        return box;
    }

    bool SceneBlocks::needsRead(const Scene& scene, BlockId id) const {
        if (id >= blocks_.size() || loaded(id) || !alive(scene, id) || kept_.count(id)) return false;
        const SceneBlockSource& s = sources_[id];
        if (s.path.empty()) return false;
        auto it = shared_.find(poolKey(s));
        return !(it != shared_.end() && it->second.pool >= 0 && size_t(it->second.pool) < scene.geometryPool.size() &&
                 scene.geometryPool[size_t(it->second.pool)].refCount > 0 &&
                 scene.geometryPool[size_t(it->second.pool)].contentStamp == it->second.stamp);
    }

    bool SceneBlocks::load(Scene& scene, BlockId id) {
        if (!needsRead(scene, id)) return install(scene, id, nullptr);
        DataBlock data = readDataBlock(sources_[id].path, sources_[id].entry);
        return install(scene, id, &data);
    }

    bool SceneBlocks::install(Scene& scene, BlockId id, DataBlock* data) {
        if (id >= blocks_.size()) return false;
        if (loaded(id)) return true;
        if (!alive(scene, id)) return false;
        const SceneBlockSource& s = sources_[id];
        Block& block = blocks_[id];

        Placement p = placement(scene, s);
        ComponentRecord& rec = scene.components[s.component];
        bool free = rec.kind == ComponentKind::Grid
            ? p.valid && scene.grids[size_t(rec.gridIndex)].cellToChunk[size_t(p.cell)] == CELL_NOT_LOADED
            : p.valid && rec.chunkHandle >= scene.chunks.size();
        if (!free) {
            // Something else took the place -- an edit made the cell, or the component changed
            // kind. Loading over it would lose that.
            core::warn("SceneBlocks: block ({}, {}, {}) of '{}' was not loaded because its place is taken",
                       s.entry.gridX, s.entry.gridY, s.entry.gridZ, rec.name);
            return false;
        }

        // The voxels: kept from an earlier unload, shared with another instance, or read.
        int32_t pool = -1;
        GeometryBlob kept;
        if (takeKept(id, kept)) {
            kept.refCount = 1;
            kept.dirty = true;
            pool = poolInsertBlob(scene, std::move(kept));
            block.edited = true;
        } else if (!needsRead(scene, id)) {
            if (s.path.empty()) {
                core::error("SceneBlocks: block {} was made at runtime and nothing was kept of it", id);
                return false;
            }
            pool = shared_[poolKey(s)].pool;
            scene.geometryPool[size_t(pool)].refCount++;
            block.edited = false;
        } else {
            if (!data || (data->geometry.empty() && data->tiles.texels.empty())) {
                core::error("SceneBlocks: block ({}, {}, {}) of {} could not be read", s.entry.gridX,
                            s.entry.gridY, s.entry.gridZ, s.path);
                return false;
            }
            GeometryBlob blob;
            if (!data->tiles.texels.empty()) setBlobTiles(blob, std::move(data->tiles));
            else setBlobTree(blob, std::move(data->geometry), std::move(data->materialIDs));
            if (s.foldedSlot != INVALID_MATERIAL)   // a 256th palette entry's voxels, moved (ComposeComponent::foldedSlot)
                remapSlotBytes(editBlobMaterials(blob), INVALID_MATERIAL, s.foldedSlot);
            blob.envelope = std::move(data->envelope);
            blob.envelopeMotion = std::move(data->envelopeMotion);
            blob.sourceDataPath = s.path;
            blob.sourceBlockCoord = core::ivec3(s.entry.gridX, s.entry.gridY, s.entry.gridZ);
            blob.refCount = 1;
            blob.dirty = true;
            pool = poolInsertBlob(scene, std::move(blob));
            shared_[poolKey(s)] = {pool, scene.geometryPool[size_t(pool)].contentStamp};
            block.edited = false;
        }
        block.readStamp = scene.geometryPool[size_t(pool)].contentStamp;

        Chunk chunk;
        chunk.header.resolution = s.resolution;
        chunk.header.voxelScale = s.voxelScale * p.uniformScale;
        chunk.header.scale = p.scale;
        chunk.header.position = p.corner;
        chunk.header.rotation = p.rotation;
        chunk.nativeScale = p.nativeScale;
        chunk.mutability = s.mutability;
        chunk.geometryPoolIndex = pool;
        chunk.componentHandle = s.component;
        if (rec.kind == ComponentKind::Grid) {
            chunk.gridIndex = rec.gridIndex;
            chunk.cellIndex = p.cell;
            block.chunk = allocateChunkSlot(scene, std::move(chunk));
            scene.grids[size_t(rec.gridIndex)].cellToChunk[size_t(p.cell)] = int32_t(block.chunk);
        } else {
            block.chunk = allocateChunkSlot(scene, std::move(chunk));
            rec.chunkHandle = block.chunk;
            scene.looseChunks.push_back(block.chunk);
            scene.looseChunkCount = uint32_t(scene.looseChunks.size());
        }
        byChunk_[block.chunk] = id;
        blockResidencyCounter++;
        return true;
    }

    void SceneBlocks::unload(Scene& scene, BlockId id) {
        if (id >= blocks_.size() || !loaded(id)) return;
        Block& block = blocks_[id];
        const ChunkHandle handle = block.chunk;
        block.chunk = INVALID_CHUNK_HANDLE;
        byChunk_.erase(handle);
        // Deleting the component freed the chunk already, and its row may hold another by now.
        if (!alive(scene, id)) {
            block.edited = false;
            return;
        }
        Chunk& chunk = scene.chunks[handle];
        const int32_t pool = chunk.geometryPoolIndex;
        if (pool >= 0 && size_t(pool) < scene.geometryPool.size()) {
            GeometryBlob& blob = scene.geometryPool[size_t(pool)];
            if (block.edited || blob.contentStamp != block.readStamp) {
                GeometryBlob copy = blob;   // a deep copy: the blob may be shared still
                copy.refCount = 0;
                copy.brickMap.reset();      // rebuilt from the tree on the next edit
                keep(id, std::move(copy));
            }
            releaseBlob(scene, pool);
            // The CPU copy goes now; the GPU frees its range when it sees refCount 0.
            if (blob.refCount == 0) {
                setBlobTree(blob, {}, {});
                blob.envelope = {};
                blob.envelopeMotion = {};
                blob.brickMap.reset();
                markBlobContentChanged(blob);
            }
        }
        block.edited = false;

        ComponentRecord& rec = scene.components[sources_[id].component];
        if (chunk.gridIndex >= 0 && size_t(chunk.gridIndex) < scene.grids.size() && chunk.cellIndex >= 0) {
            scene.grids[size_t(chunk.gridIndex)].cellToChunk[size_t(chunk.cellIndex)] = CELL_NOT_LOADED;
        } else {
            std::vector<ChunkHandle>& loose = scene.looseChunks;
            loose.erase(std::remove(loose.begin(), loose.end(), handle), loose.end());
            scene.looseChunkCount = uint32_t(loose.size());
            rec.chunkHandle = INVALID_CHUNK_HANDLE;
        }
        // The row is freed as deleteComponent frees one: dead, empty, and its GPU row degenerate.
        chunk.alive = false;
        chunk.geometryPoolIndex = -1;
        chunk.geometryData.clear();
        chunk.gridIndex = -1;
        chunk.cellIndex = -1;
        chunk.componentHandle = INVALID_COMPONENT_HANDLE;
        chunk.headerDirty = true;
        if (scene.slots.enabled) scene.slots.chunks.push_back(handle);
        blockResidencyCounter++;
    }

    void SceneBlocks::replaceContent(Scene& scene, BlockId id, TileTable tiles) {
        if (id >= blocks_.size()) return;
        if (loaded(id)) {
            replaceChunkTiles(scene, blocks_[id].chunk, std::move(tiles));
            blocks_[id].edited = true;
            return;
        }
        GeometryBlob blob;
        setBlobTiles(blob, std::move(tiles));
        blob.sourceDataPath = sources_[id].path;
        blob.sourceBlockCoord = core::ivec3(sources_[id].entry.gridX, sources_[id].entry.gridY, sources_[id].entry.gridZ);
        keptOrder_.remove(id);
        kept_.erase(id);
        keep(id, std::move(blob));
    }

    void SceneBlocks::loadAll(Scene& scene) {
        for (BlockId id = 0; id < blocks_.size(); id++) load(scene, id);
    }

    std::vector<BlockId> SceneBlocks::adopt(const Scene& scene, size_t maxCells) {
        std::vector<BlockId> added;
        if (scene.grids.empty() || maxCells == 0) return added;
        // The components this opened, by handle: only their grids are ours to look in.
        std::unordered_set<ComponentHandle> ours;
        for (BlockId id = 0; id < blocks_.size(); id++) {
            ComponentHandle h = resolve(scene, blocks_[id].component);
            if (h != INVALID_COMPONENT_HANDLE) ours.insert(h);
        }
        size_t scanned = 0;
        for (size_t grids = 0; grids <= scene.grids.size() && scanned < maxCells; grids++) {
            if (adoptGrid_ >= scene.grids.size()) adoptGrid_ = 0, adoptCell_ = 0;
            const SceneGrid& grid = scene.grids[adoptGrid_];
            if (!ours.count(grid.componentHandle)) {
                adoptGrid_++, adoptCell_ = 0;
                continue;
            }
            for (; adoptCell_ < grid.cellToChunk.size() && scanned < maxCells; adoptCell_++, scanned++) {
                int32_t c = grid.cellToChunk[adoptCell_];
                if (c < 0 || byChunk_.count(ChunkHandle(c))) continue;
                const Chunk& chunk = scene.chunks[size_t(c)];
                int ix = int(adoptCell_ % size_t(grid.dims.x));
                int iy = int((adoptCell_ / size_t(grid.dims.x)) % size_t(grid.dims.y));
                int iz = int(adoptCell_ / (size_t(grid.dims.x) * size_t(grid.dims.y)));
                core::ivec3 coord = core::ivec3(ix, iy, iz) + grid.originCellCoord;
                float uniform = grid.nativeCellSize > 0.0f ? grid.cellSize / grid.nativeCellSize : 1.0f;
                SceneBlockSource s;
                s.component = grid.componentHandle;
                s.entry.gridX = coord.x;
                s.entry.gridY = coord.y;
                s.entry.gridZ = coord.z;
                s.resolution = chunk.header.resolution;
                s.voxelScale = chunk.header.voxelScale / uniform;
                s.mutability = chunk.mutability;
                BlockId id = BlockId(sources_.size());
                sources_.push_back(std::move(s));
                Block block;
                block.component = refOf(scene, grid.componentHandle);
                block.chunk = ChunkHandle(c);
                block.edited = true;   // nothing on disk holds it
                blocks_.push_back(block);
                byChunk_[ChunkHandle(c)] = id;
                added.push_back(id);
            }
            if (adoptCell_ >= grid.cellToChunk.size()) adoptGrid_++, adoptCell_ = 0;
        }
        return added;
    }

    // ---- Kept blocks ----

    void SceneBlocks::setKeptLimit(size_t memoryBytes, const std::string& folder) {
        keptLimit_ = memoryBytes;
        if (!folder.empty()) spillFolder_ = folder;
        spill();
    }

    size_t SceneBlocks::keptBytes() const {
        size_t bytes = 0;
        for (const auto& [id, kept] : kept_) bytes += kept.file.empty() ? kept.bytes : 0;
        return bytes;
    }

    size_t SceneBlocks::spilledBlocks() const {
        size_t n = 0;
        for (const auto& [id, kept] : kept_) n += !kept.file.empty();
        return n;
    }

    void SceneBlocks::keep(BlockId id, GeometryBlob&& blob) {
        Kept& kept = kept_[id];
        kept.bytes = blobBytes(blob);
        kept.blob = std::move(blob);
        kept.file.clear();
        keptOrder_.remove(id);
        keptOrder_.push_back(id);
        spill();
    }

    void SceneBlocks::spill() {
        size_t inMemory = keptBytes();
        while (inMemory > keptLimit_ && !keptOrder_.empty()) {
            BlockId id = keptOrder_.front();
            keptOrder_.pop_front();
            Kept& kept = kept_[id];
            if (spillFolder_.empty()) {
                spillFolder_ = (std::filesystem::temp_directory_path() /
                                ("projv_kept_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
            }
            if (!spillFolderMade_) {
                std::error_code e;
                std::filesystem::create_directories(spillFolder_, e);
                spillFolderMade_ = !e;
            }
            // One block in the engine's own container: the format already holds everything a blob is.
            DataFile file;
            file.resolution = sources_[id].resolution;
            file.voxelScale = sources_[id].voxelScale;
            DataBlock block;
            block.tiles = blobTiles(kept.blob);
            block.envelope = std::move(kept.blob.envelope);
            block.envelopeMotion = std::move(kept.blob.envelopeMotion);
            file.blocks.push_back(std::move(block));
            std::string path = (std::filesystem::path(spillFolder_) / ("block_" + std::to_string(id) + ".data")).string();
            if (!writeDataFile(path, file)) {
                // Kept in memory rather than lost: over the limit is better than gone.
                core::error("SceneBlocks: an edited block could not be written to {}; it stays in memory", path);
                DataBlock& back = file.blocks[0];
                kept.blob.envelope = std::move(back.envelope);   // its voxels never left
                kept.blob.envelopeMotion = std::move(back.envelopeMotion);
                return;
            }
            kept.file = path;
            setBlobTree(kept.blob, {}, {});   // on disk now; read back by takeKept
            inMemory -= kept.bytes;
        }
    }

    bool SceneBlocks::takeKept(BlockId id, GeometryBlob& out) {
        auto it = kept_.find(id);
        if (it == kept_.end()) return false;
        Kept& kept = it->second;
        if (!kept.file.empty()) {
            DataFile file = readDataFile(kept.file);
            if (file.blocks.empty()) {
                core::error("SceneBlocks: the kept copy of block {} could not be read back from {}", id, kept.file);
                return false;
            }
            DataBlock& b = file.blocks[0];
            if (!b.tiles.texels.empty()) setBlobTiles(kept.blob, std::move(b.tiles));
            else setBlobTree(kept.blob, std::move(b.geometry), std::move(b.materialIDs));
            kept.blob.envelope = std::move(b.envelope);
            kept.blob.envelopeMotion = std::move(b.envelopeMotion);
            std::error_code e;
            std::filesystem::remove(kept.file, e);
        }
        out = std::move(kept.blob);
        markBlobContentChanged(out);
        keptOrder_.remove(id);
        kept_.erase(it);
        return true;
    }

    // ---- Policies -------------------------------------------------------------------------------

    LoadPolicy loadEverything() {
        return {[](const LoadCandidate&, std::span<const Viewer>) { return Presence::Active; }, {}};
    }

    LoadPolicy loadNear(float active, float kept, float margin) {
        kept = std::max(kept, active);
        LoadRule rule = [active, kept, margin](const LoadCandidate& c, std::span<const Viewer> viewers) {
            float d = std::numeric_limits<float>::max();
            for (const Viewer& v : viewers) d = std::min(d, distanceToBox(c.bounds, v.position));
            float grace = 1.0f + margin;
            if (d <= active * (c.current == Presence::Active ? grace : 1.0f)) return Presence::Active;
            if (d <= kept * (c.current != Presence::Absent ? grace : 1.0f)) return Presence::Kept;
            return Presence::Absent;
        };
        return {std::move(rule), LoadHint{kept * (1.0f + margin)}};
    }

    LoadPolicy loadInside(WorldBox box, Presence inside) {
        return {[box, inside](const LoadCandidate& c, std::span<const Viewer>) {
                    bool touches = glm::all(glm::lessThanEqual(c.bounds.min, box.max)) &&
                                   glm::all(glm::lessThanEqual(box.min, c.bounds.max));
                    return touches ? inside : Presence::Absent;
                }, {}};
    }

    // ---- SceneLoader ----------------------------------------------------------------------------

    // Reads .data blocks off the main thread. It touches only files, never the Scene.
    struct SceneLoader::Worker {
        struct Job {
            BlockId          id;
            uint32_t         serial;
            std::string      path;
            BlockEntry       entry;
        };
        struct Result {
            BlockId   id;
            uint32_t  serial;
            DataBlock data;
        };
        std::mutex              mutex;
        std::condition_variable wake, done;
        std::deque<Job>         jobs;
        std::vector<Result>     results;
        size_t                  inFlight = 0;   // queued or being read
        bool                    stop = false;
        std::thread             thread;

        Worker() : thread([this] { run(); }) {}
        ~Worker() {
            { std::lock_guard lock(mutex); stop = true; }
            wake.notify_all();
            thread.join();
        }
        void run() {
            for (;;) {
                Job job;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [this] { return stop || !jobs.empty(); });
                    if (stop) return;
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
                DataBlock data = readDataBlock(job.path, job.entry);
                {
                    std::lock_guard lock(mutex);
                    results.push_back({job.id, job.serial, std::move(data)});
                    inFlight--;
                }
                done.notify_all();
            }
        }
    };

    SceneLoader::SceneLoader(const LoaderConfig& config) : config_(config) {
        config_.recheckUpdates = std::max(config_.recheckUpdates, 1u);
        config_.indexCell = std::max(config_.indexCell, 1.0f);
    }

    SceneLoader::~SceneLoader() = default;

    bool SceneLoader::open(Scene& scene, const std::string& folder) {
        worker_.reset();
        blocks_.setKeptLimit(config_.keptMemoryBytes, config_.spillFolder);
        bool ok = blocks_.open(scene, folder);
        size_t n = blocks_.size();
        want_.assign(n, Presence::Absent);
        reading_.assign(n, 0);
        readSerial_.assign(n, 0);
        nearStamp_.assign(n, 0);
        failed_.assign(n, 0);
        updateCount_ = 0;
        sliceCursor_ = 0;
        ruleCalls_ = 0;
        fullPass_ = true;
        askedFrom_.clear();
        reindex(scene);
        if (config_.threaded) worker_ = std::make_unique<Worker>();
        return ok;
    }

    void SceneLoader::setRule(LoadPolicy policy) {
        policy_ = std::move(policy);
        if (!policy_.rule) policy_ = loadEverything();
        fullPass_ = true;
        std::fill(failed_.begin(), failed_.end(), 0);
    }

    void SceneLoader::refreshBounds(const Scene& scene) {
        reindex(scene);
        fullPass_ = true;
    }

    namespace {
        uint64_t cellKey(core::ivec3 c) {
            constexpr int64_t bias = 1 << 20;
            return (uint64_t(c.x + bias) & 0x1FFFFF) | ((uint64_t(c.y + bias) & 0x1FFFFF) << 21) |
                   ((uint64_t(c.z + bias) & 0x1FFFFF) << 42);
        }
    }

    void SceneLoader::index(BlockId id) {
        const WorldBox& box = bounds_[id];
        if (box.empty()) return;
        core::ivec3 lo(glm::floor(box.min / config_.indexCell)), hi(glm::floor(box.max / config_.indexCell));
        for (int z = lo.z; z <= hi.z; z++)
            for (int y = lo.y; y <= hi.y; y++)
                for (int x = lo.x; x <= hi.x; x++) grid_[cellKey({x, y, z})].push_back(id);
    }

    void SceneLoader::reindex(const Scene& scene) {
        grid_.clear();
        bounds_.resize(blocks_.size());
        for (BlockId id = 0; id < blocks_.size(); id++) {
            bounds_[id] = blocks_.bounds(scene, id);
            index(id);
        }
    }

    void SceneLoader::nearBlocks(std::vector<std::pair<BlockId, bool>>& out) {
        const float r = policy_.hint.radius;
        for (const Viewer& v : viewers_) {
            core::ivec3 lo(glm::floor((v.position - r) / config_.indexCell));
            core::ivec3 hi(glm::floor((v.position + r) / config_.indexCell));
            for (int z = lo.z; z <= hi.z; z++)
                for (int y = lo.y; y <= hi.y; y++)
                    for (int x = lo.x; x <= hi.x; x++) {
                        auto it = grid_.find(cellKey({x, y, z}));
                        if (it == grid_.end()) continue;
                        for (BlockId id : it->second) {
                            if (nearStamp_[id] == updateCount_) continue;
                            if (distanceToBox(bounds_[id], v.position) > r) continue;
                            out.push_back({id, nearStamp_[id] + 1 != updateCount_});
                            nearStamp_[id] = updateCount_;
                        }
                    }
        }
    }

    void SceneLoader::evaluate(const Scene& scene, BlockId id) {
        (void)scene;
        if (bounds_[id].empty()) {
            want_[id] = Presence::Absent;
            return;
        }
        const SceneBlockSource& s = blocks_.source(id);
        LoadCandidate c;
        c.id = id;
        c.bounds = bounds_[id];
        c.component = s.component;
        c.cell = core::ivec3(s.entry.gridX, s.entry.gridY, s.entry.gridZ);
        c.current = want_[id];
        want_[id] = policy_.rule(c, viewers_);
        ruleCalls_++;
    }

    void SceneLoader::update(Scene& scene) {
        updateCount_++;
        const size_t before = blocks_.size();

        // 1. Cells an edit made in our grids become blocks (they are loaded: the edit made them).
        for (BlockId id : blocks_.adopt(scene, config_.adoptCellsPerUpdate)) {
            (void)id;
        }
        if (blocks_.size() != before) {
            size_t n = blocks_.size();
            want_.resize(n, Presence::Active);
            reading_.resize(n, 0);
            readSerial_.resize(n, 0);
            nearStamp_.resize(n, 0);
            failed_.resize(n, 0);
            bounds_.resize(n);
            for (BlockId id = BlockId(before); id < n; id++) {
                bounds_[id] = blocks_.bounds(scene, id);
                index(id);
            }
        }

        // 2. Ask the rule. Viewers that moved far enough since the last full pass make this one.
        bool moved = viewers_.size() != askedFrom_.size();
        for (size_t i = 0; i < viewers_.size() && !moved; i++)
            moved = viewers_[i].id != askedFrom_[i].id ||
                    glm::distance(viewers_[i].position, askedFrom_[i].position) >= config_.viewerMoveRecheck;
        if (moved) {
            fullPass_ = true;
            askedFrom_ = viewers_;
        }
        const size_t n = blocks_.size();
        const size_t slice = (n + config_.recheckUpdates - 1) / config_.recheckUpdates;
        if (policy_.hint.radius > 0.0f) {
            std::vector<std::pair<BlockId, bool>> nearby;
            nearBlocks(nearby);
            // Just come within range (or everything, on a full pass): asked now.
            for (auto [id, newly] : nearby) {
                if (fullPass_ || newly) evaluate(scene, id);
            }
            // Gone out of range: Absent now, without asking.
            for (BlockId id : prevNear_) {
                if (id < n && nearStamp_[id] != updateCount_) want_[id] = Presence::Absent;
            }
            if (fullPass_) {
                for (BlockId id = 0; id < n; id++)
                    if (nearStamp_[id] != updateCount_) want_[id] = Presence::Absent;
            }
            // The slice: asked again if near, Absent if not.
            for (size_t k = 0; k < slice && n > 0; k++, sliceCursor_++) {
                BlockId id = BlockId(sliceCursor_ % n);
                if (nearStamp_[id] == updateCount_) evaluate(scene, id);
                else want_[id] = Presence::Absent;
            }
            prevNear_.clear();
            for (auto [id, newly] : nearby) prevNear_.push_back(id);
        } else if (fullPass_) {
            for (BlockId id = 0; id < n; id++) evaluate(scene, id);
        } else {
            for (size_t k = 0; k < slice && n > 0; k++, sliceCursor_++) evaluate(scene, BlockId(sliceCursor_ % n));
        }
        fullPass_ = false;

        // 3. What to unload and what to load.
        std::vector<BlockId> unload, load;
        for (BlockId id = 0; id < n; id++) {
            bool wanted = want_[id] != Presence::Absent;
            if (!wanted && reading_[id]) {
                reading_[id] = 0;
                readSerial_[id]++;   // its read lands and is dropped
            }
            if (blocks_.loaded(id)) {
                if (!wanted) unload.push_back(id);
            } else if (wanted && !reading_[id] && !failed_[id]) {
                load.push_back(id);
            }
        }
        for (size_t i = 0; i < unload.size() && i < config_.maxUnloadsPerUpdate; i++) blocks_.unload(scene, unload[i]);

        // 4. Reads that have landed.
        uint32_t made = 0;
        if (worker_) {
            std::vector<Worker::Result> results;
            {
                std::lock_guard lock(worker_->mutex);
                results.swap(worker_->results);
            }
            for (Worker::Result& r : results) {
                if (r.id >= n || r.serial != readSerial_[r.id]) continue;
                reading_[r.id] = 0;
                if (want_[r.id] == Presence::Absent || blocks_.loaded(r.id)) continue;
                if (made >= config_.maxLoadsPerUpdate) {
                    // Over budget this update: back in the queue.
                    std::lock_guard lock(worker_->mutex);
                    reading_[r.id] = 1;
                    worker_->results.push_back(std::move(r));
                    continue;
                }
                if (!blocks_.install(scene, r.id, &r.data)) failed_[r.id] = 1;
                made++;
            }
        }

        // 5. New loads, nearest a viewer first.
        if (!viewers_.empty()) {
            std::vector<float> distance(load.size());
            for (size_t i = 0; i < load.size(); i++) {
                float d = std::numeric_limits<float>::max();
                for (const Viewer& v : viewers_) d = std::min(d, distanceToBox(bounds_[load[i]], v.position));
                distance[i] = d;
            }
            std::vector<size_t> order(load.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return distance[a] < distance[b]; });
            std::vector<BlockId> sorted(load.size());
            for (size_t i = 0; i < order.size(); i++) sorted[i] = load[order[i]];
            load.swap(sorted);
        }
        for (BlockId id : load) {
            if (made >= config_.maxLoadsPerUpdate) break;
            if (!blocks_.needsRead(scene, id) || !worker_) {
                if (!blocks_.load(scene, id)) failed_[id] = 1;
                made++;
                continue;
            }
            std::lock_guard lock(worker_->mutex);
            if (worker_->inFlight >= config_.maxPendingReads) break;
            const SceneBlockSource& s = blocks_.source(id);
            worker_->jobs.push_back({id, readSerial_[id], s.path, s.entry});
            worker_->inFlight++;
            reading_[id] = 1;
            worker_->wake.notify_one();
        }
    }

    void SceneLoader::settle(Scene& scene) {
        for (int round = 0; round < 1000000; round++) {
            update(scene);
            bool busy = false;
            for (BlockId id = 0; id < blocks_.size() && !busy; id++) {
                bool wanted = want_[id] != Presence::Absent;
                busy = reading_[id] || (wanted != blocks_.loaded(id) && !(wanted && failed_[id]));
            }
            if (!busy) return;
            if (worker_) {
                std::unique_lock lock(worker_->mutex);
                worker_->done.wait_for(lock, std::chrono::milliseconds(5),
                                       [this] { return !worker_->results.empty() || worker_->inFlight == 0; });
            }
        }
        core::error("SceneLoader::settle: gave up with blocks still loading");
    }

    LoaderStats SceneLoader::stats() const {
        LoaderStats s;
        s.blocks = blocks_.size();
        for (BlockId id = 0; id < blocks_.size(); id++) {
            s.loaded += blocks_.loaded(id);
            s.wanted += want_[id] != Presence::Absent;
            s.pendingReads += reading_[id];
        }
        s.kept = blocks_.keptBlocks();
        s.spilled = blocks_.spilledBlocks();
        s.ruleCalls = ruleCalls_;
        return s;
    }

    size_t residentVoxelBytes(const Scene& scene) {
        size_t bytes = 0;
        for (const GeometryBlob& blob : scene.geometryPool) {
            if (blob.refCount > 0) bytes += blobBytes(blob);
        }
        return bytes;
    }
}
