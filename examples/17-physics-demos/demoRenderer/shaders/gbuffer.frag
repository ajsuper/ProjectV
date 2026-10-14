$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer.frag -- pass 1 of the demos' renderer, at RENDER resolution (a quarter of the output's
// pixels: resources.json sets the scale).
//
// One primary ray per pixel, and -- when renderParams.x is on -- one hard shadow ray toward the sun.
// Both go through the SAME raySceneIntersect call, in a loop of one or two: the march is huge once
// inlined, and a second call site slows every ray in the shader (measured in 35-surface-voxels).
// The loop's bound comes from a uniform so the compiler cannot unroll it into two copies.
//
// What it writes is what the passes after it need, and the face identity is what the reconstruction
// needs most: the face's centre, its voxel's edge length and the chunk's rotation pin down the whole
// voxel from this one sample, so reconstruct.frag can rebuild its edges at output resolution.
//
// Outputs (FBO 1):
//   0 gPos     xyz world hit, w = distance from the camera (w < 0: sky)
//   1 gNormal  xyz face normal (world), w = voxel edge length
//   2 gAlbedo  rgb surface colour (sky: the sky colour)
//   3 gLight   x = sun visibility (0 or 1), y = n . sun
//   4 gFace    xyz voxel FACE CENTRE (world), w = voxel edge length
//   5 gRot     the chunk's rotation, quaternion xyzw: the voxel's own axes
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 sunDir;
// x: shadow ray on (1) or off (0).
uniform vec4 renderParams;

#define FOV 60.0

vec3 sky(vec3 direction) {
    float height = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.10, 0.11, 0.15), vec3(0.42, 0.55, 0.72), pow(height, 1.5));
}

void main() {
    Ray ray;
    ray.origin    = cameraPos.xyz;
    ray.direction = rayStartDirection(v_texcoord0, windowRes.xy, cameraPos.xyz, normalize(cameraDir.xyz), FOV);
    vec3 primaryDirection = ray.direction;
    vec3 sun = normalize(sunDir.xyz);

    RayQuery query = pjvPrimaryQuery(100u);
    query.maxRaySteps = 768u;
    query.startLOD = 0;
    query.finishLOD = 0;
    query.distanceToFinishLOD = 10000;

    SceneIntersectData hit;
    float visibility = 1.0;
    int rays = renderParams.x > 0.5 ? 2 : 1;
    for (int i = 0; i < rays; i++) {
        SceneIntersectData found = raySceneIntersect(ray, query).hit;
        if (i == 0) {
            hit = found;
            bool missed = hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(hit.normal, hit.normal) < 0.5;
            vec3 n = missed ? vec3(0.0) : normalize(hit.normal);
            // No shadow ray for the sky, or for a face turned from the sun: it is unlit either way.
            if (missed || dot(n, sun) <= 0.0) break;
            ray.origin = cameraPos.xyz + primaryDirection * hit.rayT + n * 0.02;
            ray.direction = sun;
            query = pjvShadowQuery(256u, 1e6);
        } else {
            visibility = found.foundBox.size >= 0.0 ? 0.0 : 1.0;
        }
    }

    if (hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(hit.normal, hit.normal) < 0.5) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, -1.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[2] = vec4(sky(primaryDirection), 1.0);
        gl_FragData[3] = vec4(1.0, 0.0, 0.0, 0.0);
        gl_FragData[4] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[5] = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 normal = normalize(hit.normal);
    vec3 P = cameraPos.xyz + primaryDirection * hit.rayT;
    vec3 albedo = fetchVoxelColor(hit.foundBox, hit.headerIndex);

    // foundBox.position is the voxel's minimum LOCAL corner, carried to world by the chunk's
    // rotation and translation -- so in a turned chunk (a tumbling crate) the voxel's centre is that
    // corner plus the rotated half-diagonal, not plus a world-axis one.
    vec4 rotation = headers(int(hit.headerIndex)).rotation;
    mat3 R = rotationFromQuat(rotation);
    float voxelSize = hit.foundBox.size;
    vec3 centre = hit.foundBox.position + R * vec3(voxelSize * 0.5);
    vec3 faceCentre = centre + normal * (voxelSize * 0.5);

    gl_FragData[0] = vec4(P, hit.rayT);
    gl_FragData[1] = vec4(normal, voxelSize);
    gl_FragData[2] = vec4(albedo, 1.0);
    gl_FragData[3] = vec4(visibility, dot(normal, sun), 0.0, 0.0);
    gl_FragData[4] = vec4(faceCentre, voxelSize);
    gl_FragData[5] = rotation;
}
