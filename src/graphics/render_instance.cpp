#include "graphics/render_instance.h"

namespace projv::graphics {
    void RenderInstance::setActiveRenderer(const std::shared_ptr<ConstructedRenderer>& constructedRendererToUse) {
        this->constructedRenderer = constructedRendererToUse;
    }

    std::shared_ptr<ConstructedRenderer> RenderInstance::getActiveRenderer() {
        return this->constructedRenderer;
    }

    void RenderInstance::addRendererSpecification(uint rendererID, const RendererSpecification &rendererSpecification) {
        this->renderers[rendererID] = rendererSpecification;
    }

    void RenderInstance::removeRendererSpecification(uint rendererID) {
        this->renderers.erase(rendererID);
    }

    RendererSpecification &RenderInstance::getRendererSpecification(uint rendererID) {
        return this->renderers.at(rendererID);
    }

    void RenderInstance::initialize(int width, int height, std::string name) {
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWwindow *window = glfwCreateWindow(width, height, name.c_str(), NULL, NULL);

        bgfx::PlatformData platformData;
#if defined(__APPLE__)
            platformData.nwh = glfwGetCocoaWindow(window);
            platformData.type = bgfx::NativeWindowHandleType::Default;
#elif defined(__linux__)
            platformData.nwh = glfwGetWaylandWindow(window);
            platformData.type = bgfx::NativeWindowHandleType::Wayland;
            platformData.ndt = glfwGetWaylandDisplay();
            if (platformData.nwh == NULL) {  
                core::error("ERROR: glfwGetWaylandWindow() returned NULL. GLFW may not be compiled with Wayland support.");  
            }
#endif
        // platformData.ndt = glfwGetCocoaDisplay();
        bgfx::setPlatformData(platformData);

        bgfx::Init bgfxInit;
#if defined(__APPLE__)
        bgfxInit.type = bgfx::RendererType::Metal;
#elif defined(__linux__)
        bgfxInit.type = bgfx::RendererType::Vulkan;
#endif
        bgfxInit.resolution.width = width;
        bgfxInit.resolution.height = height;
        bgfxInit.resolution.reset = BGFX_RESET_NONE;
        bgfxInit.platformData = platformData;

        bgfx::renderFrame();
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
