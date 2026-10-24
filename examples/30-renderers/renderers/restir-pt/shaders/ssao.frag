$input v_color0
$input v_texcoord0

// ssao.frag -- pass 10, render resolution: screen-space ambient obscurance, for the contact shading the
// paths lose. They are traced for one pixel in sixteen and blurred over dozens, so the light they bring is
// broad: a corner, a crease, the floor under a block are not darker than the wall beside them, and objects
// look set down rather than standing. This puts that small-scale shading back, on the indirect light only
// (ao_apply; the sun's shadows are traced, and exact).
//
// Alchemy obscurance (McGuire et al. 2011), from the G-buffer's world positions: points on a spiral round
// the texel, each counting by how far it rises above the texel's plane -- max(0, n . v / |v| - bias) --
// fading out by its distance. A flat floor counts nothing (no self-occlusion, the old depth test's grey),
// a wall meeting it counts about half. Two scales: contact (restirLook.x / 2: about three voxels) and the
// surroundings (2 restirLook.x), multiplied. The spiral turns per texel and per frame; ao_apply's 4x4
// blur and the resolve's history average it out.
//
// Inputs: 0-5 the G-buffer (gPos, gNormal, ...). Output: ao (r the unoccluded share, g distance; < 0 sky).

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(gPos, 0);
SAMPLER2D(gNormal, 1);

#define RESTIR_AO_SAMPLES 8     // a scale
#define RESTIR_AO_BIAS 0.1      // n . v / |v| below this counts nothing: a slightly curved or noisy plane
#define RESTIR_AO_STRENGTH 1.6
#define RESTIR_AO_MAX_PIXELS 48.0
// The far scale: a radius of this many texels on screen, wherever that is wider than the surroundings'
// scale -- far away, where two voxels are under a texel and the world-space scales see nothing. It gives
// distant terrain and canopy the creases and contact shading the near field gets from the others.
#define RESTIR_AO_FAR_PIXELS 24.0

// The obscurance from points within `radius` (world units) of P, out to `pixels` texels on screen.
float restirObscurance(vec3 P, vec3 N, float radius, float pixels, float turn, ivec2 p) {
    ivec2 last = ivec2(passTargetRes.xy) - ivec2(1, 1);
    float sum = 0.0;
    for (int i = 0; i < RESTIR_AO_SAMPLES; i++) {
        float u = (float(i) + 0.5) / float(RESTIR_AO_SAMPLES);
        float a = turn + float(i) * 2.39996323;            // the golden angle
        ivec2 q = clamp(p + ivec2(round(vec2(cos(a), sin(a)) * (u * pixels))), ivec2(0, 0), last);
        vec4 there = texelFetch(gPos, q, 0);
        if (there.w < 0.0) continue;
        vec3 v = there.xyz - P;
        float d2 = dot(v, v);
        float fade = max(1.0 - d2 / (radius * radius), 0.0);
        sum += max(dot(N, v) * inversesqrt(d2 + 1e-8) - RESTIR_AO_BIAS, 0.0) * fade;
    }
    return clamp(1.0 - RESTIR_AO_STRENGTH * sum / float(RESTIR_AO_SAMPLES), 0.0, 1.0);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 surface = texelFetch(gPos, p, 0);
    if (surface.w < 0.0) { gl_FragColor = vec4(1.0, -1.0, 0.0, 0.0); return; }
    vec3 P = surface.xyz;
    vec3 N = normalize(texelFetch(gNormal, p, 0).xyz);
    // A world unit's width in texels here.
    float perUnit = 1.0 / max(restirSettings.w * surface.w, 1e-6);
    float turn = 6.2831853 * fract(52.9829189 * fract(dot(vec2(p), vec2(0.06711056, 0.00583715))) + frameCount.x * 0.618034);
    float contact = 0.5 * restirLook.x;
    float around = 2.0 * restirLook.x;
    float open = restirObscurance(P, N, contact, min(contact * perUnit, RESTIR_AO_MAX_PIXELS), turn, p) *
                 restirObscurance(P, N, around, min(around * perUnit, RESTIR_AO_MAX_PIXELS), turn + 1.0, p);
    // Fading in from where it is twice the surroundings' radius, so near surfaces are as they were.
    float far = RESTIR_AO_FAR_PIXELS / perUnit;
    float farShare = smoothstep(2.0 * around, 4.0 * around, far);
    if (restirLook.w > 0.5 && farShare > 0.0) open *= mix(1.0, restirObscurance(P, N, far, RESTIR_AO_FAR_PIXELS, turn + 2.0, p), farShare);
    gl_FragColor = vec4(open, surface.w, 0.0, 0.0);
}
