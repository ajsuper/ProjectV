$input v_color0
$input v_texcoord0

// cascade2.frag -- WORLD-SPACE PER-FACE gather, merges with the already-merged cascade 3 above.
// Probe anchor is the voxel FACE CENTRE (gFace, register 4) -- FACE_PROBES; gather traces the
// voxel DDA. FBO1 carries 6 targets here (renderer 6), so the upper cascade binds at register 6.
// See pjv_cascade_ws.sc.

#include <bgfx_shader.sh>

SAMPLER2D(gPos,         0);
SAMPLER2D(gNormal,      1);
SAMPLER2D(gFace,        4);
SAMPLER2D(upperCascade, 6);

#include <pjv_utils_DDA.sc>

#define CASCADE_INDEX 2
#define FACE_PROBES
// Coarser probe spacing: far fewer probes/gather rays (the per-face variant's speed win). See common.
#define PROBE_SPACING0 16
#include <pjv_cascade_ws.sc>

void main() {
    ivec2 fragPx = ivec2(floor(v_texcoord0 * windowRes.xy));
    gl_FragData[0] = vec4(computeCascadeTexel(fragPx), 1.0);
}
