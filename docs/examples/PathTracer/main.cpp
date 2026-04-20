// ProjectV Path Tracer
// An interactive real-time path tracer that loads a pre-voxelized scene from disk and renders
// it using a tree64-based path tracing renderer. The camera can be freely navigated with
// keyboard controls, and the renderer accumulates samples across frames when the camera is still.
//
// Scene: SponzaScene — voxelized Sponza atrium produced by ProjectVObjVoxelizer.
//
// Controls:
//   W/S       — move forward / backward
//   A/D       — strafe left / right
//   R/F       — move up / down
//   Q/E       — rotate camera left / right
//
// ProjectV Engine Features Used:
//   - Core ECS        : Application, world, global resources, system stage assignment (Startup/Update/Render)
//   - Core Math       : vec2/vec3/vec4, cos/sin — used for camera direction and uniform packing
//   - Logging         : info/warn via spdlog wrapper for structured output
//   - Voxel I/O       : loadSceneFromDisk — deserializes the chunked voxel scene from disk
//   - GPU Interface   : createTexturesForScene, GPUData — uploads voxel scene data to the GPU
//   - Manage Resources: RendererSpecification, ConstructedRenderer, setTextureToData — builds the renderer pipeline
//   - Disk I/O        : loadRendererSpecification, loadShader — loads renderer config and compiled shaders
//   - Render Instance : window creation, active renderer management, window resolution query
//   - Perform Renderer: renderConstructedRenderer, setUniformToValue — drives per-frame rendering and uniform upload

#include "core/ecs.h"
#include "core/math.h"
#include "core/log.h"
#include "graphics/render_instance.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/type_mapping.h"
#include "utils/voxel_io.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Startup: create the window, load the scene and renderer, upload all GPU resources.
void startup(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance = projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(2560, 1440, "hengry!");

    projv::Scene& scene   = projv::core::createGlobalResource<projv::Scene>(app.world);
    float& cameraPhi      = projv::core::createGlobalResource<float>(app.world);
    projv::GPUData& gpuData = projv::core::createGlobalResource<projv::GPUData>(app.world);

    cameraPhi = 3.14/2 + 0.4;

    scene = projv::utils::loadSceneFromDisk("./SponzaScene/");

    projv::RendererSpecification rendererSpec = projv::graphics::loadRendererSpecification("./tree64Renderer/");
    renderInstance.addRendererSpecification(1, rendererSpec);

    bgfx::ShaderHandle vsh = projv::graphics::loadShader("./tree64Renderer/pathTracerShaders/vs_quad.bin");
    std::shared_ptr<projv::ConstructedRenderer> constructedRenderer = projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vsh);

    // Load the blue-noise LUT used by the path tracer for sample decorrelation.
    int width, height, channels;
    unsigned char* img = stbi_load("LDR_RGBA_7.png", &width, &height, &channels, 4);
    projv::core::info("Blue-noise texture: {}x{}", width, height);
    projv::graphics::setTextureToData(constructedRenderer, 1, img, width, height);

    renderInstance.setActiveRenderer(constructedRenderer);
    gpuData = projv::graphics::createTexturesForScene(scene);
}

// Update: frame timing profiler.
void update(projv::Application& app) {
    static auto lastFrameTime = std::chrono::high_resolution_clock::now();

    auto currentFrameTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> frameDuration = currentFrameTime - lastFrameTime;
    lastFrameTime = currentFrameTime;
    projv::core::info("PROFILING: Frame time {:.2f}ms", frameDuration.count() * 1000.0);
}

// Render: handle camera input, upload per-frame uniforms, and dispatch the path tracer.
void render(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance = projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    projv::GPUData& gpuData   = projv::core::getGlobalResource<projv::GPUData>(app.world);
    float& cameraPhi          = projv::core::getGlobalResource<float>(app.world);

    static projv::core::vec3 cameraPosition = projv::core::vec3(74.0, 30.0, -24.0);
    bool cameraMoved = false;

    // Yaw rotation (Q/E).
    if (glfwGetKey(renderInstance.window, GLFW_KEY_Q)) { cameraPhi -= (5*3.14)/180; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_E)) { cameraPhi += (5*3.14)/180; cameraMoved = true; }

    projv::core::vec3 cameraDirection;
    cameraDirection.x = projv::core::cos(cameraPhi);
    cameraDirection.y = 0;
    cameraDirection.z = projv::core::sin(cameraPhi);

    // Horizontal movement (W/A/S/D).
    if (glfwGetKey(renderInstance.window, GLFW_KEY_W)) { cameraPosition += cameraDirection * 0.3f; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_S)) { cameraPosition -= cameraDirection * 0.3f; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_A)) {
        float leftPhi = cameraPhi + 3.14/2;
        projv::core::vec3 leftDirection = { projv::core::cos(leftPhi), 0, projv::core::sin(leftPhi) };
        cameraPosition -= leftDirection * 0.3f;
        cameraMoved = true;
    }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_D)) {
        float rightPhi = cameraPhi - 3.14/2;
        projv::core::vec3 rightDirection = { projv::core::cos(rightPhi), 0, projv::core::sin(rightPhi) };
        cameraPosition -= rightDirection * 0.3f;
        cameraMoved = true;
    }

    // Vertical movement (R/F).
    if (glfwGetKey(renderInstance.window, GLFW_KEY_R)) { cameraPosition[1] += 0.3; cameraMoved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_F)) { cameraPosition[1] -= 0.3; cameraMoved = true; }

    projv::core::info("Pos: ({:.2f}, {:.2f}, {:.2f})", cameraPosition[0], cameraPosition[1], cameraPosition[2]);

    // Track the frame the camera last moved on so the shader can reset accumulation.
    static int frameCameraLastMovedOn = 0;
    if (cameraMoved) frameCameraLastMovedOn = app.frameCount;

    projv::core::vec2 windowDimensions = renderInstance.getWindowResolution();
    projv::core::vec4 frameCount = { app.frameCount, cameraMoved, frameCameraLastMovedOn, 0 };

    projv::graphics::setUniformToValue(renderInstance.getActiveRenderer(), "cameraPos",  cameraPosition);
    projv::graphics::setUniformToValue(renderInstance.getActiveRenderer(), "cameraDir",  cameraDirection);
    projv::graphics::setUniformToValue(renderInstance.getActiveRenderer(), "windowRes",  windowDimensions);
    projv::graphics::setUniformToValue(renderInstance.getActiveRenderer(), "frameCount", frameCount);
    projv::core::vec2 texelSize = {1/windowDimensions.x, 1/windowDimensions.y};
    projv::graphics::setUniformToValue(renderInstance.getActiveRenderer(), "texelSize",  texelSize);

    projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpuData);
}

int main() {
    projv::Application app = projv::core::createApp();
    projv::core::assignSystemStage(app, projv::SystemStage::Startup, startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update,  update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render,  render);
    projv::core::runApplication(app);
    return 0;
}
