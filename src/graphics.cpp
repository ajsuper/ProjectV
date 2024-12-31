// Last edited on: 31-12-2024

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
    // Global variables
    GLuint shaderProgram = 0;
    GLuint VAO, VBO;  // Vertex Array Object and Vertex Buffer Object

    /**
     * Renders a frame to the given window.
     * 
     * @param window The GLFW window to render to.
     * @param swapInterval The interval for swapping buffers.
     */
    void renderFrame(GLFWwindow* window, int swapInterval) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Use shader program and pass uniforms
        glUseProgram(shaderProgram);

        // Draw quad
        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        glfwSwapInterval(swapInterval);
        glfwSwapBuffers(window); 
  
        return;
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

    /**
     * Updates the camera position and direction in the shader.
     */
    void updateCameraInShader(){
        GLint cameraPosLoc = glGetUniformLocation(shaderProgram, "cameraPos");
        GLint cameraDirLoc = glGetUniformLocation(shaderProgram, "cameraDir");
        glUniform3fv(cameraPosLoc, 1, cam.position);
        glUniform3fv(cameraDirLoc, 1, cam.direction);

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
     * Compiles the vertex and fragment shaders and links them into a shader program.
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
}