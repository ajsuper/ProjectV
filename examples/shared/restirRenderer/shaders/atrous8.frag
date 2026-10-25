$input v_color0
$input v_texcoord0

// atrous8.frag -- the a-trous filter's last level (step 8), from the previous level (at3). It also hands
// the GI pixels' surfaces on to the composite.
// Inputs: 0 at3, 1 accIrr, 2 accMom, 3 accPos, 4 accNrm. Outputs: giFinal, giPos, giNrm.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(at3, 0);
SAMPLER2D(accPos, 3);
SAMPLER2D(accNrm, 4);

#define RESTIR_STEP 8
#define RESTIR_ATROUS_LAST
#define RESTIR_COLOUR(q) texelFetch(at3, q, 0)
#include "atrous.sc"
