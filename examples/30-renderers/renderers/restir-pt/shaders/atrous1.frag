$input v_color0
$input v_texcoord0

// atrous1.frag -- pass 6: the a-trous filter's first level (step 1), from the accumulation after its history
// fix (gi_history_fix.frag).
// Inputs: 0 fixDiff, 1 fixSpec (rgb, variance), 2 surf0, 3 surf1. Outputs: at1d, at1s.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(fixDiff, 0);
SAMPLER2D(fixSpec, 1);
SAMPLER2D(surf0, 2);
SAMPLER2D(surf1, 3);

#define RESTIR_STEP 1
#define RESTIR_DIFFUSE(q) texelFetch(fixDiff, q, 0)
#define RESTIR_SPECULAR(q) texelFetch(fixSpec, q, 0)
#include "atrous.sc"
