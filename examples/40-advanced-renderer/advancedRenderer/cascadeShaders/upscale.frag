$input v_color0
$input v_texcoord0

// =============================================================================
// upscale.frag  --  Reconstructs the render-resolution frame to OUTPUT resolution.
//
// Sits between compose and taa, and the position in the chain is the whole point of the pass
// existing separately at all.
//
// It used to live at the end of display.frag, after taa. That order cannot work with a jittered
// primary ray. This reconstruction reads the CURRENT frame's G-buffer to decide which voxel face each
// output pixel belongs to, and the jitter moves what the G-buffer holds at a given texel every frame,
// so the edges it rebuilds moved every frame too -- and being downstream of taa, nothing could average
// them. Measured on a static scene with the jitter on, the reconstruction contributed 0.0193 RMS of
// frame-to-frame change against 0.0074 for a plain bilinear magnify, and 0.0 with the jitter off.
//
// Reconstructing FIRST and accumulating afterwards fixes that by construction: taa now sees the
// rebuilt image and averages the jitter out of it, which is the arrangement every temporal upscaler
// uses and for exactly this reason. The jitter can then buy sub-pixel detail without the edges paying
// for it in stability.
//
// Input  (FBO 8 [0], FBO 1 [1..6]): 0 composeHDR at render resolution, then the G-buffer.
// Output (FBO 10): the same HDR at output resolution, still linear and ungraded -- display.frag does
// the tone map, after taa has had its say.
// =============================================================================

#include <bgfx_shader.sh>

// Slot order is declaration order across this pass's input framebuffers (render.json). FBO8 supplies
// composeHDR at 0, then FBO1's six G-buffer attachments take 1..6. gAlbedo (3), gDirect (4) and gKey
// (6) are bound but unread here and are left undeclared, as the cascades do.
SAMPLER2D(composedHDR, 0);
SAMPLER2D(gPos,        1);   // xyz world hit, a = camDist (a < 0 => sky)
SAMPLER2D(gNormal,     2);   // xyz face normal
SAMPLER2D(gFace,       5);   // xyz voxel FACE CENTRE, w = voxel edge size

// Engine-set: (w, h, 1/w, 1/h). The target is the output resolution; input 0 is the render-resolution
// frame being magnified. Comparing them is how this decides whether there is anything to reconstruct.
uniform vec4 passTargetRes;
uniform vec4 passInputRes[8];
// The camera the G-buffer was rendered with, so the ray rebuilt here and the ray that filled the
// G-buffer agree.
uniform vec4 cameraPos;
uniform vec4 cameraDir;
// x = debug view (P key). Only the upscale view is read here; see the block at the end of main().
uniform vec4 renderParams;
// x = bypass this pass's reconstruction (Q key) and point-sample the render-resolution frame instead.
uniform vec4 debugParams;

#define FOV 60.0

// Radius of the candidate search, in SOURCE texels. 1 is a centred 3x3.
//
// This was the 2x2 bilinear bracket, on the reasoning that "the face covering an output pixel is at
// most one source texel away from it". That holds for a face sampled at many texels, and fails for
// exactly the faces that need help: a face only one or two texels across is recorded at a single
// texel, and the output pixels it covers can sit outside the bracket that texel happens to fall in --
// so the face is never a candidate and the pixel drops to bilinear even though the geometry is right
// there and fully known.
//
// A face's whole quad is derivable from ONE sample of it (gFace gives the centre and the edge length,
// gNormal the plane), so a texel that saw a face can speak for the entire face. Widening the search is
// what lets it: the test below was always against the face, never against the texel, so a further tap
// costs a fetch and admits no error -- a face that does not cover this ray still returns zero.
#define SEARCH_RADIUS 1

// ---- WHEN DO TWO CANDIDATES DESCRIBE THE SAME SURFACE? ---------------------------------------
// Two things have to be caught here, and both were getting through.
//
// ONE VOXEL SEEN TWICE. A voxel several texels across is recorded by every texel that lands on it,
// often through different faces, and each recovers the centre as faceCentre - n*halfSide where
// faceCentre was built as centre + n*halfSide. That term is added and subtracted along a DIFFERENT
// AXIS in each case, so it does not cancel to the last bit. The residual is one float32 ULP of the
// world magnitude -- and at a world coordinate of 16384, exactly where a 16384-resolution scene sits,
// that is 0.00195. An absolute tolerance of 1e-3 therefore split one voxel into two, which is the
// precision trap gbuffer.frag's own header documents. The tolerance has to scale with the voxel.
//
// ONE SURFACE AT TWO LEVELS. The LOD boundary is dithered per pixel (see gbuffer.frag), so inside a
// transition band neighbouring texels legitimately disagree about level: one reports a 2S block, the
// next reports an S voxel INSIDE that block. Not two surfaces -- one surface at two granularities.
// Treated as distinct they are ruinous: a block plus the eight voxels within it is nine candidates
// competing for three layer slots, six are dropped, and the uncovered remainder falls to the bilinear
// fallback -- which paints a screen-aligned square the size of one source texel, a 4x4 block of output
// pixels at a quarter render scale, sitting exactly where a voxel should be.
//
// So the test is CONTAINMENT rather than equality, which covers both: identical centres trivially, and
// a finer voxel lying inside a coarser block. Two genuinely ADJACENT voxels sit a whole voxel apart
// against a threshold of 0.75 of one, so they stay separate -- which is the case that must keep working.
bool sameSurface(vec3 centreA, float sizeA, vec3 centreB, float sizeB) {
    float span   = 0.5  * max(sizeA, sizeB);   // half the larger cube: its containment radius
    float slack  = 0.25 * min(sizeA, sizeB);   // ULP headroom, on the smaller scale
    vec3  offset = abs(centreA - centreB);
    return all(lessThanEqual(offset, vec3(span + slack)));
}

// A grazing face's pixel footprint grows without bound as the view direction approaches the plane.
// Capped so a face seen edge-on antialiases over a few pixels instead of smearing across the screen.
#define MAX_FOOTPRINT_PIXELS 8.0

// Copied from pjv_utils_DDA.sc rather than included: this pass has no business with the voxel
// traversal, and including that header would pull in samplers this pass does not bind. It must stay
// EXACTLY the forward map gbuffer.frag used, or the face test below is testing a different ray than
// the one that produced the G-buffer.
vec3 upscaleRayDirection(vec2 uv, vec2 res, vec3 camPos, vec3 camDirection) {
    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float aspectRatio = res.x / res.y;
    float scale = tan(radians(FOV * 0.5));

    vec3 forward = normalize(camDirection);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));

    return normalize(right * (ndc.x * scale * aspectRatio) + up * (ndc.y * scale) + forward);
}

// The overlap of two intervals, as a fraction of the second one: a slab of half-width `half` whose
// centre sits `offset` away from the centre of a pixel footprint of width `width`.
//
// THIS IS THE WHOLE FIX for sub-pixel voxels. What was here before was the coverage of a HALF-PLANE
// at signed distance e, `clamp(0.5 + e/width, 0, 1)` -- exact for an edge with surface extending
// forever behind it, which is what a voxel face looks like only while it is much larger than a pixel.
// A face is a bounded square, and the two models diverge exactly where the reconstruction was falling
// apart: for a face a tenth of a pixel across with the ray through its centre, the half-plane form
// reports 0.5 (the boundary passes through the pixel centre, so half of it is "behind" the edge)
// where the true answer is 0.1. Sub-pixel geometry was being dilated by up to an order of magnitude,
// and since the jitter changes which sub-pixel face lands in a given source texel every frame, the
// dilated blob moved every frame -- shimmer, generated here, upstream of the only pass that could
// have averaged it.
//
// The slab form is exact at BOTH ends. offset=0, half>>width -> 1. offset=half (ray on the border)
// -> 0.5. half<<width -> 2*half/width, which goes to zero as the face does, like it should.
float slabOverlap(float offset, float halfWidth, float width) {
    float lo = max(offset - halfWidth, -0.5 * width);
    float hi = min(offset + halfWidth,  0.5 * width);
    return max(hi - lo, 0.0) / width;
}

// Where along the ray this source texel's voxel face is, and HOW MUCH of the output pixel it covers.
//
// Coverage is what turns the selector into an antialiaser, and it is the one kind of blending that is
// legitimate here. Blending two faces by their distance to a sample point fabricates a colour (the
// bilinear problem). Blending them by the FRACTION OF THE PIXEL EACH ACTUALLY COVERS is an area
// integral -- it is what the true value of that pixel is, and it converges to ground truth rather than
// away from it. Interiors are unaffected: a pixel well inside a face has coverage exactly 1 and stays
// perfectly crisp. Only the roughly one-pixel band along each edge blends at all.
//
// Without this the upscale is a pure classifier, and every edge it gets right is a hard staircase at
// the output grid while every edge it misses drops to the nearest texel and reads as a blocky step.
// Along a single silhouette you get both, and corners get both at once, which is exactly where it
// showed. TAA cannot rescue it either: TAA runs at source resolution, and this edge does not exist
// until the final pass, downstream of every temporal mechanism in the renderer.
float faceCoverage(vec3 faceCentre, vec3 n, float halfSide,
                   vec3 rayOrigin, vec3 rayDir, vec3 camRight, vec3 camUp, float theta,
                   out float outT) {
    outT = 1e30;

    float denom = dot(rayDir, n);
    if (abs(denom) < 1e-6) return 0.0;                  // edge-on: no coverage worth claiming
    float t = dot(faceCentre - rayOrigin, n) / denom;
    if (t <= 0.0) return 0.0;

    vec3 d = (rayOrigin + rayDir * t) - faceCentre;

    // The face's own two in-plane axes. A voxel face is axis-aligned in its chunk's space, and an
    // unrotated chunk keeps that in world space -- the case here -- so the two world axes on which n
    // is zero ARE the face's axes and the extents below are exact. The branch picks the smallest
    // component of |n| first, which for an axis-aligned normal is always one of those two.
    //
    // A ROTATED chunk's face is still a square but its in-plane axes cannot be recovered from the
    // G-buffer. It then gets an arbitrary basis and is treated as if aligned to it, which is the same
    // approximation the old circumscribed disc was making, minus the isotropy.
    vec3 an = abs(n);
    vec3 tangent = (an.x <= an.y && an.x <= an.z) ? vec3(1.0, 0.0, 0.0)
                 : (an.y <= an.z)                 ? vec3(0.0, 1.0, 0.0)
                                                  : vec3(0.0, 0.0, 1.0);
    tangent = normalize(tangent - n * dot(tangent, n));
    vec3 bitangent = cross(n, tangent);

    // The output pixel's footprint ON THIS PLANE, as the two world vectors one pixel of screen-x and
    // screen-y map to. Derived rather than fudged: perturbing the ray direction by a small e displaces
    // the plane intersection by t * (e - rayDir * dot(e, n) / denom), which is the exact Jacobian of
    // the ray/plane solve.
    //
    // What this buys over the `pixelWorld / max(abs(denom), 0.15)` it replaces is ANISOTROPY. An
    // oblique face stretches a pixel along one in-plane direction and leaves the perpendicular one
    // alone; the old isotropic inflation widened the antialiasing band across the sharp axis too, so
    // every steeply-viewed face -- half of them, in a voxel scene -- was softer along its crisp
    // direction than it had any reason to be.
    vec3 fx = t * theta * (camRight - rayDir * (dot(camRight, n) / denom));
    vec3 fy = t * theta * (camUp    - rayDir * (dot(camUp,    n) / denom));

    // Extent of that (sheared) footprint along each face axis, as a box: the separable bound, which is
    // what makes the two 1-D overlaps below multiply into an area.
    float cap = MAX_FOOTPRINT_PIXELS * t * theta;
    float wu = clamp(abs(dot(fx, tangent))   + abs(dot(fy, tangent)),   1e-6, cap);
    float wv = clamp(abs(dot(fx, bitangent)) + abs(dot(fy, bitangent)), 1e-6, cap);

    // Area of intersection of the face's square with the pixel's footprint, over the footprint's area.
    // Separable because both are axis-aligned in this basis. A corner now correctly reports the
    // PRODUCT of its two edge coverages (0.25 for a pixel centred on a corner) where taking the
    // nearer edge alone reported 0.5 and left corners over-covered.
    outT = t;
    return slabOverlap(abs(dot(d, tangent)),   halfSide, wu) *
           slabOverlap(abs(dot(d, bitangent)), halfSide, wv);
}

// ---- THE UNIT IS THE VOXEL, NOT THE FACE ------------------------------------------------------
// gFace carries the face centre AND the voxel's edge length, and gNormal the face normal, so the
// solid the face belongs to is fully determined: its centre is one half-edge back along the normal. A
// texel that recorded a voxel's top face has also told us where its sides are.
//
// Which means the right question is not "how much of the pixel does THIS FACE cover" but "how much
// does THIS VOXEL cover", and the second is answerable from a single texel. A convex box shows
// exactly the faces whose outward normal opposes the ray -- at most three -- and a ray that enters the
// box passes through exactly ONE of them. Their coverages are therefore disjoint, and they sum to the
// coverage of the whole silhouette.
//
// That sum is what fixes voxel corners. Three faces meeting at a corner each covered a fraction of the
// pixel; with only two layers the third had nowhere to go and its share fell to bilinear, leaving a
// red dot at every corner even after the edges came good. Asked as one question about one voxel, the
// three fractions add up to the silhouette's coverage -- frequently a full 1.0 -- and the corner is
// reconstructed whole, out of a single texel, with no extra layer needed.
float voxelSilhouetteCoverage(vec3 voxelCentre, float halfSide,
                              vec3 rayOrigin, vec3 rayDir, vec3 camRight, vec3 camUp, float theta,
                              out float outT) {
    outT = 1e30;
    float total = 0.0;

    for (int axis = 0; axis < 3; axis++) {
        vec3 a = axis == 0 ? vec3(1.0, 0.0, 0.0)
               : axis == 1 ? vec3(0.0, 1.0, 0.0)
                           : vec3(0.0, 0.0, 1.0);
        // The face on this axis that can be seen: the one whose outward normal opposes the ray.
        vec3 n = dot(rayDir, a) > 0.0 ? -a : a;
        float t;
        float cov = faceCoverage(voxelCentre + n * halfSide, n, halfSide,
                                 rayOrigin, rayDir, camRight, camUp, theta, t);
        if (cov > 0.0) {
            total += cov;
            outT = min(outT, t);     // hit faces only, so this is the entry distance
        }
    }
    // Clamped because the three are disjoint in exact arithmetic but the footprint boxes that
    // approximate them are separable bounds, which can overlap slightly at a corner.
    return min(total, 1.0);
}

// Everything one source texel tells us about the voxel it saw: where that voxel is, how much of this
// output pixel it covers, and how much of the pixel is covered by the ONE face this texel actually
// recorded -- which is the weight its colour deserves when several texels describe the same voxel.
bool voxelSample(vec2 srcUV, vec3 rayOrigin, vec3 rayDir, vec3 camRight, vec3 camUp, float theta,
                 out float outT, out float outCoverage, out vec3 outVoxelCentre,
                 out float outRecordedCoverage, out float outVoxelSize) {
    outT = 1e30;
    outCoverage = 0.0;
    outVoxelCentre = vec3(0.0);
    outRecordedCoverage = 0.0;
    outVoxelSize = 0.0;
    if (texture2D(gPos, srcUV).a < 0.0) return false;   // sky: no voxel to be inside of

    vec4  face = texture2D(gFace, srcUV);
    vec3  n    = texture2D(gNormal, srcUV).xyz;
    float halfSide = face.w * 0.5;
    if (halfSide <= 0.0) return false;

    outVoxelCentre = face.xyz - n * halfSide;
    outVoxelSize = face.w;

    // The recorded face's own coverage. Not used for geometry any more -- the silhouette below covers
    // that -- but it is exactly the right weight for this texel's COLOUR, because it is the share of
    // the pixel this texel's shading actually describes. A texel whose face the ray misses entirely
    // contributes its colour with almost no weight, and one squarely on the dominant face dominates.
    float tRecorded;
    outRecordedCoverage = faceCoverage(face.xyz, n, halfSide,
                                       rayOrigin, rayDir, camRight, camUp, theta, tRecorded);

    outCoverage = voxelSilhouetteCoverage(outVoxelCentre, halfSide,
                                          rayOrigin, rayDir, camRight, camUp, theta, outT);
    return outCoverage > 0.0;
}

// ---- WHERE THE OUTPUT PIXEL SITS IN THE SOURCE GRID, AS A WEIGHT --------------------------------
// The standard bilinear tent, in source-texel units: 1 at a texel's own centre, falling to 0 one texel
// away on each axis. Used ONLY to weight colour between texels already established to describe the
// same surface, which is what makes it safe here -- see the note at cw in main().
//
// A tent rather than a Gaussian because it is exactly bilinear, so at 1:1, where each output pixel
// lands on a texel centre, it returns 1 for that texel and 0 for every other and this whole mechanism
// degenerates to the plain fetch the full-resolution path already had. Nothing at scale 1.0 changes.
float texelTent(vec2 tapCentre, vec2 srcPos) {
    vec2 d = abs(tapCentre - srcPos);
    return max(0.0, 1.0 - d.x) * max(0.0, 1.0 - d.y);
}

void main() {
    // RUNS AT EVERY SCALE, including 1.0, and that is not an oversight.
    //
    // This does two separable things: it reconstructs WHICH face an output pixel belongs to (only
    // meaningful when magnifying), and HOW MUCH of the pixel that face covers (meaningful always). At
    // 1.0 the first is trivial and the second is plain analytic antialiasing of the voxel edges, which
    // this renderer otherwise gets only from temporal accumulation of the sub-pixel jitter.

    // ---- BYPASS (Q key): WHAT THIS PASS IS WORTH ------------------------------------------------
    // The same frame reaching the output grid with NO reconstruction: every pass upstream produced
    // exactly what it produces normally, and each output pixel takes the one render texel it falls in.
    // The comparison that supports is the honest one -- NOT against full resolution, which would be a
    // different amount of work, but against the identical samples magnified for free.
    //
    // POINT SAMPLED, so what comes out is the render-resolution frame itself, blocky, with no filter
    // of any kind between it and the screen. Not bilinear: a bilinear magnify is already a
    // reconstruction of a sort, and comparing against it understates what this pass does by exactly
    // the amount that filter was contributing. The honest baseline for "what did the reconstruction
    // buy" is the raw samples.
    //
    // Point sampling HERE is necessary and not sufficient, because taa.frag is downstream and is also
    // a filter: its running mean integrates the sub-pixel jitter and its moving path resamples with a
    // Catmull-Rom built from bilinear fetches, which together turn these blocks back into a smoothly
    // magnified image. It reads the same debugParams.x and takes its own early-out, so the two are off
    // together; see the note at that early-out for what that costs the frame-time half of the A/B.
    //
    // A real early-out rather than a colour substituted at the end, so the milliseconds move too.
    // Alpha goes out as 1 because the purity channel is part of what is being switched off, and it is
    // what taa's face gate reads -- moot while taa is bypassed too, and correct the moment it is not.
    if (debugParams.x > 0.5) {
        vec2 rawUV = (floor(v_texcoord0 * passInputRes[0].xy) + 0.5) * passInputRes[0].zw;
        gl_FragData[0] = vec4(texture2D(composedHDR, rawUV).rgb, 1.0);
        return;
    }

    vec3 hdr;
    // How much of this pixel the front-most face covers -- 1 for a pixel wholly inside one face, less
    // for anything mixed. Written to alpha for taa.frag, which cannot derive it: see the note at the
    // output below.
    float purity;
    // How much of the pixel came from reconstructed faces rather than the bilinear fallback. Distinct
    // from purity: this counts BOTH layers, because both are reconstruction. See the composite below.
    float reconstructed;
    {
    vec2 srcRes   = passInputRes[0].xy;
    vec2 srcTexel = passInputRes[0].zw;

    // The nearest source texel. Only the seed for uv1/uv2 now -- the fallback colour for unclaimed
    // area is `rest` below, which is centred rather than snapped.
    vec2 nearestUV = (floor(v_texcoord0 * srcRes) + 0.5) * srcTexel;

    // The search is CENTRED on the nearest texel rather than being the bilinear 2x2 bracket. The
    // bracket is asymmetric by construction (it leans toward +x/+y), which biased which faces could
    // ever become candidates; a centred window treats every direction alike. See SEARCH_RADIUS.
    vec2 centreTexel = floor(v_texcoord0 * srcRes);
    // This pixel's position in source-texel units, unsnapped. centreTexel is its floor; the fractional
    // part is what the tent below needs and what nothing in this pass used to look at.
    vec2 srcPos = v_texcoord0 * srcRes;
    vec3 rayDir = upscaleRayDirection(v_texcoord0, passTargetRes.xy, cameraPos.xyz, cameraDir.xyz);

    // The camera basis and the angular size of one OUTPUT pixel, for the footprint Jacobian in
    // faceSample. Same construction as upscaleRayDirection, so the two agree by inspection. theta is
    // isotropic: the horizontal ndc range carries the aspect ratio, which cancels against the wider
    // pixel count, leaving 2*tan(fov/2)/height per pixel on both axes.
    vec3 forward  = normalize(cameraDir.xyz);
    vec3 worldUp  = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 camRight = normalize(cross(forward, worldUp));
    vec3 camUp    = normalize(cross(camRight, forward));
    float theta   = (2.0 * tan(radians(FOV * 0.5))) / passTargetRes.y;

    // The two front-most faces with any coverage, kept in depth order. Two layers is enough for
    // the cases that matter and for the one the corners are made of: a silhouette is one face over
    // whatever is behind it, and a voxel corner is two faces of the same voxel partitioning the
    // pixel between them. A third layer would need a full sort for a contribution that is already
    // below a quantisation step -- and once geometry is sub-pixel the deeper layers are better
    // served by the box average in `rest`, which is all four texels at once and unordered.
    // Three layers, not two, and each is a VOXEL rather than a face. Three because that is what a
    // corner of the voxel lattice needs: where three voxels meet, two layers leave a third of the
    // pixel to the bilinear fallback however exact the coverage is.
    //
    // Colour is accumulated rather than fetched once at the end. Several texels can describe the same
    // voxel from different faces, and each deserves a say in proportion to how much of the pixel its
    // own recorded face covers -- so a layer carries a running sum of colour*weight and its weight,
    // and divides at the end. That is what gives a corner the correct blend of its three faces'
    // shading instead of whichever one happened to be found first.
    float t1 = 1e30, a1 = 0.0; vec3 c1 = vec3(0.0); float w1c = 0.0; vec3 v1 = vec3(0.0); float s1 = 0.0;
    float t2 = 1e30, a2 = 0.0; vec3 c2 = vec3(0.0); float w2c = 0.0; vec3 v2 = vec3(0.0); float s2 = 0.0;
    float t3 = 1e30, a3 = 0.0; vec3 c3 = vec3(0.0); float w3c = 0.0; vec3 v3 = vec3(0.0); float s3 = 0.0;

    // ---- THE SKY IS A LAYER TOO, AND LEAVING IT OUT IS WHAT MADE CLOSE EDGES JAGGED --------------
    // A silhouette against the sky is the case this pass should be BEST at: one voxel of known extent
    // over a background of known colour, so the pixel is exactly cov*voxel + (1-cov)*sky and cov is
    // computed analytically. It was the worst instead.
    //
    // Sky texels are not voxels, so they were rejected outright and never became candidates. The part
    // of the pixel the voxel did not claim therefore fell through to `rest` -- a BILINEAR fetch of the
    // composed frame, which along a silhouette is a blend of voxel and sky texels mixed at SOURCE
    // resolution. So an edge whose coverage was computed exactly at output resolution was then
    // composited against a background carrying the source grid's own staircase, and the staircase came
    // back at source scale. Exactly the "slight jaggedness up close" that should not be possible from
    // an exact edge.
    //
    // Point-sampled and averaged over whichever texels in the window are sky. The sky is smooth, so
    // its neighbours are an excellent estimate of it here, and unlike the bilinear fetch not one of
    // them carries any of the voxel's colour.
    vec3  skySum   = vec3(0.0);
    float skyWeight = 0.0;

    for (int j = -SEARCH_RADIUS; j <= SEARCH_RADIUS; j++)
    for (int i = -SEARCH_RADIUS; i <= SEARCH_RADIUS; i++) {
        // Clamped because the window reaches outside the texture at the border, and the samplers wrap.
        vec2 candidateUV = clamp((centreTexel + vec2(float(i), float(j)) + 0.5) * srcTexel,
                                 srcTexel * 0.5, 1.0 - srcTexel * 0.5);
        // Sky first, and kept rather than discarded -- see skySum above.
        if (texture2D(gPos, candidateUV).a < 0.0) {
            // Tented for the same reason the surface layers are, and it is the same bug: a flat mean
            // over the window steps whenever the window does, so a sky GRADIENT behind a silhouette
            // came out as bands one source texel wide. Weighting cannot pull any voxel colour in here
            // -- only sky taps reach this branch -- so this is purely a smoother estimate of the same
            // quantity. Floored like cw so a lone distant sky tap still counts for something.
            float skyW = max(texelTent(centreTexel + vec2(float(i), float(j)) + 0.5, srcPos), 1e-6);
            skySum   += texture2D(composedHDR, candidateUV).rgb * skyW;
            skyWeight += skyW;
            continue;
        }

        float t, coverage, recorded, vsize; vec3 vc;
        if (!voxelSample(candidateUV, cameraPos.xyz, rayDir, camRight, camUp, theta,
                         t, coverage, vc, recorded, vsize))
            continue;

        // ---- COLOUR, AND THE WEIGHT IT EARNS ------------------------------------------------------
        // Two factors, and the second was missing. That omission is what made a soft shadow come out as
        // a COARSE GRID at the source resolution while the geometry around it stayed exact.
        //
        // WHY. `recorded` is the coverage of THIS ray by the face the tap recorded. For a voxel several
        // source texels across, every texel on it records the SAME face -- one flat square, one centre,
        // one normal -- so every one of them returns the same `recorded`, and the layer's colour came
        // out as the flat unweighted mean of whichever same-voxel texels the 3x3 window happened to
        // contain. The window is centred on floor(srcPos), so its membership changes only when the
        // output pixel crosses into the next source texel. The colour was therefore piecewise constant
        // per SOURCE TEXEL: a grid of flat blocks, one render pixel each.
        //
        // On geometry that is invisible, because what varies across a face at high frequency is the
        // face BOUNDARY, and that is carried by `coverage` at output resolution and was always right.
        // What is smooth across a face is the light -- and a blurred sun visibility is the smoothest
        // signal in the frame, so it is exactly where the quantisation shows. compose.frag's blur was
        // working correctly the whole time; this pass was resampling its output with a nearest filter.
        //
        // THE FIX is a bilinear tent on the tap's distance from the pixel, which makes the weights vary
        // continuously with sub-texel position, so the colour does too. This is a joint bilateral
        // upsample and not a betrayal of the pass's rule: the rule is that colour is never blended
        // ACROSS surfaces, and the layer bookkeeping above has already established that every tap
        // summed here describes the same voxel. Interpolating WITHIN one surface is what a smooth
        // shading signal on that surface actually wants. A silhouette still cannot bleed, because the
        // faces on either side of it are different layers and are combined by coverage alone.
        //
        // Floored, not clamped away: a voxel described by a single tap more than a texel off gets tent
        // 0, and since a layer normalises by its own total weight, a lone tap must still be able to
        // carry its colour. With one tap the floor cancels exactly; with several the tent dominates it.
        vec3  col = texture2D(composedHDR, candidateUV).rgb;
        float cw  = max(max(recorded, 1e-4) *
                        texelTent(centreTexel + vec2(float(i), float(j)) + 0.5, srcPos), 1e-6);

        // ---- A FACE ALREADY HELD MUST NOT TAKE THE OTHER SLOT TOO --------------------------------
        // This is the red line along every voxel boundary, and it is a bookkeeping bug rather than a
        // geometric one.
        //
        // A face wider than a texel is recorded by EVERY texel that lands on it, so the window
        // routinely returns the same face several times. Those duplicates used to be treated as fresh
        // candidates: the first took layer 1, the second took layer 2 -- and the two layers were then
        // the same face twice, so a genuinely different neighbouring face, arriving third, had nowhere
        // to go and was dropped. Adjacent faces are coplanar, so its `t` tied with the ones already
        // held and it lost every comparison.
        //
        // The pixel then composited one face over itself over `rest`, and the part of it that belonged
        // to the neighbour fell through to the bilinear fallback -- a soft, unreconstructed seam along
        // every boundary between two voxels, which is precisely where the reconstruction should be at
        // its most confident. Skipping duplicates is free and exact: the test is against the FACE, so
        // two samples of one face return identical t and coverage and the second carries no
        // information the first did not.
        //
        // Deduped on the VOXEL now rather than the face, which is what lets several texels describe
        // one voxel from different sides without any of them being thrown away: the geometry is taken
        // once (it is identical -- the silhouette is a property of the voxel, not of the texel that
        // reported it) and every texel still donates its colour at its own weight.
        // Compared at the SMALLER of the two voxels' scales, so a large merged block can never
        // swallow a small voxel that merely sits near its centre.
        // Merged on CONTAINMENT, so a block and the finer voxels inside it share one layer. Every
        // texel still donates its colour; the layer's GEOMETRY is taken from whichever description
        // covers this pixel best, which is the one that explains what is actually here.
        if (a1 > 0.0 && sameSurface(vc, vsize, v1, s1)) {
            c1 += col * cw; w1c += cw;
            if (coverage > a1) { a1 = coverage; t1 = t; v1 = vc; s1 = vsize; }
            continue;
        }
        if (a2 > 0.0 && sameSurface(vc, vsize, v2, s2)) {
            c2 += col * cw; w2c += cw;
            if (coverage > a2) { a2 = coverage; t2 = t; v2 = vc; s2 = vsize; }
            continue;
        }
        if (a3 > 0.0 && sameSurface(vc, vsize, v3, s3)) {
            c3 += col * cw; w3c += cw;
            if (coverage > a3) { a3 = coverage; t3 = t; v3 = vc; s3 = vsize; }
            continue;
        }

        if (t < t1) {
            t3 = t2; a3 = a2; c3 = c2; w3c = w2c; v3 = v2; s3 = s2;
            t2 = t1; a2 = a1; c2 = c1; w2c = w1c; v2 = v1; s2 = s1;
            t1 = t;  a1 = coverage; c1 = col * cw; w1c = cw; v1 = vc; s1 = vsize;
        } else if (t < t2) {
            t3 = t2; a3 = a2; c3 = c2; w3c = w2c; v3 = v2; s3 = s2;
            t2 = t;  a2 = coverage; c2 = col * cw; w2c = cw; v2 = vc; s2 = vsize;
        } else if (t < t3) {
            t3 = t;  a3 = coverage; c3 = col * cw; w3c = cw; v3 = vc; s3 = vsize;
        }
    }

    // ---- WHAT THE UNCLAIMED PART OF THE PIXEL IS -------------------------------------------------
    // Behind the covered layers, and it is NOT unconditionally the nearest texel any more.
    //
    // "Nearest" presumes the source grid resolves the geometry -- that a texel is a sample OF A FACE,
    // so the face nearest this pixel is the right thing to fall back to. Once a voxel is smaller than
    // a source texel that stops being true: each texel is a point sample of whichever of the many
    // voxels in its footprint the ray happened to hit, the four samples are independent draws from the
    // same distribution, and their MEAN is the estimate of what the pixel contains while any single
    // one of them is the aliased signal itself. Nearest was propagating the aliasing it was being
    // asked to remove.
    //
    // It also fixes an asymmetry at silhouettes against the sky. Sky texels are not candidates
    // (faceSample rejects them), so they could only reach the output through `rest` -- and for an
    // output pixel whose nearest texel is the surface, `rest` was that same surface, making the
    // composite front*a + surface*(1-a) == surface with no sky in it at all. Edges softened on the sky
    // side and stayed hard on the surface side. The box average always carries the sky.
    //
    // A PLAIN CENTRED BILINEAR, unconditionally, and the two things it is not are both things this
    // line has already been.
    //
    // It is not `nearest`. That was the original, and under magnification it is the whole "the
    // upscaler falls back to showing the raw render resolution" complaint: wherever the classifier
    // claims nothing, every output pixel inside a source texel gets that texel's exact colour and the
    // magnify degenerates to a blocky point sample.
    //
    // It is not a wider low-pass faded in on projected voxel size either, which is what replaced
    // nearest and had to come back out. Two things went wrong with it, both only visible once
    // magnifying. The fade was measured in SOURCE texels per voxel, so halving the render scale halves
    // the count and drops a large part of the scene below the threshold that was tuned at 1:1 -- the
    // band-limit went from a far-field correction to most of the image, which is the softness. Worse,
    // the fade was computed from gFace/gPos at nearestUV, which is JITTERED data: at 1:1 it saturates
    // and never moves, but under magnification much more of the image sits mid-fade, where it flips
    // frame to frame and the pixel alternates between two different filters. That is a temporal blend
    // factor driven by the very jitter this pass exists to resolve, and it belongs nowhere near here.
    //
    // Bilinear is the honest answer to "what is here, given the source grid": centred on the pixel, so
    // no half-texel shift; smooth under magnification, so no blocky fallback; and at 1:1 it lands on a
    // texel centre and degenerates to exactly `nearest`, so the full-resolution path that is currently
    // sharp and stable is bit-for-bit untouched. It is also stable by construction -- no G-buffer
    // value enters it, so nothing it does can flicker.
    //
    // It still carries the sky at a silhouette, which is the other thing nearest got wrong: sky texels
    // are not candidates (faceSample rejects them), so for an output pixel whose nearest texel was the
    // surface the composite came out `front*a + surface*(1-a) == surface`, with no sky in it at all --
    // edges softened on the sky side and stayed hard on the surface side.
    vec3 rest = texture2D(composedHDR, v_texcoord0).rgb;

    // Front to back. Each candidate fetch is at a texel CENTRE, so the sampler's own bilinear filter
    // degenerates to that exact texel and contributes nothing of its neighbours -- every blend
    // here is by coverage and by nothing else.
    //
    // Composited in scene-referred HDR rather than after the tone map: coverage is a statement
    // about how much of the pixel's SOLID ANGLE each surface occupies, and radiance is what is
    // linear in solid angle. Averaging tone-mapped values would darken every antialiased edge,
    // which is the classic mistake and is visible as a dark fringe around bright silhouettes.

    // ---- DO THE TWO LAYERS PARTITION THE PIXEL, OR DOES ONE OCCLUDE THE OTHER? -------------------
    // The old chain, mix(mix(rest, second, a2), front, a1), answers "occlude" unconditionally. That is
    // the correct composite for a silhouette -- a near face over whatever is behind it, where the two
    // coverages are independent and the second only shows through the (1-a1) the first leaves. It is
    // the wrong one for the far more common case.
    //
    // Adjacent faces -- two voxels side by side, or the top and side of one voxel meeting at its edge
    // -- do not overlap at all. They DIVIDE the pixel between them, and their coverages sum to 1 when
    // they fill it. Composited as though they overlapped, they leave (1-a1)(1-a2) of the pixel
    // unclaimed: two faces covering 0.75 and 0.25 of a pixel still handed 18.75% of it to the bilinear
    // fallback, at every voxel boundary in the frame. That is the same seam the duplicate-slot bug
    // above was widening, arriving by a second route.
    //
    // Which case applies is a question about DEPTH, and the geometry answers it cleanly: adjacent
    // faces sit within about a voxel of each other along the ray, while a silhouette drops away to
    // whatever is behind it. Faded rather than switched, so a shallow step does not snap between two
    // different composites.
    // A voxel, or a pixel out here if that is larger. t1 is clamped because with no layer found at all
    // it is still 1e30, and 1e30 * theta overflows to infinity -- smoothstep with infinite edges is a
    // NaN, which would propagate into a pixel that should simply have been all `rest`.
    float adjacency = max(max(s1, min(t1, 1e6) * theta), 1e-6);

    // Each layer behind the front is either dividing the pixel with it (adjacent voxels, near-equal
    // depth) or hidden behind it (a silhouette). Resolved per layer against the front's depth, and
    // faded rather than switched so a shallow step does not snap between two composites.
    float part2 = 1.0 - smoothstep(0.25 * adjacency, 1.5 * adjacency, abs(t2 - t1));
    float part3 = 1.0 - smoothstep(0.25 * adjacency, 1.5 * adjacency, abs(t3 - t1));

    // Front to back, tracking what is left. A partitioning layer takes as much of the remainder as it
    // can fill; an occluded one takes its own coverage OF that remainder.
    float w1   = a1;
    float rem1 = 1.0 - w1;
    float w2   = mix(rem1 * a2, min(a2, rem1), part2);
    float rem2 = max(0.0, rem1 - w2);
    float w3   = mix(rem2 * a3, min(a3, rem2), part3);
    float wRest = max(0.0, 1.0 - w1 - w2 - w3);

    // What fills the part of the pixel no voxel layer claimed. The sky when the window saw any, which
    // is the silhouette case and the one that has to be exact; otherwise the band-limited bilinear,
    // which is right where the remainder is unresolved geometry rather than background.
    vec3 background = skyWeight > 0.0 ? skySum / skyWeight : rest;

    // Each layer's colour is its coverage-weighted mean over every texel that described it.
    vec3 col1 = w1c > 0.0 ? c1 / w1c : background;
    vec3 col2 = w2c > 0.0 ? c2 / w2c : background;
    vec3 col3 = w3c > 0.0 ? c3 / w3c : background;

        hdr = w1 * col1 + w2 * col2 + w3 * col3 + wRest * background;

        // Fraction of the pixel that came from reconstructed geometry rather than from the bilinear
        // fallback -- what the debug view reports. With the two fixes above, a flat wall should read
        // as solid green rather than as a grid of seams.
        reconstructed = 1.0 - wRest;

        // Zero when no candidate covered this ray at all, and that is deliberate rather than a gap.
        // It is tempting to read "no coverage" as "no information" and report full purity so taa's
        // size test decides alone -- but a pixel the classifier cannot attribute to any face is one
        // whose G-buffer entry is a single arbitrary jittered texel, which is exactly the comparison
        // the gate must not trust. Reporting 0 keeps it switched off there.
        purity = a1;
    }

    // ALPHA CARRIES THE FRONT COVERAGE, and it is the reason this pass and taa.frag stop fighting.
    //
    // taa.frag validates history by comparing voxel FACE CENTRES, which is exact for a pixel that
    // sits wholly inside one face and meaningless for one that straddles two. At a silhouette the
    // jitter puts the G-buffer on the foreground one frame and the background the next -- that IS the
    // signal taa exists to resolve -- and the face centre then jumps by the whole depth separation
    // between them. The gate reads a legitimate sub-pixel sample as a disocclusion, throws the history
    // away, and the pixel emits the raw jittered sample every frame. It cannot converge, and it is
    // worst at close range where the tolerance is at its floor of two voxels.
    //
    // taa cannot work this out for itself: it point-samples the G-buffer, so all it can see is which
    // single face won a texel, not that the pixel is split between two. This pass already integrated
    // the coverage to shade the pixel, so the answer is sitting here for free.
    // ---- UPSCALE DEBUG VIEW (P key, renderParams.x == 4) ----------------------------------------
    // Answers "is the magnify actually running, and where is it reconstructing rather than filtering"
    // -- which is not a question the composite can answer, because a reconstruction that is working
    // looks exactly like a frame that was never scaled down. That is the intent, and it is also why
    // the pass is impossible to judge by eye.
    //
    //   BLUE      no magnification at all: input and output are the same size, so there is nothing to
    //             reconstruct and every pixel is passing through. If the whole frame is blue the
    //             render scale is 1.0 -- check ADVANCED_RENDER_SCALE and the Z/X/C keys.
    //   GREEN     the pixel was claimed entirely by reconstructed geometry -- by ONE face, or by two
    //             adjacent faces dividing it between them. Either is the reconstruction working, which
    //             is why this counts both layers rather than the front one alone: a boundary between
    //             two voxels is fully reconstructed, and reporting only the front face's share drew a
    //             seam along every one of them.
    //   YELLOW    partially claimed, the remainder filtered. Should be a thin band on silhouettes,
    //             where the layer behind genuinely is not resolved -- not a grid over flat walls.
    //   RED       no face claimed it, so it fell back to a plain bilinear fetch. Expected on sub-pixel
    //             geometry where no source ray hit the face; large red areas on geometry that clearly
    //             IS resolved mean the classifier is failing and the magnify has degraded to bilinear.
    // ---- SAMPLE BUDGET VIEW (P key, renderParams.x == 5) ----------------------------------------
    // What an ADAPTIVE sample rate could actually buy, per pixel, measured rather than guessed.
    //
    // The reconstruction is analytic: one sample of a voxel face gives that face's whole extent, and
    // one sample of any face gives the whole voxel's silhouette. So the rays a region needs scale with
    // how many FACES it contains, not how many pixels -- a voxel v pixels across needs about
    // FACES_PER_VOXEL samples for its v*v pixels, and is pixel-exact either way. This paints that
    // ratio, clamped at 1 because nothing needs more than a ray per pixel.
    //
    // The ramp is LOGARITHMIC in the saving, because the saving is what the decision is about and it
    // spans orders of magnitude. Each colour stop is one doubling of voxel size, which is 4x fewer
    // rays -- so the legend is four numbers rather than a gradient to squint at:
    //
    //   RED     1x    voxels ~1.7 px   full rate genuinely required, nothing to exploit
    //   YELLOW  4x    voxels ~3.5 px
    //   GREEN   16x   voxels ~7 px
    //   CYAN    64x   voxels ~14 px
    //   BLUE    256x  voxels ~28 px    near, large faces -- essentially free to reconstruct
    //
    // BLACK is sky, which needs no rays at all and would otherwise read as the cheapest surface in
    // the frame and flatter the average.
    //
    // (An earlier version of this ramp ran blue->green then jumped straight to yellow->red, so the red
    // channel stepped 0 to 1 at the midpoint and drew a hard band across smooth data. The stops below
    // are continuous.)
    //
    // Read it as a map of where a uniform render scale is wasting work. Mostly blue means adaptive has
    // a lot to win; mostly red means the log LOD has already flattened the distribution and a uniform
    // scale is close to optimal, so the machinery would not pay for itself.
    //
    // FACES_PER_VOXEL is 3 rather than 1 deliberately. One sample is enough to RECONSTRUCT a voxel's
    // geometry, but each visible face needs its own sample for its own SHADING, and a cube shows at
    // most three. 1 would be the geometric floor and a more flattering number than the truth.
    if (renderParams.x > 4.5 && renderParams.x < 5.5) {
        #define FACES_PER_VOXEL 3.0
        // Recomputed locally: the reconstruction's own copies are scoped to the block below.
        vec2  snapUV = (floor(v_texcoord0 * passInputRes[0].xy) + 0.5) * passInputRes[0].zw;
        float rad    = (2.0 * tan(radians(FOV * 0.5))) / passTargetRes.y;
        vec4  nf = texture2D(gFace, snapUV);
        float nd = texture2D(gPos, snapUV).a;
        vec3 view;
        if (nd < 0.0) {
            view = vec3(0.0);                       // sky: no rays needed here at all
        } else {
            float pixelWorld  = max(nd * rad, 1e-9);
            float voxelPixels = max(nf.w / pixelWorld, 1e-4);
            float rate    = clamp(FACES_PER_VOXEL / (voxelPixels * voxelPixels), 0.0, 1.0);
            // log2 of the saving, normalised over 1x .. 256x. Four even stops, one per 4x.
            float t = clamp(log2(1.0 / max(rate, 1e-6)) / 8.0, 0.0, 1.0);
            view = t < 0.25 ? mix(vec3(1.0, 0.0, 0.0), vec3(1.0, 1.0, 0.0),  t          * 4.0)
                 : t < 0.50 ? mix(vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0), (t - 0.25)  * 4.0)
                 : t < 0.75 ? mix(vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 1.0), (t - 0.50)  * 4.0)
                            : mix(vec3(0.0, 1.0, 1.0), vec3(0.0, 0.2, 1.0), (t - 0.75)  * 4.0);
        }
        gl_FragData[0] = vec4(view, 1.0);
        return;
    }

    if (renderParams.x > 3.5 && renderParams.x < 4.5) {
        float magnification = passTargetRes.x / max(passInputRes[0].x, 1.0);
        vec3 view = magnification < 1.001
                  ? vec3(0.0, 0.0, 1.0)
                  : mix(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), reconstructed);
        gl_FragData[0] = vec4(view, 1.0);
        return;
    }

    gl_FragData[0] = vec4(hdr, purity);
}
