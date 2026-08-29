$input v_color0
$input v_texcoord0

// =============================================================================
// taa.frag  --  Temporal anti-aliasing of the composed HDR frame.
//
// Resolves the gbuffer's per-frame sub-pixel jitter into anti-aliased edges (and keeps them
// smooth while the camera moves). Based on the fast renderer's taa.frag, with one change aimed at
// its "blurry under movement" weakness: the reprojected history is resampled with a sharp 9-tap
// CATMULL-ROM filter instead of plain bilinear. Bilinear refetch re-blurs the history a little
// every single frame, so under sustained motion the image softens; Catmull-Rom is (near) sharpness-
// preserving, so moving edges stay crisp while still being anti-aliased.
//
//   STILL  -> identity accumulation, own texel, long running mean -> supersampled crisp edges.
//   MOVING -> reproject this hit into last frame (worldToUV), Catmull-Rom history fetch, clamp to
//             the current 3x3 colour AABB (deghost), loose position gate, short history.
//
// Inputs (FBO 8 [0], FBO 1 [0..3], FBO 9 [0..1]):
//   0 curColor   1 gPos  2 gNormal  3 gAlbedo  4 gDirect   5 histColor  6 histPos
// Outputs (FBO 9, ping-pong):
//   0 accumColor rgb = anti-aliased HDR, a = age    1 histPos (carried for next reprojection)
// =============================================================================

#include <bgfx_shader.sh>

// Renderer 6 inputs [FBO8, FBO1, FBO9]. FBO1 has 6 targets, so the FBO9 history binds at 7/8.
SAMPLER2D(curColor,  0);   // FBO8[0]: composed HDR this frame
SAMPLER2D(gPos,      1);   // FBO1[0]: xyz world pos, a = camDist (a < 0 => sky)
SAMPLER2D(gNormal,   2);   // FBO1[1] (bound, unused -- no normal gate, see fast renderer note)
SAMPLER2D(gAlbedo,   3);   // FBO1[2] (bound, unused)
SAMPLER2D(gDirect,   4);   // FBO1[3] (bound, unused)
SAMPLER2D(histColor, 7);   // FBO9[0] prev: accumulated HDR + age
SAMPLER2D(histPos,   8);   // FBO9[1] prev: world pos for reprojection validation

uniform vec4 windowRes;
uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;
uniform vec4 frameCount;

#define FOV 60.0

// STILL long so jitter fully resolves; MOVING short so reprojected history stays responsive. A bit
// longer than the fast renderer's 8 because Catmull-Rom keeps it sharp, so we can hold more history
// for stability without the extra blur.
#define STILL_MAX_AGE  64.0
#define MOVING_MAX_AGE 12.0
// Neighbourhood variance-clip width (mean +- GAMMA*stddev). ~1 is tight (crisp, more clipping),
// larger keeps more history (smoother, risks ghosting). 1.25 is a robust middle ground.
#define VARIANCE_GAMMA 1.25

// Exact inverse of rayStartDirection (same as the reprojection renderer's worldToUV): a world point
// -> the screen UV it projects to under the given camera.
vec2 worldToUV(vec3 P, vec3 camPos, vec3 camDir, vec2 res, float fov, out bool valid) {
    vec3 forward = normalize(camDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));
    vec3 v = P - camPos;
    float z = dot(v, forward);
    if (z <= 1e-4) { valid = false; return vec2(-1.0); }
    float scale  = tan(radians(fov * 0.5));
    float aspect = res.x / res.y;
    float ndcx = (dot(v, right) / z) / (scale * aspect);
    float ndcy = (dot(v, up)    / z) / scale;
    vec2 flip = (vec2(ndcx, ndcy) + 1.0) * 0.5;
    vec2 uv = vec2(flip.x, 1.0 - flip.y);
    valid = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    return uv;
}

// Sharp 9-tap Catmull-Rom history resample (the standard bilinear-optimized form: 5 bilinear
// fetches). Preserves edge sharpness across reprojection, unlike a single bilinear tap.
vec3 sampleCatmullRom(sampler2D tex, vec2 uv, vec2 res) {
    vec2 samplePos = uv * res;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 texPos0  = (texPos1 - 1.0)      / res;
    vec2 texPos3  = (texPos1 + 2.0)      / res;
    vec2 texPos12 = (texPos1 + offset12) / res;

    vec3 result = vec3(0.0);
    result += texture2D(tex, vec2(texPos0.x,  texPos0.y )).rgb * (w0.x  * w0.y );
    result += texture2D(tex, vec2(texPos12.x, texPos0.y )).rgb * (w12.x * w0.y );
    result += texture2D(tex, vec2(texPos3.x,  texPos0.y )).rgb * (w3.x  * w0.y );
    result += texture2D(tex, vec2(texPos0.x,  texPos12.y)).rgb * (w0.x  * w12.y);
    result += texture2D(tex, vec2(texPos12.x, texPos12.y)).rgb * (w12.x * w12.y);
    result += texture2D(tex, vec2(texPos3.x,  texPos12.y)).rgb * (w3.x  * w12.y);
    result += texture2D(tex, vec2(texPos0.x,  texPos3.y )).rgb * (w0.x  * w3.y );
    result += texture2D(tex, vec2(texPos12.x, texPos3.y )).rgb * (w12.x * w3.y );
    result += texture2D(tex, vec2(texPos3.x,  texPos3.y )).rgb * (w3.x  * w3.y );
    return result;
}

void main() {
    vec2 uv = v_texcoord0;

    vec3 curRad = texture2D(curColor, uv).rgb;
    vec4 pos4   = texture2D(gPos, uv);
    vec3 curP   = pos4.xyz;
    float curD  = pos4.w;

    // Sky / background (gPos.a < 0): nothing jitter-dependent to resolve, pass through.
    if (curD < 0.0) {
        gl_FragData[0] = vec4(curRad, 1.0);
        gl_FragData[1] = pos4;
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
            sampUV = worldToUV(curP, prevCameraPos.xyz, prevCameraDir.xyz,
                               windowRes.xy, FOV, valid);
        } else {
            // Still: identity, own texel centre -> zero resampling blur while jitter converges.
            sampUV = (floor(uv * windowRes.xy) + 0.5) / windowRes.xy;
        }

        if (valid) {
            float hAge = texture2D(histColor, sampUV).a;
            if (isnan(hAge) || hAge < 1.0) hAge = 1.0;

            // Sharp Catmull-Rom history fetch when moving; identity texel needs no resample.
            vec3 hist = moving ? sampleCatmullRom(histColor, sampUV, windowRes.xy)
                               : texture2D(histColor, sampUV).rgb;
            bool sane = !any(isnan(hist));

            if (moving) {
                // VARIANCE-CLIP deghoster (3x3 of the current frame). A plain min/max colour box is
                // fragile here: the composite still carries some GI noise at fresh/edge pixels, so a
                // single firefly neighbour blows the box's max up (a bright value then pollutes the
                // history and recovers only slowly) and a single black-noise neighbour drops the box's
                // min to ~0 (history clamped to black -> the "edges go black" pop). Clipping instead to
                // mean +- GAMMA*stddev is robust to those single outliers: one bad neighbour widens the
                // spread but can't collapse the interval onto itself. This is the standard robust TAA
                // neighbourhood clip. No normal gate (voxel micro-face staircases must average -- see
                // the fast renderer). A loose distance-scaled position gate catches gross disocclusion.
                vec2 texel = 1.0 / windowRes.xy;
                vec3 m1 = vec3(0.0), m2 = vec3(0.0);
                for (int y = -1; y <= 1; y++)
                for (int x = -1; x <= 1; x++) {
                    vec3 s = texture2D(curColor, uv + vec2(float(x), float(y)) * texel).rgb;
                    m1 += s; m2 += s * s;
                }
                vec3 mu    = m1 / 9.0;
                vec3 sigma = sqrt(max(m2 / 9.0 - mu * mu, vec3(0.0)));
                vec3 cmin  = mu - VARIANCE_GAMMA * sigma;
                vec3 cmax  = mu + VARIANCE_GAMMA * sigma;
                // Catmull-Rom has negative lobes and rings below zero at high-contrast HDR edges
                // (another source of black halos); force the history non-negative before clipping.
                hist = clamp(max(hist, vec3(0.0)), cmin, cmax);

                vec3  hPos   = texture2D(histPos, sampUV).xyz;
                float posTol = max(0.5, curD * 0.03);
                if (distance(curP, hPos) >= posTol) sane = false;
            }

            if (sane) {
                age = min(min(hAge, maxAge) + 1.0, maxAge);
                accum = mix(hist, curRad, 1.0 / age);   // equal-weight running mean
            }
        }
    }

    gl_FragData[0] = vec4(accum, age);
    gl_FragData[1] = vec4(curP, curD);
}
