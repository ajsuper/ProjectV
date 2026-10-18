$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer.frag -- pass 1, at RENDER resolution (0.4 of the window on each axis by default).
//
// One primary ray per pixel and one sun shadow ray, both through the SAME scene query in a loop of one
// or two. The march is enormous once inlined, and a second call site makes every ray in the shader
// slower; the loop's bound comes from a uniform so the compiler cannot unroll it back into two copies.
// (The physics demos' renderer, which this one grew from, measured both.)
//
// ---- WHY raySceneIntersectFrom AND NOT raySceneIntersect ------------------------------------------
// raySceneIntersect is the engine's full query: it wraps the march in a loop that peels transparent
// layers, bends at refracting interfaces and resolves materials, and all of that is compiled into the
// shader whether a ray asks for it or not. raySceneIntersectFrom is the bare nearest-solid-voxel query
// underneath it. Measured here, at 0.4 scale:
//
//                                    Cave meadow    SanMiguel courtyard
//     raySceneIntersect               2.19 ms        4.74 ms
//       ...with transparency off      2.11 ms        4.67 ms
//     raySceneIntersectFrom           1.21 ms        2.60 ms
//
// So the cost was never the transparency a scene uses -- it is the machinery's mere presence. The
// march counters say why it matters so much: a ray here takes only about ten tree steps across one or
// two chunks (see GRR_STATS below), so fixed per-ray work and the shader's size are most of the bill.
// What this gives up is see-through voxels: every voxel is opaque, to the camera and to the sun.
//
// Outputs (FBO 1):
//   0 gPos     xyz world hit, w = distance along the ray (w < 0: sky)
//   1 gNormal  xyz face normal, w = the hit voxel's edge length
//   2 gAlbedo  rgb surface colour (sky: the sky's colour), a = emission, as a multiple of the albedo
//   3 gLight   x = sun visibility (0 or 1), y = n . sun
//   4 gFace    xyz the voxel FACE's centre, w = voxel edge length (0: sky)
//   5 gRot     the chunk's rotation quaternion -- the voxel's own axes, for the reconstruction
// =============================================================================

#include <bgfx_shader.sh>
// Nothing in this renderer animates, so the engine's animated march -- a second tree march inlined
// into every scene query -- is left out of the shader. Its size alone costs occupancy.
#define PJV_NO_ANIMATION
// GRR_STATS -- a measurement build, never the shipped one. 1 writes the PRIMARY ray's march counters
// into gAlbedo (r tree-loop iterations, g chunks marched, b grid cells visited, a = 1 if it hit), 2 the
// SHADOW ray's (a = 1 where one was cast). Build with it defined and read the target back with
// GAME_CAPTURE_TEXTURE=3. Measured on the default views it is ~10 iterations over 1-2 chunks a ray.
#ifndef GRR_STATS
#define GRR_STATS 0
#endif
#if GRR_STATS
#define PJV_MARCH_STATS
#endif
#include <pjv_utils_DDA.sc>
#include <grr_common.sc>

uniform vec4 passTargetRes;
// x = frame index, y = jitter amplitude in render pixels (0 = none), z = TAA on, w = camera moved
uniform vec4 frameParams;
// x = shadows on, y = AO on, z = reconstruction bypass, w = debug view
uniform vec4 renderParams;

// Where the shadow ray leaves the surface, in voxels. In voxels rather than world units because scenes
// span a thousand-fold range of scale, and a fixed epsilon is either a skipped occluder or a self-hit.
#define SHADOW_BIAS_VOXELS 0.25
#define PRIMARY_STEPS 768u
#define SHADOW_STEPS  256u
// The shadow ray coarsens to tree level 2 (16^3 blocks) over this many voxels away from the surface.
#define SHADOW_FINISH_LOD 2u
#define SHADOW_LOD_RAMP_VOXELS 32u
// The primary ray takes no LOD at all: at a reduced render scale "a node smaller than a render pixel"
// is several output pixels, and the merged blocks read as a visible change of detail with distance.

float halton(int index, int base) {
    float f = 1.0, r = 0.0;
    for (int k = 0; k < 8; k++) {
        if (index <= 0) break;
        f /= float(base);
        r += f * float(index - (index / base) * base);
        index /= base;
    }
    return r;
}

bool isMiss(SceneIntersectData h) {
    return h.foundBox.size < 0.0 || h.rayT <= 0.0 || dot(h.normal, h.normal) < 0.5;
}

void main() {
    // Sub-pixel jitter for TAA, off unless E turns it on (frameParams.y is then 1).
    int frame = int(frameParams.x);
    int phase = frame - (frame / 16) * 16 + 1;
    vec2 jitter = (vec2(halton(phase, 2), halton(phase, 3)) - 0.5) * frameParams.y;

    Ray ray;
    ray.origin = cameraPos.xyz;
    ray.direction = grrCameraRay(v_texcoord0 + jitter * passTargetRes.zw, passTargetRes.xy, cameraDir.xyz);
    vec3 primaryDirection = ray.direction;
    vec3 sun = normalize(sunDir.xyz);
    RayQuery query = pjvPrimaryQuery(PRIMARY_STEPS);

#if GRR_STATS
    vec4 statsOut = vec4(0.0, 0.0, 0.0, 0.0);
#endif
    SceneIntersectData hit;
    float visibility = 1.0;
    int rays = renderParams.x > 0.5 ? 2 : 1;
    for (int i = 0; i < rays; i++) {
        SceneIntersectData found = raySceneIntersectFrom(ray, query, 0.0);
        if (i == 0) {
            hit = found;
#if GRR_STATS == 1
            statsOut = vec4(float(pjvStatSteps), float(pjvStatMarches), float(pjvStatVisits),
                            isMiss(found) ? 0.0 : 1.0);
#elif GRR_STATS == 2
            pjvStatSteps = 0u; pjvStatMarches = 0u; pjvStatVisits = 0u;
#endif
            vec3 n = isMiss(found) ? vec3(0.0, 0.0, 0.0) : normalize(found.normal);
            // No shadow ray for the sky, or for a face turned from the sun: it is unlit either way.
            if (isMiss(found) || dot(n, sun) <= 0.0) break;
            ray.origin = cameraPos.xyz + primaryDirection * found.rayT +
                         n * (SHADOW_BIAS_VOXELS * found.foundBox.size);
            ray.direction = sun;
            query = pjvShadowQuery(SHADOW_STEPS, 1e6);
            // Full detail near the surface, where a shadow's shape is decided, coarsening with
            // distance: a leaf thirty voxels up casts the same blurred patch whether the ray sees the
            // leaf or the block around it, and the blocks are far fewer steps.
            //
            // The ramp is measured AWAY FROM THE SURFACE, not along the ray -- hence the divide by
            // n.sun. A merged box is bigger than the voxels in it, so the coarse boxes of the very wall
            // the ray started on stick out from it; a ray grazing up that wall stays close enough to
            // hit them, and a ramp along the ray drew the wall's LOD grid on it as false shadow.
            float rampVoxels = float(SHADOW_LOD_RAMP_VOXELS) / max(dot(n, sun), 0.05);
            pjvQueryLinearLOD(query, SHADOW_FINISH_LOD, uint(rampVoxels));
        } else {
#if GRR_STATS == 2
            statsOut = vec4(float(pjvStatSteps), float(pjvStatMarches), float(pjvStatVisits), 1.0);
#endif
            visibility = isMiss(found) ? 1.0 : 0.0;
        }
    }

    if (isMiss(hit)) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, -1.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[2] = vec4(grrSkyWithSun(primaryDirection), 0.0);
#if GRR_STATS
        gl_FragData[2] = statsOut;
#endif
        gl_FragData[3] = vec4(1.0, 0.0, 0.0, 0.0);
        gl_FragData[4] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[5] = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 normal = normalize(hit.normal);
    vec3 P = cameraPos.xyz + primaryDirection * hit.rayT;
    // The geometry-only query resolves no material, so the hit's is fetched here: one descent, for the
    // primary ray only.
    VoxelMaterial material = fetchVoxelMaterialFromHit(hit);
    vec3 albedo = material.albedo;
    // Emission travels as a multiple of the albedo, so it fits the albedo target's spare channel: an
    // emitter's glow is its own colour, scaled.
    float albedoPeak = max(max(albedo.r, albedo.g), max(albedo.b, 0.04));
    float emission = max(max(material.emission.r, material.emission.g), material.emission.b) / albedoPeak;

    // The voxel's centre is its minimum LOCAL corner plus the half-diagonal turned by the chunk's
    // rotation, so the face centre is right for a turned chunk as well as an axis-aligned one.
    vec4 rotation = headers(int(hit.headerIndex)).rotation;
    float voxelSize = hit.foundBox.size;
    vec3 centre = hit.foundBox.position + rotationFromQuat(rotation) * vec3(voxelSize * 0.5);
    vec3 faceCentre = centre + normal * (voxelSize * 0.5);

    gl_FragData[0] = vec4(P, hit.rayT);
    gl_FragData[1] = vec4(normal, voxelSize);
    gl_FragData[2] = vec4(albedo, emission);
#if GRR_STATS
    gl_FragData[2] = statsOut;
#endif
    gl_FragData[3] = vec4(visibility, dot(normal, sun), 0.0, 0.0);
    gl_FragData[4] = vec4(faceCentre, voxelSize);
    gl_FragData[5] = rotation;
}
