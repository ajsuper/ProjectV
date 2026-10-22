// restir_common.sc -- what every pass of the restir renderer shares: its uniforms, a hash RNG, the
// camera's inverse, the ReSTIR GI reservoir and its four-texture layout, and the reuse arithmetic.
//
// THE PIPELINE (render.json). "render" is the render resolution, half the window each way; the
// reconstruction rebuilds the window's resolution from it, as the voxel renderer does.
//    1 gbuffer        render     camera ray + sun shadow ray: position, normal, albedo, direct light, and
//                                the hit face and the chunk's rotation for the reconstruction
//    2 sample         GI         one candidate per GI pixel: a cosine bounce from its visible point, and
//                                that point's outgoing light (sun, emission, one more bounce + its sun)
//    3 temporal       GI         ReSTIR temporal reuse: the candidate against last frame's reservoir
//    4 spatial        GI         ReSTIR spatial reuse: five neighbours' reservoirs, and a visibility ray;
//                                resolves to demodulated indirect light (divided by albedo)
//    5 gi_accumulate  GI         temporal accumulation of that light, with luminance moments (variance)
//    6-9 atrous*      GI         SVGF-style a-trous filter, steps 1, 2, 4 and 8, edge-stopped by normal,
//                                plane and variance-scaled luminance; the last also hands on the surfaces
//   10 ssao           render     screen-space ambient occlusion, in a fixed 4x4 pattern
//   11 composite      render     edge-aware upsample of the indirect light: direct, and albedo * indirect
//   12 ao_apply       render     the SSAO, blurred over its pattern, on the indirect light; the views (G)
//   13 reconstruct    window     the voxel renderer's reconstruction: voxel by voxel, antialiased
//   14 resolve        window     temporal accumulation of the image (what is left of the noise, and the
//                                2x2 jitter's samples)
//   15 meter          64x64      log luminance, for the auto-exposure
//   16 exposure       1x1        the auto-exposure, adapted over time
//   17 display        window     the post (P: vignette, exposure, grade), then the neutral display transform
//
// The GI resolution is half the render resolution each way (the host's --restir-gi 2: the same as it).
// A GI pixel takes its visible point from one render texel of its stride x stride block -- a different
// one each frame (restirJitter.xy), so over four frames every texel's surface is sampled.

#ifndef RESTIR_COMMON_SC
#define RESTIR_COMMON_SC

#include <pjv_sun_sky.sc>   // sunDir, SUN_COLOR, skyColor / skyGradient: the gallery's light rig

uniform vec4 passTargetRes;   // engine-set: xy = this pass's target size, zw = 1 / size
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 prevCameraPos;   // last frame's camera, for every temporal pass
uniform vec4 prevCameraDir;
uniform vec4 frameCount;      // x = frame index, y = 1 when the camera or the sun moved this frame
uniform vec4 restirReuse;     // x temporal reuse, y spatial reuse, z visibility re-test, w temporal M cap
uniform vec4 restirDenoise;   // x a-trous on, y GI accumulation on, z view (0 image, 1 indirect, 2 direct), w GI history cap
uniform vec4 restirJitter;    // xy this frame's offset inside the block, z stride (4 or 2), w 1 = start every history over
uniform vec4 jitterParams;    // xy: where this frame's render texels are traced, off their centres, in texels
uniform vec4 restirLook;      // x SSAO radius (world units), y its strength on the indirect light
uniform vec4 restirSettings;  // x the trace loops' bound (a uniform, so they are not unrolled), y surface offset
                              // (world units, a twentieth of the finest voxel), z the image's longest history
                              // (resolve), w one render texel's width per unit of distance (2 tan(fov/2) / height)

#define RESTIR_FOV 60.0       // the gallery's, as every renderer here

// The look. The gallery's shared sky (pjv_sun_sky.sc) is bright and pale -- its zenith is about four
// times the voxel renderer's -- and lighting by it flooded every surface with blue-white and washed the
// colours out. restir lights by (and shows) a sky RESTIR_SKY_LIGHT as bright, near the voxel renderer's.
#ifndef RESTIR_SKY_LIGHT
#define RESTIR_SKY_LIGHT 0.35
#endif

#define RESTIR_PI 3.14159265

// PCG hash (Jarzynski & Olano), one state word per pixel.
uint restirPcg(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float restirRandom(inout uint state) {
    return float(restirPcg(state) >> 8u) * (1.0 / 16777216.0);
}

// Where world point P lands on screen (uv, top-left origin) for a camera at `position` looking along
// `forward` -- the inverse of the engine's rayStartDirection, basis and flip included.
vec2 restirWorldToUV(vec3 P, vec3 position, vec3 forward, vec2 res, out bool inFront) {
    vec3 f = normalize(forward);
    vec3 worldUp = abs(f.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(f, worldUp));
    vec3 up = normalize(cross(right, f));
    vec3 d = P - position;
    float z = dot(d, f);
    inFront = z > 1e-4;
    float scale = tan(radians(RESTIR_FOV * 0.5));
    vec2 ndc = vec2(dot(d, right) / (z * scale * (res.x / res.y)), dot(d, up) / (z * scale));
    return vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
}

// An orthonormal basis round unit n.
void restirBasis(vec3 n, out vec3 t, out vec3 b) {
    t = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    b = cross(n, t);
}

// Cosine-weighted over the hemisphere round n: the Lambertian BRDF and the cosine cancel against the pdf.
vec3 restirCosine(vec3 n, inout uint seed) {
    float u1 = restirRandom(seed);
    float u2 = restirRandom(seed);
    float r = sqrt(u1);
    float phi = 2.0 * RESTIR_PI * u2;
    vec3 t;
    vec3 b;
    restirBasis(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

// A direction inside the sun's cone, uniformly (soft shadow edges).
vec3 restirToSun(inout uint seed) {
    float cosT = mix(cos(SUN_ANGULAR), 1.0, restirRandom(seed));
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi = 2.0 * RESTIR_PI * restirRandom(seed);
    vec3 t;
    vec3 b;
    restirBasis(SUN_DIR, t, b);
    return normalize(t * (cos(phi) * sinT) + b * (sin(phi) * sinT) + SUN_DIR * cosT);
}

// Octahedral normal encoding (two floats).
vec2 restirOctWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}
vec2 restirOctEncode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : restirOctWrap(n.xy);
}
vec3 restirOctDecode(vec2 e) {
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = restirOctWrap(n.xy);
    return normalize(n);
}

float restirLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// The sky's radiance along d, without the sun's disc (what bounce rays that escape gather), and with it
// (what the camera sees).
vec3 restirSky(vec3 d) { return skyGradient(d) * RESTIR_SKY_LIGHT; }
vec3 restirSkyWithSun(vec3 d) { return dot(d, SUN_DIR) > cos(SUN_ANGULAR) ? SUN_COLOR : restirSky(d); }

// A ReSTIR GI reservoir: the sample (a reconnection point x2 with its normal and outgoing light), the
// visible point it was formed at (x1, n1, distance t1; t1 < 0 = no surface), and the weights.
//   texel 0: x2.xyz, Lo.r                 (RGBA32F)
//   texel 1: oct(n2), Lo.g, Lo.b          (RGBA16F)
//   texel 2: x1.xyz, t1                   (RGBA32F)
//   texel 3: oct(n1), M, W                (RGBA32F)
struct Reservoir {
    vec3  x2;
    vec3  n2;
    vec3  Lo;
    vec3  x1;
    float t1;
    vec3  n1;
    float M;
    float W;
    float wSum;   // only while resampling
};

Reservoir restirEmpty() {
    Reservoir r;
    r.x2 = vec3(0.0, 0.0, 0.0);
    r.n2 = vec3(0.0, 1.0, 0.0);
    r.Lo = vec3(0.0, 0.0, 0.0);
    r.x1 = vec3(0.0, 0.0, 0.0);
    r.t1 = -1.0;
    r.n1 = vec3(0.0, 1.0, 0.0);
    r.M = 0.0;
    r.W = 0.0;
    r.wSum = 0.0;
    return r;
}

Reservoir restirUnpack(vec4 a, vec4 b, vec4 c, vec4 d) {
    Reservoir r;
    r.x2 = a.xyz;
    r.Lo = vec3(a.w, b.z, b.w);
    r.n2 = restirOctDecode(b.xy);
    r.x1 = c.xyz;
    r.t1 = c.w;
    r.n1 = restirOctDecode(d.xy);
    r.M = d.z;
    r.W = d.w;
    r.wSum = 0.0;
    return r;
}

// A macro, not a function: bgfx's HLSL-family translation only lets main() write the outputs.
#define restirWrite(RESTIR_R) \
    gl_FragData[0] = vec4((RESTIR_R).x2, (RESTIR_R).Lo.r); \
    gl_FragData[1] = vec4(restirOctEncode((RESTIR_R).n2), (RESTIR_R).Lo.g, (RESTIR_R).Lo.b); \
    gl_FragData[2] = vec4((RESTIR_R).x1, (RESTIR_R).t1); \
    gl_FragData[3] = vec4(restirOctEncode((RESTIR_R).n1), (RESTIR_R).M, (RESTIR_R).W)

// The target function at visible point (x1, n1): the sample's light, cosine-weighted. (The Lambertian
// BRDF's albedo / pi is the same for every sample at a pixel and drops out.)
float restirTarget(Reservoir r, vec3 x1, vec3 n1) {
    vec3 d = r.x2 - x1;
    float len2 = dot(d, d);
    if (len2 < 1e-8) return 0.0;
    return restirLum(r.Lo) * max(0.0, dot(n1, d * inversesqrt(len2)));
}

// The reconnection shift's Jacobian (ReSTIR GI, eq. 11): moving the visible point from r.x1 to x1 while
// keeping x2 changes the solid angle x2 subtends -- dw = cos(at x2) dA / d^2 -- so a reservoir's weight,
// an inverse solid-angle pdf, carries over multiplied by (cos_now / cos_was) * (d_was^2 / d_now^2).
// That is what this returns, and callers MULTIPLY by it. (The paper writes p / |J| with J the other way
// up. Dividing by this ratio instead brightened the image and threw fireflies.) Shifts that change it by
// more than 8x either way are refused (0): those are the near-grazing reconnections whose weights blow up.
float restirJacobian(Reservoir r, vec3 x1) {
    vec3 was = r.x1 - r.x2;
    vec3 now = x1 - r.x2;
    float wasLen2 = max(dot(was, was), 1e-8);
    float nowLen2 = max(dot(now, now), 1e-8);
    float cosWas = abs(dot(r.n2, was * inversesqrt(wasLen2)));
    float cosNow = abs(dot(r.n2, now * inversesqrt(nowLen2)));
    float ratio = (cosNow / max(cosWas, 1e-4)) * (wasLen2 / nowLen2);
    return ratio < 0.125 || ratio > 8.0 ? 0.0 : ratio;
}

// Weighted reservoir update: take `s`'s sample with probability w / wSum.
void restirUpdate(inout Reservoir r, Reservoir s, float w, float u) {
    r.wSum += w;
    r.M += s.M;
    if (w > 0.0 && u * r.wSum < w) {
        r.x2 = s.x2;
        r.n2 = s.n2;
        r.Lo = s.Lo;
    }
}

// Is (x, n) the same surface as (x0, n0) seen at distance t0, as far as a GI pixel can tell? Off its
// plane by more than a sliver is another surface -- but the sliver grows with the GI pixel's width at
// that distance, and once that width passes a voxel the normals stop counting: one GI pixel then spans
// several voxels and faces, and the full-res pixel it samples (a different one each frame, jittered)
// lands on any of them. Holding that to the exact face threw the history away every frame in the
// distance -- the shimmer.
bool restirSameSurface(vec3 x, vec3 n, vec3 x0, vec3 n0, float t0) {
    float giPixel = t0 * restirSettings.w * restirJitter.z;
    float voxel = 20.0 * restirSettings.y;
    float minCos = giPixel > voxel ? -0.5 : 0.9;
    return dot(n, n0) > minCos && abs(dot(x - x0, n0)) < 0.004 * t0 + 4.0 * restirSettings.y + 2.0 * giPixel;
}

// The GI resolution's pixel, and the full-res pixel it takes its visible point from this frame.
ivec2 restirFullPixel(ivec2 giPixel, ivec2 fullSize) {
    int stride = int(restirJitter.z);
    return min(giPixel * stride + ivec2(restirJitter.xy), fullSize - ivec2(1, 1));
}

#endif
