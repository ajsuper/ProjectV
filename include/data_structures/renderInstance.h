#ifndef RENDER_INSTANCE_H
#define RENDER_INSTANCE_H

#include "/opt/homebrew/Cellar/glfw/3.4/include/GLFW/glfw3.h"
#include <unordered_map>

#include "rendererSpecification.h"
#include "constructedRenderer.h"

struct RenderInstance {
    std::unordered_map<uint, RendererSpecification> renderers;
    ConstructedRenderer constructedRenderer;
    GLFWwindow *window;
    bgfx::VertexBufferHandle vertexBuffer;
    bgfx::IndexBufferHandle indexBuffer;
};

#endif
