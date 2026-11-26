$input v_color0
$input v_texcoord0

// shadow_blur.frag -- render resolution, after shadow_soft.frag: the penumbra's visibility averaged over
// RESTIR_SHADOW_BLUR of its own half-width (at most RESTIR_SHADOW_BLUR_MAX texels), on this surface only
// (plane and face normal, as shadow_soft's own taps), with a tent's weights. shadow_soft averages 32 binary
// visibilities on a disc that turns per pixel and per frame, so each texel's answer is off by a few
// percent in its own direction: speckle, which the resolve smoothed only while the light held still. This
// takes it out within the frame, so the direct light needs no history -- and keeps none, so it never lags
// the sun. Texels with no penumbra (a hard edge, a shadow's interior, open light) pass through; a penumbra's
// taps still read them (their visibility is exact).
//
// Inputs: 0 sunRaw (rgb the unshadowed sun light, a the visibility), 1 sunPenumbra (r the half-width,
// render texels); 2 gPos, 3 gNormal (resources).
// Output: sunLight -- rgb the sun's light leaving the surface (unshadowed x visibility), a 0.

#include <bgfx_shader.sh>
#include <pjv_random.sc>     // pjvInterleavedGradientNoise
#include "restir_common.sc"

SAMPLER2D(sunRaw, 0);
SAMPLER2D(sunPenumbra, 1);
SAMPLER2D(gPos, 2);
SAMPLER2D(gNormal, 3);

#define RESTIR_SHADOW_BLUR      0.6    // of the penumbra's half-width
#define RESTIR_SHADOW_BLUR_MAX  8.0    // render texels
#define RESTIR_SHADOW_BLUR_TAPS 16

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 centre = texelFetch(sunRaw, p, 0);
    float penumbra = texelFetch(sunPenumbra, p, 0).x;
    float radius = min(penumbra * RESTIR_SHADOW_BLUR, RESTIR_SHADOW_BLUR_MAX);
    if (radius < 1.0) { gl_FragColor = vec4(centre.rgb * centre.a, 0.0); return; }
    vec4 surface = texelFetch(gPos, p, 0);
    vec3 P = surface.xyz;
    vec3 N = normalize(texelFetch(gNormal, p, 0).xyz + vec3(0.0, 1e-6, 0.0));
    float texel = max(restirSettings.w * surface.w, 1.0e-4);
    float plane = restirSettings.y * 20.0 * 1.5 + texel;
    ivec2 last = ivec2(passTargetRes.xy) - ivec2(1, 1);
    float turn = 6.2831853 * fract(pjvInterleavedGradientNoise(vec2(p) + vec2(17.0, 31.0)) + frameCount.x * 0.618034);

    float sum = centre.a;
    float weight = 1.0;
    for (int i = 0; i < RESTIR_SHADOW_BLUR_TAPS; i++) {
        float u = (float(i) + 0.5) / float(RESTIR_SHADOW_BLUR_TAPS);
        float a = float(i) * 2.39996323 + turn;
        ivec2 q = clamp(p + ivec2(round(vec2(cos(a), sin(a)) * (sqrt(u) * radius))), ivec2(0, 0), last);
        vec4 qs = texelFetch(gPos, q, 0);
        if (qs.w < 0.0 || abs(dot(qs.xyz - P, N)) > plane || dot(texelFetch(gNormal, q, 0).xyz, N) < 0.9) continue;
        float w = 1.0 - 0.75 * sqrt(u);   // a tent: the near taps count most
        sum += texelFetch(sunRaw, q, 0).a * w;
        weight += w;
    }
    gl_FragColor = vec4(centre.rgb * (sum / weight), 0.0);
}
