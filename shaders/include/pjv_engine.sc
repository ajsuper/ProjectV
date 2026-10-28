#ifndef PJV_ENGINE_SC
#define PJV_ENGINE_SC

// Every uniform the engine owns, declared once. None of these is named in a renderer's
// resources.json: the renderer driver creates them (manage_resources.cpp) and sets them before every
// pass (perform_renderer.cpp). A shader includes this file instead of declaring them itself; a
// uniform it never reads costs nothing (shaderc drops it).
//
// Sizes match the C++ side (data_structures/constructedRenderer.h): an array declared at any
// other size here would bind against the wrong range.

uniform vec4 passTargetRes;       // xy: this pass's target size; zw: 1 / size
uniform vec4 passInputRes[8];     // per input slot, as passTargetRes; zero for an empty slot. PROJV_MAX_PASS_INPUTS
uniform vec4 pjvAnimTime;         // (seconds, 0, 0, 0)

// The motion table: three vec4 per set, sixteen sets (PROJV_MOTION_SET_VEC4S). Laid out and read in
// pjv_utils_DDA.sc; all zero (nothing moves) unless the program configures it.
uniform vec4 pjvMotionSets[48];

#endif
