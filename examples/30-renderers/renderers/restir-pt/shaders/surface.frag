$input v_color0
$input v_texcoord0

// surface.frag -- pass 2, GI resolution: each GI pixel's visible point, compacted. One render texel of the
// pixel's block (a different one each frame: restirFullPixel), its position, normal and material packed
// into two texels, so the path passes after it read two inputs instead of the G-buffer's six -- a tracing
// pass has nine input slots, and a path reservoir takes five.
//
// Inputs: 0-5 the G-buffer. Outputs: surf0 (x1, t1; t1 < 0 the sky), surf1 (oct n1, material, albedo).

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(gPos, 0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gAlbedo, 2);
SAMPLER2D(gDirect, 3);

void main() {
    ivec2 q = restirFullPixel(ivec2(gl_FragCoord.xy), ivec2(textureSize(gPos, 0)));
    vec4 surface = texelFetch(gPos, q, 0);
    vec3 n = normalize(texelFetch(gNormal, q, 0).xyz + vec3(0.0, 1e-6, 0.0));
    gl_FragData[0] = surface;
    // The material as the paths see it: glossiness capped (restir_common.sc).
    vec2 material = restirUnpackMaterial(texelFetch(gDirect, q, 0).a);
    float packed = restirPackMaterial(min(material.x, RESTIR_GI_MAX_GLOSSINESS), material.y);
    gl_FragData[1] = vec4(restirOctEncode(n), packed, restirPackColour(texelFetch(gAlbedo, q, 0).rgb));
}
