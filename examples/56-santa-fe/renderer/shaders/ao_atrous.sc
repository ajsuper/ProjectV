// =============================================================================
// ao_atrous.sc -- the traced AO's spatial denoise, an A-Trous step at its own resolution (santa-fe-reference
// S22d). ao_atrous1/2/4.frag define SF_ATROUS_STEP and include this.
//
// The traced AO is the large-scale term (under a crown, behind a boulder): its fixed rays (ao_trace.frag) are
// filtered broadly here, a 5 x 5 B3-spline kernel SF_ATROUS_STEP texels apart, held to the surface (depth and
// normal); three steps reach 14 of its texels (56 window pixels). resolve.frag then holds it per voxel face.
// sfUpscale2.z off: as traced.
//
// Inputs: the last step's output 0 (r the AO, g its depth); the trace pass 1-8 (gPos 2, gNormal 3).
// Output: the same as the input.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>

SAMPLER2D(aoIn, 0);
SAMPLER2D(gPos, 2);
SAMPLER2D(gNormal, 3);

uniform vec4 sfUpscale2;   // z: the AO's A-Trous filter on

vec3 sfAoNormal(ivec2 q) {
    vec2 gsize = vec2(textureSize(gNormal, 0));
    ivec2 g = clamp(ivec2((vec2(q) + 0.5) * gsize / passTargetRes.xy), ivec2(0, 0), ivec2(gsize) - 1);
    return texelFetch(gNormal, g, 0).xyz;
}

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec4 c = texelFetch(aoIn, at, 0);
    if (sfUpscale2.z < 0.5 || c.g < 0.0) { gl_FragColor = c; return; }
    ivec2 size = ivec2(passTargetRes.xy);
    vec3 n0 = sfAoNormal(at);
    float sum = 0.0, wsum = 0.0;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            float kx = x == 0 ? 0.375 : (abs(x) == 1 ? 0.25 : 0.0625);
            float ky = y == 0 ? 0.375 : (abs(y) == 1 ? 0.25 : 0.0625);
            float w = kx * ky;
            ivec2 q = clamp(at + ivec2(x, y) * SF_ATROUS_STEP, ivec2(0, 0), size - 1);
            vec4 s = texelFetch(aoIn, q, 0);
            if (x != 0 || y != 0) {
                if (s.g < 0.0) continue;
                float rel = abs(s.g - c.g) / max(c.g * 0.03, 0.05);
                float nn = max(dot(sfAoNormal(q), n0), 0.0);
                w *= nn * nn * exp(-rel * rel);
            }
            sum += s.r * w;
            wsum += w;
        }
    gl_FragColor = vec4(sum / max(wsum, 1e-6), c.g, 0.0, 1.0);
}
