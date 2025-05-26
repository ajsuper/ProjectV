#include "graphics/window.h"

namespace projv::graphics {
    void createRenderQuad(RenderInstance& renderInstance) {
        // Define vertices for a window filling quad
        float vertices[] = {
            // positions   // texCoords
            -1.0f, -1.0f, 0.0f, 0.0f,
            1.0f, -1.0f, 1.0f, 0.0f,
            -1.0f, 1.0f, 0.0f, 1.0f,
            -1.0f, 1.0f, 0.0f, 1.0f,
            1.0f, -1.0f, 1.0f, 0.0f,
            1.0f, 1.0f, 1.0f, 1.0f};

        glGenVertexArrays(1, &renderInstance.VAO);
        glGenBuffers(1, &renderInstance.VBO);

        // Bind and set buffer data
        glBindVertexArray(renderInstance.VAO);

        glBindBuffer(GL_ARRAY_BUFFER, renderInstance.VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        // Position attribute (location = 0)
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(0);

        // Texture Coord attribute (location = 1)
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glBindVertexArray(0);

        return;
    }

    RenderInstance initializeRenderInstance(int windowWidth, int windowHeight, const std::string& windowName) { // CORE
        RenderInstance renderInstance;
        if (!glfwInit()) {
            core::error("[initializeRenderInstance] Failed to initialize GLFW!");
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        renderInstance.window = glfwCreateWindow(windowWidth, windowHeight, windowName.c_str(), NULL, NULL);
        if (!renderInstance.window) {
            core::error("[initializeRenderInstance] Failed to create GLFW window!");
            glfwTerminate();
        }
        glfwMakeContextCurrent(renderInstance.window);

        glewExperimental = GL_TRUE;  // Enable modern OpenGL features
        if (glewInit() != GLEW_OK) {
            core::error("[initializeRenderInstance] Failed to initialize GLEW!");
        }

        createRenderQuad(renderInstance);

        return renderInstance;
    }

    void setErrorCallback(void (*error_callback)(int error_code, const char* description)) {
        glfwSetErrorCallback(error_callback);
    }

    void setFrameBufferSizeCallback(RenderInstance& renderInstance, void (*framebuffer_callback)(GLFWwindow* window, int width, int height)) {
        glfwSetFramebufferSizeCallback(renderInstance.window, framebuffer_callback);
    }
}