$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer.frag  --  Pass 1 of the radiance-cascade renderer.
//
// One primary ray per pixel. Produces the G-buffer that every later pass consumes and,
// crucially, the DIRECT-LIT colour buffer (gDirect) that the screen-space cascade rays
// sample when they hit a surface -- that's how bounced sunlight enters the GI.
//
// Outputs (FBO 1):
//   0 gPos     rgb = world hit position, a = camera-space depth   (a < 0  => sky pixel)
//   1 gNormal  rgb = world normal,       a = hit mask (1 hit / 0 sky)
//   2 gAlbedo  rgb = surface albedo
//   3 gDirect  rgb = surface outgoing radiance (albedo * direct sun, hard shadow);
//                    for sky pixels this holds the sky colour for the background.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;

#define PI 3.14159265358979
#define FOV 60.0

#include <pjv_sun_sky.sc>

// Hard sun shadow ray: is the sun disk visible from p (surface facing n)?
bool sunVisible(vec3 p, vec3 n) {
    Ray shadow;
    shadow.origin = p + n * 0.02;
    shadow.direction = SUN_DIR;
    RayQuery rq;
    rq.maxRaySteps = 128u;
    rq.startLOD = 0u; rq.finishLOD = 0u; rq.distanceToFinishLOD = 30u;
    return raySceneIntersect(shadow, rq).rayT < 0.0;
}

void main() {
    Ray ray;
    ray.origin = cameraPos.xyz;
    ray.direction = rayStartDirection(v_texcoord0, windowRes.xy, cameraPos.xyz,
                                      normalize(cameraDir.xyz), FOV);

    // Primary ray at the finest LOD everywhere -- a distance LOD ramp collapses far
    // geometry into big coarse blocks (the "LOD too coarse" look). GI is screen-space so
    // only this primary + the sun shadow touch the voxel tree; keep the visible surface crisp.
    RayQuery rq;
    rq.maxRaySteps         = 512u;
    rq.startLOD            = 0u;
    rq.finishLOD           = 0u;
    rq.distanceToFinishLOD = 100000u;

    SceneIntersectData hit = raySceneIntersect(ray, rq);
    vec3 n = hit.normal;

    // Miss / degenerate boundary hit -> sky background.
    if (hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(n, n) < 0.5) {
        vec3 sky = skyColor(ray.direction);
        gl_FragData[0] = vec4(ray.origin + ray.direction * 1e5, -1.0); // a<0 => sky
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);                     // hit mask 0
        gl_FragData[2] = vec4(0.0);
        gl_FragData[3] = vec4(sky, 0.0);
        return;
    }
    n = normalize(n);

    vec3  P = ray.origin + ray.direction * hit.rayT;
    Voxel voxel = fetchVoxelData(hit.foundBox, hit.headerIndex);
    vec3  albedo = voxel.color;

    // Direct sun (hard shadow) -- this is the light the GI bounces.
    vec3 direct = vec3(0.0);
    float NdotL = dot(n, SUN_DIR);
    if (NdotL > 0.0 && sunVisible(P, n)) {
        direct = (albedo / PI) * NdotL * SUN_COLOR;
    }

    float camDist = dot(P - cameraPos.xyz, normalize(cameraDir.xyz));

    gl_FragData[0] = vec4(P, camDist);
    gl_FragData[1] = vec4(n, 1.0);
    gl_FragData[2] = vec4(albedo, 1.0);
    gl_FragData[3] = vec4(direct, 1.0);
}
