#ifndef SCENE_H
#define SCENE_H

#include <vector>
#include <string>
#include <stdint.h>

#include "core/math.h"
#include "data_structures/voxel.h"

namespace projv{
    // Stable slot handle for a chunk. A chunk keeps its handle for its whole life, and the handle IS
    // its row in the GPU header texture and its index into Scene.chunks. Grids and the loose list
    // reference chunks by handle so add/remove never has to rebase anything. See Scene below.
    using ChunkHandle = uint32_t;

    // Stable handle for a compose component (one `data` leaf instance from a compose.json -- see
    // ComposeComponent in compose.h). A component becomes exactly one of: a single loose Chunk, or a
    // whole SceneGrid (when its .data file has more than one block). Editing is addressed by
    // component (see utils::addVoxelsToComponent/removeVoxelsFromComponent in utils/streaming.h) so
    // callers never need to know or care which of the two a given component turned out to be.
    using ComponentHandle = uint32_t;
    static constexpr ComponentHandle INVALID_COMPONENT_HANDLE = 0xFFFFFFFFu;

    enum class ComponentKind { Chunk, Grid };

    // One entry per loaded component, in Scene.components, indexed by ComponentHandle. Populated by
    // loadComposeFromDisk at the same two sites that create a loose Chunk or push a SceneGrid.
    struct ComponentRecord {
        ComponentKind kind;
        ChunkHandle   chunkHandle = 0;  // valid when kind == ComponentKind::Chunk
        int32_t       gridIndex = -1;   // valid when kind == ComponentKind::Grid
        std::string   sourcePath;       // provenance/debugging, mirrors GeometryBlob::sourceDataPath
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
    struct GPUChunkHeader { // Not designed to be user interfacable on CPU. Only exists during runtime, mainly on GPU. Only the necessary information for rendering.
        uint32_t chunkID;
        float positionX;
        float positionY;
        float positionZ;
        float scale;
        uint32_t resolution;
        uint32_t geometryStartIndex;
        uint32_t geometryEndIndex;
        uint32_t voxelTypeDataStartIndex;
        uint32_t voxelTypeDataEndIndex;
        uint32_t padding[2];
        // 4th texel: world rotation quaternion [x, y, z, w]. Identity = (0,0,0,1).
        float rotationX;
        float rotationY;
        float rotationZ;
        float rotationW;
    };
    #pragma pack(pop)
    
    struct Chunk { // Only exists during runtime. Contains all of the header data and our geometry and color data, along with any extra runtime data not used in rendering.
        ChunkHeader header;
        std::vector<uint32_t> geometryData;
        std::vector<uint32_t> voxelTypeData;
        VoxelBatch chunkQueue;
        uint32_t LOD;
        // Instancing: when >= 0, this chunk's geometry lives once in Scene.geometryPool[idx]
        // (shared across every instance of the same .data block) and geometryData/voxelTypeData
        // above are left empty. -1 = unpooled (interned into the pool by internChunkGeometry before
        // any GPU work; see internChunkGeometry in scene.h).
        int32_t geometryPoolIndex = -1;
        // Persistence policy for this instance (from its compose.json component). Drives whether an
        // edit is written back (Direct), copied off (Copy), or in-memory only (Locked).
        Mutability mutability = Mutability::Locked;
        // Copy-on-write bookkeeping: set once a Copy instance has forked its own private pool blob.
        bool copyDiverged = false;
        // Slot liveness. A removed chunk keeps its slot in Scene.chunks (so no handle shifts) but is
        // marked dead and its handle recycled via Scene.chunkFreeList. Dead slots get a degenerate GPU
        // header row (scale <= 0) that the shader skips. Loaders create chunks alive.
        bool alive = true;
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
    struct GeometryBlob {
        std::vector<uint32_t> geometry;
        std::vector<uint32_t> voxelTypeData;
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
        // Whether utils::addVoxelsToComponent may grow this grid's dims/cellToChunk (and shift
        // origin) to include a voxel outside current bounds, rather than skipping it. Default on --
        // flip off for a grid that should stay a fixed size no matter what an edit asks for.
        bool resizeToFitVoxels = true;
        // How far this grid's local cell index 0 has drifted from the on-disk block table's (0,0,0)
        // -- i.e. fileBlockCoord = localCellCoord + cellCoordOffset. Starts at zero (local index ==
        // file coord, today's assumption); utils::expandGridToInclude updates it whenever growth
        // shifts the local coordinate origin (e.g. growing in a negative direction), so
        // materializeGridCell's on-disk block lookup stays correct after a resize.
        core::ivec3 cellCoordOffset = core::ivec3(0);
    };

    struct Scene {
        // Slot-indexed with holes: a chunk's index is its stable ChunkHandle and its GPU header row.
        // Removed chunks are marked !alive and their slots pushed to chunkFreeList for reuse; the
        // vector never shifts, so grids[] and looseChunks[] handles stay valid across add/remove.
        std::vector<Chunk> chunks;
        std::vector<ChunkHandle> chunkFreeList; // recycled dead chunk slots (LIFO).
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
        // Refcounted (GeometryBlob.refCount); empty slots recycled via blobFreeList so live pool
        // indices stay stable (never compacted).
        std::vector<GeometryBlob> geometryPool;
        std::vector<uint32_t> blobFreeList; // recycled empty geometryPool slots (LIFO).
        // One entry per loaded compose component, indexed by ComponentHandle. Populated by
        // loadComposeFromDisk; never compacted or reordered (handles are stable for the Scene's life).
        std::vector<ComponentRecord> components;
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
    inline int32_t internChunkGeometry(Scene& scene, Chunk& chunk) {
        if (chunk.geometryPoolIndex >= 0) return chunk.geometryPoolIndex;
        GeometryBlob blob;
        blob.geometry = std::move(chunk.geometryData);
        blob.voxelTypeData = std::move(chunk.voxelTypeData);
        blob.refCount = 1;
        chunk.geometryData.clear();
        chunk.voxelTypeData.clear();
        chunk.geometryPoolIndex = poolInsertBlob(scene, std::move(blob));
        return chunk.geometryPoolIndex;
    }

    // What resolveComponentLocation resolves a ChunkHandle to: which component owns it, and how to
    // promote a coordinate local to THAT SPECIFIC CHUNK into the component's own voxel space (add
    // this to a chunk-local coordinate to get a component-voxel-space coordinate).
    struct ComponentLocation {
        ComponentHandle component;
        core::ivec3 voxelSpaceOrigin;
    };

    // Resolves a ChunkHandle (e.g. from GPU picking, which is inherently chunk-granular -- the
    // handle IS the GPU header row) to the component that owns it. A loose chunk's voxel space
    // already IS its component's voxel space (identity origin); a grid-resident chunk's component is
    // the whole grid, so its origin is that cell's placement within the grid's voxel space (this
    // cell's coordinate, decomposed from Chunk.cellIndex the same way streaming.cpp's internal
    // cellToCoord does, times the shared per-cell resolution already on Chunk.header). Callers (e.g.
    // PathTracer's picking) do their own chunk-local coordinate math exactly as before and only need
    // this once, right before calling utils::addVoxelsToComponent/removeVoxelsFromComponent.
    inline ComponentLocation resolveComponentLocation(const Scene& scene, ChunkHandle handle) {
        const Chunk& chunk = scene.chunks[handle];
        if (chunk.gridIndex < 0) {
            return { chunk.componentHandle, core::ivec3(0) };
        }
        const SceneGrid& grid = scene.grids[chunk.gridIndex];
        int plane = grid.dims.x * grid.dims.y;
        int cellZ = chunk.cellIndex / plane;
        int rem = chunk.cellIndex % plane;
        int cellY = rem / grid.dims.x;
        int cellX = rem % grid.dims.x;
        core::ivec3 cellCoord(cellX, cellY, cellZ);
        return { grid.componentHandle, (cellCoord + grid.cellCoordOffset) * static_cast<int>(chunk.header.resolution) };
    }
}

#endif
