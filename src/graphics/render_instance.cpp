#include "graphics/render_instance.h"
#include "data_structures/rendererSpecification.h"

namespace projv::graphics {
    void RenderInstance::setActiveRenderer(const std::shared_ptr<ConstructedRenderer>& constructedRendererToUse) {
        this->constructedRenderer = constructedRendererToUse;
    }

    void RenderInstance::addRendererSpecification(uint rendererID, const RendererSpecification &constructedRendererToUse) {
        this->renderers[rendererID] = constructedRendererToUse;
    }

    void RenderInstance::removeRendererSpecification(uint rendererID) {
        this->renderers.erase(rendererID);
    }

    RendererSpecification &RenderInstance::getRendererSpecification(uint rendererID) {
        return this->renderers.at(rendererID);
    }
}
