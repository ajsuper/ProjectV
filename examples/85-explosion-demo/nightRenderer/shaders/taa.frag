$input v_color0
$input v_texcoord0

// =============================================================================
// taa.frag -- pass 5, at OUTPUT resolution, after the reconstruction.
//
// The reconstruction antialiases every edge it can see from one frame. What it cannot do is invent a
// voxel no render texel hit -- a blade of grass, a railing, anything thinner than a render pixel comes
// and goes as the samples land on it or miss. The G-buffer's sub-pixel jitter makes each frame sample a
// different point, and this pass averages them: the coverage the quarter-rate frames never had one at a
// time, they have together.
//
// The standard shape, and nothing more:
//   * reproject: the world point under this pixel, through LAST frame's camera, to where it was;
//   * fetch history there with a Catmull-Rom filter, which stays sharp under motion where a bilinear
//     refetch would soften the image a little more every frame;
//   * clip it to the current 3x3 neighbourhood's colour distribution (mean +- a few deviations, in
//     YCoCg), which is what removes ghosts: history that no longer fits what is here now is pulled in;
//   * blend with a luminance weight, so one bright sample cannot flicker through the average.
//
// Order matters: reconstructing AFTER this would rebuild edges from a jittered G-buffer every frame with
// nothing downstream to average them. Here the edges are rebuilt first and averaged second.
//
// Inputs: FBO 4 (the reconstructed frame, slot 0), FBO 1 (G-buffer, slots 1..6), FBO 5 (history,
// slot 7 -- ping-pong, the engine alternates it). Output (FBO 5).
// =============================================================================

#include <bgfx_shader.sh>
#include <grr_common.sc>

SAMPLER2D(curColor,  0);
SAMPLER2D(gPos,      1);
SAMPLER2D(histColor, 7);

#include <pjv_engine.sc>   // passTargetRes, passInputRes: engine-set
uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;
uniform vec4 frameParams;    // x = frame, z = TAA on, w = camera moved this frame
uniform vec4 renderParams;   // z = reconstruction bypass (taa steps aside too, so the A/B is raw)

// The weight of the new frame. Lower converges further (smoother edges) and ghosts more; the clip
// below is what keeps a low value safe.
#define BLEND_STILL  0.08
#define BLEND_MOVING 0.14
// Clip box half-width in standard deviations. Wider holds more history (stabler, more ghosting).
#define CLIP_GAMMA_STILL  1.5
#define CLIP_GAMMA_MOVING 1.0

vec3 toYCoCg(vec3 c) {
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}
vec3 fromYCoCg(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// The 9-tap Catmull-Rom as five bilinear fetches. Can ring slightly; the clip after it catches that.
vec3 sampleCatmullRom(vec2 uv, vec2 res) {
    vec2 samplePos = uv * res;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 tc0 = (texPos1 - 1.0) / res;
    vec2 tc3 = (texPos1 + 2.0) / res;
    vec2 tc12 = (texPos1 + w2 / w12) / res;
    vec3 r = texture2D(histColor, vec2(tc12.x, tc0.y)).rgb * (w12.x * w0.y)
           + texture2D(histColor, vec2(tc0.x, tc12.y)).rgb * (w0.x * w12.y)
           + texture2D(histColor, vec2(tc12.x, tc12.y)).rgb * (w12.x * w12.y)
           + texture2D(histColor, vec2(tc3.x, tc12.y)).rgb * (w3.x * w12.y)
           + texture2D(histColor, vec2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, vec3(0.0, 0.0, 0.0));
}

void main() {
    vec2 uv = v_texcoord0;
    vec3 current = texture2D(curColor, uv).rgb;
    if (frameParams.z < 0.5 || renderParams.z > 0.5 || frameParams.x < 1.0) {
        gl_FragColor = vec4(current, 1.0);
        return;
    }

    // ---- Where was this pixel last frame? ---------------------------------------------------------
    // The world point is read from the G-buffer at render resolution, from the NEAREST-DEPTH texel of
    // the 3x3 around this pixel: at a silhouette that reprojects the edge with the foreground's motion,
    // which is the surface the eye tracks.
    vec2 gRes = passInputRes[1].xy;
    vec2 gTexel = passInputRes[1].zw;
    vec2 gCentre = (floor(uv * gRes) + 0.5) * gTexel;
    vec4 nearest = texture2D(gPos, gCentre);
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            vec4 g = texture2D(gPos, gCentre + vec2(float(i), float(j)) * gTexel);
            if (g.w >= 0.0 && (nearest.w < 0.0 || g.w < nearest.w)) nearest = g;
        }

    vec3 world;
    if (nearest.w < 0.0) {
        // Sky: only the camera's turn moves it. A point far along this pixel's ray, offset by the
        // previous camera's position so translation does not move it at all.
        world = prevCameraPos.xyz + grrCameraRay(uv, passTargetRes.xy, cameraDir.xyz) * 1.0e4;
    } else {
        world = nearest.xyz;
    }
    bool valid;
    vec2 prevUV = grrWorldToUV(world, prevCameraPos.xyz, prevCameraDir.xyz, passTargetRes.xy, valid);
    bool moved = frameParams.w > 0.5;
    if (!moved) prevUV = uv;   // exactly: no resampling blur at all while still
    if (!valid && moved) {
        gl_FragColor = vec4(current, 1.0);
        return;
    }

    // ---- The neighbourhood this frame actually shows ----------------------------------------------
    vec2 texel = passTargetRes.zw;
    vec3 m1 = vec3(0.0, 0.0, 0.0), m2 = vec3(0.0, 0.0, 0.0);
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            vec3 c = toYCoCg(texture2D(curColor, uv + vec2(float(i), float(j)) * texel).rgb);
            m1 += c;
            m2 += c * c;
        }
    m1 /= 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0, 0.0, 0.0)));
    float gamma = moved ? CLIP_GAMMA_MOVING : CLIP_GAMMA_STILL;
    vec3 boxMin = m1 - gamma * sigma;
    vec3 boxMax = m1 + gamma * sigma;

    vec3 history = moved ? sampleCatmullRom(prevUV, passTargetRes.xy) : texture2D(histColor, prevUV).rgb;
    vec3 hY = toYCoCg(history);
    // Clip toward the box centre rather than clamping per channel: keeps the hue of what is clipped.
    vec3 toCentre = hY - m1;
    vec3 extent = max(boxMax - m1, vec3(1e-5, 1e-5, 1e-5));
    vec3 ratio = abs(toCentre) / extent;
    float worst = max(ratio.x, max(ratio.y, ratio.z));
    if (worst > 1.0) hY = m1 + toCentre / worst;

    vec3 cY = toYCoCg(current);
    float alpha = moved ? BLEND_MOVING : BLEND_STILL;
    // Luminance weighting (Karis): each sample counts as 1 / (1 + luma), so a firefly does not dominate.
    float wc = alpha / (1.0 + cY.x);
    float wh = (1.0 - alpha) / (1.0 + max(hY.x, 0.0));
    vec3 result = fromYCoCg((cY * wc + hY * wh) / (wc + wh));
    gl_FragColor = vec4(max(result, vec3(0.0, 0.0, 0.0)), 1.0);
}
