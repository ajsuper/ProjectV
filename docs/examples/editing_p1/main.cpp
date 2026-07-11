// Phase 1 editing test driver.
//
// Loads a compose scene (folder arg or default SponzaScene), then exercises the new editing API:
//   1. queueVoxelAdd / queueVoxelRemove on a loose (Chunk-kind) component.
//   2. queueVoxelAdd on a Grid-kind component -- must return false (P2 territory).
//   3. updateScene -- must drain the loose component's queue, fork its blob, and rebuild.
//   4. After update, the component's queue is empty and its chunk's geometryPoolIndex points
//      at a fork with refCount == 1 and non-empty geometry.
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
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

namespace fs = std::filesystem;

namespace {
    int g_failures = 0;

    void check(bool cond, const char* msg) {
        if (cond) {
            projv::core::info("[OK]   {}", msg);
        } else {
            projv::core::error("[FAIL] {}", msg);
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
            if (scene.components[h].kind == projv::ComponentKind::Chunk && looseOut == projv::INVALID_COMPONENT_HANDLE) {
                looseOut = h;
            } else if (scene.components[h].kind == projv::ComponentKind::Grid && gridOut == projv::INVALID_COMPONENT_HANDLE) {
                gridOut = h;
            }
            if (looseOut != projv::INVALID_COMPONENT_HANDLE && gridOut != projv::INVALID_COMPONENT_HANDLE) break;
        }
    }
}

int main(int argc, char** argv) {
    // Allow custom scene folder, default to the bundled Sponza scene.
    std::string scenePath = (argc > 1) ? argv[1]
                                       : "docs/examples/PathTracer/SponzaScene";
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
            ? scene.geometryPool[origPoolIdx].voxelTypeData.size() / 3
            : 0;

        // Queue three adds inside bounds + one out-of-bounds (must be skipped with warn).
        std::vector<projv::PendingVoxelOp> adds{
            {false, {1, 2, 3},   projv::Color{255, 0,   0}},
            {false, {4, 5, 6},   projv::Color{0,   255, 0}},
            {false, {7, 8, 9},   projv::Color{0,   0,   255}},
            {false, {9999,0, 0}, projv::Color{0,   0,   0}}, // OOB; should be skipped
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
        check(newPoolIdx != origPoolIdx, "chunk points at a NEW pool index (COW fork)");

        // The original blob's refCount must NOT be decremented (forkBlob spec).
        if (origPoolIdx >= 0 && static_cast<size_t>(origPoolIdx) < scene.geometryPool.size()) {
            check(scene.geometryPool[origPoolIdx].refCount == origRefCount,
                  "original blob refCount unchanged (fork spec)");
        }

        // The new fork must have refCount == 1 and a populated geometry array.
        if (newPoolIdx >= 0 && static_cast<size_t>(newPoolIdx) < scene.geometryPool.size()) {
            const projv::GeometryBlob& fork = scene.geometryPool[newPoolIdx];
            check(fork.refCount == 1, "forked blob refCount == 1");
            check(!fork.geometry.empty(), "forked blob has non-empty geometry");

            // Three adds were applied (the OOB op was skipped). Voxel count must have grown.
            size_t newVoxelCount = fork.voxelTypeData.size() / 3;
            check(newVoxelCount > origVoxelCount,
                  "voxel count grew after adds (orig -> new)");
        }

        // Pool grew by at least one entry.
        check(scene.geometryPool.size() > origBlobCount,
              "geometryPool size grew (fork added)");

        // DataReference for this component was allocated.
        check(comp.dataRefID >= 0, "component.dataRefID assigned");
        check(static_cast<size_t>(comp.dataRefID) < scene.dataReferences.size(),
              "dataRefID in range");
    }

    // Grid-kind rejection.
    if (grid != projv::INVALID_COMPONENT_HANDLE) {
        projv::ComponentRecord& gcomp = scene.components[grid];
        std::vector<projv::PendingVoxelOp> ops{
            {false, {0, 0, 0}, projv::Color{255, 255, 255}}
        };
        size_t before = gcomp.editQueue.ops.size();
        bool ok = projv::utils::queueVoxelAdd(scene, grid, ops);
        check(!ok, "queueVoxelAdd on Grid component returned false (P1 scope)");
        check(gcomp.editQueue.ops.size() == before,
              "Grid component's queue unchanged after rejected queueVoxelAdd");
    } else {
        projv::core::info("No Grid component in scene -- grid-rejection path skipped.");
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
        projv::VoxelBatch seed;
        for (int i = 0; i < 5; ++i)
            seed.push_back(projv::utils::createVoxel(
                projv::Color{128, 128, 128},
                projv::core::ivec3(i * 2, i * 2, i * 2)));
        projv::utils::moveVoxelBatchToChunk(seed, chunk);
        projv::utils::updateChunkFromItsVoxelBatch(chunk, /*clearBatch=*/true);

        // Intern into a pool blob.
        int32_t blobIdx = projv::internChunkGeometry(testScene, chunk);
        check(blobIdx >= 0, "programmatic: internChunkGeometry succeeded");
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
            testScene.geometryPool[0].voxelTypeData.size() / 3u;
        check(origVoxelCount == 5, "programmatic: seed has 5 voxels");

        // Queue 3 adds at positions that don't collide with the seed (seed has 0,2,4,6,8).
        std::vector<projv::PendingVoxelOp> adds{
            {false, {10, 10, 10}, projv::Color{255, 0, 0}},
            {false, {12, 12, 12}, projv::Color{0, 255, 0}},
            {false, {14, 14, 14}, projv::Color{0, 0, 255}},
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
        check(newPoolIdx != blobIdx, "programmatic: COW fork to new blob index");

        // Original blob refCount unchanged.
        if (static_cast<size_t>(blobIdx) < testScene.geometryPool.size()) {
            check(testScene.geometryPool[blobIdx].refCount == 1,
                  "programmatic: original blob refCount unchanged");
        }

        // Forked blob exists with refCount 1 and non-empty geometry.
        if (newPoolIdx >= 0 && static_cast<size_t>(newPoolIdx) < testScene.geometryPool.size()) {
            const projv::GeometryBlob& fork = testScene.geometryPool[newPoolIdx];
            check(fork.refCount == 1, "programmatic: fork refCount == 1");
            check(!fork.geometry.empty(), "programmatic: fork has geometry");

            uint32_t newVoxelCount =
                static_cast<uint32_t>(fork.voxelTypeData.size() / 3u);
            check(newVoxelCount == origVoxelCount + 3,
                  "programmatic: voxel count grew by 3");
        }

        check(testScene.components[0].dataRefID >= 0,
              "programmatic: dataRefID assigned");
        check(static_cast<size_t>(testScene.components[0].dataRefID) <
                  testScene.dataReferences.size(),
              "programmatic: dataRefID in range");
    }

    if (g_failures == 0) {
        projv::core::info("Phase 1 editing: all checks passed.");
        return 0;
    } else {
        projv::core::error("Phase 1 editing: {} check(s) failed.", g_failures);
        return 1;
    }
}