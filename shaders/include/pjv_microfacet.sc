#ifndef PJV_MICROFACET_SC
#define PJV_MICROFACET_SC

// =============================================================================
// pjv_microfacet.sc -- reflection models (docs/plans/shader-library.md)
// =============================================================================
//
// The terms a surface model is built from, each taking its parameters: Lambert, the GGX distribution,
// Smith masking, Fresnel, and the samplers that go with them. pjv_bsdf.sc builds the engine material's
// BSDF from these; a renderer with another surface model builds its own.
//
// Angles come in as cosines against the normal, roughness as GGX alpha (perceptual roughness squared).
// Random numbers come in as arguments, so a caller keeps its own generator and its own draw order.

#include <pjv_math.sc>   // PJV_PI, pjvBasisAround

// Lambertian diffuse: the BRDF of a surface scattering albedo evenly over the hemisphere.
vec3 pjvLambert(vec3 albedo) {
    return albedo / PJV_PI;
}

// GGX / Trowbridge-Reitz normal distribution D(h).
float pjvGgxDistribution(float cosHalf, float alpha) {
    float a2 = alpha * alpha;
    float d = cosHalf * cosHalf * (a2 - 1.0) + 1.0;
    return a2 / max(PJV_PI * d * d, 1e-9);
}

// One direction's term of the separable Smith masking-shadowing function for GGX. The separable G is
// pjvSmithG1(cos_o) * pjvSmithG1(cos_i).
float pjvSmithG1(float cosTheta, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * cosTheta / max(cosTheta + sqrt(a2 + (1.0 - a2) * cosTheta * cosTheta), 1e-9);
}

// The height-correlated Smith G for GGX (Heitz 2014): masking and shadowing of one microsurface,
// rather than two independent ones. Slightly darker than the separable form at grazing angles.
float pjvSmithGCorrelated(float cosView, float cosLight, float alpha) {
    float a2 = alpha * alpha;
    float lambdaView = cosLight * sqrt(cosView * cosView * (1.0 - a2) + a2);
    float lambdaLight = cosView * sqrt(cosLight * cosLight * (1.0 - a2) + a2);
    return 2.0 * cosView * cosLight / max(lambdaView + lambdaLight, 1e-9);
}

// Schlick's Fresnel: f0 at normal incidence, rising to 1 at grazing.
vec3 pjvFresnelSchlick(float cosTheta, vec3 f0) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0, 1.0, 1.0) - f0) * (m2 * m2 * m);
}

// The exact Fresnel reflectance of an interface between dielectrics, unpolarised. eta is the
// refractive index on the far side over the near side's (1.5 for air into glass); cosIncident is
// against the normal on the near side. 1 under total internal reflection.
float pjvFresnelDielectric(float cosIncident, float eta) {
    float c = clamp(cosIncident, 0.0, 1.0);
    float sinTransmitted2 = (1.0 - c * c) / (eta * eta);
    if (sinTransmitted2 >= 1.0) return 1.0;
    float cosTransmitted = sqrt(1.0 - sinTransmitted2);
    float parallel = (eta * c - cosTransmitted) / (eta * c + cosTransmitted);
    float perpendicular = (c - eta * cosTransmitted) / (c + eta * cosTransmitted);
    return 0.5 * (parallel * parallel + perpendicular * perpendicular);
}

// --- Sampling ----------------------------------------------------------------
// World-space, about n. Their pdfs: cos / pi for the cosine lobe; D(h) cos_h for the GGX half vector,
// which is D(h) cos_h / (4 |wo . h|) once reflected into a direction.

// Cosine-weighted over the hemisphere about n. u1 picks the azimuth, u2 the polar angle (the mapping
// pjv_sampling.sc's pjvSampleCosineHemisphere draws the other way round, radius first).
vec3 pjvSampleCosine(vec3 n, float u1, float u2) {
    float phi = 2.0 * PJV_PI * u1;
    float cosTheta = sqrt(1.0 - u2);
    float sinTheta = sqrt(u2);
    return pjvBasisAround(n) * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// A half vector from the GGX distribution (density D cos_h).
vec3 pjvSampleGgxHalf(vec3 n, float alpha, float u1, float u2) {
    float phi = 2.0 * PJV_PI * u1;
    float a2 = alpha * alpha;
    float cosTheta = sqrt(clamp((1.0 - u2) / (1.0 + (a2 - 1.0) * u2), 0.0, 1.0));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    return pjvBasisAround(n) * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// Power heuristic, beta 2: one sampling strategy's MIS weight against another's.
float pjvPowerHeuristic(float pdfA, float pdfB) {
    float a = pdfA * pdfA;
    return a / max(a + pdfB * pdfB, 1e-9);
}

#endif
