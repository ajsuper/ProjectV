$input v_color0
$input v_texcoord0

// ao_apply.frag -- pass 12, render resolution: the lit frame, for the reconstruction. The SSAO is
// averaged over the 4x4 texels round this one -- its pattern's period -- counting only those at about
// this one's distance, so it does not bleed across silhouettes; then it darkens the indirect light, by
// restirLook.y of the way.
//
// The sun's light joins here, its shadow softened (shadow_soft.frag), and last the distance fog
// (restirFog), so the reconstruction and the resolve carry it like any other light.
//
// The views (G, restirDenoise.z): 0 the image, 1 the indirect light alone, 2 the direct alone (the sun
// included), 3 the ambient occlusion alone. Only the image is fogged.
//
// Inputs: 0 litDirect, 1 litIndirect (composite), 2 ao (g: the distance, < 0 sky), 3 sunLight.
// Output: litHDR.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(litDirect, 0);
SAMPLER2D(litIndirect, 1);
SAMPLER2D(ao, 2);
SAMPLER2D(sunLight, 3);

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
    if (restirDenoise.z > 0.5 && restirDenoise.z < 1.5) colour = indirect * shade;
    else if (restirDenoise.z > 1.5 && restirDenoise.z < 2.5) colour = direct;
    else if (restirDenoise.z > 2.5) colour = vec3(shade, shade, shade);
    gl_FragColor = vec4(colour, 1.0);
}
