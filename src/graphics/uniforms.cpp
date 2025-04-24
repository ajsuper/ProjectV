#include "graphics/uniforms.h"

namespace projv::graphics {
    void updateFloatInShader(GLuint shader, float variable, const char variableNameInShader[24]) {
        // Ensure the correct shader program is bound
        if (glIsProgram(shader)) {
            glUseProgram(shader);
        } else {
            core::error("Invalid shader program.");
            return;
        }

        // Get uniform location
        GLint floatLoc = glGetUniformLocation(shader, variableNameInShader);

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            core::error("OpenGL error after glUseProgram: {}", error);
        }

        // Check if the uniform location is valid
        if (floatLoc == -1) {
            core::error("Failed to get uniform location for {}", variableNameInShader);
            return;
        }

        // Update the uniform
        glUniform1f(floatLoc, variable);

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR) {
            core::error("OpenGL error after glUniform1f: {}", error);
        }

        return;
    }

    void updateCameraInShader(GLuint shader) {
        // Ensure the correct shader program is bound
        if (glIsProgram(shader)) {
            glUseProgram(shader);
        } else {
            core::error("Invalid shader program.");
            return;
        }

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            core::error("OpenGL error after glUseProgram: {}", error);
        }

        // Get uniform locations
        GLint cameraPosLoc = glGetUniformLocation(shader, "cameraPos");
        GLint cameraDirLoc = glGetUniformLocation(shader, "cameraDir");

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR) {
            core::error("OpenGL error after glGetUniformLocation: {}", error);
        }

        // Check if the uniform locations are valid
        if (cameraPosLoc == -1) {
            core::error("Failed to get uniform location for cameraPos.");
            return;
        }
        if (cameraDirLoc == -1) {
            core::error("Failed to get uniform location for cameraDir.");
            return;
        }

        // Update the uniforms
        glUniform3fv(cameraPosLoc, 1, cam.position);
        glUniform3fv(cameraDirLoc, 1, cam.direction);

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR) {
            core::error("OpenGL error after glUniform3fv: {}", error);
        }
        
        return;
    }

    void updateResolutionInShader(GLuint shader, GLFWwindow *window) {
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);

        // Pass the resolution uniform
        GLint resolutionLoc = glGetUniformLocation(shader, "resolution");
        if (resolutionLoc == -1) {
            core::warn("Failed to get uniform location for resolution.");
            return;
        }

        float res[2] = {float(width), float(height)};
        glUniform2fv(resolutionLoc, 1, res);

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            core::error("OpenGL error after glUniform2fv: {}", error);
        }

        return;
    }
}