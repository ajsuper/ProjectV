#ifndef PROJV_DEPENDENCY_GRAPH_H
#define PROJV_DEPENDENCY_GRAPH_H

#include <cstdint>
#include <vector>

namespace projv {
    struct RenderPass {
        uint32_t shaderID;
        std::vector<uint32_t> frameBufferInputIDs;
        std::vector<uint32_t> textureResourceIDs;
        int frameBufferOutputID;
        uint32_t multiPassPassNumber; // 0 if multipass is 0.
        uint32_t multiPass;
    };

    struct DependencyGraph {
        std::vector<RenderPass> renderPasses;
    };
}

#endif
