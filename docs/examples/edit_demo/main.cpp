// ProjectV Interactive Edit Demo
//
// An interactive real-time demo that loads the Sponza scene and lets you add/remove
// voxels while the scene is rendered. Demonstrates the full editing pipeline:
// queueVoxelAdd/Remove -> updateScene -> flushSceneUpdates -> render.
//
// Placement is SURFACE-PICKED: pressing E/Q shoots the crosshair ray into the scene and
// drops a voxel SPHERE onto the geometry the camera is actually looking at, instead of a
// cube floating a fixed distance in front of the camera. The hit point is recovered by
// reading the renderer's G-buffer back to the CPU: the gbuffer_shade pass already writes
// the world-space primary-hit position (and ray distance) of every pixel into the gPos
// render target, which is exactly the "which voxel am I looking at" information we need to
// identify a voxel. pickCrosshairWorldPos() blits the single crosshair texel of gPos into a
// 1x1 read-back staging texture and reads it back, so no extra full-screen render target or
// re-trace is required.
//
// Controls:
    //   W/S       — move forward / backward
    //   A/D       — strafe left / right
    //   R/F       — move up / down
    //   Mouse     — look around (cursor is captured; Esc releases it, left-click re-captures)
    //   E (hold)  — ADD voxel spheres on the surface under the crosshair (repeats every 10 frames)
    //   Q (hold)  — REMOVE (carve) voxel spheres from the surface under the crosshair (repeats every 10 frames)
    //   1-5       — switch sphere radius (4, 8, 12, 20, 32 voxels)

#include <functional>
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>

#include "core/ecs.h"
#include "core/math.h"
#include "core/log.h"
#include "graphics/render_instance.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/type_mapping.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/scene_query.h"
#include "utils/voxel_management.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <glm/gtc/quaternion.hpp>

namespace fs = std::filesystem;

// =============================================================================
// Frame context / renderer module (simplified single-renderer version)
// =============================================================================

struct FrameContext {
    projv::core::vec3 cameraPosition;
    projv::core::vec3 cameraDirection;
    projv::core::vec3 prevCameraPosition;
    projv::core::vec3 prevCameraDirection;
    bool              cameraMoved;
    int               frameCount;
    int               frameCameraLastMovedOn;
    projv::core::vec2 windowResolution;
    projv::core::vec3 sunDirection;
};

static void uploadCommonUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    projv::core::vec3 cameraPosition = ctx.cameraPosition;
    projv::core::vec3 cameraDirection = ctx.cameraDirection;
    projv::core::vec3 prevCameraPosition = ctx.prevCameraPosition;
    projv::core::vec3 prevCameraDirection = ctx.prevCameraDirection;
    projv::core::vec2 windowResolution = ctx.windowResolution;
    projv::core::vec4 frameCount = { (float)ctx.frameCount, (float)ctx.cameraMoved, (float)ctx.frameCameraLastMovedOn, 0 };
    projv::core::vec2 texelSize = { 1 / windowResolution.x, 1 / windowResolution.y };
    projv::core::vec4 sunDir = { ctx.sunDirection.x, ctx.sunDirection.y, ctx.sunDirection.z, 0.0f };
    projv::graphics::setUniformToValue(renderer, "cameraPos",  cameraPosition);
    projv::graphics::setUniformToValue(renderer, "cameraDir",  cameraDirection);
    projv::graphics::setUniformToValue(renderer, "windowRes",  windowResolution);
    projv::graphics::setUniformToValue(renderer, "frameCount", frameCount);
    projv::graphics::setUniformToValue(renderer, "texelSize",  texelSize);
    projv::graphics::setUniformToValue(renderer, "sunDir",     sunDir);
    projv::graphics::setUniformToValue(renderer, "prevCameraPos", prevCameraPosition);
    projv::graphics::setUniformToValue(renderer, "prevCameraDir", prevCameraDirection);
}

// =============================================================================
// Editing state
// =============================================================================

struct EditState {
    projv::ComponentHandle component = projv::INVALID_COMPONENT_HANDLE;
    int sphereRadius = 8;    // radius of the placed/removed sphere, in voxels
    int fallbackDistance = 15; // where to place when the crosshair ray misses all geometry
    int voxelColorIndex = 0;
};

// =============================================================================
// Preview sphere state
// =============================================================================

// A pre-voxelized yellow sphere rendered as a loose chunk that follows the
// crosshair target, showing where the next edit sphere will go. The sphere
// is re-voxelized only when the radius changes (scrollwheel / key press).
struct PreviewState {
    projv::ChunkHandle chunk = static_cast<projv::ChunkHandle>(-1);
    int currentRadius = 0;   // radius the chunk was built with; 0 = not yet built
};

// Cycle through visible colors so edits are easy to spot.
static projv::Color editColor(int index) {
    static const projv::Color colors[] = {
        {255, 0,   0  },   // red
        {0,   255, 0  },   // green
        {0,   0,   255},   // blue
        {255, 255, 0  },   // yellow
        {255, 0,   255},   // magenta
        {0,   255, 255},   // cyan
        {255, 128, 0  },   // orange
    };
    return colors[static_cast<size_t>(index) % (sizeof(colors) / sizeof(colors[0]))];
}

// =============================================================================
// Surface picking (G-buffer read-back)
// =============================================================================

// texID of the gPos render target in fastRenderer/resources.json. gPos is an
// RGBA32_FLOAT target whose .xyz holds the world-space primary-hit position and .w the
// ray distance to that hit (the sky path writes distance 1e5). That is precisely the
// data needed to identify which voxel a pixel is looking at, so picking reads it back
// rather than adding a second render target or re-tracing on the CPU.
static constexpr uint32_t kGPosTextureID = 3;

struct PickResult {
    bool             hit;          // true if the crosshair ray landed on real geometry
    projv::core::vec3 worldPos;    // world-space hit position (valid when hit == true)
    uint32_t         headerIndex;  // header index of the hit chunk
};

// Reads the single crosshair-center texel of the gPos G-buffer target back to the CPU.
// The gPos content is from the most recently presented frame (renderConstructedRenderer
// ends with bgfx::frame()), which is the exact image the user is looking at when they
// press E/Q, and edit handling runs before this frame's camera movement -- so the pick is
// consistent with the current camera. The blit copies just one texel into a 1x1 staging
// texture, so the read-back is tiny even though gPos is full-screen.
static PickResult pickCrosshairWorldPos(projv::graphics::RenderInstance& renderInstance) {
    PickResult miss{false, projv::core::vec3(0.0f), 0xFFFFFFFFu};

    std::shared_ptr<projv::ConstructedRenderer> renderer = renderInstance.getActiveRenderer();
    auto texIt = renderer->resources.textures.textureHandles.find(kGPosTextureID);
    if (texIt == renderer->resources.textures.textureHandles.end() || !bgfx::isValid(texIt->second)) {
        return miss;
    }
    bgfx::TextureHandle gPos = texIt->second;

    projv::core::ivec2 res = renderInstance.getWindowResolution();
    if (res.x <= 0 || res.y <= 0) return miss;
    uint16_t crosshairX = static_cast<uint16_t>(res.x / 2);
    uint16_t crosshairY = static_cast<uint16_t>(res.y / 2);

    // 1x1 RGBA32F staging texture, created once.
    static bgfx::TextureHandle staging = BGFX_INVALID_HANDLE;
    if (!bgfx::isValid(staging)) {
        staging = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA32F,
                                        BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
    }

    // gPos stores: xyz = world-space hit position, w = float(headerIndex).
    // Sky misses encode headerIndex = 0xFFFFFFFF -> 4294967296.0f, well above any
    // real geometry headerIndex (0-4), so the same >= 1e4 threshold still works.
    const bgfx::ViewId kPickView = 250;
    bgfx::blit(kPickView, staging, 0, 0, gPos, crosshairX, crosshairY, 1, 1);

    float texel[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t readyFrame = bgfx::readTexture(staging, texel);
    while (bgfx::frame() < readyFrame) {}

    if (texel[3] >= 1.0e4f) return miss;

    uint32_t headerIdx = static_cast<uint32_t>(texel[3]);
    return PickResult{true, projv::core::vec3(texel[0], texel[1], texel[2]), headerIdx};
}

// Build the set of edit ops filling a solid voxel sphere of the given radius (in voxels)
// centred on centerVoxel. Component-space voxel coords map 1:1 to world integer coords for
// the Sponza grid, so the caller passes floor(worldPos) as the centre.
static std::vector<projv::PendingVoxelOp> makeSphereOps(projv::core::ivec3 centerVoxel,
                                                        int radius, projv::Color color, bool isAdd) {
    std::vector<projv::PendingVoxelOp> ops;
    int r2 = radius * radius;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy + dz * dz > r2) continue;
                projv::core::ivec3 pos(centerVoxel.x + dx, centerVoxel.y + dy, centerVoxel.z + dz);
                ops.push_back({isAdd, pos, color});
            }
        }
    }
    return ops;
}

// =============================================================================
// Preview sphere (loose chunk)
// =============================================================================

static projv::ChunkHandle buildPreviewSphere(projv::Scene& scene, projv::GPUData& gpuData,
                                              int radius, const projv::core::vec3& position) {
    // Fixed 256^3 volume (power of 4, correct for tree64 traversal).
    constexpr int     kRes    = 256;
    constexpr float   kScale  = 256.0f;
    constexpr int     kCenter = 127;   // sphere center in local voxel coords

    float voxelScale = 1.0f;

    projv::ChunkHeader hdr;
    hdr.chunkID    = 0xFFFF;  // preview marker
    hdr.position   = position;
    hdr.scale      = kScale;
    hdr.voxelScale = voxelScale;
    hdr.resolution = kRes;
    hdr.rotation   = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);

    projv::Chunk chunk;
    chunk.header = hdr;
    chunk.LOD    = 0;
    chunk.alive  = true;

    // Voxelize a yellow sphere into a brick map centered at (127,127,127).
    auto brickMap = projv::utils::createVoxelBrickMap(
        projv::utils::computeBrickDims(kRes));
    int r2 = radius * radius;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy + dz * dz > r2) continue;
                projv::utils::brickMapSetVoxel(*brickMap,
                    kCenter + dx, kCenter + dy, kCenter + dz,
                    projv::Color{255, 255, 0});
            }
        }
    }
    projv::utils::updateChunkFromBrickMap(chunk, *brickMap);

    // Pool the geometry and register as a loose chunk.
    int32_t poolIdx = projv::internChunkGeometry(scene, chunk);
    (void)poolIdx;
    projv::ChunkHandle h = static_cast<projv::ChunkHandle>(scene.chunks.size());
    scene.chunks.push_back(std::move(chunk));

    if (static_cast<size_t>(scene.looseChunkCount) >= scene.looseChunks.size())
        scene.looseChunks.push_back(static_cast<int32_t>(h));
    else
        scene.looseChunks[scene.looseChunkCount] = static_cast<int32_t>(h);
    scene.looseChunkCount++;

    // Flush so the GPU knows about the new chunk and its blob.
    projv::graphics::flushSceneUpdates(scene, gpuData);

    return h;
}

// Rebuild the preview sphere geometry in-place when the radius changes.
// The chunk's blob (refCount == 1) is replaced with new voxel data.
// Resolution is fixed at 256, sphere centred at (127,127,127), so the chunk
// position stays the same -- no recalculation needed.
static void rebuildPreviewSphere(projv::Scene& scene, projv::GPUData& gpuData,
                                  projv::ChunkHandle h, int radius) {
    projv::Chunk& preview = scene.chunks[h];

    constexpr int     kRes    = 256;
    constexpr float   kScale  = 256.0f;
    constexpr int     kCenter = 127;

    // Build a temporary chunk with the new sphere via brick map.
    projv::Chunk temp;
    temp.header = preview.header;
    temp.header.resolution = kRes;
    temp.header.scale = kScale;
    temp.LOD   = 0;
    temp.alive = true;

    auto brickMap = projv::utils::createVoxelBrickMap(
        projv::utils::computeBrickDims(kRes));
    int r2 = radius * radius;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy + dz * dz > r2) continue;
                projv::utils::brickMapSetVoxel(*brickMap,
                    kCenter + dx, kCenter + dy, kCenter + dz,
                    projv::Color{255, 255, 0});
            }
        }
    }
    projv::utils::updateChunkFromBrickMap(temp, *brickMap);

    // Replace the blob content in-place (sole owner, refCount == 1).
    int32_t poolIdx = preview.geometryPoolIndex;
    if (poolIdx >= 0 && static_cast<size_t>(poolIdx) < scene.geometryPool.size()) {
        projv::GeometryBlob& blob = scene.geometryPool[poolIdx];
        blob.geometry = std::move(temp.geometryData);
        blob.voxelTypeData = std::move(temp.voxelTypeData);
        blob.dirty = true;
    }

    // Resolution/scale are fixed -- no header fields to update.
    // Flush the dirty blob.
    projv::graphics::flushSceneUpdates(scene, gpuData);
}

// GLFW scroll callback -- accumulates scroll offset for sphere radius changes.
static double g_scrollAccum = 0.0;
static void scrollCallback(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    (void)window;
    g_scrollAccum += yoffset;
}

// =============================================================================
// Application stages
// =============================================================================

void startup(projv::Application& app) {

    // Change to the PathTracer directory so relative paths to renderer resources
    // and the Sponza scene resolve correctly.
    fs::current_path(fs::path(__FILE__).parent_path() / "../PathTracer");

    projv::graphics::RenderInstance& renderInstance = projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(1920, 1080, "ProjectV Edit Demo");

    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    projv::Scene& scene    = projv::core::createGlobalResource<projv::Scene>(app.world);
    projv::GPUData& gpuData = projv::core::createGlobalResource<projv::GPUData>(app.world);
    EditState& editState    = projv::core::createGlobalResource<EditState>(app.world);
    PreviewState& preview   = projv::core::createGlobalResource<PreviewState>(app.world);

    // Load the Sponza scene.
    scene = projv::utils::loadComposeFromDisk("./SponzaScene/");
    projv::core::info("Edit demo: loaded {} components, {} chunks, {} grids",
                      scene.components.size(), scene.chunks.size(), scene.grids.size());

    // Log the component tree.
    for (projv::ComponentHandle h = 0; h < scene.components.size(); ++h) {
        const auto& c = scene.components[h];
        projv::core::info("  [{}] kind={} name=\"{}\" parent={} children={} path=\"{}\"",
            h,
            c.kind == projv::ComponentKind::Asset ? "Asset" :
            c.kind == projv::ComponentKind::Grid ? "Grid" : "Chunk",
            c.name, c.parent, c.children.size(),
            projv::utils::getComponentPath(scene, h));
    }

    // Use the first non-Asset component as the edit target.
    editState.component = projv::INVALID_COMPONENT_HANDLE;
    for (projv::ComponentHandle h = 0; h < scene.components.size(); ++h) {
        if (scene.components[h].kind != projv::ComponentKind::Asset) {
            editState.component = h;
            break;
        }
    }
    projv::core::info("Edit demo: targeting component {} (path=\"{}\")", editState.component,
                      projv::utils::getComponentPath(scene, editState.component));

    // Load the fast renderer (direct + AO, TAA).
    projv::RendererSpecification rendererSpec = projv::graphics::loadRendererSpecification(
        "./fastRenderer/");
    renderInstance.addRendererSpecification(1, rendererSpec);

    bgfx::ShaderHandle vsh = projv::graphics::loadShader(
        "./fastRenderer/pathTracerShaders/vs_quad.bin");
    std::shared_ptr<projv::ConstructedRenderer> constructedRenderer =
        projv::graphics::constructRendererSpecification(
            renderInstance.getRendererSpecification(1), vsh);

    // Load the blue-noise LUT.
    int width, height, channels;
    unsigned char* img = stbi_load("LDR_RGBA_7.png", &width, &height, &channels, 4);
    projv::core::info("Blue-noise texture: {}x{}", width, height);
    projv::graphics::setTextureToData(constructedRenderer, 1, img, width, height);

    renderInstance.setActiveRenderer(constructedRenderer);
    gpuData = projv::graphics::createTexturesForScene(scene);

    // Create the preview sphere (yellow ghost showing where the next edit will go).
    {
        constexpr float kCenter = 127.0f;
        projv::core::vec3 targetCenter = projv::core::vec3(1006.6f, 413.0f, -302.4f);
        projv::core::vec3 chunkPos(
            targetCenter.x - kCenter,
            targetCenter.y - kCenter,
            targetCenter.z - kCenter);
        preview.chunk = buildPreviewSphere(scene, gpuData, editState.sphereRadius, chunkPos);
        preview.currentRadius = editState.sphereRadius;
        projv::core::warn("PREVIEW: chunk={} alive={} poolIdx={} res={} scale={} pos=({},{},{})",
                          preview.chunk,
                          scene.chunks[preview.chunk].alive,
                          scene.chunks[preview.chunk].geometryPoolIndex,
                          scene.chunks[preview.chunk].header.resolution,
                          scene.chunks[preview.chunk].header.scale,
                          scene.chunks[preview.chunk].header.position.x,
                          scene.chunks[preview.chunk].header.position.y,
                          scene.chunks[preview.chunk].header.position.z);
    }

    // Set up scroll callback for sphere radius changes.
    glfwSetScrollCallback(renderInstance.window, scrollCallback);
}

void update(projv::Application& app) {
#if defined(PROJV_ENABLE_PERF)
    static auto lastFrameTime = std::chrono::high_resolution_clock::now();
    auto currentFrameTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> frameDuration = currentFrameTime - lastFrameTime;
    lastFrameTime = currentFrameTime;

    static int frameCount = 0;
    static double frameTimes[100];
    frameTimes[frameCount % 100] = frameDuration.count() * 1000.0;
    if (++frameCount % 100 == 0) {
        double sum = 0, mn = 1e9, mx = 0;
        for (int i = 0; i < 100; i++) {
            sum += frameTimes[i];
            mn = std::min(mn, frameTimes[i]);
            mx = std::max(mx, frameTimes[i]);
        }
        core::perf("Frame stats (last 100): avg={:.2f}ms min={:.2f}ms max={:.2f}ms",
                   sum / 100.0, mn, mx);
    }
#endif
}

void render(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance = projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    projv::Scene& scene        = projv::core::getGlobalResource<projv::Scene>(app.world);
    projv::GPUData& gpuData    = projv::core::getGlobalResource<projv::GPUData>(app.world);
    EditState& editState       = projv::core::getGlobalResource<EditState>(app.world);
    PreviewState& preview      = projv::core::getGlobalResource<PreviewState>(app.world);

    static projv::core::vec3 cameraPosition = projv::core::vec3(1018.0, 413.0, -330.0);
    static projv::core::vec3 prevCameraPosition  = cameraPosition;
    static projv::core::vec3 prevCameraDirection = projv::core::vec3(0.0, 0.0, 1.0);
    static bool prevCameraInitialized = false;
    bool cameraMoved = false;
    static float cameraPitch = 0.0f;
    static float cameraPhi = 3.14f / 2.0f + 0.4f;

    // Camera direction (needed for both editing and rendering).
    projv::core::vec3 cameraDirection;
    cameraDirection.x = projv::core::cos(cameraPitch) * projv::core::cos(cameraPhi);
    cameraDirection.y = projv::core::sin(cameraPitch);
    cameraDirection.z = projv::core::cos(cameraPitch) * projv::core::sin(cameraPhi);

    projv::core::vec3 forwardDirection = { projv::core::cos(cameraPhi), 0, projv::core::sin(cameraPhi) };

    // --- Editing keybindings ---

    // Key 1-5: switch sphere radius (4, 8, 12, 20, 32).
    if (glfwGetKey(renderInstance.window, GLFW_KEY_1) == GLFW_PRESS) editState.sphereRadius = 4;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_2) == GLFW_PRESS) editState.sphereRadius = 8;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_3) == GLFW_PRESS) editState.sphereRadius = 12;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_4) == GLFW_PRESS) editState.sphereRadius = 20;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_5) == GLFW_PRESS) editState.sphereRadius = 32;

    // Scrollwheel also changes the radius (scaled so one notch = ~2 voxels).
    {
        static constexpr int kMinRadius = 2;
        static constexpr int kMaxRadius = 64;
        int scrollDelta = static_cast<int>(std::round(g_scrollAccum * 2.0));
        if (scrollDelta != 0) {
            g_scrollAccum = 0.0;
            editState.sphereRadius = std::clamp(editState.sphereRadius + scrollDelta,
                                                 kMinRadius, kMaxRadius);
        }
    }

    // Rebuild the preview sphere if radius changed.
    if (preview.chunk != static_cast<projv::ChunkHandle>(-1) &&
        editState.sphereRadius != preview.currentRadius) {
        preview.currentRadius = editState.sphereRadius;
        rebuildPreviewSphere(scene, gpuData, preview.chunk, editState.sphereRadius);
        projv::core::warn("PREVIEW REBUILD: radius={}", editState.sphereRadius);
    }

    // Resolve the sphere centre by picking the surface the crosshair is looking at. When the
    // ray misses all geometry (aiming at the sky) fall back to a fixed distance so E still does
    // something visible. The centre is floored to integer component-space (== world) voxel coords.
    auto sphereCenter = [&](bool addingToSurface) -> projv::core::ivec3 {
        PickResult pick = pickCrosshairWorldPos(renderInstance);
        projv::core::vec3 worldPos;
        if (pick.hit) {
            // If the pick lands on the preview sphere, use the sphere's centre
            // instead of its surface -- the centre sits on the real geometry the
            // sphere was placed on.
            if (pick.headerIndex == static_cast<uint32_t>(preview.chunk)) {
                constexpr float kCenter = 127.0f;
                worldPos = scene.chunks[preview.chunk].header.position +
                           projv::core::vec3(kCenter, kCenter, kCenter);
            } else {
                worldPos = pick.worldPos;
            }
            // Nudge an ADD sphere back toward the camera by half its radius so it visibly sits
            // ON the surface instead of being buried halfway inside it. A REMOVE (carve) stays
            // centered on the hit so it scoops a crater out of the surface.
            if (addingToSurface) {
                worldPos = worldPos - cameraDirection * (static_cast<float>(editState.sphereRadius) * 0.5f);
            }
        } else {
            worldPos = cameraPosition + cameraDirection * static_cast<float>(editState.fallbackDistance);
        }
        return projv::core::ivec3(static_cast<int>(std::floor(worldPos.x)),
                                  static_cast<int>(std::floor(worldPos.y)),
                                  static_cast<int>(std::floor(worldPos.z)));
    };

    // E: add a voxel sphere on the surface under the crosshair (repeats every 10 frames while held).
    // Q: remove (carve) a voxel sphere from the surface under the crosshair (same repeat).
    static int editFrameCountdown = 0;
    static constexpr int kEditRepeatInterval = 10;

    bool wantsAdd    = glfwGetKey(renderInstance.window, GLFW_KEY_E) == GLFW_PRESS;
    bool wantsRemove = glfwGetKey(renderInstance.window, GLFW_KEY_Q) == GLFW_PRESS;
    bool doEdit = (wantsAdd || wantsRemove) && --editFrameCountdown <= 0;

    if (doEdit && editState.component != projv::INVALID_COMPONENT_HANDLE) {
        editFrameCountdown = kEditRepeatInterval;

        bool isAdd = wantsAdd; // remove is only active when add is not
        projv::core::ivec3 center = sphereCenter(isAdd);
        auto ops = makeSphereOps(center, editState.sphereRadius,
                                 isAdd ? editColor(editState.voxelColorIndex++) : projv::Color{0, 0, 0},
                                 isAdd);
        if (isAdd) {
            projv::utils::queueVoxelAdd(scene, editState.component, ops);
        } else {
            projv::utils::queueVoxelRemove(scene, editState.component, ops);
        }
        projv::utils::updateScene(scene);
        projv::graphics::flushSceneUpdates(scene, gpuData);
        projv::core::warn("{} sphere r={} at ({}, {}, {}) ({} voxels)",
                          isAdd ? "ADD" : "REMOVE",
                          editState.sphereRadius, center.x, center.y, center.z, ops.size());
        cameraMoved = true; // force TAA reset so the new voxels are immediately visible
    }

    // --- Cursor capture toggle ---
    static bool mouseCaptured = true;
    if (mouseCaptured && glfwGetKey(renderInstance.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        mouseCaptured = false;
    } else if (!mouseCaptured && glfwGetMouseButton(renderInstance.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        mouseCaptured = true;
    }

    // --- Mouse look ---
    static double lastMouseX = 0.0, lastMouseY = 0.0;
    static bool mouseInitialized = false;
    double mouseX, mouseY;
    glfwGetCursorPos(renderInstance.window, &mouseX, &mouseY);
    if (!mouseInitialized || !mouseCaptured) {
        lastMouseX = mouseX;
        lastMouseY = mouseY;
        mouseInitialized = true;
    }
    double mouseDeltaX = mouseX - lastMouseX;
    double mouseDeltaY = mouseY - lastMouseY;
    lastMouseX = mouseX;
    lastMouseY = mouseY;

    const float mouseSensitivity = 0.0025f;
    if (mouseCaptured && (mouseDeltaX != 0.0 || mouseDeltaY != 0.0)) {
        cameraPhi   += (float)mouseDeltaX * mouseSensitivity;
        cameraPitch -= (float)mouseDeltaY * mouseSensitivity;
        const float pitchLimit = 1.55f;
        if (cameraPitch >  pitchLimit) cameraPitch =  pitchLimit;
        if (cameraPitch < -pitchLimit) cameraPitch = -pitchLimit;
        cameraMoved = true;
        // Recompute direction after mouse look.
        cameraDirection.x = projv::core::cos(cameraPitch) * projv::core::cos(cameraPhi);
        cameraDirection.y = projv::core::sin(cameraPitch);
        cameraDirection.z = projv::core::cos(cameraPitch) * projv::core::sin(cameraPhi);
        forwardDirection = { projv::core::cos(cameraPhi), 0, projv::core::sin(cameraPhi) };
    }

    const float moveSpeed = 4.13f;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_W)) { cameraPosition += forwardDirection * moveSpeed; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_S)) { cameraPosition -= forwardDirection * moveSpeed; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_A)) {
        float leftPhi = cameraPhi + 3.14f / 2.0f;
        projv::core::vec3 leftDirection = { projv::core::cos(leftPhi), 0, projv::core::sin(leftPhi) };
        cameraPosition -= leftDirection * moveSpeed;
        cameraMoved = true;
    }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_D)) {
        float rightPhi = cameraPhi - 3.14f / 2.0f;
        projv::core::vec3 rightDirection = { projv::core::cos(rightPhi), 0, projv::core::sin(rightPhi) };
        cameraPosition -= rightDirection * moveSpeed;
        cameraMoved = true;
    }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_R)) { cameraPosition[1] += moveSpeed; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_F)) { cameraPosition[1] -= moveSpeed; cameraMoved = true; }

    core_info_every(60, "Pos: ({:.2f}, {:.2f}, {:.2f})  SphereRadius: {}  Color: {}",
                      cameraPosition[0], cameraPosition[1], cameraPosition[2],
                      editState.sphereRadius, editState.voxelColorIndex);

    if (!prevCameraInitialized) {
        prevCameraPosition   = cameraPosition;
        prevCameraDirection  = cameraDirection;
        prevCameraInitialized = true;
    }

    // Fixed sun direction.
    projv::core::vec3 sunDirection = { -0.6402f, 0.6f, 0.7682f };

    static int frameCameraLastMovedOn = 0;
    if (cameraMoved) frameCameraLastMovedOn = app.frameCount;

    FrameContext ctx;
    ctx.cameraPosition         = cameraPosition;
    ctx.cameraDirection        = cameraDirection;
    ctx.prevCameraPosition     = prevCameraPosition;
    ctx.prevCameraDirection    = prevCameraDirection;
    ctx.cameraMoved            = cameraMoved;
    ctx.frameCount             = app.frameCount;
    ctx.frameCameraLastMovedOn = frameCameraLastMovedOn;
    ctx.windowResolution       = renderInstance.getWindowResolution();
    ctx.sunDirection           = sunDirection;

    uploadCommonUniforms(renderInstance.getActiveRenderer(), ctx);
    projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpuData);

    // --- Preview sphere follow (surface-picked position) ---
    // Only update when the pick lands on real scene geometry, not the preview
    // sphere itself (headerIndex == preview.chunk), to avoid the feedback loop
    // where the sphere is repeatedly moved onto its own surface.
    if (preview.chunk != static_cast<projv::ChunkHandle>(-1)) {
        PickResult pick = pickCrosshairWorldPos(renderInstance);
        if (pick.hit && pick.headerIndex != static_cast<uint32_t>(preview.chunk)) {
            // Sphere centre sits at (127,127,127) in local voxel coords, so
            // chunkPos = floor(hitPos) - 127 puts the centre on the picked voxel.
            constexpr float kCenter = 127.0f;
            projv::core::vec3 newPos(
                static_cast<float>(static_cast<int>(std::floor(pick.worldPos.x))) - kCenter,
                static_cast<float>(static_cast<int>(std::floor(pick.worldPos.y))) - kCenter,
                static_cast<float>(static_cast<int>(std::floor(pick.worldPos.z))) - kCenter);
            scene.chunks[preview.chunk].header.position = newPos;
            projv::graphics::updateChunkHeader(scene, gpuData, preview.chunk);
        }
    }

    prevCameraPosition  = cameraPosition;
    prevCameraDirection = cameraDirection;
}

int main() {
    projv::Application app = projv::core::createApp();
    projv::core::assignSystemStage(app, projv::SystemStage::Startup, startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update,  update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render,  render);
    projv::core::runApplication(app);
    return 0;
}