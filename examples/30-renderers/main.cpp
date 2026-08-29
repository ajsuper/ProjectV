// ProjectV Renderer Gallery
//
// One host application, seven renderers, the same scene and the same camera. The point is the
// comparison: each renderer answers "how should this scene be lit" differently, and the only way
// to judge that is to put them side by side under identical conditions.
//
// A renderer here is *data*, not code -- a folder holding render.json (the pass order),
// resources.json (the shaders, render targets and framebuffers) and the shaders themselves. This
// file supplies the window, the camera, the scene and the per-frame uniforms; everything that
// makes one renderer differ from another lives in its folder. Adding an eighth is dropping in a
// directory and adding one entry to buildRendererRegistry() below.
//
// Each renderer has its own README in renderers/<name>/, and renderers/README.md compares them.
//
//   tree64              accumulation path tracer; resets whenever the camera moves
//   reprojection        path traced, temporally reprojected, so it stays converged while moving
//   fast                direct sun + screen-space AO, anti-aliased by TAA. Cheapest.
//   face                per-face flat shading with full GI, accumulated by exact face identity
//   radiance-cascade    screen-space radiance cascades
//   world-cascade       world-space radiance cascades: the gather traces real voxel rays, so
//                       occlusion and sky visibility are correct off-screen too
//   world-face-cascade  world-space cascades anchored to voxel face centres, accumulated per face
//
// Usage:
//   ./renderer_gallery [--renderer <name>] [--scene <directory>] [--list]
//
//   --renderer   which renderer to run (default: tree64)
//   --scene      a folder holding a compose.json (default: SanMiguel, from the scene previewer)
//   --list       print the available renderers and exit
//
// Selection is a flag rather than a prompt so the gallery can be scripted: a benchmark or a
// screenshot matrix across all seven is a shell loop.
//
// Controls:
//   W/S          — move forward / backward
//   A/D          — strafe left / right
//   R/F          — move up / down
//   Mouse        — look around (cursor is captured; Esc releases it, left-click re-captures)
//   Scroll wheel — sun elevation

#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "core/ecs.h"
#include "core/math.h"
#include "core/log.h"
#include "core/paths.h"
#include "graphics/render_instance.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/type_mapping.h"
#include "utils/compose_io.h"
#include "utils/picking.h"

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
    std::string id;               // What --renderer takes; matches the folder name.
    std::string description;      // One line, shown by --list.
    std::string directory;        // Folder holding render.json / resources.json.
    std::string vertexShaderPath; // Compiled vertex shader for its passes.
    // Uploads this renderer's per-frame uniforms. Must set every uniform the
    // renderer's resources.json declares (and may ignore the rest).
    std::function<void(std::shared_ptr<projv::ConstructedRenderer>, const FrameContext&)> uploadUniforms;
};

// Bypass the reconstruction and point-sample the render-resolution frame instead (Q). Only the
// `reconstruction` renderer reads it; the others declare no such uniform. Flipping between the two
// on the same view is the only honest way to judge an upscaler, so the demo carries the A/B.
static bool g_reconstructBypass = false;

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
// The reconstruction demo. Its G-buffer and compose run at half resolution and the reconstruction
// magnifies to the output grid, so it needs the two uniforms that pass reads: debugParams.x carries
// the bypass, renderParams.x a debug view it does not use here.
static void uploadReconstructionUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    uploadCommonUniforms(renderer, ctx);
    projv::core::vec4 debugParams  = { g_reconstructBypass ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
    projv::core::vec4 renderParams = { 0.0f, 0.0f, 0.0f, 0.0f };
    projv::graphics::setUniformToValue(renderer, "debugParams",  debugParams);
    projv::graphics::setUniformToValue(renderer, "renderParams", renderParams);
}

static void uploadReprojectionUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    uploadCommonUniforms(renderer, ctx);
    projv::core::vec3 prevCameraPosition = ctx.prevCameraPosition;
    projv::core::vec3 prevCameraDirection = ctx.prevCameraDirection;
    projv::graphics::setUniformToValue(renderer, "prevCameraPos", prevCameraPosition);
    projv::graphics::setUniformToValue(renderer, "prevCameraDir", prevCameraDirection);
}

// The registry. One entry per folder under renderers/; adding an eighth renderer means adding a
// directory and one push_back here.
//
// The `id` must match the directory name: --renderer takes it, and it is what the per-renderer
// README is filed under.
static std::vector<RendererModule> buildRendererRegistry() {
    std::vector<RendererModule> registry;

    registry.push_back({
        "tree64",
        "Accumulation path tracer. Converges while the camera is still, resets when it moves.",
        "./renderers/tree64/",
        "./renderers/tree64/shaders/vs_quad.bin",
        uploadTree64Uniforms
    });
    // The temporal filtering demo. It happens to be built on a path tracer, but the path trace is
    // the *input* here rather than the subject: what this shows is how to reproject last frame's
    // result through this frame's camera, validate the match, and blend -- the machinery every
    // temporal upscaler and antialiaser is made of. AdvancedRenderer used to carry a TAA pass; this
    // is where that technique is taught now.
    registry.push_back({
        "taa",
        "Temporal reprojection and accumulation -- the TAA technique, shown on a path traced input.",
        "./renderers/taa/",
        "./renderers/taa/shaders/vs_quad.bin",
        uploadReprojectionUniforms
    });
    registry.push_back({
        "fast",
        "Direct sun plus screen-space AO, anti-aliased by TAA. The cheapest renderer here.",
        "./renderers/fast/",
        "./renderers/fast/shaders/vs_quad.bin",
        uploadReprojectionUniforms
    });

    // Rendering below native resolution and rebuilding the missing pixels from voxel FACE identity
    // rather than blurring between samples. The single most effective thing here for a weak GPU:
    // half resolution is a quarter of the pixels traced, and the reconstruction gives most of the
    // detail back because a voxel face is a flat quad whose whole extent one sample describes.
    //
    // Press Q to bypass it and point-sample instead -- that A/B is the demo.
    registry.push_back({
        "reconstruction",
        "Traces at half resolution and reconstructs to full from voxel face identity. Q toggles it.",
        "./renderers/reconstruction/",
        "./renderers/reconstruction/shaders/vs_quad.bin",
        uploadReconstructionUniforms
    });

    return registry;
}

// Command line. Selection is a flag rather than a prompt so the gallery can be driven by a
// script -- a benchmark across all seven renderers should not need someone at the keyboard.
struct Options {
    std::string rendererId = "tree64";
    std::string scenePath;                // Empty: use the bundled scene beside the binary.
    bool        listAndExit = false;
};

static void printRenderers(const std::vector<RendererModule>& registry) {
    std::cout << "Available renderers:\n";
    for (const RendererModule& renderer : registry) {
        std::cout << "  " << renderer.id << "\n      " << renderer.description << "\n";
    }
}

// Returns false if the program should stop (bad usage, or --list).
static bool parseCommandLine(int argc, char** argv, const std::vector<RendererModule>& registry,
                             Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "--list") {
            options.listAndExit = true;
            return true;
        }
        if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: renderer_gallery [--renderer <name>] [--scene <directory>] [--list]\n\n";
            printRenderers(registry);
            options.listAndExit = true;
            return true;
        }
        if (argument == "--renderer" || argument == "--scene") {
            if (i + 1 >= argc) {
                std::cerr << argument << " needs a value.\n";
                return false;
            }
            (argument == "--renderer" ? options.rendererId : options.scenePath) = argv[++i];
            continue;
        }
        std::cerr << "Unrecognised argument: " << argument << "\n"
                  << "Usage: renderer_gallery [--renderer <name>] [--scene <directory>] [--list]\n";
        return false;
    }
    return true;
}

// Index of the named renderer, or -1. Names a wrong id loudly rather than silently falling back:
// a benchmark that quietly measured the default renderer seven times would be worse than an error.
static int findRenderer(const std::vector<RendererModule>& registry, const std::string& id) {
    for (size_t i = 0; i < registry.size(); ++i) {
        if (registry[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

// =============================================================================
// Automatic framing
// =============================================================================
//
// The gallery takes --scene, so it gets pointed at scenes of wildly different scale. A camera
// tuned for one of them is wrong for every other, which is why this is derived from the scene's
// own bounding box rather than hardcoded. Ported from 10-scene-previewer, which does the same.

struct CameraFraming {
    projv::core::vec3 position;
    float yaw;
    float pitch;
    float moveSpeed;   // World units per frame.
};

// Bounding box of every live chunk. A chunk header carries its world position and scale, which is
// all a bounding box needs -- no geometry is touched, so this stays instant on a large scene.
static bool measureSceneBounds(const projv::Scene& scene, projv::core::vec3& boundsMin,
                               projv::core::vec3& boundsMax) {
    bool found = false;
    for (const projv::Chunk& chunk : scene.chunks) {
        if (!chunk.alive || chunk.header.scale <= 0.0f) continue;
        projv::core::vec3 chunkMin = chunk.header.position;
        projv::core::vec3 chunkMax = chunk.header.position + projv::core::vec3(chunk.header.scale);
        if (!found) { boundsMin = chunkMin; boundsMax = chunkMax; found = true; }
        else {
            boundsMin = projv::core::min(boundsMin, chunkMin);
            boundsMax = projv::core::max(boundsMax, chunkMax);
        }
    }
    return found;
}

static CameraFraming frameScene(const projv::Scene& scene) {
    using namespace projv::core;
    CameraFraming framing;
    vec3 boundsMin, boundsMax;
    if (!measureSceneBounds(scene, boundsMin, boundsMax)) {
        projv::core::warn("Scene has no live chunks -- the image will be empty.");
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

    projv::core::info("Scene extents: {:.1f} x {:.1f} x {:.1f} (radius {:.1f})",
                      extents.x, extents.y, extents.z, radius);

    // Back far enough that the bounding sphere fits a 60-degree vertical FOV, with a margin so the
    // subject is not jammed against the frame edge.
    float voxelScale = 1.0f;
    for (const projv::Chunk& chunk : scene.chunks) {
        if (chunk.alive && chunk.header.voxelScale > 0.0f) { voxelScale = chunk.header.voxelScale; break; }
    }

    const float FOV_RADIANS = 60.0f * 3.14159265f / 180.0f;
    float distance = (radius / std::tan(FOV_RADIANS * 0.5f)) * 1.25f;

    // ...but not further than the renderers can actually see. These are walkthrough path tracers:
    // their primary rays carry a step budget (tree64 uses maxRaySteps = 200) and drop to a coarse
    // LOD only after distanceToFinishLOD, so a ray crossing a long stretch of empty space runs out
    // of steps before it reaches anything. Framing a 4096-wide scene from outside its bounding
    // sphere therefore renders *black* -- and renders it fast, because every ray terminates on the
    // step limit immediately.
    //
    // The reach estimate is deliberately conservative: roughly the step budget times the voxel
    // size, doubled for the coarser LOD the march falls back to. Clamping to it puts the camera
    // inside a large scene (which is where a walkthrough renderer belongs) while leaving a small
    // asset framed from outside, where the fit distance is already within reach.
    const float reach = 150.0f * voxelScale * 2.0f;
    if (distance > reach) {
        projv::core::info("Framing distance {:.0f} exceeds the renderers' ray reach (~{:.0f}); "
                          "moving the camera inside the scene.", distance, reach);
        distance = reach;
    }

    framing.yaw   = 3.14159265f * 0.25f;
    framing.pitch = -0.15f;   // Nearly level: a walkthrough camera, not an overhead view.
    vec3 viewDirection = {
        std::cos(framing.pitch) * std::cos(framing.yaw),
        std::sin(framing.pitch),
        std::cos(framing.pitch) * std::sin(framing.yaw)
    };
    framing.position = center - viewDirection * distance;

    // Being at the right distance is not the same as being in open space -- clamping into a large
    // scene lands the camera inside a wall about as often as not. Drop a ray from above the scene
    // onto whatever is below the chosen spot and stand on it, using the engine's own CPU picker.
    // This is also the cheapest demonstration of utils::pickVoxel in the tree.
    // A person's height in voxels, not a fraction of the scene. Scaling this with the scene put the
    // camera 46 units up in SanMiguel, which sounds harmless until you count steps: tree64's primary
    // ray is 100 steps at LOD 0, so a floor 46 units below is only reachable by rays steeper than
    // about 30 degrees. Everything shallower missed, and tree64 writes a miss as black -- the whole
    // frame went dark while the other renderers, which coarsen to LOD 1-2 and reach much further,
    // looked fine.
    const float eyeHeight = 8.0f * voxelScale;
    projv::utils::VoxelPick ground = projv::utils::pickVoxel(
        scene,
        vec3(framing.position.x, boundsMax.y + radius * 0.1f, framing.position.z),
        vec3(0.0f, -1.0f, 0.0f),
        (boundsMax.y - boundsMin.y) + radius * 0.25f);

    if (ground.hit) {
        framing.position.y = ground.worldPosition.y + eyeHeight;
        projv::core::info("Stood the camera on the surface at y={:.1f} (+{:.1f} eye height).",
                          ground.worldPosition.y, eyeHeight);
    } else {
        // Nothing underneath -- the spot is over a hole or outside the geometry. Mid-height is the
        // least-bad guess and at least is not inside the floor.
        framing.position.y = center.y;
    }
    // Roughly two seconds to cross the scene at 60fps, so the controls feel the same at any scale.
    framing.moveSpeed = std::max(radius * 2.0f / 120.0f, 0.01f);

    projv::core::info("Framed camera at ({:.1f}, {:.1f}, {:.1f}), move speed {:.2f}/frame",
                      framing.position.x, framing.position.y, framing.position.z, framing.moveSpeed);
    return framing;
}

// =============================================================================
// Application stages
// =============================================================================

// Chosen by main() before the loop starts; the ECS stage signature takes only the Application.
static RendererModule g_selectedRenderer;
static std::string    g_scenePath;

// Startup: create the window, load the scene and the chosen renderer, upload all GPU resources.
void startup(projv::Application& app) {

    projv::graphics::RenderInstance& renderInstance = projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(1920, 1080, ("ProjectV — " + g_selectedRenderer.id).c_str());

    // Capture the cursor so mouse motion drives the camera (FPS-style mouse look).
    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // Scroll wheel controls the sun's elevation (day cycle).
    glfwSetScrollCallback(renderInstance.window, sunScrollCallback);

    projv::Scene& scene     = projv::core::createGlobalResource<projv::Scene>(app.world);
    float& cameraPhi         = projv::core::createGlobalResource<float>(app.world);
    projv::GPUData& gpuData  = projv::core::createGlobalResource<projv::GPUData>(app.world);
    RendererModule& selectedRenderer = projv::core::createGlobalResource<RendererModule>(app.world);
    CameraFraming& framing = projv::core::createGlobalResource<CameraFraming>(app.world);
    selectedRenderer = g_selectedRenderer;

    // Eager load: loads all scene geometry up front.
    projv::core::info("Loading scene: {}", g_scenePath);
    scene = projv::utils::loadComposeFromDisk(g_scenePath);
    projv::core::info("Renderer: {}", selectedRenderer.id);

    // A scene can parse and still hold no geometry -- most often because its .data containers are
    // a version the loader rejects, which it reports above. Say so here rather than presenting an
    // empty frame as a working render.
    if (scene.chunks.empty()) {
        projv::core::warn("Scene contains no chunks; the image will be empty. Check the log above "
                          "for a rejected .data version.");
    }

    // The camera used to be hardcoded to a position tuned for one scene, which meant --scene
    // pointed anywhere else opened inside the geometry or in the void.
    framing = frameScene(scene);
    cameraPhi = framing.yaw;

    projv::RendererSpecification rendererSpec = projv::graphics::loadRendererSpecification(selectedRenderer.directory);
    renderInstance.addRendererSpecification(1, rendererSpec);

    bgfx::ShaderHandle vsh = projv::graphics::loadShader(selectedRenderer.vertexShaderPath);
    std::shared_ptr<projv::ConstructedRenderer> constructedRenderer = projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vsh);

    // Load the blue-noise LUT used by the path tracers for sample decorrelation.
    int width, height, channels;
    unsigned char* img = stbi_load("LDR_RGBA_7.png", &width, &height, &channels, 4);
    if (img) {
        projv::core::info("Blue-noise texture: {}x{}", width, height);
        projv::graphics::setTextureToData(constructedRenderer, 1, img, width, height);
    } else {
        projv::core::warn("Could not load LDR_RGBA_7.png; sampling will be correlated and noisier.");
    }

    renderInstance.setActiveRenderer(constructedRenderer);
    gpuData = projv::graphics::createTexturesForScene(scene);
}

// Update: frame timing profiler.
void update(projv::Application& app) {
    // The engine records a window-manager close request on the RenderInstance; acting on it is the
    // application's decision. Ending the loop here runs the Shutdown stage on the way out.
    projv::graphics::RenderInstance& renderInstance =
        projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    if (renderInstance.shouldClose) {
        app.closeAppFlag = true;
    }

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
    CameraFraming& framing           = projv::core::getGlobalResource<CameraFraming>(app.world);

    // Seeded from the automatic framing on the first frame, so --scene works at any scale.
    static projv::core::vec3 cameraPosition;
    static bool cameraInitialized = false;

    // Previous-frame camera, used by renderers that do temporal reprojection.
    static projv::core::vec3 prevCameraPosition  = cameraPosition;
    static projv::core::vec3 prevCameraDirection = projv::core::vec3(0.0, 0.0, 1.0);
    static bool prevCameraInitialized = false;

    bool cameraMoved = false;

    // Pitch (look up/down). Yaw is stored in the persistent cameraPhi resource.
    static float cameraPitch = 0.0f;

    if (!cameraInitialized) {
        cameraPosition = framing.position;
        cameraPitch    = framing.pitch;
        cameraInitialized = true;
    }

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
    // Derived from the scene's size (see frameScene), so flying feels the same whether the scene
    // is a 64^3 asset or a 4096-wide street. Scroll used to be the sun only; the sun still has it.
    const float moveSpeed = framing.moveSpeed;

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

    // Q toggles the reconstruction bypass (reconstruction renderer only). Edge-triggered: holding a
    // key down would flip it every frame.
    {
        static bool prevQ = false;
        bool q = glfwGetKey(renderInstance.window, GLFW_KEY_Q) == GLFW_PRESS;
        if (q && !prevQ) {
            g_reconstructBypass = !g_reconstructBypass;
            projv::core::info("Reconstruction: {}",
                              g_reconstructBypass ? "BYPASSED (point sampled)" : "on");
        }
        prevQ = q;
    }

    // H re-frames on the scene, for when navigation has left the subject behind.
    if (glfwGetKey(renderInstance.window, GLFW_KEY_H)) {
        cameraPosition = framing.position;
        cameraPhi      = framing.yaw;
        cameraPitch    = framing.pitch;
        cameraMoved    = true;
    }


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

void shutdown(projv::Application& app) {
    projv::GPUData& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    projv::graphics::destroyGPUData(gpuData);
}

int main(int argc, char** argv) {
    const std::vector<RendererModule> registry = buildRendererRegistry();

    Options options;
    if (!parseCommandLine(argc, argv, registry, options)) {
        return 2;
    }
    if (options.listAndExit) {
        printRenderers(registry);
        return 0;
    }

    const int selected = findRenderer(registry, options.rendererId);
    if (selected < 0) {
        std::cerr << "No renderer named \"" << options.rendererId << "\".\n\n";
        printRenderers(registry);
        return 2;
    }
    g_selectedRenderer = registry[selected];

    // The renderer folders and the bundled scene are staged beside the binary by the build, and
    // resources.json names its shaders relative to the working directory -- so run from here.
    // Doing the chdir once, at the top, is what lets the example be launched from anywhere.
    std::error_code chdirError;
    std::filesystem::current_path(projv::core::executableDirectory(), chdirError);
    if (chdirError) {
        std::cerr << "Could not enter the executable's directory: "
                  << chdirError.message() << "\n";
        return 2;
    }

    // The bundled Sponza is a .data version the loader rejects, so it opens as zero chunks and
    // renders black. Default to a scene that actually loads: SanMiguel, staged by the scene
    // previewer next door. Examples sit side by side under build/examples, so the sibling path
    // resolves once this binary has entered its own directory (above).
    g_scenePath = options.scenePath.empty()
        ? "../scene_previewer/scenes/SanMiguel/"
        : options.scenePath;
    if (!g_scenePath.empty() && g_scenePath.back() != '/') g_scenePath += '/';

    projv::Application app = projv::core::createApp();
    projv::core::assignSystemStage(app, projv::SystemStage::Startup,  startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update,   update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render,   render);
    projv::core::assignSystemStage(app, projv::SystemStage::Shutdown, shutdown);
    projv::core::runApplication(app);
    return 0;
}
