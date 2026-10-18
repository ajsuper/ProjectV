// =============================================================================================
// 45 -- Game Ready Render
// =============================================================================================
//
// A renderer to put under a game: cheap, steady, and good-looking at 120 frames a second. It grew from
// the physics demos' renderer (17-physics-demos/demoRenderer) -- a G-buffer, SSAO, a shade pass and the
// voxel reconstruction -- and adds only what that one lacked for a finished image:
//
//   * TAA after the reconstruction, to steady motion. (The sub-pixel jitter it can integrate is off by
//     default -- see FrameState::jitter -- because cleanliness is the priority here over detail.)
//   * A light rig: a sun that warms toward sunset, a sky, and an ambient built from the two (sky above,
//     the sun bounced off the ground below). That ambient times a two-scale AO -- contact creases plus
//     a wide "how enclosed is this point" term -- is the whole indirect light. It is GI in the sense a
//     game needs: shadowed sides are lit, arcades and doorways fall off into darkness, nothing is
//     noisy, and it costs no rays.
//   * A distance blur over voxels only a few pixels across, so far foliage is not per-voxel speckle.
//   * Height fog with a closed-form optical depth, and an ACES grade.
//
// Seven passes, one primary and one shadow ray per render pixel, everything else screen space. The
// render target is 0.4 of the window on each axis, and the reconstruction rebuilds every voxel edge at
// full resolution from the voxel faces themselves.
//
// ---------------------------------------------------------------------------------------------
// Controls:
//   W/A/S/D, R/F  fly; Shift x5          Mouse  look (Esc releases, left-click recaptures)
//   Scroll        sun elevation          Z X C `  render scale 1.0 / 0.5 / 0.4 / 0.33
//   Q             reconstruction bypass  T  TAA on/off    E  sub-pixel jitter on/off
//   G             AO on/off              H  sun shadows on/off   V  fog off / normal / heavy
//   I             ray-traced GI on/off (off: the cheap sky ambient and the wide AO instead)
//   J             screen-space colour bounce off / normal / strong
//   U             debug view: off / reconstruction coverage / AO only / raw sun visibility /
//                 the colour bounce only / the ray-traced GI only
//   Y             per-pass GPU timing to the log      P  print the camera as GAME_CAMERA
//
// Environment (for scripted runs and measurements):
//   GAME_RENDER_SCALE=0.4     render scale at launch (0.05..1)
//   GAME_CAMERA=x,y,z,yaw,pitch   start camera;  GAME_LOCK_CAMERA=1  ignore input
//   GAME_FOG=<mult>           fog density multiplier (0 = off)
//   GAME_SUN=<notches>        start the sun this many scroll notches from its default (negative: lower)
//   GAME_SHADOWS, GAME_AO, GAME_TAA, GAME_RAYGI = 0|1, GAME_BOUNCE = 0..2,
//   GAME_DEBUG_VIEW = 0..4 or 7   the toggles, at launch
//   GAME_PROFILE=1            per-pass GPU timing from the first frame
//   GAME_BENCH=<seconds>      measure for that long after a 3 s warm-up, print the result, exit
//   GAME_BENCH_SPIN=<rad/s>   turn the camera at this rate while benchmarking (the moving-camera case)
//   GAME_CAPTURE=<file.ppm>   write the frame after GAME_CAPTURE_FRAME
//                             frames (default 240), then exit. GAME_CAPTURE_TEXTURE=<texID> dumps that
//                             render target's raw values instead (see Capture)
// =============================================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/application.h"
#include "core/log.h"
#include "core/math.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "utils/compose_io.h"

#include <bgfx/bgfx.h>
#include <GLFW/glfw3.h>

// The default scene is the kind of thing this renderer is for: Cave, a terrain block of meadow, rock
// outcrops and pine forest. It opens on the ground among the grass rather than at the automatic
// framing, which sits outside the whole scene and shows a small scene as a speck. Any compose folder
// can be given instead; SanMiguel (../scene_previewer/scenes/SanMiguel/) is the heavier test.
static const char* DEFAULT_SCENE_PATH = "../scene_previewer/scenes/Cave/";
static const char* DEFAULT_SCENE_CAMERA = "-270,245,-240,0.644,0.0";
static const char* RENDERER_DIRECTORY = "./gameReadyRenderer/";
static const char* VERTEX_SHADER_PATH = "./gameReadyRenderer/shaders/vs_quad.bin";

// Must match FOV in the shaders.
static const float CAMERA_VERTICAL_FOV_DEGREES = 60.0f;

// ---- The light rig ------------------------------------------------------------------------------
// Intensities are in the shaders' radiance units: a white surface facing the noon sun reflects about
// SUN_INTENSITY / pi. EXPOSURE scales the whole frame before the tone map.
static const float SUN_INTENSITY = 8.0f;
static const float SKY_INTENSITY = 1.0f;
static const float EXPOSURE = 1.0f;

// ---- Fog, in units of the scene -------------------------------------------------------------------
// The fraction of the light from one bounding-sphere radius away that is lost to haze, at the floor.
// Inverted through Beer-Lambert at load, so "fog" means the same thing in a small model and a city.
static const float FOG_DEPTH_AT_RADIUS = 0.08f;
static const float FOG_HEIGHT_FRACTION = 0.35f;
static const float FOG_INSCATTER = 0.25f;

// 0.4 of the window on each axis, 16% of the pixels. 0.5 (a quarter) is visibly a little crisper and
// about 1.5 ms dearer in a dense view; C and X switch between them live.
static const float DEFAULT_RENDER_SCALE = 0.4f;

// Scroll wheel -> sun. GLFW callbacks are plain function pointers, so the accumulator is file scope.
static double g_sunScrollAccum = 0.0;
static void sunScrollCallback(GLFWwindow*, double, double yoffset) { g_sunScrollAccum += yoffset; }

// =============================================================================================
// Scene measurements
// =============================================================================================

static float detectVoxelSize(const projv::Scene& scene) {
    for (const projv::Chunk& chunk : scene.chunks) {
        if (!chunk.alive || chunk.header.scale <= 0.0f || chunk.header.resolution == 0) continue;
        return chunk.header.scale / float(chunk.header.resolution);
    }
    return 1.0f;
}

struct CameraFraming {
    projv::core::vec3 position;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float moveSpeed = 1.0f;     // World units per SECOND -- flight speed is frame-rate independent.
    float sceneRadius = 1.0f;
    float sceneFloorY = 0.0f;
};

// Outside the bounding sphere, raised three-quarter view of its centre -- the same framing as the
// scene editor and example 40. GAME_CAMERA overrides it, which is what makes two runs comparable.
static CameraFraming frameScene(const projv::Scene& scene, const char* startCamera) {
    using namespace projv::core;
    CameraFraming framing;
    framing.yaw = 3.14159265f * 0.25f;
    framing.pitch = -0.35f;

    bool found = false;
    vec3 boundsMin(0.0f), boundsMax(0.0f);
    for (const projv::Chunk& chunk : scene.chunks) {
        if (!chunk.alive || chunk.header.scale <= 0.0f) continue;
        vec3 chunkMin = chunk.header.position;
        vec3 chunkMax = chunk.header.position + vec3(chunk.header.scale);
        boundsMin = found ? min(boundsMin, chunkMin) : chunkMin;
        boundsMax = found ? max(boundsMax, chunkMax) : chunkMax;
        found = true;
    }
    if (!found) {
        warn("Scene has no live chunks -- there will be nothing but sky to render.");
        framing.position = vec3(0.0f, 0.0f, -100.0f);
        return framing;
    }

    info("Scene bounds: ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f})", boundsMin.x, boundsMin.y,
         boundsMin.z, boundsMax.x, boundsMax.y, boundsMax.z);
    vec3 center = (boundsMin + boundsMax) * 0.5f;
    float radius = std::max(length(boundsMax - boundsMin) * 0.5f, 1.0f);
    framing.sceneRadius = radius;
    framing.sceneFloorY = boundsMin.y;

    const float fovRadians = CAMERA_VERTICAL_FOV_DEGREES * 3.14159265f / 180.0f;
    float distance = (radius / std::tan(fovRadians * 0.5f)) * 1.25f;
    vec3 forward = {std::cos(framing.pitch) * std::cos(framing.yaw), std::sin(framing.pitch),
                    std::cos(framing.pitch) * std::sin(framing.yaw)};
    framing.position = center - forward * distance;
    // Crossing the scene takes about seven seconds, whatever its size.
    framing.moveSpeed = std::max(radius * 0.3f, 0.5f);

    const char* overrideCamera = std::getenv("GAME_CAMERA");
    if (overrideCamera == nullptr) overrideCamera = startCamera;
    if (overrideCamera != nullptr) {
        float x, y, z, yaw, pitch;
        if (std::sscanf(overrideCamera, "%f,%f,%f,%f,%f", &x, &y, &z, &yaw, &pitch) == 5) {
            framing.position = vec3(x, y, z);
            framing.yaw = yaw;
            framing.pitch = pitch;
        } else {
            warn("GAME_CAMERA is not \"x,y,z,yaw,pitch\" -- ignoring it: {}", overrideCamera);
        }
    }
    return framing;
}

// =============================================================================================
// Per-frame state
// =============================================================================================

struct FrameState {
    projv::core::vec3 cameraPosition;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float moveSpeed = 1.0f;

    // The camera the previous frame was rendered with: taa reprojects through it.
    projv::core::vec3 prevCameraPosition;
    projv::core::vec3 prevCameraDirection = {0.0f, 0.0f, 1.0f};
    bool prevCameraInitialized = false;

    bool mouseCaptured = true;
    bool mouseInitialized = false;
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;
    double prevSunScroll = 0.0;
    std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();

    // Declared scale of every relative target, captured before any scaling, so a new render scale is
    // derived from the originals rather than by compounding ratios.
    std::unordered_map<unsigned int, float> declaredTextureScales;
    float renderScale = DEFAULT_RENDER_SCALE;

    float voxelSize = 1.0f;
    float fogDensity = 0.0f;
    float fogHeight = 1.0f;
    float fogFloorY = 0.0f;
    float fogScale = 1.0f;

    bool reconstructionBypass = false;
    bool ambientOcclusion = true;
    bool shadows = true;
    bool rayGI = true;   // the traced large-scale light (I)
    int bounce = 1;      // the screen-space colour bounce: off, normal, strong (J)
    // 0 off, 1 reconstruction coverage, 2 AO only, 3 sun visibility, 4 the colour bounce, 7 the ray GI
    int debugView = 0;
    bool temporalFilter = true;
    // Off: the reconstruction antialiases every edge it can see in one frame, and without the jitter a
    // still frame is identical to the last, so nothing can shimmer. On (E) buys sub-render-pixel detail
    // at the price of exactly that shimmer.
    bool jitter = false;
    bool gpuProfile = false;
};

struct SceneRequest {
    std::string path = DEFAULT_SCENE_PATH;
};

// =============================================================================================
// Render scale
// =============================================================================================

// Targets that live at OUTPUT resolution: the reconstruction writes one, taa the other.
// Scaling them would put the magnify back at the end of the chain, after the temporal filter.
static bool isOutputResolutionTarget(const std::string& name) {
    return name == "reconstructed" || name == "taaColor";
}

static float readRenderScale() {
    const char* raw = std::getenv("GAME_RENDER_SCALE");
    if (raw == nullptr) return DEFAULT_RENDER_SCALE;
    float requested = std::strtof(raw, nullptr);
    if (!(requested > 0.05f) || requested > 1.0f) {
        projv::core::warn("GAME_RENDER_SCALE must be in (0.05, 1.0] -- ignoring it: {}", raw);
        return DEFAULT_RENDER_SCALE;
    }
    return requested;
}

// Rescales a constructed renderer. resizeRenderTargets re-derives each relative target's size from
// relativeTextureScales every frame, so writing the map is all it takes.
static void setRenderScale(projv::graphics::RenderInstance& renderInstance, FrameState& state,
                           float renderScale) {
    std::shared_ptr<projv::ConstructedRenderer> renderer = renderInstance.getActiveRenderer();
    if (!renderer) return;
    for (auto& [textureID, scale] : renderer->resources.textures.relativeTextureScales) {
        auto declared = state.declaredTextureScales.find(textureID);
        if (declared != state.declaredTextureScales.end()) scale = declared->second * renderScale;
    }
    state.renderScale = renderScale;
    projv::core::info("Render scale -> {:.2f} ({:.0f}% of the pixels).", renderScale,
                      renderScale * renderScale * 100.0f);
}

// =============================================================================================
// Startup
// =============================================================================================

// bgfx view i is render.json pass i, so this is positional; startup refuses to label on a mismatch.
static const char* const PASS_NAMES[] = {
    "gbuffer", "ssao", "raygi", "giaccum", "giblur1", "giblur2", "shade", "reconstruct", "taa", "display"
};
static const size_t PASS_NAME_COUNT = sizeof(PASS_NAMES) / sizeof(PASS_NAMES[0]);

void startup(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance =
        app.world.ctx().emplace<projv::graphics::RenderInstance>();
    renderInstance.initialize(1920, 1080, "ProjectV Game Ready Render");
    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetScrollCallback(renderInstance.window, sunScrollCallback);

    SceneRequest& request = app.world.ctx().emplace<SceneRequest>();
    projv::Scene& scene = app.world.ctx().emplace<projv::Scene>();
    projv::GPUData& gpuData = app.world.ctx().emplace<projv::GPUData>();
    FrameState& state = app.world.ctx().emplace<FrameState>();

    projv::core::info("Loading scene: {}", request.path);
    scene = projv::utils::loadComposeFromDisk(request.path);
    if (scene.chunks.empty()) {
        if (!std::filesystem::exists(request.path)) {
            projv::core::error("Scene folder not found: {}. Pass one on the command line, or run "
                               "from build/examples/game_ready_render.", request.path);
        } else {
            projv::core::error("Scene at {} loaded no chunks -- if the log reports a rejected .data "
                               "version, that asset needs re-voxelizing.", request.path);
        }
    }

    state.voxelSize = detectVoxelSize(scene);
    projv::core::info("Voxel size {:.4f} world units -- the unit the AO radii and shadow bias use.",
                      state.voxelSize);
    // The default scene has a known good start; anything else is framed from its bounds.
    CameraFraming framing = frameScene(scene, request.path == DEFAULT_SCENE_PATH ? DEFAULT_SCENE_CAMERA
                                                                                 : nullptr);
    state.cameraPosition = framing.position;
    state.prevCameraPosition = framing.position;
    state.yaw = framing.yaw;
    state.pitch = framing.pitch;
    state.moveSpeed = framing.moveSpeed;

    // T = exp(-d * r)  =>  d = -ln(T) / r, with T = 1 - FOG_DEPTH_AT_RADIUS.
    state.fogHeight = std::max(framing.sceneRadius * FOG_HEIGHT_FRACTION, 1e-3f);
    state.fogFloorY = framing.sceneFloorY;
    state.fogDensity = -std::log(std::max(1.0f - FOG_DEPTH_AT_RADIUS, 1e-4f)) /
                       std::max(framing.sceneRadius, 1e-3f);
    if (const char* fog = std::getenv("GAME_FOG")) {
        float scale = std::strtof(fog, nullptr);
        if (scale >= 0.0f && scale <= 100.0f) state.fogScale = scale;
    }
    state.gpuProfile = std::getenv("GAME_PROFILE") != nullptr;
    // The keyboard toggles, for scripted A/B runs that have no keyboard.
    if (const char* v = std::getenv("GAME_SUN")) g_sunScrollAccum = std::atof(v);
    if (const char* v = std::getenv("GAME_SHADOWS")) state.shadows = std::atoi(v) != 0;
    if (const char* v = std::getenv("GAME_RAYGI")) state.rayGI = std::atoi(v) != 0;
    if (const char* v = std::getenv("GAME_BOUNCE")) state.bounce = std::clamp(std::atoi(v), 0, 2);
    if (const char* v = std::getenv("GAME_AO")) state.ambientOcclusion = std::atoi(v) != 0;
    if (const char* v = std::getenv("GAME_TAA")) state.temporalFilter = std::atoi(v) != 0;
    if (const char* v = std::getenv("GAME_DEBUG_VIEW")) state.debugView = std::clamp(std::atoi(v), 0, 7);

    projv::RendererSpecification rendererSpec =
        projv::graphics::loadRendererSpecification(RENDERER_DIRECTORY);
    for (projv::Texture& texture : rendererSpec.resources.textures) {
        if (texture.sizeMode != projv::TextureSizeMode::Relative) continue;
        if (isOutputResolutionTarget(texture.name)) continue;
        state.declaredTextureScales[texture.textureID] = texture.scale;
    }
    state.renderScale = readRenderScale();
    for (projv::Texture& texture : rendererSpec.resources.textures) {
        if (texture.sizeMode != projv::TextureSizeMode::Relative) continue;
        if (isOutputResolutionTarget(texture.name)) continue;
        texture.scale *= state.renderScale;
    }
    projv::core::info("Render scale {:.2f} ({:.0f}% of the pixels); the reconstruction and TAA "
                      "rebuild the output.", state.renderScale,
                      state.renderScale * state.renderScale * 100.0f);
    renderInstance.addRendererSpecification(1, rendererSpec);

    bgfx::ShaderHandle vertexShader = projv::graphics::loadShader(VERTEX_SHADER_PATH);
    std::shared_ptr<projv::ConstructedRenderer> renderer =
        projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1),
                                                        vertexShader);
    renderInstance.setActiveRenderer(renderer);
    gpuData = projv::graphics::createTexturesForScene(scene);

    if (renderer->dependencyGraph.size() == PASS_NAME_COUNT) {
        for (size_t i = 0; i < PASS_NAME_COUNT; i++) {
            bgfx::setViewName(bgfx::ViewId(renderer->dependencyGraph[i].renderPassID), PASS_NAMES[i]);
        }
    } else {
        projv::core::warn("render.json has {} passes but PASS_NAMES has {} -- views left unnamed.",
                          renderer->dependencyGraph.size(), PASS_NAME_COUNT);
    }
    if (state.gpuProfile) bgfx::setDebug(BGFX_DEBUG_PROFILER);

    std::printf(
        "\n"
        "  ProjectV Game Ready Render\n"
        "  ----------------------------------------------------------------------\n"
        "   W A S D / R F   fly (Shift x5)     Mouse   look (Esc frees the cursor)\n"
        "   Scroll          sun elevation       Z X C `  scale 1.0/0.5/0.4/0.33\n"
        "   Q  reconstruction bypass   T  TAA on/off   E  jitter on/off\n"
        "   G  AO on/off   H  shadows on/off   V  fog off/normal/heavy\n"
        "   I  ray-traced GI on/off   J  colour bounce off/normal/strong\n"
        "   U  debug view: off / reconstruction coverage / AO only / sun visibility / bounce / ray GI\n"
        "   Y  per-pass GPU timing (logged every 120 frames)   P  print camera\n"
        "  ----------------------------------------------------------------------\n\n");
    std::fflush(stdout);
}

// =============================================================================================
// Input
// =============================================================================================

static bool keyPressed(GLFWwindow* window, int key) {
    // Edge-triggered: true on the frame the key goes down. One slot per GLFW key code.
    static bool previous[GLFW_KEY_LAST + 1] = {};
    bool down = glfwGetKey(window, key) == GLFW_PRESS;
    bool pressed = down && !previous[key];
    previous[key] = down;
    return pressed;
}

static bool updateMouseLook(GLFWwindow* window, FrameState& state) {
    if (state.mouseCaptured && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        state.mouseCaptured = false;
    } else if (!state.mouseCaptured &&
               glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        state.mouseCaptured = true;
    }

    double mouseX, mouseY;
    glfwGetCursorPos(window, &mouseX, &mouseY);
    if (!state.mouseInitialized || !state.mouseCaptured) {
        state.lastMouseX = mouseX;
        state.lastMouseY = mouseY;
        state.mouseInitialized = true;
    }
    double deltaX = mouseX - state.lastMouseX;
    double deltaY = mouseY - state.lastMouseY;
    state.lastMouseX = mouseX;
    state.lastMouseY = mouseY;
    if (!state.mouseCaptured || (deltaX == 0.0 && deltaY == 0.0)) return false;

    const float sensitivity = 0.0025f;
    state.yaw += float(deltaX) * sensitivity;
    state.pitch = std::clamp(state.pitch - float(deltaY) * sensitivity, -1.55f, 1.55f);
    return true;
}

static bool updateMovement(GLFWwindow* window, FrameState& state, float deltaSeconds) {
    float step = state.moveSpeed * deltaSeconds;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) step *= 5.0f;
    projv::core::vec3 forward = {std::cos(state.yaw), 0.0f, std::sin(state.yaw)};
    projv::core::vec3 right = {-std::sin(state.yaw), 0.0f, std::cos(state.yaw)};

    bool moved = false;
    if (glfwGetKey(window, GLFW_KEY_W)) { state.cameraPosition += forward * step; moved = true; }
    if (glfwGetKey(window, GLFW_KEY_S)) { state.cameraPosition -= forward * step; moved = true; }
    if (glfwGetKey(window, GLFW_KEY_D)) { state.cameraPosition += right * step;   moved = true; }
    if (glfwGetKey(window, GLFW_KEY_A)) { state.cameraPosition -= right * step;   moved = true; }
    if (glfwGetKey(window, GLFW_KEY_R)) { state.cameraPosition.y += step;         moved = true; }
    if (glfwGetKey(window, GLFW_KEY_F)) { state.cameraPosition.y -= step;         moved = true; }
    return moved;
}

static void handleToggles(projv::graphics::RenderInstance& renderInstance, FrameState& state) {
    GLFWwindow* window = renderInstance.window;
    if (keyPressed(window, GLFW_KEY_Z)) setRenderScale(renderInstance, state, 1.0f);
    if (keyPressed(window, GLFW_KEY_X)) setRenderScale(renderInstance, state, 0.5f);
    if (keyPressed(window, GLFW_KEY_C)) setRenderScale(renderInstance, state, 0.4f);
    if (keyPressed(window, GLFW_KEY_GRAVE_ACCENT)) setRenderScale(renderInstance, state, 0.33f);

    if (keyPressed(window, GLFW_KEY_Q)) {
        state.reconstructionBypass = !state.reconstructionBypass;
        projv::core::info("Reconstruction -> {}", state.reconstructionBypass ? "BYPASSED (point sampled)" : "on");
    }
    if (keyPressed(window, GLFW_KEY_T)) {
        state.temporalFilter = !state.temporalFilter;
        projv::core::info("TAA -> {}", state.temporalFilter ? "on" : "off");
    }
    if (keyPressed(window, GLFW_KEY_E)) {
        state.jitter = !state.jitter;
        projv::core::info("Sub-pixel jitter -> {}", state.jitter ? "on" : "off");
    }
    if (keyPressed(window, GLFW_KEY_G)) {
        state.ambientOcclusion = !state.ambientOcclusion;
        projv::core::info("AO -> {}", state.ambientOcclusion ? "on" : "off");
    }
    if (keyPressed(window, GLFW_KEY_I)) {
        state.rayGI = !state.rayGI;
        projv::core::info("Ray-traced GI -> {}", state.rayGI ? "on" : "off (sky ambient and wide AO)");
    }
    if (keyPressed(window, GLFW_KEY_J)) {
        state.bounce = (state.bounce + 1) % 3;
        static const char* const names[] = {"off", "normal", "strong"};
        projv::core::info("Colour bounce -> {}", names[state.bounce]);
    }
    if (keyPressed(window, GLFW_KEY_H)) {
        state.shadows = !state.shadows;
        projv::core::info("Sun shadows -> {}", state.shadows ? "on" : "off");
    }
    if (keyPressed(window, GLFW_KEY_U)) {
        state.debugView = state.debugView == 4 ? 7 : state.debugView == 7 ? 0 : state.debugView + 1;
        static const char* const names[] = {"off", "reconstruction coverage (green claimed, red fell back)",
                                            "AO only", "raw sun visibility",
                                            "the screen-space colour bounce only (x4)", "", "",
                                            "the ray-traced GI only"};
        projv::core::info("Debug view -> {}", names[state.debugView]);
    }
    if (keyPressed(window, GLFW_KEY_V)) {
        state.fogScale = state.fogScale <= 0.001f ? 1.0f : (state.fogScale <= 1.001f ? 3.0f : 0.0f);
        projv::core::info("Fog -> {:.0f}x", state.fogScale);
    }
    if (keyPressed(window, GLFW_KEY_P)) {
        // In GAME_CAMERA's own format, so a view found by flying can be pasted into a scripted run.
        projv::core::info("GAME_CAMERA=\"{:.2f},{:.2f},{:.2f},{:.4f},{:.4f}\"", state.cameraPosition.x,
                          state.cameraPosition.y, state.cameraPosition.z, state.yaw, state.pitch);
    }
    if (keyPressed(window, GLFW_KEY_Y)) {
        state.gpuProfile = !state.gpuProfile;
        bgfx::setDebug(state.gpuProfile ? BGFX_DEBUG_PROFILER : BGFX_DEBUG_NONE);
        projv::core::info("GPU pass timing -> {}", state.gpuProfile ? "on" : "off");
    }
}

// The sun on a full great circle in a fixed vertical plane, driven by the scroll wheel.
static projv::core::vec4 sunFromScroll() {
    float angle = 0.86f + float(g_sunScrollAccum) * 0.08f;
    const float azimuthX = -0.6402f, azimuthZ = 0.7682f;
    float c = std::cos(angle), s = std::sin(angle);
    return {c * azimuthX, s, c * azimuthZ, 0.0f};
}

// =============================================================================================
// Uniforms
// =============================================================================================

// Every uniform resources.json declares is set here, every frame: one the renderer declares but never
// uploads holds whatever bgfx last had, which is a black sun rather than an error.
static void uploadFrameUniforms(const std::shared_ptr<projv::ConstructedRenderer>& renderer,
                                const FrameState& state, projv::core::vec3 cameraDirection,
                                projv::core::vec4 sunDirection, bool sceneChanged, int frame) {
    using projv::graphics::setUniformToValue;
    // Locals: setUniformToValue deduces the type from its argument and does not take a const member.
    projv::core::vec3 cameraPosition = state.cameraPosition;
    projv::core::vec3 prevCameraPosition = state.prevCameraPosition;
    projv::core::vec3 prevCameraDirection = state.prevCameraDirection;
    setUniformToValue(renderer, "cameraPos", cameraPosition);
    setUniformToValue(renderer, "cameraDir", cameraDirection);
    setUniformToValue(renderer, "prevCameraPos", prevCameraPosition);
    setUniformToValue(renderer, "prevCameraDir", prevCameraDirection);

    setUniformToValue(renderer, "sunDir", sunDirection);

    // x = sun intensity, y = sky intensity, z = exposure, w = the finest voxel's edge (world units).
    projv::core::vec4 lightParams = {SUN_INTENSITY, SKY_INTENSITY, EXPOSURE, state.voxelSize};
    setUniformToValue(renderer, "lightParams", lightParams);

    // x = extinction at the floor, y = the height it thins over, z = that floor, w = sun inscatter.
    projv::core::vec4 fogParams = {state.fogDensity * state.fogScale, state.fogHeight, state.fogFloorY,
                                   FOG_INSCATTER * std::min(state.fogScale, 1.5f)};
    setUniformToValue(renderer, "fogParams", fogParams);

    // x = frame index (the jitter's phase), y = jitter amplitude in render pixels, z = TAA on,
    // w = the camera moved (taa reprojects only then, and samples its own texel exactly otherwise).
    projv::core::vec4 frameParams = {float(frame), state.jitter && state.temporalFilter ? 1.0f : 0.0f,
                                     state.temporalFilter ? 1.0f : 0.0f, sceneChanged ? 1.0f : 0.0f};
    setUniformToValue(renderer, "frameParams", frameParams);

    // x = sun shadows, y = AO, z = reconstruction bypass, w = debug view.
    projv::core::vec4 renderParams = {state.shadows ? 1.0f : 0.0f, state.ambientOcclusion ? 1.0f : 0.0f,
                                      state.reconstructionBypass ? 1.0f : 0.0f, float(state.debugView)};
    setUniformToValue(renderer, "renderParams", renderParams);

    // x = the screen-space colour bounce's strength (0 off, and off with the AO), y = ray-traced GI on.
    static const float bounceStrength[3] = {0.0f, 2.5f, 5.0f};
    projv::core::vec4 giParams = {state.ambientOcclusion ? bounceStrength[state.bounce] : 0.0f,
                                  state.rayGI ? 1.0f : 0.0f, 0.0f, 0.0f};
    setUniformToValue(renderer, "giParams", giParams);
}

// =============================================================================================
// Measurement
// =============================================================================================

// Per-pass GPU time, averaged over 120 frames (a single frame's timestamps are too noisy to rank by).
static void reportGpuPasses(const FrameState& state) {
    static double passMs[32] = {};
    static double frameMs = 0.0;
    static int samples = 0;

    const bgfx::Stats* stats = bgfx::getStats();
    if (stats == nullptr || stats->gpuTimerFreq <= 0) return;
    const double toMs = 1000.0 / double(stats->gpuTimerFreq);
    for (uint16_t v = 0; v < stats->numViews; v++) {
        const bgfx::ViewStats& viewStats = stats->viewStats[v];
        if (viewStats.view < 32) passMs[viewStats.view] += double(viewStats.gpuTimeEnd - viewStats.gpuTimeBegin) * toMs;
    }
    frameMs += double(stats->gpuTimeEnd - stats->gpuTimeBegin) * toMs;
    if (++samples < 120) return;

    const double inv = 1.0 / double(samples);
    projv::core::info("---- GPU per pass, mean of {} frames, render scale {:.2f} ----", samples,
                      state.renderScale);
    for (size_t i = 0; i < 32; i++) {
        if (passMs[i] <= 0.0) continue;
        projv::core::info("  {:>12}  {:6.3f} ms", i < PASS_NAME_COUNT ? PASS_NAMES[i] : "?",
                          passMs[i] * inv);
    }
    projv::core::info("  {:>12}  {:6.3f} ms", "FRAME", frameMs * inv);
    for (double& ms : passMs) ms = 0.0;
    frameMs = 0.0;
    samples = 0;
}

// GAME_BENCH: frame times over a fixed window after a warm-up, then exit. The warm-up covers the scene
// upload, the shader pipeline creation and the TAA history filling, none of which a player sees twice.
struct Benchmark {
    double seconds = 0.0;
    double warmup = 3.0;
    double spin = 0.0;
    double elapsed = 0.0;
    int frames = 0;
    std::vector<double> frameMs;
};

static bool updateBenchmark(Benchmark& bench, double deltaSeconds, GLFWwindow* window,
                            const FrameState& state) {
    // The frame that crosses the warm-up is not counted: the first frame alone builds every pipeline
    // and takes seconds, and it would land in the window if the test came after the add.
    // The first frames are not counted toward the warm-up either: the very first builds every
    // pipeline and alone takes longer than the warm-up is.
    if (++bench.frames <= 10) return false;
    if (bench.elapsed < bench.warmup) {
        bench.elapsed += deltaSeconds;
        return false;
    }
    bench.elapsed += deltaSeconds;
    bench.frameMs.push_back(deltaSeconds * 1000.0);
    if (bench.elapsed < bench.warmup + bench.seconds) return false;

    std::vector<double> sorted = bench.frameMs;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double ms : sorted) sum += ms;
    double mean = sum / double(sorted.size());
    auto percentile = [&](double p) { return sorted[size_t(p * double(sorted.size() - 1))]; };
    int width = 0, height = 0;
    glfwGetWindowSize(window, &width, &height);
    std::printf("BENCH frames=%zu mean=%.3fms (%.1f fps) p50=%.3fms p95=%.3fms p99=%.3fms "
                "window=%dx%d scale=%.2f spin=%.2f\n",
                sorted.size(), mean, 1000.0 / mean, percentile(0.5), percentile(0.95),
                percentile(0.99), width, height, state.renderScale, bench.spin);
    std::fflush(stdout);
    return true;
}

// GAME_CAPTURE=<file.ppm>: write the anti-aliased frame to disk after GAME_CAPTURE_FRAME frames (240 by
// default, so the TAA history has converged), then exit. For checking a change by looking at it from a
// script -- the window itself cannot always be screenshotted.
//
// It reads taaColor, the last texture in the chain (display.frag draws straight into the back buffer),
// and grades it here on the CPU with display.frag's curve -- the same picture minus the vignette.
struct Capture {
    std::string path;
    int atFrame = 240;
    int stage = 0;                // 0 waiting, 1 blit + read requested, 2 written
    int framesSinceRequest = 0;
    bgfx::TextureHandle readback = BGFX_INVALID_HANDLE;
    std::vector<uint16_t> pixels;
    int width = 0, height = 0;
    // GAME_CAPTURE_TEXTURE=<texID>: dump that render target's raw RGBA16F values as float32 instead
    // (a 'w h' text line, then w*h*4 floats). For measuring what a debug pass wrote into the G-buffer.
    int textureID = -1;
};

static float halfToFloat(uint16_t h) {
    uint32_t sign = uint32_t(h >> 15) << 31;
    uint32_t exponent = (h >> 10) & 0x1f;
    uint32_t mantissa = h & 0x3ff;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) bits = sign;
        else {  // subnormal: renormalise
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400) == 0) { mantissa <<= 1; exponent--; }
            bits = sign | (exponent << 23) | ((mantissa & 0x3ff) << 13);
        }
    } else if (exponent == 31) bits = sign | 0x7f800000 | (mantissa << 13);
    else bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// display.frag's grade, minus the vignette and dither: exposure, ACES, gamma, contrast,
// saturation. Kept in step with that file's #defines by hand.
static void gradePixel(const float hdr[3], uint8_t out[3]) {
    const float exposure = EXPOSURE, tint[3] = {1.0f, 1.0f, 1.0f}, contrast = 1.12f, saturation = 1.25f;
    float c[3];
    for (int i = 0; i < 3; i++) {
        float x = std::max(hdr[i], 0.0f) * exposure * tint[i];
        x = std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
        c[i] = (std::pow(x, 1.0f / 2.2f) - 0.5f) * contrast + 0.5f;
    }
    float luma = c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f;
    for (int i = 0; i < 3; i++) {
        out[i] = uint8_t(std::clamp(luma + (c[i] - luma) * saturation, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

// Returns true once the file is written.
static bool updateCapture(Capture& capture, const std::shared_ptr<projv::ConstructedRenderer>& renderer,
                          int frame, bool debugView) {
    if (capture.path.empty() || capture.stage == 2) return capture.stage == 2;
    const uint32_t TAA_COLOR = 10, TAA_FBO = 5;

    if (capture.stage == 0) {
        if (frame < capture.atFrame) return false;
        projv::ConstructedTextures& textures = renderer->resources.textures;
        const uint32_t sourceID = capture.textureID >= 0 ? uint32_t(capture.textureID) : TAA_COLOR;
        projv::core::ivec2 size = textures.textureResolutions.at(sourceID);
        capture.width = size.x;
        capture.height = size.y;
        capture.readback = bgfx::createTexture2D(uint16_t(size.x), uint16_t(size.y), false, 1,
                                                 bgfx::TextureFormat::RGBA16F,
                                                 BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        // The ping-pong half taa wrote LAST frame; nothing overwrites it before this frame's view 0,
        // and a blit runs ahead of its view's draws.
        uint32_t fbo = sourceID == TAA_COLOR ? TAA_FBO : textures.textureIDToFrameBufferID.at(sourceID);
        bool primary = !textures.pingPongFlags.at(sourceID) ||
                       renderer->resources.framebuffers.primaryWasLastRenderedToo.at(int(fbo));
        bgfx::TextureHandle source = primary ? textures.textureHandles.at(sourceID)
                                             : textures.textureHandlesAlternate.at(sourceID);
        capture.pixels.assign(size_t(size.x) * size_t(size.y) * 4, 0);
        bgfx::blit(0, capture.readback, 0, 0, source);
        bgfx::readTexture(capture.readback, capture.pixels.data());
        capture.stage = 1;
        return false;
    }
    // readTexture's data lands a frame or two later; four is comfortably past it.
    if (++capture.framesSinceRequest < 4) return false;

    std::FILE* file = std::fopen(capture.path.c_str(), "wb");
    if (file == nullptr) {
        projv::core::error("GAME_CAPTURE: cannot open {}", capture.path);
    } else if (capture.textureID >= 0) {
        std::fprintf(file, "%d %d\n", capture.width, capture.height);
        std::vector<float> values(capture.pixels.size());
        for (size_t i = 0; i < values.size(); i++) values[i] = halfToFloat(capture.pixels[i]);
        std::fwrite(values.data(), sizeof(float), values.size(), file);
        std::fclose(file);
        projv::core::info("GAME_CAPTURE: wrote texture {} raw to {}", capture.textureID, capture.path);
    } else {
        std::fprintf(file, "P6\n%d %d\n255\n", capture.width, capture.height);
        std::vector<uint8_t> row(size_t(capture.width) * 3);
        for (int y = 0; y < capture.height; y++) {
            for (int x = 0; x < capture.width; x++) {
                const uint16_t* p = &capture.pixels[(size_t(y) * capture.width + x) * 4];
                float hdr[3] = {halfToFloat(p[0]), halfToFloat(p[1]), halfToFloat(p[2])};
                // A debug view is data, not a picture: display.frag passes it through ungraded.
                if (debugView) {
                    for (int c = 0; c < 3; c++) {
                        row[size_t(x) * 3 + c] = uint8_t(std::clamp(hdr[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                } else {
                    gradePixel(hdr, &row[size_t(x) * 3]);
                }
            }
            std::fwrite(row.data(), 1, row.size(), file);
        }
        std::fclose(file);
        projv::core::info("GAME_CAPTURE: wrote {} ({}x{})", capture.path, capture.width, capture.height);
    }
    bgfx::destroy(capture.readback);
    capture.stage = 2;
    return true;
}

// =============================================================================================
// Frame
// =============================================================================================

void render(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance =
        app.world.ctx().get<projv::graphics::RenderInstance>();
    projv::GPUData& gpuData = app.world.ctx().get<projv::GPUData>();
    FrameState& state = app.world.ctx().get<FrameState>();

    auto now = std::chrono::steady_clock::now();
    double deltaSeconds = std::chrono::duration<double>(now - state.lastFrameTime).count();
    state.lastFrameTime = now;
    // A hitch (a window drag, the first frame) must not throw the camera across the scene.
    float moveDelta = float(std::min(deltaSeconds, 0.1));

    static Benchmark bench = [] {
        Benchmark b;
        if (const char* s = std::getenv("GAME_BENCH")) b.seconds = std::max(std::atof(s), 1.0);
        if (const char* s = std::getenv("GAME_BENCH_SPIN")) b.spin = std::atof(s);
        return b;
    }();
    static Capture capture = [] {
        Capture c;
        if (const char* path = std::getenv("GAME_CAPTURE")) c.path = path;
        if (const char* f = std::getenv("GAME_CAPTURE_FRAME")) c.atFrame = std::max(std::atoi(f), 2);
        if (const char* t = std::getenv("GAME_CAPTURE_TEXTURE")) c.textureID = std::atoi(t);
        return c;
    }();
    static const bool cameraLocked = std::getenv("GAME_LOCK_CAMERA") != nullptr || bench.seconds > 0.0 ||
                                     !capture.path.empty();

    bool cameraMoved = false;
    if (!cameraLocked) {
        cameraMoved = updateMouseLook(renderInstance.window, state);
        cameraMoved = updateMovement(renderInstance.window, state, moveDelta) || cameraMoved;
    } else if (bench.seconds > 0.0 && bench.spin != 0.0) {
        state.yaw += float(bench.spin * std::min(deltaSeconds, 0.1));
        cameraMoved = true;
    }
    handleToggles(renderInstance, state);

    projv::core::vec3 cameraDirection = {std::cos(state.pitch) * std::cos(state.yaw),
                                         std::sin(state.pitch),
                                         std::cos(state.pitch) * std::sin(state.yaw)};
    if (!state.prevCameraInitialized) {
        state.prevCameraPosition = state.cameraPosition;
        state.prevCameraDirection = cameraDirection;
        state.prevCameraInitialized = true;
    }

    projv::core::vec4 sunDirection = sunFromScroll();
    bool sceneChanged = cameraMoved || g_sunScrollAccum != state.prevSunScroll;

    uploadFrameUniforms(renderInstance.getActiveRenderer(), state, cameraDirection, sunDirection,
                        sceneChanged, app.frameCount);
    state.prevSunScroll = g_sunScrollAccum;

    projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(),
                                               &gpuData);

    state.prevCameraPosition = state.cameraPosition;
    state.prevCameraDirection = cameraDirection;

    if (state.gpuProfile) reportGpuPasses(state);

    // Title-bar readout: smoothed frame time and the render/output resolutions it was spent on.
    {
        static double emaMs = 0.0;
        static auto lastTitle = now;
        double frameMs = deltaSeconds * 1000.0;
        emaMs = emaMs > 0.0 ? emaMs * 0.95 + frameMs * 0.05 : frameMs;
        if (std::chrono::duration<double>(now - lastTitle).count() >= 0.25) {
            lastTitle = now;
            int w = 0, h = 0;
            glfwGetWindowSize(renderInstance.window, &w, &h);
            char title[256];
            std::snprintf(title, sizeof(title),
                          "Game Ready Render  |  %.2f ms (%.0f fps)  |  render %dx%d -> %dx%d  |  "
                          "TAA %s  AO %s",
                          emaMs, emaMs > 0.0 ? 1000.0 / emaMs : 0.0,
                          int(std::ceil(float(w) * state.renderScale)),
                          int(std::ceil(float(h) * state.renderScale)), w, h,
                          state.temporalFilter ? "on" : "off", state.ambientOcclusion ? "on" : "off");
            glfwSetWindowTitle(renderInstance.window, title);
        }
    }

    bool benchDone = bench.seconds > 0.0 &&
                     updateBenchmark(bench, deltaSeconds, renderInstance.window, state);
    bool captureDone = updateCapture(capture, renderInstance.getActiveRenderer(), app.frameCount,
                                     state.debugView != 0);
    if (renderInstance.shouldClose || benchDone || captureDone) app.closeRequested = true;
}

void shutdown(projv::Application& app) {
    projv::graphics::destroyGPUData(app.world.ctx().get<projv::GPUData>());
    // The engine starts bgfx but never stops it, and leaving it running to process exit crashed the
    // NVIDIA driver about one exit in three: bgfx's render thread was still presenting while the main
    // thread's teardown freed the driver's memory under it. Stop it here, as the scene editor does.
    bgfx::shutdown();
    glfwTerminate();
}

int main(int argc, char** argv) {
    projv::Application app;
    app.addSystem(projv::Stage::Startup, "startup", startup);
    app.addSystem(projv::Stage::Render, "render", render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);

    if (argc > 1) {
        SceneRequest& request = app.world.ctx().emplace<SceneRequest>();
        request.path = argv[1];
        if (!request.path.empty() && request.path.back() != '/') request.path += '/';
    }
    app.run();
    return 0;
}
