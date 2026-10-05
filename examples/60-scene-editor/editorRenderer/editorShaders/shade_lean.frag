// =============================================================================
// shade_lean.frag  --  shade.frag without its two traced terms.
//
// Dispatched whenever neither the Sun shadow nor the Advanced preview toggle is on -- the default --
// leaving screen-space occlusion and normal shading, neither of which casts a ray. The full shade.bin
// is swapped back in the frame either traced toggle is turned on (selectViewportPrograms, main.cpp).
//
// Same reason as albedo_lean.frag: the shadow ray and the transmittance query pull the whole scene
// traversal into this shader, and registers are allocated for the whole shader whether or not a pixel
// ever calls it. The 16-tap occlusion is texture-bound and needs every wave in flight it can get.
//
// No $input lines here: shaderc reads them out of the included file as well.
// =============================================================================
#define EDITOR_SHADE_LEAN 1
#include "shade.frag"
