// =============================================================================
// viewport_ray.sc  --  The viewport's primary ray, for every pass that has to agree on it exactly.
//
// albedo.frag casts it; reconstruct_common.sc rebuilds it per pixel to re-intersect the coarse trace's
// voxels; shade.frag rebuilds it per occlusion tap to turn a stored ray distance back into the point
// it hit. Each of them has to produce the same ray for the same pixel and frame -- to the bit, for the
// reconstruction -- so there is one copy of it.
//
// The camera is the engine's (pjv_camera.sc). The including shader declares frameCount first, and
// passTargetRes has to be the viewport's full resolution, which it is for all three.
// =============================================================================

#include <pjv_camera.sc>      // the engine camera: pjvView*, pjvPrevView*

// This frame's sub-pixel jitter, in pixels of whichever target the caller is writing: Halton in bases
// 2 and 3 (pjvHalton), the same sequence the fast renderer uses, so edges converge at the same rate.
vec2 viewportJitter() {
    int frame = int(frameCount.x);
    return vec2(pjvHalton(frame + 1, 2), pjvHalton(frame + 1, 3)) - 0.5;
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
    Ray ray;
    ray.origin = pjvViewRayOrigin(uv, passTargetRes.xy);
    ray.direction = pjvViewRayDirection(uv, passTargetRes.xy);
    return ray;
}
