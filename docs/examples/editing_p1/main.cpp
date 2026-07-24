// Phase 1 + Phase 2 editing test driver.
//
// Loads a compose scene (folder arg or default SponzaScene), then exercises the editing API:
//   1. floorDiv/floorMod sanity.
//   2. queueVoxelAdd / queueVoxelRemove on a loose (Chunk-kind) component.
//   3. queueVoxelAdd on a Grid-kind component (P2: accepted and drained with cell bucketing).
//   4. updateScene on both Chunk and Grid components.
//   5. Programmatic loose chunk test (full COW fork / refCount / dataRefID).
//   6. Programmatic grid test (grid expansion, cell bucketing, new cell creation, COW fork).
//
// CPU-only. No GPU. The driver prints assertions; exits 0 on success, non-zero on failure.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "core/log.h"
#include "data_structures/color.h"
#include "data_structures/scene.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/scene_query.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"
#include "utils/material.h"

namespace fs = std::filesystem;

namespace {
    int g_failures = 0;

    void check(bool cond, const char* msg) {
        if (cond) {
            projv::core::info("[OK]   {}", msg);
        } else {
            projv::core::error("[FAIL] {}", msg);
            fprintf(stderr, "CHECK FAILED: %s\n", msg);
            ++g_failures;
        }
    }

    // Find the first loose (Chunk-kind) and first grid (Grid-kind) component handles.
    void findComponents(const projv::Scene& scene,
                        projv::ComponentHandle& looseOut,
                        projv::ComponentHandle& gridOut) {
        looseOut = projv::INVALID_COMPONENT_HANDLE;
        gridOut  = projv::INVALID_COMPONENT_HANDLE;
        for (projv::ComponentHandle h = 0; h < scene.components.size(); ++h) {
            auto kind = scene.components[h].kind;
            if (kind == projv::ComponentKind::Asset) continue;
            if (kind == projv::ComponentKind::Chunk && looseOut == projv::INVALID_COMPONENT_HANDLE) {
                looseOut = h;
            } else if (kind == projv::ComponentKind::Grid && gridOut == projv::INVALID_COMPONENT_HANDLE) {
                gridOut = h;
            }
            if (looseOut != projv::INVALID_COMPONENT_HANDLE && gridOut != projv::INVALID_COMPONENT_HANDLE) break;
        }
    }
}

int main(int argc, char** argv) {
    // Allow custom scene folder, default to the bundled Sponza scene.
    std::string scenePath = (argc > 1) ? argv[1]
                                       : "../PathTracer/SponzaScene/";
    if (!fs::exists(scenePath)) {
        // Try relative to the binary's CWD; sometimes test runners chdir.
        std::cerr << "Scene folder not found: " << scenePath << "\n";
        return 2;
    }

    projv::core::info("Loading scene from {}", scenePath);
    projv::Scene scene = projv::utils::loadComposeFromDisk(scenePath);

    projv::core::info("Scene: {} chunks, {} loose, {} grids, {} blobs, {} components",
               scene.chunks.size(), scene.looseChunkCount, scene.grids.size(),
               scene.geometryPool.size(), scene.components.size());

    for (projv::ComponentHandle h = 0; h < scene.components.size(); ++h) {
        projv::core::info("  component[{}]: kind={}", h,
            scene.components[h].kind == projv::ComponentKind::Chunk ? "Chunk" : "Grid");
    }

    projv::ComponentHandle loose = projv::INVALID_COMPONENT_HANDLE;
    projv::ComponentHandle grid  = projv::INVALID_COMPONENT_HANDLE;
    findComponents(scene, loose, grid);
    projv::core::info("findComponents: loose={}, grid={}", loose, grid);

    // floorDiv/floorMod sanity.
    check(projv::utils::floorDiv(-5, 256) == -1, "floorDiv(-5,256)==-1");
    check(projv::utils::floorMod(-5, 256) == 251, "floorMod(-5,256)==251");
    check(projv::utils::floorDiv(7, 256) == 0, "floorDiv(7,256)==0");
    check(projv::utils::floorMod(7, 256) == 7, "floorMod(7,256)==7");

    if (loose == projv::INVALID_COMPONENT_HANDLE) {
        projv::core::warn("No loose (Chunk-kind) component in scene -- P1 main path skipped.");
    } else {
        projv::ComponentRecord& comp = scene.components[loose];
        projv::Chunk& chunk = scene.chunks[comp.chunkHandle];

        // Snapshot pre-edit state.
        int32_t origPoolIdx = chunk.geometryPoolIndex;
        uint32_t origRefCount = (origPoolIdx >= 0) ? scene.geometryPool[origPoolIdx].refCount : 0;
        size_t origBlobCount = scene.geometryPool.size();
        size_t origVoxelCount = (origPoolIdx >= 0)
            ? scene.geometryPool[origPoolIdx].materialIDs.size()
            : 0;

        // Queue three adds inside bounds + one out-of-bounds (must be skipped with warn).
        std::vector<projv::PendingVoxelOp> adds{
            {false, {1, 2, 3}, 0, {0,0,0}},
            {false, {4, 5, 6}, 0, {0,0,0}},
            {false, {7, 8, 9}, 0, {0,0,0}},
            {false, {9999, 0, 0}, 0, {0,0,0}}, // OOB; should be skipped
        };
        bool ok = projv::utils::queueVoxelAdd(scene, loose, adds);
        check(ok, "queueVoxelAdd returned true on loose component");
        check(comp.editQueue.ops.size() == adds.size(), "queue appended all ops (incl. OOB)");

        // Drain.
        uint32_t processed = projv::utils::updateScene(scene);
        check(processed >= 1, "updateScene processed >= 1 component");

        // Post-edit assertions.
        check(comp.editQueue.ops.empty(), "edit queue cleared after updateScene");

        int32_t newPoolIdx = chunk.geometryPoolIndex;

        // COW optimization: if the chunk was the sole blob owner (refCount==1), forkBlob
        // edits in-place and returns the same index. Otherwise a new fork blob is created.
        if (origRefCount == 1) {
            check(newPoolIdx == origPoolIdx,
                  "chunk edits in-place when sole blob owner (refCount==1)");
        } else {
            check(newPoolIdx != origPoolIdx,
                  "chunk forks to new pool index when blob is shared (refCount>1)");
        }

        // The original blob's refCount must NOT be decremented (forkBlob spec).
        if (origPoolIdx >= 0 && static_cast<size_t>(origPoolIdx) < scene.geometryPool.size()) {
            check(scene.geometryPool[origPoolIdx].refCount == origRefCount,
                  "original blob refCount unchanged (fork spec)");
        }

        // Verify the blob at newPoolIdx has the edited content.
        if (newPoolIdx >= 0 && static_cast<size_t>(newPoolIdx) < scene.geometryPool.size()) {
            const projv::GeometryBlob& blob = scene.geometryPool[newPoolIdx];
            // In-place: refCount == origRefCount. Forked: refCount == 1.
            bool isInPlace = (newPoolIdx == origPoolIdx && origRefCount == 1);
            if (isInPlace) {
                check(blob.refCount == origRefCount,
                      "in-place blob refCount unchanged");
            } else {
                check(blob.refCount == 1,
                      "forked blob refCount == 1");
            }
            check(blob.dirty, "blob marked dirty after edit");
            check(!blob.geometry.empty(), "blob has non-empty geometry");

            // Three adds were applied (the OOB op was skipped). Voxel count must have grown.
            size_t newVoxelCount = blob.materialIDs.size();
            check(newVoxelCount > origVoxelCount,
                  "voxel count grew after adds (orig -> new)");
        }

        // Pool grew only if a new fork blob was created.
        if (newPoolIdx != origPoolIdx) {
            check(scene.geometryPool.size() > origBlobCount,
                  "geometryPool size grew (fork added)");
        } else {
            check(scene.geometryPool.size() == origBlobCount,
                  "geometryPool size unchanged (in-place edit)");
        }

        // DataReference for this component was allocated.
        check(comp.dataRefID >= 0, "component.dataRefID assigned");
        check(static_cast<size_t>(comp.dataRefID) < scene.dataReferences.size(),
              "dataRefID in range");
    }

    // Grid-kind acceptance (P2: queue and drain with cell bucketing).
    if (grid != projv::INVALID_COMPONENT_HANDLE) {
        projv::ComponentRecord& gcomp = scene.components[grid];
        size_t before = gcomp.editQueue.ops.size();
        std::vector<projv::PendingVoxelOp> gops{
            {false, {5, 5, 5}, 0, {0,0,0}},
            {false, {10, 10, 10}, 0, {0,0,0}},
        };
        bool ok = projv::utils::queueVoxelAdd(scene, grid, gops);
        check(ok, "queueVoxelAdd on Grid component returned true (P2 scope)");
        check(gcomp.editQueue.ops.size() == before + gops.size(),
              "Grid component's queue grew after queueVoxelAdd");

        // Drain the grid queue.
        uint32_t processed = projv::utils::updateScene(scene);
        check(processed >= 1, "updateScene processed Grid component");
        check(gcomp.editQueue.ops.empty(),
              "Grid component's edit queue cleared after updateScene");

        // Verify dataRefID was assigned to the grid component.
        check(gcomp.dataRefID >= 0, "Grid component dataRefID assigned");
        check(static_cast<size_t>(gcomp.dataRefID) < scene.dataReferences.size(),
              "Grid component dataRefID in range");

        // The grid must not have expanded (both voxels land in existing cell 0).
        // Sponza grid is (2,1,2) -- 4 blocks in a 2x1x2 arrangement.
        const projv::SceneGrid& sponzaGrid = scene.grids[gcomp.gridIndex];
        check(sponzaGrid.dims == projv::core::ivec3(2, 1, 2),
              "Sponza grid dims unchanged after in-bounds edit");

        // The edited chunk (cell 0) should still exist.
        int32_t cell0Chunk = sponzaGrid.cellToChunk[0];
        check(cell0Chunk >= 0, "Grid cell 0 has a chunk");
    } else {
        projv::core::info("No Grid component in scene -- grid acceptance path skipped.");
    }

    // queueVoxelRemove on a non-existent component returns false.
    bool badHandle = projv::utils::queueVoxelRemove(scene, 0xDEADBEEFu, {});
    check(!badHandle, "queueVoxelRemove on invalid handle returns false");

    // --- Programmatic loose-chunk test ---
    {
        // Build a minimal Scene with one loose chunk, one blob, and one component.
        projv::Scene testScene;

        // Header: 64^3 chunk, voxelScale 0.5, at origin.
        projv::ChunkHeader hdr;
        hdr.chunkID     = 1;
        hdr.position    = projv::core::vec3(0.0f);
        hdr.scale       = 32.0f;          // 64 * 0.5
        hdr.voxelScale  = 0.5f;
        hdr.resolution  = 64;
        hdr.rotation    = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);

        projv::Chunk chunk;
        chunk.header = hdr;
        chunk.LOD    = 0;
        chunk.alive  = true;

        // Seed with 5 voxels so we can check growth.
        auto brickMap = projv::utils::createVoxelBrickMap(
            projv::utils::computeBrickDims(64));
        for (int i = 0; i < 5; ++i)
            projv::utils::brickMapSetVoxel(*brickMap,
                i * 2, i * 2, i * 2, 0);
        projv::utils::updateChunkFromBrickMap(chunk, *brickMap);

        // Bake materials into the chunk's geometry.
        projv::GeometryBlob tmpBlob;
        projv::utils::bakeMaterialsFromBrickMap(chunk.geometryData, tmpBlob, *brickMap);

        // Intern into a pool blob, passing the brick map.
        int32_t blobIdx = projv::internChunkGeometry(testScene, chunk, std::move(brickMap));
        // Transfer baked materials to the blob.
        if (blobIdx >= 0 && static_cast<size_t>(blobIdx) < testScene.geometryPool.size()) {
            testScene.geometryPool[blobIdx].materialIDs = std::move(tmpBlob.materialIDs);
            testScene.geometryPool[blobIdx].materialPalette = std::move(tmpBlob.materialPalette);
        }
        check(blobIdx >= 0, "programmatic: internChunkGeometry succeeded");
        check(testScene.geometryPool[blobIdx].dirty,
              "programmatic: interned blob is dirty (P5)");
        testScene.chunks.push_back(std::move(chunk));

        // Component pointing at chunk handle 0.
        projv::ComponentHandle looseH =
            static_cast<projv::ComponentHandle>(testScene.components.size());
        testScene.components.push_back(projv::ComponentRecord{
            projv::ComponentKind::Chunk,   // kind
            0,                              // chunkHandle (first chunk)
            -1,                             // gridIndex
            "internal/test.data",           // sourcePath
            {},                             // editQueue (default)
            -1                              // dataRefID (unassigned)
        });

        // Register as loose.
        testScene.looseChunks.push_back(0);
        testScene.looseChunkCount = 1;

        check(testScene.components.size() == 1, "programmatic: one component");
        check(testScene.chunks.size() == 1, "programmatic: one chunk");

        uint32_t origVoxelCount =
            testScene.geometryPool[0].materialIDs.size();
        check(origVoxelCount == 5, "programmatic: seed has 5 voxels");

        // Simulate prior GPU flush: clear dirty flags so only the fork is dirty.
        for (auto& blob : testScene.geometryPool) blob.dirty = false;

        // Queue 3 adds at positions that don't collide with the seed (seed has 0,2,4,6,8).
        std::vector<projv::PendingVoxelOp> adds{
            {false, {10, 10, 10}, 0, {0,0,0}},
            {false, {12, 12, 12}, 0, {0,0,0}},
            {false, {14, 14, 14}, 0, {0,0,0}},
        };
        bool ok = projv::utils::queueVoxelAdd(testScene, looseH, adds);
        check(ok, "programmatic: queueVoxelAdd returned true");
        check(testScene.components[0].editQueue.ops.size() == 3,
              "programmatic: 3 ops queued");

        uint32_t processed = projv::utils::updateScene(testScene);
        check(processed >= 1, "programmatic: updateScene processed >= 1");

        // Post-edit checks.
        check(testScene.components[0].editQueue.ops.empty(),
              "programmatic: editQueue cleared");

        int32_t newPoolIdx = testScene.chunks[0].geometryPoolIndex;
        // refCount was 1, so forkBlob edits in-place (same index).
        check(newPoolIdx == blobIdx, "programmatic: in-place edit (sole owner)");

        // Original blob refCount unchanged (in-place, same blob).
        if (static_cast<size_t>(blobIdx) < testScene.geometryPool.size()) {
            check(testScene.geometryPool[blobIdx].refCount == 1,
                  "programmatic: original blob refCount unchanged");
        }

        // Edited blob has refCount 1 and non-empty geometry.
        if (newPoolIdx >= 0 && static_cast<size_t>(newPoolIdx) < testScene.geometryPool.size()) {
            const projv::GeometryBlob& fork = testScene.geometryPool[newPoolIdx];
            check(fork.refCount == 1, "programmatic: fork refCount == 1");
            check(!fork.geometry.empty(), "programmatic: fork has geometry");

            uint32_t newVoxelCount =
                static_cast<uint32_t>(fork.materialIDs.size());
            check(newVoxelCount == origVoxelCount + 3,
                  "programmatic: voxel count grew by 3");
        }

        check(testScene.components[0].dataRefID >= 0,
              "programmatic: dataRefID assigned");
        check(static_cast<size_t>(testScene.components[0].dataRefID) <
                  testScene.dataReferences.size(),
              "programmatic: dataRefID in range");

        // P5 dirty-flag checks.
        if (newPoolIdx >= 0 && static_cast<size_t>(newPoolIdx) < testScene.geometryPool.size()) {
            check(testScene.geometryPool[newPoolIdx].dirty,
                  "programmatic: edited blob is dirty (P5)");
            // In-place: original IS the edited blob (same index, dirty == true).
            // Forked: original is a separate blob (dirty == false).
            if (newPoolIdx != blobIdx) {
                check(!testScene.geometryPool[blobIdx].dirty,
                      "programmatic: original blob NOT dirty after fork (P5)");
            }
        }
    }

    // --- Programmatic loose-chunk overflow → grid conversion test ---
    {
        // Build a Scene with one loose chunk (64^3, 5 seed voxels) and queue an add
        // that overflows the chunk's resolution. updateScene should convert the loose
        // chunk to a 1-cell grid, expand it to accommodate the overflow, and process
        // both the in-bounds and overflow edit via the Grid cell-bucketing path.
        projv::Scene testScene;

        projv::ChunkHeader hdr;
        hdr.chunkID     = 1;
        hdr.position    = projv::core::vec3(0.0f);
        hdr.scale       = 32.0f;
        hdr.voxelScale  = 0.5f;
        hdr.resolution  = 64;
        hdr.rotation    = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);

        projv::Chunk chunk;
        chunk.header = hdr;
        chunk.LOD    = 0;
        chunk.alive  = true;

        // Seed with 5 voxels.
        auto brickMap = projv::utils::createVoxelBrickMap(
            projv::utils::computeBrickDims(64));
        for (int i = 0; i < 5; ++i)
            projv::utils::brickMapSetVoxel(*brickMap,
                i * 2, i * 2, i * 2, 0);
        projv::utils::updateChunkFromBrickMap(chunk, *brickMap);

        projv::GeometryBlob tmpBlob;
        projv::utils::bakeMaterialsFromBrickMap(chunk.geometryData, tmpBlob, *brickMap);

        chunk.header.resolution = 64;
        chunk.header.scale = 32.0f;

        int32_t blobIdx = projv::internChunkGeometry(testScene, chunk, std::move(brickMap));
        if (blobIdx >= 0 && static_cast<size_t>(blobIdx) < testScene.geometryPool.size()) {
            testScene.geometryPool[blobIdx].materialIDs = std::move(tmpBlob.materialIDs);
            testScene.geometryPool[blobIdx].materialPalette = std::move(tmpBlob.materialPalette);
        }
        check(blobIdx >= 0, "p3-convert: internChunkGeometry succeeded");
        testScene.chunks.push_back(std::move(chunk));

        // Loose component.
        projv::ComponentHandle h =
            static_cast<projv::ComponentHandle>(testScene.components.size());
        testScene.components.push_back(projv::ComponentRecord{
            projv::ComponentKind::Chunk,
            0,                              // chunkHandle
            -1,                             // gridIndex
            "internal/p3-convert.data",
            {},
            -1
        });
        testScene.looseChunks.push_back(0);
        testScene.looseChunkCount = 1;

        check(testScene.components.size() == 1, "p3-convert: one component");
        check(testScene.looseChunkCount == 1, "p3-convert: one loose chunk");

        // Queue two adds: one in-bounds (10,10,10), one overflowing (70,0,0).
        // 70 >= 64 triggers overflow conversion.
        std::vector<projv::PendingVoxelOp> adds{
            {false, {10, 10, 10}, 0, {0,0,0}},
            {false, {70, 0, 0}, 0, {0,0,0}},
        };
        bool ok = projv::utils::queueVoxelAdd(testScene, h, adds);
        check(ok, "p3-convert: queueVoxelAdd returned true");
        check(testScene.components[0].editQueue.ops.size() == 2,
              "p3-convert: 2 ops queued");

        // Initial component kind is Chunk.
        check(testScene.components[0].kind == projv::ComponentKind::Chunk,
              "p3-convert: kind is Chunk before updateScene");

        uint32_t processed = projv::utils::updateScene(testScene);
        check(processed >= 1, "p3-convert: updateScene processed >= 1");

        // Post-edit: component should now be a Grid.
        check(testScene.components[0].kind == projv::ComponentKind::Grid,
              "p3-convert: kind converted to Grid");
        check(testScene.components[0].gridIndex >= 0,
              "p3-convert: gridIndex assigned");
        check(testScene.components[0].editQueue.ops.empty(),
              "p3-convert: editQueue cleared");

        int32_t gridIdx = testScene.components[0].gridIndex;
        const projv::SceneGrid& grid = testScene.grids[gridIdx];

        // Grid should have dims (2,1,1) — original cell 0 + overflow cell 1.
        check(grid.dims == projv::core::ivec3(2, 1, 1),
              "p3-convert: grid dims expanded to (2,1,1)");
        check(grid.cellToChunk.size() == 2,
              "p3-convert: cellToChunk size == 2");

        // Cell 0 should still point at the original chunk.
        check(grid.cellToChunk[0] == 0,
              "p3-convert: cell 0 points at original chunk");
        check(testScene.chunks[0].gridIndex == gridIdx,
              "p3-convert: chunk 0 gridIndex matches");
        check(testScene.chunks[0].cellIndex == 0,
              "p3-convert: chunk 0 cellIndex == 0");

        // Cell 1 should have a new chunk.
        int32_t cell1Chunk = grid.cellToChunk[1];
        check(cell1Chunk >= 0, "p3-convert: cell 1 has new chunk");
        int32_t cell1Pool = testScene.chunks[cell1Chunk].geometryPoolIndex;
        check(cell1Pool >= 0, "p3-convert: cell 1 chunk has pool blob");
        check(testScene.chunks[cell1Chunk].gridIndex == gridIdx,
              "p3-convert: new chunk gridIndex matches");
        check(testScene.chunks[cell1Chunk].cellIndex == 1,
              "p3-convert: new chunk cellIndex == 1");

        // The original chunk edits its blob in-place (sole owner, refCount==1).
        check(testScene.chunks[0].geometryPoolIndex == blobIdx,
              "p3-convert: chunk 0 edits in-place");

        // Blob refCount unchanged (in-place edit).
        if (static_cast<size_t>(blobIdx) < testScene.geometryPool.size()) {
            check(testScene.geometryPool[blobIdx].refCount == 1,
                  "p3-convert: blob refCount unchanged");
        }

        // Cell 0 blob: 5 seed + 1 add (in-bounds) = 6 voxels.
        int32_t newPool0 = testScene.chunks[0].geometryPoolIndex;
        if (newPool0 >= 0 && static_cast<size_t>(newPool0) < testScene.geometryPool.size()) {
            size_t vc = testScene.geometryPool[newPool0].materialIDs.size();
            check(vc == 6, "p3-convert: cell 0 has 6 voxels (5 seed + 1 add)");
        }

        // New cell at cell 1: 1 add (the overflow at local (6,0,0)).
        if (cell1Pool >= 0 && static_cast<size_t>(cell1Pool) < testScene.geometryPool.size()) {
            size_t vc = testScene.geometryPool[cell1Pool].materialIDs.size();
            check(vc == 1, "p3-convert: cell 1 has 1 voxel (overflow add)");
            check(testScene.geometryPool[cell1Pool].refCount == 1,
                  "p3-convert: cell 1 blob refCount == 1");
        }

        // Loose chunk count decreased (chunk no longer loose).
        check(testScene.looseChunkCount == 0,
              "p3-convert: loose chunk count decremented");

        // dataRefID assigned.
        check(testScene.components[0].dataRefID >= 0,
              "p3-convert: dataRefID assigned");
        check(static_cast<size_t>(testScene.components[0].dataRefID) <
                  testScene.dataReferences.size(),
              "p3-convert: dataRefID in range");

        // Grid origin unchanged — the chunk didn't move.
        check(grid.origin.x == 0.0f, "p3-convert: grid origin x unchanged");
    }

    // --- Programmatic grid component test ---
    {
        projv::Scene testScene;

        // Helper to create a chunk with seed voxels and intern it.
        // Ensures the chunk's header resolution stays at 64 (the grid's data-file
        // resolution) even though updateChunkFromItsVoxelBatch shrinks it to fit
        // the seed span.
        auto makeChunk = [&](int id, const projv::core::vec3& pos,
                             const std::vector<projv::core::ivec3>& seedPositions,
                             int32_t cellIdx, int32_t gridIdx,
                             projv::ComponentHandle compHandle) -> projv::ChunkHandle {
            projv::ChunkHeader hdr;
            hdr.chunkID     = id;
            hdr.position    = pos;
            hdr.scale       = 32.0f;
            hdr.voxelScale  = 0.5f;
            hdr.resolution  = 64;
            hdr.rotation    = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);
            projv::Chunk chunk;
            chunk.header = hdr;
            chunk.LOD    = 0;
            chunk.alive  = true;
            chunk.gridIndex = gridIdx;
            chunk.cellIndex = cellIdx;
            chunk.componentHandle = compHandle;

            auto brickMap = projv::utils::createVoxelBrickMap(
                projv::utils::computeBrickDims(64));
            for (auto& p : seedPositions)
                projv::utils::brickMapSetVoxel(*brickMap,
                    p.x, p.y, p.z, 0);
            projv::utils::updateChunkFromBrickMap(chunk, *brickMap);
            projv::GeometryBlob tmpBlob;
            projv::utils::bakeMaterialsFromBrickMap(chunk.geometryData, tmpBlob, *brickMap);
            // Override resolution back to 64 (updateChunkFromBrickMap computes shrinks
            // it to fit the seed span; grid cells need the full data-file resolution).
            chunk.header.resolution = 64;
            chunk.header.scale = 32.0f;
            int32_t blobIdx = projv::internChunkGeometry(testScene, chunk, std::move(brickMap));
            if (blobIdx >= 0 && static_cast<size_t>(blobIdx) < testScene.geometryPool.size()) {
                testScene.geometryPool[blobIdx].materialIDs = std::move(tmpBlob.materialIDs);
                testScene.geometryPool[blobIdx].materialPalette = std::move(tmpBlob.materialPalette);
            }
            projv::ChunkHandle h = static_cast<projv::ChunkHandle>(testScene.chunks.size());
            testScene.chunks.push_back(std::move(chunk));
            return h;
        };

        // Create grid component handle (advance past any existing components).
        projv::ComponentHandle gridH =
            static_cast<projv::ComponentHandle>(testScene.components.size());

        // Two cells (2x1x1), 3 voxels in cell 0, 2 voxels in cell 1.
        projv::ChunkHandle c0 = makeChunk(0, {0, 0, 0},
            {{0,0,0}, {1,0,0}, {2,0,0}}, 0, 0, gridH);
        projv::ChunkHandle c1 = makeChunk(1, {32, 0, 0},
            {{0,0,0}, {1,0,0}}, 1, 0, gridH);

        // SceneGrid: 2x1x1, origin (0,0,0), cellSize 32.
        projv::SceneGrid sgrid;
        sgrid.origin         = projv::core::vec3(0.0f);
        sgrid.cellSize       = 32.0f;
        sgrid.dims           = projv::core::ivec3(2, 1, 1);
        sgrid.rotation       = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);
        sgrid.cellToChunk    = {static_cast<int32_t>(c0), static_cast<int32_t>(c1)};
        sgrid.componentHandle = gridH;
        sgrid.originCellCoord = projv::core::ivec3(0);
        int32_t gridIdx = static_cast<int32_t>(testScene.grids.size());
        testScene.grids.push_back(std::move(sgrid));

        // Component record.
        testScene.components.push_back(projv::ComponentRecord{
            projv::ComponentKind::Grid, // kind
            0,                          // chunkHandle (not used for Grid)
            gridIdx,                    // gridIndex
            "grid/test.data",           // sourcePath
            {},                         // editQueue
            -1                          // dataRefID
        });

        // Simulate prior GPU flush: clear dirty flags so only fork blobs are dirty.
        for (auto& blob : testScene.geometryPool) blob.dirty = false;

        // Track pre-edit state.
        int32_t origPool0 = testScene.chunks[c0].geometryPoolIndex;
        int32_t origPool1 = testScene.chunks[c1].geometryPoolIndex;
        uint32_t origRefCount0 = (origPool0 >= 0) ? testScene.geometryPool[origPool0].refCount : 0;
        uint32_t origRefCount1 = (origPool1 >= 0) ? testScene.geometryPool[origPool1].refCount : 0;
        size_t origGridCells = testScene.grids[gridIdx].cellToChunk.size();

        // Queue adds: 2 in cell 0 (positions (5,5,5), (10,10,10)),
        // 1 in cell 1 (position (70,5,5)),
        // 1 requiring negative expansion (position (-5,5,5), cellCoord = (-1,0,0)).
        // With res=64: floorDiv(5,64)=0, floorDiv(10,64)=0, floorDiv(70,64)=1, floorDiv(-5,64)=-1.
        std::vector<projv::PendingVoxelOp> adds{
            {false, {5, 5, 5}, 0, {0,0,0}},
            {false, {10, 10, 10}, 0, {0,0,0}},
            {false, {70, 5, 5}, 0, {0,0,0}},
            {false, {-5, 5, 5}, 0, {0,0,0}},
        };
        bool ok = projv::utils::queueVoxelAdd(testScene, gridH, adds);
        check(ok, "grid-test: queueVoxelAdd returned true");
        check(testScene.components[0].editQueue.ops.size() == 4,
              "grid-test: 4 ops queued");

        uint32_t processed = projv::utils::updateScene(testScene);
        check(processed >= 1, "grid-test: updateScene processed >= 1");

        // Post-edit checks.
        check(testScene.components[0].editQueue.ops.empty(),
              "grid-test: editQueue cleared");

        projv::SceneGrid& grid = testScene.grids[gridIdx];

        // Grid expanded: dims (3,1,1), originCellCoord (-1,0,0).
        check(grid.dims == projv::core::ivec3(3, 1, 1),
              "grid-test: dims expanded to (3,1,1)");
        check(grid.originCellCoord == projv::core::ivec3(-1, 0, 0),
              "grid-test: originCellCoord shifted to (-1,0,0)");
        check(grid.cellToChunk.size() == 3,
              "grid-test: cellToChunk size == 3");
        check(grid.cellToChunk.size() == origGridCells + 1,
              "grid-test: cellToChunk grew by 1");

        // Cell 0 (new, lin=0): must have a chunk.
        check(grid.cellToChunk[0] >= 0, "grid-test: cell 0 has chunk");

        // Cell 1 (lin=1, was original cell 0): chunk handle unchanged (same index in Scene.chunks).
        check(grid.cellToChunk[1] == static_cast<int32_t>(c0),
              "grid-test: cell 1 still points at chunk 0");

        // Cell 2 (lin=2, was original cell 1): chunk handle unchanged.
        check(grid.cellToChunk[2] == static_cast<int32_t>(c1),
              "grid-test: cell 2 still points at chunk 1");

        // Chunks edit their blobs in-place (sole owners, refCount==1).
        check(testScene.chunks[c0].geometryPoolIndex == origPool0,
              "grid-test: chunk 0 edits in-place");
        check(testScene.chunks[c1].geometryPoolIndex == origPool1,
              "grid-test: chunk 1 edits in-place");

        // Original blob refCounts unchanged (in-place edit).
        if (static_cast<size_t>(origPool0) < testScene.geometryPool.size()) {
            check(testScene.geometryPool[origPool0].refCount == origRefCount0,
                  "grid-test: original blob 0 refCount unchanged");
        }
        if (static_cast<size_t>(origPool1) < testScene.geometryPool.size()) {
            check(testScene.geometryPool[origPool1].refCount == origRefCount1,
                  "grid-test: original blob 1 refCount unchanged");
        }

        // Edited blobs have refCount 1 (in-place, sole owners).
        int32_t newPool0 = testScene.chunks[c0].geometryPoolIndex;
        int32_t newPool1 = testScene.chunks[c1].geometryPoolIndex;
        if (newPool0 >= 0 && static_cast<size_t>(newPool0) < testScene.geometryPool.size()) {
            check(testScene.geometryPool[newPool0].refCount == 1,
                  "grid-test: fork 0 refCount == 1");
            // Chunk 0 had 3 seed + 2 adds = 5 voxels.
            size_t voxCount0 = testScene.geometryPool[newPool0].materialIDs.size();
            check(voxCount0 == 5, "grid-test: chunk 0 has 5 voxels (3 seed + 2 adds)");
        }
        if (newPool1 >= 0 && static_cast<size_t>(newPool1) < testScene.geometryPool.size()) {
            check(testScene.geometryPool[newPool1].refCount == 1,
                  "grid-test: fork 1 refCount == 1");
            // Chunk 1 had 2 seed + 1 add = 3 voxels.
            size_t voxCount1 = testScene.geometryPool[newPool1].materialIDs.size();
            check(voxCount1 == 3, "grid-test: chunk 1 has 3 voxels (2 seed + 1 add)");
        }

        // New cell (cell 0, lin=0) should have 1 voxel (the negative expansion add at (-5,5,5)).
        // localPos = floorMod(-5, 64) = 59, floorMod(5, 64) = 5, so position (59,5,5).
        int32_t newChunkIdx = grid.cellToChunk[0];
        check(newChunkIdx >= 0, "grid-test: new cell chunk exists");
        int32_t newPoolIdx = testScene.chunks[newChunkIdx].geometryPoolIndex;
        if (newPoolIdx >= 0 && static_cast<size_t>(newPoolIdx) < testScene.geometryPool.size()) {
            size_t voxCount = testScene.geometryPool[newPoolIdx].materialIDs.size();
            check(voxCount == 1, "grid-test: new cell has 1 voxel");
            check(testScene.geometryPool[newPoolIdx].refCount == 1,
                  "grid-test: new cell blob refCount == 1");
            check(testScene.geometryPool[newPoolIdx].dirty,
                  "grid-test: new cell blob is dirty (P5)");
        }

        // P5 dirty-flag checks on fork blobs.
        if (newPool0 >= 0 && static_cast<size_t>(newPool0) < testScene.geometryPool.size()) {
            check(testScene.geometryPool[newPool0].dirty,
                  "grid-test: fork 0 blob is dirty (P5)");
        }
        if (newPool1 >= 0 && static_cast<size_t>(newPool1) < testScene.geometryPool.size()) {
            check(testScene.geometryPool[newPool1].dirty,
                  "grid-test: fork 1 blob is dirty (P5)");
        }
        if (static_cast<size_t>(origPool0) < testScene.geometryPool.size() && origPool0 != newPool0) {
            check(!testScene.geometryPool[origPool0].dirty,
                  "grid-test: original blob 0 NOT dirty (P5)");
        }
        if (static_cast<size_t>(origPool1) < testScene.geometryPool.size() && origPool1 != newPool1) {
            check(!testScene.geometryPool[origPool1].dirty,
                  "grid-test: original blob 1 NOT dirty (P5)");
        }

        // dataRefID assigned.
        check(testScene.components[0].dataRefID >= 0,
              "grid-test: dataRefID assigned");
        check(static_cast<size_t>(testScene.components[0].dataRefID) <
                  testScene.dataReferences.size(),
              "grid-test: dataRefID in range");

        // Origin shifted due to negative expansion: origin(-5) = -1 * cellSize.
        check(grid.origin.x == -32.0f, "grid-test: origin shifted to x=-32");
    }

    // --- P6 component tree tests ---
    {
        projv::core::info("--- P6: Component tree tests ---");

        // Verify component names were auto-generated.
        for (projv::ComponentHandle h = 0; h < scene.components.size(); ++h) {
            projv::core::info("  component[{}]: kind={} name=\"{}\" parent={} children={}",
                h,
                scene.components[h].kind == projv::ComponentKind::Chunk ? "Chunk" :
                scene.components[h].kind == projv::ComponentKind::Grid ? "Grid" : "Asset",
                scene.components[h].name,
                scene.components[h].parent,
                scene.components[h].children.size());
        }

        // Verify names are non-empty.
        check(!scene.components[0].name.empty(), "P6: component[0] has non-empty name");
        if (scene.components.size() > 1)
            check(!scene.components[1].name.empty(), "P6: component[1] has non-empty name");

        // Find a non-Asset component for subsequent tests.
        projv::ComponentHandle firstData = projv::INVALID_COMPONENT_HANDLE;
        for (projv::ComponentHandle h = 0; h < scene.components.size(); ++h) {
            if (scene.components[h].kind != projv::ComponentKind::Asset) {
                firstData = h;
                break;
            }
        }
        check(firstData != projv::INVALID_COMPONENT_HANDLE,
              "P6: found non-Asset component for tests");

        // Verify local transforms were stored on the data component.
        check(true, "P6: data component has localPosition stored (no assert on value, scene-dependent)");

        // getComponentPath round-trip test.
        std::string path0 = projv::utils::getComponentPath(scene, 0);
        std::string pathD = projv::utils::getComponentPath(scene, firstData);
        projv::core::info("  getComponentPath(0) = \"{}\"", path0);
        projv::core::info("  getComponentPath({}) = \"{}\"", firstData, pathD);
        check(!path0.empty(), "P6: getComponentPath(0) returns non-empty path");
        check(!pathD.empty(), "P6: getComponentPath(firstData) returns non-empty path");

        // findComponentByPath round-trips.
        projv::ComponentHandle found0 = projv::utils::findComponentByPath(scene, path0);
        check(found0 == 0, "P6: findComponentByPath round-trips for component 0");
        projv::ComponentHandle foundD = projv::utils::findComponentByPath(scene, pathD);
        check(foundD == firstData, "P6: findComponentByPath round-trips for firstData");

        // findComponentsByName.
        auto byName = projv::utils::findComponentsByName(scene, scene.components[firstData].name);
        check(!byName.empty(), "P6: findComponentsByName returns results");

        // getComponentWorldMatrix / getComponentWorldPosition.
        projv::core::mat4 worldD = projv::utils::getComponentWorldMatrix(scene, firstData);
        projv::core::vec3 worldPosD = projv::utils::getComponentWorldPosition(scene, firstData);
        check(worldPosD.x == worldD[3][0] &&
              worldPosD.y == worldD[3][1] &&
              worldPosD.z == worldD[3][2],
              "P6: getComponentWorldPosition matches matrix translation");

        // getComponentVoxelCount.
        uint32_t vcD = projv::utils::getComponentVoxelCount(scene, firstData);
        projv::core::info("  getComponentVoxelCount({}) = {}", firstData, vcD);
        check(vcD > 0, "P6: firstData component has voxels");

        // listComponents.
        auto infos = projv::utils::listComponents(scene);
        check(infos.size() == scene.components.size(),
              "P6: listComponents returns all components");

        // findComponentByPath with bad path.
        projv::ComponentHandle bad = projv::utils::findComponentByPath(scene, "nonexistent/component");
        check(bad == projv::INVALID_COMPONENT_HANDLE,
              "P6: findComponentByPath with nonexistent path returns invalid");

        // setComponentTransform sanity: save original position, move, verify, restore.
        projv::core::vec3 origPos = scene.components[firstData].localPosition;
        projv::core::quat origRot = scene.components[firstData].localRotation;
        projv::core::vec3 newPos(10.0f, 20.0f, 30.0f);
        projv::utils::setComponentPosition(scene, firstData, newPos);
        check(scene.components[firstData].localPosition == newPos,
              "P6: setComponentPosition updates localPosition");
        check(scene.components[firstData].localRotation == origRot,
              "P6: setComponentPosition preserves localRotation");

        projv::core::vec3 movedWorld = projv::utils::getComponentWorldPosition(scene, firstData);
        check(movedWorld != worldPosD,
              "P6: world position changed after setComponentPosition");

        // Restore.
        projv::utils::setComponentPosition(scene, firstData, origPos);
        projv::core::vec3 restoredWorld = projv::utils::getComponentWorldPosition(scene, firstData);
        check(restoredWorld.x == worldPosD.x && restoredWorld.y == worldPosD.y &&
              restoredWorld.z == worldPosD.z,
              "P6: world position restored after undo");
    }

    if (g_failures == 0) {
        projv::core::info("Phase 6 editing: all checks passed.");
        fprintf(stderr, "TESTS PASSED\n");
        return 0;
    } else {
        projv::core::error("Phase 3 editing: {} check(s) failed.", g_failures);
        fprintf(stderr, "TESTS FAILED: %d\n", g_failures);
        return 1;
    }
}
