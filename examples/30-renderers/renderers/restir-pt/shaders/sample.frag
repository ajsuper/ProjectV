$input v_color0
$input v_texcoord0

// sample.frag -- pass 2, GI resolution: one ReSTIR GI candidate per pixel.
//
// From the visible point x1 (a full-res G-buffer pixel of this GI pixel's block), a cosine-weighted
// bounce to x2, and x2's outgoing light Lo: its emission, the sun at x2 (shadow ray), and one more
// cosine bounce to x3 with x3's emission, sky and sun -- two bounces of light, through one restirTrace
// call site (four rays at most). The candidate is a reservoir of one: M = 1, W = 1 / pdf in the
// target's units = pi / cos.
//
// Inputs: 0 gPos, 1 gNormal, 2 gAlbedo, 3 gDirect. Outputs: cand0..cand3 (restir_common.sc's layout).

#include <bgfx_shader.sh>
#include "restir_trace.sc"

SAMPLER2D(gPos, 0);
SAMPLER2D(gNormal, 1);

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 q = restirFullPixel(p, ivec2(textureSize(gPos, 0)));
    vec4 surface = texelFetch(gPos, q, 0);
    Reservoir r = restirEmpty();
    if (surface.w < 0.0) { restirWrite(r); return; }
    vec3 x1 = surface.xyz;
    vec3 n1 = normalize(texelFetch(gNormal, q, 0).xyz);
    r.x1 = x1;
    r.n1 = n1;
    r.t1 = surface.w;

    uint seed = uint(p.x) * 7919u + uint(p.y) * 104729u + uint(frameCount.x) * 15485863u;
    restirPcg(seed);
    vec3 w1 = restirCosine(n1, seed);

    // The walk: 0 bounce to x2, 1 sun at x2, 2 bounce to x3, 3 sun at x3.
    vec3 origin = x1 + n1 * restirSettings.y;
    vec3 dir = w1;
    int kind = RESTIR_KIND_BOUNCE;
    int stage = 0;
    vec3 Lo = vec3(0.0, 0.0, 0.0);
    vec3 albedo2 = vec3(0.0, 0.0, 0.0);
    vec3 x2 = x1 + w1 * 1.0e4;
    vec3 n2 = -w1;
    vec3 surface2 = x1;
    vec3 next = w1;
    vec3 pending = vec3(0.0, 0.0, 0.0);
    int rays = min(int(restirSettings.x), 4);
    for (int i = 0; i < rays; i++) {
        RestirHit hit = restirTrace(origin, dir, kind, 1e30);
        if (stage == 0) {
            if (!hit.hit) { Lo = restirSky(dir); break; }
            n2 = dot(hit.normal, hit.normal) > 0.5 ? normalize(hit.normal) : -dir;
            x2 = origin + dir * hit.t;
            albedo2 = hit.albedo;
            Lo = hit.emission;
            surface2 = x2 + n2 * restirSettings.y;
            next = restirCosine(n2, seed);
            vec3 toSun = restirToSun(seed);
            float facing = dot(n2, toSun);
            if (facing > 0.0) {
                pending = albedo2 * (SUN_COLOR / RESTIR_PI) * facing;
                origin = surface2; dir = toSun; kind = RESTIR_KIND_SHADOW_INDIRECT; stage = 1;
            } else {
                origin = surface2; dir = next; kind = RESTIR_KIND_BOUNCE; stage = 2;
            }
        } else if (stage == 1) {
            if (!hit.hit) Lo += pending;
            origin = surface2; dir = next; kind = RESTIR_KIND_BOUNCE; stage = 2;
        } else if (stage == 2) {
            if (!hit.hit) { Lo += albedo2 * restirSky(dir); break; }
            vec3 n3 = dot(hit.normal, hit.normal) > 0.5 ? normalize(hit.normal) : -dir;
            vec3 x3 = origin + dir * hit.t;
            Lo += albedo2 * hit.emission;
            vec3 toSun = restirToSun(seed);
            float facing = dot(n3, toSun);
            if (facing <= 0.0) break;
            pending = albedo2 * hit.albedo * (SUN_COLOR / RESTIR_PI) * facing;
            origin = x3 + n3 * restirSettings.y; dir = toSun; kind = RESTIR_KIND_SHADOW_INDIRECT; stage = 3;
        } else {
            if (!hit.hit) Lo += pending;
            break;
        }
    }

    r.x2 = x2;
    r.n2 = n2;
    r.Lo = Lo;
    r.M = 1.0;
    float cosine = max(dot(n1, w1), 1e-4);
    r.W = restirTarget(r, x1, n1) > 0.0 ? RESTIR_PI / cosine : 0.0;
    restirWrite(r);
}
