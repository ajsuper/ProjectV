$input v_color0
$input v_texcoord0

// ao_apply.frag -- pass 12, render resolution: the lit frame, for the reconstruction. The SSAO is
// averaged over the 4x4 texels round this one -- its pattern's period -- counting only those at about
// this one's distance, so it does not bleed across silhouettes; then it darkens the indirect light, by
// restirLook.y of the way.
//
// The sun's light joins here, its shadow softened (shadow_soft.frag), and last the distance fog
// (restirFog) and the light shafts (shafts.frag), so the reconstruction and the resolve carry them like any
// other light.
//
// The views (G, restirDenoise.z): 0 the image, 1 the indirect light alone, 2 the direct alone (the sun
// included), 3 the ambient occlusion alone, 4 and 5 the indirect diffuse and specular alone (the composite
// hands on only that one as the indirect light). Only the image is fogged and has the shafts.
//
// Inputs: 0 litDirect, 1 litIndirect (composite), 2 ao (g: the distance, < 0 sky), 3 sunLight; 4 shafts,
// 5 shaftDepth (resources).
// Output: litHDR.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(litDirect, 0);
SAMPLER2D(litIndirect, 1);
SAMPLER2D(ao, 2);
SAMPLER2D(sunLight, 3);
SAMPLER2D(shafts, 4);
SAMPLER2D(shaftDepth, 5);

// The light shafts (shafts.frag, GI resolution) here: a 4x4 tent of GI pixels round this texel, each
// weighted by how near its view ray's length is to this one's, so a shaft does not bleed across a
// silhouette onto what stands in front of it. Wide on purpose: the shafts are soft, and the tent takes out
// what noise the accumulation left.
vec3 restirShaftsHere(ivec2 p, float t) {
    float stride = restirJitter.z;
    vec2 f = (vec2(p) + 0.5) / stride - 0.5;
    ivec2 base = ivec2(floor(f));
    ivec2 last = ivec2(textureSize(shafts, 0)) - ivec2(1, 1);
    vec3 sum = vec3(0.0, 0.0, 0.0);
    float total = 0.0;
    for (int y = -1; y <= 2; y++)
        for (int x = -1; x <= 2; x++) {
            ivec2 g = clamp(base + ivec2(x, y), ivec2(0, 0), last);
            vec2 d = abs(vec2(base + ivec2(x, y)) - f);
            float w = max(1.0 - d.x * 0.5, 0.0) * max(1.0 - d.y * 0.5, 0.0);
            float tq = texelFetch(shaftDepth, g, 0).x;
            w *= exp(-abs(tq - t) / (0.05 * t + 1e-3));
            sum += texelFetch(shafts, g, 0).rgb * w;
            total += w;
        }
    return total > 1e-6 ? sum / total : vec3(0.0, 0.0, 0.0);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 direct = texelFetch(litDirect, p, 0).rgb + texelFetch(sunLight, p, 0).rgb;
    vec3 indirect = texelFetch(litIndirect, p, 0).rgb;
    float t = texelFetch(ao, p, 0).g;
    float open = 1.0;
    if (t >= 0.0) {
        ivec2 last = ivec2(passTargetRes.xy) - ivec2(1, 1);
        float sum = 0.0;
        float count = 0.0;
        for (int y = -1; y <= 2; y++)
            for (int x = -1; x <= 2; x++) {
                vec2 q = texelFetch(ao, clamp(p + ivec2(x, y), ivec2(0, 0), last), 0).rg;
                if (q.g < 0.0 || abs(q.g - t) > 0.05 * t + 4.0 * restirLook.x) continue;
                sum += q.r;
                count += 1.0;
            }
        open = count > 0.0 ? sum / count : 1.0;
    }
    float shade = mix(1.0, open, restirLook.y);
    vec3 colour = direct + indirect * shade;
    vec2 uv = v_texcoord0 + jitterParams.xy * passTargetRes.zw;   // as the G-buffer traced it (same resolution)
    colour = restirFog(colour, restirViewDirection(uv), t);
    colour += restirShaftsHere(p, t < 0.0 ? 1.0e4 : t);   // all zero when they are off (L, P)
    if (restirView(RESTIR_VIEW_INDIRECT) || restirView(RESTIR_VIEW_DIFFUSE) || restirView(RESTIR_VIEW_SPECULAR)) colour = indirect * shade;
    else if (restirView(RESTIR_VIEW_DIRECT)) colour = direct;
    else if (restirView(RESTIR_VIEW_AO)) colour = vec3(shade, shade, shade);
    gl_FragColor = vec4(colour, 1.0);
}
