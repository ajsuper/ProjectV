$input v_color0
$input v_texcoord0

// atrous8.frag -- the a-trous filter at step 8, from the previous level.
// Inputs: 0 at3d, 1 at3s, 2 surf0, 3 surf1. Outputs: giDiffFinal, giSpecFinal.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(at3d, 0);
SAMPLER2D(at3s, 1);
SAMPLER2D(surf0, 2);
SAMPLER2D(surf1, 3);

#define RESTIR_STEP 8
#define RESTIR_DIFFUSE(q) texelFetch(at3d, q, 0)
#define RESTIR_SPECULAR(q) texelFetch(at3s, q, 0)
#include "atrous.sc"
