#ifndef PROJV_RENDER_INSTANCE_H
#define PROJV_RENDER_INSTANCE_H



#include <string>
#include <unordered_map>
#include <cstdint>
#include <memory>
#include <iostream>

#include "data_structures/rendererSpecification.h"
#include "data_structures/constructedRenderer.h"
#include "data_structures/posTexVertex.h"

#include "core/math.h"
#include "core/log.h"

#include "GLFW/glfw3.h"
#include <bgfx/platform.h>
// Define the correct platform-specific macro
#if defined(_WIN32)
    #define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
    #define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(__linux__)
    #if defined(PROJV_USE_X11)
        #define GLFW_EXPOSE_NATIVE_X11
    #else
        #define GLFW_EXPOSE_NATIVE_WAYLAND
    #endif
#else
    #error "Unsupported platform for GLFW native access"
#endif
#include "GLFW/glfw3native.h"

namespace projv::graphics {

    /**
     * Contains the logic and data for a bgfx renderer. Manages our window, default vertex buffer, and renderers/constructedRenderer.
     */
    class RenderInstance {
        public:
            GLFWwindow *window;
            bgfx::VertexBufferHandle vertexBuffer;
            bgfx::IndexBufferHandle indexBuffer;

            /**
            * True once the user has asked to close the window (the WM close button, Alt-F4, and so
            * on). Refreshed by renderConstructedRenderer, which is where GLFW events are polled.
            *
            * Nothing in the engine acts on this. An application reads it and decides -- typically
            * by setting Application::closeAppFlag to end the loop, but a tool with unsaved work
            * may want to raise a prompt instead, which it could not do if the engine closed the
            * window on its behalf. Note that projv::core cannot see this type at all: core has no
            * dependency on graphics, so the hand-off is the application's to make.
            */
            bool shouldClose = false;

            /**
            * Initializes our window and bgfx.
            * @param width The width of the window.
            * @param height The height of the window.
            * @param name The name of the window.
            */
            void initialize(int width, int height, std::string name);

            /**
            * Gets the resolution of the glfw window.
            * @return Returns a projv::core::ivec2 with our window's resolution.
            */
            core::ivec2 getWindowResolution();

            /**
            * Sets the constructed renderer. The RenderInstance only points to one because it is not a good idea to have unnecessary GPU resources.
            */
            void setActiveRenderer(const std::shared_ptr<ConstructedRenderer>& constructedRendererToUse);

            /**
            * Gets a pointer to the current constructed renderer stored in the render instance.
            * @return The std::shared_ptr<ConstructedRenderer> stored in the RenderInstance.
            */
            std::shared_ptr<ConstructedRenderer> getActiveRenderer();

            /**
            * Adds an additional projv::RendererSpecification to be stored in the render instance.
            * This function associates a renderer ID with its corresponding specification and constructs
            * the associated ConstructedRenderer object.
            * @param rendererID A unique identifier for this renderer configuration.
            * @param rendererSpecification The RendererSpecification object that defines the desired rendering setup.
            */
            void addRendererSpecification(uint32_t rendererID, const RendererSpecification &rendererSpecification);

            /**
            * Removes a previously added renderer specification from the render instance.
            * @param rendererID The unique identifier of the renderer configuration to be removed.
            */
            void removeRendererSpecification(uint32_t rendererID);

            /**
            * Retrieves a reference to the RendererSpecification associated with a given renderer ID.
            * @param rendererID The unique identifier of the desired renderer configuration.
            * @return A reference to the RendererSpecification object for that ID.
            */
            RendererSpecification &getRendererSpecification(uint32_t rendererID);

            RenderInstance() = default;
        private:
            std::unordered_map<uint32_t, RendererSpecification> renderers;
            std::shared_ptr<ConstructedRenderer> constructedRenderer;
    };
}

#endif
