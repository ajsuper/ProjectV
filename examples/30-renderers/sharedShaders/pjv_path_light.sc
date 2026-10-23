// =============================================================================
// pjv_path_light.sc  --  The light that pt-reference and restir-pt both render by.
//
// pjv_sun_sky.sc's rig, with two changes the path tracers need to agree on exactly, since one is the
// reference the other is measured against:
//
//   * the sky at PATH_SKY_LIGHT of the shared one. That sky's zenith is about four times the voxel
//     renderer's, and lighting by it floods every surface with blue-white (restir found this first).
//   * the sun as a real disc, PATH_SUN_HALF_ANGLE across each way, whose radiance integrates to the rig's
//     irradiance (SUN_COLOR). pjv_sun_sky.sc's 0.3-radian disc is a display convenience; a glossy floor
//     would mirror it as a blob a sixth of the sky across. 0.02 is about four times the real sun's, the
//     soft shadow restir-pt draws (its host's RESTIR_SUN_HALF_ANGLE).
// =============================================================================

#ifndef PJV_PATH_LIGHT_SC
#define PJV_PATH_LIGHT_SC

#include <pjv_sun_sky.sc>
#include <pjv_bsdf.sc>   // pjvBasisAround

#define PATH_SKY_LIGHT 0.35
#define PATH_SUN_HALF_ANGLE 0.02
#define PATH_PI 3.14159265359

float pathSunSolidAngle() { return 2.0 * PATH_PI * (1.0 - cos(PATH_SUN_HALF_ANGLE)); }

// The disc's radiance: SUN_COLOR is its irradiance at normal incidence.
vec3 pathSunRadiance() { return SUN_COLOR / pathSunSolidAngle(); }

bool pathInSun(vec3 d) { return dot(d, normalize(SUN_DIR)) >= cos(PATH_SUN_HALF_ANGLE); }

// The sky along d, without the sun's disc: what a bounce that escapes gathers (the disc is counted by
// next-event estimation, and MIS-weighted when a bounce finds it).
vec3 pathSky(vec3 d) { return skyGradient(d) * PATH_SKY_LIGHT; }

// What the camera sees along d: the sky, or the disc.
vec3 pathSkyWithSun(vec3 d) { return pathInSun(d) ? pathSunRadiance() : pathSky(d); }

// A direction inside the sun's disc, uniformly over its solid angle (pdf 1 / pathSunSolidAngle()).
vec3 pathSampleSun(float u1, float u2) {
    float cosTheta = mix(cos(PATH_SUN_HALF_ANGLE), 1.0, u1);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 2.0 * PATH_PI * u2;
    return pjvBasisAround(normalize(SUN_DIR)) * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

#endif
