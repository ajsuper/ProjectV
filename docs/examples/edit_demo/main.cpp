// ProjectV Interactive Edit Demo
//
// An interactive real-time demo that loads the Sponza scene and lets you add/remove
// voxels while the scene is rendered. Demonstrates the full editing pipeline:
// queueVoxelAdd/Remove -> updateScene -> rebuildSceneTextures -> render.
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
//   E         — ADD a voxel sphere on the surface under the crosshair
//   Q         — REMOVE (carve) a voxel sphere from the surface under the crosshair
//   1-4       — switch sphere radius (2, 4, 6, 10 voxels)

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

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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

// Global editing state, stored in a global resource so it persists across frames.
struct EditState {
    projv::ComponentHandle component = projv::INVALID_COMPONENT_HANDLE;
    int sphereRadius = 4;    // radius of the placed/removed sphere, in voxels
    int fallbackDistance = 15; // where to place when the crosshair ray misses all geometry
    int voxelColorIndex = 0;
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
    bool             hit;      // true if the crosshair ray landed on real geometry
    projv::core::vec3 worldPos; // world-space hit position (valid when hit == true)
};

// Reads the single crosshair-center texel of the gPos G-buffer target back to the CPU.
// The gPos content is from the most recently presented frame (renderConstructedRenderer
// ends with bgfx::frame()), which is the exact image the user is looking at when they
// press E/Q, and edit handling runs before this frame's camera movement -- so the pick is
// consistent with the current camera. The blit copies just one texel into a 1x1 staging
// texture, so the read-back is tiny even though gPos is full-screen.
static PickResult pickCrosshairWorldPos(projv::graphics::RenderInstance& renderInstance) {
    PickResult miss{false, projv::core::vec3(0.0f)};

    std::shared_ptr<projv::ConstructedRenderer> renderer = renderInstance.getActiveRenderer();
    auto texIt = renderer->resources.textures.textureHandles.find(kGPosTextureID);
    if (texIt == renderer->resources.textures.textureHandles.end() || !bgfx::isValid(texIt->second)) {
        return miss;
    }
    bgfx::TextureHandle gPos = texIt->second;

    // 1x1 RGBA32F staging texture, created once. BLIT_DST lets gPos be blitted into it and
    // READ_BACK lets bgfx::readTexture pull it to the CPU.
    static bgfx::TextureHandle staging = BGFX_INVALID_HANDLE;
    if (!bgfx::isValid(staging)) {
        staging = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA32F,
                                        BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
    }

    projv::core::ivec2 res = renderInstance.getWindowResolution();
    if (res.x <= 0 || res.y <= 0) return miss;
    uint16_t crosshairX = static_cast<uint16_t>(res.x / 2);
    uint16_t crosshairY = static_cast<uint16_t>(res.y / 2);

    // Blit the crosshair texel of gPos into the staging texture and read it back. The blit
    // view id sits above the renderer's own view ids so it executes after any pending work.
    const bgfx::ViewId kPickView = 250;
    bgfx::blit(kPickView, staging, 0, 0, gPos, crosshairX, crosshairY, 1, 1);

    float texel[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t readyFrame = bgfx::readTexture(staging, texel);
    while (bgfx::frame() < readyFrame) {
        // Spin until the GPU has produced the copy (same pattern as verifyTextureWithReadback).
    }

    // .w is the ray distance; the sky path writes 1e5, so anything near that is a miss.
    if (texel[3] >= 1.0e4f) return miss;
    return PickResult{true, projv::core::vec3(texel[0], texel[1], texel[2])};
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
// Application stages
// =============================================================================

void startup(projv::Application& app) {
    spdlog::set_level(spdlog::level::warn);

    // Change to the PathTracer directory so relative paths to renderer resources
    // and the Sponza scene resolve correctly.
    fs::current_path(fs::path(__FILE__).parent_path() / "../PathTracer");

    projv::graphics::RenderInstance& renderInstance = projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(1920, 1080, "ProjectV Edit Demo");

    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    projv::Scene& scene    = projv::core::createGlobalResource<projv::Scene>(app.world);
    projv::GPUData& gpuData = projv::core::createGlobalResource<projv::GPUData>(app.world);
    EditState& editState    = projv::core::createGlobalResource<EditState>(app.world);

    // Load the Sponza scene.
    scene = projv::utils::loadComposeFromDisk("./SponzaScene/");

    // Find the first Grid component (the Sponza scene has one).
    for (projv::ComponentHandle h = 0; h < scene.components.size(); ++h) {
        if (scene.components[h].kind == projv::ComponentKind::Grid) {
            editState.component = h;
            break;
        }
    }
    projv::core::info("Edit demo: targeting component {}", editState.component);

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
}

void update(projv::Application& app) {
    static auto lastFrameTime = std::chrono::high_resolution_clock::now();
    auto currentFrameTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> frameDuration = currentFrameTime - lastFrameTime;
    lastFrameTime = currentFrameTime;
    projv::core::warn("PROFILING: Frame time {:.2f}ms", frameDuration.count() * 1000.0);
}

void render(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance = projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    projv::Scene& scene        = projv::core::getGlobalResource<projv::Scene>(app.world);
    projv::GPUData& gpuData    = projv::core::getGlobalResource<projv::GPUData>(app.world);
    EditState& editState       = projv::core::getGlobalResource<EditState>(app.world);

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

    // --- Editing keybindings (processed up front, before cursor capture check) ---

    // Change sphere radius (in voxels): 1-4.
    if (glfwGetKey(renderInstance.window, GLFW_KEY_1) == GLFW_PRESS) editState.sphereRadius = 2;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_2) == GLFW_PRESS) editState.sphereRadius = 4;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_3) == GLFW_PRESS) editState.sphereRadius = 6;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_4) == GLFW_PRESS) editState.sphereRadius = 10;

    // Resolve the sphere centre by picking the surface the crosshair is looking at. When the
    // ray misses all geometry (aiming at the sky) fall back to a fixed distance so E still does
    // something visible. The centre is floored to integer component-space (== world) voxel coords.
    auto sphereCenter = [&](bool addingToSurface) -> projv::core::ivec3 {
        PickResult pick = pickCrosshairWorldPos(renderInstance);
        projv::core::vec3 worldPos;
        if (pick.hit) {
            worldPos = pick.worldPos;
            // Nudge an ADD sphere back toward the camera by half its radius so it visibly sits
            // ON the surface instead of being buried halfway inside it. A REMOVE (carve) stays
            // centred on the hit so it scoops a crater out of the surface.
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

    // E: add a voxel sphere on the surface under the crosshair.
    static bool prevE = false;
    bool currentE = glfwGetKey(renderInstance.window, GLFW_KEY_E) == GLFW_PRESS;
    if (currentE && !prevE && editState.component != projv::INVALID_COMPONENT_HANDLE) {
        projv::core::ivec3 center = sphereCenter(/*addingToSurface=*/true);
        std::vector<projv::PendingVoxelOp> ops =
            makeSphereOps(center, editState.sphereRadius, editColor(editState.voxelColorIndex++), true);
        projv::utils::queueVoxelAdd(scene, editState.component, ops);
        projv::utils::updateScene(scene);
        projv::graphics::rebuildSceneTextures(scene, gpuData);
        projv::core::warn("ADD sphere r={} at ({}, {}, {}) ({} voxels)",
                          editState.sphereRadius, center.x, center.y, center.z, ops.size());
        cameraMoved = true; // force TAA reset so the new voxels are immediately visible
    }
    prevE = currentE;

    // Q: remove (carve) a voxel sphere from the surface under the crosshair.
    static bool prevQ = false;
    bool currentQ = glfwGetKey(renderInstance.window, GLFW_KEY_Q) == GLFW_PRESS;
    if (currentQ && !prevQ && editState.component != projv::INVALID_COMPONENT_HANDLE) {
        projv::core::ivec3 center = sphereCenter(/*addingToSurface=*/false);
        std::vector<projv::PendingVoxelOp> ops =
            makeSphereOps(center, editState.sphereRadius, projv::Color{0, 0, 0}, false);
        projv::utils::queueVoxelRemove(scene, editState.component, ops);
        projv::utils::updateScene(scene);
        projv::graphics::rebuildSceneTextures(scene, gpuData);
        projv::core::warn("REMOVE sphere r={} at ({}, {}, {}) ({} voxels)",
                          editState.sphereRadius, center.x, center.y, center.z, ops.size());
        cameraMoved = true;
    }
    prevQ = currentQ;

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

    projv::core::warn("Pos: ({:.2f}, {:.2f}, {:.2f})  SphereRadius: {}  Color: {}",
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