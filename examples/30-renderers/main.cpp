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
//   restir              ReSTIR GI at a sixteenth of the pixels, denoised, then a temporal resolve
//   fast                direct sun + screen-space AO, anti-aliased by TAA. Cheapest.
//   face                per-face flat shading with full GI, accumulated by exact face identity
//   radiance-cascade    screen-space radiance cascades
//   world-cascade       world-space radiance cascades: the gather traces real voxel rays, so
//                       occlusion and sky visibility are correct off-screen too
//   world-face-cascade  world-space cascades anchored to voxel face centres, accumulated per face
//   voxel               the examples' game renderer (examples/shared/voxelRenderer): what the physics
//                       demos, the explosion and the multiplayer games draw with
//
// Usage:
//   ./renderer_gallery [--renderer <name>] [--scene <directory>] [--list]
//
//   --renderer   which renderer to run (default: tree64)
//   --scene      a folder holding a compose.json (default: SanMiguel, from the scene previewer)
//   --list       print the available renderers and exit
//
// Measuring (unattended; the window opens and closes by itself):
//   --bench            each pass's GPU time over frames 120..360, printed, then exit
//   --capture <prefix> saves frames 300, 301 and 302 as <prefix>-<frame>.tga, then exits
//   --look x,y,z,tx,ty,tz   stands the camera at x,y,z looking at tx,ty,tz (default: the framing)
//   --pan <rad>        turns the camera this much a frame (a moving-camera measurement)
//   --voxel-folder <f> which staged folder the voxel renderer loads (default: voxelRenderer)
//   --restir-gi <n>    restir traces GI for one pixel in n x n: 4 (a sixteenth, default) or 2 (a quarter)
//
// Selection is a flag rather than a prompt so the gallery can be scripted: a benchmark or a
// screenshot matrix across all seven is a shell loop.
//
// Controls:
//   W/S          — move forward / backward (Shift: four times as fast)
//   A/D          — strafe left / right
//   R/F          — move up / down
//   Mouse        — look around (cursor is captured; Esc releases it, left-click re-captures)
//   Scroll wheel — sun elevation
//   T            — TAA on / off (voxel)
//   Q            — reconstruction bypassed: point-sample the render resolution (reconstruction, restir)
//   G            — the image / indirect light only / direct light only / ambient occlusion (restir)
//   J            — ReSTIR reuse on / off (restir)
//   P            — post-processing on / off (restir); V vignette, B colour grade, E auto-exposure, K depth of field
//   O / U        — distance fog / soft sun shadow on / off (restir)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "core/application.h"
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
#include "voxel_renderer.h"

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
    float             sunAngle;       // the same, as its angle up the arc
    projv::World*     world;
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
    // Optional: loads the renderer itself, in place of the generic load of `directory`.
    std::function<void(projv::Application&, projv::graphics::RenderInstance&, const projv::Scene&)> install;
};

// The voxel renderer's staged folder (--voxel-folder), and its TAA (T).
static std::string g_voxelFolder = "voxelRenderer";
static bool g_voxelTAA = true;

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

// The restir renderer's switches: G cycles what it shows (the image, the indirect light alone, the
// direct alone, the ambient occlusion), J turns ReSTIR's reuse off -- each GI pixel's one fresh sample,
// denoised -- for the A/B, and Q bypasses the reconstruction, as for the reconstruction renderer.
// The post (display.frag): P turns it all off and on, V the vignette, B the grade, E the auto-exposure.
static int  g_restirView = 0;
static bool g_restirReuse = true;
// K the depth of field (part of the post). Two more are the scene's, not the post's: O the distance fog,
// U the sun's soft shadow.
struct RestirPost { bool on = true, vignette = true, grade = true, exposure = true, dof = true; };
static RestirPost g_restirPost;
static bool g_restirFog = true;
static bool g_restirSoftShadow = true;
static constexpr float RESTIR_VIGNETTE = 0.15f;       // the corners' falloff: subtle
static constexpr float RESTIR_DOF_PIXELS = 1.5f;      // the far distance's blur radius at 1080 lines: very slight
static constexpr float RESTIR_SUN_HALF_ANGLE = 0.02f; // radians, ~4x the real sun's: penumbrae a little soft
static float g_sceneRadius = 100.0f;                  // frameScene's, for the fog's length
static int  g_restirStride = 4;   // --restir-gi: GI for one window pixel in stride x stride (4 or 2)
static constexpr float RESTIR_RENDER_SCALE = 0.5f;   // its G-buffer, each way; resources.json's 0.5 targets

// ReSTIR GI at a sixteenth (or a quarter) of the window's pixels, a G-buffer at half resolution each way
// rebuilt to the window by the voxel renderer's reconstruction, then a temporal resolve.
static void uploadRestirUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    using projv::core::vec4;
    using projv::graphics::setUniformToValue;
    // Surfaces are offset a twentieth of the finest voxel before a ray leaves them; the same-surface
    // tests are scaled from it too.
    static float finest = 0.0f;
    if (finest <= 0.0f) {
        for (const projv::Chunk& chunk : ctx.world->ctx().get<projv::Scene>().chunks)
            if (chunk.alive && chunk.header.voxelScale > 0.0f)
                finest = finest > 0.0f ? std::min(finest, chunk.header.voxelScale) : chunk.header.voxelScale;
        if (finest <= 0.0f) finest = 1.0f;
    }
    // Which render texel of its block each GI pixel samples this frame (the block: the GI stride against
    // the render resolution, 2 or 1): every one in turn, diagonals first.
    const int giStride = std::max(1, int(float(g_restirStride) * RESTIR_RENDER_SCALE));
    static const int order4[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    const int* offset = order4[giStride == 2 ? ctx.frameCount % 4 : 0];
    // Each render texel is traced through the centre of another of the 2x2 window pixels it covers each
    // frame, as the voxel renderer does: a still view repeats the same four, and the resolve settles on them.
    static const int cellOrder[4] = {0, 3, 1, 2};
    const int cell = cellOrder[ctx.frameCount % 4];
    vec4 jitterParams{(float(cell % 2) + 0.5f) * 0.5f - 0.5f, (float(cell / 2) + 0.5f) * 0.5f - 0.5f, 0.0f, 0.0f};
    vec4 renderParams{0.0f, 0.0f, g_reconstructBypass ? 1.0f : 0.0f, 0.0f};
    // SSAO six of the finest voxels round, darkening the indirect light by up to half; the sun's penumbra
    // per unit of blocker distance (the disc's full width); the fog's density: its length 1.6 scene radii,
    // so the far side of the scene is about a third hazed, held to 30..300 m of Teardown-scale voxels.
    const float fogLength = std::clamp(1.6f * g_sceneRadius, 300.0f, 3000.0f);
    vec4 look{6.0f * finest, 0.5f, g_restirSoftShadow ? 2.0f * std::tan(RESTIR_SUN_HALF_ANGLE) : 0.0f,
              g_restirFog ? 1.0f / fogLength : 0.0f};
    static bool first = true;

    vec4 cameraPos{ctx.cameraPosition, 1.0f}, cameraDir{ctx.cameraDirection, 0.0f};
    vec4 prevCameraPos{ctx.prevCameraPosition, 1.0f}, prevCameraDir{ctx.prevCameraDirection, 0.0f};
    vec4 sunDir{ctx.sunDirection, 0.0f};
    vec4 frameCount{float(ctx.frameCount), ctx.cameraMoved ? 1.0f : 0.0f, 0.0f, 0.0f};
    vec4 reuse{g_restirReuse ? 1.0f : 0.0f, g_restirReuse ? 1.0f : 0.0f, 1.0f, 4.0f};   // temporal, spatial, visibility, M cap
    // For unattended captures (--capture): RESTIR_VIEW picks the starting view; RESTIR_POST=0, RESTIR_FOG=0
    // and RESTIR_SOFT=0 start with the post, the fog or the soft shadow off.
    static const bool fromEnv = [] {
        if (const char* v = std::getenv("RESTIR_VIEW")) g_restirView = std::atoi(v) & 3;
        if (const char* v = std::getenv("RESTIR_POST")) g_restirPost.on = std::atoi(v) != 0;
        if (const char* v = std::getenv("RESTIR_FOG")) g_restirFog = std::atoi(v) != 0;
        if (const char* v = std::getenv("RESTIR_SOFT")) g_restirSoftShadow = std::atoi(v) != 0;
        return true;
    }();
    (void)fromEnv;
    vec4 denoise{1.0f, 1.0f, float(g_restirView), 32.0f};                           // a-trous, GI accumulation, view, its cap
    vec4 jitter{float(offset[0]), float(offset[1]), float(giStride), first ? 1.0f : 0.0f};
    // The image's history: short while the camera or the sun moves, so changing light is not dragged along.
    // w: one render texel's width per unit of distance, for the tests that ask "is this the same surface?".
    const float renderHeight = std::max(ctx.windowResolution.y * RESTIR_RENDER_SCALE, 1.0f);
    const float pixelWidth = 2.0f * std::tan(30.0f * 3.14159265f / 180.0f) / renderHeight;
    vec4 settings{6.0f, finest * 0.05f, ctx.cameraMoved ? 16.0f : 1024.0f, pixelWidth};
    setUniformToValue(renderer, "cameraPos", cameraPos);
    setUniformToValue(renderer, "cameraDir", cameraDir);
    setUniformToValue(renderer, "prevCameraPos", prevCameraPos);
    setUniformToValue(renderer, "prevCameraDir", prevCameraDir);
    setUniformToValue(renderer, "sunDir", sunDir);
    setUniformToValue(renderer, "frameCount", frameCount);
    setUniformToValue(renderer, "restirReuse", reuse);
    setUniformToValue(renderer, "restirDenoise", denoise);
    setUniformToValue(renderer, "restirJitter", jitter);
    setUniformToValue(renderer, "restirSettings", settings);
    setUniformToValue(renderer, "restirLook", look);
    // The post's switches, and the frame's length for the exposure's adaptation (its own clock: the
    // gallery's frame timer exists only in perf builds).
    static auto lastFrame = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::chrono::duration<float>(now - lastFrame).count();
    lastFrame = now;
    const RestirPost& post = g_restirPost;
    vec4 postParams{post.on && post.vignette ? RESTIR_VIGNETTE : 0.0f, post.on && post.grade ? 1.0f : 0.0f,
                    post.on && post.exposure ? 1.0f : 0.0f, seconds};
    setUniformToValue(renderer, "restirPost", postParams);
    vec4 dof{post.on && post.dof ? RESTIR_DOF_PIXELS * ctx.windowResolution.y / 1080.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    setUniformToValue(renderer, "restirDof", dof);
    setUniformToValue(renderer, "jitterParams", jitterParams);
    setUniformToValue(renderer, "renderParams", renderParams);
    first = false;
}

static bool measureSceneBounds(const projv::Scene& scene, projv::core::vec3& boundsMin, projv::core::vec3& boundsMax);

// The examples' game renderer: its own loader (it scales its targets by the View's render scale) and its
// own uniforms, from the View. The scene sets its scale: the finest voxel, and the haze across it.
static void installVoxelRenderer(projv::Application& app, projv::graphics::RenderInstance& renderInstance, const projv::Scene& scene) {
    voxel_renderer::View& view = app.world.ctx().emplace<voxel_renderer::View>();
    float finest = 0.0f;
    for (const projv::Chunk& chunk : scene.chunks)
        if (chunk.alive && chunk.header.voxelScale > 0.0f) finest = finest > 0.0f ? std::min(finest, chunk.header.voxelScale) : chunk.header.voxelScale;
    if (finest > 0.0f) view.finestVoxel = finest;
    projv::core::vec3 boundsMin, boundsMax;
    if (measureSceneBounds(scene, boundsMin, boundsMax)) view.hazeRadius = std::max(projv::core::length(boundsMax - boundsMin), 1.0f);
    voxel_renderer::install(app.world, renderInstance, std::filesystem::current_path(), g_voxelFolder);
}

static void uploadVoxelUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    voxel_renderer::View& view = ctx.world->ctx().get<voxel_renderer::View>();
    static const int debugView = std::getenv("GALLERY_DEBUG") ? std::atoi(std::getenv("GALLERY_DEBUG")) : 0;
    view.debugView = debugView;
    static const float jitter = std::getenv("GALLERY_JITTER") ? float(std::atof(std::getenv("GALLERY_JITTER"))) : 1.0f;
    view.jitter = jitter;
    view.sun = ctx.sunAngle;
    view.taa = g_voxelTAA;
    voxel_renderer::upload(*ctx.world, renderer, ctx.cameraPosition, ctx.cameraDirection);
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

    // ReSTIR GI: two bounces of light, traced for a sixteenth of the pixels and shared between pixels
    // and frames by reservoir resampling, then denoised. Born in the ray lab, where it ran the whole
    // frame in about a quarter of the path tracer's time at a similar or better image.
    registry.push_back({
        "restir",
        "ReSTIR GI at a sixteenth of the pixels, SSAO, a half-res G-buffer reconstructed to full. G view, J reuse, Q reconstruction, P post (V, B, E, K), O fog, U soft shadow.",
        "./renderers/restir/",
        "./renderers/restir/shaders/vs_quad.bin",
        uploadRestirUniforms
    });

    // The one the games use, from examples/shared/voxelRenderer (staged as voxelRenderer/).
    registry.push_back({
        "voxel",
        "The examples' game renderer: G-buffer, AO and bounce, ray-traced GI, reconstruction, TAA. T toggles TAA.",
        "./voxelRenderer/",
        "./voxelRenderer/shaders/vs_quad.bin",
        uploadVoxelUniforms,
        installVoxelRenderer
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

// Unattended measurement (--bench, --capture, --look, --pan).
struct Measure {
    bool        bench = false;
    std::string capture;
    bool        look = false;
    projv::core::vec3 from{0.0f}, at{0.0f};
    float       pan = 0.0f;
};
static Measure g_measure;

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
        if (argument == "--bench") { g_measure.bench = true; continue; }
        if ((argument == "--capture" || argument == "--look" || argument == "--pan" || argument == "--voxel-folder" ||
             argument == "--restir-gi") && i + 1 < argc) {
            const char* value = argv[++i];
            if (argument == "--capture") g_measure.capture = value;
            else if (argument == "--pan") g_measure.pan = float(std::atof(value));
            else if (argument == "--voxel-folder") g_voxelFolder = value;
            else if (argument == "--restir-gi") g_restirStride = std::atoi(value) == 2 ? 2 : 4;
            else if (std::sscanf(value, "%f,%f,%f,%f,%f,%f", &g_measure.from.x, &g_measure.from.y, &g_measure.from.z,
                                 &g_measure.at.x, &g_measure.at.y, &g_measure.at.z) == 6) g_measure.look = true;
            else { std::cerr << "--look takes x,y,z,tx,ty,tz\n"; return false; }
            continue;
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
    float moveSpeed;   // World units per second.
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
        framing.moveSpeed = 60.0f;
        return framing;
    }

    vec3 center = (boundsMin + boundsMax) * 0.5f;
    vec3 extents = boundsMax - boundsMin;
    float radius = length(extents) * 0.5f;
    if (radius <= 0.0f) radius = 1.0f;
    g_sceneRadius = radius;

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
    // About ten seconds to cross the scene, so a small model is as quick to get round as a big one -- but
    // no more than ten eye heights a second (80 voxels: 8 m/s at Teardown's scale), or a level whose
    // bounds take in its terrain and sea is crossed at a car's speed. Shift moves four times as fast.
    framing.moveSpeed = std::min(std::max(radius * 2.0f / 10.0f, 0.5f), std::max(10.0f * eyeHeight, 0.5f));

    projv::core::info("Framed camera at ({:.1f}, {:.1f}, {:.1f}), move speed {:.2f}/s",
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

    projv::graphics::RenderInstance& renderInstance = app.world.ctx().emplace<projv::graphics::RenderInstance>();
    renderInstance.initialize(1920, 1080, ("ProjectV — " + g_selectedRenderer.id).c_str());

    // Capture the cursor so mouse motion drives the camera (FPS-style mouse look).
    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // Scroll wheel controls the sun's elevation (day cycle).
    glfwSetScrollCallback(renderInstance.window, sunScrollCallback);

    projv::Scene& scene     = app.world.ctx().emplace<projv::Scene>();
    float& cameraPhi         = app.world.ctx().emplace<float>();
    projv::GPUData& gpuData  = app.world.ctx().emplace<projv::GPUData>();
    RendererModule& selectedRenderer = app.world.ctx().emplace<RendererModule>();
    CameraFraming& framing = app.world.ctx().emplace<CameraFraming>();
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

    if (selectedRenderer.install) {
        selectedRenderer.install(app, renderInstance, scene);
        gpuData = projv::graphics::createTexturesForScene(scene);
        return;
    }

    projv::RendererSpecification rendererSpec = projv::graphics::loadRendererSpecification(selectedRenderer.directory);
    // restir's GI targets are its quarter-size ones; --restir-gi sets how far below the window they are.
    if (selectedRenderer.id == "restir")
        for (projv::Texture& texture : rendererSpec.resources.textures)
            if (texture.scale == 0.25f) texture.scale = 1.0f / float(g_restirStride);
    renderInstance.addRendererSpecification(1, rendererSpec);

    bgfx::ShaderHandle vsh = projv::graphics::loadShader(selectedRenderer.vertexShaderPath);
    std::shared_ptr<projv::ConstructedRenderer> constructedRenderer = projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vsh);

    // Load the blue-noise LUT used by the path tracers for sample decorrelation -- into texture 1, for
    // the renderers that declare it there as a CPU-fed texture. (restir's texture 1 is a render target.)
    bool wantsBlueNoise = false;
    for (const projv::Texture& texture : rendererSpec.resources.textures)
        if (texture.textureID == 1 && texture.origin == projv::CPUBuffer) wantsBlueNoise = true;
    if (wantsBlueNoise) {
        int width, height, channels;
        unsigned char* img = stbi_load("LDR_RGBA_7.png", &width, &height, &channels, 4);
        if (img) {
            projv::core::info("Blue-noise texture: {}x{}", width, height);
            projv::graphics::setTextureToData(constructedRenderer, 1, img, width, height);
        } else {
            projv::core::warn("Could not load LDR_RGBA_7.png; sampling will be correlated and noisier.");
        }
    }

    renderInstance.setActiveRenderer(constructedRenderer);
    gpuData = projv::graphics::createTexturesForScene(scene);
}

// Update: frame timing profiler.
void update(projv::Application& app) {
    // The engine records a window-manager close request on the RenderInstance; acting on it is the
    // application's decision. Ending the loop here runs the Shutdown stage on the way out.
    projv::graphics::RenderInstance& renderInstance =
        app.world.ctx().get<projv::graphics::RenderInstance>();
    if (renderInstance.shouldClose) {
        app.closeRequested = true;
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
    projv::graphics::RenderInstance& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
    projv::GPUData& gpuData          = app.world.ctx().get<projv::GPUData>();
    float& cameraPhi                 = app.world.ctx().get<float>();
    RendererModule& selectedRenderer = app.world.ctx().get<RendererModule>();
    CameraFraming& framing           = app.world.ctx().get<CameraFraming>();

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
        if (g_measure.look) {
            projv::core::vec3 d = projv::core::normalize(g_measure.at - g_measure.from);
            cameraPosition = g_measure.from;
            cameraPhi = std::atan2(d.z, d.x);
            cameraPitch = std::asin(d.y);
        }
        cameraInitialized = true;
    }
    const bool unattended = g_measure.bench || !g_measure.capture.empty();
    if (g_measure.pan != 0.0f) {
        cameraPhi += g_measure.pan;
        cameraMoved = true;
    }

    // Cursor capture toggle: Esc releases the cursor (so it can leave the
    // window), left-click re-captures it. Only look around while captured.
    static bool mouseCaptured = !unattended;
    if (unattended) glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
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

    // Movement: the framing's speed (world units a second, from the scene's size -- see frameScene) times
    // this frame's length, so it does not change with the frame rate. Shift moves four times as fast.
    static auto lastMove = std::chrono::steady_clock::now();
    const auto moveNow = std::chrono::steady_clock::now();
    const float moveSeconds = std::min(std::chrono::duration<float>(moveNow - lastMove).count(), 0.1f);
    lastMove = moveNow;
    const bool fast = glfwGetKey(renderInstance.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
    const float moveSpeed = framing.moveSpeed * moveSeconds * (fast ? 4.0f : 1.0f);

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

    {
        static bool prevT = false;
        bool t = glfwGetKey(renderInstance.window, GLFW_KEY_T) == GLFW_PRESS;
        if (t && !prevT) {
            g_voxelTAA = !g_voxelTAA;
            projv::core::info("TAA: {}", g_voxelTAA ? "on" : "off");
        }
        prevT = t;
    }

    {
        static bool prevG = false, prevJ = false;
        bool g = glfwGetKey(renderInstance.window, GLFW_KEY_G) == GLFW_PRESS;
        bool j = glfwGetKey(renderInstance.window, GLFW_KEY_J) == GLFW_PRESS;
        // The post's toggles: P all of it, V the vignette, B the grade, E the auto-exposure.
        struct PostKey { int key; bool* flag; const char* name; bool prev; };
        static PostKey postKeys[] = {
            {GLFW_KEY_P, &g_restirPost.on, "post-processing", false},
            {GLFW_KEY_V, &g_restirPost.vignette, "vignette", false},
            {GLFW_KEY_B, &g_restirPost.grade, "colour grade", false},
            {GLFW_KEY_E, &g_restirPost.exposure, "auto-exposure", false},
            {GLFW_KEY_K, &g_restirPost.dof, "depth of field", false},
            {GLFW_KEY_O, &g_restirFog, "distance fog", false},
            {GLFW_KEY_U, &g_restirSoftShadow, "soft sun shadow", false},
        };
        for (PostKey& k : postKeys) {
            bool down = glfwGetKey(renderInstance.window, k.key) == GLFW_PRESS;
            if (down && !k.prev) {
                *k.flag = !*k.flag;
                const bool partOfPost = k.flag != &g_restirPost.on && k.flag != &g_restirFog && k.flag != &g_restirSoftShadow;
                projv::core::info("ReSTIR {}: {}{}", k.name, *k.flag ? "on" : "off",
                                  partOfPost && !g_restirPost.on ? " (post is off: P)" : "");
            }
            k.prev = down;
        }
        if (g && !prevG) {
            g_restirView = (g_restirView + 1) % 4;
            static const char* views[] = {"the image", "indirect light only", "direct light only", "ambient occlusion"};
            projv::core::info("ReSTIR view: {}", views[g_restirView]);
        }
        if (j && !prevJ) {
            g_restirReuse = !g_restirReuse;
            projv::core::info("ReSTIR reuse: {}", g_restirReuse ? "on" : "off (one fresh sample per GI pixel)");
        }
        prevG = g;
        prevJ = j;
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
    ctx.sunAngle               = sunAngle;
    ctx.world                  = &app.world;

    selectedRenderer.uploadUniforms(renderInstance.getActiveRenderer(), ctx);

    projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpuData);

    // --bench: each pass's GPU time, frames 120..360, by the shader each view ran.
    if (g_measure.bench) {
        static double passMs[32] = {}, frameMs = 0.0;
        static int samples = 0;
        if (app.frameCount == 1) bgfx::setDebug(BGFX_DEBUG_PROFILER);
        const bgfx::Stats* stats = bgfx::getStats();
        if (app.frameCount > 120 && app.frameCount <= 360 && stats && stats->gpuTimerFreq > 0) {
            const double toMs = 1000.0 / double(stats->gpuTimerFreq);
            frameMs += double(stats->gpuTimeEnd - stats->gpuTimeBegin) * toMs;
            for (uint16_t v = 0; v < stats->numViews; v++)
                if (stats->viewStats[v].view < 32)
                    passMs[stats->viewStats[v].view] += double(stats->viewStats[v].gpuTimeEnd - stats->viewStats[v].gpuTimeBegin) * toMs;
            samples++;
        }
        if (app.frameCount == 361) {
            const projv::RendererSpecification& spec = renderInstance.getRendererSpecification(1);
            std::string line = fmt::format("BENCH {} {}x{}: frame {:.2f} ms |", selectedRenderer.id, int(ctx.windowResolution.x),
                                           int(ctx.windowResolution.y), samples ? frameMs / samples : 0.0);
            const auto& passes = spec.dependencyGraph.renderPasses;
            for (size_t v = 0; v < passes.size() && v < 32; v++) {
                std::string name = std::to_string(passes[v].shaderID);
                for (const projv::Shader& shader : spec.resources.shaders)
                    if (shader.shaderID == passes[v].shaderID) name = std::filesystem::path(shader.filePath).stem().string();
                line += fmt::format(" {} {:.2f}", name, samples ? passMs[v] / samples : 0.0);
            }
            projv::core::info("{}", line);
            app.closeRequested = true;
        }
    }
    // --capture: three frames in a row, for what changes between them with nothing moving.
    if (!g_measure.capture.empty()) {
        if (app.frameCount >= 300 && app.frameCount <= 302)
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, fmt::format("{}-{}", g_measure.capture, app.frameCount).c_str());
        if (app.frameCount > 306) app.closeRequested = true;
    }

    // Remember this frame's camera for next frame's reprojection.
    prevCameraPosition  = cameraPosition;
    prevCameraDirection = cameraDirection;
}

void shutdown(projv::Application& app) {
    projv::GPUData& gpuData = app.world.ctx().get<projv::GPUData>();
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

    projv::Application app;
    app.addSystem(projv::Stage::Startup, "startup", startup);
    app.addSystem(projv::Stage::Update, "update", update);
    app.addSystem(projv::Stage::Render, "render", render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    return 0;
}
