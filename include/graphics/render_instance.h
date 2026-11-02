#ifndef PROJV_RENDER_INSTANCE_H
#define PROJV_RENDER_INSTANCE_H



#include <string>
#include <unordered_map>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
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
    /** The view endFrame copies an unconsumed frame texture to the window in: under every overlay's. */
    inline constexpr bgfx::ViewId FRAME_COPY_VIEW = 127;


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
            * by setting Application::closeRequested to end the loop, but a tool with unsaved work
            * may want to raise a prompt instead, which it could not do if the engine closed the
            * window on its behalf. Note that projv::core cannot see this type at all: core has no
            * dependency on graphics, so the hand-off is the application's to make.
            */
            bool shouldClose = false;

            /**
            * Whether renderConstructedRenderer calls glfwPollEvents itself. True by default, which
            * is how every application without the platform system gets its events at all.
            * graphics::installPlatform clears it and polls at the top of the frame instead, in
            * PreUpdate, where Input is filled.
            */
            bool pollEventsInRender = true;

            /**
            * Whether the window may be resized, read by initialize. True by default. False asks the
            * window manager for exactly the size given -- a tiling one floats a fixed-size window
            * rather than fitting it into the layout -- so unattended captures and benchmarks render at
            * the same size every run.
            */
            bool resizable = true;

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

            /**
            * Something drawn on top of the frame -- a UI layer -- after the renderer's passes and before
            * the frame is submitted. It is handed the back buffer's size, and draws in bgfx views of
            * its own numbered above every renderer's passes (ui::USER_VIEW, ui::DEV_VIEW).
            */
            using Overlay = std::function<void(core::ivec2 backBufferSize)>;

            /**
            * Adds an overlay. Overlays run in ascending `order`, and in the order added within one.
            * Run by graphics::endFrame, which renderConstructedRenderer ends with.
            */
            void addOverlay(int order, Overlay overlay);

            /** Runs every overlay, in order. graphics::endFrame calls this; a program that owns its frame can too. */
            void drawOverlays(core::ivec2 backBufferSize);

            // ---- The frame as a texture (docs/plans/ui.md, D18) ----------------------------------------
            // While wanted, the renderer's passes that write the window (frameBufferOutputID -1) write
            // an engine texture of the back buffer's size instead, so something drawn over the frame
            // can read it: the UI's backdrop filters, which blur what is behind a panel. Whoever reads
            // it writes the window from it and says so (markFrameConsumed); a frame nobody consumed is
            // copied to the window by endFrame, in FRAME_COPY_VIEW, below every overlay's views.

            /** Asks for the frame as a texture from the next pass that writes the window on. */
            void wantFrameTexture(bool want) { frameWanted = want; }
            /** Whether this frame's window passes went to frameTexture(). */
            bool frameRedirected() const { return redirected; }
            bgfx::TextureHandle frameTexture() const { return frameColour; }
            /** The window has been written from frameTexture() this frame: endFrame need not copy it. */
            void markFrameConsumed() { consumed = true; }
            /**
            * The framebuffer a window pass should write at `size`: the frame texture's when wanted (made
            * or remade at that size), else BGFX_INVALID_HANDLE, the back buffer. performRenderPasses
            * calls it.
            */
            bgfx::FrameBufferHandle windowTarget(core::ivec2 size);
            /** Copies an unconsumed redirected frame to the window, and starts the next frame. endFrame calls it. */
            void finishFrame(core::ivec2 backBufferSize);
            /** Frees the frame texture: before bgfx goes. */
            void releaseFrameTexture();

            RenderInstance() = default;
        private:
            bool frameWanted = false, redirected = false, consumed = false;
            core::ivec2 frameSize{0, 0};
            bgfx::TextureHandle frameColour = BGFX_INVALID_HANDLE;
            bgfx::FrameBufferHandle frameTarget = BGFX_INVALID_HANDLE;
            bgfx::ProgramHandle frameCopy = BGFX_INVALID_HANDLE;
            bgfx::UniformHandle frameSampler = BGFX_INVALID_HANDLE;

            std::unordered_map<uint32_t, RendererSpecification> renderers;
            std::shared_ptr<ConstructedRenderer> constructedRenderer;
            std::vector<std::pair<int, Overlay>> overlays;
    };
}

#endif
