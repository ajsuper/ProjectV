#include "graphics/render.h"

namespace projv::graphics {
    void renderFragmentShaderToTargetBuffer(RenderInstance renderInstance, GLuint shaderProgram, FrameBuffer targetBuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(shaderProgram);

        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            core::error("OpenGL error after shader program usage: {}", error);
        }

        glBindVertexArray(renderInstance.VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        return;
    }

    void renderFragmentShaderToTargetBufferWithOneInputBuffer(RenderInstance renderInstance, GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer targetBuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);

        // Clear the framebuffer
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Use the shader program
        glUseProgram(shaderProgram);

        // Bind the quad VAO
        glBindVertexArray(renderInstance.VAO);

        for (size_t i = 0; i < inputBuffer1.textures.size(); i++) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, inputBuffer1.textures[i].textureID);
            std::string fullTextureName = "inputBuffer1_" + inputBuffer1.textures[i].name;
            glUniform1i(glGetUniformLocation(shaderProgram, fullTextureName.c_str()), i);
        }

        // Draw the full-screen quad
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Unbind VAO
        glBindVertexArray(0);

        return;
    }

    void renderFragmentShaderToTargetBufferWithTwoInputBuffers(RenderInstance renderInstance, GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer inputBuffer2, FrameBuffer targetBuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);

        // Clear the framebuffer
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Use the shader program
        glUseProgram(shaderProgram);

        // Bind the quad VAO
        glBindVertexArray(renderInstance.VAO);

        for (size_t i = 0; i < inputBuffer1.textures.size(); i++) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, inputBuffer1.textures[i].textureID);
            std::string fullTextureName = "inputBuffer1_" + inputBuffer1.textures[i].name;
            glUniform1i(glGetUniformLocation(shaderProgram, fullTextureName.c_str()), i);
        }

        for (size_t i = 0; i < inputBuffer2.textures.size(); i++) {
            glActiveTexture(GL_TEXTURE0 + i + inputBuffer1.textures.size());
            glBindTexture(GL_TEXTURE_2D, inputBuffer2.textures[i].textureID);
            std::string fullTextureName = "inputBuffer2_" + inputBuffer2.textures[i].name;
            glUniform1i(glGetUniformLocation(shaderProgram, fullTextureName.c_str()), inputBuffer1.textures.size() + i);
        }

        // Draw the full-screen quad
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Unbind VAO
        glBindVertexArray(0);

        return;
    }

    void renderMultipassFragmentShaderToTargetBuffer(RenderInstance renderInstance, int numberOfPasses, GLuint multiPassShaderProgram, FrameBuffer frameBuffer1, FrameBuffer frameBuffer2, FrameBuffer targetBuffer) {
        // Ensure the frame buffers have the same texture attachments.
        if (frameBuffer1.textures.size() != frameBuffer2.textures.size()) {
            core::error("[renderMultipassFragmentShaderToTargetBuffer] Mismatch in number of texture attachments between inputBuffer1 and inputBuffer2");
        }
        for (size_t i = 0; i < frameBuffer1.textures.size(); i++) {
            if (frameBuffer1.textures[i].name != frameBuffer2.textures[i].name) {
                core::error("[renderMultipassFragmentShaderToTargetBuffer] Mismatch between textures. frameBuffer1 contains {} while frameBuffer2 contains {}", frameBuffer1.textures[i].name, frameBuffer2.textures[i].name);
            }
        }

        // Multi-Pass denoiser.
        for (int pass = 0; pass < numberOfPasses; ++pass) {
            // Decide which framebuffer to bind
            if (pass < numberOfPasses - 1) {
                // Alternate between FBO and FBO2 for intermediate passes
                if (pass % 2 == 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer2.buffer);
                }
                else {
                    glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer1.buffer);
                }
            } else {
                // For the final pass, render to the default framebuffer
                glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);
            }

            // Clear the framebuffer
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Use the multiPassShaderProgram shader program
            glUseProgram(multiPassShaderProgram);

            GLint passLocation = glGetUniformLocation(multiPassShaderProgram, "PassNumber");
            glUniform1i(passLocation, pass);

            // Bind the quad VAO
            glBindVertexArray(renderInstance.VAO);

            // Bind textures
            FrameBuffer activeFrameBuffer = (pass % 2 == 0) ? frameBuffer1 : frameBuffer2;

            for (size_t i = 0; i < activeFrameBuffer.textures.size(); i++) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, activeFrameBuffer.textures[i].textureID);
                std::string fullTextureName = "inputBuffer1_" + activeFrameBuffer.textures[i].name;
                glUniform1i(glGetUniformLocation(multiPassShaderProgram, fullTextureName.c_str()), i);
            }

            // Draw the full-screen quad
            glDrawArrays(GL_TRIANGLES, 0, 6);

            // Unbind VAO
            glBindVertexArray(0);
        }
    }
}