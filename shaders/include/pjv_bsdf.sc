#ifndef PJV_BSDF_SC
#define PJV_BSDF_SC

// =============================================================================
// pjv_bsdf.sc -- ProjectV's surface model, in one place (docs/plans/restir-pt.md)
// =============================================================================
//
// A voxel's material (decodeMaterial: albedo, glossiness, metallic) as a BSDF: a Lambertian diffuse
// lobe under a GGX microfacet specular lobe.
//
//   roughness   alpha = max(1 - glossiness, 0.03)^2
//   diffuse     albedo * (1 - metallic) / pi, times (1 - F)
//   specular    F D G / (4 cos_o cos_i): GGX D, separable Smith G, Schlick F,
//               F0 = mix(0.04 * glossiness, albedo, metallic)
//
// Hoisted from the scene editor's Render-tab path tracer, which defined it first; that path tracer, the
// gallery's pt-reference and restir-pt all shade through this file, so none of them can disagree with
// another about what a material looks like. The arithmetic is the editor's, expression for expression.
//
// Two rules, from the editor:
//
//   1. A DEFAULT MATERIAL COSTS NOTHING AND CHANGES NOTHING. glossiness 0 and metallic 0 -- every palette
//      written before materials had properties -- gives specularProbability exactly zero, which switches
//      the specular term and its pdf off: the BSDF is albedo / pi, the pdf cos / pi, exactly. That is why
//      the dielectric F0 is scaled by glossiness rather than a fixed 0.04, which would put a sheen on
//      every scene already on disk.
//   2. EVALUATION AND SAMPLING AGREE. pjvBsdfEvaluate returns the pdf pjvBsdfSample samples with, so an
//      estimator that weights one strategy against another (NEE against BSDF sampling) stays unbiased.
//
// Random numbers come in as arguments, so a caller keeps its own generator and its own draw order.

#include <pjv_color.sc>   // pjvLuminance
#include <pjv_math.sc>    // pjvBasisAround

#define PJV_BSDF_PI 3.14159265359

// A material's lobes, set up once per surface.
struct PjvBsdf {
    vec3  diffuseAlbedo;         // albedo * (1 - metallic)
    vec3  f0;                    // the specular reflectance at normal incidence
    float alpha;                 // GGX roughness
    float specularProbability;   // how often pjvBsdfSample picks the specular lobe; 0 = no specular lobe
};

// GGX roughness from the stored glossiness. Floored: a true mirror is a delta lobe, which these
// estimators cannot sample, and alpha 0 divides by zero. At the floor it still reads as a mirror.
float pjvBsdfAlpha(float glossiness) {
    float perceptual = clamp(1.0 - glossiness, 0.03, 1.0);
    return perceptual * perceptual;
}

PjvBsdf pjvBsdf(vec3 albedo, float glossiness, float metallic) {
    PjvBsdf s;
    // A metal has no diffuse lobe and tints its reflection with its own colour; a dielectric keeps its
    // diffuse colour and reflects white -- scaled by glossiness (rule 1).
    float dielectricF0 = 0.04 * glossiness;
    s.diffuseAlbedo = albedo * (1.0 - metallic);
    s.f0 = mix(vec3(dielectricF0, dielectricF0, dielectricF0), albedo, metallic);
    s.alpha = pjvBsdfAlpha(glossiness);
    s.specularProbability = 0.0;
    if (max(dielectricF0, metallic) > 0.0) {
        // The lobe in proportion to the light each carries, so a mirror rarely wastes a sample on its
        // diffuse lobe. Clamped away from 0 and 1: a lobe evaluated in the pdf but never sampled biases.
        float specularLuminance = pjvLuminance(s.f0);
        float diffuseLuminance = pjvLuminance(s.diffuseAlbedo);
        s.specularProbability = clamp(specularLuminance / max(specularLuminance + diffuseLuminance, 1e-4), 0.05, 0.95);
    }
    return s;
}

// GGX / Trowbridge-Reitz normal distribution.
float pjvGgxDistribution(float cosHalf, float alpha) {
    float a2 = alpha * alpha;
    float d = cosHalf * cosHalf * (a2 - 1.0) + 1.0;
    return a2 / max(PJV_BSDF_PI * d * d, 1e-9);
}

// One direction's term of the separable Smith masking-shadowing function for GGX.
float pjvSmithG1(float cosTheta, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * cosTheta / max(cosTheta + sqrt(a2 + (1.0 - a2) * cosTheta * cosTheta), 1e-9);
}

vec3 pjvFresnelSchlick(float cosTheta, vec3 f0) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0, 1.0, 1.0) - f0) * (m2 * m2 * m);
}

// The surface's response to light arriving along wi and leaving along wo, and the density pjvBsdfSample
// has for wi. Zero below either horizon.
void pjvBsdfEvaluate(PjvBsdf s, vec3 normal, vec3 wo, vec3 wi, out vec3 bsdf, out float pdf) {
    bsdf = vec3(0.0, 0.0, 0.0);
    pdf = 0.0;
    float cosLight = dot(normal, wi);
    float cosView = dot(normal, wo);
    if (cosLight <= 0.0 || cosView <= 0.0) return;

    vec3 diffuse = s.diffuseAlbedo / PJV_BSDF_PI;
    float diffusePdf = cosLight / PJV_BSDF_PI;
    if (s.specularProbability <= 0.0) {
        // Rule 1: the untouched material, byte for byte Lambertian.
        bsdf = diffuse;
        pdf = diffusePdf;
        return;
    }

    // NOTE: `half` is a reserved type name in HLSL -- do not name this that.
    vec3 halfVector = normalize(wo + wi);
    float cosHalf = max(dot(normal, halfVector), 0.0);
    float viewDotHalf = max(dot(wo, halfVector), 1e-4);
    float d = pjvGgxDistribution(cosHalf, s.alpha);
    float g = pjvSmithG1(cosView, s.alpha) * pjvSmithG1(cosLight, s.alpha);
    vec3 f = pjvFresnelSchlick(viewDotHalf, s.f0);
    vec3 specular = f * (d * g / max(4.0 * cosView * cosLight, 1e-6));
    // What the specular lobe reflects, the diffuse lobe underneath does not get to scatter.
    bsdf = diffuse * (vec3(1.0, 1.0, 1.0) - f) + specular;
    float specularPdf = d * cosHalf / (4.0 * viewDotHalf);
    pdf = mix(diffusePdf, specularPdf, s.specularProbability);
}

// The same, with the lobes apart and the diffuse one without its albedo: the BSDF is
// diffuseAlbedo * diffuseFactor + specular. For a renderer that filters the two separately, each divided
// by what it is multiplied back by (the diffuse light by the albedo, the specular by pjvSpecularAlbedo).
void pjvBsdfEvaluateSplit(PjvBsdf s, vec3 normal, vec3 wo, vec3 wi, out vec3 diffuseFactor, out vec3 specular, out float pdf) {
    diffuseFactor = vec3(0.0, 0.0, 0.0);
    specular = vec3(0.0, 0.0, 0.0);
    pdf = 0.0;
    float cosLight = dot(normal, wi);
    float cosView = dot(normal, wo);
    if (cosLight <= 0.0 || cosView <= 0.0) return;

    float diffusePdf = cosLight / PJV_BSDF_PI;
    if (s.specularProbability <= 0.0) {
        diffuseFactor = vec3(1.0, 1.0, 1.0) / PJV_BSDF_PI;
        pdf = diffusePdf;
        return;
    }
    vec3 halfVector = normalize(wo + wi);
    float cosHalf = max(dot(normal, halfVector), 0.0);
    float viewDotHalf = max(dot(wo, halfVector), 1e-4);
    float d = pjvGgxDistribution(cosHalf, s.alpha);
    float g = pjvSmithG1(cosView, s.alpha) * pjvSmithG1(cosLight, s.alpha);
    vec3 f = pjvFresnelSchlick(viewDotHalf, s.f0);
    specular = f * (d * g / max(4.0 * cosView * cosLight, 1e-6));
    diffuseFactor = (vec3(1.0, 1.0, 1.0) - f) / PJV_BSDF_PI;
    pdf = mix(diffusePdf, d * cosHalf / (4.0 * viewDotHalf), s.specularProbability);
}

// Cosine-weighted over the hemisphere about n: pdf cos / pi, which cancels the Lambertian BRDF's own.
vec3 pjvSampleCosine(vec3 n, float u1, float u2) {
    float phi = 2.0 * PJV_BSDF_PI * u1;
    float cosTheta = sqrt(1.0 - u2);
    float sinTheta = sqrt(u2);
    return pjvBasisAround(n) * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// A half vector from the GGX distribution (density D cos_h), for the specular lobe.
vec3 pjvSampleGgxHalf(vec3 n, float alpha, float u1, float u2) {
    float phi = 2.0 * PJV_BSDF_PI * u1;
    float a2 = alpha * alpha;
    float cosTheta = sqrt(clamp((1.0 - u2) / (1.0 + (a2 - 1.0) * u2), 0.0, 1.0));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    return pjvBasisAround(n) * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// A direction for light leaving along wo to have come from: one lobe, picked by uLobe against
// specularProbability, sampled with (u1, u2). May fall below the horizon (a GGX reflection can): callers
// test the cosine. Weight it by pjvBsdfEvaluate's value and pdf -- the COMBINED ones, whichever lobe it
// came from (dividing by the picked lobe's density alone makes glossy surfaces too bright).
vec3 pjvBsdfSample(PjvBsdf s, vec3 normal, vec3 wo, float uLobe, float u1, float u2) {
    if (s.specularProbability > 0.0 && uLobe < s.specularProbability)
        return reflect(-wo, pjvSampleGgxHalf(normal, s.alpha, u1, u2));
    return pjvSampleCosine(normal, u1, u2);
}

// Power heuristic, beta 2: one strategy's MIS weight against another's.
float pjvPowerHeuristic(float pdfA, float pdfB) {
    float a = pdfA * pdfA;
    return a / max(a + pdfB * pdfB, 1e-9);
}

// The specular lobe's directional albedo -- how much of uniform incident light it reflects towards wo --
// pre-integrated (Karis's fit to the split-sum LUT, "Physically Based Shading on Mobile", 2014), in
// place of a lookup table. Zero for a material with no specular lobe (rule 1).
vec3 pjvSpecularAlbedo(PjvBsdf s, float cosView) {
    if (s.specularProbability <= 0.0) return vec3(0.0, 0.0, 0.0);
    float roughness = sqrt(s.alpha);
    vec4 r = roughness * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
    float a004 = min(r.x * r.x, exp2(-9.28 * max(cosView, 0.0))) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return max(s.f0 * ab.x + vec3(ab.y, ab.y, ab.y), vec3(0.0, 0.0, 0.0));
}

#endif
