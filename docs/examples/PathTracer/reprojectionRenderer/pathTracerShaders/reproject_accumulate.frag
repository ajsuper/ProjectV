$input v_color0
$input v_texcoord0

// =============================================================================
// reproject_accumulate.frag  --  Pass 2 of the reprojection renderer.
//
// Temporal accumulation of the noisy 1-spp path-traced frame. This is both the
// denoiser's temporal half AND the anti-aliaser: the primary ray is sub-pixel
// JITTERED in pass 1, and folding those jittered samples into a running mean is
// what turns stair-stepped edges into smooth ones.
//
// Two regimes, chosen by whether the camera moved this frame (frameCount.y):
//
//   STILL  -> IDENTITY accumulation. The camera has not moved, so every pixel's
//             surface point is still under that same pixel; we read history from
//             THIS pixel's own texel (zero resampling error) and fold the new
//             jittered sample into an UNBOUNDED running mean. No surface validation
//             and no colour rectification are done, and that is deliberate:
//               * unbounded + no rectification  => the mean converges all the way to
//                 zero noise (a true frame-count average, like the tree64 renderer).
//                 This is why histColor is a 32F target -- an fp16 accumulator
//                 quantizes the tiny late updates away and freezes short of clean.
//               * no validation                 => at a silhouette the jitter makes
//                 the pixel land on foreground some frames and background others;
//                 averaging them is exactly the anti-aliasing. Validating here would
//                 reject that and snap edges back to aliased.
//
//   MOVING -> REPROJECT this frame's world hit through last frame's camera and read
//             the history BILINEARLY there. History is rejected purely on GEOMETRY
//             (world position + normal), never on colour:
//               * a silhouette / disocclusion moves the reprojected point onto a
//                 different surface -> position mismatch -> reject -> fresh sample
//                 (noisy for a few frames, cleaned by the spatial denoise pass, but
//                 never a ghost).
//               * a correctly tracked surface passes, and since diffuse GI is
//                 view-independent and the scene is static, accumulating it while
//                 moving is CORRECT, not a smear.
//             We do NOT colour-clamp: clamping a bright converged history against a
//             noisy 1-spp neighbourhood mean (which sits below the true mean) drags
//             the image dark every frame -- that was the "goes dark when I move" bug.
//             A moderate age cap bounds the residual bilinear-resampling softening.
//
// Inputs (sampler order matches the flattened FBO dependency list):
//   0 curColor   1 curPos   2 curNormal   3 curAlbedo   (FBO 1, this frame)
//   4 histColor  5 histPos  6 histNormal                (FBO 2, previous frame)
// Outputs (FBO 2, ping-pong):
//   0 accumColor rgb = accumulated radiance, a = age
//   1 curPos     carried forward for next frame's reprojection/validation
//   2 curNormal  carried forward for next frame's reprojection/validation
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(curColor,   0);
SAMPLER2D(curPos,     1);
SAMPLER2D(curNormal,  2);
SAMPLER2D(curAlbedo,  3);
SAMPLER2D(histColor,  4);
SAMPLER2D(histPos,    5);
SAMPLER2D(histNormal, 6);

uniform vec4 windowRes;
uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;
uniform vec4 frameCount;

#define FOV 60.0

// Moving-camera history cap. Kept moderate: long enough to knock down most of the
// 1-spp noise while moving, short enough that the mild bilinear-resampling softening
// on moving edges refreshes instead of lingering. Still-camera history is UNBOUNDED
// (handled separately below) so the parked image converges to zero noise.
#define MOVING_MAX_AGE 24.0

// Project a world-space point into a camera and return its screen UV. Exact inverse
// of pjv_utils_DDA.sc::rayStartDirection (same basis, same NDC flip), so a surface
// point reprojects to the pixel it was rendered from -- including under pitch.
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

    vec4 c        = texture2D(curColor,  uv);
    vec3 curRad   = c.rgb;
    float hitMask = c.a;
    vec4 pos4     = texture2D(curPos,    uv);
    vec3 curP     = pos4.xyz;
    float curD    = pos4.w;
    vec3 curN     = texture2D(curNormal, uv).xyz;

    // Sky / background: no surface to track, already stable. Pass it through.
    if (hitMask < 0.5) {
        gl_FragData[0] = vec4(curRad, 1.0);
        gl_FragData[1] = pos4;
        gl_FragData[2] = vec4(curN, 0.0);
        return;
    }

    // Default (first frame / disocclusion): show the fresh sample at age 1.
    vec3  accum = curRad;
    float age   = 1.0;

    bool moving = frameCount.y != 0.0;

    if (frameCount.x > 0.0) {
        if (!moving) {
            // ---- STILL: identity accumulation, unbounded, no validation. ----
            // Read this pixel's own texel centre (no resampling), average forever.
            vec2  ownUV  = (floor(uv * windowRes.xy) + 0.5) / windowRes.xy;
            vec4  hColor = texture2D(histColor, ownUV);
            if (!any(isnan(hColor.rgb)) && !isnan(hColor.a)) {
                age = max(hColor.a, 1.0) + 1.0;   // grow without bound -> converges to 0 noise
                accum = mix(hColor.rgb, curRad, 1.0 / age);
            }
        } else {
            // ---- MOVING: reproject, bilinear read, geometry-gated, no clamp. ----
            bool valid;
            vec2 prevUV = worldToUV(curP, prevCameraPos.xyz, prevCameraDir.xyz,
                                    windowRes.xy, FOV, valid);
            if (valid) {
                vec4 hColor = texture2D(histColor,  prevUV);   // bilinear
                vec3 hPos   = texture2D(histPos,    prevUV).xyz;
                vec3 hNrm   = texture2D(histNormal, prevUV).xyz;

                // Reject on geometry only: a moved-onto-different-surface reprojection
                // (silhouette / disocclusion) fails the position or normal test and
                // restarts clean instead of ghosting. Tolerance grows a little with
                // distance so far surfaces are not rejected for sub-texel drift.
                float posTol   = max(0.35, curD * 0.02);
                bool  posOK    = distance(curP, hPos) < posTol;
                bool  normalOK = dot(curN, hNrm) > 0.9;
                bool  sane     = !any(isnan(hColor.rgb)) && !isnan(hColor.a);

                if (posOK && normalOK && sane) {
                    // Clamp the stored age down to the cap BEFORE incrementing so a
                    // long-converged pixel becomes responsive the instant we move.
                    float hAge = max(hColor.a, 1.0);
                    age = min(min(hAge, MOVING_MAX_AGE) + 1.0, MOVING_MAX_AGE);
                    accum = mix(hColor.rgb, curRad, 1.0 / age);
                }
            }
        }
    }

    gl_FragData[0] = vec4(accum, age);
    gl_FragData[1] = vec4(curP, curD);
    gl_FragData[2] = vec4(curN, 0.0);
}
