$input v_color0
$input v_texcoord0

// =============================================================================
// sky_reproject.frag  --  Pass 3 of the fast renderer (temporal skylight denoise).
//
// The skylight-occlusion pass (rtao.frag) casts only a FEW long hemisphere rays
// per pixel per frame, so its raw output (skyRaw) is inherently noisy: whether a
// given direction escapes to open sky or is blocked flips from frame to frame as
// the blue-noise-rotated sample set advances. combine_blur's à-trous kernel takes
// out the high-frequency part spatially, but a small number of rays still leaves a
// low-frequency wobble -- especially while the camera moves, where the final TAA's
// colour clamp (tuned for the deterministic direct term) actively fights the noisy
// ambient and drags it dark instead of letting it settle.
//
// This pass accumulates the skylight term TEMPORALLY, before it is composited. The
// occluded sky irradiance a diffuse surface receives is VIEW-INDEPENDENT and the
// scene is static, so the same world point's sky value is valid across frames: we
// can just keep averaging it. That turns "a few rays this frame" into "hundreds of
// rays over the last N frames", which is the real skylight-occlusion detection the
// user asked for -- crisp contact darkening that stays put and stops shimmering.
//
// It is a direct sibling of reprojectionRenderer/reproject_accumulate.frag, with
// two differences: (1) it accumulates the sky term only (skyRaw), not the full
// radiance, and (2) its "current geometry" comes from the shared G-buffer (FBO 1's
// gPos/gNormal) rather than from its own colour target -- the sky FBO only needs to
// carry the accumulated value forward plus the pos/normal used to validate it.
//
// Two regimes, chosen by whether the camera moved this frame (frameCount.y):
//
//   STILL  -> IDENTITY accumulation. The surface under each pixel is unchanged, so
//             read history from THIS pixel's own texel (zero resampling error) and
//             fold the new sample into an UNBOUNDED running mean. Unbounded + no
//             rectification => the mean converges all the way to zero noise (a true
//             frame-count average). This is why accumSky is a 32F target: an fp16
//             accumulator quantizes the tiny late updates away and freezes short of
//             clean (learned the hard way in the reprojection renderer).
//
//   MOVING -> REPROJECT this frame's world hit through last frame's camera and read
//             the history BILINEARLY there. History is kept unless it fails a loose
//             distance-scaled POSITION gate (gross disocclusion). There is NO normal
//             gate (a far diagonal wall's perpendicular voxel-staircase micro-faces
//             would fail it on nearly every jittered frame -> reset -> flicker) and NO
//             colour clamp (the raw 1-ray sky sample is binary, so a min/max box
//             collapses toward black in creases and biases the whole image DARK while
//             moving). It is a plain, unbiased running mean under a moderate age cap:
//             holds the true brightness, carries a little residual noise while moving
//             that combine_blur + the final TAA remove. See the MOVING block below.
//
// Inputs (sampler order matches the flattened FBO dependency list):
//   0 gDirect  1 gPos  2 gNormal  3 gAmbient   (FBO 1, this frame -- only 1,2 used)
//   4 skyRaw                                    (FBO 2, this frame's raw sky term)
//   5 histSky  6 histPos  7 histNormal         (FBO 5, previous frame -- ping-pong)
// Outputs (FBO 5, ping-pong):
//   0 accumSky  rgb = accumulated mean incoming sky radiance, a = age
//   1 skyPos    world hit position, carried forward for next frame's reprojection
//   2 skyNormal world normal,       carried forward for next frame's reprojection
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos,       1);
SAMPLER2D(gNormal,    2);
SAMPLER2D(skyRaw,     4);
SAMPLER2D(histSky,    5);
SAMPLER2D(histPos,    6);
SAMPLER2D(histNormal, 7);   // still written to FBO 5, but no longer READ (no normal gate)

uniform vec4 windowRes;
uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;
uniform vec4 frameCount;

#define FOV 60.0

// Moving-camera history cap. Long enough to knock down the few-ray noise while
// moving, short enough that the mild bilinear-resampling softening on moving edges
// refreshes instead of lingering. Still-camera history is UNBOUNDED (below) so a
// parked view converges to zero noise.
#define MOVING_MAX_AGE 24.0

// Project a world-space point into a camera and return its screen UV. Exact inverse
// of pjv_utils_DDA.sc::rayStartDirection (same basis, same NDC flip), identical to
// the copy in taa.frag / reproject_accumulate.frag.
vec2 worldToUV(vec3 P, vec3 camPos, vec3 camDir, vec2 res, float fov, out bool valid) {
    vec3 forward = normalize(camDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));

    vec3 v = P - camPos;
    float z = dot(v, forward);
    if (z <= 1e-4) { valid = false; return vec2(-1.0); }   // behind the camera

    float scale  = tan(radians(fov * 0.5));
    float aspect = res.x / res.y;
    float ndcx = (dot(v, right) / z) / (scale * aspect);
    float ndcy = (dot(v, up)    / z) / scale;

    vec2 flip = (vec2(ndcx, ndcy) + 1.0) * 0.5;
    vec2 uv = vec2(flip.x, 1.0 - flip.y);
    valid = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    return uv;
}

void main() {
    vec2 uv = v_texcoord0;

    vec4 pos4    = texture2D(gPos,    uv);
    vec3 curP    = pos4.xyz;
    float curD   = pos4.w;
    vec3 curN    = texture2D(gNormal, uv).xyz;
    vec3 curSky  = texture2D(skyRaw,  uv).rgb;

    // Sky / background: no surface to track (rtao already wrote 0 here, and combine
    // ignores this pixel's sky term via the direct hit mask). Pass through at age 1
    // so a disocclusion that lands here restarts cleanly.
    if (dot(curN, curN) < 0.01) {
        gl_FragData[0] = vec4(curSky, 1.0);
        gl_FragData[1] = pos4;
        gl_FragData[2] = vec4(curN, 0.0);
        return;
    }

    // Default (first frame / disocclusion): show the fresh sample at age 1.
    vec3  accum = curSky;
    float age   = 1.0;

    bool moving = frameCount.y != 0.0;

    if (frameCount.x > 0.0) {
        if (!moving) {
            // ---- STILL: identity accumulation, unbounded, no validation. ----
            vec2  ownUV  = (floor(uv * windowRes.xy) + 0.5) / windowRes.xy;
            vec4  hSky   = texture2D(histSky, ownUV);
            if (!any(isnan(hSky.rgb)) && !isnan(hSky.a)) {
                age   = max(hSky.a, 1.0) + 1.0;      // grow without bound -> 0 noise
                accum = mix(hSky.rgb, curSky, 1.0 / age);
            }
        } else {
            // ---- MOVING: reproject, bilinear read, POSITION-gated only. ----
            bool valid;
            vec2 prevUV = worldToUV(curP, prevCameraPos.xyz, prevCameraDir.xyz,
                                    windowRes.xy, FOV, valid);
            if (valid) {
                vec4 hSky = texture2D(histSky, prevUV);   // bilinear
                vec3 hPos = texture2D(histPos, prevUV).xyz;

                // Gate ONLY on world position -- never on normal, never on colour:
                //
                //  * NO normal gate. A far diagonal wall is a staircase of axis-aligned
                //    voxel micro-faces whose normals are PERPENDICULAR (a +X face next to
                //    a +Z face). Sub-pixel jitter lands the primary ray on a different
                //    micro-face almost every frame, so a dot(curN,hNrm) gate failed on
                //    essentially every reprojection there -> history discarded every frame
                //    -> the raw few-ray noise was re-exposed as violent flicker. Dropping
                //    it lets the alternating micro-faces ACCUMULATE into their correct
                //    anti-aliased average. The position gate below still rejects a genuine
                //    disocclusion (a big depth jump moves the point many voxels) while its
                //    tolerance comfortably spans the ~1-voxel offset between adjacent
                //    micro-faces, so the gate itself never flickers.
                //
                //  * NO colour clamp. With one ray per pixel the raw sky sample is
                //    essentially BINARY (the ray either escapes to bright sky or is
                //    occluded -> 0), so a 3x3 min/max box collapses toward [0,0] in creases
                //    on any frame where all nine neighbours' single rays are occluded, and
                //    clamps the converged history to black. That floor-without-a-ceiling is
                //    a one-sided DARKENING bias -- the "scene gets darker while I move" bug.
                //    A plain running mean (mix, never clamp) is unbiased: it holds the true
                //    brightness and merely carries a little residual noise while moving,
                //    which combine_blur's spatial kernel and the final TAA mop up.
                float posTol = max(0.5, curD * 0.02);
                bool  posOK  = distance(curP, hPos) < posTol;
                bool  sane   = !any(isnan(hSky.rgb)) && !isnan(hSky.a);

                if (posOK && sane) {
                    // Clamp the stored age down to the cap BEFORE incrementing so a
                    // long-converged pixel becomes responsive the instant we move.
                    float hAge = max(hSky.a, 1.0);
                    age   = min(min(hAge, MOVING_MAX_AGE) + 1.0, MOVING_MAX_AGE);
                    accum = mix(hSky.rgb, curSky, 1.0 / age);
                }
            }
        }
    }

    gl_FragData[0] = vec4(accum, age);
    gl_FragData[1] = vec4(curP, curD);
    gl_FragData[2] = vec4(curN, 0.0);
}
