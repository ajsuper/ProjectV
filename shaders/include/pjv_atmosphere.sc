#ifndef PJV_ATMOSPHERE_SC
#define PJV_ATMOSPHERE_SC

// =============================================================================
// pjv_atmosphere.sc -- scattering and extinction in air (docs/plans/shader-library.md)
// =============================================================================
//
// The mechanisms a sky, a fog or a light shaft is made from: phase functions, Beer-Lambert,
// exponential height layers in closed form, distance fog, and the ray-sphere test for a planet's
// shell. No colours, densities or heights live here -- those are each renderer's look, passed in.
//
// cosTheta is the cosine between the direction light travels on and the direction it arrived from
// (1: straight on, forward scattering). g is the asymmetry: 0 isotropic, towards 1 forward.

#include <pjv_math.sc>   // PJV_PI

// --- Phase functions: how light scattering off a particle divides over directions. Each integrates
// to 1 over the sphere.

float pjvPhaseIsotropic() {
    return 1.0 / (4.0 * PJV_PI);
}

// Rayleigh: air molecules, much smaller than the light's wavelength. Symmetric front to back.
float pjvPhaseRayleigh(float cosTheta) {
    return 3.0 / (16.0 * PJV_PI) * (1.0 + cosTheta * cosTheta);
}

// Henyey-Greenstein: haze, fog, smoke -- particles that throw light forward.
float pjvPhaseHenyeyGreenstein(float cosTheta, float g) {
    float gg = g * g;
    return (1.0 - gg) / (4.0 * PJV_PI * pow(max(1.0 + gg - 2.0 * g * cosTheta, 1e-4), 1.5));
}

// Schlick's approximation to Henyey-Greenstein: one multiply-add and a divide instead of a pow, with
// k fitted to g.
float pjvPhaseSchlick(float cosTheta, float g) {
    float k = 1.55 * g - 0.55 * g * g * g;
    float d = 1.0 - k * cosTheta;
    return (1.0 - k * k) / (4.0 * PJV_PI * max(d * d, 1e-4));
}

// Cornette-Shanks: Henyey-Greenstein corrected towards Mie's back lobe, for cloud and aerosol.
float pjvPhaseCornetteShanks(float cosTheta, float g) {
    float gg = g * g;
    float numerator = 3.0 * (1.0 - gg) * (1.0 + cosTheta * cosTheta);
    float denominator = 8.0 * PJV_PI * (2.0 + gg) * pow(max(1.0 + gg - 2.0 * g * cosTheta, 1e-4), 1.5);
    return numerator / denominator;
}

// --- Extinction --------------------------------------------------------------

// Beer-Lambert: the fraction of light left after an optical depth.
float pjvTransmittance(float opticalDepth) {
    return exp(-opticalDepth);
}

vec3 pjvTransmittance3(vec3 opticalDepth) {
    return exp(-opticalDepth);
}

// The mean of e^-s over s in [0, k], relative to its value at 0: (1 - e^-k) / k. Exact at k = 0 (a
// level ray through a height layer), where the plain form divides zero by zero. Not floored: below
// about k = -88 it overflows, and the caller bounds k as fits its other clamps.
float pjvExpMean(float k) {
    return abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0 - 0.5 * k;
}

// The density of an exponential height layer at height y: `density` at baseHeight, falling by 1/e every
// scaleHeight up (and rising the same way below it).
float pjvHeightLayerDensity(float y, float density, float baseHeight, float scaleHeight) {
    return density * exp(-(y - baseHeight) / scaleHeight);
}

// How many scale heights a ray climbs over a distance: dirY (its unit direction's y) * distance /
// scaleHeight. Negative going down. pjvHeightLayerOpticalDepth takes it.
float pjvHeightLayerRise(float dirY, float distance, float scaleHeight) {
    return dirY * distance / scaleHeight;
}

// The optical depth along a ray through an exponential height layer, in closed form: the density
// where the ray starts, times the distance, times the layer's mean along the ray (pjvExpMean of its
// rise). The caller bounds both inputs, and the two bounds go together: a camera far above the layer
// has a vanishing density at its origin and a rise so steeply negative that e^-rise overflows. Where
// a renderer clamps one, it floors the other to match.
float pjvHeightLayerOpticalDepth(float densityAtOrigin, float rise, float distance) {
    return densityAtOrigin * distance * pjvExpMean(rise);
}

// Plain distance fog: the fraction hidden over a distance through a uniform density.
float pjvDistanceFog(float density, float distance) {
    return 1.0 - exp(-density * distance);
}

// --- A planet's shell --------------------------------------------------------

// Where a ray from origin along unit dir enters and leaves a sphere: (near, far) distances, either
// negative when behind the origin. far < near when it misses.
vec2 pjvRaySphere(vec3 origin, vec3 dir, vec3 centre, float radius) {
    vec3 oc = origin - centre;
    float b = dot(oc, dir);
    float c = dot(oc, oc) - radius * radius;
    float h = b * b - c;
    if (h < 0.0) return vec2(1.0, -1.0);
    h = sqrt(h);
    return vec2(-b - h, -b + h);
}

#endif
