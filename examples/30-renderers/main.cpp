// ProjectV Renderer Gallery
//
// One host application, three renderers, the same scene and the same camera. The point is the
// comparison: each renderer answers "how should this scene be lit" differently, and the only way
// to judge that is to put them side by side under identical conditions.
//
// A renderer here is *data*, not code -- a folder holding render.json (the pass order),
// resources.json (the shaders, render targets and framebuffers) and the shaders themselves. This
// file supplies the window, the camera, the scene and the per-frame uniforms; everything that
// makes one renderer differ from another lives in its folder.
//
// Each renderer has its own README in renderers/<name>/, and the example's README compares them.
//
//   restir-pt           ReSTIR PT: two-bounce paths through the materials, reused at a sixteenth of the
//                       pixels, denoised, then a temporal resolve
//   pt-reference        the ground truth: a plain path tracer on the engine's BSDF, accumulated while still
//   fast                direct sun + screen-space AO, anti-aliased by TAA. Cheapest.
//
// The Gallery panel (the dev layer, F1) switches between them and holds each one's settings. A renderer
// is built the first time it is picked and kept, so switching back is instant (docs/plans/gallery-ui.md).
//
// Usage:
//   ./renderer_gallery [--renderer <name>] [--scene <directory>] [--list]
//
//   --renderer   which renderer to start on (default: restir-pt)
//   --scene      a folder holding a compose.json (default: SanMiguel, from the scene previewer), or
//                material-room: a room built here, to compare materials in (glossy floor, metal, mirror)
//   --list       print the available renderers and exit
//
// Measuring (unattended; the window opens and closes by itself, the panel hidden):
//   --bench            each pass's GPU time over frames 120..360, printed, then exit
//   --capture <prefix> saves frames 300, 301 and 302 as <prefix>-<frame>.tga, then exits
//                      (--capture and --bench open a fixed 1280x720 window, so runs compare)
//   --look x,y,z,tx,ty,tz   stands the camera at x,y,z looking at tx,ty,tz (default: the framing)
//   --pan <rad>        turns the camera this much a frame (a moving-camera measurement)
//   --restir-gi <n>    restir-pt traces paths for one pixel in n x n: 4 (a sixteenth, default) or 2 (a quarter)
//
// Selection is a flag rather than a prompt so the gallery can be scripted: a benchmark or a
// screenshot matrix across all of them is a shell loop.
//
// Controls:
//   W/S          — move forward / backward (Shift: four times as fast)
//   A/D          — strafe left / right
//   R/F          — move up / down
//   Mouse        — look around (Esc releases the cursor, a click on the scene captures it again)
//   Scroll wheel — sun elevation (also in the panel)
//   H            — re-frame on the scene
//   C            — print where the camera stands, as the --look (and GALLERY_SUN) that puts it back
//   F1           — the panel: the renderer and its settings

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
#include "graphics/engine_shaders.h"
#include "graphics/camera.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/type_mapping.h"
#include "graphics/input.h"
#include "ui/dev.h"
#include "ui/ui.h"
#include "utils/blob_voxels.h"
#include "utils/compose_io.h"
#include "utils/material.h"
#include "utils/picking.h"
#include "utils/voxel_management.h"
#include "restir_pt.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// The sun's elevation (a day cycle -- see render()), in scroll-wheel notches: the wheel over the scene, the
// panel's slider, or GALLERY_SUN.
static double g_sunScrollAccum = 0.0;

// =============================================================================
// Renderer modularity
// =============================================================================

// Everything a per-frame uniform upload might need, gathered in one place so
// each renderer module can pick out exactly the uniforms it declares.
struct FrameContext {
    projv::core::vec3 cameraPosition;
    projv::core::vec3 cameraDirection;
    bool              cameraMoved;    // the camera or the sun
    bool              sunMoved;       // the sun alone: the light changed, not just the view
    bool              switched;       // the renderer was switched to this frame: its histories start over
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
    // Uploads this renderer's per-frame uniforms. Must set every uniform the
    // renderer's resources.json declares (and may ignore the rest).
    std::function<void(std::shared_ptr<projv::ConstructedRenderer>, const FrameContext&)> uploadUniforms;
    // Optional: loads the renderer itself, in place of the generic load of `directory`.
    std::function<void(projv::Application&, projv::graphics::RenderInstance&, const projv::Scene&)> install;
};

// Uniforms shared by every renderer in the project.
static void uploadCommonUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    projv::core::vec3 cameraPosition = ctx.cameraPosition;
    projv::core::vec3 cameraDirection = ctx.cameraDirection;
    projv::core::vec2 windowResolution = ctx.windowResolution;
    projv::core::vec4 frameCount = { (float)ctx.frameCount, (float)ctx.cameraMoved, (float)ctx.frameCameraLastMovedOn, 0 };
    projv::core::vec2 texelSize = { 1 / windowResolution.x, 1 / windowResolution.y };

    projv::core::vec4 sunDir = { ctx.sunDirection.x, ctx.sunDirection.y, ctx.sunDirection.z, 0.0f };

    projv::CameraView camera;
    camera.position = cameraPosition;
    camera.forward = cameraDirection;
    projv::graphics::setCamera(*renderer, camera);
    projv::graphics::setUniformToValue(renderer, "windowRes",  windowResolution);
    projv::graphics::setUniformToValue(renderer, "frameCount", frameCount);
    projv::graphics::setUniformToValue(renderer, "texelSize",  texelSize);
    projv::graphics::setUniformToValue(renderer, "sunDir",     sunDir);
}

// The finest voxel edge in the scene: rays leave a surface a twentieth of it off, and the "same surface"
// tests of the reusing renderers scale from it.
static float finestVoxel(const projv::World& world) {
    static float finest = 0.0f;
    if (finest <= 0.0f) {
        for (const projv::Chunk& chunk : world.ctx().get<projv::Scene>().chunks)
            if (chunk.alive && chunk.header.voxelScale > 0.0f)
                finest = finest > 0.0f ? std::min(finest, chunk.header.voxelScale) : chunk.header.voxelScale;
        if (finest <= 0.0f) finest = 1.0f;
    }
    return finest;
}

// The reference path tracer: the common uniforms, and its settings -- the loop's bound (a path ray and
// a shadow ray per vertex), the surface offset, and the bounces. Two by default: restir-pt's path length,
// so the two compare; the panel goes to six.
static int g_referenceBounces = restir_pt::PATH_BOUNCES;
static void uploadReferenceUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    uploadCommonUniforms(renderer, ctx);
    projv::core::vec4 settings{float(2 * (g_referenceBounces + 1)), finestVoxel(*ctx.world) * 0.05f, float(g_referenceBounces), 0.0f};
    projv::graphics::setUniformToValue(renderer, "ptSettings", settings);
}

// The fast renderer. Its TAA reprojects by last frame's camera, which the engine keeps (pjvPrevView*). Its
// settings are its SSAO's (ssao.frag) and whether combine_blur.frag denoises it.
struct FastSettings {
    float aoRadius = 4.0f;      // world units an occluder may sit from the point
    float aoIntensity = 1.0f;   // how much of the occlusion is applied
    float aoPower = 2.0f;       // how hard its response bends
    bool  blur = true;          // the bilateral denoise of the AO
};
static FastSettings g_fast;
static void uploadReprojectionUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    uploadCommonUniforms(renderer, ctx);
    projv::core::vec4 settings{g_fast.aoRadius, g_fast.aoIntensity, g_fast.aoPower, g_fast.blur ? 1.0f : 0.0f};
    projv::graphics::setUniformToValue(renderer, "fastSettings", settings);
}

// The restir-pt renderer's switches: G cycles what it shows (the image, the indirect light alone, the
// direct alone, the ambient occlusion, the indirect diffuse alone, the indirect specular alone), J turns
// ReSTIR's reuse off -- each GI pixel's one fresh path, denoised -- for the A/B, and Q bypasses the
// reconstruction, as for the reconstruction renderer.
// The post (display.frag): P turns it all off and on, V the vignette, B the grade, E the auto-exposure.
// restir-pt's settings: restir_pt::Settings (examples/shared/restir_pt.h, where its uniforms are worked
// out for every program that draws with it), changed from the Gallery panel. Their starting values come
// from the environment, for unattended runs (readRestirEnvironment).
static restir_pt::Settings g_restir;
static bool  g_restirSoftNormals = true;               // the blurred normals; g_restirNormalBlur their width
static float g_restirNormalBlur = restir_pt::NORMAL_BLUR;
static bool  g_restirUntold = false;                   // RESTIR_UNTOLD=1: see readRestirEnvironment
static float g_sceneRadius = 100.0f;                  // frameScene's, for the fog's length
static float g_sceneGround = 0.0f;                    // ...and the scene's lowest point, for the fog's height
static float g_sceneHeight = 0.0f;                    // ...and its height, for the aerial haze's
static int  g_restirStride = 4;   // --restir-gi: GI for one window pixel in stride x stride (4 or 2)
static constexpr float RESTIR_RENDER_SCALE = restir_pt::RENDER_SCALE;   // its G-buffer, each way

// The environment's starting values: RESTIR_VIEW the view; RESTIR_POST=0 (the raw ReSTIR PT output),
// RESTIR_FOG=0, RESTIR_SHAFTS=0, RESTIR_AIR=0, RESTIR_FAR_AO=0, RESTIR_BLOOM=0, RESTIR_MBLUR=0,
// RESTIR_EXPOSURE=0 and RESTIR_SOFT=0 start with that part off; RESTIR_VALIDATE=1 the paths' validation on;
// RESTIR_REUSE=0 the reuse off, RESTIR_TEMPORAL=0 / RESTIR_SPATIAL=0 / RESTIR_VISIBILITY=0 one part of it;
// RESTIR_SSAO=0 the SSAO (for comparing with pt-reference, which has none); RESTIR_ATROUS=0 the a-trous;
// RESTIR_RAW=1 no GI accumulation and no image history; RESTIR_BOUNCES=n the paths' length;
// RESTIR_NORMAL_BLUR=w the blurred normals' width; RESTIR_JITTER=0 the old four window-pixel centres;
// GALLERY_BYPASS=1 the reconstruction bypassed. RESTIR_UNTOLD=1 moves the sun without telling the
// renderer -- no history starts over -- as a game's light changes: what only validation can catch.
static void readRestirEnvironment() {
    auto flag = [](const char* name, bool& value) {
        if (const char* v = std::getenv(name)) value = std::atoi(v) != 0;
    };
    if (const char* v = std::getenv("RESTIR_VIEW")) g_restir.view = std::clamp(std::atoi(v), 0, restir_pt::VIEWS - 1);
    if (const char* v = std::getenv("RESTIR_VALIDATE")) g_restir.validation = std::atoi(v) != 0 ? restir_pt::VALIDATION : 0.0f;
    if (const char* v = std::getenv("RESTIR_SSAO")) g_restir.ssao = std::atoi(v) != 0 ? restir_pt::SSAO : 0.0f;
    if (const char* v = std::getenv("RESTIR_ATROUS")) g_restir.atrous = std::atoi(v) != 0 ? 1.0f : 0.0f;
    if (const char* v = std::getenv("RESTIR_BOUNCES")) g_restir.bounces = float(std::max(1, std::atoi(v)));
    if (const char* v = std::getenv("RESTIR_NORMAL_BLUR")) g_restirNormalBlur = float(std::atof(v));
    flag("RESTIR_REUSE", g_restir.reuse);
    flag("RESTIR_TEMPORAL", g_restir.temporal);
    flag("RESTIR_SPATIAL", g_restir.spatial);
    flag("RESTIR_VISIBILITY", g_restir.visibility);
    flag("RESTIR_RAW", g_restir.raw);
    flag("RESTIR_JITTER", g_restir.halton16);
    flag("GALLERY_BYPASS", g_restir.bypass);
    flag("RESTIR_POST", g_restir.post.on);
    flag("RESTIR_BLOOM", g_restir.post.bloom);
    flag("RESTIR_MBLUR", g_restir.post.motionBlur);
    flag("RESTIR_EXPOSURE", g_restir.post.exposure);
    flag("RESTIR_FOG", g_restir.fog);
    flag("RESTIR_SHAFTS", g_restir.shafts);
    flag("RESTIR_AIR", g_restir.air);
    flag("RESTIR_FAR_AO", g_restir.farAO);
    flag("RESTIR_SOFT", g_restir.softShadow);
    flag("RESTIR_UNTOLD", g_restirUntold);
}

// ReSTIR PT at a sixteenth (or a quarter) of the window's pixels, a G-buffer at half resolution each way
// rebuilt to the window, then a temporal resolve.
static void uploadRestirUniforms(std::shared_ptr<projv::ConstructedRenderer> renderer, const FrameContext& ctx) {
    const bool untold = g_restirUntold;
    restir_pt::Settings settings = g_restir;
    settings.normalBlur = g_restirSoftNormals ? g_restirNormalBlur : 0.0f;
    settings.stride = g_restirStride;
    // P off: the raw ReSTIR PT output. No a-trous, no GI accumulation and no image history (the resolve's
    // TAA): each frame's estimate alone, as noisy as it is.
    if (!settings.post.on) {
        settings.atrous = 0.0f;
        settings.raw = true;
    }

    // The post's frame length for the exposure's adaptation: its own clock (the gallery's frame timer
    // exists only in perf builds).
    static auto lastFrame = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    static bool first = true;
    restir_pt::Frame frame;
    frame.position = ctx.cameraPosition;
    frame.direction = ctx.cameraDirection;
    frame.sunDirection = ctx.sunDirection;
    // An untold sun is no move at all, as far as the renderer knows: the camera stands still in that test.
    frame.sunMoved = ctx.sunMoved && !untold;
    frame.moved = ctx.cameraMoved && !(ctx.sunMoved && untold);
    frame.first = first || ctx.switched;   // a switch back: every history starts over
    frame.count = ctx.frameCount;
    frame.window = ctx.windowResolution;
    frame.seconds = std::chrono::duration<float>(now - lastFrame).count();
    lastFrame = now;
    first = false;
    restir_pt::upload(renderer, frame, {finestVoxel(*ctx.world), g_sceneRadius, g_sceneGround, g_sceneHeight}, settings, ctx.world);
}

// The registry: the three renderers the gallery shows (docs/plans/gallery-ui.md), one folder each under
// renderers/. The `id` is the folder's name, what --renderer takes, and what the panel lists.
static std::vector<RendererModule> buildRendererRegistry() {
    std::vector<RendererModule> registry;

    // ReSTIR PT: two-bounce paths through the materials, traced for a sixteenth of the pixels and
    // shared between pixels and frames by resampling whole paths (reconnected where that is safe,
    // replayed where it is not), then denoised diffuse and specular apart.
    registry.push_back({
        "restir-pt",
        "ReSTIR PT at a sixteenth of the pixels, through the materials; a half-res G-buffer reconstructed to full.",
        "./renderers/restir-pt/",
        uploadRestirUniforms
    });

    // The reference: one path per pixel per frame through the engine's BSDF (pjv_bsdf.sc), nothing
    // reused or filtered, averaged while the view is still. Slow and exact -- what the others are
    // measured against, restir-pt above all (the same sun, sky, path length and display).
    registry.push_back({
        "pt-reference",
        "Reference path tracer: paths through the materials, accumulated while still. The ground truth.",
        "./renderers/pt-reference/",
        uploadReferenceUniforms
    });

    registry.push_back({
        "fast",
        "Direct sun plus screen-space AO, anti-aliased by TAA. The cheapest renderer here.",
        "./renderers/fast/",
        uploadReprojectionUniforms
    });

    return registry;
}

// Command line. Selection is a flag rather than a prompt so the gallery can be driven by a
// script -- a benchmark across all four renderers should not need someone at the keyboard.
struct Options {
    std::string rendererId = "restir-pt";
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
        if ((argument == "--capture" || argument == "--look" || argument == "--pan" || argument == "--restir-gi") && i + 1 < argc) {
            const char* value = argv[++i];
            if (argument == "--capture") g_measure.capture = value;
            else if (argument == "--pan") g_measure.pan = float(std::atof(value));
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
// a benchmark that quietly measured the default renderer four times would be worse than an error.
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
    g_sceneGround = boundsMin.y;
    g_sceneHeight = extents.y;

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
    // their primary rays carry a step budget (tree64, the old path tracer, used 200) and drop to a coarse
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
    // camera 46 units up in SanMiguel, which sounds harmless until you count steps: tree64's (since retired) primary
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
// The material room (--scene material-room)
// =============================================================================
//
// What restir-pt is compared with pt-reference on: one 64^3 chunk of quarter-metre voxels, a room 16 m
// across with a checkered glossy floor, a metal wall, a mirror panel, a red wall to bleed colour, a
// window and a skylight the default sun falls through onto the floor, a semi-glossy block, a white cube
// and a small lamp. Built here rather than shipped, so the materials are in the code that names them.

static constexpr float ROOM_VOXEL = 0.25f;

static projv::Scene buildMaterialRoom() {
    using projv::packColor;
    constexpr int N = 64;
    projv::Scene scene;
    projv::Chunk chunk;
    chunk.header.chunkID = 1;
    chunk.header.position = projv::core::vec3(0.0f);
    chunk.header.scale = N * ROOM_VOXEL;
    chunk.header.voxelScale = ROOM_VOXEL;
    chunk.header.resolution = N;
    chunk.header.rotation = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);
    chunk.requestedLOD = 0;
    chunk.alive = true;
    chunk.componentHandle = 0;
    scene.components.push_back(projv::ComponentRecord{projv::ComponentKind::Chunk, 0, -1, "internal/material-room", false, {}, -1});

    // glossiness, metallic; an emitter's strength (its light is its albedo's colour).
    auto material = [&](const char* name, projv::Color colour, float glossiness, float metallic, float emission = 0.0f) {
        uint8_t slot = projv::utils::addMaterial(scene, 0, name, packColor(colour));
        projv::utils::setMaterialProperties(scene, 0, slot, 0u, projv::packSurfaceWord(glossiness, metallic, 0.0f, 1.0f),
                                            projv::packExtraWord(emission, 0.0f, 0u));
        return slot;
    };
    const uint8_t floorLight = material("floor light", {200, 195, 185}, 0.85f, 0.0f);
    const uint8_t floorDark  = material("floor dark", {60, 62, 70}, 0.85f, 0.0f);
    const uint8_t white      = material("white", {220, 220, 220}, 0.0f, 0.0f);
    const uint8_t red        = material("red", {190, 50, 40}, 0.0f, 0.0f);
    const uint8_t metal      = material("metal", {230, 170, 120}, 0.75f, 1.0f);
    const uint8_t mirror     = material("mirror", {240, 240, 240}, 1.0f, 1.0f);
    const uint8_t satin      = material("satin blue", {50, 90, 200}, 0.5f, 0.0f);
    const uint8_t lamp       = material("lamp", {255, 200, 140}, 0.0f, 0.0f, 8.0f);

    auto map = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(N));
    auto box = [&](int x0, int y0, int z0, int x1, int y1, int z1, uint8_t slot) {   // [x0, x1) etc.
        for (int z = z0; z < z1; ++z)
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) projv::utils::brickMapSetVoxel(*map, x, y, z, slot);
    };
    auto hole = [&](int x0, int y0, int z0, int x1, int y1, int z1) {
        for (int z = z0; z < z1; ++z)
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) projv::utils::brickMapClearVoxel(*map, x, y, z);
    };
    for (int z = 0; z < N; z += 8)   // the floor: 2 m tiles, light and dark, both glossy
        for (int x = 0; x < N; x += 8) box(x, 0, z, x + 8, 2, z + 8, ((x + z) / 8) % 2 ? floorDark : floorLight);
    box(0, 38, 0, N, 40, N, white);       // the ceiling...
    hole(8, 38, 30, 32, 40, 54);          // ...and its skylight: the default sun lands mid-floor
    box(0, 2, 0, 2, 38, N, metal);        // west: brushed metal
    box(62, 2, 0, N, 38, N, red);         // east: red, to bleed onto the floor
    box(2, 2, 62, 62, 38, N, white);      // north: white, with a mirror panel
    box(16, 6, 62, 48, 30, 63, mirror);
    box(2, 2, 0, 62, 38, 2, white);       // south: white, with a window
    hole(24, 8, 0, 40, 28, 2);
    box(40, 2, 40, 48, 10, 48, white);    // a white cube
    box(14, 2, 30, 22, 14, 38, satin);    // a semi-glossy block
    box(50, 2, 12, 52, 4, 14, lamp);      // a small lamp

    projv::utils::updateChunkFromBrickMap(chunk, *map);
    std::vector<uint8_t> materialIDs;
    projv::utils::bakeMaterialsFromBrickMap(chunk.geometryData, materialIDs, *map);
    const int32_t blob = projv::internChunkGeometry(scene, chunk, std::move(map));
    if (blob >= 0 && size_t(blob) < scene.geometryPool.size()) projv::utils::setBlobTreeMaterials(scene.geometryPool[blob], std::move(materialIDs));
    scene.chunks.push_back(std::move(chunk));
    scene.looseChunks.push_back(0);
    scene.looseChunkCount = 1;
    return scene;
}

// Standing in the south of the room, looking north along the floor at the mirror, the metal wall to the
// right and the red one to the left.
static void frameMaterialRoom(CameraFraming& framing) {
    framing.position = projv::core::vec3(32.0f, 8.0f, 6.0f) * ROOM_VOXEL;
    framing.yaw = 3.14159265f * 0.5f;
    framing.pitch = -0.2f;
    framing.moveSpeed = 3.0f;
}

// =============================================================================
// Application stages
// =============================================================================

// Chosen by main() before the loop starts; the ECS stage signature takes only the Application.
// The gallery's renderers: which is drawing, and which are built. A renderer is built the first time it
// is picked and kept, so switching back is instant (docs/plans/gallery-ui.md, D2).
struct Gallery {
    std::vector<RendererModule> registry;
    std::vector<std::shared_ptr<projv::ConstructedRenderer>> built;   // nullptr until first picked
    int  active = 0;
    bool switched = false;     // switched this frame: the renderer's histories start over
    bool reframe = false;      // the panel's buttons, done by render()
    bool printPose = false;
    bool restart = false;      // the panel asked for the active renderer's history to start over
    int  stillFrames = 0;      // frames since the camera, the sun or the renderer last changed
};
static std::vector<RendererModule> g_registry;
static int         g_startRenderer = 0;
static std::string g_scenePath;

// Loads, constructs and returns one renderer, as renderer slot `index + 1`.
static std::shared_ptr<projv::ConstructedRenderer> buildRenderer(projv::graphics::RenderInstance& renderInstance,
                                                                 const RendererModule& module, int index) {
    projv::RendererSpecification rendererSpec = projv::graphics::loadRendererSpecification(module.directory);
    // restir-pt's path targets are its quarter-size ones; --restir-gi sets how far below the window they are.
    if (module.id == "restir-pt")
        for (projv::Texture& texture : rendererSpec.resources.textures)
            if (texture.scale == 0.25f && texture.name.rfind("bloom", 0) != 0)   // the bloom's own quarter level is not one
                texture.scale = 1.0f / float(g_restirStride);
    const uint32_t slot = uint32_t(index + 1);
    renderInstance.addRendererSpecification(slot, rendererSpec);

    static const bgfx::ShaderHandle vsh = projv::graphics::loadEngineShader("vs_quad");
    std::shared_ptr<projv::ConstructedRenderer> renderer =
        projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(slot), vsh);

    // The blue-noise LUT the path tracers decorrelate their samples with -- into texture 1, for the renderers
    // that declare it there as a CPU-fed texture. (restir's texture 1 is a render target.)
    bool wantsBlueNoise = false;
    for (const projv::Texture& texture : rendererSpec.resources.textures)
        if (texture.textureID == 1 && texture.origin == projv::CPUBuffer) wantsBlueNoise = true;
    if (wantsBlueNoise) {
        int width, height, channels;
        unsigned char* img = stbi_load("LDR_RGBA_7.png", &width, &height, &channels, 4);
        if (img) {
            projv::graphics::setTextureToData(renderer, 1, img, width, height);
            stbi_image_free(img);
        } else {
            projv::core::warn("Could not load LDR_RGBA_7.png; sampling will be correlated and noisier.");
        }
    }
    projv::core::info("Renderer built: {}", module.id);
    return renderer;
}

// Makes renderer `index` the one drawing, building it if this is its first time.
static void activateRenderer(projv::graphics::RenderInstance& renderInstance, Gallery& gallery, int index) {
    if (!gallery.built[index]) gallery.built[index] = buildRenderer(renderInstance, gallery.registry[index], index);
    renderInstance.setActiveRenderer(gallery.built[index]);
    gallery.active = index;
    gallery.switched = true;
    glfwSetWindowTitle(renderInstance.window, ("ProjectV — " + gallery.registry[index].id).c_str());
}

// The sun's angle up its arc, from the wheel's notches (see render()), and back.
static float sunAngleFromNotches(double notches) { return 0.86f + float(notches) * 0.08f; }
static double notchesFromSunAngle(float angle) { return double(angle - 0.86f) / 0.08; }

// ---- The panel's per-renderer sections ---------------------------------------------------------------------
// Each changes the settings its renderer reads every frame; the one change that invalidates a history
// (pt-reference's bounces) asks for a restart.

static void restirSettings(Gallery&) {
    restir_pt::Settings& r = g_restir;
    static const char* views[restir_pt::VIEWS] = {"the image", "indirect light", "direct light", "ambient occlusion",
                                                  "indirect diffuse", "indirect specular", "light shafts"};
    ImGui::Combo("view", &r.view, views, restir_pt::VIEWS);

    ImGui::SeparatorText("Final render");
    ImGui::Checkbox("final render", &r.post.on);
    ImGui::SetItemTooltip("Off: the raw ReSTIR PT output -- no post, no fog or shafts, no SSAO, and no denoiser");
    ImGui::BeginDisabled(!r.post.on);
    ImGui::Checkbox("vignette", &r.post.vignette);      ImGui::SameLine(160.0f);
    ImGui::Checkbox("colour grade", &r.post.grade);
    ImGui::Checkbox("auto-exposure", &r.post.exposure); ImGui::SameLine(160.0f);
    ImGui::Checkbox("depth of field", &r.post.dof);
    ImGui::Checkbox("bloom", &r.post.bloom);            ImGui::SameLine(160.0f);
    ImGui::Checkbox("motion blur", &r.post.motionBlur);
    ImGui::Checkbox("distance fog", &r.fog);            ImGui::SameLine(160.0f);
    ImGui::Checkbox("aerial haze", &r.air);
    ImGui::Checkbox("light shafts", &r.shafts);
    ImGui::SliderFloat("SSAO", &r.ssao, 0.0f, 1.0f, "%.2f");
    ImGui::Checkbox("far-field SSAO", &r.farAO);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Lighting");
    int bounces = int(r.bounces);
    if (ImGui::SliderInt("path bounces", &bounces, 1, 4)) r.bounces = float(bounces);
    ImGui::Checkbox("soft sun shadow", &r.softShadow);
    ImGui::Checkbox("blurred normals", &g_restirSoftNormals);
    ImGui::BeginDisabled(!g_restirSoftNormals);
    ImGui::SliderFloat("blur width", &g_restirNormalBlur, 0.01f, 0.5f, "%.2f");
    ImGui::EndDisabled();

    ImGui::SeparatorText("ReSTIR");
    ImGui::Checkbox("reuse", &r.reuse);
    ImGui::SetItemTooltip("Off: each GI pixel's one fresh path, denoised");
    ImGui::BeginDisabled(!r.reuse);
    ImGui::Checkbox("temporal", &r.temporal); ImGui::SameLine();
    ImGui::Checkbox("spatial", &r.spatial);   ImGui::SameLine();
    ImGui::Checkbox("visibility", &r.visibility);
    ImGui::EndDisabled();
    bool validation = r.validation > 0.0f;
    if (ImGui::Checkbox("path validation", &validation)) r.validation = validation ? restir_pt::VALIDATION : 0.0f;

    ImGui::SeparatorText("Denoise and reconstruction");
    ImGui::BeginDisabled(!r.post.on);
    bool atrous = r.atrous > 0.0f;
    if (ImGui::Checkbox("a-trous denoise", &atrous)) r.atrous = atrous ? 1.0f : 0.0f;
    bool accumulate = !r.raw;
    if (ImGui::Checkbox("accumulate (GI and image history)", &accumulate)) r.raw = !accumulate;
    ImGui::EndDisabled();
    bool reconstruct = !r.bypass;
    if (ImGui::Checkbox("reconstruct to full resolution", &reconstruct)) r.bypass = !reconstruct;
    ImGui::SetItemTooltip("Off: the half-resolution G-buffer point-sampled");
    ImGui::Checkbox("16-phase jitter", &r.halton16);
    ImGui::TextDisabled("GI for one pixel in %dx%d (--restir-gi 2 or 4, at start)", g_restirStride, g_restirStride);
}

static void referenceSettings(Gallery& gallery) {
    if (ImGui::SliderInt("bounces", &g_referenceBounces, 1, 6)) gallery.restart = true;
    ImGui::SetItemTooltip("Two matches restir-pt's paths, which is what makes it restir-pt's reference");
    ImGui::Text("%d frames accumulated", gallery.stillFrames);
    ImGui::SameLine();
    if (ImGui::Button("Restart")) gallery.restart = true;
    ImGui::TextDisabled("Accumulates while the camera and the sun are still.");
}

static void fastSettings(Gallery&) {
    ImGui::SliderFloat("AO radius", &g_fast.aoRadius, 0.5f, 16.0f, "%.1f");
    ImGui::SliderFloat("AO intensity", &g_fast.aoIntensity, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("AO power", &g_fast.aoPower, 0.5f, 4.0f, "%.2f");
    ImGui::Checkbox("AO denoise blur", &g_fast.blur);
    if (ImGui::Button("Defaults")) g_fast = FastSettings{};
}

// The Gallery panel: the renderer, its settings, and the view.
static void galleryPanel(projv::Application& app) {
    auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
    Gallery& gallery = app.world.ctx().get<Gallery>();
    ImGui::SeparatorText("Renderer");
    for (int i = 0; i < int(gallery.registry.size()); i++) {
        if (ImGui::RadioButton(gallery.registry[i].id.c_str(), gallery.active == i) && gallery.active != i)
            activateRenderer(renderInstance, gallery, i);
        if (i + 1 < int(gallery.registry.size())) ImGui::SameLine();
    }
    ImGui::TextWrapped("%s", gallery.registry[gallery.active].description.c_str());

    const std::string& id = gallery.registry[gallery.active].id;
    if (ImGui::CollapsingHeader((id + " settings").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (id == "restir-pt") restirSettings(gallery);
        else if (id == "pt-reference") referenceSettings(gallery);
        else if (id == "fast") fastSettings(gallery);
    }

    ImGui::SeparatorText("View");
    float sunDegrees = sunAngleFromNotches(g_sunScrollAccum) * 180.0f / 3.14159265f;
    if (ImGui::SliderFloat("sun", &sunDegrees, -30.0f, 210.0f, "%.1f deg"))
        g_sunScrollAccum = notchesFromSunAngle(sunDegrees * 3.14159265f / 180.0f);
    ImGui::SetItemTooltip("The sun's height up its arc (the scroll wheel, over the scene)");
    if (ImGui::Button("Re-frame (H)")) gallery.reframe = true;
    ImGui::SameLine();
    if (ImGui::Button("Print pose (C)")) gallery.printPose = true;
    ImGui::TextDisabled("Esc frees the cursor for the panel; a click on the scene takes it back.");
}

// Startup: create the window, load the scene and the starting renderer, upload all GPU resources.
void startup(projv::Application& app) {

    projv::graphics::RenderInstance& renderInstance = app.world.ctx().emplace<projv::graphics::RenderInstance>();
    // Unattended runs are measurements: a fixed-size window, so two runs render the same pixels
    // whatever the desktop's layout is doing.
    const bool unattendedRun = g_measure.bench || !g_measure.capture.empty();
    renderInstance.resizable = !unattendedRun;
    renderInstance.initialize(unattendedRun ? 1280 : 1920, unattendedRun ? 720 : 1080, "ProjectV");

    // Input through the engine, and the dev layer over it: the panel drives the gallery (D4, D5). An
    // unattended run hides it and leaves the cursor alone, so captures and measurements are unchanged.
    projv::graphics::installPlatform(app, renderInstance);
    // Unattended: hidden unless PROJV_DEV_UI_SHOWN=1 asks (to capture the panel itself).
    projv::ui::installUi(app, renderInstance, {.devShown = unattendedRun ? std::nullopt : std::optional<bool>(true)});
    projv::ui::dev::addPanel(app, "Gallery", galleryPanel);

    projv::Scene& scene     = app.world.ctx().emplace<projv::Scene>();
    float& cameraPhi         = app.world.ctx().emplace<float>();
    projv::GPUData& gpuData  = app.world.ctx().emplace<projv::GPUData>();
    CameraFraming& framing = app.world.ctx().emplace<CameraFraming>();
    Gallery& gallery = app.world.ctx().emplace<Gallery>();
    gallery.registry = g_registry;
    gallery.built.resize(g_registry.size());

    // Eager load: loads all scene geometry up front.
    const bool materialRoom = g_scenePath == "material-room/";
    projv::core::info("Loading scene: {}", g_scenePath);
    scene = materialRoom ? buildMaterialRoom() : projv::utils::loadComposeFromDisk(g_scenePath);

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
    if (materialRoom) frameMaterialRoom(framing);
    cameraPhi = framing.yaw;

    activateRenderer(renderInstance, gallery, g_startRenderer);
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
    Gallery& gallery                 = app.world.ctx().get<Gallery>();
    const projv::Input& input        = app.world.ctx().get<projv::Input>();
    CameraFraming& framing           = app.world.ctx().get<CameraFraming>();

    // Seeded from the automatic framing on the first frame, so --scene works at any scale.
    static projv::core::vec3 cameraPosition;
    static bool cameraInitialized = false;

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

    // The cursor: Esc frees it (for the panel), a click on the scene captures it again -- a click on the
    // panel never arrives here, the UI host keeps it (docs/concepts/ui.md). Nothing in an unattended run.
    if (!unattended) {
        if (input.cursorCaptured && input.pressed(projv::Key::Escape)) projv::graphics::setCursorCaptured(app, renderInstance, false);
        else if (!input.cursorCaptured && input.pressed(projv::MouseButton::Left)) projv::graphics::setCursorCaptured(app, renderInstance, true);
    }

    // Mouse look, while the cursor is captured.
    const float mouseSensitivity = 0.0025f;
    if (input.cursorCaptured && (input.cursorDelta.x != 0.0f || input.cursorDelta.y != 0.0f)) {
        cameraPhi   += input.cursorDelta.x * mouseSensitivity;  // right -> look right
        cameraPitch -= input.cursorDelta.y * mouseSensitivity;  // up    -> look up
        // Clamp pitch just shy of straight up/down to avoid gimbal flip.
        const float pitchLimit = 1.55f; // ~89 degrees
        cameraPitch = std::clamp(cameraPitch, -pitchLimit, pitchLimit);
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
    const float moveSeconds = std::min(app.time().unscaledDelta, 0.1f);
    const bool fast = input.down(projv::Key::LeftShift);
    const float moveSpeed = framing.moveSpeed * moveSeconds * (fast ? 4.0f : 1.0f);

    // Horizontal movement (W/A/S/D). None of it in an unattended run: a key pressed while the window
    // has focus would move the camera between two captures that are meant to compare.
    if (!unattended) {
        if (input.down(projv::Key::W)) { cameraPosition += forwardDirection * moveSpeed; cameraMoved = true; }
        if (input.down(projv::Key::S)) { cameraPosition -= forwardDirection * moveSpeed; cameraMoved = true; }
        if (input.down(projv::Key::A)) {
            float leftPhi = cameraPhi + 3.14 / 2;
            projv::core::vec3 leftDirection = { projv::core::cos(leftPhi), 0, projv::core::sin(leftPhi) };
            cameraPosition -= leftDirection * moveSpeed;
            cameraMoved = true;
        }
        if (input.down(projv::Key::D)) {
            float rightPhi = cameraPhi - 3.14 / 2;
            projv::core::vec3 rightDirection = { projv::core::cos(rightPhi), 0, projv::core::sin(rightPhi) };
            cameraPosition -= rightDirection * moveSpeed;
            cameraMoved = true;
        }

        // Vertical movement (R/F).
        if (input.down(projv::Key::R)) { cameraPosition[1] += moveSpeed; cameraMoved = true; }
        if (input.down(projv::Key::F)) { cameraPosition[1] -= moveSpeed; cameraMoved = true; }
    }

    // GALLERY_SWITCH_AT=n:id,m:id switches renderer at those frames, as the panel does: a switch unattended.
    static const std::vector<std::pair<int, std::string>> switches = [] {
        std::vector<std::pair<int, std::string>> list;
        if (const char* v = std::getenv("GALLERY_SWITCH_AT")) {
            std::string all = v;
            for (size_t at = 0; at < all.size();) {
                size_t comma = all.find(',', at);
                std::string item = all.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                size_t colon = item.find(':');
                if (colon != std::string::npos) list.emplace_back(std::atoi(item.c_str()), item.substr(colon + 1));
                at = comma == std::string::npos ? all.size() : comma + 1;
            }
        }
        return list;
    }();
    for (const auto& [frame, id] : switches)
        if (frame == app.frameCount) {
            const int index = findRenderer(gallery.registry, id);
            if (index >= 0 && index != gallery.active) activateRenderer(renderInstance, gallery, index);
        }

    // GALLERY_STEP_AT=n sidesteps the camera half a second's walk to the right at frame n: with --capture and
    // GALLERY_CAPTURE_AT, how what that uncovers settles (disocclusion).
    static const int stepAt = std::getenv("GALLERY_STEP_AT") ? std::atoi(std::getenv("GALLERY_STEP_AT")) : -1;
    if (app.frameCount == stepAt) {
        float rightPhi = cameraPhi - 3.14159265f / 2.0f;
        cameraPosition -= projv::core::vec3(projv::core::cos(rightPhi), 0.0f, projv::core::sin(rightPhi)) * (framing.moveSpeed * 0.5f);
        cameraMoved = true;
    }

    // C prints where the camera stands as the arguments that put it back there (--look, and the sun's
    // GALLERY_SUN), so a view found by hand can be captured unattended.
    {
        if (input.pressed(projv::Key::C) || gallery.printPose) {
            gallery.printPose = false;
            projv::core::vec3 at = cameraPosition + cameraDirection * 10.0f;
            projv::core::info("GALLERY_SUN={} --look {:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f}", g_sunScrollAccum,
                              cameraPosition.x, cameraPosition.y, cameraPosition.z, at.x, at.y, at.z);
        }
    }

    // H re-frames on the scene, for when navigation has left the subject behind.
    if (input.pressed(projv::Key::H) || gallery.reframe) {
        gallery.reframe = false;
        cameraPosition = framing.position;
        cameraPhi      = framing.yaw;
        cameraPitch    = framing.pitch;
        cameraMoved    = true;
    }

    // Sun direction from the scroll wheel: the scroll angle traces a FULL great circle (rise ->
    // overhead -> set on the far side -> below the horizon / night -> back), orbiting all the way
    // around in a fixed vertical plane. The angle is unbounded (no clamp) so it never stops; sunDir
    // = cos(theta)*azimuth + sin(theta)*up is unit-length for a unit azimuth, so no renormalize. A
    // sun change counts as "moved" so the temporal history refreshes to the new lighting.
    // GALLERY_SUN=n starts the sun n scroll notches from its default (C prints it).
    static double prevSunScroll = [] {
        if (const char* v = std::getenv("GALLERY_SUN")) g_sunScrollAccum = std::atof(v);
        return g_sunScrollAccum;
    }();
    if (!unattended) g_sunScrollAccum += input.scroll.y;   // over the panel, the UI host keeps the wheel
    // GALLERY_SUN_STEP_AT=n lowers the sun three notches (~14 degrees) at frame n: with --capture and
    // GALLERY_CAPTURE_AT, how fast the image follows a change of light.
    static const int sunStepAt = std::getenv("GALLERY_SUN_STEP_AT") ? std::atoi(std::getenv("GALLERY_SUN_STEP_AT")) : -1;
    if (app.frameCount == sunStepAt) g_sunScrollAccum -= 3.0;
    float sunAngle = 0.86f + (float)g_sunScrollAccum * 0.08f;  // radians; ~0.86 default, ~4.6deg/notch
    const float sunAzX = -0.6402f, sunAzZ = 0.7682f;          // = normalize(vec2(-0.5, 0.6))
    float ca = projv::core::cos(sunAngle), sa = projv::core::sin(sunAngle);
    projv::core::vec3 sunDirection = { ca * sunAzX, sa, ca * sunAzZ };
    bool sunMoved = (g_sunScrollAccum != prevSunScroll);
    prevSunScroll = g_sunScrollAccum;
    // Switching to a renderer is a move for it: its history is from wherever it last drew. So is the panel's
    // restart.
    cameraMoved = cameraMoved || sunMoved || gallery.switched || gallery.restart;
    gallery.restart = false;

    // Track the frame the camera last moved on so the shader can reset accumulation.
    static int frameCameraLastMovedOn = 0;
    if (cameraMoved) frameCameraLastMovedOn = app.frameCount;
    gallery.stillFrames = app.frameCount - frameCameraLastMovedOn;

    FrameContext ctx;
    ctx.cameraPosition         = cameraPosition;
    ctx.cameraDirection        = cameraDirection;
    ctx.cameraMoved            = cameraMoved;
    ctx.sunMoved               = sunMoved;
    ctx.switched               = gallery.switched;
    ctx.frameCount             = app.frameCount;
    ctx.frameCameraLastMovedOn = frameCameraLastMovedOn;
    ctx.windowResolution       = renderInstance.getWindowResolution();
    ctx.sunDirection           = sunDirection;
    ctx.sunAngle               = sunAngle;
    ctx.world                  = &app.world;

    // The active renderer as of now: the switch hook above may have changed it this frame.
    const RendererModule& selectedRenderer = gallery.registry[gallery.active];
    selectedRenderer.uploadUniforms(renderInstance.getActiveRenderer(), ctx);
    gallery.switched = false;

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
            const projv::RendererSpecification& spec = renderInstance.getRendererSpecification(uint32_t(gallery.active + 1));
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
    // --capture: three frames in a row, for what changes between them with nothing moving -- or the frames
    // GALLERY_CAPTURE_AT lists ("4,8,16,300"), for how fast an image settles after the first frame.
    if (!g_measure.capture.empty()) {
        static const std::vector<int> frames = [] {
            std::vector<int> list;
            if (const char* v = std::getenv("GALLERY_CAPTURE_AT"))
                for (const char* c = v; *c; ) { list.push_back(std::atoi(c)); while (*c && *c != ',') ++c; if (*c) ++c; }
            if (list.empty()) list = {300, 301, 302};
            return list;
        }();
        if (std::find(frames.begin(), frames.end(), app.frameCount) != frames.end())
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, fmt::format("{}-{}", g_measure.capture, app.frameCount).c_str());
        if (app.frameCount > *std::max_element(frames.begin(), frames.end()) + 4) app.closeRequested = true;
    }
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
    g_registry = registry;
    g_startRenderer = selected;
    projv::ui::setCommandLine(argc, argv);
    readRestirEnvironment();

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
