$input v_color0
$input v_texcoord0

// atrous1.frag -- pass 6: the a-trous filter's first level (step 1), from the accumulation.
// Inputs: 0 accDiff, 1 accSpec, 2 accMom, 3 accSurf, 4 surf0, 5 surf1. Outputs: at1d, at1s.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(accDiff, 0);
SAMPLER2D(accSpec, 1);
SAMPLER2D(accMom, 2);
SAMPLER2D(surf0, 4);
SAMPLER2D(surf1, 5);

#define RESTIR_STEP 1
vec4 restirDiffuseAt(ivec2 q) { return vec4(texelFetch(accDiff, q, 0).rgb, texelFetch(accMom, q, 0).z); }
vec4 restirSpecularAt(ivec2 q) { return vec4(texelFetch(accSpec, q, 0).rgb, texelFetch(accMom, q, 0).w); }
#define RESTIR_DIFFUSE(q) restirDiffuseAt(q)
#define RESTIR_SPECULAR(q) restirSpecularAt(q)
#include "atrous.sc"
