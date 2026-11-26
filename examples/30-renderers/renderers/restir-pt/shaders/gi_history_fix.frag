$input v_color0
$input v_texcoord0

// gi_history_fix.frag -- pass 5b, GI resolution, between gi_accumulate.frag and the a-trous: a GI pixel with
// little history -- uncovered by the camera's move, or past an edge it could not reproject across -- has one
// frame or two of paths, which the a-trous would spread into blotches that change every frame. Below
// RESTIR_FIX_FRAMES it is replaced by a disc blur over this same surface (normal, plane; the specular also
// its glossiness), RESTIR_FIX_RADIUS GI pixels across with no history and narrowing as the history grows,
// each tap weighted by its own history length: an uncovered pixel takes the settled light beside it rather
// than its own few paths. (NRD's HistoryFix; docs/plans/restir-gi-confidence.md, D3.) Its variance is kept --
// still high -- so the a-trous goes on blurring it harder until its own history has grown. Pixels with
// RESTIR_FIX_FRAMES of history or more pass through.
//
// Inputs: 0 accDiff, 1 accSpec (rgb, history length), 2 accMom (z, w the variances), 3 accSurf (unused);
// 4 surf0, 5 surf1 (resources). Outputs: fixDiff, fixSpec (rgb, variance) -- the a-trous's first level.

#include <bgfx_shader.sh>
#include <pjv_random.sc>     // pjvInterleavedGradientNoise
#include "restir_common.sc"

SAMPLER2D(accDiff, 0);
SAMPLER2D(accSpec, 1);
SAMPLER2D(accMom, 2);
SAMPLER2D(surf0, 4);
SAMPLER2D(surf1, 5);

#define RESTIR_FIX_FRAMES 4.0
#define RESTIR_FIX_RADIUS 6.0    // GI pixels, with no history at all
#define RESTIR_FIX_TAPS   16

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 d = texelFetch(accDiff, p, 0);
    vec4 sp = texelFetch(accSpec, p, 0);
    vec4 mom = texelFetch(accMom, p, 0);
    RestirSurface s = restirSurface(texelFetch(surf0, p, 0), texelFetch(surf1, p, 0));
    float length0 = d.a;
    if (s.t < 0.0 || length0 >= RESTIR_FIX_FRAMES || restirDenoise.x < 0.5) {
        gl_FragData[0] = vec4(d.rgb, mom.z);
        gl_FragData[1] = vec4(sp.rgb, mom.w);
        return;
    }
    ivec2 size = ivec2(passTargetRes.xy);
    float radius = RESTIR_FIX_RADIUS * (1.0 - max(length0 - 1.0, 0.0) / (RESTIR_FIX_FRAMES - 1.0)) + 1.0;
    float giPixel = s.t * restirSettings.w * restirJitter.z;
    float plane = 0.01 * s.t + 0.05 + 2.0 * giPixel * radius;
    float turn = 6.2831853 * fract(pjvInterleavedGradientNoise(vec2(p)) + frameCount.x * 0.618034);

    float wc = max(length0, 0.5);
    vec3 sumD = d.rgb * wc;
    vec3 sumS = sp.rgb * wc;
    float totalD = wc;
    float totalS = wc;
    for (int i = 0; i < RESTIR_FIX_TAPS; i++) {
        float r = radius * sqrt((float(i) + 0.5) / float(RESTIR_FIX_TAPS));
        float a = float(i) * 2.39996323 + turn;
        ivec2 q = p + ivec2(round(vec2(cos(a), sin(a)) * r));
        if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) continue;
        RestirSurface o = restirSurface(texelFetch(surf0, q, 0), texelFetch(surf1, q, 0));
        if (o.t < 0.0 || dot(o.n, s.n) < 0.9 || abs(dot(o.x - s.x, s.n)) > plane) continue;
        vec4 qd = texelFetch(accDiff, q, 0);
        float w = max(qd.a, 0.5);   // the settled light counts most
        sumD += qd.rgb * w;
        totalD += w;
        if (abs(o.glossiness - s.glossiness) <= 0.1) {
            sumS += texelFetch(accSpec, q, 0).rgb * w;
            totalS += w;
        }
    }
    gl_FragData[0] = vec4(sumD / totalD, mom.z);
    gl_FragData[1] = vec4(sumS / totalS, mom.w);
}
