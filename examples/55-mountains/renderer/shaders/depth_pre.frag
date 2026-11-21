$input v_color0
$input v_texcoord0

// =============================================================================
// depth_pre.frag  --  Pass 0 of the mountains renderer: how far each ray may skip.
//
// At an eighth of the window each way (a quarter of the G-buffer's), one camera ray per texel,
// through its centre: how far it went before it hit anything, or that it hit nothing. gbuffer_shade
// starts each of its rays a little short of the nearest of the nine around it, so it does not walk the
// empty cells of every ring in front of the ground again ("beam optimisation").
//
// Conservative where it matters: a silhouette's near side wins the minimum, so a ray that just grazes
// a ridge still starts in front of it. A feature narrower than a pre-pass texel can slip between nine
// rays; the rings keep voxels about three window pixels wide or more, and a pre-pass texel is eight.
//
// The far field is marched the same way when the voxels miss, so its long rays skip ahead too.
//
// Output: x the distance to the hit (1e9: nothing).
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_scene.sc>
#include <pjv_camera.sc>

// The far field too, past the voxels: its rays are the long ones.
SAMPLER2D(farHeight, 0);
SAMPLER2D(farColour, 1);
SAMPLER2D(farState, 2);
#include <mountains_far.sc>

void main() {
    vec2 res = passTargetRes.xy;
    PjvRay ray;
    ray.origin = pjvViewPosition();
    ray.direction = pjvViewRayDirection(v_texcoord0, res);
    PjvRayQuery rq = pjvPrimaryQuery(100u);
    rq.maxRaySteps = 768u;
    rq.startLOD = 0;
    rq.finishLOD = 2u;
    rq.distanceToFinishLOD = 10000u;
    PjvSceneIntersectData found = pjvRaySceneIntersect(ray, rq).hit;
    bool hit = !(found.foundBox.size < 0.0 || found.rayT <= 0.0 || dot(found.normal, found.normal) < 0.5);
    float t = hit ? found.rayT : 1e9;
    if (!hit) {
        float tFar;
        int ring;
        if (mtnFarMarch(ray.origin, ray.direction, mtnLeaveVoxels(ray.origin, ray.direction), 2.0e5, tFar, ring)) t = tFar;
    }
    gl_FragData[0] = vec4(t, 0.0, 0.0, 1.0);
}
