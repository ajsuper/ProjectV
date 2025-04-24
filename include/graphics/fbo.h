#ifndef PROJECTV_FBO_H
#define PROJECTV_FBO_H

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <string>
#include <iostream>

#include "data_structures/framebuffer.h"
#include "data_structures/renderInstance.h"
#include "core/log.h"

namespace projv::graphics {
    /**
     * Adds a texture to the frameBuffer with the specified format and name.
     * 
     * @param frameBuffer The frame buffer to add the texture to.
     * @param format The format of the texture. (Prints an error when non-supported formats are used)
     * @param textureName The name of the texture. Used whenever you use the texture as a sampler2D in a shader. inputBuffer(buffer #)_ will pe prepended to the name specified when used as inputs.
     */
    void addTextureToFrameBuffer(FrameBuffer &frameBuffer, GLuint format, std::string textureName);

    /**
     * Creates a frame buffer object with the specified width and height.
     * 
     * @param width The width of the frame buffer object.
     * @param height The height of the frame buffer object.
     */
    FrameBuffer createFrameBufferObjectAdvanced(int width, int height);

    /**
     * Adds a framebuffer to our render instance.
     * 
     * @param renderInstance The RenderInstance to which the framebuffer will be added.
     * @param framebuffer The FrameBuffer object to be added.
     * @param name The name of the framebuffer to be used as a key for retrieval.
     */
    void addFramebufferToRenderInstance(RenderInstance& renderInstance, const FrameBuffer& framebuffer, const std::string& name);

    /**
     * Removes a framebuffer from our render instance.
     * 
     * @param renderInstance The RenderInstance from which the framebuffer will be removed.
     * @param name The name of the framebuffer to be removed.
     */
    void removeFramebufferFromRenderInstance(RenderInstance& renderInstance, const std::string& name);

    /**
     * Retrieves a framebuffer from our render instance.
     * 
     * @param renderInstance The RenderInstance from which the framebuffer will be retrieved.
     * @param name The name of the framebuffer to retrieve.
     * @return The FrameBuffer object associated with the given name.
     */
    FrameBuffer getFramebufferFromRenderInstance(RenderInstance& renderInstance, const std::string& name);
}

#endif