$input v_color0
$input v_texcoord0

// cascade0.frag -- WORLD-SPACE gather, merges with the already-merged cascade 1 above.
// Probe anchor from FBO1 (gPos, gNormal); gather traces the voxel DDA. See pjv_cascade_ws.sc.

#include <bgfx_shader.sh>

SAMPLER2D(gPos,         0);
SAMPLER2D(gNormal,      1);
SAMPLER2D(upperCascade, 4);

#include <pjv_utils_DDA.sc>

#define CASCADE_INDEX 0
#include <pjv_cascade_ws.sc>

void main() {
    ivec2 fragPx = ivec2(floor(v_texcoord0 * windowRes.xy));
    gl_FragData[0] = vec4(computeCascadeTexel(fragPx), 1.0);
}
