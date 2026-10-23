$input v_color0
$input v_texcoord0

// atrous4.frag -- the a-trous filter at step 4, from the previous level.
// Inputs: 0 at2d, 1 at2s, 2 surf0, 3 surf1. Outputs: at3d, at3s.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(at2d, 0);
SAMPLER2D(at2s, 1);
SAMPLER2D(surf0, 2);
SAMPLER2D(surf1, 3);

#define RESTIR_STEP 4
#define RESTIR_DIFFUSE(q) texelFetch(at2d, q, 0)
#define RESTIR_SPECULAR(q) texelFetch(at2s, q, 0)
#include "atrous.sc"
