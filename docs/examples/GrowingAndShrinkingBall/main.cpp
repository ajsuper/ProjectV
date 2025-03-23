/*
This is an example piece of code using ProjectV that generates a sphere, converts it to an octree, passes this to the shader, and then changes the radius and does it again.
This example covers many of the C++ side features of ProjectV that makes it powerful and easy to use.
*/

#include "utils.h" // For generating data types.
#include "graphics.h" // For handling shaders and passing data to the GPU.
#include "core.h" // For handling user input and window creation.
#include <vector>
#include <string>
#include <cstdint>

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

int main(){
    
    glfwSetErrorCallback(projv::error_callback); // Allows for GLFW to print errors to the console.
    GLFWwindow* window; // The window that our program will use.
    if(!projv::initializeGLFWandGLEWWindow(window, 900, 900, "window!!")){ // Initializes GLFW and creates a window. Prints if initialization fails.
        std::cerr << "Failed to initialize GLFW and GLEW" << std::endl;
        return -1;
    }

    projv::compileAndUseVertexAndFragShader(); // Compiles the shaders so they can be run.
    projv::createRenderQuad(); // Creates a quad (2 triangles that cover the whole screen) to render to.
    
    std::vector<std::vector<std::vector<projv::voxel>>> voxels = projv::createVoxelGrid(128); // Creates a 128x128x128 voxel grid.

    std::vector<uint32_t> octree; // Defines our octree that will store the scene.
    createSphere(voxels, 10, 64, 64, 64); // References the helper function to fill the scene with a sphere.
    octree = projv::createOctree(voxels, 128, 0); // Converts the voxel grid into our octree.
    projv::passOctreeToFrag(octree); // Passes the octree to the fragment shader to be rendered.

    int radius = 20;
    float x = 0;
    while(!glfwWindowShouldClose(window)){ // Main loop that runs until the window is closed.
        createSphere(voxels, radius, 64, 64, 64); // Creates a sphere with a radius that changes over time to grow and shrink.
        octree = projv::createOctree(voxels, 128, 0); // Regenrates the octree with the new sphere.
        projv::passOctreeToFrag(octree); // Updates the octree in the fragment shader.
        x++; // Increments x to change the radius of the sphere.
        radius = 32 + 32*sin(float(x)/50); // Calculates our new sphere radius.
        glfwPollEvents(); // Checks for events to allow for user input.
        projv::moveCameraFromUserInput(window); // Moves the camera based off of user input.
        projv::renderFrame(window, 1); // Runs the shaders and updates the window to display the new frame. 
                                       // Second parameter is vsync option. The 1 indiciates it will run at your monitors refresh rate. A 2 would be 1/2, 3 would be 1/4. 0 would be no vsync.
        projv::updateCameraInShader(); // Updates the camera position in the shader.
        projv::updateResolutionInShader(window); // Updates the resolution in the shader based off of the size of the window.
        GLenum err; // Creates a variable to store OpenGL errors.
        while ((err = glGetError()) != GL_NO_ERROR) { // Detects and stores OpenGL errors in err.
            std::cout << "OpenGL ERROR: " << err << std::endl; // If there is an error, print it to the console.
            return -1; // Return -1 to close the program.
        }
    }    
    glfwTerminate(); // Terminates GLFW and closes the window.
    return 0;
}
