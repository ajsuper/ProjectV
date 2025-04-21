// Please see the README.md for this example for more information on this example.
// Also please see the README.md in the root directory for more information on the whole project.

#include "core/ecs.h"
#include "core/math.h"

#include "graphics/fbo.h"
#include "graphics/render.h"
#include "graphics/shader.h"
#include "graphics/uniforms.h"
#include "graphics/user_input.h"
#include "graphics/window.h"
#include "graphics/scene.h"

#include "utils/lod.h"
#include "utils/voxel_io.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

#include "include/noise.hpp"
#include <algorithm> // For std::clamp

#define WATER_LEVEL 64
#define MOUNTAIN_THRESHOLD 300
#define PLAINS_MAX 100

int ID = 0;

// A basic spline set of points to use for ProjectV's built in linear spline function.
std::vector<projv::core::vec2> curvePoints = {
    {0.0f, 0.01f},     // very low
    {0.29f, 0.01f},    // still hugging the bottom
    {0.4f, 0.1f},    // barely rising
    {0.5f, 0.12f},     // small incline
    {0.65f, 0.25f},   // starting to go upward
    {0.75f, 0.45f},    // boom, rising fast
    {0.85f, 0.6f},    // reaching the top
    {0.9f, 0.75f},    // flattening out
    {1.0f, 0.9f}      // plateau
};

// Helper function to generate terrain to demonstrate ProjectV.
void createScene(int xSize, int ySize, int zSize, int resolution, int scale) {
    const float scale2 = 1.0f;
    const siv::PerlinNoise::seed_type seed = 123456u;
    const siv::PerlinNoise perlin{ seed };

    for(int chunkX = 0; chunkX < xSize; chunkX++){
        for(int chunkY = 0; chunkY < ySize; chunkY++){
            for(int chunkZ = 0; chunkZ < zSize; chunkZ++){
                // Creates a chunk header to store meta deta about our chunk.
                projv::CPUChunkHeader chunkHeader;
                chunkHeader.position = {chunkX * scale, chunkY * scale, chunkZ * scale};
                chunkHeader.scale = scale;
                chunkHeader.resolution = resolution;
                chunkHeader.chunkID = ID;
                ID++;

                // Creates the empty container for our voxel data. This isn't the final data, just an intermediate layer for optimization.   
                projv::VoxelBatch voxelBatch = projv::utils::createVoxelBatch();

                // For all chunks, loop over all voxels and determine the height and color of that voxel.
                for(int voxelX = 0; voxelX < chunkHeader.resolution; voxelX++) {
                    for(int voxelZ = 0; voxelZ < chunkHeader.resolution; voxelZ++) {
                        // Gathers our noise data, uses the built in spline function to modify the curve (creates hills, plains, valleys, mountains).
                        double noise = perlin.octave2D_01((voxelX + chunkX * chunkHeader.resolution + 500) * 0.0010/scale2, (voxelZ + chunkZ * chunkHeader.resolution + 500) * 0.0010/scale2, 9);
                        float scaledNoise = projv::core::evaluateCurve(noise, curvePoints) * 700 * (scale2);

                        // Loops over all the y axis of the voxel.
                        for(int voxelY = 0; voxelY < resolution; voxelY++) {
                            if(voxelY+chunkY*resolution < scaledNoise && voxelY+chunkY*resolution > scaledNoise - 5 || (voxelY+chunkY*resolution == WATER_LEVEL)) {
                                // Assign color based on elevation
                                projv::Color color = {255, 255, 255};
                                if (voxelY+chunkY*resolution == WATER_LEVEL) {
                                    color = { 0, 0, 255 }; // Blue
                                } else if (voxelY+chunkY*resolution < PLAINS_MAX) {
                                    color = { 80, 200, 120 }; // Green
                                } else if (voxelY+chunkY*resolution < MOUNTAIN_THRESHOLD) {
                                    color = { 120, 120, 120 }; // Grayish rocks
                                }

                                // Create a voxel and add it to the batch.
                                projv::Voxel vox;
                                vox.color = color;
                                vox.ZOrderPosition = projv::utils::createZOrderIndex({voxelX, voxelY, voxelZ}, 15);
                                projv::utils::addVoxelToBatch(vox, voxelBatch); // This batch editing is very fast for large amounts of voxels.
                            }
                        }
                    }
                }
                // Add the voxels to the grid so they can be converted into an octree. This ensures proper sorting of the voxels.
                projv::VoxelGrid voxels = projv::utils::createVoxelGrid();  
                projv::utils::addVoxelBatchToVoxelGrid(voxels, voxelBatch);
                std::vector<uint32_t> chunkGeometry = projv::utils::createOctree(voxels, chunkHeader.resolution); // Handles geometry
                std::vector<uint32_t> voxelTypeData = projv::utils::createVoxelTypeData(voxels); // Handles colors and other extra data.

                // Stores all the data for this octree.
                projv::RuntimeChunkData chunkData;
                chunkData.header = chunkHeader;
                chunkData.geometryData = chunkGeometry;
                chunkData.voxelTypeData = voxelTypeData;
                chunkData.LOD = 0;

                // Writes a chunk to the specified file.
                projv::utils::writeChunkToDisk("./SceneTest", chunkData);
            }
        } 
    }
}

int calculateTargetLod(int camX, int camY, int camZ, projv::RuntimeChunkData chunkData) {
    projv::core::vec3 position = chunkData.header.position;
    // Calculate the Euclidean distance between the camera and the chunk
    float distance = projv::core::distance(position, {camX, camY, camZ});

    distance = sqrt(distance);

    // Determine the LOD based on the distance (example thresholds)
    if (distance < 4) {
        return 0; // Highest detail
    } else if (distance < 10) {
        return 1; // Medium detail
    } else if (distance < 12) {
        return 2; // Low detail
    } else {
        return 3; // Lowest detail
    }
}

void error_callback(int error, const char* description) {
    std::cerr << "GLFW Error (" << error << "): " << description << std::endl;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) { 
    int width1, height1;
    glfwGetFramebufferSize(window, &width1, &height1);
    width1 = width1;
    height1 = height1;
    glViewport(0, 0, width1, height1);
}

namespace gloablFlags {
    using UpdateScene = bool;
}

// The startup function that ProjectV will automatically run with parameters of the application it is running in. This is where the game will be initialized.
void startup(projv::Application& app) {
    createScene(5, 1, 5, 512, 30);
    // Initialize the render instance as a global resource in our ECS system. This allows it to be accessed by the other stages (render and update). renderInstance will contain all of our data for rendering (window, framebuffers, shaders).
    projv::RenderInstance& renderInstance = projv::core::createGlobalResource<projv::RenderInstance>(app.world); // A reference is important that way you actually change the data in the global resource. ANY data structure can be assigned to the ECS system.
    renderInstance = projv::graphics::initializeRenderInstance(400, 400, "Jimm");
    projv::graphics::setErrorCallback(error_callback); // Set our error_callback. This is how openGL will output errors, and the user defined error_callback decides how to handle them (close application, print to console, log to file, etc.)
    projv::graphics::setFrameBufferSizeCallback(renderInstance, framebuffer_size_callback); // Set's our framebuffer_size_callback. Useful for resizing FrameBufferObjects.

    // Create the frame buffer for the screen. OpenGL automatically assigns bufferID 0 to be the window.
    projv::FrameBuffer outputFrameBuffer;
    outputFrameBuffer.buffer = 0;

    // add our shader and our frame buffer to our render instance. This will allow the render stage to access the shaders.
    projv::graphics::addShaderToRenderInstance(renderInstance, projv::graphics::compileVertexAndFragmentShaders("./shaders/vertexShader.glsl", "./shaders/fragmentShader.frag"), "rayMarcher");
    projv::graphics::addFramebufferToRenderInstance(renderInstance, outputFrameBuffer, "outputFBO"); // This name is used to access the framebuffer from the other stages. Think of it like it's key.

    // Load our scene.
    projv::Scene& scene = projv::core::createGlobalResource<projv::Scene>(app.world); // We first create a scene. A reference is important so that way you actually change the data in the global resource.
    scene = projv::utils::loadSceneFromDisk("./SceneTest"); // This loads all chunks of a scene into the scene object.
    projv::core::createGlobalResource<gloablFlags::UpdateScene>(app.world); // We then create a user defined globalFlags::UpdateScene, this allows us to track when the scene needs to be re-sent to OpenGL in our render function.
}

// The update function that ProjectV will automatically run with parameters of the application it is running in. This is where the game logic will be updated.
void update(projv::Application& app) {
    // Access global render state (window, shaders, FBOs). These were the globalResources we created and modified in startup.
    projv::RenderInstance& renderInstance = projv::core::getGlobalResource<projv::RenderInstance>(app.world);
    projv::Scene& scene = projv::core::getGlobalResource<projv::Scene>(app.world);
    gloablFlags::UpdateScene& updateScene = projv::core::getGlobalResource<gloablFlags::UpdateScene>(app.world);

    // Get the shader and FBO from the render instance.
    GLuint rayMarcher = projv::graphics::getShaderFromRenderInstance(renderInstance, "rayMarcher");
    projv::FrameBuffer outputFBO = projv::graphics::getFramebufferFromRenderInstance(renderInstance, "outputFBO");

    // Get our camera's positions.
    float xcam = projv::cam.position[0];
    float y = projv::cam.position[1];
    float z = projv::cam.position[2];

    // Loop over every chunk, if it's distance is determined to be too far away by calculateTargetLod, then it's LOD is a higher number (higher LOD # = lower LOD of actual geometry).
    updateScene = false;
    for(int x = 0; x < scene.chunks.size(); x++ ){
        projv::RuntimeChunkData& chunk = scene.chunks[x];
        int desiredLOD = calculateTargetLod(xcam, y, z, chunk);
        if(desiredLOD != scene.chunks[x].LOD) {
            bool forceReloadingOfChunkFromDisk = false;
            projv::utils::updateLOD(chunk, desiredLOD, "./SceneTest", forceReloadingOfChunkFromDisk);
            updateScene = true;
        }
    }

    // Update the necessary variables in OpenGL.
    projv::graphics::moveCameraFromUserInput(renderInstance.window);
    projv::graphics::updateCameraInShader(rayMarcher);
    projv::graphics::updateResolutionInShader(rayMarcher, renderInstance.window);
}

// The render function that ProjectV will automatically run with parameters of the application it is running in. This is where the game will be rendered.
void render(projv::Application& app) {
    static int frameCount = 0;
    glfwPollEvents(); // Processes inputs & window events.
    // Access global render state (window, shaders, FBOs).
    projv::RenderInstance& renderInstance = projv::core::getGlobalResource<projv::RenderInstance>(app.world);
    projv::Scene& scene = projv::core::getGlobalResource<projv::Scene>(app.world);
    gloablFlags::UpdateScene& updateScene = projv::core::getGlobalResource<gloablFlags::UpdateScene>(app.world);

    // Get the shader and FBO from the render instance.
    GLuint rayMarcher = projv::graphics::getShaderFromRenderInstance(renderInstance, "rayMarcher");
    projv::FrameBuffer outputFBO = projv::graphics::getFramebufferFromRenderInstance(renderInstance, "outputFBO");

    // Set our viewport based on the window size.
    int width, height;
    glfwGetFramebufferSize(renderInstance.window, &width, &height);
    glViewport(0, 0, width, height);

    // Render and swap buffers. This allows you to render any shader to any FrameBufferObject.
    projv::graphics::renderFragmentShaderToTargetBuffer(renderInstance, rayMarcher, outputFBO);
    glfwSwapInterval(1); // VSync
    glfwSwapBuffers(renderInstance.window); // Send the rendered frame to our screen.

    // Update our scene in the case that our LOD changed, frameCount is 0.
    if(updateScene || frameCount == 0) {
        projv::graphics::passSceneToOpenGL(scene);
    }

    frameCount++;
}

int main() {
    projv::Application app = projv::core::createApp(); // Creating a ProjectV application to organize the rest of our game.
    projv::core::addSystem(app, projv::SystemStage::Startup, startup); // We're passing our startup function as a parameter to run on SystemStage::Startup.
    projv::core::addSystem(app, projv::SystemStage::Update, update); // We're passing our update function as a parameter to run on SystemStage::Update.
    projv::core::addSystem(app, projv::SystemStage::Render, render); // We're passing our render function as a parameter to run on SystemStage::Render.
    projv::core::runApplication(app); // We're telling our application to run. This starts off it's loop.
    return 0;
}