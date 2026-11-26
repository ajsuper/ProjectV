$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer_shade.frag  --  Pass 1 of the mountains renderer (forked from the gallery's fast), at half
// resolution each way.
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
//   3 gDirect  r the sun's visibility (0..1) on a hit, rgb the sky's colour on a miss; a 1 hit / 0 sky
//   4 gFace    xyz the hit face's centre, w the voxel's edge (0: sky)
//   5 gRot     the chunk's rotation
//   6 gMotion  xy where the hit was on screen last frame minus where it is now (uv), by the camera alone:
//              fast does not track moving chunks (docs/plans/fast-renderer.md, D4)
// =============================================================================

#include <bgfx_shader.sh>

#define PJV_SCENE_GRIDS_FIRST   // props: many small loose chunks over grid terrain (pjv_scene.sc)
#include <pjv_scene.sc>

#include <pjv_camera.sc>      // the engine camera: pjvView*, pjvPrevView*

uniform vec4 frameCount;     // x = frame index
uniform vec4 jitterParams;   // xy: this frame's offset inside the render texel, in texels

SAMPLER2D(preDepth, 0);      // depth_pre.frag: how far the rays around this one went
uniform vec4 renderParams;   // x: no sun shadows (1), y: no skip (1); z is reconstruct's

// How far this texel's camera ray may skip: a little short of the nearest of the nine pre-pass rays
// around it (each pre-pass texel is 4 x 4 of these).
float skipDistance(vec2 fragCoord) {
    ivec2 size = ivec2(textureSize(preDepth, 0));
    ivec2 c = ivec2(fragCoord) / 4;
    float nearest = 1e9;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            nearest = min(nearest, texelFetch(preDepth, clamp(c + ivec2(x, y), ivec2(0, 0), size - 1), 0).x);
    if (nearest >= 1e8) return 0.0;   // the sky all round: no ground to start near, march it all
    return max(0.0, nearest * 0.96 - 2.0);
}

#define PI 3.14159265358979

// The sky, the air and the sun: mountains_sky.sc.
#include <mountains_sky.sc>

// The far field (mountains_far.sc): the heightfield rings past the voxels.
SAMPLER2D(farHeight, 1);
SAMPLER2D(farColour, 2);
SAMPLER2D(farState, 3);
#include <mountains_far.sc>

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
    vec3  direct = mtnSkyView(ray.direction);
    // The camera ray starts where the pre-pass says the ground can begin; its distances are measured
    // from the camera all the same.
    float skip = renderParams.y > 0.5 ? 0.0 : skipDistance(gl_FragCoord.xy);
    PjvRay current = ray;
    current.origin = ray.origin + ray.direction * skip;
    // Each ray, the camera's then the shadow's, meets the voxels first and then, past them, the far field.
    bool farHit = false;
    vec3 blendNormal = vec3(0.0, 1.0, 0.0);
    float blendAmount = 0.0;
    for (int i = 0; i < 2; i++) {
        bool voxelHit = false;
        PjvSceneIntersectData found;
        found.foundBox.size = -1.0;
        found.rayT = -1.0;
        if (!(i == 1 && farHit)) {   // a far hit's shadow is the far field's alone
            PjvRayQuery rq = pjvPrimaryQuery(100u);
            rq.maxRaySteps = i == 0 ? 768u : 384u;
            // Distance-LOD on the camera ray was tried (distanceToFinishLOD 64) and gave almost no speed-up
            // while making distant geometry unrecognisably blocky. Keep it crisp.
            rq.startLOD = 0;
            rq.finishLOD = i == 0 ? 2u : 0u;
            rq.distanceToFinishLOD = i == 0 ? 10000u : 30u;
            // A shadow ray meets props (no taller than ~4 m) only near its start: past where it climbs over
            // the tallest of them it walks the terrain alone.
            if (i == 1) pjvQueryLooseReach(rq, min(4.0 / max(SUN_DIR.y, 0.08), 60.0));
            found = pjvRaySceneIntersect(current, rq).hit;
            // Trust the march's own hit data: re-intersecting foundBox misclassifies boundary-exact hits.
            voxelHit = !(found.foundBox.size < 0.0 || found.rayT <= 0.0 || dot(found.normal, found.normal) < 0.5);
        }
        if (i == 1 && voxelHit) break;   // in a voxel's shadow: direct stays 0
        if (i == 0 && voxelHit) {
            hit = true;
            distance = found.rayT + skip;
            normal = normalize(found.normal);
            position = ray.origin + ray.direction * distance;
            albedo = pjvFetchVoxelColor(found.foundBox, found.headerIndex);
            // What the reconstruction needs to know the whole voxel: its face's centre, its edge, and its
            // chunk's rotation (the voxel's minimum LOCAL corner plus the turned half-diagonal is its centre).
            float edge = found.foundBox.size;
            rotation = pjvHeader(int(found.headerIndex)).rotation;
            vec3 centre = found.foundBox.position + pjvRotationFromQuat(rotation) * vec3(edge * 0.5, edge * 0.5, edge * 0.5);
            face = vec4(centre + normal * (edge * 0.5), edge);
            // Toward the edge of the voxels, their shading turns into the far field's at the same place, so
            // nothing changes but the silhouette where the far field takes over.
            {
                vec4 B = mtnVoxelBox;
                if (B.x < B.z) {
                    vec2 halfSize = 0.5 * (B.zw - B.xy);
                    vec2 rel = abs(position.xz - 0.5 * (B.xy + B.zw)) / halfSize;
                    float w = smoothstep(0.6, 0.97, max(rel.x, rel.y));
                    float hFar;
                    int fFar;
                    if (w > 0.0 && mtnFarHeight(position.xz, hFar, fFar) && abs(position.y - hFar) < 3.0 * edge + 15.0) {
                        vec3 nFar, aFar;
                        mtnFarSurface(fFar, position.xz, distance * 2.0 * pjvViewTanHalfFov() / (res.y * 2.0), nFar, aFar);
                        albedo = mix(albedo, aFar, w);
                        blendNormal = nFar;
                        blendAmount = w;
                    }
                }
            }
            // The sun, through one hard shadow ray. Only whether it gets there is stored (gDirect.r):
            // combine_blur works out how much it lights with the smoothed normal (normal_smooth), which
            // may face the sun where this face does not -- the riser of a step on a slope that does.
            direct = vec3(0.0, 0.0, 0.0);
            float facing = dot(normal, SUN_DIR);
            if (facing <= -0.6) break;
            if (renderParams.x > 0.5) { direct = vec3(1.0, 0.0, 0.0); break; }
            // Off the surface by a share of its own voxel. A face turned from the sun starts just over its
            // voxel's top instead, so the ray clears the voxel.
            current.origin = position + normal * (edge * 0.05);
            if (facing <= 0.05) current.origin += vec3(0.0, edge * 0.55, 0.0);
            // Where a voxel is small on screen (its normal smoothed: normal_smooth.frag), the steps of a
            // slope must not shadow each other either: the ray starts a voxel up and a voxel toward the sun.
            float texelWorld = distance * 2.0 * pjvViewTanHalfFov() / res.y;
            float small = 1.0 - smoothstep(1.5, 3.5, edge / max(texelWorld, 1e-6));   // as normal_smooth's
            current.origin += (vec3(0.0, 1.0, 0.0) + SUN_DIR) * (edge * 1.1 * small);
            current.direction = SUN_DIR;
            continue;
        }
        // The far field, from where the ray leaves the voxel rings (inside them the voxels are the ground).
        float t0 = farHit ? 0.0 : mtnLeaveVoxels(current.origin, current.direction);
        float tFar;
        int ring;
        bool far = mtnFarMarch(current.origin, current.direction, t0, i == 0 ? 2.0e5 : 4.0e4, tFar, ring);
        if (i == 1) {
            direct = vec3(far ? 0.0 : 1.0, 0.0, 0.0);
            break;
        }
        if (!far) break;   // the sky
        hit = true;
        farHit = true;
        distance = tFar + skip;
        position = ray.origin + ray.direction * distance;
        mtnFarSurface(ring, position.xz, distance * 2.0 * pjvViewTanHalfFov() / (res.y * 2.0), normal, albedo);
        face = vec4(position, 0.0);   // not a voxel: the reconstruction upsamples it (D17)
        direct = vec3(0.0, 0.0, 0.0);
        if (dot(normal, SUN_DIR) <= -0.2) break;
        if (renderParams.x > 0.5) { direct = vec3(1.0, 0.0, 0.0); break; }
        float texel = mtnFarRing(ring).z / float(MTN_FAR_RES);
        current.origin = position + (normal + vec3(0.0, 1.0, 0.0)) * (texel * 0.5);
        current.direction = SUN_DIR;
    }

    // Motion by the camera alone: where this point was in last frame's image, minus where it is now. The
    // sky's "point" is far along the ray from each frame's own camera, so it moves with the turn only.
    bool nowInFront;
    bool thenInFront;
    vec3 then = hit ? position : pjvPrevViewPosition() + ray.direction * 1e4;
    vec2 nowUV = pjvPerspectiveUV(position, res, pjvViewPosition(), pjvViewDirection(), pjvViewTanHalfFov(), nowInFront);
    vec2 thenUV = pjvPrevViewUV(then, res, thenInFront);

    gl_FragData[0] = vec4(position, distance);
    // The normal shading reads (normal_smooth passes gNormal.w > 0 through as is): the voxel's own,
    // turned toward the far field's near the voxels' edge.
    if (blendAmount > 0.0) normal = normalize(mix(normal, blendNormal, blendAmount));
    gl_FragData[1] = vec4(normal, blendAmount);
    gl_FragData[2] = vec4(albedo, 0.0);
    gl_FragData[3] = vec4(direct, hit ? 1.0 : 0.0);
    gl_FragData[4] = face;
    gl_FragData[5] = rotation;
    gl_FragData[6] = vec4(thenInFront && nowInFront ? thenUV - nowUV : vec2(0.0, 0.0), 0.0, 0.0);
}
