#ifndef PROJV_RENDER_INSTANCE_H
#define PROJV_RENDER_INSTANCE_H

#include <memory>

#include "data_structures/rendererSpecification.h"
#include "data_structures/constructedRenderer.h"
#include "data_structures/rendererSpecification.h"
#include "data_structures/posTexVertex.h"
#include "../../external/bgfx/include/bgfx/platform.h"
#include "/opt/homebrew/Cellar/glfw/3.4/include/GLFW/glfw3.h"
#define GLFW_EXPOSE_NATIVE_COCOA
#include "/opt/homebrew/Cellar/glfw/3.4/include/GLFW/glfw3native.h"

namespace projv::graphics {
    class RenderInstance {
        public:
            GLFWwindow *window;
            bgfx::VertexBufferHandle vertexBuffer;
            bgfx::IndexBufferHandle indexBuffer;

            void initialize(int width, int height, std::string name);

            void setActiveRenderer(const std::shared_ptr<ConstructedRenderer>& constructedRendererToUse);
            std::shared_ptr<ConstructedRenderer> getActiveRenderer();

            void addRendererSpecification(uint rendererID, const RendererSpecification &rendererSpecification);
            void removeRendererSpecification(uint rendererID);
            RendererSpecification &getRendererSpecification(uint rendererID);

            RenderInstance() = default;
        private:
            std::unordered_map<uint, RendererSpecification> renderers;
            std::shared_ptr<ConstructedRenderer> constructedRenderer;
    };
}

#endif
