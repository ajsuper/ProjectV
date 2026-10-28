$input v_color0
$input v_texcoord0

// =============================================================================
// taa.frag -- pass 5, at OUTPUT resolution, after the reconstruction. (docs/plans/voxel-renderer.md)
//
// The reconstruction antialiases every edge it can see from one frame. What it cannot do is show a voxel
// no render texel hit: anything thinner than a texel comes and goes as the rays land on it or miss. So the
// G-buffer is jittered through the centres of the output pixels each render texel covers (2x2 of them at
// half scale; voxel_renderer.h): each frame, one pixel in each block is sampled exactly at its centre,
// and over the cycle every pixel is. A pixel takes most of a frame whose sample was its own and almost
// nothing of the others -- so a still view, which repeats the same samples, settles to the full-resolution
// image (antialiased by the reconstruction) and stays exactly there, and a change reaches every pixel
// within one cycle.
//
// It is built to stay responsive -- nothing it shows is more than a few frames old:
//   * reproject by the G-buffer's motion vectors, which carry each chunk's own motion as well as the
//     camera's: a falling crate's history is found where the crate was, not where the wall behind it was.
//     The motion is taken from the nearest surface in the 3x3 render texels round the pixel, so an edge
//     moves with what is in front;
//   * fetch history with a Catmull-Rom filter, which stays sharp under motion;
//   * tell whether history shows the same surface: the depth it showed (kept in its alpha) against where
//     the surfaces round this pixel were last frame (gMotion.z, the 3x3's range -- a thin pole or a gap
//     the jitter finds only some frames is still the same view). Uncovered -- something moved off it --
//     is not, and takes this frame;
//   * clip it to this frame's 3x3 colour distribution (YCoCg): tight while moving or uncovered, so
//     nothing ghosts; on a still, same surface only in the frames that sampled the pixel itself (the
//     others' boxes are of other samples, and would pull at detail they missed);
//   * blend: on a still surface by how near this frame's sample is to the pixel's centre; moving, a tenth
//     a frame or more (and more where the sample is the pixel's own); luminance-weighted so one bright
//     sample cannot flicker through.
//
// Inputs: FBO 4 (the reconstructed frame, slot 0), FBO 1 (G-buffer, slots 1..6: gPos 1, gMotion 4),
// FBO 5 (history, slot 7 -- ping-pong, the engine alternates it). Output (FBO 5): rgb the frame, a the
// distance from the camera of what it shows (the nearest round it).
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_filter.sc>     // pjvCatmullRomTaps
#include <grr_common.sc>

SAMPLER2D(curColor,  0);
SAMPLER2D(gPos,      1);
SAMPLER2D(gMotion,   4);
SAMPLER2D(histColor, 7);

#include <pjv_engine.sc>   // passTargetRes, passInputRes: engine-set
uniform vec4 frameParams;    // x = frame, z = TAA on
uniform vec4 renderParams;   // z = reconstruction bypass (the TAA steps aside too, so the A/B is raw)
uniform vec4 jitterParams;   // xy: where this frame's texels were traced, off their centres, in texels

// The weight of the new frame: a still surface, a moving one, one moving fast (MOTION_FAST output pixels
// a frame or more), and one just uncovered.
// A still surface: a frame whose sample is the pixel's own, and one whose sample is a neighbour's.
#ifndef BLEND_OWN
#define BLEND_OWN 0.8
#endif
#ifndef BLEND_OTHER
#define BLEND_OTHER 0.0
#endif
// How near, in output pixels, a sample must be to the pixel's centre to count as its own.
#ifndef OWN_RADIUS
#define OWN_RADIUS 0.3
#endif
#ifndef BLEND_MOVING
#define BLEND_MOVING 0.1
#endif
#ifndef BLEND_FAST
#define BLEND_FAST 0.25
#endif
#ifndef BLEND_NEW
#define BLEND_NEW 0.5
#endif
#ifndef MOTION_FAST
#define MOTION_FAST 16.0
#endif
// Clip box half-width in standard deviations: a still surface, a moving or uncovered one.
#ifndef CLIP_GAMMA_SETTLE
#define CLIP_GAMMA_SETTLE 1.5
#endif
#ifndef CLIP_GAMMA_MOVING
#define CLIP_GAMMA_MOVING 1.0
#endif
// How far, relative, a depth may be outside the 3x3's range and still be the same view.
#ifndef DEPTH_TOLERANCE
#define DEPTH_TOLERANCE 0.03
#endif
#define SKY_DEPTH 60000.0

vec3 toYCoCg(vec3 c) {
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}
vec3 fromYCoCg(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// The 9-tap Catmull-Rom as five bilinear fetches. Can ring slightly; the clip after it catches that.
vec3 sampleCatmullRom(vec2 uv, vec2 res) {
    PjvCatmullRomTaps t = pjvCatmullRomTaps(uv, res);
    vec2 tc0 = t.tc0, tc12 = t.tc12, tc3 = t.tc3;
    vec2 w0 = t.w0, w12 = t.w12, w3 = t.w3;
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

    // ---- Where was this pixel last frame? The motion of the nearest surface round it, and the range
    // of distances the surfaces round it were at from last frame's camera
    vec2 gRes = passInputRes[1].xy;
    vec2 gTexel = passInputRes[1].zw;
    vec2 gCentre = (floor(uv * gRes) + 0.5) * gTexel;
    vec2 nearestUV = gCentre;
    float nearest = SKY_DEPTH, wasNear = SKY_DEPTH, wasFar = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            vec2 tap = gCentre + vec2(float(i), float(j)) * gTexel;
            float d = texture2D(gPos, tap).w;
            if (d >= 0.0 && d < nearest) { nearest = d; nearestUV = tap; }
            float was = texture2D(gMotion, tap).z;
            wasNear = min(wasNear, was);
            wasFar = max(wasFar, was);
        }
    if (frameParams.z < 0.5 || renderParams.z > 0.5 || frameParams.x < 1.0) {
        gl_FragColor = vec4(current, nearest);
        return;
    }
    vec2 motion = texture2D(gMotion, nearestUV).xy;
    vec2 prevUV = uv + motion;
    if (prevUV.x < 0.0 || prevUV.y < 0.0 || prevUV.x > 1.0 || prevUV.y > 1.0) {
        gl_FragColor = vec4(current, nearest);
        return;
    }
    vec2 res = passTargetRes.xy;
    float speed = length(motion * res);   // output pixels a frame

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
    // The same view as last frame, or uncovered? The depth history showed, against the 3x3's range then.
    float shown = texture2D(histColor, prevUV).a;
    bool same = shown > wasNear * (1.0 - DEPTH_TOLERANCE) && shown < wasFar * (1.0 + DEPTH_TOLERANCE);
    float settle = same ? 1.0 - clamp(speed, 0.0, 1.0) : 0.0;
    float gamma = mix(CLIP_GAMMA_MOVING, CLIP_GAMMA_SETTLE, settle);
    // How much this frame's sample is this pixel's own: its distance from the pixel's centre, in output
    // pixels (texel i was traced through i + 0.5 + jitter).
    vec2 off = (fract(uv * gRes - jitterParams.xy) - 0.5) * (passTargetRes.xy / gRes);
    float own = exp(-dot(off, off) / (2.0 * OWN_RADIUS * OWN_RADIUS));
    float clipStrength = mix(1.0, own, settle);

    // A still pixel's history is read exactly where it is: no resampling blur at all.
    vec3 history = speed < 0.01 ? texture2D(histColor, prevUV).rgb : sampleCatmullRom(prevUV, res);
    vec3 hY = toYCoCg(history);
    // Clip toward the box centre rather than clamping per channel: keeps the hue of what is clipped.
    vec3 toCentre = hY - m1;
    vec3 extent = max(gamma * sigma, vec3(1e-5, 1e-5, 1e-5));
    vec3 ratio = abs(toCentre) / extent;
    float worst = max(ratio.x, max(ratio.y, ratio.z));
    if (worst > 1.0) hY = mix(hY, m1 + toCentre / worst, clipStrength);

    vec3 cY = toYCoCg(current);
    float moving = max(mix(BLEND_MOVING, BLEND_FAST, clamp(speed / MOTION_FAST, 0.0, 1.0)), own * BLEND_OWN);
    float still = mix(BLEND_OTHER, BLEND_OWN, own);
    float alpha = same ? mix(moving, still, settle) : max(BLEND_NEW, own * BLEND_OWN);
    // Luminance weighting (Karis): each sample counts as 1 / (1 + luma), so a firefly does not dominate.
    float wc = alpha / (1.0 + cY.x);
    float wh = (1.0 - alpha) / (1.0 + max(hY.x, 0.0));
    vec3 result = fromYCoCg((cY * wc + hY * wh) / (wc + wh));
    gl_FragColor = vec4(max(result, vec3(0.0, 0.0, 0.0)), nearest);
}
