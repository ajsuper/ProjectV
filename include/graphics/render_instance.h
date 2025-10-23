#ifndef PROJV_RENDER_INSTANCE_H
#define PROJV_RENDER_INSTANCE_H

#include <memory>

#include "data_structures/rendererSpecification.h"
#include "data_structures/constructedRenderer.h"
#include "/opt/homebrew/Cellar/glfw/3.4/include/GLFW/glfw3.h"

namespace projv::graphics {
    class RenderInstance {
        public:
            GLFWwindow *window;
            bgfx::VertexBufferHandle vertexBuffer;
            bgfx::IndexBufferHandle indexBuffer;

            void setActiveRenderer(const std::shared_ptr<ConstructedRenderer>& constructedRendererToUse);

            void addRendererSpecification(uint rendererID, const RendererSpecification &constructedRendererToUse);
            void removeRendererSpecification(uint rendererID);
            RendererSpecification &getRendererSpecification(uint rendererID);

            RenderInstance() = default;
        private:
            std::unordered_map<uint, RendererSpecification> renderers;
            std::shared_ptr<ConstructedRenderer> constructedRenderer;
    };
}

#endif
