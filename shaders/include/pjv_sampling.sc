#ifndef PJV_SAMPLING_SC
#define PJV_SAMPLING_SC

// =============================================================================
// pjv_sampling.sc -- directions from uniform numbers (docs/plans/shader-library.md)
// =============================================================================
//
// Each sampler returns a direction in LOCAL space, about +z, from u in [0, 1)^2. The caller turns it
// into world space with the basis it chooses (pjvBasisAround, or its own): the basis is part of a
// renderer's noise pattern, so it is not chosen here.

#include <pjv_math.sc>

// Cosine-weighted over the hemisphere: pdf cos(theta) / pi. u.x picks the radius, u.y the angle.
vec3 pjvSampleCosineHemisphere(vec2 u) {
    float radius = sqrt(u.x);
    float phi = PJV_TWO_PI * u.y;
    return vec3(radius * cos(phi), radius * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
}

// Uniform over the cone of directions within acos(cosMax) of +z: pdf 1 / (2 pi (1 - cosMax)).
// u.x picks the polar angle, u.y the azimuth. A sun's disc, a soft light's solid angle.
vec3 pjvSampleUniformCone(vec2 u, float cosMax) {
    float cosTheta = mix(cosMax, 1.0, u.x);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = PJV_TWO_PI * u.y;
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

#endif
