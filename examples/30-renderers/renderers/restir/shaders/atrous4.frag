$input v_color0
$input v_texcoord0

// atrous4.frag -- the a-trous filter at step 4, from the previous level (at2).
// Inputs: 0 at2, 1 accIrr, 2 accMom, 3 accPos, 4 accNrm.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(at2, 0);
SAMPLER2D(accPos, 3);
SAMPLER2D(accNrm, 4);

#define RESTIR_STEP 4
#define RESTIR_COLOUR(q) texelFetch(at2, q, 0)
#include "atrous.sc"
