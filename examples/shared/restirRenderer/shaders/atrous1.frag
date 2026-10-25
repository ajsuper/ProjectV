$input v_color0
$input v_texcoord0

// atrous1.frag -- pass 6: the a-trous filter's first level (step 1), from the accumulation.
// Inputs: 0 accIrr, 1 accMom, 2 accPos, 3 accNrm.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(accIrr, 0);
SAMPLER2D(accMom, 1);
SAMPLER2D(accPos, 2);
SAMPLER2D(accNrm, 3);

#define RESTIR_STEP 1
vec4 restirColourAt(ivec2 q) { return vec4(texelFetch(accIrr, q, 0).rgb, texelFetch(accMom, q, 0).z); }
#define RESTIR_COLOUR(q) restirColourAt(q)
#include "atrous.sc"
