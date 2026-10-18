$input v_color0
$input v_texcoord0

// =============================================================================
// client.frag -- the multiplayer client's renderer: sunlight with shadows, a sky,
// distance fog and a crosshair to aim with.
//
// Two rays a pixel -- what the camera sees, then whether the sun reaches it --
// through ONE call to the march, in a loop. A second call site would inline the
// whole march twice, and that alone makes a shader of this kind about twice as
// slow (see the note on single march call sites).
// =============================================================================

// Nothing here animates: leave the animated march out of the shader.
#define PJV_NO_ANIMATION

#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;

#define FOV 65.0

vec3 sky(vec3 direction, vec3 sun) {
    float height = clamp(direction.y, 0.0, 1.0);
    vec3 colour = mix(vec3(0.62, 0.72, 0.84), vec3(0.28, 0.46, 0.78), pow(height, 0.6));
    float glow = pow(max(dot(direction, sun), 0.0), 64.0);
    return colour + vec3(1.0, 0.9, 0.7) * glow * 0.8 + vec3(1.0, 0.95, 0.85) * pow(max(dot(direction, sun), 0.0), 900.0) * 4.0;
}

void main() {
    const vec3 sun = normalize(vec3(0.45, 0.8, 0.35));
    Ray view;
    view.origin    = cameraPos.xyz;
    view.direction = rayStartDirection(v_texcoord0, windowRes.xy, cameraPos.xyz, normalize(cameraDir.xyz), FOV);

    vec3 colour = sky(view.direction, sun);
    vec3 albedo = vec3(0.0);
    vec3 normal = vec3(0.0, 1.0, 0.0);
    float distance = 0.0;
    bool found = false;
    bool sunlit = false;

    Ray ray = view;
    RayQuery query = pjvPrimaryQuery(100u);
    query.maxRaySteps = 768u;
    query.startLOD = 0;
    query.finishLOD = 0;
    query.distanceToFinishLOD = 10000;
    for (int pass = 0; pass < 2; pass++) {
        SceneHit result = raySceneIntersect(ray, query);
        if (pass == 0) {
            SceneIntersectData hit = result.hit;
            if (hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(hit.normal, hit.normal) < 0.5) break;
            found = true;
            albedo = fetchVoxelColor(hit.foundBox, hit.headerIndex);
            normal = normalize(hit.normal);
            distance = hit.rayT;
            if (dot(normal, sun) <= 0.0) break;   // facing away: in its own shade
            // The second pass: from just off the surface toward the sun.
            ray.origin = view.origin + view.direction * hit.rayT + normal * 0.02;
            ray.direction = sun;
            query = pjvShadowQuery(384u, 80.0);
        } else {
            sunlit = result.stopReason != PJV_STOP_OPAQUE;
        }
    }

    if (found) {
        float direct = sunlit ? max(dot(normal, sun), 0.0) : 0.0;
        // Sky light from above, a little bounce from below, and the sun.
        float ambient = 0.32 + 0.18 * normal.y;
        vec3 lit = albedo * (vec3(1.0, 0.95, 0.85) * direct * 0.95 + vec3(0.55, 0.65, 0.8) * ambient);
        float fog = 1.0 - exp(-max(distance - 30.0, 0.0) * 0.012);
        colour = mix(lit, sky(view.direction, sun), fog);
    }

    // A little tone curve, then the crosshair: two short bars through the centre, inverted.
    colour = colour / (colour + vec3(0.6)) * 1.6;
    vec2 pixel = v_texcoord0 * windowRes.xy;
    vec2 offset = abs(pixel - windowRes.xy * 0.5);
    bool bar = (offset.x < 1.0 && offset.y < 9.0 && offset.y > 3.0) ||
               (offset.y < 1.0 && offset.x < 9.0 && offset.x > 3.0);
    if (bar) colour = vec3(1.0) - colour * 0.8;

    gl_FragColor = vec4(colour, 1.0);
}
