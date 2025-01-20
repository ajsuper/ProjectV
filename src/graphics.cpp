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
    GLuint VAO, VBO;  // Vertex Array Object and Vertex Buffer Object
    
    FrameBuffer createFrameBufferObject(int width, int height){
        FrameBuffer frameBufferObject;

        glGenFramebuffers(1, &frameBufferObject.buffer);

        // Create our color buffer.
        glGenTextures(1, &frameBufferObject.colorTexture);
        glBindTexture(GL_TEXTURE_2D, frameBufferObject.colorTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); 

        // Create our depth buffer.
        glGenTextures(1, &frameBufferObject.colorTexture2);
        glBindTexture(GL_TEXTURE_2D, frameBufferObject.colorTexture2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); 
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Create our normal buffer.
        glGenTextures(1, &frameBufferObject.normalTexture);
        glBindTexture(GL_TEXTURE_2D, frameBufferObject.normalTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, width, height, 0, GL_RGB, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); 
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        //Create our color buffer.
        glGenTextures(1, &frameBufferObject.positionTexture);
        glBindTexture(GL_TEXTURE_2D, frameBufferObject.positionTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, width, height, 0, GL_RGB, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); 

        // Bind the framebuffer and attach the color buffer, depth buffer, and normal buffer.
        glBindFramebuffer(GL_FRAMEBUFFER, frameBufferObject.buffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frameBufferObject.colorTexture, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, frameBufferObject.colorTexture2, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, frameBufferObject.normalTexture, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, frameBufferObject.positionTexture, 0);

        // Tell OpenGL which color attachments we'll use (of this framebuffer) for rendering.
        GLenum drawBuffers[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
        glDrawBuffers(4, drawBuffers);
 
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){
            std::cerr << "Framebuffer is not complete!" << std::endl;
        }

        // Unbind the framebuffer.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        return frameBufferObject;
    }

    void renderFragmentShaderToTargetBuffer(GLuint shaderProgram, FrameBuffer targetBuffer){
        glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(shaderProgram);

        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "OpenGL error after shader program usage: " << error << std::endl;
        }

        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        return;
    }

    void renderFragmentShaderToTargetBufferWithOneInputBuffer(GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer targetBuffer){
        glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);

        // Clear the framebuffer
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Use the post-processing shader program
        glUseProgram(shaderProgram);

        // Bind the quad VAO
        glBindVertexArray(VAO);

        // Bind textures for buffer 1
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, inputBuffer1.colorTexture);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer1ScreenTexture"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, inputBuffer1.colorTexture2);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer1ColorTexture2"), 1);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, inputBuffer1.normalTexture);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer1NormalTexture"), 2);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, inputBuffer1.positionTexture);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer1PositionTexture"), 3);

        // Draw the full-screen quad
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Unbind VAO
        glBindVertexArray(0);

        return;
    }

    void renderFragmentShaderToTargetBufferWithTwoInputBuffers(GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer inputBuffer2, FrameBuffer targetBuffer){
        glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);

        // Clear the framebuffer
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Use the post-processing shader program
        glUseProgram(shaderProgram);

        // Bind the quad VAO
        glBindVertexArray(VAO);

        // Bind textures for buffer 1
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, inputBuffer1.colorTexture);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer1ScreenTexture"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, inputBuffer1.colorTexture2);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer1ColorTexture2"), 1);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, inputBuffer1.normalTexture);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer1NormalTexture"), 2);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, inputBuffer1.positionTexture);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer1PositionTexture"), 3);

        // Bind textures for buffer 2
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, inputBuffer2.colorTexture);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer2ScreenTexture"), 4);

        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, inputBuffer2.colorTexture2);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer2ColorTexture2"), 5);

        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, inputBuffer2.normalTexture);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer2NormalTexture"), 6);

        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, inputBuffer2.positionTexture);
        glUniform1i(glGetUniformLocation(shaderProgram, "buffer2PositionTexture"), 7);

        // Draw the full-screen quad
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Unbind VAO
        glBindVertexArray(0);

        return;
    }
    

    void renderMultipassFragmentShaderToTargetBuffer(int numberOfPasses, GLuint multiPassShaderProgram, FrameBuffer frameBuffer1, FrameBuffer frameBuffer2, FrameBuffer targetBuffer){
        // Multi-Pass denoiser.
        for (int pass = 0; pass < numberOfPasses; ++pass) {

            // Decide which framebuffer to bind
            if (pass < numberOfPasses - 1) {
                // Alternate between FBO and FBO2 for intermediate passes
                if (pass % 2 == 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer2.buffer);
                } else {
                    glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer1.buffer);
                }
            } else {
                // For the final pass, render to the default framebuffer
                glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);
            }

            // Clear the framebuffer
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Use the post-processing shader program
            glUseProgram(multiPassShaderProgram);

            GLint passLocation = glGetUniformLocation(multiPassShaderProgram, "PassNumber");
            glUniform1i(passLocation, pass);

            // Bind the quad VAO
            glBindVertexArray(VAO);

            // Bind textures
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, (pass % 2 == 0) ? frameBuffer1.colorTexture : frameBuffer2.colorTexture);
            glUniform1i(glGetUniformLocation(multiPassShaderProgram, "screenTexture"), 0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, (pass % 2 == 0) ? frameBuffer1.colorTexture2 : frameBuffer2.colorTexture2);
            glUniform1i(glGetUniformLocation(multiPassShaderProgram, "colorTexture2"), 1);

            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, (pass % 2 == 0) ? frameBuffer1.normalTexture : frameBuffer2.normalTexture);
            glUniform1i(glGetUniformLocation(multiPassShaderProgram, "normalTexture"), 2);

            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, (pass % 2 == 0) ? frameBuffer1.positionTexture : frameBuffer2.positionTexture);
            glUniform1i(glGetUniformLocation(multiPassShaderProgram, "positionTexture"), 3);

            // Draw the full-screen quad
            glDrawArrays(GL_TRIANGLES, 0, 6);

            // Unbind VAO
            glBindVertexArray(0);
        }
    }

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

    void updateFloatInShader(GLuint shader, float variable, const char variableNameInShader[24]){
        // Ensure the correct shader program is bound
        if (glIsProgram(shader)) {
            glUseProgram(shader);
        } else {
            std::cerr << "Invalid shader program" << std::endl;
            return;
        }

        // Get uniform location
        GLint floatLoc = glGetUniformLocation(shader, variableNameInShader);

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "OpenGL error after glUseProgram: " << error << std::endl;
        }

        // Check if the uniform location is valid
        if (floatLoc == -1) {
            std::cerr << "Failed to get uniform location for time" << std::endl;
            return;
        }

        // Update the uniform
        glUniform1f(floatLoc, variable);

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "OpenGL error after glUniform1f: " << error << std::endl;
        }

        return;
    }

    void updateTimeInShader(GLuint shader){
        // Ensure the correct shader program is bound
        if (glIsProgram(shader)) {
            glUseProgram(shader);
        } else {
            std::cerr << "Invalid shader program" << std::endl;
            return;
        }

        // Get uniform location
        GLint timeLoc = glGetUniformLocation(shader, "time");

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

    void updateCameraInShader(GLuint shader){
        // Ensure the correct shader program is bound
        if (glIsProgram(shader)) {
            glUseProgram(shader);
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
        GLint cameraPosLoc = glGetUniformLocation(shader, "cameraPos");
        GLint cameraDirLoc = glGetUniformLocation(shader, "cameraDir");

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "OpenGL error after glGetUniformLocation: " << error << std::endl;
        }

        // Check if the uniform locations are valid
        if (cameraPosLoc == -1) {
            std::cerr << "Failed to get uniform location for cameraPos" << std::endl;
            return;
        }
        if (cameraDirLoc == -1) {
            std::cerr << "Failed to get uniform location for cameraDir" << std::endl;
            return;
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

    void updateResolutionInShader(GLuint shader, GLFWwindow* window){
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        
        // Pass the resolution uniform
        GLint resolutionLoc = glGetUniformLocation(shader, "resolution");
        float res[2] = { static_cast<float>(width), static_cast<float>(height) };
        glUniform2fv(resolutionLoc, 1, res);

        return;
    }

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

    void passOctreeToFrag(std::vector<uint32_t> octree) {
        GLuint ssbo;
        glGenBuffers(1, &ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, octree.size() * sizeof(uint32_t), octree.data(), GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo);

        return;
    }

    void passVoxelTypeDataToFrag(std::vector<uint32_t> voxelTypeData) {
        GLuint ssbo;
        glGenBuffers(1, &ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, voxelTypeData.size() * sizeof(uint32_t), voxelTypeData.data(), GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssbo);

        return;
    }

    GLuint compileVertexAndFragmentShaders(const char* vertexPath, const char* fragmentPath){
        GLuint shader = glCreateProgram();

        std::string vertCode = loadShaderSource(vertexPath);
        std::string fragCode = loadShaderSource(fragmentPath);

        const char* vertShaderSource = vertCode.c_str();
        const char* fragShaderSource = fragCode.c_str();

        GLint success;
        GLchar infoLog[512];
        
        // Compile vertex shader
        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &vertShaderSource, NULL);
        glCompileShader(vertexShader);
        // Check for compilation errors
        glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
            std::cerr << "Vertex Shader compilation failed:\n" << infoLog << std::endl;
        }

        // Compile fragment shader
        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &fragShaderSource, NULL);
        glCompileShader(fragmentShader);
        // Check for compilation errors
        glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
            std::cerr << "Fragment Shader compilation failed:\n" << infoLog << std::endl;
        }

        // Link shaders into a program
        glAttachShader(shader, vertexShader);
        glAttachShader(shader, fragmentShader);
        glLinkProgram(shader);
        // Check for linking errors
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(shader, 512, NULL, infoLog);
            std::cerr << "Shader Program linking failed:\n" << infoLog << std::endl;
        }

        // Clean up shaders
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        return shader;
    }

}