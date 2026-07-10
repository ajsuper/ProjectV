$input v_color0
$input v_texcoord0

// =============================================================================
// taa.frag  --  Pass 2 of the fast renderer.
//
// Temporal anti-aliasing. The shading from pass 1 is already noise-free, so this
// pass has exactly one job: fold the per-frame sub-pixel JITTER into smooth,
// anti-aliased edges (and keep them smooth while the camera moves).
//
// Two regimes, chosen by whether the camera moved this frame:
//
//   STILL  -> identity accumulation. History is read straight from THIS pixel
//             (snapped to its own texel centre, no resampling blur) and the new
//             jittered sample is folded into a long running mean. Over a handful
//             of frames each pixel averages its sub-pixel coverage == supersampled,
//             crisp edges. No surface validation is needed: the scene is static and
//             the camera is not moving, so a pixel cannot change surface -- and NOT
//             validating is what lets edge pixels average foreground/background
//             across jitter offsets, which IS the anti-aliasing.
//
//   MOVING -> reproject this frame's world hit through LAST frame's camera, sample
//             the history bilinearly, then CLAMP it to the local 3x3 colour AABB of
//             the current frame. The neighbourhood clamp is what kills ghosting and
//             disocclusion smear without a hard history reject, so moving edges stay
//             anti-aliased instead of snapping back to aliased. History is kept short
//             so it stays responsive.
//
// Inputs (sampler order matches the flattened FBO dependency list):
//   0 curColor  1 curPos  2 curNormal            (FBO 1, this frame)
//   3 histColor 4 histPos 5 histNormal           (FBO 2, previous frame)
// Outputs (FBO 2, ping-pong):
//   0 accumColor rgb = anti-aliased radiance, a = age
//   1 curPos     carried forward for next frame's reprojection
//   2 curNormal  carried forward for next frame's reprojection
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(curColor,   0);
SAMPLER2D(curPos,     1);
SAMPLER2D(curNormal,  2);
SAMPLER2D(histColor,  3);
SAMPLER2D(histPos,    4);
SAMPLER2D(histNormal, 5);

uniform vec4 windowRes;
uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;
uniform vec4 frameCount;

#define FOV 60.0

// History length caps. STILL is long so jitter fully resolves into supersampled
// edges; MOVING is short so reprojected history stays responsive (the clamp keeps
// it clean). Both stay within fp16's exactly-representable integer range.
#define STILL_MAX_AGE  64.0
#define MOVING_MAX_AGE 8.0

// Project a world-space point into a camera and return its screen UV. Exact
// inverse of pjv_utils_DDA.sc::rayStartDirection (same as the reprojection
// renderer's worldToUV) so a surface point lands on the pixel it came from.
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

    vec4 c        = texture2D(curColor, uv);
    vec3 curRad   = c.rgb;
    float hitMask = c.a;
    vec4 pos4     = texture2D(curPos,   uv);
    vec3 curP     = pos4.xyz;
    float curD    = pos4.w;
    vec3 curN     = texture2D(curNormal, uv).xyz;

    // Sky / background: nothing jitter-dependent to resolve, pass it straight through.
    if (hitMask < 0.5) {
        gl_FragData[0] = vec4(curRad, 1.0);
        gl_FragData[1] = pos4;
        gl_FragData[2] = vec4(curN, 0.0);
        return;
    }

    bool  moving = frameCount.y != 0.0;
    float maxAge = moving ? MOVING_MAX_AGE : STILL_MAX_AGE;

    vec3  accum = curRad;
    float age   = 1.0;

    if (frameCount.x > 0.0) {
        vec2 sampUV;
        bool valid = true;

        if (moving) {
            // Reproject the current hit into last frame's screen space.
            sampUV = worldToUV(curP, prevCameraPos.xyz, prevCameraDir.xyz,
                               windowRes.xy, FOV, valid);
        } else {
            // Still: identity. Read this pixel's own history at its texel centre so
            // there is zero resampling blur while jitter converges.
            sampUV = (floor(uv * windowRes.xy) + 0.5) / windowRes.xy;
        }

        if (valid) {
            vec4 hColor = texture2D(histColor, sampUV);
            float hAge  = hColor.a;
            if (isnan(hAge) || hAge < 1.0) hAge = 1.0;
            bool sane = !any(isnan(hColor.rgb));

            vec3 hist = hColor.rgb;

            if (moving) {
                // Neighbourhood colour clamp (3x3 of the current frame). This is the
                // deghoster: reprojected history that no longer fits the local colour
                // distribution (occlusion, edges) is pulled back toward it instead of
                // smearing. Also validate the reprojected surface so a wildly wrong
                // reprojection restarts cleanly.
                vec2 texel = 1.0 / windowRes.xy;
                vec3 cmin = curRad;
                vec3 cmax = curRad;
                for (int y = -1; y <= 1; y++) {
                    for (int x = -1; x <= 1; x++) {
                        vec3 s = texture2D(curColor, uv + vec2(float(x), float(y)) * texel).rgb;
                        cmin = min(cmin, s);
                        cmax = max(cmax, s);
                    }
                }
                hist = clamp(hist, cmin, cmax);

                // No normal gate (matches sky_reproject.frag): a far diagonal wall is a
                // staircase of perpendicular voxel micro-faces, and sub-pixel jitter
                // lands the primary ray on a different one almost every frame. Its
                // albedo/direct differ per face, so a dot(curN,hNrm) gate failed on
                // nearly every reprojection there and reset the history each frame ->
                // the source aliasing showed as flicker instead of averaging out. The
                // 3x3 colour clamp above is the deghoster; a loose, distance-scaled
                // position gate is kept only to catch a gross disocclusion.
                vec3  hPos   = texture2D(histPos, sampUV).xyz;
                float posTol = max(0.5, curD * 0.03);
                if (distance(curP, hPos) >= posTol) sane = false;
            }

            if (sane) {
                age = min(min(hAge, maxAge) + 1.0, maxAge);
                float alpha = 1.0 / age;          // equal-weight running mean
                accum = mix(hist, curRad, alpha);
            }
        }
    }

    gl_FragData[0] = vec4(accum, age);
    gl_FragData[1] = vec4(curP, curD);
    gl_FragData[2] = vec4(curN, 0.0);
}
