// ProjectV Hello Voxel
//
// The smallest program that opens a window and draws voxels. Everything is built in memory, so
// there is no scene to download and nothing to voxelize first: run it and a coloured 64^3 shape
// appears.
//
// It exists to show the startup sequence once, in one place, at a size you can hold in your head.
// Every other example is this plus its own subject.
//
//   1. create the window and bring bgfx up      RenderInstance::initialize
//   2. put the scene and its GPU mirror in the world   world.ctx().emplace
//      and connect the window to the application       installPlatform
//   3. get some voxels                           buildScene() below, or loadComposeFromDisk
//   4. describe the renderer                     loadRendererSpecification (render.json + resources.json)
//   5. load the vertex shader                    loadShader
//   6. turn the description into GPU objects     constructRendererSpecification
//   7. upload the voxels                         createTexturesForScene
//
// Then each frame: move the camera from Input and Time (Update), set the uniforms the shaders read,
// and call renderConstructedRenderer (Render).
//
// Two things worth copying into every application of your own:
//
//   * It exits cleanly, through the event system. The platform system turns the window's close
//     button into a CloseRequested event, and the Application ends when it hears one. A tool with
//     unsaved work would set Application::closeOnRequest to false and handle the event itself --
//     raise a prompt, then close -- which it could not do if the engine closed the window for it.
//
//   * It finds its assets from its own location rather than the working directory, using
//     projv::core::executableDirectory(). Run it from anywhere and the renderer folder still
//     resolves.
//
// Controls:
//   W/S/A/D  — move
//   R/F      — up / down
//   Mouse    — look (Esc releases the cursor, left-click re-captures)
//   Esc      — release the cursor; close the window to quit

#include <cmath>
#include <filesystem>
#include <string>

#include "core/application.h"
#include "core/log.h"
#include "core/math.h"
#include "core/paths.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/input.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "utils/material.h"
#include "utils/voxel_management.h"

namespace {

// The voxel grid is CHUNK_RESOLUTION on a side. It must be a power of four: the tree64 structure
// the engine traverses branches four ways per axis per level.
constexpr int   CHUNK_RESOLUTION = 64;
constexpr float VOXEL_SCALE      = 0.5f;   // World units per voxel.

// Where the renderer folder lives, relative to the executable. The build stages it there.
std::filesystem::path assetDirectory() {
    return projv::core::executableDirectory();
}

// -----------------------------------------------------------------------------------------
// Building a scene in memory
// -----------------------------------------------------------------------------------------
//
// A scene is components; a component owns geometry and a material palette. The shortest route to
// something on screen is one "loose" chunk -- a single volume placed directly in the world, as
// opposed to a grid of them.
//
// Geometry is authored through a *brick map*, a plain 3D array of material slots that is easy to
// write into. updateChunkFromBrickMap compresses it to the tree64 the GPU traverses, so nothing
// here has to know that format.
projv::Scene buildScene() {
    projv::Scene scene;

    projv::ChunkHeader header;
    header.chunkID    = 1;
    header.position   = projv::core::vec3(0.0f);
    header.scale      = CHUNK_RESOLUTION * VOXEL_SCALE;  // World size of the whole chunk.
    header.voxelScale = VOXEL_SCALE;
    header.resolution = CHUNK_RESOLUTION;
    header.rotation   = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);

    projv::Chunk chunk;
    chunk.header          = header;
    chunk.requestedLOD    = 0;
    chunk.alive           = true;
    chunk.componentHandle = 0;

    // The component has to exist before the palette does: material slots are per-component, and
    // internMaterial writes into this record.
    scene.components.push_back(projv::ComponentRecord{
        projv::ComponentKind::Chunk,
        0,                      // chunkHandle -- the chunk pushed below
        -1,                     // gridIndex: not a grid
        "internal/hello",       // sourcePath, for diagnostics
        false,                  // externalSource: nothing on disk backs this
        {},                     // editQueue
        -1                      // dataRefID
    });
    projv::ComponentRecord& component = scene.components[0];

    // Four colours. internMaterial dedupes and hands back the slot the voxel data stores, so a
    // palette is built by asking for colours rather than by managing indices.
    const uint8_t red   = projv::utils::internMaterial(scene, component, "red",
                              projv::packColor({220,  60,  60}));
    const uint8_t green = projv::utils::internMaterial(scene, component, "green",
                              projv::packColor({ 70, 190,  90}));
    const uint8_t blue  = projv::utils::internMaterial(scene, component, "blue",
                              projv::packColor({ 70, 120, 220}));
    const uint8_t white = projv::utils::internMaterial(scene, component, "white",
                              projv::packColor({230, 230, 230}));

    auto brickMap = projv::utils::createVoxelBrickMap(
        projv::utils::computeBrickDims(CHUNK_RESOLUTION));

    // A hollow-ish sphere with the axes drawn through it, so the image says which way is up and
    // shows more than one material at once.
    const float centre = CHUNK_RESOLUTION * 0.5f;
    const float radius = CHUNK_RESOLUTION * 0.40f;
    for (int z = 0; z < CHUNK_RESOLUTION; ++z) {
        for (int y = 0; y < CHUNK_RESOLUTION; ++y) {
            for (int x = 0; x < CHUNK_RESOLUTION; ++x) {
                const float dx = x - centre, dy = y - centre, dz = z - centre;
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

                // Shell only: a solid ball would hide its own interior and look identical.
                if (distance < radius && distance > radius - 3.0f) {
                    projv::utils::brickMapSetVoxel(*brickMap, x, y, z, white);
                }
            }
        }
    }
    // Three axis bars from the centre, each in its own colour.
    for (int i = 0; i < CHUNK_RESOLUTION / 2; ++i) {
        projv::utils::brickMapSetVoxel(*brickMap, (int)centre + i, (int)centre, (int)centre, red);
        projv::utils::brickMapSetVoxel(*brickMap, (int)centre, (int)centre + i, (int)centre, green);
        projv::utils::brickMapSetVoxel(*brickMap, (int)centre, (int)centre, (int)centre + i, blue);
    }

    // Compress the brick map into the chunk's tree64, then bake the per-voxel material slots into
    // the parallel array the shader reads.
    projv::utils::updateChunkFromBrickMap(chunk, *brickMap);
    std::vector<uint8_t> bakedMaterialIDs;
    projv::utils::bakeMaterialsFromBrickMap(chunk.geometryData, bakedMaterialIDs, *brickMap);

    // Geometry lives in a refcounted pool blob rather than on the chunk, so several chunks can
    // share one volume. internChunkGeometry moves it there and returns the blob's index.
    const int32_t blobIndex = projv::internChunkGeometry(scene, chunk, std::move(brickMap));
    if (blobIndex >= 0 && static_cast<size_t>(blobIndex) < scene.geometryPool.size()) {
        scene.geometryPool[blobIndex].materialIDs = std::move(bakedMaterialIDs);
    }

    scene.chunks.push_back(std::move(chunk));
    scene.looseChunks.push_back(0);
    scene.looseChunkCount = 1;

    projv::core::info("Built a {}^3 chunk with {} material(s).",
                      CHUNK_RESOLUTION, component.materialPalette.size());
    return scene;
}

// -----------------------------------------------------------------------------------------
// Camera
// -----------------------------------------------------------------------------------------

struct Camera {
    projv::core::vec3 position{-40.0f, 40.0f, -40.0f};
    float yaw   = 0.785f;   // Radians, looking back toward the origin.
    float pitch = -0.45f;
};

// Mouse look and movement, from the Input resource the platform system fills each frame. Speeds are
// per second, scaled by Time::delta, so the camera moves the same on a 60 Hz and a 144 Hz display.
void moveCamera(projv::Application& app) {
    auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
    const auto& input = app.world.ctx().get<projv::Input>();
    auto& camera = app.world.ctx().get<Camera>();

    // Cursor capture is a mode, not a key state: Esc leaves it, a click re-enters it.
    if (input.cursorCaptured && input.pressed(projv::Key::Escape)) {
        projv::graphics::setCursorCaptured(app, renderInstance, false);
    } else if (!input.cursorCaptured && input.pressed(projv::MouseButton::Left)) {
        projv::graphics::setCursorCaptured(app, renderInstance, true);
    }
    if (input.cursorCaptured) {
        const float sensitivity = 0.0025f;
        camera.yaw   += input.cursorDelta.x * sensitivity;
        camera.pitch -= input.cursorDelta.y * sensitivity;
        const float pitchLimit = 1.55f;  // Just shy of straight up, where yaw would gimbal.
        camera.pitch = std::fmax(-pitchLimit, std::fmin(pitchLimit, camera.pitch));
    }

    // Forward from yaw only, so W/S flies level regardless of where you are looking.
    const projv::core::vec3 forward{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
    const projv::core::vec3 right{std::cos(camera.yaw - 1.5708f), 0.0f, std::sin(camera.yaw - 1.5708f)};
    const float speed = 24.0f * app.time().delta;   // world units per second

    if (input.down(projv::Key::W)) camera.position += forward * speed;
    if (input.down(projv::Key::S)) camera.position -= forward * speed;
    if (input.down(projv::Key::D)) camera.position += right * speed;
    if (input.down(projv::Key::A)) camera.position -= right * speed;
    if (input.down(projv::Key::R)) camera.position[1] += speed;
    if (input.down(projv::Key::F)) camera.position[1] -= speed;
}

// -----------------------------------------------------------------------------------------
// Application stages
// -----------------------------------------------------------------------------------------

void startup(projv::Application& app) {
    auto& renderInstance =
        app.world.ctx().emplace<projv::graphics::RenderInstance>();
    renderInstance.initialize(1280, 720, "ProjectV Hello Voxel");
    // Events and Input from here on: the platform system polls the window at the top of every
    // frame, and turns the close button into a CloseRequested event, which ends the application.
    projv::graphics::installPlatform(app, renderInstance);
    projv::graphics::setCursorCaptured(app, renderInstance, true);

    auto& scene   = app.world.ctx().emplace<projv::Scene>();
    auto& gpuData = app.world.ctx().emplace<projv::GPUData>();
    app.world.ctx().emplace<Camera>();

    scene = buildScene();

    // The renderer is described by data, not code: render.json is the pass order, resources.json
    // names the shaders, textures and framebuffers. Paths inside resources.json are relative to
    // the working directory, which is why the folder is passed as a path from here.
    const std::string rendererDirectory = (assetDirectory() / "helloRenderer").string() + "/";
    projv::RendererSpecification specification =
        projv::graphics::loadRendererSpecification(rendererDirectory);
    renderInstance.addRendererSpecification(1, specification);

    const std::string vertexShaderPath =
        (assetDirectory() / "helloRenderer/helloShaders/vs_quad.bin").string();
    bgfx::ShaderHandle vertexShader = projv::graphics::loadShader(vertexShaderPath);

    renderInstance.setActiveRenderer(projv::graphics::constructRendererSpecification(
        renderInstance.getRendererSpecification(1), vertexShader));

    // Uploads the voxel data to the textures the shader reads. Everything above this line is CPU
    // side; nothing is on the GPU until here.
    gpuData = projv::graphics::createTexturesForScene(scene);
}

void update(projv::Application& app) {
#if defined(PROJV_ENABLE_PERF)
    // Frame timing, so "is this slow?" has an answer without attaching a profiler. Compiled out
    // unless performance logging is enabled.
    static double frameTimes[100];
    static int timedFrames = 0;
    frameTimes[timedFrames % 100] = app.time().unscaledDelta * 1000.0;
    if (++timedFrames % 100 == 0) {
        double sum = 0.0;
        for (int i = 0; i < 100; ++i) sum += frameTimes[i];
        projv::core::perf("Frame stats (last 100): avg={:.2f}ms", sum / 100.0);
    }
#else
    (void)app;
#endif
}

void render(projv::Application& app) {
    auto& renderInstance =
        app.world.ctx().get<projv::graphics::RenderInstance>();
    auto& gpuData = app.world.ctx().get<projv::GPUData>();
    auto& camera  = app.world.ctx().get<Camera>();

    // Deliberately not const: setUniformToValue is a template that dispatches on the deduced type,
    // and a const vec3 is not one of the types it knows about. Passing one gets you
    // "Typename T for data is unknown" at runtime and a uniform that never reaches the shader.
    projv::core::vec3 direction{
        std::cos(camera.pitch) * std::cos(camera.yaw),
        std::sin(camera.pitch),
        std::cos(camera.pitch) * std::sin(camera.yaw)
    };

    projv::core::vec2 resolution = renderInstance.getWindowResolution();
    auto renderer = renderInstance.getActiveRenderer();
    projv::graphics::setUniformToValue(renderer, "cameraPos", camera.position);
    projv::graphics::setUniformToValue(renderer, "cameraDir", direction);
    projv::graphics::setUniformToValue(renderer, "windowRes", resolution);

    projv::graphics::renderConstructedRenderer(renderInstance, renderer, &gpuData);
}

void shutdown(projv::Application& app) {
    // Frees the GPU-side scene textures. bgfx and GLFW are torn down by the RenderInstance.
    auto& gpuData = app.world.ctx().get<projv::GPUData>();
    projv::graphics::destroyGPUData(gpuData);
    projv::core::info("Goodbye.");
}

} // namespace

int main() {
    projv::Application app;
    app.addSystem(projv::Stage::Startup, "startup", startup);
    app.addSystem(projv::Stage::Update, "move camera", moveCamera);
    app.addSystem(projv::Stage::Update, "frame stats", update);
    app.addSystem(projv::Stage::Render, "render", render);
    app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    app.run();
    return 0;
}
