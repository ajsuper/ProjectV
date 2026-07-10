#ifndef PATHTRACER_EDIT_DIAGNOSTICS_H
#define PATHTRACER_EDIT_DIAGNOSTICS_H

#include <cstdint>
#include <cmath>
#include <vector>
#include <iostream>

#include "core/math.h"
#include "core/log.h"
#include "data_structures/scene.h"
#include "data_structures/gpuData.h"
#include "data_structures/voxel.h"
#include "graphics/scene_dynamics.h"
#include "utils/streaming.h"
#include "utils/voxel_management.h"

// Diagnostic test modes for voxel editing system
// Test 1: Edit before scene upload (baseline verification)
// Test 2: Edit after scene upload with re-upload
// Test 3: Edit using optimized mutation path

struct DiagnosticState {
    int testMode = 0;
    int frameCount = 0;
    bool testExecuted = false;
    projv::ChunkHandle testChunkHandle = 0;
};

// Create a simple test chunk with a known pattern
inline projv::ChunkHandle createTestChunk(projv::Scene& scene, projv::core::vec3 position, float voxelScale) {
    projv::core::warn("EDITTEST: Creating test chunk at position ({}, {}, {})", position.x, position.y, position.z);
    
    projv::ChunkHandle handle;
    if (!scene.chunkFreeList.empty()) {
        handle = scene.chunkFreeList.back();
        scene.chunkFreeList.pop_back();
        scene.chunks[handle] = projv::Chunk{};
    } else {
        handle = static_cast<projv::ChunkHandle>(scene.chunks.size());
        scene.chunks.push_back(projv::Chunk{});
    }
    
    projv::Chunk& chunk = scene.chunks[handle];
    chunk.alive = true;
    chunk.header.chunkID = handle;
    chunk.header.position = position;
    chunk.header.voxelScale = voxelScale;
    chunk.header.rotation = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);
    chunk.gridIndex = -1;
    chunk.cellIndex = -1;
    chunk.LOD = 0;
    
    // Create a component record for this chunk (needed for editing system)
    projv::ComponentHandle compHandle = static_cast<projv::ComponentHandle>(scene.components.size());
    scene.components.push_back({projv::ComponentKind::Chunk, handle, -1, "diagnostic_test"});
    chunk.componentHandle = compHandle;
    
    // Create a simple 8x8x8 cube of voxels (positions 0-7 in each axis)
    projv::VoxelBatch batch;
    projv::Color testColor = {255, 0, 0}; // Red for visibility
    
    for (int z = 0; z < 8; z++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                projv::core::ivec3 pos(x, y, z);
                batch.push_back(projv::utils::createVoxel(testColor, pos));
            }
        }
    }
    
    projv::core::warn("EDITTEST: Created {} voxels in test chunk", batch.size());
    
    // Move batch to chunk and build geometry
    projv::utils::moveVoxelBatchToChunk(batch, chunk);
    projv::utils::updateChunkFromItsVoxelBatch(chunk);
    
    projv::core::warn("EDITTEST: Test chunk {} has resolution {} and scale {}", 
                      handle, chunk.header.resolution, chunk.header.scale);
    projv::core::warn("EDITTEST: Test chunk has {} geometry nodes and {} voxelType entries",
                      chunk.geometryData.size(), chunk.voxelTypeData.size());
    
    scene.looseChunks.push_back(handle);
    scene.looseChunkCount = static_cast<uint32_t>(scene.looseChunks.size());
    
    return handle;
}

// Apply a sphere edit to a chunk (before upload)
inline void applySphereEditToChunk(projv::Scene& scene, projv::ChunkHandle handle, 
                                   projv::core::ivec3 center, float radius, bool isAdd) {
    projv::core::warn("EDITTEST: Applying sphere edit to chunk {} at center ({}, {}, {}), radius {}, isAdd={}",
                      handle, center.x, center.y, center.z, radius, isAdd);
    
    projv::Chunk& chunk = scene.chunks[handle];
    if (!chunk.alive) {
        projv::core::error("EDITTEST: Chunk {} is not alive!", handle);
        return;
    }
    
    // Get existing voxels
    projv::VoxelBatch existing = projv::utils::getChunkVoxelBatch(scene, chunk, true);
    projv::core::warn("EDITTEST: Chunk has {} existing voxels before edit", existing.size());
    
    // Build sphere batch
    projv::VoxelBatch sphere;
    int r = static_cast<int>(std::ceil(radius));
    projv::Color addColor = {0, 255, 0}; // Green for adds
    
    for (int dz = -r; dz <= r; dz++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                projv::core::vec3 offset(static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz));
                if (glm::length(offset) > radius) continue;
                projv::core::ivec3 pos = center + projv::core::ivec3(dx, dy, dz);
                sphere.push_back(projv::utils::createVoxel(addColor, pos));
            }
        }
    }
    
    projv::core::warn("EDITTEST: Sphere contains {} voxels", sphere.size());
    
    // Apply edit
    if (isAdd) {
        projv::utils::addVoxelBatchAToVoxelBatchB(sphere, existing);
    } else {
        projv::utils::removeVoxelBatchAFromVoxelBatchB(sphere, existing);
    }
    
    projv::core::warn("EDITTEST: Chunk has {} voxels after edit", existing.size());
    
    // Rebuild geometry
    projv::utils::moveVoxelBatchToChunk(existing, chunk);
    projv::utils::updateChunkFromItsVoxelBatch(chunk);
    
    projv::core::warn("EDITTEST: After rebuild - resolution={}, scale={}, geometry={}, voxelType={}",
                      chunk.header.resolution, chunk.header.scale,
                      chunk.geometryData.size(), chunk.voxelTypeData.size());
}

// Test 1: Edit before scene upload
inline void runDiagnosticTest1_BeforeUpload(projv::Scene& scene, projv::GPUData& gpuData, DiagnosticState& state) {
    if (state.testExecuted) return;
    
    projv::core::warn("EDITTEST: ========== DIAGNOSTIC TEST 1: Edit Before Upload ==========");
    projv::core::warn("EDITTEST: Creating test chunk and applying edits BEFORE GPU upload");
    
    // Create test chunk
    state.testChunkHandle = createTestChunk(scene, projv::core::vec3(100.0f, 100.0f, 100.0f), 1.0f);
    
    // Apply a sphere add that EXTENDS BEYOND the cube (center at 10,4,4 with radius 3)
    // This covers x=7-13, y=1-7, z=1-7, so it partially overlaps and partially doesn't
    applySphereEditToChunk(scene, state.testChunkHandle, projv::core::ivec3(10, 4, 4), 3.0f, true);
    
    // Apply a sphere remove at a corner (this should work since it removes existing voxels)
    applySphereEditToChunk(scene, state.testChunkHandle, projv::core::ivec3(0, 0, 0), 2.0f, false);
    
    projv::core::warn("EDITTEST: ========== TEST 1 COMPLETE: Now uploading to GPU ==========");
    projv::core::warn("EDITTEST: Expected: Red cube with green voxels extending to the right, and a corner removed");
    
    state.testExecuted = true;
}

// Test 2: Edit after scene upload with re-upload
inline void runDiagnosticTest2_AfterUpload(projv::Scene& scene, projv::GPUData& gpuData, 
                                           projv::utils::StreamingContext& streaming, DiagnosticState& state) {
    if (state.testExecuted) return;
    if (state.frameCount < 100) {
        if (state.frameCount == 0) {
            projv::core::warn("EDITTEST: ========== DIAGNOSTIC TEST 2: Edit After Upload ==========");
            projv::core::warn("EDITTEST: Creating test chunk and uploading to GPU first");
            state.testChunkHandle = createTestChunk(scene, projv::core::vec3(100.0f, 100.0f, 100.0f), 1.0f);
            
            // Upload to GPU
            streaming.mutations.push_back({projv::graphics::SceneMutationKind::Add, state.testChunkHandle});
            projv::graphics::applySceneMutations(scene, gpuData, streaming.mutations);
            
            projv::core::warn("EDITTEST: Test chunk uploaded. Will apply edit at frame 100");
        }
        state.frameCount++;
        return;
    }
    
    projv::core::warn("EDITTEST: ========== Frame 100: Applying edit to uploaded chunk ==========");
    
    projv::Chunk& chunk = scene.chunks[state.testChunkHandle];
    projv::core::warn("EDITTEST: Chunk {} geometryPoolIndex={}, alive={}", state.testChunkHandle, chunk.geometryPoolIndex, chunk.alive);
    
    // Get existing voxels from the pool
    projv::VoxelBatch existing = projv::utils::getChunkVoxelBatch(scene, chunk, true);
    projv::core::warn("EDITTEST: Chunk has {} existing voxels before edit", existing.size());
    
    // Build sphere batch
    projv::VoxelBatch sphere;
    projv::Color addColor = {0, 255, 0};
    float radius = 3.0f;
    projv::core::ivec3 center(10, 4, 4);
    int r = static_cast<int>(std::ceil(radius));
    
    for (int dz = -r; dz <= r; dz++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                projv::core::vec3 offset(static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz));
                if (glm::length(offset) > radius) continue;
                projv::core::ivec3 pos = center + projv::core::ivec3(dx, dy, dz);
                sphere.push_back(projv::utils::createVoxel(addColor, pos));
            }
        }
    }
    
    projv::core::warn("EDITTEST: Sphere contains {} voxels", sphere.size());
    
    // Add sphere to existing voxels
    projv::utils::addVoxelBatchAToVoxelBatchB(sphere, existing);
    projv::core::warn("EDITTEST: Chunk has {} voxels after edit", existing.size());
    
    // Put edited voxels into chunk queue and call editChunkVoxels
    projv::utils::moveVoxelBatchToChunk(existing, chunk);
    projv::core::warn("EDITTEST: Calling editChunkVoxels to update pool blob");
    projv::utils::editChunkVoxels(scene, chunk);
    
    projv::core::warn("EDITTEST: After editChunkVoxels: chunk.geometryPoolIndex={}", chunk.geometryPoolIndex);
    projv::core::warn("EDITTEST: Chunk header: resolution={}, scale={}", chunk.header.resolution, chunk.header.scale);
    if (chunk.geometryPoolIndex >= 0) {
        const projv::GeometryBlob& blob = scene.geometryPool[chunk.geometryPoolIndex];
        projv::core::warn("EDITTEST: Pool blob has {} geometry nodes and {} voxelType entries",
                          blob.geometry.size(), blob.voxelTypeData.size());
    }
    
    // Re-upload
    projv::core::warn("EDITTEST: Re-uploading edited chunk to GPU");
    streaming.mutations.push_back({projv::graphics::SceneMutationKind::Update, state.testChunkHandle});
    projv::graphics::applySceneMutations(scene, gpuData, streaming.mutations);
    
    projv::core::warn("EDITTEST: ========== TEST 2 COMPLETE ==========");
    projv::core::warn("EDITTEST: Expected: Red cube with green voxels extending to the right");
    state.testExecuted = true;
}

// Test 3: Edit using optimized mutation path (the actual editing system)
inline void runDiagnosticTest3_OptimizedPath(projv::Scene& scene, projv::GPUData& gpuData,
                                             projv::utils::StreamingContext& streaming, DiagnosticState& state) {
    if (state.testExecuted) return;
    if (state.frameCount < 100) {
        if (state.frameCount == 0) {
            projv::core::warn("EDITTEST: ========== DIAGNOSTIC TEST 3: Optimized Mutation Path ==========");
            projv::core::warn("EDITTEST: Creating test chunk and uploading to GPU first");
            state.testChunkHandle = createTestChunk(scene, projv::core::vec3(100.0f, 100.0f, 100.0f), 1.0f);
            
            // Upload to GPU
            streaming.mutations.push_back({projv::graphics::SceneMutationKind::Add, state.testChunkHandle});
            projv::graphics::applySceneMutations(scene, gpuData, streaming.mutations);
            
            projv::core::warn("EDITTEST: Test chunk uploaded. Will apply edit at frame 100");
        }
        state.frameCount++;
        return;
    }
    
    projv::core::warn("EDITTEST: ========== Frame 100: Applying edit via optimized path ==========");
    
    projv::Chunk& chunk = scene.chunks[state.testChunkHandle];
    projv::core::warn("EDITTEST: Chunk {} geometryPoolIndex={}, alive={}, componentHandle={}", 
                      state.testChunkHandle, chunk.geometryPoolIndex, chunk.alive, chunk.componentHandle);
    
    // Build edit batch - sphere extends beyond cube (center at 10,4,4 with radius 3)
    projv::EditVoxelBatch editBatch;
    projv::Color addColor = {0, 255, 0};
    float radius = 3.0f;
    projv::core::ivec3 center(10, 4, 4);
    int r = static_cast<int>(std::ceil(radius));
    
    for (int dz = -r; dz <= r; dz++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                projv::core::vec3 offset(static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz));
                if (glm::length(offset) > radius) continue;
                editBatch.push_back({center + projv::core::ivec3(dx, dy, dz), addColor});
            }
        }
    }
    
    projv::core::warn("EDITTEST: Built edit batch with {} voxels", editBatch.size());
    
    // Use the actual editing system
    projv::ComponentLocation loc = projv::resolveComponentLocation(scene, state.testChunkHandle);
    projv::core::warn("EDITTEST: Resolved component {} with voxelSpaceOrigin ({}, {}, {})",
                      loc.component, loc.voxelSpaceOrigin.x, loc.voxelSpaceOrigin.y, loc.voxelSpaceOrigin.z);
    
    if (loc.component >= scene.components.size()) {
        projv::core::error("EDITTEST: Component {} is out of range! scene.components.size()={}", loc.component, scene.components.size());
        state.testExecuted = true;
        return;
    }
    
    const projv::ComponentRecord& rec = scene.components[loc.component];
    projv::core::warn("EDITTEST: Component record: kind={}, chunkHandle={}, gridIndex={}",
                      rec.kind == projv::ComponentKind::Chunk ? "Chunk" : "Grid", rec.chunkHandle, rec.gridIndex);
    
    // Promote to component space
    for (auto& ev : editBatch) {
        ev.position = loc.voxelSpaceOrigin + ev.position;
    }
    
    projv::core::warn("EDITTEST: Calling addVoxelsToComponent with component={}", loc.component);
    projv::utils::addVoxelsToComponent(scene, streaming, loc.component, editBatch);
    
    projv::core::warn("EDITTEST: After addVoxelsToComponent: chunk header resolution={}, scale={}", 
                      chunk.header.resolution, chunk.header.scale);
    if (chunk.geometryPoolIndex >= 0) {
        const projv::GeometryBlob& blob = scene.geometryPool[chunk.geometryPoolIndex];
        projv::core::warn("EDITTEST: Pool blob has {} geometry nodes and {} voxelType entries",
                          blob.geometry.size(), blob.voxelTypeData.size());
    }
    
    projv::core::warn("EDITTEST: After addVoxelsToComponent: mutations queue size={}", streaming.mutations.size());
    for (size_t i = 0; i < streaming.mutations.size(); i++) {
        const auto& m = streaming.mutations[i];
        projv::core::warn("EDITTEST:   Mutation {}: kind={}, handle={}", i,
                          m.kind == projv::graphics::SceneMutationKind::Add ? "Add" :
                          m.kind == projv::graphics::SceneMutationKind::Update ? "Update" : "Remove",
                          m.handle);
    }
    
    projv::core::warn("EDITTEST: Calling applySceneMutations");
    projv::graphics::applySceneMutations(scene, gpuData, streaming.mutations);
    
    projv::core::warn("EDITTEST: ========== TEST 3 COMPLETE ==========");
    projv::core::warn("EDITTEST: Expected: Red cube with green voxels extending to the right");
    state.testExecuted = true;
}

// Prompt for diagnostic test mode
inline int promptForDiagnosticMode() {
    std::cout << "\n=== EDITTEST: Diagnostic Test Mode ===\n";
    std::cout << "  0) Normal operation (no diagnostic)\n";
    std::cout << "  1) Test 1: Edit before scene upload (baseline)\n";
    std::cout << "  2) Test 2: Edit after scene upload with re-upload\n";
    std::cout << "  3) Test 3: Edit using optimized mutation path\n";
    std::cout << "Enter choice [0-3] (default 0): " << std::flush;
    
    std::string line;
    std::getline(std::cin, line);
    try {
        int choice = std::stoi(line);
        if (choice >= 0 && choice <= 3) return choice;
    } catch (...) {
    }
    std::cout << "EDITTEST: Defaulting to normal operation (0).\n";
    return 0;
}

#endif
