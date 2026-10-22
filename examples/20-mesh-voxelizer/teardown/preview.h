#ifndef PROJECTV_TEARDOWN_PREVIEW_H
#define PROJECTV_TEARDOWN_PREVIEW_H
// A software render of a converted level, written beside it as preview.png: a quick way to see
// that a conversion is right without opening the engine.

#include <string>

#include "level.h"

namespace teardown {

// An isometric view from the south-east, `width` pixels across, sunlit with face normals. With a
// focus radius > 0, only what lies within `radius` metres (horizontally) of `focus` is drawn.
bool renderPreview(const Level& level, const std::string& pngPath, int width = 1600, vec3 focus = vec3(0.0f),
                   float radius = 0.0f);

} // namespace teardown

#endif // PROJECTV_TEARDOWN_PREVIEW_H
