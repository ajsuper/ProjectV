$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer.frag  --  Pass 1 of the radiance-cascade renderer.
//
// One primary ray per pixel. Produces the G-buffer that every later pass consumes and,
// crucially, the DIRECT-LIT colour buffer (gDirect) that carries direct sunlight and voxel
// emission straight to compose, unfiltered by the GI's temporal machinery.
//
// The split is the point of the whole pipeline: direct light and albedo are HIGH frequency (a
// shadow edge, a voxel colour boundary) and must stay crisp, while the indirect term is low
// frequency and can be denoised hard. So this pass writes them apart, the cascade passes resolve
// only the indirect, accumulate averages only that, and compose puts them back together.
//
// Outputs (FBO 1):
//   0 gPos     rgb = world hit position, a = camera-space depth   (a < 0  => sky pixel)
//   1 gNormal  rgb = world normal,       a = the hit voxel's edge length (0 on a sky pixel)
//   2 gAlbedo  rgb = surface diffuse albedo (metal subtracted out)
//   3 gDirect  rgb = surface outgoing radiance -- albedo * soft-shadowed direct sun, plus the
//                    voxel's own emission; for sky pixels this holds the sky colour instead.
//   4 gFace    rgb = the hit voxel FACE's centre in world space, a = the voxel's edge length
//   5 gKey     rgb = the hit voxel's integer coordinate, a = the face's identity number
//
// -----------------------------------------------------------------------------
// THE LAST TWO ARE WHAT MAKE THE LIGHTING PER-FACE
// -----------------------------------------------------------------------------
// The indirect term in this renderer is computed and stored PER VOXEL FACE, not per screen pixel.
// Two targets carry what that needs, and they are deliberately different in kind:
//
//   gFace is GEOMETRY -- where to put the gather ray. It is camera-independent and identical for
//   every pixel that lands on the same face, which is the whole point: every screen probe on a
//   face gathers from one stable world origin, so the probe grid can be half as dense and the
//   result does not swim as the camera moves. Small float error here is harmless; it is a ray
//   origin, and the ray is metres long.
//
//   gKey is IDENTITY -- which face this is. Float error here is not harmless at all: it decides
//   whether a pixel may reuse a converged value, and one wrong bit throws the value away.
//
// So the key comes from `hit.voxelCoord`, the exact integer the DDA held throughout the march,
// and NOT from the world-space box. Recovering an integer from `foundBox.position` means undoing
// the chunk's rotation and translation in float32, which loses low bits in proportion to the
// chunk's distance from the origin and then TRUNCATES -- a single ULP below an exact integer
// drops the coordinate a whole cell. Measured over a chunk's voxels: 6% wrong for a small
// translation, 10% for a rotation alone, 39% for both. That is the difference between a face key
// that is stable and one that changes when you look at it. See SceneIntersectData::voxelCoord.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>

uniform vec4 passTargetRes;   // Engine-set: (w, h, 1/w, 1/h) of THIS pass's target.
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;

#define PI 3.14159265358979
#define FOV 60.0

#include <pjv_sun_sky.sc>
#include <pjv_face_key.sc>

// Where the shadow ray leaves the surface, in edge lengths of the voxel it starts on. A fixed world
// epsilon does not survive the range of scenes this opens: scenes sit at coordinates in the
// thousands, where a float32 ULP is around a thousandth of a unit, so an offset small enough not to
// skip a real occluder near the origin is below the noise floor out there and the ray self-hits.
// Scaling by the voxel keeps it meaningful at both ends. Same reasoning, and the same number, as the
// scene editor's shadow and bounce rays.
#define SHADOW_ORIGIN_BIAS_VOXELS 0.25

// Is the sun visible from p (a surface facing n, one voxel of it `voxelSize` across)? One ray, aimed
// at a random point on the sun's DISK rather than at its centre, so the shadow's edge comes out as
// wide as the sun is instead of one pixel wide. One sample per pixel per frame is noisy on its own
// and is meant to be: taa.frag averages 64 frames on a settled camera, which resolves the penumbra,
// and only penumbra pixels differ between samples in the first place. Set the sun's angular radius
// (sunDir.w) near zero to get the hard shadow this pass used to cast.
bool sunVisible(vec3 p, vec3 n, float voxelSize, inout uint seed) {
    Ray shadow;
    shadow.origin = p + n * (SHADOW_ORIGIN_BIAS_VOXELS * voxelSize);
    shadow.direction = pjvSampleSunCone(seed);
    // Below the horizon of this surface: no trace needed, and tracing one would answer a question
    // about the wrong hemisphere.
    if (dot(shadow.direction, n) <= 0.0) return false;
    RayQuery rq;
    rq.maxRaySteps = 128u;
    // Finest LOD for the whole ray. A coarse-LOD shadow ray tests against merged boxes that
    // over-occlude, which shows up as false shadow -- so the cost is cut with the step budget
    // instead, which errs the safe way: a ray that runs out of steps reports a miss, i.e. lit.
    rq.startLOD = 0u; rq.finishLOD = 0u; rq.distanceToFinishLOD = 100000u;
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
    vec2 uvJit  = v_texcoord0 + jitter / passTargetRes.xy;

    Ray ray;
    ray.origin = cameraPos.xyz;
    ray.direction = rayStartDirection(uvJit, passTargetRes.xy, cameraPos.xyz,
                                      normalize(cameraDir.xyz), FOV);

    // Primary ray at the finest LOD everywhere -- a distance LOD ramp collapses far
    // geometry into big coarse blocks (the "LOD too coarse" look). GI is screen-space so
    // only this primary + the sun shadow touch the voxel tree; keep the visible surface crisp.
    RayQuery rq;
    rq.maxRaySteps         = 512u;
    rq.startLOD            = 0u;
    rq.finishLOD           = 0u;
    rq.distanceToFinishLOD = 100000u;

    SceneIntersectData hit = raySceneIntersect(ray, rq);
    // Trust the march's own hit data rather than re-intersecting foundBox analytically: a fresh slab
    // test disagrees with the march by ULPs on a boundary-exact hit and misclassifies it.
    vec3 n = hit.normal;

    // Miss / degenerate boundary hit -> sky background.
    if (hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(n, n) < 0.5) {
        vec3 sky = skyColor(ray.direction);
        gl_FragData[0] = vec4(ray.origin + ray.direction * 1e5, -1.0); // a<0 => sky
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);                     // no surface, no voxel size
        gl_FragData[2] = vec4(0.0);
        gl_FragData[3] = vec4(sky, 0.0);
        gl_FragData[4] = vec4(0.0);
        // A key no real face can hold, so nothing ever matches against the background. NOT zero:
        // zero is voxel (0,0,0) face 0, which is a real face in every scene with a chunk at the
        // origin, and the sky would then share its converged lighting.
        gl_FragData[5] = vec4(FACE_KEY_NONE, FACE_KEY_NONE, FACE_KEY_NONE, FACE_KEY_NONE);
        return;
    }
    n = normalize(n);

    vec3 P = ray.origin + ray.direction * hit.rayT;

    // The whole material, off the march's own hit: two texelFetches against the material list index
    // and palette offset the DDA carried out, rather than a chunk-header fetch plus a root-to-leaf
    // descent to rediscover the leaf it was already standing in. It is also the only lookup that
    // stays correct once a chunk has been moved or rotated -- rebuilding an integer voxel coordinate
    // from the hit's world-space box is a float32 round trip that shades a growing fraction of
    // voxels with their neighbour's colour. See SceneIntersectData::materialListIndex.
    VoxelMaterial material = fetchVoxelMaterialFromHit(hit);
    // A metal has no diffuse lobe -- what it shows is its reflection, tinted by its own albedo -- so
    // leaving the albedo in would draw a chrome voxel as a bright grey one. This renderer has no
    // mirror reflection to put back in its place (compose's rough specular reads the same cascade),
    // so a metal reads as dark and reflective rather than as bright and flat. A default material has
    // metallic 0 and is untouched.
    vec3 albedo = material.albedo * (1.0 - material.metallic);

    // Per pixel and per frame, for the sun-cone sample below. The pixel coordinate is rebuilt from
    // the UNJITTERED uv rather than from gl_FragCoord, which bgfx does not expose to a fragment
    // shader on the SPIR-V path.
    ivec2 pixel = ivec2(v_texcoord0 * passTargetRes.xy);
    uint seed = pjvHashSeed(uvec3(uint(pixel.x), uint(pixel.y), uint(frame)));

    // Direct sun, soft-shadowed against the sun disk. Kept out of the GI's temporal accumulation on
    // purpose: this is the crisp half of the image and compose adds it back after the indirect term
    // has been denoised.
    vec3 direct = vec3(0.0);
    float NdotL = dot(n, SUN_DIR);
    if (NdotL > 0.0 && sunVisible(P, n, hit.foundBox.size, seed)) {
        direct = (albedo / PI) * NdotL * SUN_COLOR;
    }
    // An emitter's own glow is not scaled by its albedo and does not care where the sun is, so it is
    // added here rather than folded into the term above. The cascade gather picks the same emission
    // up from the other side (see pjv_cascade_ws.sc), which is what spreads an emissive voxel's
    // light onto its neighbours instead of leaving it a bright patch in an unlit room.
    direct += material.emission;

    float camDist = dot(P - cameraPos.xyz, normalize(cameraDir.xyz));

    // ---- The per-face anchor and the per-face identity -------------------------------------
    // The face centre, from the march's world-space box. A position, so the float32 round trip
    // that ruins an integer key is irrelevant at this magnitude -- half a ULP on a ray origin that
    // is about to travel several metres. It is camera-independent and identical for every pixel
    // on this face, which is exactly the property the cascade gather wants.
    float voxelSize = hit.foundBox.size;
    vec3  faceCentre = hit.foundBox.position + voxelSize * 0.5 + n * (voxelSize * 0.5);

    gl_FragData[0] = vec4(P, camDist);
    // The voxel's edge length travels with the normal: it is what gives every world-space offset
    // downstream a scene-independent unit, the same way the scene editor's G-buffer carries it.
    gl_FragData[1] = vec4(n, voxelSize);
    gl_FragData[2] = vec4(albedo, 1.0);
    gl_FragData[3] = vec4(direct, 1.0);
    gl_FragData[4] = vec4(faceCentre, voxelSize);
    gl_FragData[5] = pjvFaceKey(hit.voxelCoord, hit.headerIndex, n);
}
