#include "utils.h" // For generating data types.
#include "graphics.h" // For handling shaders and passing data to the GPU.
#include "core.h" // For handling user input and window creation.
#include "camera.h"
#include <vector>
#include <string>
#include <cstdint>
#include <cstdlib> // For rand() and srand()


#include <iostream>
#include <fstream>
#include <vector>
#include <stdexcept>



// Custom function to create a sphere inside of a projectV voxel grid.
void createSphere(std::vector<std::vector<std::vector<projv::voxel>>> &voxels, int radius, int centerX, int centerY, int centerZ){
    for(int x = 0; x < 128; x++){
        for(int y = 0; y < 128; y++){
            for(int z = 0; z < 128; z++){

                voxels[x][y][z].filled = false;
                int deltaX = x - centerX;
                int deltaY = y - centerY;
                int deltaZ = z - centerZ;
                if(deltaX*deltaX + deltaY*deltaY + deltaZ*deltaZ < radius*radius){
                    voxels[x][y][z].filled = true;
                    voxels[x][y][z].color.r = x;
                    voxels[x][y][z].color.g = y;
                    voxels[x][y][z].color.b = z;
                }
            }
        }
    }
}

void error_callback(int error, const char* description) {
    std::cerr << "GLFW Error (" << error << "): " << description << std::endl;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    int width1, height1;
    glfwGetFramebufferSize(window, &width1, &height1);
    glViewport(0, 0, width1, height1);
    projv::createPostProcessingFrameBuffers(window);

}

int main(){
    glfwSetErrorCallback(error_callback); // Allows for GLFW to print errors to the console.
    GLFWwindow* window; // The window that our program will use.
    if(!projv::initializeGLFWandGLEWWindow(window, 1920, 1080, "window!!")){ // Initializes GLFW and creates a window. Prints if initialization fails.
        std::cerr << "Failed to initialize GLFW and GLEW" << std::endl;
        return -1;
    }
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    projv::createShaders();
    projv::createPostProcessingFrameBuffers(window);
    projv::compileAndUseVertexAndFragShader(); // Compiles the shaders so they can be run.
    projv::compileAndUseVertexAndPostProcessingShader();
    projv::createRenderQuad(); // Creates a quad (2 triangles that cover the whole screen) to render to.
    std::vector<std::vector<std::vector<projv::voxel>>> voxels = projv::createVoxelGrid(256); // Creates a 512x512x512 voxel grid.

    for(int x = 0; x < 256; x++){ // Loops over all the voxels and fills them based off of a funciton.
        for(int y = 0; y < 256; y++){
            for(int z = 0; z < 256; z++){
                if(sin(float(x)/10) + sin(float(y)/10) + sin(float(z)/10) > 0){
                    voxels[x][y][z].filled = true;
                    voxels[x][y][z].color.r = x; //Assigns colors (0-255)
                    voxels[x][y][z].color.g = y;
                    voxels[x][y][z].color.b = z;
                }
            }
        }
    }

    std::vector<uint32_t> octree; // Defines our octree that will store the scene.
    std::vector<uint32_t> vtd; // Defines our voxel type data that will store the scene.
    octree = projv::createOctree(voxels, 256, 0); // Converts the voxel grid into our octree.
    vtd = projv::createVoxelTypeData(voxels, 256, 0); // Converts the voxel grid into our octree.
    projv::passOctreeToFrag(octree); // Passes the octree to the fragment shader to be rendered.
    projv::passVoxelTypeDataToFrag(vtd); // Passes the octree to the fragment shader to be rendered.

    projv::cam.position[0] = 23.7;
    projv::cam.position[1] = 4.2;
    projv::cam.position[2] = 15.6;

    projv::cam.direction[0] = 0;
    projv::cam.direction[2] = -1;

    int radius = 20;
    std::cout << "Here!" << std::endl;
    while(!glfwWindowShouldClose(window)){
        std::cout << "x:" << projv::cam.position[0] << " y:" << projv::cam.position[1] << " z:" << projv::cam.position[2] << std::endl;
        GLenum error;
        glfwPollEvents(); // Checks for events to allow for user input.
        projv::moveCameraFromUserInput(window); // Moves the camera based off of user input.
        projv::updateTimeInShader(); // Updates the time in the shader.
        projv::updateCameraInShader(); // Updates the camera position in the shader.
        projv::updateResolutionInShader(window); // Updates the resolution in the shader based off of the size of the window.
        std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now(); // Starts a timer to measure how long it takes to render a frame.
        projv::renderFrame(window, 0); // Runs the shaders and updates the window to display the new frame. 
        std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now(); // Ends the timer.
        std::chrono::duration<double, std::milli> time_span = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start); // Calculates the time it took to render the frame in milliseconds.
        std::cout << "Frame time: " << time_span.count() << " ms." << std::endl; // Prints the time it took to render the frame in milliseconds.
                                       // Second parameter is vsync option. The 1 indiciates it will run at your monitors refresh rate. A 2 would be 1/2, 3 would be 1/4. 0 would be no vsync.
        while ((error = glGetError()) != GL_NO_ERROR) { // Detects and stores OpenGL errors in err.
            std::cout << "OpenGL ERROR 11: " << error << std::endl; // If there is an error, print it to the console.
            return -1; // Return -1 to close the program.
        }
        GLenum err; // Creates a variable to store OpenGL errors.
        while ((err = glGetError()) != GL_NO_ERROR) { // Detects and stores OpenGL errors in err.
            std::cout << "OpenGL ERROR: " << err << std::endl; // If there is an error, print it to the console.
            return -1; // Return -1 to close the program.
        }
    }    
    glfwTerminate(); // Terminates GLFW and closes the window.
    return 0;
}
