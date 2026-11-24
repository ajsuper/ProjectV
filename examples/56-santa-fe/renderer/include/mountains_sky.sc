// =============================================================================
// mountains_sky.sc  --  the sky and the air over the mountains (plan D11).
//
// An exponential atmosphere, two layers: air (Rayleigh, 8 km scale height) and haze (Mie, 1.2 km), at
// densities for high, dry air (Santa Fe: ~270 km visual range). World y is metres above sea level, so
// the camera's altitude is its y.
//
// Optical depth along a straight ray through an exponential layer has a closed form, so the
// transmittance and the light scattered into a ray are evaluated per pixel with no lookup tables:
//
//   layer(y0, dy, d, H) = exp(-y0/H) * (1 - exp(-d dy / H)) / (dy / H)      (-> exp(-y0/H) d as dy -> 0)
//
// Single scattering takes the sun's light as constant along the ray (its colour is worked out on the
// CPU from the sun's airmass and ozone, mountains main.cpp; for the air, at the height most of the
// scattering happens, not at the camera's): in-scatter = sun * (sigma_R P_R + sigma_M P_M) /
// sigma_t * (1 - T), per channel. A small isotropic term stands in for multiple scattering.
//
// Uses: the sky behind everything (mtnSkyView), the ambient light the SSAO gathers (skyColor), and the
// aerial perspective on every surface by its distance (mtnAerial).
// =============================================================================

#ifndef MOUNTAINS_SKY_SC
#define MOUNTAINS_SKY_SC

uniform vec4 sunDir;    // xyz: toward the sun, unit length
uniform vec4 mtnSun;    // rgb: the sun's irradiance where the camera is (after the air above it); w: exposure
uniform vec4 mtnSunAir; // rgb: the sun's irradiance up in the air that scatters it (~6 km above the camera,
                        // less reddened: a low sun's sky stays blue rather than going green)
uniform vec4 mtnAtmos;  // x: air density (1 = sea-level standard); y: haze density; z: haze anisotropy g;
                        // w: multiple scattering's share

#define SUN_DIR   (sunDir.xyz)
#define SUN_COLOR (mtnSun.rgb)

#define MTN_BETA_R vec3(5.8e-6, 13.5e-6, 33.1e-6)   // per metre, at sea-level density
#define MTN_BETA_M 3.0e-6                           // per metre, haze at density 1
#define MTN_H_R 8000.0
#define MTN_H_M 1200.0
#define MTN_GROUND_ALBEDO vec3(0.42, 0.34, 0.26)    // the tan ground, for the light it throws back up

float mtnLayer(float y0, float dy, float d, float H) {
    float a = exp(-max(y0, -1000.0) / H);
    float k = dy / H;
    float kd = clamp(k * d, -30.0, 30.0);
    if (abs(kd) < 1e-4) return a * d;
    return a * (1.0 - exp(-kd)) / k;
}

// Transmittance T and in-scattered light L along `dir` from `origin` for `d` metres.
void mtnAtmosphere(vec3 origin, vec3 dir, float d, out vec3 T, out vec3 L) {
    float lr = mtnLayer(origin.y, dir.y, d, MTN_H_R);
    float lm = mtnLayer(origin.y, dir.y, d, MTN_H_M);
    vec3  sR = MTN_BETA_R * mtnAtmos.x * lr;
    float sM = MTN_BETA_M * mtnAtmos.y * lm;
    vec3  tau = sR + vec3(sM * 1.11, sM * 1.11, sM * 1.11);   // haze absorbs a little too
    T = exp(-tau);
    float mu = dot(dir, SUN_DIR);
    float pr = 0.0596831 * (1.0 + mu * mu);
    float g = mtnAtmos.z;
    float pm = 0.0795775 * (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5);
    vec3  share = (1.0 - T) / max(tau, vec3(1e-7, 1e-7, 1e-7));
    vec3  single = (sR * pr + vec3(sM * pm, sM * pm, sM * pm)) * share;
    vec3  multi = (sR + vec3(sM, sM, sM)) * share * (0.0796 * mtnAtmos.w);
    // A low sun lights the air less: the scattered light fades with it, as the sun's colour has.
    L = mtnSunAir.rgb * (single + multi);
}

// The sky as seen: the air to the top of the atmosphere (or far off along the horizon), and the sun.
vec3 mtnSkyView(vec3 dir) {
    vec3 T, L;
    float d = dir.y > 0.0 ? 1.0e6 : 4.0e5;
    mtnAtmosphere(pjvViewPosition(), dir, d, T, L);
    if (dir.y <= 0.0) L += T * MTN_GROUND_ALBEDO * SUN_COLOR * max(SUN_DIR.y, 0.0) * 0.3183;   // the plains beyond
    if (dot(dir, SUN_DIR) > 0.99996) L += SUN_COLOR * 40.0;                                    // the disc
    return L;
}

// The light arriving from a direction, for the ambient term: the sky above, the sunlit ground below.
vec3 skyColor(vec3 dir) {
    if (dir.y < 0.0) return MTN_GROUND_ALBEDO * SUN_COLOR * max(SUN_DIR.y, 0.0) * 0.3183;
    vec3 T, L;
    mtnAtmosphere(pjvViewPosition(), dir, 1.0e6, T, L);
    return L;
}

// The light arriving at `origin` from `dir` (no sun disc): the air above, the sunlit plains below the
// horizon. For diffuse rays that leave the scene.
vec3 mtnSkyFrom(vec3 origin, vec3 dir) {
    vec3 T, L;
    float d = dir.y > 0.0 ? 1.0e6 : 4.0e5;
    mtnAtmosphere(origin, dir, d, T, L);
    if (dir.y <= 0.0) L += T * MTN_GROUND_ALBEDO * SUN_COLOR * max(SUN_DIR.y, 0.0) * 0.3183;
    return L;
}

// A surface seen through `d` metres of air.
vec3 mtnAerial(vec3 colour, vec3 origin, vec3 dir, float d) {
    vec3 T, L;
    mtnAtmosphere(origin, dir, d, T, L);
    return colour * T + L;
}

#endif
