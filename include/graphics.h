#ifndef PROJECTV_GRAPHICS_H
#define PROJECTV_GRAPHICS_H
// Last edited on: 31-12-2024.

#include <fstream>
#include <sstream>
#include <iostream>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <bitset>
#include <chrono>
#include <vector>
#include <cmath>

namespace projv{

    // Global variables
    extern GLuint shaderProgram;
    extern GLuint VAO, VBO;  // Vertex Array Object and Vertex Buffer Object

    /**
     * Renders a frame to the given window.
     * 
     * @param window The GLFW window to render to.
     * @param swapInterval The interval for swapping buffers.
     */
    void renderFrame(GLFWwindow* window, int swapInterval); 

    /**
     * Creates a quad for rendering to the whole window.
     */
    void createRenderQuad();

    /**
     * Updates the camera position and direction in the shader.
     */
    void updateCameraInShader();

    /**
     * Updates the resolution in the shader.
     * 
     * @param window The GLFW window to get the framebuffer size from.
     */
    void updateResolutionInShader(GLFWwindow* window);

    /**
     * Loads GLSL shader source code from a file.
     * 
     * @param filepath The path to the shader file.
     * @return The shader source code as a string.
     */
    std::string loadShaderSource(const char* filepath);

    /**
     * Passes octree data to the fragment shader.
     * 
     * @param octree The octree data to pass.
     */
    void passOctreeToFrag(std::vector<uint32_t> octree);

    /**
     * Passes voxel type data to the fragment shader.
     * 
     * @param voxelTypeData The voxel type data to pass.
     */
    void passVoxelTypeDataToFrag(std::vector<uint32_t> voxelTypeData);

    /**
     * Compiles the vertex and fragment shaders and links them into a shader program.
     */
    void compileAndUseVertexAndFragShader();
}

#endif
