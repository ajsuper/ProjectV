$input v_color0
$input v_texcoord0

// =============================================================================
// occlusion.frag  --  Pass 2 of the scene editor's viewport renderer.
//
// Ray-traced ambient occlusion, and only the raw estimate: one ray per pixel per
// frame, written to its own single-channel target for denoise.frag to filter and
// shade.frag to apply. Nothing here touches colour.
//
// Why this is its own pass rather than a branch inside shade.frag: an ambient
// occlusion term that is going to be spatially filtered has to exist as an image
// of its own first. Filtering it after it has been multiplied into the albedo
// would blur the albedo along with it, and the two voxels either side of a colour
// boundary are usually coplanar and at the same depth -- exactly the case every
// edge-stopping weight is designed to treat as the *same* surface. The colours
// would bleed across each other, which is the one thing this viewport exists not
// to do.
//
// One ray per pixel is only viable because of what happens downstream: three
// a-trous levels widen it spatially, and the accumulate pass then averages up to
// 64 frames of it. See denoise.frag for the spatial half and the README's
// occlusion section for the whole chain.
//
// Inputs (FBO 1): 0 = previewColor, 1 = previewNormal, 2 = previewPosition.
//                 The rays additionally read the scene itself, through the samplers
//                 pjv_utils_DDA.sc declares at slots 9-15; the engine binds those
//                 for every pass, so nothing in render.json changes for it.
// Output (FBO 5): occlusion.r -- 1.0 unoccluded, 0.0 fully enclosed.
// =============================================================================

#include <bgfx_shader.sh>

#include <pjv_utils_DDA.sc>

SAMPLER2D(previewColor,    0);
SAMPLER2D(previewNormal,   1);
SAMPLER2D(previewPosition, 2);

uniform vec4 windowRes;
// x = frame index, z = the frame the camera last moved on. The difference between them is
// how many frames the accumulate pass has already averaged, which is exactly the sample
// index this pass wants -- see the frameIndex note in main().
uniform vec4 frameCount;
// x = ambient occlusion (screen-space), y = normal shading, z = sun shadow, w = ray occlusion.
// Only .w is read here; the pass writes "unoccluded" and stops when it is off.
uniform vec4 renderSettings;

#define TAU 6.28318530718

// Rays per pixel per frame. One, and the whole design downstream is what pays for that: the
// spatial filter widens a single sample into an estimate over hundreds of neighbours, and the
// temporal mean then averages up to 64 frames of it. Raising this is the first thing to try if
// the image is not clean enough while flying, and it costs a scene ray per pixel to do it.
#define AO_RAY_COUNT 1

// How far a ray looks, in edge lengths of the voxel being shaded -- a scene-independent unit, so
// one constant serves a Minecraft import whose voxels are 1.0 across and a mesh voxelization
// whose voxels are 0.01 across. Eight times the screen-space estimator's 3-voxel disc, which is
// the point of this mode: 3 voxels finds the crease where two faces meet, 24 finds the wall an
// object is standing next to.
//
// Past that a ray counts as having escaped even if something stopped it. Occlusion is a local
// question -- "how enclosed is this point" -- and a ray long enough to cross a scene answers
// "how big is the scene", which darkens an open plain and a small room by the same amount.
#define AO_RAY_DISTANCE_VOXELS 24.0

// Where a ray starts, in edge lengths of the voxel being shaded. Identical in value and in
// reasoning to shade.frag's SHADOW_ORIGIN_BIAS_VOXELS -- see the long note there for why this
// cannot be a fixed world-space epsilon.
#define AO_RAY_ORIGIN_BIAS_VOXELS 0.25

// Step budget per ray. This is not what bounds the ray -- the distance test does that, and this
// only has to be large enough to reach AO_RAY_DISTANCE_VOXELS before running out. The march skips
// empty space through coarser nodes, so 48 covers 24 voxels with room to spare.
#define AO_RAY_MAX_STEPS 48u

// Full resolution, and this is not a place to save time. A coarser cap returns interior nodes as
// solid, and an interior node is 4 or 16 voxels across -- wider than the quarter-voxel the ray
// origin is lifted off the surface. Every ray would immediately hit the coarse node holding its
// own starting surface and the whole scene would render black.
#define AO_RAY_LOD 0

// Interleaved gradient noise (Jimenez 2014). One value per pixel, decorrelated from its
// neighbours. Same function shade.frag uses to rotate its tap disc.
float interleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// The R2 low-discrepancy sequence (Roberts 2018): two irrational multipliers, one fract, and a 2D
// point set that stratifies far better than a pair of hashes at the same cost. That matters more
// here than the sample count does -- the accumulate pass is averaging these across frames, and a
// sequence that clumps converges to a blotchy mean rather than a smooth one, however many frames
// it is given.
vec2 sampleR2(float index) {
    return fract(vec2(0.7548776662466927, 0.5698402909980532) * index);
}

// Tangent basis about a normal, returned as columns so `frame * v` takes a vector from tangent
// space (z up) to world. The +Z fallback for a straight-up normal is the same one
// rayStartDirection uses, for the same degenerate-cross-product reason.
mat3 tangentFrame(vec3 normal) {
    vec3 up = abs(normal.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    return mat3(tangent, bitangent, normal);
}

// Cosine-weighted over the hemisphere about the frame's normal, by Malley's method (a uniform
// point on the disc lifted to the hemisphere). Cosine-weighted rather than uniform because the
// cosine belongs in the estimator anyway -- ambient occlusion is the cosine-weighted fraction of
// the hemisphere that is blocked -- and sampling it out of the pdf means the estimator below is a
// plain average with no per-sample weight at all.
vec3 cosineHemisphereDirection(mat3 frame, vec2 u) {
    float radius = sqrt(u.x);
    float phi = TAU * u.y;
    return frame * vec3(radius * cos(phi), radius * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
}

void main() {
    vec4 geometry = texture2D(previewNormal,   v_texcoord0);
    vec4 surface  = texture2D(previewPosition, v_texcoord0);

    // Unoccluded is the identity this whole chain multiplies by, so it is what both the background
    // and the switched-off case write. That keeps the passes behind this one branch-free: they
    // filter and apply a buffer that is always meaningful, rather than testing the toggle again.
    if (renderSettings.w < 0.5 || surface.w < 0.5) {
        gl_FragColor = vec4(1.0, 0.0, 0.0, 0.0);
        return;
    }

    vec3 normal = normalize(geometry.xyz);
    float voxelSize = geometry.w;
    float maxDistance = AO_RAY_DISTANCE_VOXELS * voxelSize;
    mat3 frame = tangentFrame(normal);

    // Cranley-Patterson: one R2 sequence shared by every pixel, offset per pixel so neighbours
    // walk it from different places. Without the offset every pixel would cast the same direction
    // and the noise would be a fixed pattern locked to the geometry rather than something the
    // filter and the running mean can average away. The pixel coordinate is rebuilt from the UV
    // rather than read from gl_FragCoord, which bgfx does not expose to a fragment shader on the
    // HLSL/SPIR-V path.
    vec2 pixel = v_texcoord0 * windowRes.xy;
    vec2 pixelOffset = vec2(interleavedGradientNoise(pixel),
                            interleavedGradientNoise(pixel.yx + 41.7));

    // Frames since the camera last moved, which is the same count the accumulate pass divides by.
    // Walking the sequence from 0 on every camera move is what makes the 64 frames it averages a
    // stratified set rather than 64 arbitrary points, and it keeps the index small enough that
    // float32 still resolves the fract.
    float frameIndex = max(frameCount.x - frameCount.z, 0.0);

    RayQuery occlusionQuery;
    occlusionQuery.maxRaySteps = AO_RAY_MAX_STEPS;
    occlusionQuery.startLOD = AO_RAY_LOD;
    occlusionQuery.finishLOD = AO_RAY_LOD;
    occlusionQuery.distanceToFinishLOD = 10000;

    Ray occlusionRay;
    occlusionRay.origin = surface.xyz + normal * (AO_RAY_ORIGIN_BIAS_VOXELS * voxelSize);

    float occlusion = 0.0;
    for (int i = 0; i < AO_RAY_COUNT; i++) {
        vec2 u = fract(sampleR2(frameIndex * float(AO_RAY_COUNT) + float(i)) + pixelOffset);
        occlusionRay.direction = cosineHemisphereDirection(frame, u);

        SceneIntersectData hit = raySceneIntersect(occlusionRay, occlusionQuery);
        if (hit.foundBox.size < 0.0 || hit.rayT < 0.0 || hit.rayT >= maxDistance) {
            continue;   // Escaped, or stopped by something too far away to count.
        }

        // Linear in the hit distance: a surface pressed against another goes fully dark, one at
        // arm's length contributes almost nothing. Without this the test is binary and a distant
        // wall darkens exactly as much as a touching one, which is the specific thing that makes
        // it hard to tell how close two objects are.
        occlusion += 1.0 - hit.rayT / maxDistance;
    }

    // Written unattenuated -- the strength constant lives in shade.frag, where it is applied. A
    // filter is only entitled to smooth an estimate, not to scale it, and keeping the two apart
    // means the intensity can be changed without reasoning about the denoiser at all.
    gl_FragColor = vec4(clamp(1.0 - occlusion / float(AO_RAY_COUNT), 0.0, 1.0), 0.0, 0.0, 0.0);
}
