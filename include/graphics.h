#ifndef PROJECTV_GRAPHICS_H
#define PROJECTV_GRAPHICS_H

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

    /**
     * Contains 5 GLuint's. a buffer, colorTexture, normalTexture, colorTexture2, positionTexture, to be used in rendering. Will be upgraded to a modular system soon.
     */
    struct FrameBuffer {
        GLuint buffer;
        GLuint colorTexture;
        GLuint normalTexture;
        GLuint colorTexture2;
        GLuint positionTexture;
    };

    // Global variables
    extern GLuint VAO, VBO;  // Vertex Array Object and Vertex Buffer Object
    extern int frameCount;

    /**
     * Creates a FrameBuffer that can be used for further rendering in renderMultipassFragmentShaderToTargetBuffer and renderFragmentShaderToTargetBuffer
     * 
     * @param window The GLFW window to determine the size of the buffers.
     */
    FrameBuffer createFrameBufferObject(int width, int height);

    /**
     * Renders a fragment/vertex shader shader program to the targetBuffer.
     * 
     * @param shaderProgram The shader that will be used to render to the targetBuffer.
     * @param targetBuffer What the shaderProgram will render to.
     */
    void renderFragmentShaderToTargetBuffer(GLuint shaderProgram, FrameBuffer targetBuffer);

    /**
     * Renders a fragment/vertex shader shader program to the targetBuffer at it's resolution.
     * 
     * @param shaderProgram The shader that will be used to render to the targetBuffer.
     * @param inputBuffer1 The buffer that will be passed to the shaderProgram as a texture input. 
     * @param targetBuffer What the shaderProgram will render to.
     */
    void renderFragmentShaderToTargetBufferWithOneInputBuffer(GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer targetBuffer);

    /**
     * Renders a fragment/vertex shader shader program to the targetBuffer at it's resolution.
     * 
     * @param shaderProgram The shader that will be used to render to the targetBuffer.
     * @param inputBuffer1 The first buffer that will be passed to the shaderProgram as a texture input.
     * @param inputBuffer2 The second buffer that will be passed to the shaderProgram as a texture input.
     * @param targetBuffer What the shaderProgram will render to.
     */
    void renderFragmentShaderToTargetBufferWithTwoInputBuffers(GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer inputBuffer2, FrameBuffer targetBuffer);

    /**
     * Uses a ping-pong method to switch between frameBuffer1 and frameBuffer2 to perform operations that require multiple passes.
     * 
     * @param numberOfPassses How many passes the shader will run for. Switches between frameBuffer1 and frameBuffer2 until the last pass where it renders to targetBuffer
     * @param multiPassShaderProgram The shader that will be ran for the buffers.
     * @param frameBuffer1 The initial buffer that is used as an input for the shader.
     * @param frameBuffer2 The intermediate buffer that is first outputed to the shader.
     * @param targetBuffer The final buffer that the last pass will output to.
     */
    void renderMultipassFragmentShaderToTargetBuffer(int numberOfPasses, GLuint multiPassShaderProgram, FrameBuffer frameBuffer1, FrameBuffer frameBuffer2, FrameBuffer targetBuffer);

    /**
     * Creates a quad for rendering to the whole window.
     */
    void createRenderQuad();

    /**
     * Finds a floating point uniform in our shaders and passes the floating point variable to it.
     * 
     * @param shader The GLSL shader program to update the variable in.
     * @param variable The floating point number to send to the shader.
     * @param variableNameInShader The name of the variable in our GLSL shader that our float will be assigned to.
     */
    void updateFloatInShader(GLuint shader, float variable, const char variableNameInShader[24]);

    /**
     * Updates the time variable in the shader.
     */
    void updateTimeInShader(GLuint shader);

    /**
     * Updates the camera position and direction in the shader.
     */
    void updateCameraInShader(GLuint shader);

    /**
     * Updates the resolution in the shader.
     * 
     * @param window The GLFW window to get the framebuffer size from.
     */
    void updateResolutionInShader(GLuint shader, GLFWwindow* window);

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
     * Creates an OpenGL shader program for a vertex and fragment shader.
     * 
     * @param vertexPath The file path of our vertex shader.
     * @param fragmentPath The file path of our fragment shader.
     * 
     * @return A GLuint shader program with our compiled vertex and fragment shaders.
     */
    GLuint compileVertexAndFragmentShaders(const char* vertexPath, const char* fragmentPath);
}

#endif
