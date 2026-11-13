$input v_color0
$input v_texcoord0

// reflect_spatial.frag -- render resolution, after reflect.frag: the glossy reflection's one ray a texel,
// shared with the neighbours whose rays the lobe could have sent. (Stochastic screen-space reflections'
// resolve, Stachowiak 2015, without the BRDF-ratio weights: a lobe's rays are alike enough across one
// surface that an even average is close, and it is what keeps a grazing glossy floor from sparkling.)
//
// The footprint is what the lobe spreads a reflection over, seen from here: a reflected point the ray's
// length away moves about length * alpha across the lobe, so neighbours within that distance on the surface
// saw the same blur of the world. In render texels: length * alpha / (t * texel width per unit distance),
// up to RESTIR_REFLECT_RADIUS. A sharp lobe or a near reflection keeps a small one and stays sharp; a rough
// lobe reflecting the far distance (the sky) takes the widest.
//
// RESTIR_REFLECT_TAPS taps on a Vogel disc, turned per pixel and per frame (reflect_temporal.frag averages
// the turns), each counted when it is the same surface -- face normal, plane, glossiness within 0.1 -- and
// weighted 1 / (1 + luminance) (Karis): a lone bright ray (the sun's disc through a rough lobe) is spread,
// not stamped as a blotch. Mirrors and water pass through: their one ray is exact.
//
// Inputs: 0 reflection, 1 reflectHit (reflect.frag); 2 gPos, 3 gNormal, 4 gDirect (resources).
// Output: reflectSpatial -- as reflection (rgb the light times the specular albedo, a how much it replaces).

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(reflection, 0);
SAMPLER2D(reflectHit, 1);
SAMPLER2D(gPos, 2);
SAMPLER2D(gNormal, 3);
SAMPLER2D(gDirect, 4);

#define RESTIR_REFLECT_TAPS   12
#define RESTIR_REFLECT_RADIUS 24.0   // render texels, at most

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 centre = texelFetch(reflection, p, 0);
    float packed = texelFetch(gDirect, p, 0).a;
    vec2 material = restirUnpackMaterial(packed);
    vec4 surface = texelFetch(gPos, p, 0);
    if (centre.a <= 0.0 || surface.w < 0.0 || restirIsWater(packed) || material.x >= RESTIR_MIRROR_GLOSSINESS) {
        gl_FragColor = centre;
        return;
    }
    vec3 x = surface.xyz;
    vec3 n = normalize(texelFetch(gNormal, p, 0).xyz + vec3(0.0, 1e-6, 0.0));
    float t = surface.w;
    float alpha = pjvBsdfAlpha(material.x);
    float reach = texelFetch(reflectHit, p, 0).x * alpha / max(t * restirSettings.w, 1e-6);
    float radius = clamp(reach, 0.0, RESTIR_REFLECT_RADIUS);
    float plane = 0.004 * t + 20.0 * restirSettings.y + 2.0 * t * restirSettings.w;

    float wc = 1.0 / (1.0 + restirLum(centre.rgb));
    vec3 sum = centre.rgb * wc;
    float total = wc;
    if (radius >= 1.0) {
        ivec2 size = ivec2(passTargetRes.xy);
        float turn = 6.2831853 * fract(pjvInterleavedGradientNoise(vec2(p)) + frameCount.x * 0.618034);
        for (int i = 0; i < RESTIR_REFLECT_TAPS; i++) {
            // Vogel's disc: even coverage for any count, the golden angle apart.
            float r = radius * sqrt((float(i) + 0.5) / float(RESTIR_REFLECT_TAPS));
            float a = float(i) * 2.3999632 + turn;
            ivec2 q = p + ivec2(round(vec2(cos(a), sin(a)) * r));
            if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) continue;
            vec4 qs = texelFetch(gPos, q, 0);
            vec4 qc = texelFetch(reflection, q, 0);
            if (qs.w < 0.0 || qc.a <= 0.0) continue;
            vec3 qn = texelFetch(gNormal, q, 0).xyz;
            if (dot(qn, n) < 0.9 || abs(dot(qs.xyz - x, n)) > plane) continue;
            if (abs(restirUnpackMaterial(texelFetch(gDirect, q, 0).a).x - material.x) > 0.1) continue;
            float w = 1.0 / (1.0 + restirLum(qc.rgb));
            sum += qc.rgb * w;
            total += w;
        }
    }
    gl_FragColor = vec4(sum / total, centre.a);
}
