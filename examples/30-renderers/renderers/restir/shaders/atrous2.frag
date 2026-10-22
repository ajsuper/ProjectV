$input v_color0
$input v_texcoord0

// atrous2.frag -- the a-trous filter at step 2, from the previous level (at1).
// Inputs: 0 at1, 1 accIrr, 2 accMom, 3 accPos, 4 accNrm.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(at1, 0);
SAMPLER2D(accPos, 3);
SAMPLER2D(accNrm, 4);

#define RESTIR_STEP 2
#define RESTIR_COLOUR(q) texelFetch(at1, q, 0)
#include "atrous.sc"
