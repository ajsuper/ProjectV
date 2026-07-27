$input v_color0
$input v_texcoord0

// =============================================================================
// rtao.frag  --  Pass 2 of the fast renderer.
//
// Occluded, coloured, DIRECTIONAL sky lighting + a cheap one-bounce colour-bleed term,
// gathered with long hemisphere rays.
//
// Each pixel casts a few cosine-weighted hemisphere rays and lets each run until it
// either escapes the scene or hits geometry:
//   - ESCAPES -> that direction sees open sky, so we add skyGradient(dir): the real,
//                directional, coloured sky radiance for that direction. Averaged over
//                the hemisphere this is the true occluded sky irradiance -- contact
//                darkening and coloured skylight fall out for free.
//   - HITS    -> occluded. Instead of pure black we add a cheap ONE-BOUNCE term: the
//                blocker reflects the ambient sky it receives, tinted by its albedo, so
//                creases pick up the colour of nearby surfaces (see GI_BOUNCE below).
//
// Output is the cosine-weighted MEAN incoming radiance Li (RGB). For a diffuse
// surface Lo = albedo * mean(Li), so combine_blur just multiplies this by the primary
// surface albedo. No 1/PI or extra cosine belongs here.
//
// CLEANLINESS: a few long rays per pixel is noisy, but this feeds the SAME denoise
// chain the scalar-AO version used -- sky_reproject.frag accumulates it TEMPORALLY
// (unbiased running mean, converges to zero noise when parked), combine_blur does the
// spatial bilateral, and TAA resolves the rest. That temporal pass is new since this
// shader last existed, so the coloured version is now as clean as the scalar one was.
//
// Cost: long rays lean on the DDA LOD ramp -- full res near the surface (crisp contact
// occlusion), coarse solid boxes with distance (cheap escape). AO_SAMPLES is the
// quality/perf knob.
//
// Inputs (flattened FBO-input order == FBO 1's textures, then resourceTextures):
//   0 gDirect  1 gPos  2 gNormal  3 gAmbient   (only gPos + gNormal are used)
//   4 blueNoise (resourceTexture, texID 1)     drives the hemisphere sampling
// Output (FBO 2):
//   0 skyRaw   rgb = cosine-weighted mean incoming radiance (occluded sky + 1 bounce)
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos,    1);
SAMPLER2D(gNormal, 2);
// Blue-noise texture (LDR_RGBA_7.png, 256x256 RGBA8, 4 decorrelated channels).
// Register 4: FBO 1's four textures occupy 0..3 (gDirect,gPos,gNormal,gAmbient),
// so this first resourceTexture lands at 4 (see render.json rtao pass).
SAMPLER2D(blueNoise, 4);
#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 frameCount;

#define PI 3.14159265358979

#define AO_SAMPLES  1      // Hemisphere rays per pixel per frame. The denoiser cleans the rest.
#define AO_STEPS    24u    // DDA step budget per ray (+ dither). Long enough (with LOD) to escape.
#define GI_BOUNCE   1      // 1 = add a one-bounce colour-bleed term on occluded directions.
#define GI_STRENGTH 0.3    // Scales the one-bounce sky colour-bleed.

// Sun / sky description lives in pjv_sun_sky.sc, shared by all three
// renderers so they read as the same scene under the same light.
#include <pjv_sun_sky.sc>

// Hemispherical sky irradiance arriving on a surface with the given normal -- the
// (already-integrated) ambient a bounce surface receives, so the one-bounce sky term
// needs no second ray of its own.
vec3 skyAmbient(vec3 n) {
    return mix(SKY_GROUND, SKY_HORIZON, clamp(n.y * 0.5 + 0.5, 0.0, 1.0)) * 0.55;
}

// Build an orthonormal basis around a unit normal (Duff et al.).
void basis(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float d = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * d, -s * n.x);
    b = vec3(d, s + n.y * n.y * a, -n.y);
}

// Van der Corput radical inverse in base 2 (bit reversal). Second Hammersley dim.
float radicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 2^32
}

// Hammersley point set: N points that stratify the whole [0,1)^2 unit square. Under
// the cosine map below this covers the ENTIRE hemisphere with even density.
vec2 hammersley(uint i, uint n) {
    return vec2(float(i) / float(n), radicalInverse_VdC(i));
}

// Per-pixel 2D blue-noise offset from LDR_RGBA_7.png, decorrelated over frames by an
// R2 low-discrepancy shift. Point-sampled at texel centres (texelFetch) so the blue
// spectrum is preserved. Used as a Cranley-Patterson rotation of the Hammersley set.
vec2 blueNoiseOffset(vec2 pixel, float frame) {
    ivec2 c = ivec2(pixel) & ivec2(255);
    vec2 bn = texelFetch(blueNoise, c, 0).rg;
    return fract(bn + frame * vec2(0.7548776662, 0.5698402910));
}

// One long hemisphere ray. Returns the incoming radiance Li seen along `dir`:
//   - miss (ray escaped the scene) -> that direction's sky colour;
//   - hit  -> occluded: a cheap one-bounce sky colour-bleed term.
// LOD ramp: full resolution near the origin (accurate contact occlusion), coarse solid
// boxes with distance so a long ray stays affordable.
vec3 gatherRadiance(vec3 P, vec3 N, vec3 dir, uint maxSteps) {
    Ray ray;
    ray.origin    = P + N * (0.03 * WORLD_SCALE);
    ray.direction = dir;

    RayQuery rq;
    rq.maxRaySteps         = maxSteps;
    rq.startLOD            = 0u;   // Full detail near the surface.
    rq.finishLOD           = 1u;   // Collapse distant geometry into coarse boxes.
    rq.distanceToFinishLOD = 24u;  // Reach the coarsest LOD after ~24 voxels.

    SceneIntersectData hit = raySceneIntersect(ray, rq);

    // rayT < 0 (miss sentinel, incl. running out of steps) -> open sky. Gradient only,
    // no sun disk: a single stray ray landing on the disk would read as a bounded but
    // visible firefly in this noisy estimator. The sun's DIRECT contribution reaches the
    // image via gbuffer_shade's hard shadow ray, so nothing is lost by excluding it here.
    if (hit.rayT < 0.0) {
        return skyGradient(dir);
    }

#if GI_BOUNCE
    // Occluded: one cheap bounce. The blocker reflects the ambient sky it receives
    // (no extra shadow ray -- skyAmbient is already the integrated sky irradiance for
    // the blocker's facing), tinting the crease with the blocker's colour.
    vec3 v = fetchVoxelColor(hit.foundBox, hit.headerIndex);
    return v * skyAmbient(hit.normal) * GI_STRENGTH;
#else
    return vec3(0.0); // Occluded, no bounce -> pure ambient occlusion.
#endif
}

void main() {
    vec2 uv = v_texcoord0;

    vec3 N = texture2D(gNormal, uv).xyz;

    // Sky / background: no surface. combine passes the sky straight through.
    if (dot(N, N) < 0.01) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 P = texture2D(gPos, uv).xyz;

    vec3 t, b;
    basis(N, t, b);

    vec2  pixel  = uv * windowRes.xy;
    float frame  = frameCount.x;
    vec2  bnOff  = blueNoiseOffset(pixel, frame);

    vec3 radiance = vec3(0.0);
    for (int i = 0; i < AO_SAMPLES; i++) {
        // Complete, stratified 2D sample: Hammersley rotated by the per-pixel blue-noise
        // offset. u.x drives elevation, u.y azimuth -- both vary, full hemisphere covered.
        vec2  u = fract(hammersley(uint(i), uint(AO_SAMPLES)) + bnOff);

        // Cosine-weighted hemisphere map: the uniform average of Li over these directions
        // IS the cosine-weighted mean radiance, so diffuse Lo = albedo * (that mean).
        float cosT = sqrt(1.0 - u.x);
        float sinT = sqrt(u.x);
        float phi  = 2.0 * PI * u.y;
        vec3  dir  = normalize((cos(phi) * sinT) * t + (sin(phi) * sinT) * b + cosT * N);

        // Dither the per-ray step budget so the "ran out of steps" horizon does not land
        // at the same march length everywhere (a hard band); the blur + TAA then average
        // the scattered boundary into a smooth falloff.
        float stepRand = fract(texelFetch(blueNoise, ivec2(pixel) & ivec2(255), 0).b
                         + frame * 0.618034 + float(i) * 0.375);
        uint  steps = AO_STEPS + uint(stepRand * 24.0);

        radiance += gatherRadiance(P, N, dir, steps);
    }
    radiance /= float(AO_SAMPLES);

    gl_FragData[0] = vec4(radiance, 1.0);
}
