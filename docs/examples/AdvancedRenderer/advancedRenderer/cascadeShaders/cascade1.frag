$input v_color0
$input v_texcoord0

// cascade1.frag -- world-space per-face gather, merging with the already-merged cascade 2 above.
//
// The probe anchor is the voxel FACE CENTRE (gFace), not the pixel's own hit point, so every
// screen probe landing on one face gathers from a single stable world origin. That is what pays
// for PROBE_SPACING0 16 below: at a quarter the probe density of a screen-anchored grid the
// result is steadier, not coarser, because the origins stop drifting with the camera.
//
// SAMPLER REGISTERS: the engine binds a pass's inputs in declaration order across its input
// framebuffers, so FBO 1's six attachments take 0..5 and anything after it starts at 6. This cascade's
// upper neighbour is its second input, so it lands at 6.
// gAlbedo (2), gDirect (3) and gKey (5) are bound but unread here and are left undeclared.

#include <bgfx_shader.sh>

SAMPLER2D(gPos,    0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gFace,   4);
SAMPLER2D(upperCascade, 6);

#include <pjv_utils_DDA.sc>

#define CASCADE_INDEX 1
// Coarser than a screen-anchored grid would need, and affordable precisely because the probes are
// face-anchored. Every pass that touches the probe layout -- these four, resolve and compose --
// must agree on this number.
#define PROBE_SPACING0 16
// This pass WRITES the atlas, so its own target is the atlas grid; the probe grid is the
// G-buffer's, which is bound at slot 0 and is four times larger on each axis.
#define CASCADE_SCREEN_RES pjvResOr(passInputRes[0].xy, passTargetRes.xy)
#define CASCADE_ATLAS_RES  passTargetRes.xy
#include <pjv_cascade_ws.sc>

void main() {
    ivec2 fragPx = ivec2(floor(v_texcoord0 * passTargetRes.xy));
    gl_FragData[0] = vec4(computeCascadeTexel(fragPx), 1.0);
}
