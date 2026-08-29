$input v_color0
$input v_texcoord0

// =============================================================================
// accumulate.frag  --  Pass 7 of the radiance-cascade renderer (temporal denoise).
//
// The cascade GI is largely deterministic, but with only a handful of directions per
// probe and a per-frame sub-tile jitter it carries mild angular banding/noise. This pass
// averages the composite over time to smooth it, exactly like the reprojection/fast
// renderers' temporal passes:
//   STILL  -> identity own-texel unbounded running mean (needs a 32F accumulator).
//   MOVING -> reproject this pixel's world position into last frame's camera, bilinear
//             history, accept it only if geometry matches (position + normal). NO colour
//             clamp -- clamping a jittered signal to a noisy neighbourhood darkens it
//             while moving (the lesson from the other renderers).
//
// Inputs (FBO 6 [0], FBO 7 [1..4], FBO 1 [5..8]):
//   0 composite  1 accumColor(hist) 2 histPos 3 histNormal 4 histAlbedo  5 gPos 6 gNormal 7 gAlbedo
// Outputs (FBO 7, ping-pong):
//   0 accumColor rgb = converged colour, a = age
//   1 histPos    current world position (carried for next frame's validation)
//   2 histNormal current world normal
//   3 histAlbedo current voxel albedo (so the face gate can also require a COLOUR match)
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(composite,  0);
SAMPLER2D(accumColor, 1);
SAMPLER2D(histPos,    2);
SAMPLER2D(histNormal, 3);
SAMPLER2D(histAlbedo, 4);
SAMPLER2D(gPos,       5);
SAMPLER2D(gNormal,    6);
SAMPLER2D(gAlbedo,    7);

uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;

#include <pjv_cascade_common.sc>

#define MOVING_MAX_AGE 50.0
// STILL used to be an UNBOUNDED running mean: age grew every frame with no cap, so once a
// pixel converged (age in the hundreds) a new sample blended in at 1/age ~ 0 and the GI was
// effectively frozen -- a real lighting/geometry change took hundreds of frames to surface.
// Cap it so the mean stays responsive while still being very smooth.
#define STILL_MAX_AGE 128.0
// Radius (texels) searched around the reprojected point for a same-face history texel to
// borrow converged colour from, so edges fill instead of flickering. 1 = 3x3.
#define SEARCH_RADIUS 1
// Max albedo difference for two texels to count as the SAME voxel colour. Coplanar voxels
// of different colour share a normal + position, so the face gate also needs a colour match
// or it smears colour across voxel colour boundaries when moving.
#define ALBEDO_TOL 0.08

void main() {
    vec2 uv = v_texcoord0;
    vec4 cur = texture2D(composite, uv);
    vec4 gp  = texture2D(gPos, uv);

    // Sky / background: view-dependent, never accumulate. Pass through; store a far
    // position + zero normal so nothing validates against it.
    if (gp.a < 0.0) {
        gl_FragData[0] = vec4(cur.rgb, 0.0);
        gl_FragData[1] = vec4(gp.xyz, -1.0);
        gl_FragData[2] = vec4(0.0);
        gl_FragData[3] = vec4(0.0);
        return;
    }

    vec3 curP   = gp.xyz;
    vec3 curN   = normalize(texture2D(gNormal, uv).xyz);
    vec3 curAlb = texture2D(gAlbedo, uv).rgb;

    vec3  accum = cur.rgb;
    float age   = 1.0;
    bool  moving = frameCount.y != 0.0;

    if (frameCount.x > 0.0) {
        if (!moving) {
            // STILL: identity own texel, unbounded running mean -> converges noise-free.
            vec2 ownUV = (floor(uv * windowRes.xy) + 0.5) / windowRes.xy;
            vec4 h = texture2D(accumColor, ownUV);
            if (!any(isnan(h))) {
                float hAge = min(max(h.a, 1.0), STILL_MAX_AGE);
                age   = min(hAge + 1.0, STILL_MAX_AGE);
                accum = mix(h.rgb, cur.rgb, 1.0 / age);
            }
        } else {
            // MOVING: reproject into last frame, then keep a STRICT face gate (normal +
            // position) so faces stay crisp -- but instead of discarding history when the
            // reprojected texel doesn't match (which re-exposed the raw jittered sample as
            // edge flicker), SEARCH a small neighbourhood for the NEAREST history texel that
            // does match this pixel's face and BORROW its converged colour. Voxel edges are a
            // staircase of perpendicular micro-faces, so the exact reprojected texel often
            // lands on the wrong micro-face; a same-face texel a pixel or two away holds this
            // face's accumulated value (GI varies slowly across a face), so borrowing it fills
            // the edge with stable colour instead of black/flicker. The normal gate still
            // blocks accepting a genuinely different (perpendicular) face -> no cross-face blur.
            // This is the faceRenderer's neighbourhood-search idea, keyed on normal+pos+ALBEDO
            // here rather than an exact face id. The albedo term is what makes it work on a flat
            // face tiled with different-coloured voxels: without it, a coplanar neighbour of a
            // different colour matches (same normal+pos) and its colour smears across the voxel
            // colour boundary while moving. A real disocclusion (no same-face texel nearby) finds
            // nothing -> a clean reset to the fresh sample.
            bool valid;
            vec2 pUV = worldToUV(curP, prevCameraPos.xyz, prevCameraDir.xyz,
                                 windowRes.xy, FOV, valid);
            if (valid) {
                float posTol = max(0.5, gp.a * 0.02);
                vec2 texel  = 1.0 / windowRes.xy;
                vec2 baseUV = (floor(pUV * windowRes.xy) + 0.5) / windowRes.xy;

                bool  found = false;
                float bestDist = 1e9;
                vec4  h = vec4(0.0);
                for (int dy = -SEARCH_RADIUS; dy <= SEARCH_RADIUS; dy++)
                for (int dx = -SEARCH_RADIUS; dx <= SEARCH_RADIUS; dx++) {
                    vec2 sUV = baseUV + vec2(float(dx), float(dy)) * texel;
                    vec3 hN   = texture2D(histNormal, sUV).xyz;
                    vec3 hP   = texture2D(histPos, sUV).xyz;
                    vec3 hAlb = texture2D(histAlbedo, sUV).rgb;
                    // Same face: matching normal AND position AND voxel colour.
                    if (dot(hN, curN) > 0.9 && length(hP - curP) < posTol &&
                        length(hAlb - curAlb) < ALBEDO_TOL) {
                        float d = float(dx * dx + dy * dy);  // prefer the nearest match
                        if (d < bestDist) {
                            bestDist = d;
                            h = texture2D(accumColor, sUV);
                            found = true;
                        }
                    }
                }

                if (found && !any(isnan(h))) {
                    // NOTE: deliberately NO adaptAge here. While moving, the fresh sample is noisy
                    // (per-frame jitter + only ~16 gather rays/probe), so cur vs hist differs frame
                    // to frame from NOISE, not real change -- adaptAge can't tell them apart and
                    // collapses the age, defeating the temporal mean on stable moving surfaces (they
                    // go as noisy as disoccluded ones). Moving history is already short (capped at
                    // MOVING_MAX_AGE, and reprojection resets on real disocclusion), so it stays
                    // responsive without it. adaptAge is kept only in the STILL branch, where the
                    // own-texel mean is stable enough that a luminance jump is a genuine change.
                    float hAge = min(max(h.a, 1.0), MOVING_MAX_AGE);
                    age   = min(hAge + 1.0, MOVING_MAX_AGE);
                    accum = mix(h.rgb, cur.rgb, 1.0 / age);
                }
            }
        }
    }

    gl_FragData[0] = vec4(accum, age);
    gl_FragData[1] = vec4(curP, 1.0);
    gl_FragData[2] = vec4(curN, 0.0);
    gl_FragData[3] = vec4(curAlb, 0.0);
}
