$input v_color0
$input v_texcoord0

// cascade3.frag -- TOP cascade, WORLD-SPACE PER-FACE gather. No upper cascade; escaped rays see
// the true sky. Probe anchor is the voxel FACE CENTRE (gFace, register 4) -- FACE_PROBES; gather
// traces the voxel DDA. See pjv_cascade_ws.sc.

#include <bgfx_shader.sh>

SAMPLER2D(gPos,    0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gFace,   4);

#include <pjv_utils_DDA.sc>

#define CASCADE_INDEX 3
#define IS_TOP
#define FACE_PROBES
// Coarser probe spacing: far fewer probes/gather rays (the per-face variant's speed win). See common.
#define PROBE_SPACING0 16
#include <pjv_cascade_ws.sc>

void main() {
    ivec2 fragPx = ivec2(floor(v_texcoord0 * windowRes.xy));
    gl_FragData[0] = vec4(computeCascadeTexel(fragPx), 1.0);
}
