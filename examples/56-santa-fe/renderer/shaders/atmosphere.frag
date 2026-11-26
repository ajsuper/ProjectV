$input v_color0
$input v_texcoord0

// =============================================================================
// atmosphere.frag -- the atmosphere's tables (santa-fe-reference S29b; santa_fe_atmosphere.sc says what each
// holds and how it is mapped). One atlas, every frame, each region from the last frame's others: the
// transmittance (a fixed 40-step march to the top), the multiple scattering (Hillaire's 64 directions of 20
// steps), the sky view from the camera (30 steps) and the aerial-perspective froxels (to their depth, 1-24
// steps). The tables settle in three frames after the air changes; the light in them is per unit sun.
//
// Input: this pass's own last output (0). Output: the atlas.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>
#include <pjv_camera.sc>

uniform vec4 sunDir;
uniform vec4 mtnAtmos;   // x air density, y haze, z haze g, w multiple scattering's weight
#define SF_ATMO_SLOT 0
#include <santa_fe_atmosphere.sc>

#define SF_GROUND_ALBEDO vec3(0.3, 0.3, 0.3)

// The transmittance from (r, mu) to the top: no table read, a plain march.
vec3 transmittanceMarch(float r, float mu) {
    vec3 p = vec3(0.0, r, 0.0);
    vec3 d = vec3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0);
    float tTop = sfRaySphere(p, d, SF_RT);
    if (tTop <= 0.0) return vec3(1.0, 1.0, 1.0);
    vec3 tau = vec3(0.0, 0.0, 0.0);
    float dt = tTop / 40.0;
    for (int i = 0; i < 40; i++) {
        vec3 q = p + d * ((float(i) + 0.5) * dt);
        tau += sfMedium(length(q) - SF_RG).ext * dt;
    }
    return exp(-tau);
}

// Hillaire's psi at (r, mu_s): the light that reaches a point by scattering twice or more, as a geometric series
// of the second order's, over a uniform phase.
vec3 multiScatter(float r, float muS) {
    vec3 p = vec3(0.0, r, 0.0);
    vec3 sun = vec3(sqrt(max(1.0 - muS * muS, 0.0)), muS, 0.0);
    vec3 L2 = vec3(0.0, 0.0, 0.0), fms = vec3(0.0, 0.0, 0.0);
    for (int k = 0; k < 64; k++) {
        // 8 x 8 directions over the sphere, uniform in area.
        float u = (float(k & 7) + 0.5) / 8.0, v = (float(k >> 3) + 0.5) / 8.0;
        float cz = 1.0 - 2.0 * v, sz = sqrt(max(1.0 - cz * cz, 0.0)), ph = 6.2831853 * u;
        vec3 d = vec3(sz * cos(ph), cz, sz * sin(ph));
        float tG = sfRaySphere(p, d, SF_RG);
        float tT = sfRaySphere(p, d, SF_RT);
        float tMax = tG > 0.0 ? tG : tT;
        if (tMax <= 0.0) continue;
        vec3 T = vec3(1.0, 1.0, 1.0);
        float dt = tMax / 20.0;
        for (int i = 0; i < 20; i++) {
            vec3 q = p + d * ((float(i) + 0.5) * dt);
            float rq = length(q);
            SfMedium m = sfMedium(rq - SF_RG);
            vec3 scat = m.scatR + vec3(m.scatM, m.scatM, m.scatM);
            vec3 sunT = sfTransmittanceToTop(rq, dot(q / rq, sun));
            vec3 Ts = exp(-m.ext * dt);
            vec3 inv = 1.0 / max(m.ext, vec3(1e-12, 1e-12, 1e-12));
            L2 += T * (scat * sunT * 0.0795775 - scat * sunT * 0.0795775 * Ts) * inv;   // uniform phase 1/4pi
            fms += T * (scat - scat * Ts) * inv;
            T *= Ts;
        }
        if (tG > 0.0) {   // the ground, lit, Lambertian
            vec3 g = p + d * tG;
            vec3 n = normalize(g);
            float mu = dot(n, sun);
            L2 += T * sfTransmittanceToTop(SF_RG, mu) * max(mu, 0.0) * SF_GROUND_ALBEDO * 0.3183099;
        }
    }
    L2 *= 1.0 / 64.0;   // the mean over the sphere (the 4 pi of the solid angle and the 1/4 pi phase cancel)
    fms *= 1.0 / 64.0;
    return L2 / max(vec3(1.0, 1.0, 1.0) - fms, vec3(1e-3, 1e-3, 1e-3));
}

void main() {
    vec2 px = gl_FragCoord.xy;
    vec3 sun = sunDir.xyz;
    vec4 outC = vec4(0.0, 0.0, 0.0, 1.0);
    if (px.y < SF_TR_H && px.x < SF_TR_W) {
        float r, mu;
        sfTransmittanceRMu(px / vec2(SF_TR_W, SF_TR_H), r, mu);
        outC = vec4(transmittanceMarch(r, mu), 1.0);
    } else if (px.y < SF_MS_N && px.x >= SF_MS_X && px.x < SF_MS_X + SF_MS_N) {
        vec2 uv = (px - vec2(SF_MS_X, 0.0)) / SF_MS_N;
        uv = (uv - 0.5 / SF_MS_N) / (1.0 - 1.0 / SF_MS_N);
        float muS = clamp(uv.x, 0.0, 1.0) * 2.0 - 1.0;
        float r = SF_RG + clamp(uv.y, 0.0, 1.0) * (SF_RT - SF_RG);
        outC = vec4(multiScatter(max(r, SF_RG + 1.0), muS) * mtnAtmos.w, 1.0);
    } else if (px.y >= SF_SV_Y && px.y < SF_SV_Y + SF_SV_H && px.x < SF_SV_W) {
        vec2 uv = (px - vec2(0.0, SF_SV_Y)) / vec2(SF_SV_W, SF_SV_H);
        vec3 P = sfPlanetPos(pjvViewPosition());
        float r = length(P);
        float cosZ, cosL;
        sfSkyViewDir(uv, r, cosZ, cosL);
        // The view in the camera's frame: up is +y; the sun's azimuth is +x.
        vec2 sh = sun.xz;
        float ls = length(sh);
        vec2 sa = ls > 1e-5 ? sh / ls : vec2(1.0, 0.0);
        vec2 perp = vec2(-sa.y, sa.x);
        float sinZ = sqrt(max(1.0 - cosZ * cosZ, 0.0));
        float sinL = sqrt(max(1.0 - cosL * cosL, 0.0));
        vec2 h = (sa * cosL + perp * sinL) * sinZ;
        vec3 d = vec3(h.x, cosZ, h.y);
        vec3 p = vec3(0.0, r, 0.0);
        vec3 sunP = vec3(sun.x, sun.y, sun.z);
        float tG = sfRaySphere(p, d, SF_RG);
        float tT = sfRaySphere(p, d, SF_RT);
        float tMax = tG > 0.0 ? tG : tT;
        vec3 T;
        vec3 L = tMax > 0.0 ? sfIntegrate(p, d, tMax, sunP, 30, true, T) : vec3(0.0, 0.0, 0.0);
        outC = vec4(L, 1.0);
    } else if (px.y >= SF_AP_Y && px.y < SF_AP_Y + SF_AP_N) {
        float slice = floor(px.x / SF_AP_N);
        vec2 uv = vec2(px.x - slice * SF_AP_N, px.y - SF_AP_Y) / SF_AP_N;
        vec3 dir = pjvViewRayDirection(uv, max(windowRes.xy, vec2(1.0, 1.0)));
        float dist = SF_AP_RANGE * pow((slice + 1.0) / SF_AP_N, 2.0);
        vec3 p = sfPlanetPos(pjvViewPosition());
        // Not past the ground (a froxel under the horizon) or the top.
        float tG = sfRaySphere(p, dir, SF_RG);
        if (tG > 0.0) dist = min(dist, tG);
        vec3 T;
        int steps = int(clamp(slice * 0.75 + 1.0, 1.0, 24.0));
        vec3 L = sfIntegrate(p, dir, dist, sun, steps, true, T);
        outC = vec4(L, dot(T, vec3(0.3333, 0.3333, 0.3334)));
    }
    if (any(isnan(outC)) || any(isinf(outC))) outC = vec4(0.0, 0.0, 0.0, 1.0);
    gl_FragColor = outC;
}
