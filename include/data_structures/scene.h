#ifndef SCENE_H
#define SCENE_H

#include <vector>
#include <string>
#include <memory>
#include <stdint.h>

#include "core/math.h"
#include "data_structures/voxel.h"

namespace projv{

    constexpr uint32_t MAX_MATERIALS_PER_BLOB = 256u;
    constexpr uint8_t  INVALID_MATERIAL = 255u;

    struct Material {
        uint32_t packedColor = 0;
        uint32_t _pad = 0;
        std::string name;
    };
    // Stable slot handle for a chunk. A chunk keeps its handle for its whole life, and the handle IS
    // its row in the GPU header texture and its index into Scene.chunks. Grids and the loose list
    // reference chunks by handle so add/remove never has to rebase anything. See Scene below.
    using ChunkHandle = uint32_t;

    // Stable handle for a compose component (one `data` leaf instance from a compose.json -- see
    // ComposeComponent in compose.h). A component becomes exactly one of: a single loose Chunk, or a
    // whole SceneGrid (when its .data file has more than one block).
    using ComponentHandle = uint32_t;
    static constexpr ComponentHandle INVALID_COMPONENT_HANDLE = 0xFFFFFFFFu;

    enum class ComponentKind { Chunk, Grid, Asset };

    // One queued edit. Continuous component-space coords (not Z-order). P1: append via
    // queueVoxelAdd/queueVoxelRemove, drained by updateScene.
    struct PendingVoxelOp {
        bool         isAdd;
        core::ivec3  position;
        uint8_t      materialID;              // was: Color color
        uint8_t      _pad[3];                 // padding
    };

    // Per-component edit queue. P1: drained by updateScene; cleared after drain.
    struct ComponentEditQueue {
        std::vector<PendingVoxelOp> ops;
    };

    // Reference to the .data file backing a component. One entry per unique
    // (path, resolution, voxelScale). P1: allocated lazily by updateScene on first edit.
    struct DataReference {
        std::string sourceDataPath;
        uint32_t    resolution;
        float       voxelScale;
    };

    // One entry per loaded component, in Scene.components, indexed by ComponentHandle. Populated by
    // loadComposeFromDisk at the same two sites that create a loose Chunk or push a SceneGrid.
    struct ComponentRecord {
        ComponentKind kind;
        ChunkHandle   chunkHandle = 0;  // valid when kind == ComponentKind::Chunk
        int32_t       gridIndex = -1;   // valid when kind == ComponentKind::Grid
        std::string   sourcePath;       // provenance/debugging, mirrors GeometryBlob::sourceDataPath

        // P1 additions: per-component edit queue and lazily-assigned data reference.
        ComponentEditQueue editQueue;
        int32_t            dataRefID = -1;   // index into Scene.dataReferences; -1 = unassigned

        // P6: Identity
        std::string name;                    // local name (from compose.json or auto-generated)

        // P6: Hierarchy (Chunk, Grid, Asset -- all three participate)
        ComponentHandle parent = INVALID_COMPONENT_HANDLE;
        std::vector<ComponentHandle> children;   // populated for Asset; empty for Chunk/Grid

        // P6: Local transform (relative to parent; all three use this)
        core::vec3 localPosition = core::vec3(0.0f);
        core::quat localRotation = core::quat(1.0f, 0.0f, 0.0f, 0.0f);
        float      localScale = 1.0f;            // uniform only (v0.0)
    };

    // Governs what happens when a `data` component's voxel data is modified and persisted.
    // (Runtime home; ComposeComponent in compose.h references it.) See compose_data_structure.md.
    enum class Mutability {
        Locked, // Never written back; instances of the same source share one buffer.
        Direct, // Edits are written in place to the source .data; same-policy instances still share.
        Copy    // Edits are written to a new .data (copy-on-write); original untouched.
    };

    struct ChunkHeader { // Designed to be user interfacable on CPU. Stored in disk. Only the necessary information for loading the chunk.
        uint32_t chunkID;
        core::vec3 position;
        float scale;
        float voxelScale;
        uint32_t resolution;
        core::quat rotation = core::quat(1.0f, 0.0f, 0.0f, 0.0f); // World rotation (w,x,y,z), identity by default.
    };

    #pragma pack(push, 1)
    struct GPUChunkHeader {
        uint32_t chunkID;
        float positionX;
        float positionY;
        float positionZ;
        float scale;
        uint32_t resolution;
        uint32_t geometryStartIndex;
        uint32_t geometryEndIndex;
        uint32_t materialIDStartIndex;      // index into materialID texture (in bytes)
        uint32_t materialIDEndIndex;        // end index (exclusive, in bytes)
        uint32_t dataRefID;
        uint32_t paletteOffset; // replaced padding[1]: per-blob offset into global palette
        float rotationX;
        float rotationY;
        float rotationZ;
        float rotationW;
    };
    #pragma pack(pop)
    
    struct Chunk { // Only exists during runtime. Contains all of the header data and our geometry and color data, along with any extra runtime data not used in rendering.
        ChunkHeader header;
        std::vector<uint32_t> geometryData;
        uint32_t LOD;
        // Instancing: when >= 0, this chunk's geometry lives once in Scene.geometryPool[idx]
        // (shared across every instance of the same .data block) and geometryData/voxelTypeData
        // above are left empty. -1 = unpooled (interned into the pool by internChunkGeometry before
        // any GPU work; see internChunkGeometry in scene.h).
        int32_t geometryPoolIndex = -1;
        // Persistence policy for this instance (from its compose.json component). Drives whether an
        // edit is written back (Direct), copied off (Copy), or in-memory only (Locked).
        Mutability mutability = Mutability::Locked;
        // Slot liveness. Dead slots get a degenerate GPU header row (scale <= 0) that the shader
        // skips. Loaders create chunks alive.
        bool alive = true;
        // P6: true when the chunk's header (position/rotation) changed since the last GPU
        // sync. Set by rebakeSubtree, cleared by flushSceneUpdates → updateDirtyHeaders.
        bool headerDirty = false;
        // Residency key: which grid + cell this chunk fills, or -1/-1 when it is a loose
        // (transform-placed) leaf. Gives O(1) chunk->cell so eviction can clear the right
        // SceneGrid.cellToChunk slot without scanning. gridIndex indexes Scene.grids.
        int32_t gridIndex = -1;
        int32_t cellIndex = -1;
        // Which compose component this chunk came from. Only meaningful when loose (gridIndex == -1)
        // -- a grid-resident chunk's component is SceneGrid::componentHandle instead (one component
        // owns every cell of its grid).
        ComponentHandle componentHandle = INVALID_COMPONENT_HANDLE;
    };

    // One unique geometry blob shared across chunk instances. See Scene.geometryPool.
    struct Material;

struct GeometryBlob {
        std::vector<uint32_t> geometry;
        std::vector<uint8_t>  materialIDs;
        std::vector<Material> materialPalette;
        std::vector<uint32_t> voxelTypeData; // DEPRECATED: kept for backward compat, will be removed
        std::unique_ptr<VoxelBrickMap> brickMap;

        GeometryBlob() = default;
        GeometryBlob(GeometryBlob&&) = default;
        GeometryBlob& operator=(GeometryBlob&&) = default;
        GeometryBlob(const GeometryBlob& rhs);
        GeometryBlob& operator=(const GeometryBlob& rhs);
        // Provenance for persistence: the .data file and block this geometry came from. Deduped with
        // the blob (few per scene), so it stays off the per-chunk struct. A copy-on-write fork inherits
        // these, then repoints sourceDataPath at its new file once persisted.
        std::string sourceDataPath;
        core::ivec3 sourceBlockCoord = core::ivec3(0);
        // False on a fresh copy-on-write fork: sourceDataPath still points at the inherited *original*
        // source (read-only for us). The first Copy persist writes a new file, repoints sourceDataPath,
        // and sets this true; subsequent Copy persists then write in place to that owned file.
        bool ownsSourceFile = true;
        // Number of live chunks referencing this blob. Incremented when a chunk is bound to the blob,
        // decremented on chunk removal / COW fork-away. At 0 the blob's GPU range is freed and its pool
        // slot is recycled via Scene.blobFreeList. Bounds pool growth over a long dynamic session.
        uint32_t refCount = 0;
        // P5: true when the blob's GPU content is stale (newly forked, interned, or content changed).
        // Cleared by flushSceneUpdates after the incremental upload.
        bool dirty = false;
        // Incremented each time materialPalette is modified. Used to avoid unnecessary
        // palette texture rebuilds in uploadDirtyBlobs.
        uint32_t paletteVersion = 0;
    };
    
    // A uniform grid of equal, grid-aligned chunks (one .data grid volume). Enables a
    // top-level DDA over cells instead of brute-forcing every chunk. See compose_data_structure.md.
    struct SceneGrid {
        core::vec3 origin;                  // World position of the grid's local (0,0,0) corner.
        float cellSize;                     // World size of one cell (one block).
        core::ivec3 dims;                   // Per-axis cell count.
        core::quat rotation;                // World rotation of the grid.
        std::vector<int32_t> cellToChunk;   // dims.x*dims.y*dims.z entries; index into Scene.chunks, or -1 if empty.
        // The compose component that owns this grid (every cell shares one component identity).
        ComponentHandle componentHandle = INVALID_COMPONENT_HANDLE;
        // Phase 2: cell coordinate (in the original .data block space) that sits at grid.origin.
        // After negative expansion this shifts away from (0,0,0). Used to linearize floorDiv'd
        // cell coordinates into the cellToChunk array.
        core::ivec3 originCellCoord{0};
    };

    struct Scene {
        // Slot-indexed with holes: a chunk's index is its stable ChunkHandle and its GPU header row.
        std::vector<Chunk> chunks;
        std::vector<SceneGrid> grids;
        // Explicit set of loose (transform-placed) chunk handles. The shader iterates this list
        // instead of a positional [0, looseChunkCount) prefix, so a loose chunk can be added or
        // removed without any ordering constraint on Scene.chunks. Grid blocks are reached only via
        // SceneGrid.cellToChunk and are NOT listed here.
        std::vector<ChunkHandle> looseChunks;
        // Derived count of loose chunks, kept only so the legacy no-grid GPU fallback still works;
        // ordering of Scene.chunks is no longer load-bearing. Prefer looseChunks.size().
        uint32_t looseChunkCount = 0;
        // Shared geometry for .data blocks; chunks reference entries via Chunk.geometryPoolIndex.
        // Refcounted (GeometryBlob.refCount); empty slots recycled via blobFreeList.
        std::vector<GeometryBlob> geometryPool;
        std::vector<uint32_t> blobFreeList; // recycled empty geometryPool slots (LIFO).
        // One entry per loaded compose component, indexed by ComponentHandle. Populated by
        // loadComposeFromDisk; never compacted or reordered (handles are stable for the Scene's life).
        std::vector<ComponentRecord> components;
        // P1 addition: lazily populated by editing::updateScene on first edit per source.
        std::vector<DataReference>   dataReferences;
    };

    // Inserts a blob into the pool, reusing a recycled slot when one is free so live pool indices
    // stay stable. Pure data-structure op (no IO), so both loaders and the GPU builder can call it.
    inline int32_t poolInsertBlob(Scene& scene, GeometryBlob&& blob) {
        if (!scene.blobFreeList.empty()) {
            int32_t idx = static_cast<int32_t>(scene.blobFreeList.back());
            scene.blobFreeList.pop_back();
            scene.geometryPool[idx] = std::move(blob);
            return idx;
        }
        int32_t idx = static_cast<int32_t>(scene.geometryPool.size());
        scene.geometryPool.push_back(std::move(blob));
        return idx;
    }

    // Moves an unpooled chunk's owned geometry (geometryPoolIndex < 0) into a fresh refcount-1 pool blob
    // and points the chunk at it, so every chunk flows through the single pooled path. No-op (returns
    // the existing index) if the chunk is already pooled. Used by the voxelizer and defensively by
    // the GPU builder to guarantee no chunk reaches the GPU with geometryPoolIndex < 0.
    inline int32_t internChunkGeometry(Scene& scene, Chunk& chunk,
                                       std::unique_ptr<VoxelBrickMap> brickMap = nullptr) {
        if (chunk.geometryPoolIndex >= 0) return chunk.geometryPoolIndex;
        GeometryBlob blob;
        blob.geometry = std::move(chunk.geometryData);
        if (brickMap) blob.brickMap = std::move(brickMap);
        blob.refCount = 1;
        blob.dirty = true;
        chunk.geometryData.clear();
        chunk.geometryPoolIndex = poolInsertBlob(scene, std::move(blob));
        return chunk.geometryPoolIndex;
    }

    // Copy-on-write fork of a pool blob (P1 editing primitive). Creates a new GeometryBlob that is a
    // deep copy of geometryPool[srcIndex], with refCount = 1 (the editing chunk owns it). Does NOT
    // decrement the original's refCount -- other chunks may still share it. The caller is responsible
    // for pointing the editing chunk at the returned index. Returns srcIndex unchanged if it is
    // out of range (caller should intern the chunk first via internChunkGeometry).
    //
    // P6 optimization: if the blob's refCount is exactly 1 (sole owner), skip the copy and edit
    // in-place. The blob is marked dirty for the next GPU flush, and the same index is returned.
    // This avoids allocating a new pool entry and GPU range on every edit, which is the dominant
    // source of allocator pressure in sustained editing sessions.
    inline int32_t forkBlob(Scene& scene, int32_t srcIndex) {
        if (srcIndex < 0 || srcIndex >= static_cast<int32_t>(scene.geometryPool.size())) {
            return srcIndex;
        }
        // Sole owner — no copy needed, edit in place.
        if (scene.geometryPool[srcIndex].refCount == 1) {
            scene.geometryPool[srcIndex].dirty = true;
            return srcIndex;
        }
        GeometryBlob fork = scene.geometryPool[srcIndex]; // deep copy via copy ctor
        fork.refCount = 1;
        fork.dirty = true;
        return poolInsertBlob(scene, std::move(fork));
    }

    }

#endif
