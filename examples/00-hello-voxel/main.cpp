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
//   2. put the scene and its GPU mirror in the world   createGlobalResource
//   3. get some voxels                           buildScene() below, or loadComposeFromDisk
//   4. describe the renderer                     loadRendererSpecification (render.json + resources.json)
//   5. load the vertex shader                    loadShader
//   6. turn the description into GPU objects     constructRendererSpecification
//   7. upload the voxels                         createTexturesForScene
//
// Then each frame: set the uniforms the shaders read, and call renderConstructedRenderer.
//
// Two things worth copying into your own application, because no other example demonstrates them:
//
//   * It exits cleanly. The engine records a window-manager close request on
//     RenderInstance::shouldClose; the application decides what that means. Here it ends the loop.
//     A tool with unsaved work would raise a prompt instead, which it could not do if the engine
//     closed the window on its behalf.
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

#include "core/ecs.h"
#include "core/log.h"
#include "core/math.h"
#include "core/paths.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
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

// Cursor capture is a mode, not a key state, so it is tracked rather than polled.
bool  g_cursorCaptured = true;
double g_lastMouseX = 0.0, g_lastMouseY = 0.0;
bool  g_mouseTracking = false;

void updateCamera(Camera& camera, GLFWwindow* window) {
    if (g_cursorCaptured && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        g_cursorCaptured = false;
    } else if (!g_cursorCaptured && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        g_cursorCaptured = true;
    }

    double mouseX, mouseY;
    glfwGetCursorPos(window, &mouseX, &mouseY);
    // Re-seed the reference point whenever tracking (re)starts, so re-capturing the cursor after a
    // release does not apply one huge jump.
    if (!g_mouseTracking || !g_cursorCaptured) {
        g_lastMouseX = mouseX;
        g_lastMouseY = mouseY;
        g_mouseTracking = true;
    }
    if (g_cursorCaptured) {
        const float sensitivity = 0.0025f;
        camera.yaw   += static_cast<float>(mouseX - g_lastMouseX) * sensitivity;
        camera.pitch -= static_cast<float>(mouseY - g_lastMouseY) * sensitivity;
        const float pitchLimit = 1.55f;  // Just shy of straight up, where yaw would gimbal.
        camera.pitch = std::fmax(-pitchLimit, std::fmin(pitchLimit, camera.pitch));
    }
    g_lastMouseX = mouseX;
    g_lastMouseY = mouseY;

    // Forward from yaw only, so W/S flies level regardless of where you are looking.
    const projv::core::vec3 forward{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
    const projv::core::vec3 right{std::cos(camera.yaw - 1.5708f), 0.0f, std::sin(camera.yaw - 1.5708f)};
    const float speed = 0.4f;

    if (glfwGetKey(window, GLFW_KEY_W)) camera.position += forward * speed;
    if (glfwGetKey(window, GLFW_KEY_S)) camera.position -= forward * speed;
    if (glfwGetKey(window, GLFW_KEY_D)) camera.position += right * speed;
    if (glfwGetKey(window, GLFW_KEY_A)) camera.position -= right * speed;
    if (glfwGetKey(window, GLFW_KEY_R)) camera.position[1] += speed;
    if (glfwGetKey(window, GLFW_KEY_F)) camera.position[1] -= speed;
}

// -----------------------------------------------------------------------------------------
// Application stages
// -----------------------------------------------------------------------------------------

void startup(projv::Application& app) {
    auto& renderInstance =
        projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(1280, 720, "ProjectV Hello Voxel");
    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto& scene   = projv::core::createGlobalResource<projv::Scene>(app.world);
    auto& gpuData = projv::core::createGlobalResource<projv::GPUData>(app.world);
    projv::core::createGlobalResource<Camera>(app.world);

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
    auto& renderInstance =
        projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);

    // The hand-off. renderConstructedRenderer polls GLFW and records the close request; nothing in
    // the engine acts on it, so an application that wants to close has to say so.
    if (renderInstance.shouldClose) {
        app.closeAppFlag = true;
    }
}

void render(projv::Application& app) {
    auto& renderInstance =
        projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    auto& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    auto& camera  = projv::core::getGlobalResource<Camera>(app.world);

    updateCamera(camera, renderInstance.window);

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
    auto& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    projv::graphics::destroyGPUData(gpuData);
    projv::core::info("Goodbye.");
}

} // namespace

int main() {
    projv::Application app = projv::core::createApp();
    projv::core::assignSystemStage(app, projv::SystemStage::Startup,  startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update,   update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render,   render);
    projv::core::assignSystemStage(app, projv::SystemStage::Shutdown, shutdown);
    projv::core::runApplication(app);
    return 0;
}
