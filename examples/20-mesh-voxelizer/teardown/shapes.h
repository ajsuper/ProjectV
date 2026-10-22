#ifndef PROJECTV_TEARDOWN_SHAPES_H
#define PROJECTV_TEARDOWN_SHAPES_H
// Builders for the shape nodes of a level file. Each writes the node's *own* voxels into a canvas
// in the node's frame; merging children and placing the result is level.cpp's job.

#include <string>
#include <vector>

#include "canvas.h"
#include "level.h"

namespace teardown {

// One piece of geometry a shape node produced, in the node's frame. Most nodes produce one with an
// identity `local`; a voxscript can produce several, each placed by its Vox() call.
struct LocalShape {
    Transform local;
    Canvas voxels;
    float voxelSize = TD_VOXEL;
};

struct ShapeContext {
    const PathContext& paths;
    std::string xmlDir;   // folder of the file the node came from, for unprefixed paths
};

bool buildVox(const pugi::xml_node& n, const ShapeContext& ctx, LocalShape& out);
bool buildVoxbox(const pugi::xml_node& n, const ShapeContext& ctx, LocalShape& out);
bool buildVoxagon(const pugi::xml_node& n, const ShapeContext& ctx, LocalShape& out);
// voxscript.cpp
bool buildVoxscript(const pugi::xml_node& n, const ShapeContext& ctx, std::vector<LocalShape>& out);

} // namespace teardown

#endif // PROJECTV_TEARDOWN_SHAPES_H
