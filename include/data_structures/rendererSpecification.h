#ifndef RENDERER_SPECIFICATION_H
#define RENDERER_SPECIFICATION_H

#include <vector>

#include "resources.h"
#include "dependencyGraph.h"

struct RendererSpecification {
    Resources resources;
    DependencyGraph dependencyGraph;
};

#endif
