$input v_color0
$input v_texcoord0

// =============================================================================
// basic.frag -- the whole renderer every feature example draws with.
//
// One primary ray per pixel, straight to the back buffer: the voxel's stored
// colour, lit by one fixed sun with plain N.L and a little light from the sky so
// faces turned away still read. No shadows, no AO, no anti-aliasing, no history.
// The examples are about what the engine does, not how it looks; the _demo
// examples and examples/30-renderers are where the looks are.
//
// Everything specific to ProjectV is in the two calls below. rayStartDirection
// turns a pixel into a camera ray, and raySceneIntersect marches it through the
// voxel scene -- loose chunks or a grid alike. Both come from pjv_utils_DDA.sc,
// the engine's shader library, which the build puts on shaderc's include path.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>
#include <pjv_color.sc>

uniform vec4 windowRes;     // xy: the window, in pixels
uniform vec4 cameraPos;     // xyz
uniform vec4 cameraDir;     // xyz, need not be normalised
uniform vec4 viewOptions;   // x: draw a crosshair (1) or not (0)

#define FOV 60.0
#define SUN normalize(vec3(0.35, 0.85, 0.4))

vec3 sky(vec3 direction) {
    float height = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    return pjvSrgbToLinear(mix(vec3(0.10, 0.11, 0.14), vec3(0.42, 0.55, 0.72), height * height));   // picked as sRGB
}

void main() {
    Ray ray;
    ray.origin    = cameraPos.xyz;
    ray.direction = rayStartDirection(
        v_texcoord0, windowRes.xy, cameraPos.xyz, normalize(cameraDir.xyz), FOV);

    // A query carries the march's budget and level-of-detail policy. The march
    // stops at the first hit, so a large budget costs only the rays that need it:
    // the long ones across a streamed world hundreds of metres wide.
    RayQuery rayQuery = pjvPrimaryQuery(1024u);
    rayQuery.maxRaySteps = 1024u;
    rayQuery.startLOD = 0;
    rayQuery.finishLOD = 0;
    rayQuery.distanceToFinishLOD = 100000;

    SceneIntersectData sceneHit = raySceneIntersect(ray, rayQuery).hit;

    // A miss is reported through foundBox.size and rayT rather than a flag. Trust
    // the march's own hit data instead of re-intersecting the box: a fresh slab
    // test disagrees by ULPs on boundary-exact hits and calls them misses.
    vec3 normal = sceneHit.normal;
    vec3 colour;
    if (sceneHit.foundBox.size < 0.0 || sceneHit.rayT <= 0.0 || dot(normal, normal) < 0.5) {
        colour = sky(ray.direction);
    } else {
        vec3 albedo = fetchVoxelColor(sceneHit.foundBox, sceneHit.headerIndex);
        normal = normalize(normal);
        float sun = max(dot(normal, SUN), 0.0);
        float skyLight = 0.3 + 0.1 * normal.y;   // more from above than below
        colour = albedo * (0.75 * sun + skyLight);
    }

    colour = pjvDisplay(colour, 1.0, gl_FragCoord.xy);   // linear light out to the screen

    // Crosshair: two short bars through the centre, inverted against what is behind.
    if (viewOptions.x > 0.5) {
        vec2 offset = abs(v_texcoord0 * windowRes.xy - windowRes.xy * 0.5);
        bool bar = (offset.x < 1.0 && offset.y < 9.0 && offset.y > 3.0) ||
                   (offset.y < 1.0 && offset.x < 9.0 && offset.x > 3.0);
        if (bar) colour = vec3(1.0, 1.0, 1.0) - colour * 0.8;
    }

    gl_FragColor = vec4(colour, 1.0);
}
