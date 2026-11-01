$input v_color0
$input v_texcoord0

// =============================================================================
// basic.frag -- the engine's basic renderer (graphics/basic_renderer.h).
//
// One primary ray per pixel, straight to the back buffer: the voxel's stored
// colour, lit by one sun with plain N.L and light from the sky so faces turned
// away still read. No shadows, no AO, no anti-aliasing, no history. It is a way
// to see a scene without writing a renderer; the look is BasicRendererOptions',
// uploaded as the pjvBasic* uniforms below.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_scene.sc>
#include <pjv_camera.sc>      // the engine camera: pjvView*
#include <pjv_color.sc>

uniform vec4 pjvBasicSun;           // xyz: toward the sun, unit length
uniform vec4 pjvBasicSunColour;     // xyz: linear
uniform vec4 pjvBasicAmbientAbove;  // xyz: linear light on a face turned straight up
uniform vec4 pjvBasicAmbientBelow;  // xyz: ... and straight down
uniform vec4 pjvBasicSkyAbove;      // xyz: the sky straight up, sRGB
uniform vec4 pjvBasicSkyBelow;      // xyz: ... and straight down
uniform vec4 pjvBasicOptions;       // x: draw a crosshair (1) or not (0)

vec3 sky(vec3 direction) {
    float height = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    return pjvSrgbToLinear(mix(pjvBasicSkyBelow.xyz, pjvBasicSkyAbove.xyz, height * height));
}

void main() {
    vec2 resolution = passTargetRes.xy;
    Ray ray;
    ray.origin    = pjvViewPosition();
    ray.direction = pjvViewRayDirection(v_texcoord0, resolution);

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
        float sun = max(dot(normal, pjvBasicSun.xyz), 0.0);
        vec3 skyLight = mix(pjvBasicAmbientBelow.xyz, pjvBasicAmbientAbove.xyz, normal.y * 0.5 + 0.5);
        colour = albedo * (pjvBasicSunColour.xyz * sun + skyLight);
    }

    colour = pjvDisplay(colour, 1.0, gl_FragCoord.xy);   // linear light out to the screen

    // Crosshair: two short bars through the centre, inverted against what is behind.
    // Here until a program can draw its own (docs/plans/shader-library.md, S4).
    if (pjvBasicOptions.x > 0.5) {
        vec2 offset = abs(v_texcoord0 * resolution - resolution * 0.5);
        bool bar = (offset.x < 1.0 && offset.y < 9.0 && offset.y > 3.0) ||
                   (offset.y < 1.0 && offset.x < 9.0 && offset.x > 3.0);
        if (bar) colour = vec3(1.0, 1.0, 1.0) - colour * 0.8;
    }

    gl_FragColor = vec4(colour, 1.0);
}
