#ifndef DEPENDENCY_GRAPH_H
#define DEPENDENCY_GRAPH_H

#include <vector>

namespace projv {
    struct RenderPass {
        uint shaderID;
        std::vector<uint> frameBufferInputIDs;
        std::vector<uint> textureResourceIDs;
        int frameBufferOutputID;
        uint multiPass;
    };

    struct DependencyGraph {
        std::vector<RenderPass> renderPasses;
    };
}

#endif
