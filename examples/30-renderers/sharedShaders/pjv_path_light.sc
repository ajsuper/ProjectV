// =============================================================================
// pjv_path_light.sc  --  The light that pt-reference and restir-pt both render by.
//
// pjv_sun_sky.sc's rig, with two changes the path tracers need to agree on exactly, since one is the
// reference the other is measured against:
//
//   * a sky of its own (pathSky, below): deep zenith, bright horizon band, aureole, sunset, haze below
//     the horizon; the camera also sees clouds (pathSkyView), which do not light the scene.
//   * the sun as a real disc, PATH_SUN_HALF_ANGLE across each way, whose radiance integrates to the rig's
//     irradiance (SUN_COLOR). pjv_sun_sky.sc's 0.3-radian disc is a display convenience; a glossy floor
//     would mirror it as a blob a sixth of the sky across. 0.02 is about four times the real sun's, the
//     soft shadow restir-pt draws (its host's RESTIR_SUN_HALF_ANGLE).
// =============================================================================

#ifndef PJV_PATH_LIGHT_SC
#define PJV_PATH_LIGHT_SC

#include <pjv_sun_sky.sc>
#include <pjv_bsdf.sc>   // pjvBasisAround

#define PATH_SUN_HALF_ANGLE 0.02
#define PATH_PI 3.14159265359

float pathSunSolidAngle() { return 2.0 * PATH_PI * (1.0 - cos(PATH_SUN_HALF_ANGLE)); }

// The disc's radiance: SUN_COLOR is its irradiance at normal incidence.
vec3 pathSunRadiance() { return SUN_COLOR / pathSunSolidAngle(); }

bool pathInSun(vec3 d) { return dot(d, normalize(SUN_DIR)) >= cos(PATH_SUN_HALF_ANGLE); }

// The sky. Its own palette, not pjv_sun_sky.sc's gradient (whose zenith is about four times the voxel
// renderer's: it flooded every surface with blue-white): a deep zenith, a bright band low on the horizon,
// the sunset's warmth gathered towards the sun while it is low, the sun's aureole (forward scattering
// round the disc), and below the horizon the haze carrying on, darkening, rather than a black ground --
// a level's floor ends somewhere, and the camera sees past it.

float pathPhase(float c, float g) {
    float gg = g * g;
    return (1.0 - gg) / (4.0 * PATH_PI * pow(max(1.0 + gg - 2.0 * g * c, 1e-4), 1.5));
}

// The horizon's colour in direction d (only its azimuth counts): the haze the far distance melts into.
vec3 pathHorizon(vec3 d) {
    float e = sunDir.y;
    float day = smoothstep(-0.18, 0.22, e);
    float warm = day * (1.0 - smoothstep(0.0, 0.3, e));
    vec2 hd = normalize(d.xz + vec2(1e-5, 0.0));
    vec2 hs = normalize(sunDir.xz + vec2(1e-5, 0.0));
    float towards = max(dot(hd, hs), 0.0);
    vec3 horizon = mix(vec3(0.020, 0.025, 0.045), vec3(0.62, 0.70, 0.80), day);
    return mix(horizon, vec3(0.95, 0.48, 0.20), warm * (0.3 + 0.7 * towards * towards));
}

// The sky along d, without the sun's disc: what a bounce that escapes gathers (the disc is counted by
// next-event estimation, and MIS-weighted when a bounce finds it).
vec3 pathSky(vec3 d) {
    float day = smoothstep(-0.18, 0.22, sunDir.y);
    vec3 zenith = mix(vec3(0.010, 0.014, 0.030), vec3(0.16, 0.34, 0.80), day);
    vec3 horizon = pathHorizon(d);
    float band = pow(1.0 - clamp(d.y, 0.0, 1.0), 5.0);   // the horizon's brightness, low and narrow
    vec3 sky = mix(zenith, horizon, band);
    if (d.y < 0.0) sky = mix(horizon, horizon * 0.35, smoothstep(0.0, 0.4, -d.y));
    // The aureole: the haze scattering sunlight forwards, brightest round the disc, strongest low down.
    sky += SUN_COLOR * (0.012 * pathPhase(dot(d, normalize(SUN_DIR)), 0.8) * (0.4 + 0.6 * band));
    return sky;
}

// Value noise and its fractal sum, for the clouds.
float pathHash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float pathNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(pathHash(i), pathHash(i + vec2(1.0, 0.0)), f.x),
               mix(pathHash(i + vec2(0.0, 1.0)), pathHash(i + vec2(1.0, 1.0)), f.x), f.y);
}
float pathFbm(vec2 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 5; i++) {
        sum += amplitude * pathNoise(p);
        p = vec2(p.x * 1.6 - p.y * 1.2, p.x * 1.2 + p.y * 1.6) + vec2(3.1, 1.7);
        amplitude *= 0.5;
    }
    return sum;
}

// Clouds over the sky along d: a layer of fractal noise on a plane, flattened towards the horizon and
// fading into it, lit by the sun (a silver lining towards it, greyer where thick) and the sky. For the
// camera only: lighting by them would add noise to the GI for little light.
vec3 pathClouds(vec3 d, vec3 sky, out float cover) {
    cover = 0.0;
    if (d.y <= 0.0) return sky;
    vec2 p = d.xz / (d.y + 0.08) * 1.4;
    float n = pathFbm(p);
    cover = smoothstep(0.50, 0.78, n) * smoothstep(0.02, 0.25, d.y);
    float thick = smoothstep(0.55, 0.95, n);
    float c = dot(d, normalize(SUN_DIR));
    vec3 ambient = mix(pathHorizon(d), sky, 0.5);
    vec3 lit = ambient * 0.9 + SUN_COLOR * (0.07 * (1.0 - 0.6 * thick) + 0.05 * pow(max(c, 0.0), 8.0) * (1.0 - thick));
    return mix(sky, lit, 0.9 * cover);
}

// What the camera sees along d: the sky, its clouds, and the disc (hidden by a thick cloud).
vec3 pathSkyView(vec3 d) {
    float cover;
    vec3 sky = pathClouds(d, pathSky(d), cover);
    return pathInSun(d) ? mix(pathSunRadiance(), sky, cover) : sky;
}

// What the camera sees along d, the disc included but no clouds (what the paths see).
vec3 pathSkyWithSun(vec3 d) { return pathInSun(d) ? pathSunRadiance() : pathSky(d); }

// The air's colour looking along d: the horizon's haze in that direction and the aureole -- what distance
// fog fades towards, so the far distance melts into the sky it stands against.
vec3 pathAir(vec3 d) { return pathSky(normalize(vec3(d.x, 0.03, d.z))); }

// A direction inside the sun's disc, uniformly over its solid angle (pdf 1 / pathSunSolidAngle()).
vec3 pathSampleSun(float u1, float u2) {
    float cosTheta = mix(cos(PATH_SUN_HALF_ANGLE), 1.0, u1);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 2.0 * PATH_PI * u2;
    return pjvBasisAround(normalize(SUN_DIR)) * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

#endif
