$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer_shade.frag  --  Pass 1 of the fast renderer, at half resolution each way.
//
// A deliberately cheap, noise-free direct renderer: one camera ray and one hard sun shadow ray per
// render texel, a quarter of the window's pixels. reconstruct.frag rebuilds the window from what each
// texel saw (the hit voxel's face, edge and rotation: a voxel face is a flat quad one sample
// describes), and resolve.frag accumulates the jitter below into antialiased edges.
//
// Each texel's camera ray goes through a different point of the texel each frame (jitterParams: 16
// Halton phases, or the centre with TAA off), so the resolve's history supersamples it.
//
// Outputs, in the slots the reconstruction and the resolve read them from (restir-pt's layout):
//   0 gPos     xyz the hit, w its distance (-1: sky)
//   1 gNormal  xyz the face's normal
//   2 gAlbedo  rgb albedo
//   3 gDirect  rgb the direct sun's light (the sky's colour on a miss), a 1 hit / 0 sky
//   4 gFace    xyz the hit face's centre, w the voxel's edge (0: sky)
//   5 gRot     the chunk's rotation
//   6 gMotion  xy where the hit was on screen last frame minus where it is now (uv), by the camera alone:
//              fast does not track moving chunks (docs/plans/fast-renderer.md, D4)
// =============================================================================

#include <bgfx_shader.sh>

#include <pjv_scene.sc>

// World-unit scale: the self-hit offset below was tuned for ~0.0727-unit voxels, and voxels are now
// one unit, so world distances are ~13.76x larger.
#define WORLD_SCALE 13.76
#include <pjv_camera.sc>      // the engine camera: pjvView*, pjvPrevView*

uniform vec4 frameCount;     // x = frame index
uniform vec4 jitterParams;   // xy: this frame's offset inside the render texel, in texels

#define PI 3.14159265358979

// Sun / sky description lives in pjv_sun_sky.sc, shared by the gallery's renderers so they read as the
// same scene under the same light.
#include <pjv_sun_sky.sc>

void main() {
    vec2 res = passTargetRes.xy;
    vec2 uv = v_texcoord0 + jitterParams.xy * passTargetRes.zw;

    PjvRay ray;
    ray.origin = pjvViewPosition();
    ray.direction = pjvViewRayDirection(uv, res);

    // Both rays through ONE march call site, the camera ray then the shadow ray: a second inlined march
    // made every mode of a renderer about twice as slow (the shader library's rule; restir-pt's gbuffer
    // does the same).
    bool  hit = false;
    vec3  position = ray.origin + ray.direction * 1e4;
    float distance = -1.0;
    vec3  normal = vec3(0.0, 0.0, 0.0);
    vec3  albedo = vec3(0.0, 0.0, 0.0);
    vec4  face = vec4(0.0, 0.0, 0.0, 0.0);
    vec4  rotation = vec4(0.0, 0.0, 0.0, 1.0);
    vec3  direct = skyColor(ray.direction);
    PjvRay current = ray;
    for (int i = 0; i < 2; i++) {
        PjvRayQuery rq = pjvPrimaryQuery(100u);
        rq.maxRaySteps = i == 0 ? 256u : 128u;
        // Distance-LOD on the camera ray was tried (distanceToFinishLOD 64) and gave almost no speed-up
        // while making distant geometry unrecognisably blocky. Keep it crisp.
        rq.startLOD = 0;
        rq.finishLOD = i == 0 ? 2u : 0u;
        rq.distanceToFinishLOD = i == 0 ? 10000u : 30u;
        PjvSceneIntersectData found = pjvRaySceneIntersect(current, rq).hit;
        if (i == 0) {
            // Trust the march's own hit data: re-intersecting foundBox misclassifies boundary-exact hits.
            if (found.foundBox.size < 0.0 || found.rayT <= 0.0 || dot(found.normal, found.normal) < 0.5) break;
            hit = true;
            distance = found.rayT;
            normal = normalize(found.normal);
            position = ray.origin + ray.direction * found.rayT;
            albedo = pjvFetchVoxelColor(found.foundBox, found.headerIndex);
            // What the reconstruction needs to know the whole voxel: its face's centre, its edge, and its
            // chunk's rotation (the voxel's minimum LOCAL corner plus the turned half-diagonal is its centre).
            float edge = found.foundBox.size;
            rotation = pjvHeader(int(found.headerIndex)).rotation;
            vec3 centre = found.foundBox.position + pjvRotationFromQuat(rotation) * vec3(edge * 0.5, edge * 0.5, edge * 0.5);
            face = vec4(centre + normal * (edge * 0.5), edge);
            // The sun, through one hard shadow ray -- on its own target, so the SSAO, which only
            // attenuates the sky's ambient, cannot darken a sunlit face.
            direct = vec3(0.0, 0.0, 0.0);
            if (dot(normal, SUN_DIR) <= 0.0) break;
            current.origin = position + normal * (0.02 * WORLD_SCALE);
            current.direction = SUN_DIR;
        } else if (found.rayT < 0.0) {   // only a genuine miss (-1) is unoccluded
            direct = (albedo / PI) * dot(normal, SUN_DIR) * SUN_COLOR;
        }
    }

    // Motion by the camera alone: where this point was in last frame's image, minus where it is now. The
    // sky's "point" is far along the ray from each frame's own camera, so it moves with the turn only.
    bool nowInFront;
    bool thenInFront;
    vec3 then = hit ? position : pjvPrevViewPosition() + ray.direction * 1e4;
    vec2 nowUV = pjvPerspectiveUV(position, res, pjvViewPosition(), pjvViewDirection(), pjvViewTanHalfFov(), nowInFront);
    vec2 thenUV = pjvPrevViewUV(then, res, thenInFront);

    gl_FragData[0] = vec4(position, distance);
    gl_FragData[1] = vec4(normal, 0.0);
    gl_FragData[2] = vec4(albedo, 0.0);
    gl_FragData[3] = vec4(direct, hit ? 1.0 : 0.0);
    gl_FragData[4] = face;
    gl_FragData[5] = rotation;
    gl_FragData[6] = vec4(thenInFront && nowInFront ? thenUV - nowUV : vec2(0.0, 0.0), 0.0, 0.0);
}
