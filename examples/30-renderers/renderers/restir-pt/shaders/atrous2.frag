$input v_color0
$input v_texcoord0

// atrous2.frag -- the a-trous filter at step 2, from the previous level.
// Inputs: 0 at1d, 1 at1s, 2 surf0, 3 surf1. Outputs: at2d, at2s.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(at1d, 0);
SAMPLER2D(at1s, 1);
SAMPLER2D(surf0, 2);
SAMPLER2D(surf1, 3);

#define RESTIR_STEP 2
#define RESTIR_DIFFUSE(q) texelFetch(at1d, q, 0)
#define RESTIR_SPECULAR(q) texelFetch(at1s, q, 0)
#include "atrous.sc"
