// ProjectV Path Tracer
// An interactive real-time path tracer that loads a pre-voxelized scene from disk and renders
// it with one of several selectable renderers. The camera can be freely navigated with keyboard
// controls.
//
// Scene: SponzaScene — voxelized Sponza atrium produced by ProjectV MeshVoxelizer.
//
// ---------------------------------------------------------------------------------------------
// RENDERER SELECTION
// ---------------------------------------------------------------------------------------------
// At startup you are prompted to choose a renderer:
//   1) tree64        — the original accumulation path tracer (resets on camera movement)
//   2) reprojection  — a path tracer with temporal reprojection (stays converged while moving)
//   3) fast          — a cheap, (almost) noise-free direct renderer: hard sun shadows +
//                      screen-space ambient occlusion (SSAO), anti-aliased by temporal jitter (TAA)
//   4) face          — per-face lighting: every voxel face is one flat colour, with full GI +
//                      direct sun, keyed and accumulated per face (cheapest to denoise)
//   5) radiance cascades (world-space) — world-space radiance-cascade GI: the gather traces real
//                      voxel rays through the scene so occlusion / sky visibility are correct,
//                      temporally denoised
//   6) radiance cascades (world-space, per-face) — combines #5 and #4: the same world-space cascade
//                      GI, but probes are anchored to voxel FACE CENTRES (one effective probe per
//                      voxel -> far fewer gather rays) and the GI is temporally accumulated by EXACT
//                      voxel-face identity (no ghosting), then TAA-anti-aliased
//
// The renderers are registered in buildRendererRegistry() below. Each entry is fully
// self-describing (directory, vertex shader, and its own per-frame uniform upload), so adding a
// fourth renderer later is just one more push_back — no changes to startup()/render() needed.
// ---------------------------------------------------------------------------------------------
//
// Controls:
//   W/S       — move forward / backward
//   A/D       — strafe left / right
//   R/F       — move up / down
//   Mouse     — look around (cursor is captured; Esc releases it, left-click re-captures)

#include <functional>
#include <string>
#include <vector>
#include <iostream>

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

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Mouse scroll wheel drives the sun's elevation (a day cycle -- see render()). GLFW scroll
// callbacks are plain C function pointers, so the accumulated wheel offset lives at file scope.
static double g_sunScrollAccum = 0.0;
static void sunScrollCallback(GLFWwindow* /*window*/, double /*xoffset*/, double yoffset) {
    g_sunScrollAccum += yoffset;
}

// =============================================================================
// Renderer modularity
// =============================================================================

// Everything a per-frame uniform upload might need, gathered in one place so
// each renderer module can pick out exactly the uniforms it declares.
struct FrameContext {
    projv::core::vec3 cameraPosition;
    projv::core::vec3 cameraDirection;
    projv::core::vec3 prevCameraPosition;
    projv::core::vec3 prevCameraDirection;
    bool              cameraMoved;
    int               frameCount;
    int               frameCameraLastMovedOn;
    projv::core::vec2 windowResolution;
    projv::core::vec3 sunDirection;   // normalized; scroll-wheel controlled (see render()).
};

// A self-contained description of a selectable renderer.
struct RendererModule {
    std::string name;             // Shown in the selection menu.
    std::string directory;        // Folder holding render.json / resources.json.
    std::string vertexShaderPath; // Compiled vertex shader for its passes.
    // Uploads this renderer's per-frame uniforms. Must set every uniform the
    // renderer's resources.json declares (and may ignore the rest).
    std::function<void(std::shared_ptr<projv::ConstructedRenderer>, const FrameContext&)> uploadUniforms;
};

// Uniforms shared by every renderer in the project.
static void uploadCommonUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    projv::core::vec3 cameraPosition = ctx.cameraPosition;
    projv::core::vec3 cameraDirection = ctx.cameraDirection;
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
}

// Renderer 1: original tree64 accumulation renderer — only the common uniforms.
static void uploadTree64Uniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    uploadCommonUniforms(renderer, ctx);
}

// Renderer 2: reprojection renderer — additionally needs last frame's camera
// so it can project this frame's hits into the previous frame's screen space.
// Renderer 3 (fast) uses the same set: its TAA pass reprojects the same way.
static void uploadReprojectionUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    uploadCommonUniforms(renderer, ctx);
    projv::core::vec3 prevCameraPosition = ctx.prevCameraPosition;
    projv::core::vec3 prevCameraDirection = ctx.prevCameraDirection;
    projv::graphics::setUniformToValue(renderer, "prevCameraPos", prevCameraPosition);
    projv::graphics::setUniformToValue(renderer, "prevCameraDir", prevCameraDirection);
}

// The registry of all available renderers. Add new renderers here.
static std::vector<RendererModule> buildRendererRegistry() {
    std::vector<RendererModule> registry;
    registry.push_back({
        "tree64 (accumulation)",
        "./tree64Renderer/",
        "./tree64Renderer/pathTracerShaders/vs_quad.bin",
        uploadTree64Uniforms
    });
    registry.push_back({
        "reprojection (temporal)",
        "./reprojectionRenderer/",
        "./reprojectionRenderer/pathTracerShaders/vs_quad.bin",
        uploadReprojectionUniforms
    });
    registry.push_back({
        "fast (direct + AO, TAA)",
        "./fastRenderer/",
        "./fastRenderer/pathTracerShaders/vs_quad.bin",
        uploadReprojectionUniforms
    });
    // Renderer 4 (face): shades each voxel FACE as one flat colour with full GI +
    // direct sun. Uses the same uniform set as reprojection/fast -- its accumulation
    // pass reprojects this frame's hit into last frame's camera the same way.
    registry.push_back({
        "face (per-face flat GI)",
        "./faceRenderer/",
        "./faceRenderer/pathTracerShaders/vs_quad.bin",
        uploadReprojectionUniforms
    });
    // Renderer 5 (world-space radiance cascades): radiance-cascade global illumination whose
    // gather traces real voxel-DDA rays through the scene (not the depth buffer), so occlusion
    // and sky visibility are correct (no off-screen blind spots -> sky GI works). Probe sample
    // positions are snapped to a stable world grid for temporal stability, and probe
    // interpolation is geometry-aware (depth + normal) to stop light leaking across surfaces.
    // (The pure screen-space variant that used to sit at slot 5 has been retired.)
    registry.push_back({
        "radiance cascades (world-space GI)",
        "./worldCascadeRenderer/",
        "./worldCascadeRenderer/pathTracerShaders/vs_quad.bin",
        uploadReprojectionUniforms
    });
    // Renderer 6 (world-space PER-FACE radiance cascades): fuses renderer 5's world-space cascade
    // GI with renderer 4's per-face identity. The cascade gather is anchored to voxel FACE CENTRES
    // (so every screen probe on a face shares one stable world origin -> effectively one probe per
    // voxel face, and a coarser probe spacing = far fewer gather rays), and the resolved indirect
    // irradiance is temporally accumulated by EXACT voxel-face key (renderer 4's ghost-free gate)
    // before a TAA pass anti-aliases the composite. Same reprojection uniforms as 4/5.
    registry.push_back({
        "radiance cascades (world-space, per-face)",
        "./worldFaceCascadeRenderer/",
        "./worldFaceCascadeRenderer/pathTracerShaders/vs_quad.bin",
        uploadReprojectionUniforms
    });
    return registry;
}

// Prompts the user on the console to pick a renderer, returning its index.
static size_t promptForRenderer(const std::vector<RendererModule>& registry) {
    std::cout << "\n=== Select a renderer ===\n";
    for (size_t i = 0; i < registry.size(); i++) {
        std::cout << "  " << (i + 1) << ") " << registry[i].name << "\n";
    }
    std::cout << "Enter choice [1-" << registry.size() << "] (default 1): " << std::flush;

    std::string line;
    std::getline(std::cin, line);
    try {
        int choice = std::stoi(line);
        if (choice >= 1 && choice <= (int)registry.size()) {
            return (size_t)(choice - 1);
        }
    } catch (...) {
        // Fall through to default.
    }
    std::cout << "Defaulting to renderer 1 (" << registry[0].name << ").\n";
    return 0;
}

// =============================================================================
// Application stages
// =============================================================================

// Startup: pick the renderer, create the window, load the scene and renderer,
// and upload all GPU resources.
void startup(projv::Application& app) {

    projv::graphics::RenderInstance& renderInstance = projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(1920, 1080, "hengry!");

    // Capture the cursor so mouse motion drives the camera (FPS-style mouse look).
    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // Scroll wheel controls the sun's elevation (day cycle).
    glfwSetScrollCallback(renderInstance.window, sunScrollCallback);

    projv::Scene& scene     = projv::core::createGlobalResource<projv::Scene>(app.world);
    float& cameraPhi         = projv::core::createGlobalResource<float>(app.world);
    projv::GPUData& gpuData  = projv::core::createGlobalResource<projv::GPUData>(app.world);
    RendererModule& selectedRenderer = projv::core::createGlobalResource<RendererModule>(app.world);

    cameraPhi = 3.14 / 2 + 0.4;

    // Eager load: loads all scene geometry up front.
    scene = projv::utils::loadComposeFromDisk("./SponzaScene/");

    std::vector<RendererModule> registry = buildRendererRegistry();
    selectedRenderer = registry[promptForRenderer(registry)];
    projv::core::info("Using renderer: {}", selectedRenderer.name);

    projv::RendererSpecification rendererSpec = projv::graphics::loadRendererSpecification(selectedRenderer.directory);
    renderInstance.addRendererSpecification(1, rendererSpec);

    bgfx::ShaderHandle vsh = projv::graphics::loadShader(selectedRenderer.vertexShaderPath);
    std::shared_ptr<projv::ConstructedRenderer> constructedRenderer = projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vsh);

    // Load the blue-noise LUT used by the path tracers for sample decorrelation.
    int width, height, channels;
    unsigned char* img = stbi_load("LDR_RGBA_7.png", &width, &height, &channels, 4);
    projv::core::info("Blue-noise texture: {}x{}", width, height);
    projv::graphics::setTextureToData(constructedRenderer, 1, img, width, height);

    renderInstance.setActiveRenderer(constructedRenderer);
    gpuData = projv::graphics::createTexturesForScene(scene);
}

// Update: frame timing profiler.
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
        projv::core::perf("Frame stats (last 100): avg={:.2f}ms min={:.2f}ms max={:.2f}ms",
                   sum / 100.0, mn, mx);
    }
#endif
}

// Render: handle camera input, then hand off per-frame uniform upload to the
// selected renderer module before dispatching it.
void render(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance = projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    projv::GPUData& gpuData          = projv::core::getGlobalResource<projv::GPUData>(app.world);
    float& cameraPhi                 = projv::core::getGlobalResource<float>(app.world);
    RendererModule& selectedRenderer = projv::core::getGlobalResource<RendererModule>(app.world);

    static projv::core::vec3 cameraPosition = projv::core::vec3(1018.0, 413.0, -330.0);

    // Previous-frame camera, used by renderers that do temporal reprojection.
    static projv::core::vec3 prevCameraPosition  = cameraPosition;
    static projv::core::vec3 prevCameraDirection = projv::core::vec3(0.0, 0.0, 1.0);
    static bool prevCameraInitialized = false;

    bool cameraMoved = false;

    // Pitch (look up/down). Yaw is stored in the persistent cameraPhi resource.
    static float cameraPitch = 0.0f;

    // Cursor capture toggle: Esc releases the cursor (so it can leave the
    // window), left-click re-captures it. Only look around while captured.
    static bool mouseCaptured = true;
    if (mouseCaptured && glfwGetKey(renderInstance.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        mouseCaptured = false;
    } else if (!mouseCaptured && glfwGetMouseButton(renderInstance.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        mouseCaptured = true;
    }

    // Mouse look. The cursor is captured (GLFW_CURSOR_DISABLED), so we read its
    // absolute position each frame and apply the delta to yaw/pitch.
    static double lastMouseX = 0.0, lastMouseY = 0.0;
    static bool mouseInitialized = false;
    double mouseX, mouseY;
    glfwGetCursorPos(renderInstance.window, &mouseX, &mouseY);
    // Reset the reference point whenever tracking (re)starts, so re-capturing
    // after a release doesn't apply one huge jump.
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
        cameraPhi   += (float)mouseDeltaX * mouseSensitivity;  // right → look right
        cameraPitch -= (float)mouseDeltaY * mouseSensitivity;  // up    → look up
        // Clamp pitch just shy of straight up/down to avoid gimbal flip.
        const float pitchLimit = 1.55f; // ~89°
        if (cameraPitch >  pitchLimit) cameraPitch =  pitchLimit;
        if (cameraPitch < -pitchLimit) cameraPitch = -pitchLimit;
        cameraMoved = true;
    }

    projv::core::vec3 cameraDirection;
    cameraDirection.x = projv::core::cos(cameraPitch) * projv::core::cos(cameraPhi);
    cameraDirection.y = projv::core::sin(cameraPitch);
    cameraDirection.z = projv::core::cos(cameraPitch) * projv::core::sin(cameraPhi);

    // Horizontal forward from yaw only, so W/S flies level regardless of pitch.
    projv::core::vec3 forwardDirection = { projv::core::cos(cameraPhi), 0, projv::core::sin(cameraPhi) };

    // Movement speed in world units per frame. Voxels are 1 world unit (voxelScale = 1), so a
    // res-512 model spans ~512 units; this keeps traversal feeling the same as the old ~37-unit world.
    const float moveSpeed = 4.13f;

    // Horizontal movement (W/A/S/D).
    if (glfwGetKey(renderInstance.window, GLFW_KEY_W)) { cameraPosition += forwardDirection * moveSpeed; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_S)) { cameraPosition -= forwardDirection * moveSpeed; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_A)) {
        float leftPhi = cameraPhi + 3.14 / 2;
        projv::core::vec3 leftDirection = { projv::core::cos(leftPhi), 0, projv::core::sin(leftPhi) };
        cameraPosition -= leftDirection * moveSpeed;
        cameraMoved = true;
    }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_D)) {
        float rightPhi = cameraPhi - 3.14 / 2;
        projv::core::vec3 rightDirection = { projv::core::cos(rightPhi), 0, projv::core::sin(rightPhi) };
        cameraPosition -= rightDirection * moveSpeed;
        cameraMoved = true;
    }

    // Vertical movement (R/F).
    if (glfwGetKey(renderInstance.window, GLFW_KEY_R)) { cameraPosition[1] += moveSpeed; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_F)) { cameraPosition[1] -= moveSpeed; cameraMoved = true; }


    projv::core::warn("Pos: ({:.2f}, {:.2f}, {:.2f})", cameraPosition[0], cameraPosition[1], cameraPosition[2]);

    // On the first frame there is no history yet, so make the previous camera
    // equal to the current one (identity reprojection) to avoid garbage.
    if (!prevCameraInitialized) {
        prevCameraPosition   = cameraPosition;
        prevCameraDirection  = cameraDirection;
        prevCameraInitialized = true;
    }

    // Sun direction from the scroll wheel: the scroll angle traces a FULL great circle (rise ->
    // overhead -> set on the far side -> below the horizon / night -> back), orbiting all the way
    // around in a fixed vertical plane. The angle is unbounded (no clamp) so it never stops; sunDir
    // = cos(theta)*azimuth + sin(theta)*up is unit-length for a unit azimuth, so no renormalize. A
    // sun change counts as "moved" so the temporal history refreshes to the new lighting.
    static double prevSunScroll = 0.0;
    float sunAngle = 0.86f + (float)g_sunScrollAccum * 0.08f;  // radians; ~0.86 default, ~4.6deg/notch
    const float sunAzX = -0.6402f, sunAzZ = 0.7682f;          // = normalize(vec2(-0.5, 0.6))
    float ca = projv::core::cos(sunAngle), sa = projv::core::sin(sunAngle);
    projv::core::vec3 sunDirection = { ca * sunAzX, sa, ca * sunAzZ };
    bool sunMoved = (g_sunScrollAccum != prevSunScroll);
    prevSunScroll = g_sunScrollAccum;
    cameraMoved = cameraMoved || sunMoved;

    // Track the frame the camera last moved on so the shader can reset accumulation.
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

    selectedRenderer.uploadUniforms(renderInstance.getActiveRenderer(), ctx);

    projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpuData);

    // Remember this frame's camera for next frame's reprojection.
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
