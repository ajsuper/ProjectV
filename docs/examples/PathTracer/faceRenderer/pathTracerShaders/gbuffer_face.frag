$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer_face.frag  --  Pass 1 of the face renderer.
//
// One primary ray per pixel. Unlike the other renderers this pass does NOT shade
// anything; its whole job is to identify, for every pixel, WHICH VOXEL FACE the
// primary ray landed on and to emit a stable identity for that face. All lighting
// then happens once per face (next pass), keyed by this identity.
//
// A voxel face is uniquely described by (voxel grid index, face axis 0..5). Because
// the DDA marches at LOD 0 here, foundBox is always a single finest-resolution voxel:
//   - foundBox.position = the voxel's minimum corner (world space)
//   - foundBox.size     = the voxel's edge length (world space)
//   - sceneHit.normal   = the outward normal of the face the ray entered through
// From these we derive:
//   - the integer voxel index  vidx = round(position / size)         (exact, unique)
//   - the face id  0..5        from the dominant normal axis + sign
//   - the FACE CENTRE           = voxel centre + normal * (size/2)
// The face centre is the single anchor point the lighting pass samples from, so every
// pixel covering the same face computes the identical colour -> each face is one flat
// colour, with zero spatial noise and nothing to blur.
//
// Deliberately NO sub-pixel jitter: the face identity must be deterministic so all
// pixels of a face agree and temporal history keys exactly. This keeps voxel edges
// crisp (the intended blocky look) rather than temporally anti-aliased.
//
// Output G-buffer (FBO 1):
//   gl_FragData[0] gAlbedo  rgb = voxel albedo (or sky colour), a = hit mask
//   gl_FragData[1] gPos     rgb = world hit position, a = hit distance   (reprojection)
//   gl_FragData[2] gFace    rgb = face-centre anchor,  a = voxel edge size
//   gl_FragData[3] gKey     rgb = integer voxel index, a = face id (0..5)
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

void main() {
    Ray ray;
    ray.origin = cameraPos.xyz;
    ray.direction = rayStartDirection(
        v_texcoord0,
        windowRes.xy,
        cameraPos.xyz,
        normalize(cameraDir.xyz),
        FOV
    );

    // Primary ray at LOD 0 so every hit is a single finest-resolution voxel -- coarse
    // LOD boxes would merge many voxels into one "face" and destroy the per-face look.
    RayQuery rq;
    rq.maxRaySteps         = 256u;
    rq.startLOD            = 0u;
    rq.finishLOD           = 0u;
    rq.distanceToFinishLOD = 100000u;

    SceneIntersectData hit = raySceneIntersect(ray, rq);

    vec3 n = hit.normal;
    // Miss (or a degenerate boundary hit with no valid face normal) -> sky background.
    if (hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(n, n) < 0.5) {
        gl_FragData[0] = vec4(skyColor(ray.direction), 0.0);   // a=0 -> no surface
        gl_FragData[1] = vec4(ray.origin + ray.direction * 1e5, 1e5);
        gl_FragData[2] = vec4(0.0);
        gl_FragData[3] = vec4(0.0);
        return;
    }
    n = normalize(n);

    float vs        = hit.foundBox.size;                 // voxel edge length (world)
    vec3  boxMin    = hit.foundBox.position;             // voxel min corner  (world)
    vec3  boxCenter = boxMin + 0.5 * vs;
    vec3  faceCtr   = boxCenter + n * (0.5 * vs);         // centre of the hit face

    // Exact integer voxel index. position is a multiple of vs (grid aligned), so
    // round() recovers the index robustly; adjacent voxels differ by exactly 1.
    vec3 vidx = floor(boxMin / vs + 0.5);

    // Face id: dominant normal axis (0=x,1=y,2=z) times 2, +1 for the positive face.
    vec3  an   = abs(n);
    int   axis = (an.x >= an.y && an.x >= an.z) ? 0 : (an.y >= an.z ? 1 : 2);
    float sgn  = (axis == 0 ? n.x : (axis == 1 ? n.y : n.z));
    float faceId = float(axis * 2 + (sgn > 0.0 ? 1 : 0));

    vec3 p = ray.origin + ray.direction * hit.rayT;

    vec3 voxelColor = fetchVoxelColor(hit.foundBox, hit.headerIndex);

    gl_FragData[0] = vec4(voxelColor, 1.0);
    gl_FragData[1] = vec4(p, hit.rayT);
    gl_FragData[2] = vec4(faceCtr, vs);
    gl_FragData[3] = vec4(vidx, faceId);
}
