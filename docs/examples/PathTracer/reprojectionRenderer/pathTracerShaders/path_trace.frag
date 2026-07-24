$input v_color0
$input v_texcoord0

// =============================================================================
// path_trace.frag  --  Pass 1 of the reprojection renderer.
//
// A compact unidirectional path tracer written for this renderer. It only
// borrows the engine's voxel DDA traversal (pjv_utils_DDA.sc); all of the
// sampling / shading / G-buffer logic below is its own.
//
// Output G-buffer (FBO 1):
//   gl_FragData[0]  curColor   rgb = 1-spp radiance, a = hit mask (1 hit / 0 sky)
//   gl_FragData[1]  curPos     rgb = world-space primary hit position, a = depth
//   gl_FragData[2]  curNormal  rgb = world-space normal, a = unused
//   gl_FragData[3]  curAlbedo  rgb = surface albedo (kept for debugging)
//
// The image is intentionally left noisy (one path per pixel). The following
// reproject_accumulate pass turns that noise into a stable image by folding in
// the temporally reprojected history.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(resourceTexture, 0); // Blue-noise LUT (RGBA8, 256x256).
#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;   // x = frame index

#define PI 3.14159265358979
#define FOV 60.0
#define MAX_BOUNCES 3
#define FIREFLY_MAX 16.0 // Per-sample radiance clamp: kills sparkles from indirect
                         // bounces that randomly strike the bright sun disk, while
                         // still passing through direct sun on lit surfaces.

// Sun / sky description lives in pjv_sun_sky.sc, shared by all three
// renderers so they read as the same scene under the same light.
#include <pjv_sun_sky.sc>

// -----------------------------------------------------------------------------
// Random numbers: blue-noise LUT decorrelated per pixel/frame/bounce, mixed with
// a cheap hash so successive samples inside one path do not correlate.
// -----------------------------------------------------------------------------
float hash1(inout uint seed) {
    seed = seed * 747796405u + 2891336453u;
    uint x = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    x = (x >> 22u) ^ x;
    return float(x) * (1.0 / 4294967296.0);
}

vec2 blueNoise(ivec2 pixel, int bounce) {
    ivec2 c = (pixel + ivec2(int(frameCount.x) * 71 + bounce * 37,
                             int(frameCount.x) * 113 + bounce * 53)) % 256;
    return texelFetch(resourceTexture, c, 0).rg;
}

// Van der Corput / Halton low-discrepancy sequence (base 2 and 3), used for the
// sub-pixel primary-ray jitter that drives temporal anti-aliasing.
float halton(int i, int base) {
    float f = 1.0;
    float r = 0.0;
    for (int k = 0; k < 16; k++) {
        if (i <= 0) break;
        f /= float(base);
        r += f * float(i - (i / base) * base);
        i /= base;
    }
    return r;
}

// Build an orthonormal basis around a unit normal.
void basis(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float d = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * d, -s * n.x);
    b = vec3(d, s + n.y * n.y * a, -n.y);
}

// Cosine-weighted hemisphere sample around n. pdf = cos/PI, so for a Lambertian
// surface the path throughput simply picks up the albedo.
vec3 cosineSampleHemisphere(vec3 n, vec2 u) {
    float phi = 2.0 * PI * u.x;
    float r   = sqrt(u.y);
    vec3 local = vec3(cos(phi) * r, sin(phi) * r, sqrt(max(0.0, 1.0 - u.y)));
    vec3 t, b;
    basis(n, t, b);
    return normalize(local.x * t + local.y * b + local.z * n);
}

// Uniformly sample a direction inside the sun's cone (for soft shadow edges).
vec3 sampleSunDirection(vec2 u) {
    float cosMax = cos(SUN_ANGULAR);
    float cosT   = mix(cosMax, 1.0, u.x);
    float sinT   = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi    = 2.0 * PI * u.y;
    vec3 t, b;
    basis(SUN_DIR, t, b);
    return normalize((cos(phi) * sinT) * t + (sin(phi) * sinT) * b + cosT * SUN_DIR);
}

struct Hit {
    bool  hit;
    vec3  position;
    vec3  normal;
    vec3  albedo;
    float distance;
};

Hit traceScene(Ray ray, uint maxSteps, bool secondary) {
    RayQuery rq;
    rq.maxRaySteps = maxSteps;
    rq.startLOD = 0;
    rq.finishLOD = 0;
    rq.distanceToFinishLOD = 1;
    if (secondary) {
        rq.startLOD = 0;
        rq.finishLOD = 2;
        rq.distanceToFinishLOD = 70;
    }

    Hit h;
    SceneIntersectData sceneHit = raySceneIntersect(ray, rq);
    // Trust the march's own hit data. Re-intersecting foundBox with getRayBoxEntry
    // returned <= 0 for boundary-exact hits (ULP disagreement between the voxel-space
    // march and the world-space slab test), misclassifying real hits as sky and, on
    // shadow rays, leaking sun into shadow -> fireflies. The march already knows the
    // exact entry t and crossed face.
    vec3 nrm = sceneHit.normal;
    if (sceneHit.foundBox.size < 0.0 || sceneHit.rayT <= 0.0 || dot(nrm, nrm) < 0.5) {
        h.hit = false;
        h.distance = -1.0;
        h.normal = vec3(0.0);
        h.albedo = vec3(0.0);
        h.position = ray.origin + ray.direction * 1e5;
        return h;
    }
    h.hit = true;
    h.distance = sceneHit.rayT;
    h.normal = normalize(nrm);
    h.position = ray.origin + ray.direction * sceneHit.rayT;
    h.albedo = fetchVoxelColor(sceneHit.foundBox, sceneHit.headerIndex);
    return h;
}

// Shadow ray: returns true if the sun is visible from p along dir.
bool sunVisible(vec3 p, vec3 n, vec3 dir) {
    Ray shadow;
    shadow.origin = p + n * (0.02 * WORLD_SCALE);
    shadow.direction = dir;
    Hit s = traceScene(shadow, 128u, false);
    return !s.hit;
}

void main() {
    ivec2 pixel = ivec2(v_texcoord0 * windowRes.xy);
    uint seed = uint(pixel.x) * 1973u + uint(pixel.y) * 9277u + uint(frameCount.x) * 26699u + 1u;

    // Sub-pixel JITTER on the primary ray (+/- 0.5 px, Halton-distributed over
    // frames). This is what the temporal pass resolves into anti-aliased edges: over
    // a few frames each pixel averages its true sub-pixel coverage, so silhouettes go
    // from stair-stepped to smooth. The reproject/accumulate pass is built to make
    // this converge instead of smear (identity read + no validation when still lets
    // edge pixels average foreground/background across jitter offsets == the AA).
    vec2 jitter = vec2(halton(int(frameCount.x) + 1, 2),
                       halton(int(frameCount.x) + 1, 3)) - 0.5;
    vec2 uvJit  = v_texcoord0 + jitter / windowRes.xy;

    Ray ray;
    ray.origin = cameraPos.xyz;
    ray.direction = rayStartDirection(
        uvJit,
        windowRes.xy,
        cameraPos.xyz,
        normalize(cameraDir.xyz),
        FOV
    );

    Hit primary = traceScene(ray, 256u, false);

    vec3 radiance   = vec3(0.0);
    vec3 throughput = vec3(1.0);

    // Store the primary G-buffer straight away for the reprojection pass.
    vec3  gPos    = primary.position;
    vec3  gNormal = primary.normal;
    vec3  gAlbedo = primary.hit ? primary.albedo : vec3(0.0);
    float gDepth  = primary.hit ? primary.distance : 1e5;
    float hitMask = primary.hit ? 1.0 : 0.0;

    if (!primary.hit) {
        // Nothing to path trace, just show the sky.
        gl_FragData[0] = vec4(skyColor(ray.direction), 0.0);
        gl_FragData[1] = vec4(gPos, gDepth);
        gl_FragData[2] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[3] = vec4(0.0);
        return;
    }

    Hit current = primary;
    for (int bounce = 0; bounce < MAX_BOUNCES; bounce++) {
        vec3 n = current.normal;
        vec3 p = current.position;
        vec3 albedo = current.albedo;

        // --- Next-event estimation: direct lighting from the sun. ---
        vec2 su = blueNoise(pixel, bounce * 2 + 1);
        vec3 sunDir = sampleSunDirection(su);
        float NdotL = dot(n, sunDir);
        if (NdotL > 0.0 && sunVisible(p, n, sunDir)) {
            radiance += throughput * (albedo / PI) * NdotL * SUN_COLOR;
        }

        // --- Indirect: cosine-weighted diffuse bounce. ---
        if (bounce == MAX_BOUNCES - 1) break;
        vec2 du = blueNoise(pixel, bounce * 2 + 2);
        vec3 newDir = cosineSampleHemisphere(n, du);
        throughput *= albedo; // brdf*cos/pdf == albedo for a Lambertian surface.

        Ray next;
        next.origin = p + n * (0.02 * WORLD_SCALE);
        next.direction = newDir;
        Hit h = traceScene(next, 256u, true);
        if (!h.hit) {
            radiance += throughput * skyColor(newDir);
            break;
        }
        current = h;

        // Russian roulette to keep deep paths cheap.
        float q = max(throughput.r, max(throughput.g, throughput.b));
        if (hash1(seed) > q) break;
        throughput /= max(q, 1e-3);
    }

    radiance = min(radiance, vec3(FIREFLY_MAX));

    gl_FragData[0] = vec4(radiance, hitMask);
    gl_FragData[1] = vec4(gPos, gDepth);
    gl_FragData[2] = vec4(gNormal, 0.0);
    gl_FragData[3] = vec4(gAlbedo, 1.0);
}
