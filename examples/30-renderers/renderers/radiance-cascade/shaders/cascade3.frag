$input v_color0
$input v_texcoord0

// cascade3.frag -- TOP cascade (coarsest spatial, finest angular, longest reach). No
// upper cascade to merge; transparent rays see the open sky. See pjv_cascade.sc.
// Inputs (FBO 1): 0 gPos, 1 gNormal, (2 gAlbedo unused), 3 gDirect.

#include <bgfx_shader.sh>

SAMPLER2D(gPos,    0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gDirect, 3);

#define CASCADE_INDEX 3
#define IS_TOP
#include <pjv_cascade.sc>

void main() {
    ivec2 fragPx = ivec2(floor(v_texcoord0 * windowRes.xy));
    gl_FragData[0] = vec4(computeCascadeTexel(fragPx), 1.0);
}
