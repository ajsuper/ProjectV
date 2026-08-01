$input v_color0
$input v_texcoord0

// =============================================================================
// albedo.frag  --  Pass 1 of the scene previewer.
//
// One primary ray per pixel, and the voxel's stored albedo written out with no
// lighting applied at all. This is the whole renderer: no shadow ray, no AO, no
// GI, no sky model. It is derived from the PathTracer's `fast` renderer, which
// is the cheapest of that example's six, with everything except the primary
// march removed -- so a pixel costs exactly one scene ray.
//
// What that buys is a view of what is actually *in* a scene: the colours a
// voxelizer wrote, unmodulated by any light transport. A material that reads
// wrong here is wrong in the data, not in the lighting.
//
// The sub-pixel jitter is the only stochastic input, and it exists purely so the
// accumulate pass can resolve it into anti-aliased edges. Voxel silhouettes are
// all axis-aligned steps, which alias badly; without this the preview shimmers
// on every camera nudge.
//
// Output (FBO 1):
//   gl_FragColor  rgb = albedo (or the background), a = hit mask
// =============================================================================

#include <bgfx_shader.sh>

#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;   // x = frame index

#define FOV 60.0

// Neutral, desaturated background. A previewer's job is to let you judge stored
// colours, and a strongly tinted or bright backdrop drags perception of every
// albedo in front of it -- so this is deliberately dark and almost grey rather
// than the sky model the lit renderers use.
#define BACKGROUND_TOP    vec3(0.115, 0.125, 0.145)
#define BACKGROUND_BOTTOM vec3(0.022, 0.024, 0.030)

// Optional readability aid, OFF by default. Pure albedo means every face of a
// voxel is the same colour, so shape is carried entirely by silhouette and flat
// regions read as flat. Setting this to 1 multiplies in a fixed per-face
// gradient from the voxel normal -- no rays, no light source, just a constant
// tint per axis -- which makes form legible at the cost of no longer being the
// unmodified stored colour. Left off so the default output is exactly the data.
#define PREVIEW_FACE_SHADING 0

// Van der Corput / Halton for the sub-pixel jitter (base 2 and 3). Same sequence
// the fast renderer uses, so edges converge at the same rate.
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

vec3 backgroundColor(vec3 direction) {
    float height = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(BACKGROUND_BOTTOM, BACKGROUND_TOP, height);
}

void main() {
    int  frame  = int(frameCount.x);
    vec2 jitter = vec2(halton(frame + 1, 2), halton(frame + 1, 3)) - 0.5;
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

    RayQuery rayQuery;
    rayQuery.maxRaySteps = 256u;
    // Full-resolution primary march, matching the fast renderer. Distance LOD was
    // measured there to buy almost nothing while making distant geometry blocky,
    // and a previewer is exactly where blocky-at-distance would mislead.
    rayQuery.startLOD = 0;
    rayQuery.finishLOD = 2;
    rayQuery.distanceToFinishLOD = 10000;

    SceneIntersectData sceneHit = raySceneIntersect(ray, rayQuery);

    // Trust the march's own hit data rather than re-intersecting foundBox: a fresh
    // slab test disagrees with the march by ULPs on boundary-exact hits and
    // misclassifies them as misses.
    vec3 normal = sceneHit.normal;
    if (sceneHit.foundBox.size < 0.0 || sceneHit.rayT <= 0.0 || dot(normal, normal) < 0.5) {
        gl_FragColor = vec4(backgroundColor(ray.direction), 0.0);
        return;
    }

    vec3 albedo = fetchVoxelColor(sceneHit.foundBox, sceneHit.headerIndex);

#if PREVIEW_FACE_SHADING
    normal = normalize(normal);
    float facing = 0.55 + 0.45 * abs(dot(normal, normalize(vec3(0.35, 0.85, 0.4))));
    albedo *= facing;
#endif

    gl_FragColor = vec4(albedo, 1.0);
}
