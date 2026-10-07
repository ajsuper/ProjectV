$input v_color0
$input v_texcoord0

// =============================================================================
// surface.frag -- voxels as bounding boxes for implicit surfaces.
//
// All of the interesting work is in the engine's pjv_surface.sc: per voxel, a
// quadric clipped to the box, looked up from a hash table keyed by chunk and
// voxel. This file is a camera ray, a sun shadow ray through the same surfaces,
// and a sky.
//
//   1 voxels   2 contours (one plane per voxel)   3 quadrics
//   0 baseline: the plain voxel traversal with no surface lookup at all, for benchmarks
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_surface.sc>   // Brings in pjv_utils_DDA.sc with the surface hook switched on.

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
// x: mode (0 baseline, 1 voxels, 2 contours, 3 quadrics)   y: shadows on (1) / off (0)
// z: tint voxels with no quadric (1)   w: paint leaks red (1) -- a fitted surface hit from
//    behind, which on a closed mesh means the ray slipped through a crack
// The mode itself reaches the traversal through pjvSurfaceInfo.w.
uniform vec4 surfaceMode;

#define FOV 60.0

RayQuery svQuery() {
    RayQuery rayQuery = pjvPrimaryQuery(1024u);
    rayQuery.maxRaySteps = 1024u;
    rayQuery.startLOD = 0;
    rayQuery.finishLOD = 0;
    rayQuery.distanceToFinishLOD = 100000;
    return rayQuery;
}

vec3 svSky(vec3 dir) {
    float height = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.05, 0.06, 0.08), vec3(0.16, 0.18, 0.22), height);
}

void main() {
    int mode = int(surfaceMode.x + 0.5);

    Ray ray;
    ray.origin    = cameraPos.xyz;
    ray.direction = rayStartDirection(
        v_texcoord0, windowRes.xy, cameraPos.xyz, normalize(cameraDir.xyz), FOV);

    PjvSurfaceHit primary = pjvSurfaceIntersect(ray, svQuery());
    if (!primary.hit) {
        gl_FragColor = vec4(svSky(ray.direction), 1.0);
        return;
    }

    // Light the side the viewer sees: sheets are two-sided, and a contour plane's
    // normal can face away on a cut face.
    vec3 n = primary.normal;
    if (surfaceMode.w > 0.5 && primary.fitted && dot(n, ray.direction) > 0.0) {
        vec3 pathColour = vec3(1.0, 0.0, 0.0);
        if (primary.path == 2) pathColour = vec3(1.0, 1.0, 0.0);
        if (primary.path == 3) pathColour = vec3(0.0, 1.0, 0.0);
        if (primary.path == 4) pathColour = vec3(0.0, 1.0, 1.0);
        if (primary.path == 5) pathColour = vec3(0.0, 0.0, 1.0);
        gl_FragColor = vec4(pathColour, 1.0);
        return;
    }
    if (dot(n, ray.direction) > 0.0) n = -n;

    vec3 sunDir = normalize(vec3(-0.6, 0.75, -0.15));
    float ndl = max(dot(n, sunDir), 0.0);

    float shadow = 1.0;
    if (surfaceMode.y > 0.5 && ndl > 0.0) {
        Ray shadowRay;
        vec3 p = ray.origin + ray.direction * primary.t;
        // Off the surface, and a third of a voxel toward the light. Neighbouring voxels fit
        // the same surface independently and disagree by hundredths of a voxel, so a ray
        // starting right on the surface finds a neighbour's copy of it and shadows itself.
        // Stepping along the light (away from a lit surface) clears them; the price is
        // contact shadows thinner than that step.
        shadowRay.origin = p + (n * 0.05 + sunDir * 0.3) * primary.voxelSize;
        shadowRay.direction = sunDir;
        if (pjvSurfaceIntersect(shadowRay, svQuery()).hit) shadow = 0.0;
    }

    vec3 albedo = primary.albedo;
    if (surfaceMode.z > 0.5 && mode != PJV_SURFACE_VOXELS && !primary.fitted) {
        albedo = mix(albedo, vec3(1.0, 0.0, 1.0), 0.7);
    }

    vec3 ambient = mix(vec3(0.16, 0.15, 0.14), vec3(0.30, 0.34, 0.42), n.y * 0.5 + 0.5);
    vec3 sun = vec3(1.0, 0.95, 0.85) * 0.85 * ndl * shadow;
    gl_FragColor = vec4(albedo * (ambient + sun), 1.0);
}
