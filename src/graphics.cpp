// Last edited on: 31-12-2024.

#include "graphics.h"
#include "camera.h"
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
    int frameCount = 0;
    // Global variables
    GLuint shaderProgram;
    GLuint postProcessingShaderProgram;
    GLuint VAO, VBO;  // Vertex Array Object and Vertex Buffer Object
    GLuint FBO, colorBuffer, depthBuffer, normalBuffer, albedoBuffer;
    GLuint FBO2, colorBuffer2, depthBuffer2, normalBuffer2, albedoBuffer2;

    
    void createShaders(){
        shaderProgram = glCreateProgram();
        postProcessingShaderProgram = glCreateProgram();
        return;
    }

    void createPostProcessingFrameBuffers(GLFWwindow* window){
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        glGenFramebuffers(1, &FBO);

        // Create our color buffer.
        glGenTextures(1, &colorBuffer);
        glBindTexture(GL_TEXTURE_2D, colorBuffer);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); 

        // Create our depth buffer.
        glGenTextures(1, &depthBuffer);
        glBindTexture(GL_TEXTURE_2D, depthBuffer);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width, height, 0, GL_RED, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); 
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Create our normal buffer.
        glGenTextures(1, &normalBuffer);
        glBindTexture(GL_TEXTURE_2D, normalBuffer);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, width, height, 0, GL_RGB, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); 
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        //Create our color buffer.
        glGenTextures(1, &albedoBuffer);
        glBindTexture(GL_TEXTURE_2D, albedoBuffer);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); 

        //// Bind the framebuffer and attach the color buffer, depth buffer, and normal buffer.
        glBindFramebuffer(GL_FRAMEBUFFER, FBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorBuffer, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, depthBuffer, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, normalBuffer, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, albedoBuffer, 0);

        // Tell OpenGL which color attachments we'll use (of this framebuffer) for rendering.
        GLenum drawBuffers[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
        glDrawBuffers(4, drawBuffers);

        if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){
            std::cerr << "Framebuffer is not complete!" << std::endl;
        }

        // Unbind the framebuffer.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Generate a second version of the first FBO for multipass rendering.
        glGenFramebuffers(1, &FBO2);

        glGenTextures(1, &colorBuffer2);
        glBindTexture(GL_TEXTURE_2D, colorBuffer2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glGenTextures(1, &depthBuffer2);
        glBindTexture(GL_TEXTURE_2D, depthBuffer2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width, height, 0, GL_RED, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glGenTextures(1, &normalBuffer2);
        glBindTexture(GL_TEXTURE_2D, normalBuffer2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, width, height, 0, GL_RGB, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // Create our color buffer.
        glGenTextures(1, &albedoBuffer2);
        glBindTexture(GL_TEXTURE_2D, albedoBuffer2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glBindFramebuffer(GL_FRAMEBUFFER, FBO2);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorBuffer2, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, depthBuffer2, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, normalBuffer2, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, albedoBuffer2, 0);

        GLenum drawBuffers2[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
        glDrawBuffers(4, drawBuffers2);

        //GLenum drawBuffers2[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
        //glDrawBuffers(3, drawBuffers2);

        if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){
            std::cerr << "Framebuffer2 is not complete!" << std::endl;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    /**
     * Renders a frame to the given window.
     * 
     * @param window The GLFW window to render to.
     * @param swapInterval The interval for swapping buffers.
     */
    void renderFrame(GLFWwindow* window, int swapInterval) {
        // Bind the framebuffer and clear the color and depth buffers for the geometry pass
        glBindFramebuffer(GL_FRAMEBUFFER, FBO);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Use the shader program for the geometry pass
        glUseProgram(shaderProgram);

        // Check for OpenGL errors (useful for debugging)
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "OpenGL error after shader program usage: " << error << std::endl;
        }

        // Draw the quad for the geometry pass
        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        // Unbind the framebuffer
        int numberOfPasses = 10; // Example: Adjust based on your needs

        // Multi-Pass denoiser.
        for (int pass = 0; pass < numberOfPasses; ++pass) {

            // Decide which framebuffer to bind
            if (pass < numberOfPasses - 1) {
                // Alternate between FBO and FBO2 for intermediate passes
                if (pass % 2 == 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, FBO2);
                } else {
                    glBindFramebuffer(GL_FRAMEBUFFER, FBO);
                }
            } else {
                // For the final pass, render to the default framebuffer
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }

            // Clear the framebuffer
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Use the post-processing shader program
            glUseProgram(postProcessingShaderProgram);

            GLint passLocation = glGetUniformLocation(postProcessingShaderProgram, "PassNumber");
            glUniform1i(passLocation, pass);

            // Bind the quad VAO
            glBindVertexArray(VAO);

            // Bind textures
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, (pass % 2 == 0) ? colorBuffer : colorBuffer2);
            glUniform1i(glGetUniformLocation(postProcessingShaderProgram, "screenTexture"), 0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, (pass % 2 == 0) ? depthBuffer : depthBuffer2);
            glUniform1i(glGetUniformLocation(postProcessingShaderProgram, "depthTexture"), 1);

            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, (pass % 2 == 0) ? normalBuffer : normalBuffer2);
            glUniform1i(glGetUniformLocation(postProcessingShaderProgram, "normalTexture"), 2);

            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, (pass % 2 == 0) ? albedoBuffer : albedoBuffer2);
            glUniform1i(glGetUniformLocation(postProcessingShaderProgram, "albedoTexture"), 3);

            // Draw the full-screen quad
            glDrawArrays(GL_TRIANGLES, 0, 6);

            // Unbind VAO
            glBindVertexArray(0);
        }

        // Swap buffers and set the swap interval
        glfwSwapInterval(swapInterval);
        glfwSwapBuffers(window);
        frameCount++;
    }


    /**
     * Creates a quad for rendering to the whole window.
     */
    void createRenderQuad() {
        // Define vertices for a window filling quad
        float vertices[] = {
            // positions   // texCoords
            -1.0f, -1.0f,  0.0f, 0.0f,
            1.0f, -1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f,  0.0f, 1.0f,
            -1.0f,  1.0f,  0.0f, 1.0f,
            1.0f, -1.0f,  1.0f, 0.0f,
            1.0f,  1.0f,  1.0f, 1.0f
        };

        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);

        // Bind and set buffer data
        glBindVertexArray(VAO);

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        // Position attribute (location = 0)
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        // Texture Coord attribute (location = 1)
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glBindVertexArray(0);

        return;
    }

    void updateTimeInShader(){
        // Ensure the correct shader program is bound
        if (glIsProgram(shaderProgram)) {
            glUseProgram(shaderProgram);
        } else {
            std::cerr << "Invalid shader program" << std::endl;
            return;
        }

        // Get uniform location
        GLint timeLoc = glGetUniformLocation(shaderProgram, "time");

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "OpenGL error after glUseProgram: " << error << std::endl;
        }

        // Check if the uniform location is valid
        if (timeLoc == -1) {
            std::cerr << "Failed to get uniform location for time" << std::endl;
            return;
        }

        // Update the uniform
        glUniform1f(timeLoc, float(frameCount));

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "OpenGL error after glUniform1f: " << error << std::endl;
        }

        return;
    }
    /**
     * Updates the camera position and direction in the shader.
     */
    void updateCameraInShader(){
        // Ensure the correct shader program is bound
        if (glIsProgram(shaderProgram)) {
            glUseProgram(shaderProgram);
        } else {
            std::cerr << "Invalid shader program" << std::endl;
            return;
        }

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "OpenGL error after glUseProgram: " << error << std::endl;
        }

        // Get uniform locations
        GLint cameraPosLoc = glGetUniformLocation(shaderProgram, "cameraPos");
        GLint cameraDirLoc = glGetUniformLocation(shaderProgram, "cameraDir");

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "OpenGL error after glGetUniformLocation: " << error << std::endl;
        }

        // Check if the uniform locations are valid
        if (cameraPosLoc == -1) {
            std::cerr << "Failed to get uniform location for cameraPos" << std::endl;
            //return;
        }
        if (cameraDirLoc == -1) {
            std::cerr << "Failed to get uniform location for cameraDir" << std::endl;
            //return;
        }

        // Update the uniforms
        glUniform3fv(cameraPosLoc, 1, cam.position);
        glUniform3fv(cameraDirLoc, 1, cam.direction);

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "OpenGL error after glUniform3fv: " << error << std::endl;
        }

        return;
    }

    /**
     * Updates the resolution in the shader.
     * 
     * @param window The GLFW window to get the framebuffer size from.
     */
    void updateResolutionInShader(GLFWwindow* window){
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        
        // Pass the resolution uniform
        GLint resolutionLoc = glGetUniformLocation(shaderProgram, "resolution");
        float res[2] = { static_cast<float>(width), static_cast<float>(height) };
        glUniform2fv(resolutionLoc, 1, res);

        return;
    }

    /**
     * Loads GLSL shader source code from a file.
     * 
     * @param filepath The path to the shader file.
     * @return The shader source code as a string.
     */
    std::string loadShaderSource(const char* filepath) {
        std::ifstream shaderFile;
        shaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        try {
            shaderFile.open(filepath);
            std::stringstream shaderStream;
            shaderStream << shaderFile.rdbuf();
            shaderFile.close();
            return shaderStream.str();
        }
        catch (std::ifstream::failure& e) {
            std::cout << "Shader Loading ERROR. Failed to load: " << filepath << std::endl;
            return "";
        }
        return "";
    }

    /**
     * Passes octree data to the fragment shader.
     * 
     * @param octree The octree data to pass.
     */
    void passOctreeToFrag(std::vector<uint32_t> octree) {
        GLuint ssbo;
        glGenBuffers(1, &ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, octree.size() * sizeof(uint32_t), octree.data(), GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo);

        return;
    }

    /**
     * Passes voxel type data to the fragment shader.
     * 
     * @param voxelTypeData The voxel type data to pass.
     */
    void passVoxelTypeDataToFrag(std::vector<uint32_t> voxelTypeData) {
        GLuint ssbo;
        glGenBuffers(1, &ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, voxelTypeData.size() * sizeof(uint32_t), voxelTypeData.data(), GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssbo);

        return;
    }

    /**
     * Compiles the vertex and post processing shaders and links them into a shader program.
     */
    void compileAndUseVertexAndFragShader() {
        // Load shader code
        std::string vertexCode = loadShaderSource("./shaders/vertexShader.glsl");
        std::string fragmentCode = loadShaderSource("./shaders/fragmentShader.frag");

        // Convert to C strings
        const char* vertexShaderSource = vertexCode.c_str();
        const char* octreeRayMarchShaderSource = fragmentCode.c_str();

        GLint success;
        GLchar infoLog[512];
        
        // Compile vertex shader
        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
        glCompileShader(vertexShader);
        // Check for compilation errors
        glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
            std::cerr << "Vertex Shader compilation failed:\n" << infoLog << std::endl;
        }

        // Compile fragment shader
        GLuint octreeRayMarchShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(octreeRayMarchShader, 1, &octreeRayMarchShaderSource, NULL);
        glCompileShader(octreeRayMarchShader);
        // Check for compilation errors
        glGetShaderiv(octreeRayMarchShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(octreeRayMarchShader, 512, NULL, infoLog);
            std::cerr << "Fragment Shader compilation failed:\n" << infoLog << std::endl;
        }

        // Link shaders into a program
        shaderProgram = glCreateProgram();
        glAttachShader(shaderProgram, vertexShader);
        glAttachShader(shaderProgram, octreeRayMarchShader);
        glLinkProgram(shaderProgram);
        // Check for linking errors
        glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
            std::cerr << "Shader Program linking failed:\n" << infoLog << std::endl;
        }

        // Clean up shaders
        glDeleteShader(vertexShader);
        glDeleteShader(octreeRayMarchShader);

        return;
    }

    void compileAndUseVertexAndPostProcessingShader() {
        // Load shader code
        std::string vertexCode = loadShaderSource("./shaders/vertexShader.glsl");
        std::string fragmentCode = loadShaderSource("./shaders/postProcessing.frag");

        // Convert to C strings
        const char* vertexShaderSource = vertexCode.c_str();
        const char* postProcessingShaderSource = fragmentCode.c_str();

        GLint success;
        GLchar infoLog[512];
        
        // Compile vertex shader
        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
        glCompileShader(vertexShader);
        // Check for compilation errors
        glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
            std::cerr << "Vertex Shader compilation failed:\n" << infoLog << std::endl;
        }

        // Compile fragment shader
        GLuint postProcessingShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(postProcessingShader, 1, &postProcessingShaderSource, NULL);
        glCompileShader(postProcessingShader);
        // Check for compilation errors
        glGetShaderiv(postProcessingShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(postProcessingShader, 512, NULL, infoLog);
            std::cerr << "Fragment Shader compilation failed:\n" << infoLog << std::endl;
        }

        // Link shaders into a program
        postProcessingShaderProgram = glCreateProgram();
        glAttachShader(postProcessingShaderProgram, vertexShader);
        glAttachShader(postProcessingShaderProgram, postProcessingShader);
        glLinkProgram(postProcessingShaderProgram);
        // Check for linking errors
        glGetProgramiv(postProcessingShaderProgram, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(postProcessingShaderProgram, 512, NULL, infoLog);
            std::cerr << "Shader Program linking failed:\n" << infoLog << std::endl;
        }

        // Clean up shaders
        glDeleteShader(vertexShader);
        glDeleteShader(postProcessingShader);

        return;
    }
}