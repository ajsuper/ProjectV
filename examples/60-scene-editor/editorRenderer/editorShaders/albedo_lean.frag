// =============================================================================
// albedo_lean.frag  --  albedo.frag with the advanced preview compiled out.
//
// The editor dispatches this one whenever the Advanced preview toggle is off, which is almost always,
// and swaps in albedo.bin the frame it is turned on (see selectViewportPrograms in main.cpp).
//
// Why a second binary rather than the uniform branch albedo.frag already has: a GPU allocates
// registers for the WHOLE shader, not for the branch a pixel takes. The peel, refraction and specular
// paths need far more of them than the plain march, so with all of it compiled in, the plain path ran
// at the occupancy of the heaviest one -- fewer pixels in flight to hide each texture fetch behind.
// Measured on a Radeon 860M, 2052x1308 viewport, camera moving every frame (EDITOR_BENCH):
// Bistro from inside, this pass went from 22.5 ms to 13.8 ms, with the output unchanged.
//
// No $input lines here: shaderc reads them out of the included file as well, and declaring them in
// both is a redefinition.
// =============================================================================
#define EDITOR_ALBEDO_LEAN 1
#include "albedo.frag"
