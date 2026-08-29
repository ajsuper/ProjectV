$input v_color0
$input v_texcoord0

// =============================================================================
// hello.frag -- the entire Hello Voxel renderer.
//
// One primary ray per pixel, straight to the back buffer. No accumulation, no
// anti-aliasing, no lighting model: the voxel's stored colour, with a fixed tint
// per face so the shape reads as a shape rather than a flat silhouette.
//
// Everything specific to ProjectV is in the two calls below. rayStartDirection
// turns a pixel into a camera ray, and raySceneIntersect marches it through the
// voxel scene. Both come from pjv_utils_DDA.sc, the engine's shader library,
// which the build puts on shaderc's include path.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;

#define FOV 60.0

void main() {
    Ray ray;
    ray.origin    = cameraPos.xyz;
    ray.direction = rayStartDirection(
        v_texcoord0, windowRes.xy, cameraPos.xyz, normalize(cameraDir.xyz), FOV);

    // A query carries the march's budget and level-of-detail policy. 256 steps is
    // ample for one 64^3 chunk; a large streamed world is where the LOD fields
    // start to matter.
    RayQuery rayQuery = pjvPrimaryQuery(100u);
    rayQuery.maxRaySteps = 256u;
    rayQuery.startLOD = 0;
    rayQuery.finishLOD = 0;
    rayQuery.distanceToFinishLOD = 10000;

    SceneIntersectData sceneHit = raySceneIntersect(ray, rayQuery).hit;

    // A miss is reported through foundBox.size and rayT rather than a flag. Trust
    // the march's own hit data instead of re-intersecting the box: a fresh slab
    // test disagrees by ULPs on boundary-exact hits and calls them misses.
    vec3 normal = sceneHit.normal;
    if (sceneHit.foundBox.size < 0.0 || sceneHit.rayT <= 0.0 || dot(normal, normal) < 0.5) {
        // Vertical gradient, so "up" is legible even with nothing on screen.
        float height = clamp(ray.direction.y * 0.5 + 0.5, 0.0, 1.0);
        gl_FragColor = vec4(mix(vec3(0.05, 0.06, 0.08), vec3(0.16, 0.18, 0.22), height), 1.0);
        return;
    }

    vec3 albedo = fetchVoxelColor(sceneHit.foundBox, sceneHit.headerIndex);

    // Not a light: a constant tint per axis, so the six faces of a voxel are
    // distinguishable. Without it every face is the same colour and the sphere
    // reads as a flat disc. A real renderer traces a shadow ray here instead --
    // see examples/40-advanced-renderer.
    normal = normalize(normal);
    float facing = 0.55 + 0.45 * abs(dot(normal, normalize(vec3(0.35, 0.85, 0.4))));

    gl_FragColor = vec4(albedo * facing, 1.0);
}
