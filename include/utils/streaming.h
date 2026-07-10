#ifndef PROJV_STREAMING_H
#define PROJV_STREAMING_H

#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

#include "data_structures/scene.h"
#include "data_structures/compose.h"
#include "graphics/scene_dynamics.h"

// Streaming MECHANISMS (engine core, policy-free). The engine provides per-block disk IO, a way to
// materialize/release a grid cell, and a queryable view of the on-disk index — but NOT the decision of
// which cells should be resident. That decision is user policy (a system that keys on cameras, network
// interest, gameplay, etc.); the example app ships one interest-source policy, selectable/replaceable
// like a renderer module. See docs/data_structures/compose_data_structure.md.
namespace projv::utils {
    // Per-grid streaming metadata (parallel to Scene.grids), tiny and always resident: exactly enough to
    // synthesize any cell's chunk header on demand without re-walking the transform graph. The grid's
    // SceneGrid descriptor (origin/cellSize/dims/rotation) supplies placement; this supplies the rest.
    struct GridStreamRecord {
        std::string sourceDataPath;         // the .data file this grid volume streams from
        uint32_t    resolution = 0;         // shared block edge resolution
        float       dataVoxelScale = 0.0f;  // the file's voxelScale (before world uniform scale)
        float       uniformScale = 1.0f;    // world uniform scale applied to this grid instance
        Mutability  mutability = Mutability::Locked;
    };

    // LRU cache of .data block tables (DataFileHeader), keyed by file path. The block table IS the
    // streaming index (40 bytes/block on disk), so this is the ONLY place index memory is held — and it
    // is bounded by `budget`, a policy choice. The engine never pins every table; it reads on demand.
    struct BlockTableCache {
        size_t budget = 64; // max cached file tables; policy-set. 0 disables caching (always re-read).
        std::unordered_map<std::string, DataFileHeader> tables;
        std::unordered_map<std::string, uint64_t> lastUsed;
        uint64_t tick = 0;
    };

    // Engine-owned streaming state. No camera, no policy: the resident-blob dedup map, the bounded
    // on-disk index cache, the per-grid records, and the mutation queue a policy fills then applies.
    struct StreamingContext {
        std::vector<GridStreamRecord> gridRecords;                    // parallel to Scene.grids
        std::unordered_map<std::string, int32_t> poolKeyToIndex;      // dedup of resident blobs
        BlockTableCache blockTables;
        std::vector<graphics::PendingSceneMutation> mutations;        // drained by applySceneMutations
    };

    /**
     * Returns a grid's on-disk block table, reading + LRU-caching it on demand. Lets any policy discover
     * a grid's occupied cells and their sizes without the engine pinning every table in memory. The
     * reference is valid until the next streamGridBlockTable call (which may evict); use it immediately.
     */
    const DataFileHeader& streamGridBlockTable(StreamingContext& ctx, const Scene& scene, int gridIndex);

    /**
     * Makes a grid cell resident (mechanism, policy-free): if already resident, returns its handle; else
     * reads the cell's block, dedup-or-inserts its geometry blob (refcounted), creates a Chunk with the
     * synthesized header + residency key, marks the cell resident, and enqueues an Add mutation. The GPU
     * upload happens when the queue is applied (applySceneMutations). Returns the chunk handle, or
     * INVALID_CHUNK_HANDLE if the cell is empty on disk or the indices are out of range.
     */
    ChunkHandle materializeGridCell(Scene& scene, StreamingContext& ctx, int gridIndex, int cellIndex);

    /**
     * Releases a resident grid cell (mechanism, policy-free): enqueues a Remove mutation for the cell's
     * chunk. Applying the queue frees the blob's GPU range at refcount 0, recycles the chunk + pool
     * slots, and clears the cell. No-op if the cell is not resident.
     */
    void releaseGridCell(Scene& scene, StreamingContext& ctx, int gridIndex, int cellIndex);

    /**
     * Glue that connects the CPU edit path to the GPU: bakes a pooled chunk's queued voxel edit into its
     * geometry blob (editChunkVoxels, honoring mutability/COW) and enqueues an Update mutation so the
     * change is reflected on the GPU when the queue is applied.
     */
    void applyChunkEdit(Scene& scene, std::vector<graphics::PendingSceneMutation>& queue, ChunkHandle handle);

    // Returned by materializeGridCell when a cell cannot be made resident.
    static constexpr ChunkHandle INVALID_CHUNK_HANDLE = 0xFFFFFFFFu;

    /**
     * Creates a fresh, empty chunk for a grid cell that has no on-disk block (never authored, not
     * just unloaded) -- the sibling of materializeGridCell for building into genuinely empty grid
     * space. No-op (returns the existing handle) if the cell is already resident. The resulting
     * blob has no source file yet (sourceDataPath empty); persisting it back to disk isn't handled
     * by persistChunkData yet (a documented follow-up, not required for editing/rendering).
     * Returns INVALID_CHUNK_HANDLE if gridIndex/cellIndex are out of range.
     */
    ChunkHandle materializeEmptyGridCell(Scene& scene, StreamingContext& ctx, int gridIndex, int cellIndex);

    /**
     * Grows `grid`'s dims/cellToChunk (and shifts origin, in any direction including negative) so
     * that cellCoord falls inside [0, dims). No-op if already in bounds. Already-resident cells keep
     * their exact world position -- cellToChunk is re-indexed into the new array and origin/
     * cellCoordOffset are corrected by the same shift, so nothing already placed moves.
     */
    void expandGridToInclude(Scene& scene, SceneGrid& grid, core::ivec3 cellCoord);

    /**
     * THE editing mechanism, addressed by component (see ComponentHandle, data_structures/scene.h)
     * instead of a raw ChunkHandle/gridIndex, so callers don't need to know or care whether a
     * component turned out to be one loose chunk or a whole grid. `voxels` positions are in the
     * component's own voxel space: a loose component's own local integer index (unchanged from
     * today), or one continuous integer index across a grid component's whole extent
     * (voxelIndex = cellCoord * resolution + localIndex, pure integer math -- see EditVoxel in
     * data_structures/voxel.h for why this can't just be a Voxel/VoxelBatch).
     *
     * Reads each affected chunk's ACTUAL existing content (pool-aware -- utils::getChunkVoxelBatch),
     * merges in the requested voxels, and pushes exactly one Update mutation into ctx.mutations per
     * affected chunk -- one for a loose component, one per grid cell actually touched for a grid
     * component, regardless of how many voxels were in the batch.
     *
     * Grid path: buckets voxels by cell, growing the grid via expandGridToInclude for any cell
     * outside current bounds when grid.resizeToFitVoxels is true (skipped with a warning otherwise).
     * addVoxelsToComponent additionally creates any missing chunk on demand (materializeGridCell if
     * an on-disk block exists there, else materializeEmptyGridCell); removeVoxelsFromComponent never
     * creates a chunk just to find nothing to remove from it -- an unauthored/non-resident cell is a
     * silent no-op for a remove.
     *
     * Manual alternative (different residency policy): call utils::materializeGridCell /
     * utils::releaseGridCell yourself instead of this convenience -- both stay public, unchanged.
     */
    void addVoxelsToComponent(Scene& scene, StreamingContext& ctx, ComponentHandle component,
                               const EditVoxelBatch& voxels);
    void removeVoxelsFromComponent(Scene& scene, StreamingContext& ctx, ComponentHandle component,
                                    const EditVoxelBatch& voxels);
}

#endif
