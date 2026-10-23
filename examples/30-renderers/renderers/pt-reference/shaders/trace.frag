$input v_color0
$input v_texcoord0

// trace.frag -- pass 1, window resolution: one path per pixel per frame, nothing reused, nothing filtered.
//
// The camera ray (jittered inside the pixel, so the accumulation antialiases), then up to ptSettings.z
// bounces sampled from the surface's BSDF (pjv_bsdf.sc), with next-event estimation to the sun at every
// vertex. Both can find the sun -- a glossy lobe often does -- so each is weighted against the other by
// the power heuristic. Emission is added wherever a path meets it. Every ray is exact (no coarse LOD) and
// stops at the first solid voxel (raySceneIntersectFrom, as restir-pt: transparent materials draw solid).
//
// No clamp, no denoise, no history heuristics: a still view converges to the answer, which is what
// restir-pt is measured against. One restirTrace-style call site, in a loop whose bound is a uniform: a
// second inlined traversal would slow every ray in the shader.
//
// Output: rgb the path's radiance, a 1 where the camera ray hit something.

#include <bgfx_shader.sh>

#define PJV_NO_ANIMATION   // the gallery's scenes do not move
#include <pjv_utils_DDA.sc>
#include <pjv_path_light.sc>

uniform vec4 passTargetRes;   // engine-set: xy this pass's target size, zw 1 / size
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;      // x the frame index
uniform vec4 ptSettings;      // x the loop's bound (rays), y the surface offset (world units), z bounces

#define REF_FOV 60.0
#define REF_STEPS 512u

uint refPcg(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float refRandom(inout uint state) { return float(refPcg(state) >> 8u) * (1.0 / 16777216.0); }

struct RefHit {
    bool hit;
    float t;
    vec3 normal;
    VoxelMaterial material;
};

RefHit refTrace(vec3 origin, vec3 direction, bool shadow, float maxDistance) {
    Ray ray;
    ray.origin = origin;
    ray.direction = direction;
    RayQuery query = shadow ? pjvShadowQuery(REF_STEPS, maxDistance) : pjvPrimaryQuery(REF_STEPS);
    SceneIntersectData found = raySceneIntersectFrom(ray, query, 0.0);
    RefHit h;
    h.hit = !(found.foundBox.size < 0.0 || found.rayT <= 0.0 || dot(found.normal, found.normal) < 0.5);
    h.t = found.rayT;
    h.normal = found.normal;
    h.material = emptyVoxelMaterial();
    if (h.hit && !shadow) h.material = fetchVoxelMaterialFromHit(found);
    return h;
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uint seed = uint(pixel.x) * 1973u + uint(pixel.y) * 9277u + uint(frameCount.x) * 26699u;
    refPcg(seed);
    refPcg(seed);
    vec2 jitter = vec2(refRandom(seed), refRandom(seed)) - 0.5;
    vec3 direction = rayStartDirection(v_texcoord0 + jitter * passTargetRes.zw, passTargetRes.xy, cameraPos.xyz,
                                       normalize(cameraDir.xyz), REF_FOV);

    float sunPdf = 1.0 / pathSunSolidAngle();
    vec3 radiance = vec3(0.0, 0.0, 0.0);
    vec3 throughput = vec3(1.0, 1.0, 1.0);
    float sunWeight = 1.0;          // the MIS weight if this ray escapes onto the disc: 1 for the camera's
    float covered = 0.0;
    int vertices = 1 + int(ptSettings.z);
    int vertex = 0;

    // The walk: a path ray (camera or bounce), then -- at a surface facing the sun -- its shadow ray.
    vec3 origin = cameraPos.xyz;
    vec3 rayDir = direction;
    bool shadow = false;
    float maxDistance = 1e30;
    vec3 pending = vec3(0.0, 0.0, 0.0);   // the shadow ray's light, if nothing blocks it
    vec3 x = cameraPos.xyz;
    vec3 n = vec3(0.0, 1.0, 0.0);
    vec3 wo = -direction;
    PjvBsdf surface = pjvBsdf(vec3(0.0, 0.0, 0.0), 0.0, 0.0);
    bool bounce = false;
    int rays = int(ptSettings.x);
    for (int i = 0; i < rays; i++) {
        RefHit hit = refTrace(origin, rayDir, shadow, maxDistance);
        if (!shadow) {
            if (!hit.hit) {
                radiance += throughput * (pathSky(rayDir) + (pathInSun(rayDir) ? pathSunRadiance() * sunWeight : vec3(0.0, 0.0, 0.0)));
                break;
            }
            vertex++;
            n = dot(hit.normal, hit.normal) > 0.5 ? normalize(hit.normal) : -rayDir;
            x = origin + rayDir * hit.t;
            wo = -rayDir;
            if (vertex == 1) covered = 1.0;
            radiance += throughput * hit.material.emission;
            surface = pjvBsdf(hit.material.albedo, hit.material.glossiness, hit.material.metallic);
            // Next-event estimation: a point of the disc, weighted against the BSDF finding it.
            vec3 toSun = pathSampleSun(refRandom(seed), refRandom(seed));
            float cosSun = dot(n, toSun);
            bounce = true;
            if (cosSun > 0.0) {
                vec3 f;
                float bsdfPdf;
                pjvBsdfEvaluate(surface, n, wo, toSun, f, bsdfPdf);
                // L / pdf of the uniform cone sample is the disc's irradiance, SUN_COLOR.
                pending = throughput * f * cosSun * SUN_COLOR * pjvPowerHeuristic(sunPdf, bsdfPdf);
                if (pjvLuminance(pending) > 0.0) {
                    origin = x + n * ptSettings.y;
                    rayDir = toSun;
                    shadow = true;
                    maxDistance = 1e30;
                    bounce = false;
                }
            }
        } else {
            if (!hit.hit) radiance += pending;
            shadow = false;
            bounce = true;
        }
        if (!bounce) continue;
        // The next path ray: one lobe sampled, weighted by the combined value and pdf.
        if (vertex >= vertices) break;
        float uLobe = refRandom(seed);
        float u1 = refRandom(seed);
        float u2 = refRandom(seed);
        vec3 next = pjvBsdfSample(surface, n, wo, uLobe, u1, u2);
        float cosNext = dot(n, next);
        if (cosNext <= 0.0) break;
        vec3 f;
        float pdf;
        pjvBsdfEvaluate(surface, n, wo, next, f, pdf);
        if (pdf <= 0.0) break;
        throughput *= f * cosNext / pdf;
        sunWeight = pjvPowerHeuristic(pdf, sunPdf);
        origin = x + n * ptSettings.y;
        rayDir = next;
        shadow = false;
        maxDistance = 1e30;
    }
    // NaN never enters the running mean: it would never leave.
    radiance.r = radiance.r > 0.0 ? radiance.r : 0.0;
    radiance.g = radiance.g > 0.0 ? radiance.g : 0.0;
    radiance.b = radiance.b > 0.0 ? radiance.b : 0.0;
    gl_FragColor = vec4(radiance, covered);
}
