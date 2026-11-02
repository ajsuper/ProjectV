#include "graphics/render_instance.h"
#include "graphics/engine_shaders.h"

#include <algorithm>

namespace projv::graphics {
    void RenderInstance::setActiveRenderer(const std::shared_ptr<ConstructedRenderer>& constructedRendererToUse) {
        this->constructedRenderer = constructedRendererToUse;
    }

    std::shared_ptr<ConstructedRenderer> RenderInstance::getActiveRenderer() {
        return this->constructedRenderer;
    }

    void RenderInstance::addOverlay(int order, Overlay overlay) {
        // After every overlay of the same order or lower: equal orders run in the order added.
        auto at = std::find_if(overlays.begin(), overlays.end(),
                               [order](const std::pair<int, Overlay>& o) { return o.first > order; });
        overlays.insert(at, {order, std::move(overlay)});
    }

    void RenderInstance::drawOverlays(core::ivec2 backBufferSize) {
        for (auto& overlay : overlays) overlay.second(backBufferSize);
    }

    bgfx::FrameBufferHandle RenderInstance::windowTarget(core::ivec2 size) {
        if (!frameWanted || size.x <= 0 || size.y <= 0) return BGFX_INVALID_HANDLE;
        if (!bgfx::isValid(frameTarget) || frameSize != size) {
            if (bgfx::isValid(frameTarget)) bgfx::destroy(frameTarget);   // destroys its texture too
            frameColour = bgfx::createTexture2D(uint16_t(size.x), uint16_t(size.y), false, 1, bgfx::TextureFormat::RGBA8,
                                                BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            frameTarget = bgfx::createFrameBuffer(1, &frameColour, true);
            frameSize = size;
        }
        redirected = true;
        return frameTarget;
    }

    void RenderInstance::finishFrame(core::ivec2 size) {
        if (redirected && !consumed && bgfx::isValid(frameColour)) {
            if (!bgfx::isValid(frameCopy)) {
                bgfx::ShaderHandle vs = loadEngineShader("vs_quad");
                bgfx::ShaderHandle fs = loadEngineShader("frame_copy");
                if (bgfx::isValid(vs) && bgfx::isValid(fs)) frameCopy = bgfx::createProgram(vs, fs, true);
                frameSampler = bgfx::createUniform("s_frame", bgfx::UniformType::Sampler);
            }
            if (bgfx::isValid(frameCopy)) {
                bgfx::setViewName(FRAME_COPY_VIEW, "frame copy");
                bgfx::setViewFrameBuffer(FRAME_COPY_VIEW, BGFX_INVALID_HANDLE);
                bgfx::setViewRect(FRAME_COPY_VIEW, 0, 0, uint16_t(size.x), uint16_t(size.y));
                bgfx::setViewTransform(FRAME_COPY_VIEW, nullptr, nullptr);
                bgfx::setVertexBuffer(0, vertexBuffer);
                bgfx::setIndexBuffer(indexBuffer);
                bgfx::setTexture(0, frameSampler, frameColour);
                bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
                bgfx::submit(FRAME_COPY_VIEW, frameCopy);
            }
        }
        redirected = false;
        consumed = false;
    }

    void RenderInstance::releaseFrameTexture() {
        if (bgfx::isValid(frameTarget)) bgfx::destroy(frameTarget);
        if (bgfx::isValid(frameCopy)) bgfx::destroy(frameCopy);
        if (bgfx::isValid(frameSampler)) bgfx::destroy(frameSampler);
        frameTarget = BGFX_INVALID_HANDLE;
        frameColour = BGFX_INVALID_HANDLE;
        frameCopy = BGFX_INVALID_HANDLE;
        frameSampler = BGFX_INVALID_HANDLE;
        frameWanted = redirected = consumed = false;
    }

    void RenderInstance::addRendererSpecification(uint32_t rendererID, const RendererSpecification &rendererSpecification) {
        this->renderers[rendererID] = rendererSpecification;
    }

    void RenderInstance::removeRendererSpecification(uint32_t rendererID) {
        this->renderers.erase(rendererID);
    }

    RendererSpecification &RenderInstance::getRendererSpecification(uint32_t rendererID) {
        return this->renderers.at(rendererID);
    }

    void RenderInstance::initialize(int width, int height, std::string name) {
#if defined(__linux__)
    #if defined(PROJV_USE_X11)
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    #else
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    #endif
#endif
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, resizable ? GLFW_TRUE : GLFW_FALSE);
        GLFWwindow *window = glfwCreateWindow(width, height, name.c_str(), NULL, NULL);

        bgfx::PlatformData platformData;
#if defined(__APPLE__)
            platformData.nwh = glfwGetCocoaWindow(window);
            platformData.type = bgfx::NativeWindowHandleType::Default;
#elif defined(_WIN32)
            platformData.nwh = glfwGetWin32Window(window);
            platformData.type = bgfx::NativeWindowHandleType::Default;
            if (platformData.nwh == NULL) {
                core::error("ERROR: GLFW native window handle returned NULL.");
            }
#elif defined(__linux__)
    #if defined(PROJV_USE_X11)
            platformData.ndt = glfwGetX11Display();
            platformData.nwh = (void*)(uintptr_t)glfwGetX11Window(window);
            platformData.context = NULL;
            platformData.type = bgfx::NativeWindowHandleType::Default;
    #else
            platformData.ndt = glfwGetWaylandDisplay();
            platformData.nwh = glfwGetWaylandWindow(window);
            platformData.context = NULL;
            platformData.type = bgfx::NativeWindowHandleType::Wayland;
    #endif
            if (platformData.nwh == NULL) {
                core::error("ERROR: GLFW native window handle returned NULL.");
            }
#endif
        // platformData.ndt = glfwGetCocoaDisplay();
        bgfx::setPlatformData(platformData);

        bgfx::Init bgfxInit;
#if defined(__APPLE__)
        bgfxInit.type = bgfx::RendererType::Metal;
#elif defined(__linux__) || defined(_WIN32)
        // The same SPIR-V shaders on Windows as on Linux (cmake/ProjectVShaders.cmake): one shader
        // backend, not a second one in HLSL.
        bgfxInit.type = bgfx::RendererType::Vulkan;
#endif
        bgfxInit.resolution.width = width;
        bgfxInit.resolution.height = height;
        bgfxInit.resolution.reset = BGFX_RESET_NONE;
        bgfxInit.platformData = platformData;

        bgfx::init(bgfxInit);

        static PosTexVertex vertexQuadVertices[] = {  
            {-1.0f,  1.0f, 0.0f, 0.0f, 0.0f},  // top-left: UV (0,0)  
            { 1.0f,  1.0f, 0.0f, 1.0f, 0.0f},  // top-right: UV (1,0)  
            {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f},  // bottom-left: UV (0,1)  
            { 1.0f, -1.0f, 0.0f, 1.0f, 1.0f},  // bottom-right: UV (1,1)  
        };

        // Two triangles to cover the screen
        static const uint16_t quadTriList[] = {
            0, 2, 1, // first triangle (top-left, bottom-left, top-right)
            1, 2, 3, // second triangle (top-right, bottom-left, bottom-right)
        };

        bgfx::VertexLayout pcvDecl;
        pcvDecl.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)  
            .end();

        this->vertexBuffer = bgfx::createVertexBuffer(bgfx::makeRef(vertexQuadVertices, sizeof(vertexQuadVertices)), pcvDecl);
        this->indexBuffer = bgfx::createIndexBuffer(bgfx::makeRef(quadTriList, sizeof(quadTriList)));

        this->window = window;
    }

    core::ivec2 RenderInstance::getWindowResolution() {
        int width;
        int height;
        glfwGetWindowSize(this->window, &width, &height);
        return core::ivec2(width, height);
    }
}
