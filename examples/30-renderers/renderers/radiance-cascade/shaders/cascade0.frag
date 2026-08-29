$input v_color0
$input v_texcoord0

// cascade0.frag -- merges its own gathered interval with the already-merged cascade
// 1 above it (upperCascade). See pjv_cascade.sc.
// Inputs: FBO 1 -> 0 gPos, 1 gNormal, (2 gAlbedo unused), 3 gDirect; then 4 upperCascade.

#include <bgfx_shader.sh>

SAMPLER2D(gPos,         0);
SAMPLER2D(gNormal,      1);
SAMPLER2D(gDirect,      3);
SAMPLER2D(upperCascade, 4);

#define CASCADE_INDEX 0
#include <pjv_cascade.sc>

void main() {
    ivec2 fragPx = ivec2(floor(v_texcoord0 * windowRes.xy));
    gl_FragData[0] = vec4(computeCascadeTexel(fragPx), 1.0);
}
