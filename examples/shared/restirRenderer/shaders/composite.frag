$input v_color0
$input v_texcoord0

// composite.frag -- pass 11, render resolution: the two halves of the image. The direct light from the
// G-buffer, and the albedo times the denoised indirect light, upsampled edge-aware: the 2x2 GI pixels
// round this one, bilinear weights times "is that GI pixel's visible point on my surface" (normal and
// plane). A texel no neighbour matches (a thin edge) widens to the 4x4 round it, then falls back to the
// nearest. They are kept apart for ao_apply, which darkens only the indirect.
//
// Inputs: 0 giFinal, 1 giPos, 2 giNrm (the last a-trous level); 3-8 the G-buffer (gPos, gNormal,
// gAlbedo, gDirect, gFace, gRot). Outputs: litDirect, litIndirect.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(giFinal, 0);
SAMPLER2D(accPos, 1);
SAMPLER2D(accNrm, 2);
SAMPLER2D(gPos, 3);
SAMPLER2D(gNormal, 4);
SAMPLER2D(gAlbedo, 5);
SAMPLER2D(gDirect, 6);

void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    vec4 surface = texelFetch(gPos, q, 0);
    vec3 N = texelFetch(gNormal, q, 0).xyz;
    vec3 albedo = texelFetch(gAlbedo, q, 0).rgb;
    vec3 direct = texelFetch(gDirect, q, 0).rgb;
    if (surface.w < 0.0) {
        gl_FragData[0] = vec4(direct, 1.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 P = surface.xyz;
    float t = surface.w;
    ivec2 giSize = ivec2(textureSize(giFinal, 0));
    float stride = restirJitter.z;
    vec2 f = (vec2(q) + 0.5) / stride - 0.5;
    ivec2 base = ivec2(floor(f));
    vec2 frac = f - floor(f);

    vec3 sum = vec3(0.0, 0.0, 0.0);
    float total = 0.0;
    for (int k = 0; k < 4; k++) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 g = clamp(base + o, ivec2(0, 0), giSize - ivec2(1, 1));
        vec4 gs = texelFetch(accPos, g, 0);
        if (gs.w < 0.0 || !restirSameSurface(gs.xyz, texelFetch(accNrm, g, 0).xyz, P, N, t)) continue;
        float w = (o.x == 1 ? frac.x : 1.0 - frac.x) * (o.y == 1 ? frac.y : 1.0 - frac.y) + 1e-3;
        sum += texelFetch(giFinal, g, 0).rgb * w;
        total += w;
    }
    if (total <= 0.0) {
        for (int y = -1; y <= 2; y++)
            for (int x = -1; x <= 2; x++) {
                ivec2 g = clamp(base + ivec2(x, y), ivec2(0, 0), giSize - ivec2(1, 1));
                vec4 gs = texelFetch(accPos, g, 0);
                if (gs.w < 0.0 || !restirSameSurface(gs.xyz, texelFetch(accNrm, g, 0).xyz, P, N, t)) continue;
                sum += texelFetch(giFinal, g, 0).rgb;
                total += 1.0;
            }
    }
    vec3 indirect = total > 0.0 ? sum / total
                                : texelFetch(giFinal, clamp(ivec2(round(f)), ivec2(0, 0), giSize - ivec2(1, 1)), 0).rgb;

    gl_FragData[0] = vec4(direct, 1.0);
    gl_FragData[1] = vec4(albedo * indirect, 1.0);
}
