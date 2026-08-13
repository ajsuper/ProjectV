// ProjectV AdvancedRenderer
//
// A real-time world-space radiance-cascade global illumination renderer. It loads a Compose scene
// from disk and renders it with one ten-pass renderer -- there is no menu and no second pipeline,
// which is the difference between this example and PathTracer: that one exists to compare six
// approaches, this one exists to be the good approach and to be readable while it is.
//
// ---------------------------------------------------------------------------------------------
// WHAT THE RENDERER DOES  (advancedRenderer/render.json is the pass order)
// ---------------------------------------------------------------------------------------------
//   1  gbuffer      one primary ray per pixel -> position, normal, albedo, and the CRISP half of
//                   the image (soft-shadowed direct sun + voxel emission), kept out of every
//                   temporal filter downstream
//   2-5 cascade3..0 the radiance cascades, coarsest first. Each texel casts ONE real voxel ray
//                   over its cascade's interval and merges what it did not hit with the cascade
//                   above, so a probe's finest directions carry light gathered at every scale
//   6  resolve      cascade 0 -> one indirect irradiance value per pixel (cosine-weighted)
//   7  accumulate   temporal mean of that indirect term alone, reprojected while the camera moves
//   8  compose      direct + albedo * indirect + a rough specular read from the same cascade
//   9  taa          resolves the primary ray's sub-pixel jitter into anti-aliased edges
//  10  display      exposure, ACES, gamma, contrast, saturation -> the window
//
// The reason the indirect term is separated out and filtered on its own is that lighting is low
// frequency and albedo is not. Blurring a lit image blurs the voxel colours with it; blurring the
// light and multiplying a sharp albedo back in afterwards does not.
//
// ---------------------------------------------------------------------------------------------
// Controls:
//   W/S       -- move forward / backward
//   A/D       -- strafe left / right
//   R/F       -- move up / down
//   Shift     -- move faster (hold)
//   Mouse     -- look around (cursor is captured; Esc releases it, left-click re-captures)
//   Scroll    -- raise / lower the sun (a full day-night cycle)
//
// Usage:
//   ./advanced_renderer [scene directory]
// Any folder holding a compose.json works; the camera frames whatever it finds. See
// DEFAULT_SCENE_PATH below for what it opens with none given.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

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

// This example ships no scene of its own; it opens one of the scene library the previewer and the
// editor share. Nothing here is specific to this example -- any folder with a compose.json in it can
// be passed on the command line instead.
//
// Several scenes in that library are older than the current .data container version (2) and load as
// zero chunks, saying so in the log. This one is current. See the README.
static const char* DEFAULT_SCENE_PATH = "../ScenePreviewer/scenes/Cave/";

// The renderer's own files. Both paths inside resources.json are relative to the working directory,
// so this example is run from its own folder.
static const char* RENDERER_DIRECTORY = "./advancedRenderer/";
static const char* VERTEX_SHADER_PATH = "./advancedRenderer/cascadeShaders/vs_quad.bin";

// Must match FOV in the shaders (pjv_cascade_common.sc and gbuffer.frag). The framing below places
// the camera by it, and the CPU has no other way to learn it -- the projection lives in the shader.
static const float CAMERA_VERTICAL_FOV_DEGREES = 60.0f;

// ---------------------------------------------------------------------------------------------
// The light rig, as the shaders receive it
// ---------------------------------------------------------------------------------------------
// These are the scene editor's Render-tab defaults, value for value, because sharedShaders/
// pjv_sun_sky.sc is its light rig curve for curve. A scene set up in the editor and opened here
// should be lit the same way; if these drift, that stops being true.

// The angular RADIUS of the sun disk, in degrees -- the softness of every shadow in the image, and
// the single control with the most say over how a frame reads. 0.27 is the real sun (crisp shadow
// edges), a few degrees is a hazy day, seventeen is overcast.
static const float SUN_ANGULAR_RADIUS_DEGREES = 3.0f;
static const float SUN_INTENSITY = 8.0f;
static const float SKY_INTENSITY = 1.0f;

// Mouse scroll drives the sun's elevation (see render()). GLFW scroll callbacks are plain C
// function pointers, so the accumulated wheel offset lives at file scope.
static double g_sunScrollAccum = 0.0;
static void sunScrollCallback(GLFWwindow* /*window*/, double /*xoffset*/, double yoffset) {
    g_sunScrollAccum += yoffset;
}

// =============================================================================
// Camera framing
// =============================================================================

// Where the camera starts and how fast it flies, both derived from the scene rather than hardcoded.
// This example takes its scene on the command line, so a fixed position would put the camera inside
// one scene's wall and a kilometre from the next one.
struct CameraFraming {
    projv::core::vec3 position;
    float yaw;
    float pitch;
    float moveSpeed;   // World units per frame.
};

// The world-space bounding box of every live chunk. A chunk header carries its world position
// (minimum corner) and its scale, which is all a bounding box needs -- no geometry is touched, so
// this stays instant on a large scene.
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
// three-quarter angle -- the view that shows the most of an unfamiliar scene. Same construction the
// scene editor frames with, so opening a scene in both puts you in roughly the same place.
static CameraFraming frameScene(const projv::Scene& scene) {
    using namespace projv::core;

    CameraFraming framing;
    framing.yaw = 3.14159265f * 0.25f;   // Looking along +X/+Z, so the camera sits on the -X/-Z side.
    framing.pitch = -0.35f;              // Slightly above, looking down onto the subject.

    vec3 boundsMin, boundsMax;
    if (!measureSceneBounds(scene, boundsMin, boundsMax)) {
        warn("Scene has no live chunks -- there will be nothing but sky to render.");
        framing.position = vec3(0.0f, 0.0f, -100.0f);
        framing.moveSpeed = 1.0f;
        return framing;
    }

    vec3 center = (boundsMin + boundsMax) * 0.5f;
    float radius = length(boundsMax - boundsMin) * 0.5f;
    if (radius <= 0.0f) radius = 1.0f;

    info("Scene bounds: ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f})",
         boundsMin.x, boundsMin.y, boundsMin.z, boundsMax.x, boundsMax.y, boundsMax.z);

    // Far enough back that the bounding sphere fits the shaders' vertical FOV, with a margin so the
    // subject is not jammed against the frame edge.
    const float fovRadians = CAMERA_VERTICAL_FOV_DEGREES * 3.14159265f / 180.0f;
    float distance = (radius / std::tan(fovRadians * 0.5f)) * 1.25f;

    vec3 forward = {
        std::cos(framing.pitch) * std::cos(framing.yaw),
        std::sin(framing.pitch),
        std::cos(framing.pitch) * std::sin(framing.yaw)
    };
    framing.position = center - forward * distance;

    // Crossing the scene should take a few seconds at 60 fps regardless of how big it is. Voxels are
    // one world unit, so a 512-voxel model and a 32-voxel one otherwise fly at wildly different
    // apparent speeds.
    framing.moveSpeed = std::max(radius * 0.005f, 0.01f);

    // ADVANCED_CAMERA="x,y,z,yaw,pitch" pins the camera somewhere specific, overriding the framing
    // above. This exists for the same reason the scene editor's EDITOR_START_MODE does: a
    // screenshot cannot fly, and a frame time is only comparable between two builds if both
    // measured the same view. Paired with ADVANCED_LOCK_CAMERA below.
    if (const char* overrideCamera = std::getenv("ADVANCED_CAMERA")) {
        float x, y, z, yaw, pitch;
        if (std::sscanf(overrideCamera, "%f,%f,%f,%f,%f", &x, &y, &z, &yaw, &pitch) == 5) {
            framing.position = vec3(x, y, z);
            framing.yaw = yaw;
            framing.pitch = pitch;
            info("ADVANCED_CAMERA: ({:.1f}, {:.1f}, {:.1f}) yaw {:.3f} pitch {:.3f}", x, y, z, yaw, pitch);
        } else {
            warn("ADVANCED_CAMERA is not \"x,y,z,yaw,pitch\" -- ignoring it: {}", overrideCamera);
        }
    }
    return framing;
}

// =============================================================================
// Per-frame state
// =============================================================================

// Everything render() carries between frames. A struct rather than function-local statics so that
// the previous camera and the current one cannot get out of step -- the temporal passes reproject
// through the previous camera, and a stale value there is a smear rather than an error.
struct FrameState {
    projv::core::vec3 cameraPosition;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float moveSpeed = 1.0f;

    projv::core::vec3 prevCameraPosition;
    projv::core::vec3 prevCameraDirection = {0.0f, 0.0f, 1.0f};
    bool prevCameraInitialized = false;

    // The frame the camera (or the sun) last moved on. The accumulate and TAA passes switch between
    // their long still-camera mean and their short reprojecting one on this.
    int frameLastMovedOn = 0;

    bool mouseCaptured = true;
    bool mouseInitialized = false;
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;

    double prevSunScroll = 0.0;
};

// =============================================================================
// Application stages
// =============================================================================

// The scene path, parked here by main() because the ECS stages take only the Application.
struct SceneRequest {
    std::string path = DEFAULT_SCENE_PATH;
};

void startup(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance =
        projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(1920, 1080, "ProjectV Advanced Renderer");

    // Capture the cursor so mouse motion drives the camera (FPS-style mouse look).
    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetScrollCallback(renderInstance.window, sunScrollCallback);

    // createGlobalResource returns the existing one if main() already made it, so a path given on
    // the command line survives to here and the default fills in when none was.
    SceneRequest& request = projv::core::createGlobalResource<SceneRequest>(app.world);

    projv::Scene& scene = projv::core::createGlobalResource<projv::Scene>(app.world);
    projv::GPUData& gpuData = projv::core::createGlobalResource<projv::GPUData>(app.world);
    FrameState& state = projv::core::createGlobalResource<FrameState>(app.world);

    projv::core::info("Loading scene: {}", request.path);
    scene = projv::utils::loadComposeFromDisk(request.path);

    CameraFraming framing = frameScene(scene);
    state.cameraPosition = framing.position;
    state.prevCameraPosition = framing.position;
    state.yaw = framing.yaw;
    state.pitch = framing.pitch;
    state.moveSpeed = framing.moveSpeed;

    projv::RendererSpecification rendererSpec =
        projv::graphics::loadRendererSpecification(RENDERER_DIRECTORY);
    renderInstance.addRendererSpecification(1, rendererSpec);

    bgfx::ShaderHandle vertexShader = projv::graphics::loadShader(VERTEX_SHADER_PATH);
    std::shared_ptr<projv::ConstructedRenderer> constructedRenderer =
        projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1),
                                                        vertexShader);

    renderInstance.setActiveRenderer(constructedRenderer);
    gpuData = projv::graphics::createTexturesForScene(scene);
}

// Frame timing profiler. Compiled out entirely unless PROJV_ENABLE_PERF is defined.
void update(projv::Application& /*app*/) {
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

// Mouse look. The cursor is captured (GLFW_CURSOR_DISABLED), so its absolute position is read each
// frame and the delta applied to yaw/pitch. Returns whether the view turned.
static bool updateMouseLook(projv::graphics::RenderInstance& renderInstance, FrameState& state) {
    // Cursor capture toggle: Esc releases the cursor so it can leave the window, left-click
    // re-captures it. Only look around while captured.
    if (state.mouseCaptured && glfwGetKey(renderInstance.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        state.mouseCaptured = false;
    } else if (!state.mouseCaptured &&
               glfwGetMouseButton(renderInstance.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        state.mouseCaptured = true;
    }

    double mouseX, mouseY;
    glfwGetCursorPos(renderInstance.window, &mouseX, &mouseY);
    // Reset the reference point whenever tracking (re)starts, so re-capturing after a release does
    // not apply one huge jump.
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

    const float mouseSensitivity = 0.0025f;
    state.yaw   += float(deltaX) * mouseSensitivity;   // right -> look right
    state.pitch -= float(deltaY) * mouseSensitivity;   // up    -> look up
    // Just shy of straight up/down, to avoid the gimbal flip at the pole.
    const float pitchLimit = 1.55f;   // ~89 degrees
    state.pitch = std::max(-pitchLimit, std::min(pitchLimit, state.pitch));
    return true;
}

// W/A/S/D/R/F flight. Forward comes from yaw alone so W/S flies level regardless of where the
// camera is looking. Returns whether the camera moved.
static bool updateMovement(projv::graphics::RenderInstance& renderInstance, FrameState& state) {
    float speed = state.moveSpeed;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) speed *= 5.0f;

    projv::core::vec3 forward = {std::cos(state.yaw), 0.0f, std::sin(state.yaw)};
    projv::core::vec3 right = {std::cos(state.yaw - 1.5707963f), 0.0f,
                               std::sin(state.yaw - 1.5707963f)};

    bool moved = false;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_W)) { state.cameraPosition += forward * speed; moved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_S)) { state.cameraPosition -= forward * speed; moved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_D)) { state.cameraPosition += right * speed;   moved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_A)) { state.cameraPosition -= right * speed;   moved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_R)) { state.cameraPosition.y += speed;         moved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_F)) { state.cameraPosition.y -= speed;         moved = true; }
    return moved;
}

// The sun, from the scroll wheel. The angle traces a FULL great circle in a fixed vertical plane --
// rise, overhead, set on the far side, below the horizon (night), back -- and is deliberately
// unbounded so it never stops. sunDir = cos(theta) * azimuth + sin(theta) * up is unit length for a
// unit azimuth, so there is nothing to renormalize.
static projv::core::vec4 sunFromScroll() {
    float angle = 0.86f + float(g_sunScrollAccum) * 0.08f;   // radians; ~4.6 degrees per notch
    const float azimuthX = -0.6402f, azimuthZ = 0.7682f;     // = normalize(vec2(-0.5, 0.6))
    float c = std::cos(angle), s = std::sin(angle);
    // w is the disk's angular radius in radians, which is what makes the shadows soft. Floored well
    // above zero in the shader: a zero-radius sun has no solid angle to sample.
    const float degreesToRadians = 3.14159265f / 180.0f;
    return {c * azimuthX, s, c * azimuthZ, SUN_ANGULAR_RADIUS_DEGREES * degreesToRadians};
}

// Everything the ten passes read. Every uniform declared in resources.json is set here: a uniform
// the renderer declares but never uploads holds whatever bgfx last had in it, which is a black sun
// or a black sky rather than an error.
static void uploadFrameUniforms(const std::shared_ptr<projv::ConstructedRenderer>& renderer,
                                const FrameState& state, projv::core::vec3 cameraDirection,
                                projv::core::vec4 sunDirection, bool cameraMoved, int frame) {
    using projv::graphics::setUniformToValue;

    // Copied into locals rather than passed straight out of `state`. setUniformToValue deduces the
    // uniform's type from its argument, and a member of a const struct arrives as `const vec3&`,
    // which the deduction does not recognise -- the value is then silently never uploaded and the
    // shader reads whatever bgfx last had under that name. The engine says as much when it happens,
    // but the fix belongs here.
    projv::core::vec3 cameraPosition = state.cameraPosition;
    projv::core::vec3 prevCameraPosition = state.prevCameraPosition;
    projv::core::vec3 prevCameraDirection = state.prevCameraDirection;

    setUniformToValue(renderer, "cameraPos", cameraPosition);
    setUniformToValue(renderer, "cameraDir", cameraDirection);
    setUniformToValue(renderer, "prevCameraPos", prevCameraPosition);
    setUniformToValue(renderer, "prevCameraDir", prevCameraDirection);

    // x = frame index (drives the per-frame jitter), y = did anything move this frame (which
    // temporal branch to take), z = the frame it last moved on, w = spare.
    projv::core::vec4 frameCount = {float(frame), cameraMoved ? 1.0f : 0.0f,
                                    float(state.frameLastMovedOn), 0.0f};
    setUniformToValue(renderer, "frameCount", frameCount);

    setUniformToValue(renderer, "sunDir", sunDirection);
    // x = spare (the scene editor puts its bounce count here; this renderer's bounce depth is the
    // cascade structure, not a count), y = sun intensity, z = sky intensity, w = spare.
    projv::core::vec4 renderParams = {0.0f, SUN_INTENSITY, SKY_INTENSITY, 0.0f};
    setUniformToValue(renderer, "renderParams", renderParams);
}

void render(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance =
        projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    projv::GPUData& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    FrameState& state = projv::core::getGlobalResource<FrameState>(app.world);

    // ADVANCED_LOCK_CAMERA=1 ignores input entirely. Needed for any measurement or capture: the
    // cursor is captured, so a pointer nudge from the window manager -- or from the screenshot tool
    // itself -- turns the camera mid-run, and two builds then quietly measure different views.
    static const bool cameraLocked = std::getenv("ADVANCED_LOCK_CAMERA") != nullptr;

    bool cameraMoved = false;
    if (!cameraLocked) {
        cameraMoved = updateMouseLook(renderInstance, state);
        cameraMoved = updateMovement(renderInstance, state) || cameraMoved;
    }


    projv::core::vec3 cameraDirection = {
        std::cos(state.pitch) * std::cos(state.yaw),
        std::sin(state.pitch),
        std::cos(state.pitch) * std::sin(state.yaw)
    };

    // On the first frame there is no history, so the previous camera is made equal to the current
    // one (identity reprojection) rather than left at whatever it was initialized to.
    if (!state.prevCameraInitialized) {
        state.prevCameraPosition = state.cameraPosition;
        state.prevCameraDirection = cameraDirection;
        state.prevCameraInitialized = true;
    }

    // A sun change counts as movement: it invalidates every accumulated frame just as thoroughly as
    // a camera move does, and without this the new lighting fades in over the history's length.
    projv::core::vec4 sunDirection = sunFromScroll();
    if (g_sunScrollAccum != state.prevSunScroll) cameraMoved = true;
    state.prevSunScroll = g_sunScrollAccum;

    if (cameraMoved) state.frameLastMovedOn = app.frameCount;

    uploadFrameUniforms(renderInstance.getActiveRenderer(), state, cameraDirection, sunDirection,
                        cameraMoved, app.frameCount);

    // Resizes the render targets to the window, runs the ten passes at their own targets' sizes, and
    // presents. This driver draws straight into the window, so the back buffer and the render
    // resolution are the same thing and the engine's own entry point is the right one -- the scene
    // editor calls the two halves of this separately because its targets follow a panel instead.
    projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(),
                                               &gpuData);

    state.prevCameraPosition = state.cameraPosition;
    state.prevCameraDirection = cameraDirection;
}

int main(int argc, char** argv) {
    projv::Application app = projv::core::createApp();
    projv::core::assignSystemStage(app, projv::SystemStage::Startup, startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update, update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render, render);

    // The scene given on the command line has to survive until startup runs, and the ECS stages take
    // only the Application -- so it is parked in a global resource that startup reads.
    if (argc > 1) {
        SceneRequest& request = projv::core::createGlobalResource<SceneRequest>(app.world);
        request.path = argv[1];
        if (!request.path.empty() && request.path.back() != '/') request.path += '/';
    }

    projv::core::runApplication(app);
    return 0;
}
