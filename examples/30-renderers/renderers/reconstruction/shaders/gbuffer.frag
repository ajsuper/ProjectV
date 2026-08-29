$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer.frag  --  Pass 1 of the reconstruction demo. Runs at RENDER resolution.
//
// Deliberately small. Everything this renderer exists to show happens in the next pass, so this one
// does the least that makes reconstruction possible: one primary ray, direct sun, and -- the part
// that matters -- the identity of the voxel FACE each pixel landed on.
//
// That face identity is what separates this from a bilinear magnify. A blur has to guess what lies
// between two samples; reconstruction does not have to guess, because a voxel face is a flat quad of
// known position, size and orientation, and one sample of it describes the whole thing. Handing the
// next pass the face centre and edge length is what lets it rebuild a crisp edge at output
// resolution from a source that never had one.
//
// Outputs (FBO 1), in the order reconstruct.frag expects them:
//   0 gPos     xyz world hit, a = distance from camera (a < 0 => sky)
//   1 gNormal  xyz face normal, w = voxel edge length (the unit downstream offsets are in)
//   2 gAlbedo  xyz surface colour
//   3 gDirect  xyz direct sun (HARD shadow -- softened spatially in shade.frag), w = visibility
//   4 gFace    xyz voxel FACE CENTRE, w = voxel edge length
//   5 gKey     unused here; present so the slot layout matches the pass downstream
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>
#include <pjv_sun_sky.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;

#define FOV 60.0

// One hard shadow ray. Soft shadows would need many, and this renderer is not the one making that
// argument -- see renderers/tree64.
bool inSunShadow(vec3 origin, vec3 normal) {
    Ray shadow;
    shadow.origin    = origin + normal * 0.02;
    shadow.direction = normalize(sunDir.xyz);
    RayQuery q = pjvShadowQuery(128u, 1e6);
    return raySceneIntersect(shadow, q).hit.foundBox.size >= 0.0;
}

void main() {
    // No jitter. A jittered primary ray moves what the G-buffer holds every frame, and the
    // reconstruction reads the G-buffer to decide which face an output pixel belongs to -- so the
    // edges it rebuilds would move every frame with nothing downstream to average them. The
    // AdvancedRenderer hit exactly this and solved it by reconstructing BEFORE its temporal pass;
    // this demo has no temporal pass at all, so it simply stays deterministic.
    Ray ray;
    ray.origin    = cameraPos.xyz;
    ray.direction = rayStartDirection(v_texcoord0, windowRes.xy, cameraPos.xyz,
                                      normalize(cameraDir.xyz), FOV);

    RayQuery q = pjvPrimaryQuery(100u);
    q.maxRaySteps = 256u;
    q.startLOD = 0;
    q.finishLOD = 0;
    q.distanceToFinishLOD = 10000;

    SceneIntersectData hit = raySceneIntersect(ray, q).hit;

    vec3 normal = hit.normal;
    if (hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(normal, normal) < 0.5) {
        vec3 sky = skyColor(ray.direction);
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, -1.0);   // a < 0 marks sky
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[2] = vec4(sky, 1.0);
        gl_FragData[3] = vec4(sky, 1.0);   // sky: fully "visible", nothing to soften
        gl_FragData[4] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[5] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    normal = normalize(normal);
    vec3 P      = ray.origin + ray.direction * hit.rayT;
    vec3 albedo = fetchVoxelColor(hit.foundBox, hit.headerIndex);

    // The voxel the ray landed in, straight from the march: foundBox.position is its minimum corner
    // and foundBox.size its edge length. The face centre is the voxel centre pushed half an edge
    // along the outward normal -- exact, because the DDA marched at LOD 0 and foundBox is therefore
    // a single finest-resolution voxel rather than an interior node.
    float voxelSize  = hit.foundBox.size;
    vec3  voxelMin   = hit.foundBox.position;
    vec3  faceCentre = voxelMin + vec3(voxelSize * 0.5) + normal * (voxelSize * 0.5);

    // The shadow ray is a hard yes/no, and stays that way here. Softness is applied spatially in
    // shade.frag, which is what AdvancedRenderer does and for the same reason: a binary visibility
    // term magnified from half resolution gives hard stair-stepped shadow edges, and blurring the
    // VISIBILITY -- a signal with no noise in it -- costs one ring of taps instead of many rays.
    //
    // Visibility travels in gDirect.w so shade.frag can soften it before it is applied.
    float sunLambert  = max(0.0, dot(normal, normalize(sunDir.xyz)));
    float visibility  = inSunShadow(P, normal) ? 0.0 : 1.0;
    vec3  sunRadiance = albedo * pjvSunColor() * sunLambert;

    // Flat sky ambient so unlit faces are not black. No occlusion term on purpose:
    // renderers/fast is where screen-space AO is shown.
    vec3 ambient = albedo * skyColor(normal) * 0.35;

    gl_FragData[0] = vec4(P, length(P - cameraPos.xyz));
    gl_FragData[1] = vec4(normal, voxelSize);
    gl_FragData[2] = vec4(ambient, 1.0);   // the unshadowed half of the lighting
    gl_FragData[3] = vec4(sunRadiance, visibility);
    gl_FragData[4] = vec4(faceCentre, voxelSize);
    gl_FragData[5] = vec4(0.0, 0.0, 0.0, 0.0);
}
