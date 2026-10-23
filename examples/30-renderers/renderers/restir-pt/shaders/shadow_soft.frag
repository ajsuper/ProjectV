$input v_color0
$input v_texcoord0

// shadow_soft.frag -- render resolution, after the SSAO: the sun's light through the surface's BSDF
// (pjv_bsdf.sc: the diffuse lobe and the specular highlight), with a soft-edged shadow.
//
// The sun is a disc (PATH_SUN_HALF_ANGLE), and a glossy surface mirrors the whole of it: its lobe is
// widened by the disc's half-width before it is evaluated at the disc's centre, so a mirror shows a sun
// the disc's size rather than a point, and a rough surface is unchanged.
//
// The G-buffer traces one shadow ray at the sun's centre: a hard edge, noise-free. A real sun is a disc,
// so its shadow's edge is a penumbra as wide as the blocker's distance times the disc's angle
// (restirLook.z, the width per unit of distance). Here, as in PCSS, in screen space:
//   1. look for shadowed texels of this surface within reach; their blockers' mean distance sizes the
//      penumbra (none found: nothing to soften -- a lit texel far from a shadow, or a shadow's interior);
//   2. average the sun's visibility over a disc of that width, on this surface only (plane and normal),
//      so a shadow does not soften across a step onto another face.
// The taps turn a little each frame; the resolve's accumulation smooths what is left of the pattern.
//
// Output: rgb the sun's light leaving the surface towards the camera (BSDF x sun x cosine x visibility), a 0.
// Inputs: 0-5 the G-buffer (gPos 0, gNormal 1 -- w the blocker distance, gAlbedo 2 -- a the visibility,
// gDirect 3 -- a the material).

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(gPos, 0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gAlbedo, 2);
SAMPLER2D(gDirect, 3);

#define SOFT_SEARCH_TAPS 16
#define SOFT_FILTER_TAPS 24
#define SOFT_MAX_RADIUS 16.0   // render texels: the widest half-penumbra, and how far step 1 looks

// The i-th of n points of a golden-angle spiral filling the unit disc, turned by `turn`.
vec2 softDisc(int i, int n, float turn) {
    float r = sqrt((float(i) + 0.5) / float(n));
    float a = float(i) * 2.39996323 + turn;
    return vec2(cos(a), sin(a)) * r;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 surface = texelFetch(gPos, p, 0);
    if (surface.w < 0.0) { gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0); return; }
    vec4 nb = texelFetch(gNormal, p, 0);
    vec4 ab = texelFetch(gAlbedo, p, 0);
    vec3 P = surface.xyz;
    vec3 N = normalize(nb.xyz);
    vec3 toSun = normalize(SUN_DIR);
    float facing = max(dot(N, toSun), 0.0);
    vec2 material = restirUnpackMaterial(texelFetch(gDirect, p, 0).a);
    PjvBsdf bsdf = pjvBsdf(ab.rgb, material.x, material.y);
    bsdf.alpha = sqrt(bsdf.alpha * bsdf.alpha + 0.25 * PATH_SUN_HALF_ANGLE * PATH_SUN_HALF_ANGLE);
    vec3 f;
    float pdf;
    pjvBsdfEvaluate(bsdf, N, normalize(cameraPos.xyz - P), toSun, f, pdf);
    vec3 sunLit = f * SUN_COLOR * facing;
    if (facing <= 0.0 || restirLook.z <= 0.0) { gl_FragColor = vec4(sunLit * ab.a, 0.0); return; }

    ivec2 last = ivec2(passTargetRes.xy) - ivec2(1, 1);
    float texel = max(restirSettings.w * surface.w, 1.0e-4);           // a render texel's width here
    float finest = restirSettings.y * 20.0;                             // the finest voxel's edge
    float plane = finest * 1.5 + texel;                                 // "the same surface", along N
    float turn = 6.2831853 * fract(52.9829189 * fract(dot(vec2(p), vec2(0.06711056, 0.00583715)))
                                   + frameCount.x * 0.618034);

    // 1. The blockers within reach.
    float blockers = ab.a < 0.5 && nb.w > 0.0 ? nb.w : 0.0;
    float found = ab.a < 0.5 && nb.w > 0.0 ? 1.0 : 0.0;
    for (int i = 0; i < SOFT_SEARCH_TAPS; i++) {
        ivec2 q = clamp(p + ivec2(round(softDisc(i, SOFT_SEARCH_TAPS, turn) * SOFT_MAX_RADIUS)), ivec2(0, 0), last);
        vec4 qs = texelFetch(gPos, q, 0);
        vec4 qn = texelFetch(gNormal, q, 0);
        if (qs.w < 0.0 || abs(dot(qs.xyz - P, N)) > plane || dot(qn.xyz, N) < 0.9) continue;
        if (texelFetch(gAlbedo, q, 0).a < 0.5 && qn.w > 0.0) { blockers += qn.w; found += 1.0; }
    }
    float radius = found > 0.0 ? min(0.5 * (blockers / found) * restirLook.z / texel, SOFT_MAX_RADIUS) : 0.0;
    if (radius < 0.5) { gl_FragColor = vec4(sunLit * ab.a, 0.0); return; }

    // 2. The visibility over the penumbra's disc, on this surface.
    float sum = ab.a;
    float weight = 1.0;
    for (int i = 0; i < SOFT_FILTER_TAPS; i++) {
        ivec2 q = clamp(p + ivec2(round(softDisc(i, SOFT_FILTER_TAPS, turn) * radius)), ivec2(0, 0), last);
        vec4 qs = texelFetch(gPos, q, 0);
        if (qs.w < 0.0 || abs(dot(qs.xyz - P, N)) > plane || dot(texelFetch(gNormal, q, 0).xyz, N) < 0.9) continue;
        sum += texelFetch(gAlbedo, q, 0).a;
        weight += 1.0;
    }
    gl_FragColor = vec4(sunLit * (sum / weight), 0.0);
}
