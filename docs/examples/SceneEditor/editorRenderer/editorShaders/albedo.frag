$input v_color0
$input v_texcoord0

// =============================================================================
// albedo.frag  --  Pass 1 of the scene previewer.
//
// One primary ray per pixel, and the voxel's stored albedo written out with no
// lighting applied at all. This is the whole renderer: no shadow ray, no GI, no
// sky model. It is derived from the PathTracer's `fast` renderer, which is the
// cheapest of that example's six, with everything except the primary march
// removed -- so a pixel costs exactly one scene ray.
//
// What that buys is a view of what is actually *in* a scene: the colours a
// voxelizer wrote, unmodulated by any light transport. A material that reads
// wrong here is wrong in the data, not in the lighting. The editor's two viewport
// toggles (ambient occlusion, normal shading) are applied by shade.frag on the
// way out rather than here, so this pass stays exactly that unmodulated view and
// turning both off returns the image to it byte for byte.
//
// The two geometry targets exist only to feed that pass. They are written whether
// or not either toggle is on: branching the march on a UI toggle would change the
// pass's cost with it, and one ray per pixel is cheap enough that the two extra
// attachments are the smaller price.
//
// The sub-pixel jitter is the only stochastic input, and it exists purely so the
// accumulate pass can resolve it into anti-aliased edges. Voxel silhouettes are
// all axis-aligned steps, which alias badly; without this the preview shimmers
// on every camera nudge.
//
// Outputs (FBO 1):
//   gl_FragData[0] previewColor    rgb = albedo (or the background), a = hit mask
//   gl_FragData[1] previewNormal   rgb = face normal, a = voxel edge length (world)
//   gl_FragData[2] previewPosition rgb = world hit position, a = hit mask
// =============================================================================

#include <bgfx_shader.sh>

#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;   // x = frame index
// x = 1.0 for an orthographic projection (the editor's Orthographic and Isometric
// modes) and 0.0 for perspective; y = the world height the image spans when it is;
// z = how far back along the view direction the ray plane sits. See the editor's
// cameraOrthoHeight / cameraOrthoBackoff, which compute all three.
uniform vec4 cameraProjection;

#define FOV 60.0

// Neutral, desaturated background. A previewer's job is to let you judge stored
// colours, and a strongly tinted or bright backdrop drags perception of every
// albedo in front of it -- so this is deliberately dark and almost grey rather
// than the sky model the lit renderers use.
#define BACKGROUND_TOP    vec3(0.115, 0.125, 0.145)
#define BACKGROUND_BOTTOM vec3(0.022, 0.024, 0.030)

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

// The primary ray, under whichever projection the editor has selected.
//
// Perspective is rayStartDirection's job and unchanged. Orthographic is the same
// camera basis used the other way round: every ray points along the view direction,
// and it is the *origin* that slides across a plane `orthoHeight` world units tall.
// The plane is pushed back by cameraProjection.z rather than left at the camera
// position, because parallel rays have no equivalent of "the camera is outside
// everything in front of it" -- without the offset, anything the camera has flown
// past would simply be missing from an orthographic view of the same scene.
Ray primaryRay(vec2 uv) {
    vec3 forward = normalize(cameraDir.xyz);

    Ray ray;
    if (cameraProjection.x < 0.5) {
        ray.origin = cameraPos.xyz;
        ray.direction = rayStartDirection(uv, windowRes.xy, cameraPos.xyz, forward, FOV);
        return ray;
    }

    // Identical to rayStartDirection's basis, including the +Z fallback for a view
    // pointing straight up or down. The two must agree exactly: the editor projects
    // its outlines and gizmo onto this image with the same construction on the CPU.
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));

    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float aspectRatio = windowRes.x / windowRes.y;
    float halfHeight = cameraProjection.y * 0.5;

    ray.origin = cameraPos.xyz - forward * cameraProjection.z +
                 right * (ndc.x * halfHeight * aspectRatio) +
                 up * (ndc.y * halfHeight);
    ray.direction = forward;
    return ray;
}

void main() {
    int  frame  = int(frameCount.x);
    vec2 jitter = vec2(halton(frame + 1, 2), halton(frame + 1, 3)) - 0.5;
    vec2 uvJit  = v_texcoord0 + jitter / windowRes.xy;

    Ray ray = primaryRay(uvJit);

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
        gl_FragData[0] = vec4(backgroundColor(ray.direction), 0.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);
        // a = 0 marks "no geometry here". shade.frag tests this rather than the colour
        // target's mask so a background pixel can never be read as a world position at
        // the origin, which an ambient occlusion tap would then treat as an occluder.
        gl_FragData[2] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // The march's own integer cell, not its world-space box: recovering a coordinate back out of
    // world space is a float32 round trip that shades voxels with their neighbour's colour once the
    // chunk has been moved or rotated. See SceneIntersectData::voxelCoord.
    vec3 albedo = fetchVoxelColorAtCoord(sceneHit.voxelCoord, sceneHit.headerIndex);

    // Straight from the march's own arithmetic, for the same reason the hit test above
    // trusts it: a position re-derived from a fresh slab test lands off the surface by
    // ULPs, and the occlusion estimator measures exactly that kind of small offset.
    vec3 hitPosition = ray.origin + ray.direction * sceneHit.rayT;

    gl_FragData[0] = vec4(albedo, 1.0);
    // The voxel's edge length travels with the normal because it is what gives the
    // occlusion radius a scene-independent unit -- see shade.frag's AO_RADIUS_VOXELS.
    gl_FragData[1] = vec4(normalize(normal), sceneHit.foundBox.size);
    gl_FragData[2] = vec4(hitPosition, 1.0);
}
