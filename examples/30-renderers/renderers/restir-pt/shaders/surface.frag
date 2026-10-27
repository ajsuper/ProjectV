$input v_color0
$input v_texcoord0

// surface.frag -- pass 2, GI resolution: each GI pixel's visible point, compacted. One render texel of the
// pixel's block (a different one each frame: restirFullPixel), its position, normal and material packed
// into two texels, so the path passes after it read two inputs instead of the G-buffer's six -- a tracing
// pass has nine input slots, and a path reservoir takes five.
//
// The normal is the blurred one (normal_blur.frag): the paths shade with it, as the sun does.
//
// Inputs: 0-6 the G-buffer, 7 gSoftNormal (resource). Outputs: surf0 (x1, t1; t1 < 0 the sky), surf1 (oct n1, material, albedo).

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(gPos, 0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gAlbedo, 2);
SAMPLER2D(gDirect, 3);
SAMPLER2D(gSoftNormal, 7);

void main() {
    ivec2 q = restirFullPixel(ivec2(gl_FragCoord.xy), ivec2(textureSize(gPos, 0)));
    vec4 surface = texelFetch(gPos, q, 0);
    // Water's waves change every frame; its paths keep the flat face, or no history would match it.
    float packedMaterial = texelFetch(gDirect, q, 0).a;
    // Both fetched and the value chosen: a sampler picked by a ternary compiles, but is not GLSL (samplers
    // are opaque), and it read garbage -- every path seen from below the horizon came back black.
    vec3 hard = texelFetch(gNormal, q, 0).xyz;
    vec3 soft = texelFetch(gSoftNormal, q, 0).xyz;
    vec3 n = normalize((restirIsWater(packedMaterial) ? hard : soft) + vec3(0.0, 1e-6, 0.0));
    gl_FragData[0] = surface;
    // The material as the paths see it: glossiness capped (restir_common.sc).
    vec2 material = restirUnpackMaterial(texelFetch(gDirect, q, 0).a);
    float packed = restirPackMaterial(min(material.x, RESTIR_GI_MAX_GLOSSINESS), material.y);
    gl_FragData[1] = vec4(restirOctEncode(n), packed, restirPackColour(texelFetch(gAlbedo, q, 0).rgb));
}
