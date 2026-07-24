$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer.frag  --  Pass 1 of the world-space PER-FACE radiance-cascade renderer (renderer 6).
//
// One primary ray per pixel. Produces the G-buffer every later pass consumes. This is the
// FUSION of renderer 5's cascade gbuffer and renderer 4's face gbuffer: as well as the usual
// pos/normal/albedo/direct targets it emits, for every pixel, the EXACT voxel-face identity the
// primary ray landed on (integer voxel index + face id) and the face-centre anchor. Those two
// extra targets are what let renderer 6 do renderer 4's rock-solid per-face temporal accumulation
// (exact-key gate, no ghosting) on top of renderer 5's world-space cascade GI.
//
// Unlike renderer 4, the primary ray IS sub-pixel jittered (for the TAA pass): the face key is
// derived from whatever voxel this pixel's jittered ray actually hit, so it stays consistent with
// this pixel's shading, and the accumulate pass point-samples the key, so jitter is harmless.
// LOD 0 is forced so every hit is a single finest voxel -> a well-defined face identity.
//
// Outputs (FBO 1, 6 targets):
//   0 gPos     rgb = world hit position, a = camera-space depth   (a < 0  => sky pixel)
//   1 gNormal  rgb = world normal,       a = hit mask (1 hit / 0 sky)
//   2 gAlbedo  rgb = surface albedo,     a = voxel edge size (world)
//   3 gDirect  rgb = albedo * direct sun (hard shadow); sky colour for background pixels
//   4 gFace    rgb = voxel FACE CENTRE (probe anchor for the cascade gather), a = voxel edge size
//   5 gKey     rgb = integer voxel index, a = face id (0..5)   (exact per-face temporal key)
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
    rq.maxRaySteps = 48u;
    rq.startLOD = 0u; rq.finishLOD = 0u; rq.distanceToFinishLOD = 30u;
    return raySceneIntersect(shadow, rq).rayT < 0.0;
}

// Van der Corput / Halton for the sub-pixel jitter (base 2 and 3) that feeds the TAA pass.
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

void main() {
    // Sub-pixel jitter of the primary ray: the AA sample offset that taa.frag resolves into
    // anti-aliased edges. +-0.5px Halton so each frame samples a different point within the pixel.
    int  frame  = int(frameCount.x);
    vec2 jitter = vec2(halton(frame + 1, 2), halton(frame + 1, 3)) - 0.5;
    vec2 uvJit  = v_texcoord0 + jitter / windowRes.xy;

    Ray ray;
    ray.origin = cameraPos.xyz;
    ray.direction = rayStartDirection(uvJit, windowRes.xy, cameraPos.xyz,
                                      normalize(cameraDir.xyz), FOV);

    // Primary ray at the finest LOD everywhere so every hit is a single finest voxel (a coarse LOD
    // would merge many voxels into one "face" and destroy the per-face identity + the crisp look).
    RayQuery rq;
    rq.maxRaySteps         = 512u;
    rq.startLOD            = 0u;
    rq.finishLOD           = 0u;
    rq.distanceToFinishLOD = 100000u;

    SceneIntersectData hit = raySceneIntersect(ray, rq);
    vec3 n = hit.normal;

    // Miss / degenerate boundary hit -> sky background. Store an impossible face key so nothing
    // in the temporal accumulate ever matches against a sky pixel.
    if (hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(n, n) < 0.5) {
        vec3 sky = skyColor(ray.direction);
        gl_FragData[0] = vec4(ray.origin + ray.direction * 1e5, -1.0); // a<0 => sky
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);                     // hit mask 0
        gl_FragData[2] = vec4(0.0);
        gl_FragData[3] = vec4(sky, 0.0);
        gl_FragData[4] = vec4(0.0);
        gl_FragData[5] = vec4(1e20, 1e20, 1e20, 1e20);                 // impossible key
        return;
    }
    n = normalize(n);

    vec3  P = ray.origin + ray.direction * hit.rayT;
    vec3  albedo = fetchVoxelColor(hit.foundBox, hit.headerIndex);

    // ---- Exact voxel-face identity (from the DDA's own foundBox), like renderer 4 -------------
    float vs        = hit.foundBox.size;                 // voxel edge length (world)
    vec3  boxMin    = hit.foundBox.position;             // voxel min corner  (world)
    vec3  boxCenter = boxMin + 0.5 * vs;
    vec3  faceCtr   = boxCenter + n * (0.5 * vs);         // centre of the hit face (probe anchor)
    vec3  vidx      = floor(boxMin / vs + 0.5);           // exact integer voxel index
    vec3  an        = abs(n);
    int   axis      = (an.x >= an.y && an.x >= an.z) ? 0 : (an.y >= an.z ? 1 : 2);
    float sgn       = (axis == 0 ? n.x : (axis == 1 ? n.y : n.z));
    float faceId    = float(axis * 2 + (sgn > 0.0 ? 1 : 0));

    // Direct sun (hard shadow) -- this is the light the GI bounces.
    vec3 direct = vec3(0.0);
    float NdotL = dot(n, SUN_DIR);
    if (NdotL > 0.0 && sunVisible(P, n)) {
        direct = (albedo / PI) * NdotL * SUN_COLOR;
    }

    float camDist = dot(P - cameraPos.xyz, normalize(cameraDir.xyz));

    gl_FragData[0] = vec4(P, camDist);
    gl_FragData[1] = vec4(n, 1.0);
    gl_FragData[2] = vec4(albedo, vs);
    gl_FragData[3] = vec4(direct, 1.0);
    gl_FragData[4] = vec4(faceCtr, vs);
    gl_FragData[5] = vec4(vidx, faceId);
}
