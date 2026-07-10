#ifndef PATHTRACER_VOXEL_EDITING_H
#define PATHTRACER_VOXEL_EDITING_H

// EXAMPLE voxel editing — application code, NOT engine behavior. Mirrors residencyPolicy.h's split:
// the engine ships the editing *mechanism* (utils::addVoxelsToComponent/removeVoxelsFromComponent,
// utils/streaming.h) -- addressed by ComponentHandle and integer voxel-space coordinates, pool-aware,
// grows/materializes grid cells as needed -- but no picking, no input handling, and no shape;
// deciding what a click means is the app's call, exactly like a residency policy. This file is one
// such policy: a simple sphere add/remove, driven by an extra "pickBuffer" output that
// tree64Renderer's path_trace.frag writes (world-space hit point + chunk handle per pixel).
//
// Scoped to tree64Renderer only — see RendererModule::supportsEditing in main.cpp. The technique
// (an extra G-buffer output, read back on click) generalizes to the other renderer variants by
// copying the same pickBuffer texture + GBuffer.pick write into their own G-buffer pass.
//
// Readback: pickBuffer and the existing "normal" texture are declared with "readBack": true in
// resources.json, so bgfx::readTexture works on them directly. An earlier version of this file used
// bgfx::blit() to copy just the hovered texel into a 1x1 staging texture before reading it back —
// that reliably segfaulted inside bgfx::Encoder::blit on this GPU/driver (confirmed via
// coredumpctl), so this reads the whole frame back on click instead (a plain, universally-supported
// bgfx::readTexture — no blit involved). A click-triggered full-frame readback stalls the pipeline
// for a moment, which is an acceptable trade for a demo feature that only fires on click.
//
// Handles, not cached: perform_renderer.cpp's renderConstructedRenderer() destroys and recreates
// every "resizable" texture (pickBuffer/normal included) the moment glfwGetWindowSize() first
// differs from its function-static prevWindowWidth/Height (0 initially) — i.e. on frame 1,
// unconditionally. A bgfx::TextureHandle cached once in initVoxelEditing() would already be
// dangling by the time a click reads it (confirmed via coredumpctl: segfault inside
// bgfx::vk::RendererContextVK::readTexture). So this only caches the stable texture *IDs* and
// resolves the live bgfx::TextureHandle from the renderer's texture map at click time instead.

#include <cstdint>
#include <cmath>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "bgfx/bgfx.h"
#include "GLFW/glfw3.h"

#include "core/math.h"
#include "core/log.h"
#include "data_structures/scene.h"
#include "data_structures/gpuData.h"
#include "data_structures/constructedRenderer.h"
#include "data_structures/rendererSpecification.h"
#include "graphics/scene_dynamics.h"
#include "utils/streaming.h"

// Everything the editor needs to remember between frames: the readback texture IDs (stable across
// resizes — see "Handles, not cached" above), one in-flight readback request, click edge-detection,
// and the sphere's parameters.
struct VoxelEditState {
    uint32_t pickBufferTextureID = 0; // tree64Renderer "pickBuffer"
    uint32_t normalTextureID     = 0; // tree64Renderer "normal" (already existed)

    bool prevLeftDown  = false;
    bool prevRightDown = false;

    bool pending            = false;
    bool pendingIsAdd       = false;
    int  pendingIssuedFrame = 0;
    int  lastIssueFrame     = -1000000; // sentinel: first hold-driven issue never gated by elapsed time
    int  pendingWidth       = 0;
    int  pendingHeight      = 0;

    std::vector<float>   pickBufferData;   // pendingWidth*pendingHeight*4 floats (RGBA32F)
    std::vector<uint8_t> normalBufferData; // pendingWidth*pendingHeight*4 bytes  (RGBA8)

    float        sphereRadiusVoxels = 16.0f;
    projv::Color addColor           = {180, 180, 180};
};

// bgfx::readTexture's returned "ready" frame isn't threaded out of renderConstructedRenderer, so we
// just wait a fixed number of app ticks (each tick is one bgfx::frame()) before trusting the buffer.
static constexpr int kVoxelEditReadbackLatencyFrames = 3;

// While the mouse button is held, re-issue a pick (and thus add/remove another sphere) every
// kVoxelEditRepeatIntervalFrames frames. The readback above stalls the pipeline for a moment, so the
// repeat rate is intentionally coarser than the per-frame rate — at 5 frames/issue and ~60fps that's
// ~12 spheres/sec, which is fast enough to feel continuous to a held click without saturating the
// GPU with full-frame readbacks.
static constexpr int kVoxelEditRepeatIntervalFrames = 5;

namespace projv_internal {
    inline uint32_t findTextureIDByName(const std::vector<projv::Texture>& textures, const std::string& name) {
        for (const projv::Texture& t : textures) {
            if (t.name == name) return t.textureID;
        }
        projv::core::error("voxelEditing: texture '{}' not found in renderer resources", name);
        return 0;
    }
}

// Called once from startup() after the tree64Renderer is constructed: looks up pickBuffer/normal's
// stable texture IDs by name so we don't hardcode them (the bgfx::TextureHandle itself is resolved
// fresh at click time — see "Handles, not cached" above).
inline void initVoxelEditing(VoxelEditState& state, projv::RendererSpecification& rendererSpec) {
    const std::vector<projv::Texture>& textures = rendererSpec.resources.textures;
    state.pickBufferTextureID = projv_internal::findTextureIDByName(textures, "pickBuffer");
    state.normalTextureID     = projv_internal::findTextureIDByName(textures, "normal");
}

// Builds a sphere of voxels around `center`, purely in integer voxel-space offsets -- no resolution
// clamp, no chunk/grid awareness at all. Whether `center` is chunk-local or a grid-wide continuous
// coordinate is entirely up to the caller (see EditVoxel in data_structures/voxel.h); the engine
// mechanism this feeds (utils::addVoxelsToComponent/removeVoxelsFromComponent) resolves that, grows
// grids, and materializes cells as needed, so this function doesn't need to know or care.
inline projv::EditVoxelBatch buildSphereVoxelBatch(projv::core::ivec3 center, float radiusVoxels, projv::Color color) {
    projv::EditVoxelBatch batch;
    int r = static_cast<int>(std::ceil(radiusVoxels));
    for (int dz = -r; dz <= r; dz++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                projv::core::vec3 offset(static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz));
                if (glm::length(offset) > radiusVoxels) continue;
                batch.push_back({center + projv::core::ivec3(dx, dy, dz), color});
            }
        }
    }
    return batch;
}

// Resolves one completed pick request into a component + voxel-space coordinate, then splices a
// sphere of added or removed voxels into it using only the engine's editing mechanism.
inline void applyPendingVoxelEdit(VoxelEditState& state, projv::Scene& scene, projv::GPUData& gpuData,
                                   projv::utils::StreamingContext& streaming) {
    int centerX = state.pendingWidth / 2;
    int centerY = state.pendingHeight / 2;
    size_t pixelIndex = static_cast<size_t>(centerY) * state.pendingWidth + centerX;

    const float* pick = &state.pickBufferData[pixelIndex * 4];
    float headerIndexF = pick[3];
    if (headerIndexF < 0.0f) return; // the crosshair was pointed at the sky.

    projv::ChunkHandle handle = static_cast<projv::ChunkHandle>(headerIndexF + 0.5f);
    if (handle >= scene.chunks.size() || !scene.chunks[handle].alive) {
        projv::core::warn("voxelEditing: picked handle {} is not a live chunk", handle);
        return;
    }
    const projv::Chunk& chunk = scene.chunks[handle];

    const uint8_t* normal = &state.normalBufferData[pixelIndex * 4];
    projv::core::vec3 worldHit(pick[0], pick[1], pick[2]);
    projv::core::vec3 normalWorld = glm::normalize(
        projv::core::vec3(normal[0], normal[1], normal[2]) / 255.0f * 2.0f - 1.0f);

    // Interpreting the ray hit -- which voxel, on which side of the surface -- is app-specific
    // picking logic, unchanged: invert the same transform castRayThroughTree64 applies in-shader
    // (pjv_utils_DDA.sc), using the picked chunk's own CPU-side header, to get a coordinate LOCAL TO
    // THAT CHUNK. Push half a voxel along the true surface normal (not the ray) so grazing hits
    // still resolve to the correct side of the boundary.
    projv::core::mat3 R = glm::mat3_cast(chunk.header.rotation);
    projv::core::mat3 Rinv = glm::transpose(R);
    projv::core::vec3 localHit = (Rinv * (worldHit - chunk.header.position)) / chunk.header.voxelScale;
    projv::core::vec3 localNormal = Rinv * normalWorld;
    projv::core::vec3 targetF = state.pendingIsAdd ? (localHit + localNormal * 0.5f) : (localHit - localNormal * 0.5f);
    projv::core::ivec3 localCoord = projv::core::ivec3(glm::floor(targetF));

    // From here on, everything is engine mechanism: resolve which component owns the picked chunk
    // and promote the chunk-local coordinate into that component's own voxel space (identity for a
    // loose component; this cell's placement within the grid for a grid component), then hand off a
    // sphere built directly in that space. No matrix math, no pool/grid bookkeeping here at all.
    projv::ComponentLocation loc = projv::resolveComponentLocation(scene, handle);
    projv::core::ivec3 center = loc.voxelSpaceOrigin + localCoord;
    projv::core::warn("DIAG applyPendingVoxelEdit: pickedHandle={} gridIndex={} cellIndex={} component={} "
                       "voxelSpaceOrigin=({},{},{}) localCoord=({},{},{}) center=({},{},{}) isAdd={}",
                       handle, chunk.gridIndex, chunk.cellIndex, loc.component,
                       loc.voxelSpaceOrigin.x, loc.voxelSpaceOrigin.y, loc.voxelSpaceOrigin.z,
                       localCoord.x, localCoord.y, localCoord.z, center.x, center.y, center.z,
                       state.pendingIsAdd);
    projv::EditVoxelBatch sphere = buildSphereVoxelBatch(center, state.sphereRadiusVoxels, state.addColor);
    if (sphere.empty()) return;

    if (state.pendingIsAdd) {
        projv::utils::addVoxelsToComponent(scene, streaming, loc.component, sphere);
    } else {
        projv::utils::removeVoxelsFromComponent(scene, streaming, loc.component, sphere);
    }
    projv::core::warn("VOXELEDIT: after edit, mutations queue size={}", streaming.mutations.size());
    for (size_t i = 0; i < streaming.mutations.size(); i++) {
        const auto& m = streaming.mutations[i];
        projv::core::warn("VOXELEDIT:   mutation {}: kind={}, handle={}", i,
                          m.kind == projv::graphics::SceneMutationKind::Add ? "Add" :
                          m.kind == projv::graphics::SceneMutationKind::Update ? "Update" : "Remove",
                          m.handle);
    }
    projv::graphics::applySceneMutations(scene, gpuData, streaming.mutations);
}

// Called once per frame from render(), after renderConstructedRenderer(...). Resolves any
// previously-issued pick, then issues a new one on a left/right click edge (add / remove), and
// continues to re-issue every kVoxelEditRepeatIntervalFrames frames while the button is held.
inline void updateVoxelEditing(VoxelEditState& state, projv::Scene& scene, projv::GPUData& gpuData,
                                projv::utils::StreamingContext& streaming,
                                std::shared_ptr<projv::ConstructedRenderer> renderer,
                                GLFWwindow* window, bool mouseCaptured, projv::core::ivec2 windowResolution, int frameCount) {
    if (state.pending && (frameCount - state.pendingIssuedFrame) >= kVoxelEditReadbackLatencyFrames) {
        applyPendingVoxelEdit(state, scene, gpuData, streaming);
        state.pending = false;
    }

    bool leftDown  = mouseCaptured && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS;
    bool rightDown = mouseCaptured && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    bool leftClicked  = leftDown  && !state.prevLeftDown;
    bool rightClicked = rightDown && !state.prevRightDown;
    state.prevLeftDown  = leftDown;
    state.prevRightDown = rightDown;

    // Decide whether to issue a new pick this frame:
    //   1) on the click edge (preserves the original single-click behaviour), OR
    //   2) while the button is still held, once every kVoxelEditRepeatIntervalFrames frames.
    // Gated on !state.pending so we never overlap a readback that's still in flight (the GPU is
    // stalled on each readback, and the API doesn't promise concurrent ones are safe).
    bool wantIssue = false;
    bool wantAdd   = false;
    if (leftClicked)       { wantIssue = true; wantAdd = true;  }
    if (rightClicked)      { wantIssue = true; wantAdd = false; }
    if (!state.pending && !wantIssue && (leftDown || rightDown) &&
        (frameCount - state.lastIssueFrame) >= kVoxelEditRepeatIntervalFrames) {
        wantIssue = true;
        wantAdd   = leftDown; // leftDown implies !rightDown here in practice; mirror the click edge
    }

    if (wantIssue) {
        state.pendingWidth  = windowResolution.x;
        state.pendingHeight = windowResolution.y;
        size_t pixelCount = static_cast<size_t>(state.pendingWidth) * state.pendingHeight;
        state.pickBufferData.resize(pixelCount * 4);
        state.normalBufferData.resize(pixelCount * 4);

        // Resolved fresh, not cached — see "Handles, not cached" above.
        bgfx::TextureHandle pickBufferTexture = renderer->resources.textures.textureHandles.at(state.pickBufferTextureID);
        bgfx::TextureHandle normalTexture     = renderer->resources.textures.textureHandles.at(state.normalTextureID);
        bgfx::readTexture(pickBufferTexture, state.pickBufferData.data());
        bgfx::readTexture(normalTexture, state.normalBufferData.data());

        state.pending            = true;
        state.pendingIsAdd       = wantAdd;
        state.pendingIssuedFrame = frameCount;
        state.lastIssueFrame     = frameCount;
    }
}

#endif
