#ifndef PJV_FILTER_SC
#define PJV_FILTER_SC

// =============================================================================
// pjv_filter.sc -- reconstruction and resampling weights (docs/plans/shader-library.md)
// =============================================================================
//
// Weights and tap positions only. The caller samples its own textures with them, so nothing here
// names a sampler.

// The bilinear tent of a texel centred at tapCentre, at srcPos (both in texels): 1 at its centre,
// 0 one texel away on either axis.
float pjvTexelTent(vec2 tapCentre, vec2 srcPos) {
    vec2 d = abs(tapCentre - srcPos);
    return max(0.0, 1.0 - d.x) * max(0.0, 1.0 - d.y);
}

// Catmull-Rom (bicubic) resampling in five bilinear taps instead of sixteen point ones (Jimenez,
// after Sigg & Hadwiger). For uv on a texture of res texels it gives three tap columns and rows, in
// uv, and their weights; the caller sums
//
//     tex(tc12.x, tc0.y)  * (w12.x * w0.y)  + tex(tc0.x, tc12.y) * (w0.x * w12.y)
//   + tex(tc12.x, tc12.y) * (w12.x * w12.y)
//   + tex(tc3.x, tc12.y)  * (w3.x * w12.y)  + tex(tc12.x, tc3.y) * (w12.x * w3.y)
//
// The corners are dropped; renormalise by the sum of the five weights. The kernel overshoots, so a
// temporal filter clamps what comes out.
struct PjvCatmullRomTaps {
    vec2 tc0, tc12, tc3;
    vec2 w0, w12, w3;
};

PjvCatmullRomTaps pjvCatmullRomTaps(vec2 uv, vec2 res) {
    vec2 samplePos = uv * res;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    PjvCatmullRomTaps taps;
    taps.w0 = w0;
    taps.w12 = w1 + w2;
    taps.w3 = w3;
    taps.tc0 = (texPos1 - 1.0) / res;
    taps.tc3 = (texPos1 + 2.0) / res;
    taps.tc12 = (texPos1 + w2 / taps.w12) / res;
    return taps;
}

#endif
