#ifndef PROJV_RENDERER_SPECIFICATION_H
#define PROJV_RENDERER_SPECIFICATION_H
#include <vector>

#include "resources.h"
#include "dependencyGraph.h"

namespace projv {
    struct RendererSpecification {
        Resources resources;
        DependencyGraph dependencyGraph;
    };
}

#endif
