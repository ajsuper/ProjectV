// ProjectV Scene Previewer
// A minimal, fast viewer for any Compose scene folder. It renders the scene's *pure albedos* —
// the colours stored in the material palette — with no lighting of any kind: no shadow ray, no
// ambient occlusion, no global illumination, no sky. One primary ray per pixel, and the voxel's
// colour written straight out.
//
// It is derived from the renderer gallery's `fast` renderer (the cheapest of that example's six)
// with everything except the primary march stripped away. What remains is the answer to "what is
// actually in this scene": a material that reads wrong here is wrong in the data, not in the
// lighting. That makes it the tool to reach for after voxelizing something — MeshVoxelizer's
// output, a Minecraft import, a tree asset — before spending path-tracer frames on it.
//
// Usage:
//   ./scene_previewer [scene-directory]
//
// The scene directory is any folder holding a compose.json (what loadComposeFromDisk opens). With
// no argument it opens one of the scenes bundled in scenes/, so the previewer runs out of the box.
//
// The camera is framed automatically from the scene's bounding box, because a previewer is pointed
// at scenes of wildly different scale — a 64^3 tree and a 2048^3 world should both open looking at
// the subject rather than inside it or in the void.
//
// Controls:
//   W/S          — move forward / backward
//   A/D          — strafe left / right
//   R/F          — move up / down
//   Mouse        — look around (cursor is captured; Esc releases it, left-click re-captures)
//   Scroll wheel — movement speed (the lit renderers use this for the sun; there is no sun here)
//   H            — re-frame the camera on the scene

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <iostream>

#include "core/application.h"
#include "core/math.h"
#include "core/log.h"
#include "graphics/render_instance.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/type_mapping.h"
#include "utils/compose_io.h"

#include <bgfx/bgfx.h>
#include <chrono>
#include <cstdlib>

// Scene path from the command line. The ECS entry points take only the Application, and this is
// fixed for the process's life, so it lives at file scope alongside the GLFW callback state.
static std::string g_scenePath = "./scenes/StonehillCastle/";

// Mouse scroll wheel drives movement speed. GLFW scroll callbacks are plain C function pointers,
// so the accumulated wheel offset lives at file scope.
static double g_speedScrollAccum = 0.0;
static void speedScrollCallback(GLFWwindow* /*window*/, double /*xoffset*/, double yoffset) {
    g_speedScrollAccum += yoffset;
}

// =============================================================================
// Automatic framing
// =============================================================================

// How a scene is framed on open, and the movement scale that goes with it. Both are derived from
// the scene's own size so the controls feel the same whether the subject is a tree or a city.
struct CameraFraming {
    projv::core::vec3 position;
    float yaw;          // Radians; matches the cameraPhi convention below.
    float pitch;
    float moveSpeed;    // World units per frame at the default scroll setting.
    projv::core::vec3 center = {0.0f, 0.0f, 0.0f};  // What the framing looks at; the benchmark orbits it.
    float distance = 100.0f;                        // From position to center.
};

// Measures the world-space bounding box of every live chunk. A chunk header carries its world
// position (minimum corner) and its scale, which is all a bounding box needs — no geometry is
// touched, so this stays instant on a large scene.
static bool measureSceneBounds(const projv::Scene& scene, projv::core::vec3& boundsMin,
                               projv::core::vec3& boundsMax) {
    bool found = false;
    for (const projv::Chunk& chunk : scene.chunks) {
        if (!chunk.alive || chunk.header.scale <= 0.0f) continue;

        projv::core::vec3 chunkMin = chunk.header.position;
        projv::core::vec3 chunkMax = chunk.header.position + projv::core::vec3(chunk.header.scale);
        if (!found) {
            boundsMin = chunkMin;
            boundsMax = chunkMax;
            found = true;
        } else {
            boundsMin = projv::core::min(boundsMin, chunkMin);
            boundsMax = projv::core::max(boundsMax, chunkMax);
        }
    }
    return found;
}

// Places the camera outside the scene's bounding sphere, looking at its centre from a raised
// three-quarter angle — the view that shows the most of an unfamiliar object.
static CameraFraming frameScene(const projv::Scene& scene) {
    using namespace projv::core;

    CameraFraming framing;
    vec3 boundsMin, boundsMax;
    if (!measureSceneBounds(scene, boundsMin, boundsMax)) {
        // Nothing measurable (an empty scene). Any finite camera will do; the previewer will just
        // show the background, which is itself the useful signal.
        projv::core::warn("Scene has no live chunks — the preview will be empty.");
        framing.position = vec3(0.0f, 0.0f, -100.0f);
        framing.yaw = 3.14159f / 2.0f;
        framing.pitch = 0.0f;
        framing.moveSpeed = 1.0f;
        return framing;
    }

    vec3 center = (boundsMin + boundsMax) * 0.5f;
    vec3 extents = boundsMax - boundsMin;
    float radius = length(extents) * 0.5f;
    if (radius <= 0.0f) radius = 1.0f;

    projv::core::info("Scene bounds: ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f})",
        boundsMin.x, boundsMin.y, boundsMin.z, boundsMax.x, boundsMax.y, boundsMax.z);
    projv::core::info("Scene extents: {:.1f} x {:.1f} x {:.1f} (radius {:.1f})",
        extents.x, extents.y, extents.z, radius);

    // Pull back far enough that the bounding sphere fits the 60-degree vertical FOV the albedo
    // pass uses, with a margin so the subject is not jammed against the frame edge.
    const float FOV_RADIANS = 60.0f * 3.14159265f / 180.0f;
    float distance = (radius / std::tan(FOV_RADIANS * 0.5f)) * 1.25f;

    framing.yaw = 3.14159265f * 0.25f;  // Looking along +X/+Z, so the camera sits on the -X/-Z side.
    framing.pitch = -0.35f;             // Slightly above, looking down onto the subject.

    vec3 viewDirection = {
        std::cos(framing.pitch) * std::cos(framing.yaw),
        std::sin(framing.pitch),
        std::cos(framing.pitch) * std::sin(framing.yaw)
    };
    framing.position = center - viewDirection * distance;
    framing.center = center;
    framing.distance = distance;

    // Roughly two seconds to cross the scene at 60fps, which feels the same at any scale.
    framing.moveSpeed = std::max(radius * 2.0f / 120.0f, 0.01f);
    return framing;
}

// =============================================================================
// Benchmark mode
// =============================================================================

// PREVIEW_BENCH=<frames> flies a scripted camera path, moving on EVERY frame -- the case that
// matters, because a moving camera resets the accumulation and gets no help from history -- and
// prints the mean GPU time of each pass over <frames> frames, then exits. Two runs measure the same
// views, so two builds are comparable. PREVIEW_BENCH_PATH picks the path:
//   orbit (default) -- circles the framing's look-at point at the framing distance: the subject
//                      seen from outside, mostly geometry with sky around it.
//   spin            -- stands at the scene's centre and turns: the inside-a-level case, where every
//                      pixel hits something and rays travel through the most structure.
struct BenchState {
    int frames = 0;             // 0 = off.
    std::string path = "orbit";
    int warmup = 30;            // Frames skipped first: shader compiles, uploads, clocks ramping up.
    int measured = 0;
    double passMs[32] = {};
    double frameMs = 0.0;
    double cpuMs = 0.0;
};
static BenchState g_bench;

static void readBenchEnvironment() {
    if (const char* value = std::getenv("PREVIEW_BENCH")) g_bench.frames = std::max(0, std::atoi(value));
    if (const char* value = std::getenv("PREVIEW_BENCH_PATH")) g_bench.path = value;
}

// Accumulates this frame's timings; returns true once the run is complete and the app should exit.
static bool recordBenchFrame(int frameIndex, int width, int height) {
    if (frameIndex < g_bench.warmup) return false;
    static auto lastFrame = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    if (frameIndex > g_bench.warmup) g_bench.cpuMs += std::chrono::duration<double, std::milli>(now - lastFrame).count();
    lastFrame = now;

    const bgfx::Stats* stats = bgfx::getStats();
    if (stats != nullptr && stats->gpuTimerFreq > 0) {
        const double toMs = 1000.0 / double(stats->gpuTimerFreq);
        for (uint16_t v = 0; v < stats->numViews; v++) {
            const bgfx::ViewStats& viewStats = stats->viewStats[v];
            if (viewStats.view < 32) g_bench.passMs[viewStats.view] += double(viewStats.gpuTimeEnd - viewStats.gpuTimeBegin) * toMs;
        }
        g_bench.frameMs += double(stats->gpuTimeEnd - stats->gpuTimeBegin) * toMs;
    }
    if (++g_bench.measured < g_bench.frames) return false;

    const double inv = 1.0 / double(g_bench.measured);
    projv::core::info("BENCH path={} frames={} resolution={}x{}", g_bench.path, g_bench.measured, width, height);
    for (int i = 0; i < 32; i++) {
        if (g_bench.passMs[i] > 0.0) projv::core::info("BENCH   pass {:2d}  {:7.3f} ms", i, g_bench.passMs[i] * inv);
    }
    projv::core::info("BENCH   gpu frame {:7.3f} ms", g_bench.frameMs * inv);
    projv::core::info("BENCH   wall frame {:7.3f} ms", g_bench.cpuMs / double(g_bench.measured - 1));
    return true;
}

// =============================================================================
// Application stages
// =============================================================================

// Startup: create the window, load the scene, build the preview renderer, and upload the scene
// to the GPU.
void startup(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance =
        app.world.ctx().emplace<projv::graphics::RenderInstance>();
    renderInstance.initialize(1920, 1080, "ProjectV Scene Previewer");

    // Capture the cursor so mouse motion drives the camera (FPS-style mouse look).
    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetScrollCallback(renderInstance.window, speedScrollCallback);

    projv::Scene& scene         = app.world.ctx().emplace<projv::Scene>();
    projv::GPUData& gpuData     = app.world.ctx().emplace<projv::GPUData>();
    CameraFraming& framing      = app.world.ctx().emplace<CameraFraming>();

    projv::core::info("Loading scene: {}", g_scenePath);
    scene = projv::utils::loadComposeFromDisk(g_scenePath);
    projv::core::info("Loaded {} chunk(s), {} component(s).", scene.chunks.size(), scene.components.size());

    framing = frameScene(scene);
    projv::core::info("Framed camera at ({:.1f}, {:.1f}, {:.1f}), move speed {:.2f}/frame",
        framing.position.x, framing.position.y, framing.position.z, framing.moveSpeed);

    projv::RendererSpecification rendererSpec =
        projv::graphics::loadRendererSpecification("./previewRenderer/");
    renderInstance.addRendererSpecification(1, rendererSpec);

    bgfx::ShaderHandle vsh =
        projv::graphics::loadShader("./previewRenderer/previewShaders/vs_quad.bin");
    std::shared_ptr<projv::ConstructedRenderer> constructedRenderer =
        projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vsh);

    // No resource textures to upload: the previewer has no blue-noise LUT because it has nothing
    // stochastic to decorrelate beyond the Halton sub-pixel jitter, which is analytic.

    renderInstance.setActiveRenderer(constructedRenderer);
    // Per-view GPU timestamps are only collected with the profiler on.
    if (g_bench.frames > 0) bgfx::setDebug(BGFX_DEBUG_PROFILER);
    gpuData = projv::graphics::createTexturesForScene(scene);
}

// Update: frame timing profiler.
void update(projv::Application& app) {
#if defined(PROJV_ENABLE_PERF)
    (void)app;
    static auto lastFrameTime = std::chrono::high_resolution_clock::now();
    auto currentFrameTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> frameDuration = currentFrameTime - lastFrameTime;
    lastFrameTime = currentFrameTime;

    static int frameCount = 0;
    static double frameTimes[100];
    frameTimes[frameCount % 100] = frameDuration.count() * 1000.0;
    if (++frameCount % 100 == 0) {
        double sum = 0, minimum = 1e9, maximum = 0;
        for (int i = 0; i < 100; i++) {
            sum += frameTimes[i];
            minimum = std::min(minimum, frameTimes[i]);
            maximum = std::max(maximum, frameTimes[i]);
        }
        projv::core::perf("Frame stats (last 100): avg={:.2f}ms min={:.2f}ms max={:.2f}ms",
                          sum / 100.0, minimum, maximum);
    }
#else
    (void)app;
#endif
}

// Render: handle camera input, upload the per-frame uniforms, dispatch the three passes.
void render(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance =
        app.world.ctx().get<projv::graphics::RenderInstance>();
    projv::GPUData& gpuData = app.world.ctx().get<projv::GPUData>();
    CameraFraming& framing  = app.world.ctx().get<CameraFraming>();

    // Camera state, seeded from the automatic framing on the first frame.
    static bool cameraInitialized = false;
    static projv::core::vec3 cameraPosition;
    static float cameraPhi = 0.0f;    // Yaw.
    static float cameraPitch = 0.0f;
    if (!cameraInitialized) {
        cameraPosition = framing.position;
        cameraPhi = framing.yaw;
        cameraPitch = framing.pitch;
        cameraInitialized = true;
    }

    bool cameraMoved = false;

    // Cursor capture toggle: Esc releases the cursor (so it can leave the window), left-click
    // re-captures it. Only look around while captured.
    static bool mouseCaptured = true;
    if (mouseCaptured && glfwGetKey(renderInstance.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        mouseCaptured = false;
    } else if (!mouseCaptured && glfwGetMouseButton(renderInstance.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        mouseCaptured = true;
    }

    // Mouse look. The cursor is captured, so its absolute position is read each frame and the
    // delta applied to yaw/pitch.
    static double lastMouseX = 0.0, lastMouseY = 0.0;
    static bool mouseInitialized = false;
    double mouseX, mouseY;
    glfwGetCursorPos(renderInstance.window, &mouseX, &mouseY);
    // Reset the reference point whenever tracking (re)starts, so re-capturing after a release
    // does not apply one huge jump.
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
        const float pitchLimit = 1.55f; // ~89 degrees, just shy of gimbal flip.
        if (cameraPitch >  pitchLimit) cameraPitch =  pitchLimit;
        if (cameraPitch < -pitchLimit) cameraPitch = -pitchLimit;
        cameraMoved = true;
    }

    if (g_bench.frames > 0) {
        // One full turn over the measured frames, so the run covers every side of the subject.
        float turn = 6.2831853f * float(app.frameCount) / float(g_bench.frames + g_bench.warmup);
        if (g_bench.path == "spin") {
            cameraPosition = framing.center;
            cameraPhi = framing.yaw + turn;
            cameraPitch = -0.15f;
        } else {
            cameraPhi = framing.yaw + turn;
            cameraPitch = framing.pitch;
            projv::core::vec3 lookDirection = {
                std::cos(cameraPitch) * std::cos(cameraPhi), std::sin(cameraPitch),
                std::cos(cameraPitch) * std::sin(cameraPhi) };
            cameraPosition = framing.center - lookDirection * framing.distance;
        }
        cameraMoved = true;
    }

    projv::core::vec3 cameraDirection;
    cameraDirection.x = projv::core::cos(cameraPitch) * projv::core::cos(cameraPhi);
    cameraDirection.y = projv::core::sin(cameraPitch);
    cameraDirection.z = projv::core::cos(cameraPitch) * projv::core::sin(cameraPhi);

    // Horizontal forward from yaw only, so W/S flies level regardless of pitch.
    projv::core::vec3 forwardDirection = { projv::core::cos(cameraPhi), 0, projv::core::sin(cameraPhi) };

    // Scroll wheel scales the framing-derived speed geometrically, so one notch is a consistent
    // proportional change whether the scene is 64 or 8192 voxels across.
    float moveSpeed = framing.moveSpeed * std::pow(1.2f, (float)g_speedScrollAccum);

    if (glfwGetKey(renderInstance.window, GLFW_KEY_W)) { cameraPosition += forwardDirection * moveSpeed; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_S)) { cameraPosition -= forwardDirection * moveSpeed; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_A)) {
        float leftPhi = cameraPhi + 3.14159265f / 2;
        projv::core::vec3 leftDirection = { projv::core::cos(leftPhi), 0, projv::core::sin(leftPhi) };
        cameraPosition -= leftDirection * moveSpeed;
        cameraMoved = true;
    }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_D)) {
        float rightPhi = cameraPhi - 3.14159265f / 2;
        projv::core::vec3 rightDirection = { projv::core::cos(rightPhi), 0, projv::core::sin(rightPhi) };
        cameraPosition -= rightDirection * moveSpeed;
        cameraMoved = true;
    }

    if (glfwGetKey(renderInstance.window, GLFW_KEY_R)) { cameraPosition[1] += moveSpeed; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_F)) { cameraPosition[1] -= moveSpeed; cameraMoved = true; }

    // H re-frames on the scene, for when navigation has left the subject behind.
    if (glfwGetKey(renderInstance.window, GLFW_KEY_H)) {
        cameraPosition = framing.position;
        cameraPhi = framing.yaw;
        cameraPitch = framing.pitch;
        cameraMoved = true;
    }

    // The accumulate pass resets its running mean on this, so a stale value would freeze the
    // image on the previous view.
    static int frameCameraLastMovedOn = 0;
    if (cameraMoved) frameCameraLastMovedOn = app.frameCount;

    projv::core::vec2 windowResolution = renderInstance.getWindowResolution();
    projv::core::vec4 frameCount = {
        (float)app.frameCount, (float)cameraMoved, (float)frameCameraLastMovedOn, 0.0f
    };
    projv::core::vec2 texelSize = { 1.0f / windowResolution.x, 1.0f / windowResolution.y };

    std::shared_ptr<projv::ConstructedRenderer> renderer = renderInstance.getActiveRenderer();
    projv::graphics::setUniformToValue(renderer, "cameraPos",  cameraPosition);
    projv::graphics::setUniformToValue(renderer, "cameraDir",  cameraDirection);
    projv::graphics::setUniformToValue(renderer, "windowRes",  windowResolution);
    projv::graphics::setUniformToValue(renderer, "frameCount", frameCount);
    projv::graphics::setUniformToValue(renderer, "texelSize",  texelSize);

    projv::graphics::renderConstructedRenderer(renderInstance, renderer, &gpuData);

    if (g_bench.frames > 0 &&
        recordBenchFrame(app.frameCount, int(windowResolution.x), int(windowResolution.y))) {
        app.closeRequested = true;
    }
}

int main(int argc, char** argv) {
    if (argc > 1) {
        g_scenePath = argv[1];
        // loadComposeFromDisk expects a directory; a missing trailing separator is the obvious
        // way to get this wrong from a shell that does not tab-complete one.
        if (!g_scenePath.empty() && g_scenePath.back() != '/') g_scenePath += '/';
    }

    readBenchEnvironment();

    projv::Application app;
    app.addSystem(projv::Stage::Startup, "startup", startup);
    app.addSystem(projv::Stage::Update, "update", update);
    app.addSystem(projv::Stage::Render, "render", render);
    app.run();
    return 0;
}
