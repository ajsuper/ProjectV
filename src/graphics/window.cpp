#include "graphics/window.h"

namespace projv::graphics {
    RenderInstance initializeRenderInstance(int width, int height, std::string name) {
        RenderInstance renderInstance;
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWwindow *window = glfwCreateWindow(width, height, name.c_str(), NULL, NULL);

        std::cout << "Initializing stuff!!!!!!!" << std::endl;
        bgfx::PlatformData platformData;
#if defined(__APPLE__)
            platformData.nwh = glfwGetCocoaWindow(window);
            platformData.type = bgfx::NativeWindowHandleType::Default;
#elif defined(__linux__)
            platformData.nwh = glfwGetWaylandWindow(window);
            platformData.type = bgfx::NativeWindowHandleType::Wayland;
            platformData.ndt = bgfx::glfwGetWaylandDisplay();
#endif
        // platformData.ndt = glfwGetCocoaDisplay();
        bgfx::setPlatformData(platformData);

        bgfx::Init bgfxInit;
#if defined(__APPLE__)
        bgfxInit.type = bgfx::RendererType::Metal;
#elif defined(__linux__)
        bgfxInit.type = bgfx::RenderType::Vulkan;
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

        renderInstance.vertexBuffer = bgfx::createVertexBuffer(bgfx::makeRef(vertexQuadVertices, sizeof(vertexQuadVertices)), pcvDecl);
        renderInstance.indexBuffer = bgfx::createIndexBuffer(bgfx::makeRef(quadTriList, sizeof(quadTriList)));

        renderInstance.window = window;
        return renderInstance;
    }
}
