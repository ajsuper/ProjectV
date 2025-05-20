#include "graphics/fbo.h"

namespace projv::graphics {
    void addTextureToFrameBuffer(FrameBuffer &frameBuffer, GLuint internalFormat, std::string textureName) {
        int width = frameBuffer.width; // Get framebuffer dimensions
        int height = frameBuffer.height;

        Texture customTexture;

        // Generate and bind the texture
        glGenTextures(1, &customTexture.textureID);
        glBindTexture(GL_TEXTURE_2D, customTexture.textureID);

        // Determine the appropriate base format and type
        GLenum baseFormat;
        GLenum type;

        switch (internalFormat) {
            case GL_R8:
            case GL_R16:
                baseFormat = GL_RED;
                type = GL_UNSIGNED_BYTE;
                break;

            case GL_R16F:
            case GL_R32F:
                baseFormat = GL_RED;
                type = GL_FLOAT;
                break;

            case GL_RG8:
            case GL_RG16:
                baseFormat = GL_RG;
                type = GL_UNSIGNED_BYTE;
                break;

            case GL_RG16F:
            case GL_RG32F:
                baseFormat = GL_RG;
                type = GL_FLOAT;

            case GL_RGB8:
            case GL_RGB16:
                baseFormat = GL_RGB;
                type = GL_UNSIGNED_BYTE;
                break;

            case GL_RGB16F:
            case GL_RGB32F:
                baseFormat = GL_RGB;
                type = GL_FLOAT;
                break;

            case GL_RGBA8:
            case GL_RGBA16:
                baseFormat = GL_RGBA;
                type = GL_UNSIGNED_BYTE;
                break;

            case GL_RGBA16F:
            case GL_RGBA32F:
                baseFormat = GL_RGBA;
                type = GL_FLOAT;
                break;

            default:
                core::error("[addTextureToFrameBuffer] Unsupported format: {}. Supported formats are: GL_R8, GL_R16F, GL_R32F, GL_RGB8, GL_RGB16, GL_RGB16F, GL_RGB32F, GL_RGBA8, GL_RGBA16, GL_RGBA16F, GL_RGBA32F.", internalFormat);
                return; // Exit the function on unsupported format
        }

        // Specify the texture image
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, baseFormat, type, nullptr);

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Assign properties to the customTexture object and add it to the buffers texture list.
        customTexture.name = textureName;
        customTexture.format = internalFormat;
        frameBuffer.textures.push_back(customTexture);

        // Get the number of textures in our frame buffer object
        int numberOfTextures = frameBuffer.textures.size();

        // Create our color attachments array and bind our buffer.
        GLenum drawBuffers[numberOfTextures];
        glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer.buffer);

        // Re-bind all of the textures.
        for (int i = 0; i < numberOfTextures; i++) {
            Texture texture = frameBuffer.textures[i];                                                             // Fetch the texture
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, texture.textureID, 0); // Set this textureID to a color attachment.
            drawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;

            core::info("[addTextureToFrameBuffer] Texture {} added at location {}", texture.name, i);
        }

        glDrawBuffers(numberOfTextures, drawBuffers);

        // Unbind the texture and framebuffer.
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        return;
    }

    FrameBuffer createFrameBufferObject(int width, int height) {
        FrameBuffer FBO;
        glGenFramebuffers(1, &FBO.buffer);
        FBO.width = width;
        FBO.height = height;
        return FBO;
    }

    FrameBuffer createDefaultFrameBufferObject(int width, int height) {
        FrameBuffer FBO;
        glGenFramebuffers(1, &FBO.buffer);
        FBO.width = width;
        FBO.height = height;
        // Voxel identity
        addTextureToFrameBuffer(FBO, GL_RG32F, "voxelIdentity");
        // Surface Info
        addTextureToFrameBuffer(FBO, GL_RGBA32F, "surfaceInfo");
        // Color
        addTextureToFrameBuffer(FBO, GL_RGBA16F, "color");
        return FBO;
    }

    FrameBuffer createWindowFrameBufferObject() {
        FrameBuffer FBO;
        FBO.buffer = 0;
        return FBO;
    }

    void addFramebufferToRenderInstance(RenderInstance& renderInstance, const FrameBuffer& framebuffer, const std::string& name) {
        renderInstance.frameBufferObjects[name] = framebuffer;
    }

    void removeFramebufferFromRenderInstance(RenderInstance& renderInstance, const std::string& name) {
        renderInstance.frameBufferObjects.erase(name);
    }

    FrameBuffer& getFramebufferFromRenderInstance(RenderInstance& renderInstance, const std::string& name) {
        auto it = renderInstance.frameBufferObjects.find(name);
        if (it != renderInstance.frameBufferObjects.end()) {
            return it->second;
        } else {
            core::warn("[getFramebufferFromRenderInstance] Frame buffer doesn't exist: {}. Returning a fallback framebuffer.", name);
            FrameBuffer fallback = createFrameBufferObject(1, 1); // Return a default 1x1 framebuffer
            fallback.buffer = -1;
            fallback.height = -1;
            fallback.width = -1;
            return fallback;
        }
    }
}