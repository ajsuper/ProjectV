$input v_color0
$input v_texcoord0

// =============================================================================
// sandbox.frag -- Hello Voxel's single-ray renderer, with what a playable
// scene needs on top: a crosshair to aim with, distance fog so the arena's
// edge reads as distance, a soft light gradient across faces, and a larger
// march budget for a scene of hundreds of chunks.
//
// Still exactly one march per pixel. A second (a shadow ray, say) would be the
// obvious next step, and is what examples/40-advanced-renderer does with a
// whole pipeline around it.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;

#define FOV 60.0

vec3 sky(vec3 direction) {
    float height = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.10, 0.11, 0.15), vec3(0.42, 0.55, 0.72), pow(height, 1.5));
}

void main() {
    Ray ray;
    ray.origin    = cameraPos.xyz;
    ray.direction = rayStartDirection(
        v_texcoord0, windowRes.xy, cameraPos.xyz, normalize(cameraDir.xyz), FOV);

    RayQuery rayQuery = pjvPrimaryQuery(100u);
    rayQuery.maxRaySteps = 768u;
    rayQuery.startLOD = 0;
    rayQuery.finishLOD = 0;
    rayQuery.distanceToFinishLOD = 10000;

    SceneIntersectData sceneHit = raySceneIntersect(ray, rayQuery).hit;

    vec3 colour;
    vec3 normal = sceneHit.normal;
    if (sceneHit.foundBox.size < 0.0 || sceneHit.rayT <= 0.0 || dot(normal, normal) < 0.5) {
        colour = sky(ray.direction);
    } else {
        vec3 albedo = fetchVoxelColor(sceneHit.foundBox, sceneHit.headerIndex);
        normal = normalize(normal);
        vec3 sun = normalize(vec3(0.35, 0.85, 0.4));
        // Wrapped diffuse plus a little sky from above: not a light transport model, but enough
        // that a ball reads as round and a crate's faces are told apart.
        float diffuse = clamp(dot(normal, sun) * 0.6 + 0.4, 0.0, 1.0);
        float skyLight = 0.25 + 0.15 * normal.y;
        colour = albedo * (diffuse * 0.85 + skyLight);
        float fog = 1.0 - exp(-max(sceneHit.rayT - 40.0, 0.0) * 0.008);
        colour = mix(colour, sky(ray.direction), fog);
    }

    // Crosshair: two short bars through the centre of the image, inverted against what is behind.
    vec2 pixel = v_texcoord0 * windowRes.xy;
    vec2 offset = abs(pixel - windowRes.xy * 0.5);
    bool bar = (offset.x < 1.0 && offset.y < 9.0 && offset.y > 3.0) ||
               (offset.y < 1.0 && offset.x < 9.0 && offset.x > 3.0);
    if (bar) colour = vec3(1.0) - colour * 0.8;

    gl_FragColor = vec4(colour, 1.0);
}
