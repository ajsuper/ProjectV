$input v_color0
$input v_texcoord0

// =============================================================================
// probe.frag  --  Pass 2. The entire indirect-light gather, in one pass.
//
// Replaces cascade3/2/1/0 (four passes, a merge hierarchy and two atlases' worth of directional
// radiance) with one Monte Carlo integration per voxel face. The reasoning for the swap is in
// sharedShaders/pjv_probe.sc; the short version is that the cascades spent their whole structure
// deciding which faces owned a probe, decided it with a lattice in SCREEN space, and so returned a
// viewpoint-dependent answer to a viewpoint-independent question.
//
// This pass runs at QUARTER resolution and each of its texels gathers for whatever face the
// G-buffer holds at its centre pixel. That is still a screen lattice -- but it is a 4-pixel one
// rather than a 16-pixel one, and, far more importantly, what it produces is not a probe whose value
// depends on where it stands. Two texels that land on one face compute the identical number, and so
// does a texel that lands on it from a different camera position next frame, because the sample
// directions are seeded by the FACE KEY. The lattice now only decides how OFTEN a face is refreshed,
// not what it is worth -- and accumulate.frag holds a face's converged value between refreshes.
//
// Inputs (FBO 1 [0..5] then FBO 7 [6..8]):
//   0 gPos  1 gNormal  2 gAlbedo  3 gDirect  4 gFace  5 gKey  6 accumR  7 histKey  8 histPos
// gAlbedo, gDirect, histKey and histPos are bound but unread and are left undeclared. accumR is
// last frame's converged indirect and is read as prevIndirect -- the multi-bounce feedback term.
//
// Output (FBO 2): rgb = cosine-weighted mean radiance R for this texel's face, a = 1 if this texel
// found a face at all and 0 if it landed on sky. resolve reads that flag to know which taps are
// real, which matters at a silhouette where half the neighbourhood is background.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos,    0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gFace,   4);
SAMPLER2D(gKey,    5);
// Last frame's temporally accumulated R, at full resolution. pjv_probe.sc projects each gather hit
// into it for the second bounce; this pass never reads it at its own texel.
SAMPLER2D(prevIndirect, 6);

// w = PER-VOXEL LIGHTING (U). Read through PJV_PER_VOXEL_LIGHTING in pjv_face_key.sc, which is what
// decides whether the gather below integrates one face's hemisphere or the whole voxel's sphere.
// The other components belong to other passes.
uniform vec4 debugParams;

#include <pjv_utils_DDA.sc>

// This pass does not address a cascade atlas -- the cascades are gone -- but pjv_cascade_common.sc
// still wants both grids named rather than guessed at. The G-buffer (input 0) is the screen grid;
// this pass's own quarter-size target is the other.
#define CASCADE_SCREEN_RES pjvResOr(passInputRes[0].xy, passTargetRes.xy)
#define CASCADE_ATLAS_RES  passTargetRes.xy
#include <pjv_cascade_common.sc>
#include <pjv_face_key.sc>
// The animation controls, for one thing this pass reads from them: the clean-view switch below. The
// gather itself needs nothing else -- it does not ask for animation (see pjv_probe.sc), so it sees
// ordinary geometry through the ordinary scene query.
#include <pjv_anim_controls.sc>
#include <pjv_probe.sc>

void main() {
    if (animCleanView()) {
        gl_FragData[0] = vec4(0.0);
        return;
    }

    // Which probe texel this is. It selects this texel's offset into the face's direction sequence,
    // so the several texels that land on one face each draw DIFFERENT directions and resolve can
    // average them into one much lower-variance estimate. See pjvProbeSample.
    ivec2 probeTexel = ivec2(floor(v_texcoord0 * passTargetRes.xy));

    // This texel's anchor pixel in the G-buffer, snapped to a texel centre. Snapping is not a detail:
    // a bilinear read of gKey between two faces returns a key that names NEITHER of them, and a
    // bilinear read of gFace returns an origin floating between two surfaces. Both feed things that
    // must be exact.
    vec2 srcUV = pjvSnapToTexel(v_texcoord0, CASCADE_SCREEN_RES);

    vec4 gp = texture2D(gPos, srcUV);
    if (gp.a < 0.0) {                       // sky: no face here, nothing to gather
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // The gather origin is the voxel FACE CENTRE, not this pixel's hit point. It is camera-
    // independent and identical for every pixel on the face, which is what lets all of them agree on
    // one answer -- and the voxelisation already guarantees it lies on the surface, so no arbitrary
    // world-grid snap is needed to stabilise it.
    vec4  gf = texture2D(gFace, srcUV);
    vec3  P  = gf.xyz;
    float voxelSize = gf.a;
    if (voxelSize <= 0.0) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    vec4 key = texture2D(gKey, srcUV);
    // The normal comes from the KEY, not from gNormal. They are the same six values, but taking it
    // from the key guarantees the hemisphere this integrates over and the identity accumulate gates
    // on cannot disagree -- and a disagreement there would silently pool two faces' light.
    vec3 N = pjvFaceNormal(key.w);

    // The voxel this face belongs to, stepped back along the normal by half an edge. DERIVED rather
    // than stored: gFace is read by the upscaler as the face's plane, so it must keep meaning the
    // face centre, and the voxel centre is one subtract away from data already in registers.
    //
    // Only the per-voxel mode reads it, and in that mode every one of a voxel's six faces computes
    // the SAME centre -- which is the whole mechanism. Two probes standing on different faces then
    // integrate the same sphere about the same point, so their estimates are of one quantity and
    // averaging them is meaningful rather than a blend of two different answers.
    vec3 voxelCentre = P - N * (voxelSize * 0.5);

    gl_FragData[0] = vec4(pjvProbeGather(P, N, voxelCentre, voxelSize, key, probeTexel), 1.0);
}
