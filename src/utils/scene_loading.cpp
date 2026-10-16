#include "utils/scene_loading.h"

#include <algorithm>
#include <array>
#include <limits>

#include <glm/gtc/quaternion.hpp>

#include "core/log.h"
#include "utils/scene_query.h"
#include "utils/voxel_management.h"

namespace projv::utils {
    namespace {
        // The key the eager loader shares blobs by: one source block, one mutability.
        std::string poolKey(const SceneBlockSource& s) {
            return s.path + "#" + std::to_string(s.entry.gridX) + "_" + std::to_string(s.entry.gridY) + "_" +
                   std::to_string(s.entry.gridZ) + "#" + std::to_string(static_cast<int>(s.mutability));
        }

        size_t blobBytes(const GeometryBlob& b) {
            return b.geometry.size() * sizeof(uint32_t) + b.materialIDs.size() +
                   b.envelope.size() * sizeof(uint32_t) + b.envelopeMotion.size();
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

    bool SceneBlocks::open(Scene& scene, const std::string& folder) {
        sources_.clear();
        blocks_.clear();
        shared_.clear();
        kept_.clear();
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

    bool SceneBlocks::load(Scene& scene, BlockId id) {
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
        if (auto kept = kept_.find(id); kept != kept_.end()) {
            GeometryBlob blob = std::move(kept->second);
            kept_.erase(kept);
            blob.refCount = 1;
            blob.dirty = true;
            pool = poolInsertBlob(scene, std::move(blob));
            block.edited = true;
        } else {
            const std::string key = poolKey(s);
            auto it = shared_.find(key);
            if (it != shared_.end() && it->second.pool >= 0 && size_t(it->second.pool) < scene.geometryPool.size() &&
                scene.geometryPool[size_t(it->second.pool)].refCount > 0 &&
                scene.geometryPool[size_t(it->second.pool)].contentStamp == it->second.stamp) {
                pool = it->second.pool;
                scene.geometryPool[size_t(pool)].refCount++;
            } else {
                DataBlock data = readDataBlock(s.path, s.entry);
                if (data.geometry.empty()) {
                    core::error("SceneBlocks: block ({}, {}, {}) of {} could not be read", s.entry.gridX,
                                s.entry.gridY, s.entry.gridZ, s.path);
                    return false;
                }
                GeometryBlob blob;
                blob.geometry = std::move(data.geometry);
                blob.materialIDs = std::move(data.materialIDs);
                blob.envelope = std::move(data.envelope);
                blob.envelopeMotion = std::move(data.envelopeMotion);
                blob.sourceDataPath = s.path;
                blob.sourceBlockCoord = core::ivec3(s.entry.gridX, s.entry.gridY, s.entry.gridZ);
                blob.refCount = 1;
                blob.dirty = true;
                pool = poolInsertBlob(scene, std::move(blob));
                shared_[key] = {pool, scene.geometryPool[size_t(pool)].contentStamp};
            }
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
        return true;
    }

    void SceneBlocks::unload(Scene& scene, BlockId id) {
        if (id >= blocks_.size() || !loaded(id)) return;
        Block& block = blocks_[id];
        const ChunkHandle handle = block.chunk;
        block.chunk = INVALID_CHUNK_HANDLE;
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
                kept_[id] = std::move(copy);
            }
            releaseBlob(scene, pool);
            // The CPU copy goes now; the GPU frees its range when it sees refCount 0.
            if (blob.refCount == 0) {
                blob.geometry = {};
                blob.materialIDs = {};
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
    }

    void SceneBlocks::loadAll(Scene& scene) {
        for (BlockId id = 0; id < blocks_.size(); id++) load(scene, id);
    }

    size_t SceneBlocks::keptBytes() const {
        size_t bytes = 0;
        for (const auto& [id, blob] : kept_) bytes += blobBytes(blob);
        return bytes;
    }

    size_t residentVoxelBytes(const Scene& scene) {
        size_t bytes = 0;
        for (const GeometryBlob& blob : scene.geometryPool) {
            if (blob.refCount > 0) bytes += blobBytes(blob);
        }
        return bytes;
    }
}
