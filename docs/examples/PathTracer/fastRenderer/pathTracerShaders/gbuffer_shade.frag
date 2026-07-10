$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer_shade.frag  --  Pass 1 of the fast renderer.
//
// A deliberately cheap, (almost) noise-free direct renderer. Unlike the path
// tracers it casts NO random GI bounces: every pixel is shaded from a single
// primary ray plus a single hard sun shadow ray, so the raw image is already
// stable and "pretty" before any temporal work.
//
// Per pixel:
//   - 1 primary ray (sub-pixel JITTERED each frame; the jitter is the only
//     stochastic element and exists purely so the temporal pass can resolve it
//     into anti-aliased edges).
//   - 1 HARD sun shadow ray (single deterministic direction -> crisp, reliable
//     direct light, no soft-shadow noise).
//
// Ambient / sky lighting is NOT computed here anymore. The next pass (rtao.frag)
// casts long hemisphere rays that gather the real occluded, coloured, directional
// sky irradiance (plus a one-bounce term). That integral needs the surface albedo
// but NOT the sun light, so we write the direct sun radiance and the raw albedo to
// SEPARATE targets: rtao produces the mean incoming radiance Li, and combine_blur
// composites direct + albedo * Li (diffuse response = albedo * mean(Li)).
//
// Output G-buffer (FBO 1):
//   gl_FragData[0]  gDirect   rgb = direct sun radiance, a = hit mask
//   gl_FragData[1]  gPos      rgb = world-space primary hit position, a = depth
//   gl_FragData[2]  gNormal   rgb = world-space normal, a = unused
//   gl_FragData[3]  gAmbient  rgb = surface albedo, multiplied by the gathered sky
//                              irradiance in the next pass
// =============================================================================

#include <bgfx_shader.sh>

#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;   // x = frame index

#define PI 3.14159265358979
#define FOV 60.0

// Sun / sky description lives in pjv_sun_sky.sc, shared by all three
// renderers so they read as the same scene under the same light.
#include <pjv_sun_sky.sc>

// Van der Corput / Halton for the sub-pixel jitter (base 2 and 3).
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

struct Hit {
    bool  hit;
    vec3  position;
    vec3  normal;
    vec3  albedo;
    float distance;
};

Hit traceScene(Ray ray, uint maxSteps) {
    RayQuery rq;
    rq.maxRaySteps = maxSteps;
    // Full-resolution primary ray. Distance-LOD was tried here (distanceToFinishLOD 64)
    // but gave almost no speed-up while making distant geometry unrecognisably blocky --
    // the primary march is NOT the bottleneck. Keep it crisp.
    rq.startLOD = 0;
    rq.finishLOD = 2;
    rq.distanceToFinishLOD = 10000;

    Hit h;
    SceneIntersectData sceneHit = raySceneIntersect(ray, rq);
    // Trust the march's own hit data. Re-intersecting foundBox with getRayBoxEntry
    // returned <= 0 for boundary-exact hits (ULP disagreement between the voxel-space
    // march and the world-space slab test), which misclassified real hits as sky ->
    // fireflies. The march already knows the exact entry t and crossed face.
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
    Voxel voxel = fetchVoxelData(sceneHit.foundBox, sceneHit.headerIndex);
    h.albedo = voxel.color;
    return h;
}

// Visibility-only trace: returns the distance to the nearest hit (<= 0.0 if the
// ray reaches its step budget without hitting) WITHOUT decoding the voxel's
// albedo/normal. The sun shadow ray only needs occlusion, so it skips
// fetchVoxelData() -- a ~19-iteration binary search over the multi-MB voxel-type
// buffer plus several header texture fetches per hit -- which is pure waste for a
// visibility query. Only the primary ray, which needs the surface colour, still
// pays for the full traceScene().
float traceOccludedDist(Ray ray, uint maxSteps) {
    RayQuery rq;
    rq.maxRaySteps = maxSteps;
    rq.startLOD = 0;
    rq.finishLOD = 0;
    rq.distanceToFinishLOD = 30;

    SceneIntersectData sceneHit = raySceneIntersect(ray, rq);
    // Occlusion distance straight from the march (-1 on a genuine miss). Re-intersecting
    // foundBox with getRayBoxEntry returned <= 0 for boundary-exact hits, so a shadow
    // ray grazing a voxel edge reported "unoccluded" and leaked full sun into shadow ->
    // fireflies in shadowed regions.
    return sceneHit.rayT;
}

// Hard shadow ray toward the sun: is the sun disk visible from p?
bool sunVisible(vec3 p, vec3 n) {
    Ray shadow;
    shadow.origin = p + n * (0.02 * WORLD_SCALE);
    shadow.direction = SUN_DIR;
    // rayT >= 0 means the march found an occluder; only a genuine miss (-1) is unoccluded.
    return traceOccludedDist(shadow, 128u) < 0.0;
}

void main() {
    // Sub-pixel jitter: the ONLY stochastic input. Halton keeps the offsets well
    // distributed over frames so the temporal pass converges to clean edges. The
    // offset is +/-0.5 px (the standard TAA amount) -- a wider spread just samples
    // across neighbouring pixels and over-softens the image.
    int   frame  = int(frameCount.x);
    vec2  jitter = vec2(halton(frame + 1, 2), halton(frame + 1, 3)) - 0.5;
    vec2  uvJit  = v_texcoord0 + jitter / windowRes.xy;

    Ray ray;
    ray.origin = cameraPos.xyz;
    ray.direction = rayStartDirection(
        uvJit,
        windowRes.xy,
        cameraPos.xyz,
        normalize(cameraDir.xyz),
        FOV
    );

    Hit primary = traceScene(ray, 256u);

    if (!primary.hit) {
        // Sky background: already stable, no shading needed. Sky goes in the
        // direct target; there is no AO-able ambient for the sky.
        gl_FragData[0] = vec4(skyColor(ray.direction), 0.0);
        gl_FragData[1] = vec4(ray.origin + ray.direction * 1e5, 1e5);
        gl_FragData[2] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[3] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    vec3  p      = primary.position;
    vec3  n      = primary.normal;
    vec3  albedo = primary.albedo;

    // Direct sun (single hard shadow ray). Written on its own so SSAO does not
    // darken sunlit surfaces -- occlusion only attenuates the sky ambient.
    vec3 direct = vec3(0.0);
    float NdotL = dot(n, SUN_DIR);
    if (NdotL > 0.0 && sunVisible(p, n)) {
        direct = (albedo / PI) * NdotL * SUN_COLOR;
    }

    // Raw albedo, to be multiplied by the sky irradiance gathered in the next pass.
    gl_FragData[0] = vec4(direct,  1.0);
    gl_FragData[1] = vec4(p, primary.distance);
    gl_FragData[2] = vec4(n, 0.0);
    gl_FragData[3] = vec4(albedo, 1.0);
}
