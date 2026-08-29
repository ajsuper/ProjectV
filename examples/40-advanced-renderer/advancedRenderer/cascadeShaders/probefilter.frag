$input v_color0
$input v_texcoord0

// =============================================================================
// probefilter.frag  --  Pass 3. Edge-aware spatial blur of the probe buffer.
//
// One a-trous step over probeR, before resolve upsamples it and before accumulate averages it in
// time. What it buys is entirely for the pixels the temporal mean cannot yet help:
//
//   * A DISOCCLUDED face has no history at all -- age 1 -- so what it shows is one frame of an
//     8-ray Monte Carlo estimate. Its neighbours, which have been on screen all along, are converged.
//     Borrowing from them is the difference between an edge that resolves over a second and one that
//     resolves immediately.
//   * A SILHOUETTE pixel flips between two faces as the sub-pixel jitter moves it, and the two faces
//     hold genuinely different values. Softening the step between neighbouring faces is what stops
//     that flip reading as a crawling fringe. (Turning the jitter off with E removes the flicker by
//     removing the flipping; this removes it by removing the step.)
//   * THE UPSCALER magnifies whatever discontinuity it is handed. A GI term that steps hard from one
//     face to the next aliases when resampled to output resolution, which is why voxel edges stay
//     crisp -- they are geometry -- while the GI across them does not.
//
// -----------------------------------------------------------------------------
// WHY IN PROBE SPACE, AND WHY THAT IS THE WORLD-SPACE BLUR IT LOOKS LIKE
// -----------------------------------------------------------------------------
// Filtering here rather than at screen resolution is 16x cheaper, and it is also the more correct
// place. A probe texel IS a voxel face -- the quantity is per face, not per pixel -- so averaging
// neighbouring probe texels averages neighbouring FACES. That is a blur between probes over the
// surface, which is the thing worth doing; the same kernel run at screen resolution would spend most
// of its taps averaging a face with itself.
//
// -----------------------------------------------------------------------------
// AND WHY IT DOES NOT LEAK: TWO GUARDS, WHICH CATCH DIFFERENT THINGS
// -----------------------------------------------------------------------------
// Leaking is the whole risk of a spatial GI filter. There are two guards here and it is worth being
// exact about which failure each one covers, because they are not interchangeable.
//
// GEOMETRIC (geomWeight, shared with resolve and the old cascade merge). Every tap is weighted by
// the surface it stands on: same facing, and on this one's tangent plane. The kernel therefore
// follows a wall and stops at its corner -- a lit floor cannot bleed onto the shaded wall it meets,
// because the two fail the facing test outright, and a surface behind a silhouette fails the plane
// test. This is nearly free and it catches the large majority of leaks.
//
// VISIBILITY (a real ray between the two probes). What the geometric test cannot see is two patches
// that pass every heuristic and still have something BETWEEN them: the two sides of a thin wall are
// coplanar-ish, same-facing, at the same depth, and a metre apart through solid rock. A voxel scene
// is full of one-voxel dividers, so this is not a corner case here. A short ray from one face centre
// to the other answers it exactly, and no amount of tuning the heuristic can.
//
// What visibility does NOT fix, and it is worth saying so: two genuinely coplanar faces with
// different light -- one inside a shadow, one outside. Those ARE mutually visible; the ray comes
// back clear and the filter averages them, softening the shadow's edge in the indirect term over the
// kernel width. That is a real cost and it is the right trade, because indirect light is low
// frequency, the error is bounded by the kernel, and the alternative is per-face noise that the eye
// reads as the light itself moving.
//
// Inputs (FBO 1 [0..5] then FBO 2 [6]):
//   0 gPos  1 gNormal  2 gAlbedo  3 gDirect  4 gFace  5 gKey  6 probeR
// Output (FBO 3): same layout as probeR -- rgb = filtered R, a = the validity flag, passed through.
// =============================================================================

#include <bgfx_shader.sh>

// Cast a real occlusion ray between probes rather than trusting the geometry heuristic. See the
// header for what this catches that the heuristic cannot.
//
// 0 falls back to heuristic-only, which is most of the quality for none of the rays -- the knob to
// reach for first if this pass costs too much. It also drops the DDA include entirely, so the
// heuristic-only build is a genuinely small shader rather than a large one with dead code in it.
#ifndef FILTER_VISIBILITY
#define FILTER_VISIBILITY 1
#endif

SAMPLER2D(gPos,    0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gKey,    5);
SAMPLER2D(probeR,  6);

#if FILTER_VISIBILITY
#include <pjv_utils_DDA.sc>
#endif

// Writes AND reads the probe grid, so both are this pass's own target size.
#define CASCADE_SCREEN_RES pjvResOr(passInputRes[0].xy, passTargetRes.xy)
#define CASCADE_ATLAS_RES  passTargetRes.xy
#include <pjv_cascade_common.sc>
#include <pjv_face_key.sc>
#if FILTER_VISIBILITY
#endif

// Kernel half-width in probe texels, and the tap spacing. The a-trous trick is that a wider stride
// covers more ground for the same tap count, at the price of ringing when the signal has detail --
// indirect light has none worth preserving, so the stride is affordable and doubles the reach.
//
// 2 (a 5x5) with stride 2 reaches 8 probe texels, which is 32 screen pixels at quarter resolution --
// wide enough to bridge a freshly disoccluded sliver from converged neighbours on both sides. It
// drops to a 3x3 when the visibility rays are on, purely for their cost: a ray per tap at 24 taps
// comes to three times the gather itself, where at 8 taps it is roughly the gather's own ray count.
#if FILTER_VISIBILITY
#define FILTER_RADIUS 1
#else
#define FILTER_RADIUS 2
#endif
#define FILTER_STRIDE 2

// Step budget for one inter-probe ray. These are SHORT -- the kernel only reaches a few voxels -- so
// this bounds a ray that would otherwise wander off through a gap and cost as much as a gather ray.
// Running out of steps reports "visible", which errs toward filtering rather than toward switching
// the filter off, and is the safe direction: the failure is a slightly soft edge, not a hard one.
#define FILTER_VIS_STEPS 24u
#if FILTER_VISIBILITY
// Is the straight line from one probe's face to another's clear of solid geometry?
//
// Both endpoints sit ON surfaces, so the two ends need handling or every test fails: the origin is
// lifted half a voxel along its own normal, and a hit within a voxel of the far end is taken to BE
// the far end rather than an occluder.
bool probesVisible(vec3 a, vec3 na, vec3 b, float voxelSize) {
    vec3  d    = b - a;
    float dist = length(d);
    if (dist < 1e-5) return true;

    Ray ray;
    ray.origin    = a + na * max(0.5 * voxelSize, NORMAL_BIAS);
    ray.direction = d / dist;

    RayQuery rq = pjvPrimaryQuery(100u);
    rq.maxRaySteps = FILTER_VIS_STEPS;
    rq.startLOD = 0u; rq.finishLOD = 0u; rq.distanceToFinishLOD = 100000u;

    // Anything nearer than this is the far probe's own surface, not something in the way.
    float blockBefore = dist - max(voxelSize, 1e-4);

    // ONE query, not a skip loop.
    //
    // This used to loop up to four times, raising tMin past each envelope voxel it landed on. That was
    // necessary while the prototype dilated the swept volume into the GEOMETRY tree and marked it with
    // a palette entry: without the skip, every blade in the scene put a false occluder between its two
    // neighbouring probes and switched the filter off exactly where it was most needed.
    //
    // The envelope is an adjacent structure now, and this query does not ask for animation -- so it
    // cannot see the envelope at all, and anything it hits is real geometry. There is nothing left to
    // skip, and a loop that can only run once is worth deleting rather than leaving as a shape that
    // implies otherwise.
    SceneIntersectData h = raySceneIntersectFrom(ray, rq, 0.0);
    if (h.rayT < 0.0 || h.foundBox.size < 0.0) return true;   // nothing in the way
    if (h.rayT >= blockBefore) return true;                   // that is the far probe itself
    return false;                                             // a real occluder
}
#endif

// Where a probe texel sampled the G-buffer. Must match probe.frag's own expression exactly -- see
// the same helper in resolve.frag.
vec2 probeAnchorUV(ivec2 probeTexel) {
    vec2 centre = (vec2(probeTexel) + 0.5) / CASCADE_ATLAS_RES;
    return pjvSnapToTexel(centre, CASCADE_SCREEN_RES);
}

void main() {
    ivec2 self    = ivec2(floor(v_texcoord0 * CASCADE_ATLAS_RES));
    ivec2 maxTexel = ivec2(CASCADE_ATLAS_RES) - 1;

    vec4 centre = texture2D(probeR, v_texcoord0);
    // This texel gathered nothing (it landed on sky). Pass the miss through untouched: writing a
    // blurred value here would invent indirect light on the background, and resolve reads the alpha
    // to know which taps are real.
    if (centre.a < 0.5) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    vec2  aUV = probeAnchorUV(self);
    vec4  gp  = texture2D(gPos, aUV);
    vec4  gn  = texture2D(gNormal, aUV);
    vec3  P   = gp.xyz;
    vec3  N   = normalize(gn.xyz);
    float voxelSize = gn.a;          // the unit the visibility ray's two end biases are measured in
    vec4  key = texture2D(gKey, aUV);

    // The centre tap is weighted 1 and every neighbour by how well its surface matches, so a texel
    // with no acceptable neighbour keeps its own value exactly rather than fading toward nothing.
    vec3  acc  = centre.rgb;
    float accW = 1.0;

    for (int dy = -FILTER_RADIUS; dy <= FILTER_RADIUS; dy++)
    for (int dx = -FILTER_RADIUS; dx <= FILTER_RADIUS; dx++) {
        if (dx == 0 && dy == 0) continue;
        ivec2 t = clamp(self + ivec2(dx, dy) * FILTER_STRIDE, ivec2(0, 0), maxTexel);
        vec2  tUV = (vec2(t) + 0.5) / CASCADE_ATLAS_RES;

        vec4 r = texture2D(probeR, tUV);
        if (r.a < 0.5) continue;                    // that texel is sky; it has nothing to lend

        vec2 taUV = probeAnchorUV(t);
        vec4 tgp  = texture2D(gPos, taUV);
        vec3 tN   = normalize(texture2D(gNormal, taUV).xyz);

        // Guard one: same surface, or no contribution. Cheap, and it rejects most bad taps outright
        // -- which is also what keeps guard two affordable, since a rejected tap never casts a ray.
        float w = geomWeight(P, N, tgp.xyz, tN, tgp.a);
        if (w <= 0.0) continue;

        // Guard two: and is there actually a clear line between them? See the header -- this is the
        // thin-wall case, where every heuristic agrees and a metre of solid rock sits in between.
        #if FILTER_VISIBILITY
        if (!probesVisible(P, N, tgp.xyz, max(voxelSize, 1e-4))) continue;
        #endif

        // Two taps on the SAME face are the same quantity estimated twice, so they may be averaged
        // with no reservation at all -- probe.frag gives each texel its own slice of the direction
        // sequence precisely so that they are independent estimates. Weighting those above merely
        // similar surfaces keeps a face's own value dominant in its own filtered result, so the
        // kernel denoises within a face far more than it blends across faces.
        if (pjvSameVoxel(texture2D(gKey, taUV), key)) w *= 4.0;

        // Gaussian falloff over the kernel, so the result does not depend on where the (arbitrary)
        // kernel edge happens to fall.
        float d2 = float(dx * dx + dy * dy);
        w *= exp(-d2 / (2.0 * float(FILTER_RADIUS * FILTER_RADIUS)));

        acc  += r.rgb * w;
        accW += w;
    }

    gl_FragData[0] = vec4(acc / accW, 1.0);
}
