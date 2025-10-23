#include "graphics/render_instance.h"
#include "data_structures/rendererSpecification.h"

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
}
