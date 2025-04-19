#include "graphics/uniforms.h"

namespace projv::graphics {
    void updateFloatInShader(GLuint shader, float variable, const char variableNameInShader[24]) {
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
            std::cerr << "Failed to get uniform location for " << variableNameInShader << std::endl;
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

    void updateCameraInShader(GLuint shader) {
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

    void updateResolutionInShader(GLuint shader, GLFWwindow *window) {
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);

        // Pass the resolution uniform
        GLint resolutionLoc = glGetUniformLocation(shader, "resolution");
        float res[2] = {float(width), float(height)};
        glUniform2fv(resolutionLoc, 1, res);

        return;
    }
}