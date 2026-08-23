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
// x = the upscale bypass (Q key), read by upscale.frag and taa.frag rather than here.
// y = SHADOW RAYS OFF (T key). See the gate at sunVisibility in main().
uniform vec4 debugParams;

#define PI 3.14159265358979
#define FOV 60.0

#include <pjv_sun_sky.sc>
#include <pjv_face_key.sc>
// The animation CONTROLS -- the uniforms this example steers the engine's animation with, and
// nothing else. It is all that is left of the five sway prototypes that used to live here.
//
// ---- WHY THE PROTOTYPES ARE GONE FROM THIS PASS, AND WHAT FORCED IT ------------------------
//
// This file used to include four of them: v1's displaced re-march (pjv_wave.sc), v4's leaf gather
// (pjv_gather.sc), v5's baked envelope (pjv_envelope.sc) and its fire resolve (pjv_fire.sc). Each
// carried its OWN forked copy of the scene traversal -- raySceneIntersectWaving,
// raySceneIntersectGathered, raySceneIntersectEnvelope, raySceneIntersectEnvelopeOccluder -- because
// that was the only way to hook a leaf before the engine had a RayQuery to ask with.
//
// spirv-opt inlines every function into main, so four forked traversals are four full copies of the
// march in this shader, on top of the engine's own. Adding the advection resolve to the engine path
// pushed the total past the SPIR-V id ceiling: shaderc reported "ID overflow. Try running
// compact-ids" and wrote a ZERO-LENGTH .bin, which the renderer loaded, complained about once, and
// then rendered without -- a frame that measured ten times too fast and looked plausible.
//
// So the deletion is not tidying. The forked traversal was the thing the promotion existed to
// remove, and it was consuming the budget the feature that replaced it needed.
#include <pjv_anim_controls.sc>

// Where the shadow ray leaves the surface, in edge lengths of the voxel it starts on. A fixed world
// epsilon does not survive the range of scenes this opens: scenes sit at coordinates in the
// thousands, where a float32 ULP is around a thousandth of a unit, so an offset small enough not to
// skip a real occluder near the origin is below the noise floor out there and the ray self-hits.
// Scaling by the voxel keeps it meaningful at both ends. Same reasoning, and the same number, as the
// scene editor's shadow and bounce rays.
#define SHADOW_ORIGIN_BIAS_VOXELS 0.25
// How far past its OWN box a coarsened hit starts its shadow ray, in multiples of the box size.
// A cube of side s is crossed in at most s*sqrt(3) ~ 1.74, so 1.8 clears it from any face at any
// angle. Zero for an uncoarsened hit -- see the note in sunTransmittance.
#define SHADOW_COARSE_SKIP 1.8

// How far along a SHADOW ray animation is still resolved, in voxels. Worth having at all because a
// shadow ray crosses more envelope than the camera ray does, and past the contact region the resolve
// changes a shadow that is present either way.
//
// In voxels, because that is what the march measures rayT in.
//
// SMALL, and measured rather than reasoned: on the grass-filled view this was tuned against, 24
// voxels saved 0.2ms and 4 voxels saved 1.0ms, with the two images indistinguishable apart from the
// wind phase. 24 is generous enough to still cover nearly every envelope cell a shadow ray crosses,
// which is why it buys almost nothing -- the shadow ray leaves the grass within a few voxels of its
// origin and spends the rest of its length in open air.
//
// So this is deliberately tight, and what it trades is real: shadow sway is confined to the contact
// region, and a blade's shadow further from its base is cast by the rest pose. Raise it if that reads
// wrong -- the cost is about 0.25ms per 5 voxels and it is a one-line change.
#define ENVELOPE_SHADOW_RESOLVE_VOXELS 4.0

// ---- TRANSPARENCY ---------------------------------------------------------------------------
//
// How many transparent layers a ray may cross before it gives up and treats what it is standing in
// as opaque. A budget rather than a switch, because a transparent VOLUME -- a flame, a body of water
// -- is arbitrarily deep and the cost is per layer.
//
// The two numbers differ by an order of magnitude on purpose. The camera ray is one per pixel and
// what it gets wrong is visible directly, so it can afford depth. A shadow ray is also one per pixel
// but its answer is a scalar that then gets blurred, so the last few layers of a deep medium change
// almost nothing about the image -- and running out returns the transmittance accumulated so far
// rather than black, so the failure is a slightly-too-bright shadow that degrades smoothly.
//
// Set PRIMARY_TRANSPARENT_LAYERS to 0 to get exactly the behaviour this example had before
// transparency existed: no opt-in, no peel, every voxel opaque.
#define PRIMARY_TRANSPARENT_LAYERS 16u
// How many times the PRIMARY ray may be bent by a material whose IOR is not 1. Zero reproduces the
// non-refracting traversal exactly, which is what every pass in this renderer other than this one
// still asks for: a bent shadow ray is a caustic, and this renderer has nowhere to put one.
// Refraction's budget is animDebug.z (see pjv_anim_controls.sc), set from ADVANCED_REFRACTION and
// zero by default. Runtime rather than a #define because it is not free where it is not used: the
// bend is a branch inside the peel loop, but a caller that grants segments pays for the branch on
// every layer of every ray, measured at ~0.4 ms per allowed segment of a 4.7 ms frame with nothing
// refractive on screen. A compile-time budget would charge every scene for a feature almost none use.
#define SHADOW_TRANSPARENT_LAYERS  4u

// Is the sun visible from p (a surface facing n, one voxel of it `voxelSize` across)? One ray, aimed
// at a random point on the sun's DISK rather than at its centre, so the shadow's edge comes out as
// wide as the sun is instead of one pixel wide. One sample per pixel per frame is noisy on its own
// and is meant to be: taa.frag averages 64 frames on a settled camera, which resolves the penumbra,
// and only penumbra pixels differ between samples in the first place. Set the sun's angular radius
// (sunDir.w) near zero to get the hard shadow this pass used to cast.
// ---- WHY THIS RETURNS A SCALAR AND NOT THE TINT -----------------------------------------------
//
// raySceneTransmittance hands back a vec3, which is what puts COLOURED light on the floor under
// stained glass. This example throws the hue away and keeps the luminance, because there is nowhere
// to put it: the g-buffer's six targets are full, and sun visibility travels in `gDirect.a`, a single
// channel. Folding the tint into the albedo instead would be wrong in a way that is worse than losing
// it -- compose builds the image as `gDirect + gAlbedo * R`, so a tint meant for the SUN's path would
// silently colour the indirect bounce as well.
//
// So: transparent occluders cast shadows of the right DEPTH here, but not of the right colour. The
// editor, which shades forward and has the whole vec3 in hand, does both.
float sunTransmittance(vec3 p, vec3 n, float voxelSize, inout uint seed) {
    Ray shadow;
    shadow.direction = SUN_DIR;

    // ---- DO NOT SHADOW A SURFACE WITH GEOMETRY YOU CHOSE NOT TO DRAW ----------------------------
    // A COARSENED hit is a box reported whole because its contents were finer than a pixel. The point
    // being shaded therefore sits on that box's outer FACE, with the real geometry somewhere inside
    // it -- and the shadow ray traces at LOD 0, so it sees that geometry. Leaving along n clears the
    // box, but the ray then turns toward the sun and at any oblique angle re-enters it and is
    // occluded by the very detail the primary ray declined to resolve. The result is a coarsened
    // surface shadowed by its own interior: false shadow, worst where the sun is most grazing.
    //
    // The 0.25-of-a-box bias along n cannot fix this, because the occlusion happens along the SUN
    // direction, not along the normal. So step past the box that way too.
    //
    // Scaled by how coarse the hit actually is, not by its size, which is what keeps ordinary hits
    // untouched: a LOD-0 voxel is one finest voxel across, so `coarseness - 1` is zero and this whole
    // term vanishes. Only a box that was merged pays it, and it pays exactly enough to escape itself
    // (a cube of side s is crossed in at most s*sqrt(3), hence the 1.8).
    // A THRESHOLD, not a ramp, and not `coarseness - 1`. Coarseness is quantised -- 1, 2, 4, 8 -- so
    // there is no continuum here to smooth, and subtracting one under-clears precisely the case that
    // matters: a half-level block is 2 voxels across and needs 2*sqrt(3) ~ 3.5 to escape, where
    // (2-1)*1.8 gives 1.8 and leaves the ray inside its own box.
    float baseVoxel  = animVoxelSize();               // world size of one finest voxel
    float coarseness = voxelSize / baseVoxel;          // 1 for an ordinary hit, 2/4/8 when merged
    float skipPastOwnBox = coarseness > 1.5 ? voxelSize * SHADOW_COARSE_SKIP : 0.0;

    shadow.origin = p + n * (SHADOW_ORIGIN_BIAS_VOXELS * voxelSize)
                      + shadow.direction * skipPastOwnBox;
    // The sun's CENTRE, not a random point on its disk. Cone sampling is how a penumbra forms
    // physically, and it makes this one ray per pixel stochastic -- the shadow then arrives as
    // one-sample noise that only resolves after many frames, which nothing that moves ever gets.
    // Softness is applied spatially in compose.frag instead, to a signal that has no noise in it.
    // The disk keeps its real angular size for the sun DRAWN in the sky; only the shadow ignores it.
    // (shadow.direction is assigned above, because the origin offset needs it.)
    // Below the horizon of this surface: no trace needed, and tracing one would answer a question
    // about the wrong hemisphere.
    if (dot(shadow.direction, n) <= 0.0) return 0.0;
    RayQuery rq = pjvPrimaryQuery(100u);
    rq.maxRaySteps = 128u;
    // Finest LOD for the whole ray. A coarse-LOD shadow ray tests against merged boxes that
    // over-occlude, which shows up as false shadow -- so the cost is cut with the step budget
    // instead, which errs the safe way: a ray that runs out of steps reports a miss, i.e. lit.
    rq.startLOD = 0u; rq.finishLOD = 0u; rq.distanceToFinishLOD = 100000u;
    // Swaying occluders, so a blade's shadow moves with the blade. The sun is at infinity, so there
    // is no near occluder to bound against.
    // Rec.709 luminance of what survived the trip. See the note on this function for why the colour
    // is dropped here rather than carried.
    if (!animShadowsResolve()) {
        if (SHADOW_TRANSPARENT_LAYERS > 0u) pjvQueryTransparency(rq, SHADOW_TRANSPARENT_LAYERS, seed);
        return dot(raySceneTransmittance(shadow, rq), vec3(0.2126, 0.7152, 0.0722));
    }
    {
        // A shadow ray resolves animation only NEAR THE SURFACE. Sway reads as sway in the contact
        // region, where the shadow meets what casts it; a metre up the ray, whether the occluder is
        // at its rest position or its drawn one is a shadow that is present either way. Measured on
        // the prototype, the unbounded version cost more than the primary ray's own resolve.
        pjvQueryAnimation(rq, ENVELOPE_SHADOW_RESOLVE_VOXELS * animVoxelSize());
        // Transparency and animation on the SAME ray, which is the pairing neither prototype could
        // express: the fork that resolved sway dropped tMin, and the peel is built on tMin. A pane of
        // glass in front of swaying grass has to be right about both at once.
        if (SHADOW_TRANSPARENT_LAYERS > 0u) pjvQueryTransparency(rq, SHADOW_TRANSPARENT_LAYERS, seed);
        return dot(raySceneTransmittance(shadow, rq), vec3(0.2126, 0.7152, 0.0722));
    }
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
    //
    // SCALED BY renderParams.w, and at 0 this pass becomes fully deterministic -- every frame samples
    // the identical point, so there is nothing left for a temporal filter to converge and nothing that
    // can flicker. That is now a real option rather than a degradation, because the antialiasing this
    // jitter exists to feed is no longer the only antialiasing there is: display.frag computes edge
    // coverage analytically from the voxel faces themselves, which needs no samples over time.
    //
    // Whether the jitter still earns its place is exactly the question it is a knob to answer. What it
    // still buys, if anything, is sub-pixel detail on geometry finer than one sample -- distant grass
    // -- which coverage cannot recover because no ray ever hit it. What it costs is that every pixel's
    // value changes every frame, and anything that cannot converge then shimmers.
    int  frame  = int(frameCount.x);
    vec2 jitter = (vec2(halton(frame + 1, 2), halton(frame + 1, 3)) - 0.5) * renderParams.w;
    vec2 uvJit  = v_texcoord0 + jitter / passTargetRes.xy;

    Ray ray;
    ray.origin = cameraPos.xyz;
    ray.direction = rayStartDirection(uvJit, passTargetRes.xy, cameraPos.xyz,
                                      normalize(cameraDir.xyz), FOV);

    // Per pixel and per frame. Feeds the sun-cone sample far below AND the peel's stochastic
    // interface decision, which is why it is declared up here rather than beside its first use --
    // the primary ray is the peel's first customer. The pixel coordinate is rebuilt from the
    // UNJITTERED uv rather than from gl_FragCoord, which bgfx does not expose to a fragment shader on
    // the SPIR-V path.
    ivec2 pixel = ivec2(v_texcoord0 * passTargetRes.xy);
    uint seed = pjvHashSeed(uvec3(uint(pixel.x), uint(pixel.y), uint(frame)));

    // ---- PRIMARY RAY LOD, DRIVEN BY THE PIXEL FOOTPRINT -----------------------------------------
    // This was pinned to LOD 0 everywhere, and the reason recorded was that "a distance LOD ramp
    // collapses far geometry into big coarse blocks". That is a true observation about the wrong
    // criterion rather than a reason to have no LOD: a ramp keyed on DISTANCE coarsens geometry
    // because it is far, which says nothing about whether the coarsening would be visible. Keyed on
    // the pixel FOOTPRINT it cannot produce that look by construction, because a level is only allowed
    // once one of its nodes is smaller than a pixel -- at which point it is below what the display can
    // resolve and dropping to it removes no detail that could have been seen.
    //
    // It also attacks the far-field aliasing at its source, which nothing downstream can. One primary
    // ray per texel point-samples whichever of the many sub-pixel voxels it happens to hit, and no
    // reconstruction can recover a signal that was never sampled -- the upscaler can only stop
    // amplifying it and taa can only average the draws it is given. A node that covers the whole
    // footprint is not a sample of the footprint, it IS the footprint, so it does not alias.
    //
    // ---- THE CROSSOVER, AND WHY THE LEVELS SIT WHERE THEY DO ------------------------------------
    // One output pixel subtends `d * 2*tan(fov/2) / height` voxels at a ray distance of d voxels, so a
    // LOD-0 voxel shrinks to exactly one pixel at CROSSOVER = height / (2*tan(fov/2)). Derived from
    // the live resolution rather than hard-coded, so it retunes with the window and the render scale.
    //
    // Past that distance LOD 0 is undersampled -- one ray per texel against many voxels per pixel --
    // and the reconstruction downstream has nothing to reconstruct FROM, so it falls to a bilinear
    // filter. Coarsening exactly there is what closes that band: a level-1 node is 4 voxels, which at
    // the crossover is 4 pixels, comfortably resolvable and therefore reconstructable again.
    //
    // THE ENGINE'S RAMP IS LINEAR, AND THAT IS THE BINDING CONSTRAINT. computeTargetLOD returns
    // uint(mix(startLOD, finishLOD, d / distanceToFinishLOD)), so level k arrives at k*D/N -- EVENLY
    // spaced. The ideal onsets are exponential, since level k first fits in a pixel at 4^(k-1) *
    // CROSSOVER: 1x, 4x, 16x. A linear ramp can place ONE level exactly; everything after it arrives
    // early and is coarser than the pixel requires.
    //
    // The ramp is LOGARITHMIC, via the DDA's opt-in footprint mode (PJV_LOD_FOOTPRINT -- see the note
    // at computeTargetLOD). The engine's ordinary ramp is linear and spaces its levels evenly, which
    // cannot match onsets that are a geometric series: it placed level 1 correctly and then level 2
    // arrived twice as early as it should, at 8 pixels a node, which is the coarseness that showed.
    //
    // In footprint mode every level lands at 4^k * CROSSOVER, exactly where ITS OWN nodes shrink to a
    // pixel, so no level is ever coarser than the display can resolve:
    //
    //         onset          node size there     what it replaces
    //   L1    4x CROSSOVER        1 px           LOD-0 voxels at 1/4 px
    //   L2   16x CROSSOVER        1 px           LOD-1 nodes at 1/4 px
    //   L3   64x CROSSOVER        1 px           LOD-2 nodes at 1/4 px
    //
    // Which also puts the transitions where they were wanted: level 1 moves out from 2x to 4x and
    // level 2 from 4x all the way to 16x, and the levels beyond stop being a compromise -- 3 costs
    // nothing now, where under the linear ramp it would have put 21-pixel blocks on screen.
    //
    // BIAS scales the crossover if the transitions still read early: 2.0 pushes every level twice as
    // far out, at the cost of leaving LOD 0 undersampled for an extra octave. It should not be needed.
    #define PRIMARY_LOD_ENABLE 1
    #define PRIMARY_LOD_LEVELS 3u
    #define PRIMARY_LOD_BIAS   1.0
    // ---- DITHERED LOD BOUNDARIES ----------------------------------------------------------------
    // How far each pixel's crossover may wander, in octaves. 1.0 spreads it across the whole gap
    // between two levels, so there is no distance at which a majority of pixels switch together and
    // therefore no ring at all. 0.5 softens the ring without mixing everywhere. 0.0 is off.
    //
    // The level change is a 2x step in the geometry quantum and cannot be made smaller -- 2x is
    // already the finest the tree can express. What CAN be removed is its COHERENCE: if every pixel
    // switches at the same distance the result is a hard ring, and if each switches at its own
    // distance it is a dissolve, which the eye reads as a gradual change in detail.
    //
    // DITHERING THE CROSSOVER, not the level, is what keeps it correct. The chosen size comes from two
    // places -- computeTargetLOD's round-up and the half-level test in the march -- and both derive it
    // from this one distance. Perturbing the level in one of them makes the two disagree about which
    // size is wanted; perturbing the distance keeps them in step by construction, with no engine change
    // and no new field on RayQuery.
    //
    // The reconstruction then does the crossfade for free. In the mixed band a pixel's neighbourhood
    // holds both a block and the finer voxels inside it, and upscale.frag composites them as ordinary
    // coverage layers -- so the two levels blend by area rather than popping.
    //
    // STATIC: the noise is a function of the pixel alone, so the pattern is identical every frame and
    // cannot flicker. A per-frame dither would hand the temporal filters a fresh geometry decision
    // every frame, which is the one thing this renderer has worked hardest not to do.
    //
    // OFF, because it works against the reconstruction rather than with it. A dither produces a
    // crossfade only if something downstream AVERAGES the two levels it mixes -- which a bilinear
    // magnify does, and which upscale.frag deliberately does not. The reconstruction makes a discrete
    // choice per output pixel and draws that voxel's exact silhouette, so a neighbourhood where half
    // the texels report a block and half report the voxels inside it comes out as crisp geometric
    // noise instead of a blend. Measured by eye against the Q bypass: good bilinear, bad reconstructed.
    //
    // Kept rather than deleted because it is one number, and because it is the right tool the moment
    // anything downstream does average -- and because the narrow settings are still worth a try:
    //   1.0  spreads across the whole gap, so every neighbourhood is mixed. This is what looked bad.
    //   0.25 mixes only within a quarter octave of each boundary, leaving most of the frame uniform
    //        and softening just the ring. The honest middle ground if the ring is the main complaint.
    //   0.0  off. Hard boundaries, every neighbourhood uniform, reconstruction at full strength.
    #define PRIMARY_LOD_DITHER 0.0

    RayQuery rq = pjvPrimaryQuery(100u);
    rq.maxRaySteps         = 512u;
#if PRIMARY_LOD_ENABLE
    // Where one LOD-0 voxel projects to exactly one pixel. Derived from the live resolution, so it
    // retunes itself with the window and the render scale.
    float voxelsPerPixelPerUnit = (2.0 * tan(radians(FOV * 0.5))) / passTargetRes.y;
    float crossoverDistance     = PRIMARY_LOD_BIAS / max(voxelsPerPixelPerUnit, 1e-9);

    // Interleaved gradient noise, on the UNJITTERED pixel so it is frame-invariant. Chosen over a hash
    // because it distributes far better over a small neighbourhood, and whether the dissolve reads as
    // a gradient or as clumps is decided entirely by that.
    vec2  lodPixel = floor(v_texcoord0 * passTargetRes.xy);
    float lodNoise = fract(52.9829189 * fract(0.06711056 * lodPixel.x + 0.00583715 * lodPixel.y));
    // exp2 of a negative, so crossovers spread UNIFORMLY IN LOG distance -- the same way the level
    // boundaries themselves are spaced, which makes the dissolve even across the gap instead of
    // bunching at one end.
    crossoverDistance *= exp2(-lodNoise * PRIMARY_LOD_DITHER);

    rq.startLOD            = PJV_LOD_FOOTPRINT;
    rq.finishLOD           = PRIMARY_LOD_LEVELS;
    rq.distanceToFinishLOD = uint(clamp(crossoverDistance, 1.0, 1.0e9));
#else
    rq.startLOD            = 0u;
    rq.finishLOD           = 0u;
    rq.distanceToFinishLOD = 100000u;
#endif

    // The swaying query in place of raySceneIntersect. It hands back the material as well, because
    // every branch inside it had to fetch one to decide which class a hit belonged to.
    VoxelMaterial material;
    SceneIntersectData hit;
    // What the transparent layers in FRONT of the hit did to the light behind them, and what they
    // emitted on their own account. Both identities on every path that does not peel, so a scene with
    // no transparent material is arithmetically untouched by their presence here.
    vec3 layerTransmittance = vec3(1.0);
    vec3 layerEmission = vec3(0.0);
    // Why the traversal stopped, which is the engine's replacement for the prototype's four
    // file-scope diagnostic statics. See the debug views below.
    uint stopReason = PJV_STOP_OPAQUE;
    Ray hitRay = ray;
    {
        // The engine path: one query, one traversal, and the animation is a flag on it rather than a
        // different function to call. Past the cutoff the envelope is skipped and animated geometry
        // draws at rest -- a blade is subpixel well before it stops costing, so this is close to free
        // visually and is the largest single saving available.
        pjvQueryWantMaterial(rq);
        // `O` turns the animation TRAVERSAL off while leaving everything else exactly as it is: the
        // materials are still flagged, the envelope is still baked, still uploaded, still in the
        // header. The query simply does not ask for animation, so the geometry march stops skipping
        // animated voxels and draws them at their rest position.
        //
        // This is the observation that tests the layer BELOW the animated march. If the geometry is
        // solid and correct here, then flagging, baking, uploading and the g-buffer are all sound and
        // the entire fault is in the animated path. If it is still see-through here -- with no
        // animation code running at all -- then the fault is somewhere none of that path can reach,
        // and every hypothesis about the resolve, the envelope march and the merge is wrong.
        if (!animationSuspended()) {
            pjvQueryAnimation(rq, animResolveVoxels() * animVoxelSize());
        }
        // ---- SEE THROUGH TRANSPARENT VOXELS TO THE NEAREST OPAQUE SURFACE ----------------------
        //
        // This is the ENGINE path only, and deliberately. The prototype modes below run their own
        // forked traversals which have no peel to opt into -- that fork is exactly what the promotion
        // removed -- so asking for transparency there would compile and do nothing, which is worse
        // than not asking.
        //
        // An opaque scene takes the same traversal it did before the opt-in: the peel stops at the
        // first hit, and the material it had to fetch to decide that is handed back rather than
        // fetched a second time below. The cost lands only where transparent voxels actually are.
        if (PRIMARY_TRANSPARENT_LAYERS > 0u) {
            pjvQueryTransparency(rq, PRIMARY_TRANSPARENT_LAYERS, seed);
        }
        // ---- ...AND BEND AT AN INTERFACE THAT SAYS IT BENDS LIGHT ------------------------------
        //
        // Only the primary ray. A refracted shadow ray is a caustic, and a deferred renderer has
        // nowhere to put one -- the shadow term is a scalar per surface, not a light path. Asking for
        // it there would cost every shadow ray a segment loop and change nothing on screen.
        if (animRefractionSegments() > 0u) {
            pjvQueryRefraction(rq, animRefractionSegments());
        }
        SceneHit sh = raySceneIntersect(ray, rq);
        hit = sh.hit;
        stopReason = sh.stopReason;
        // The ray the hit was found ON, which is the ray handed in unless something bent it. Every
        // reconstruction below goes through this rather than through `ray`: after a bend,
        // `ray.origin + ray.direction * hit.rayT` names a point in empty space along the original
        // direction, and the surface would be placed there -- shaded correctly, positioned wrongly,
        // and reprojected into the wrong pixel next frame.
        hitRay = sh.finalRay;
        layerTransmittance = sh.transmittance;
        layerEmission = sh.emission;
        material = sh.materialValid ? sh.material
                 : ((hit.foundBox.size > 0.0 && hit.rayT >= 0.0)
                        ? fetchVoxelMaterialFromHit(hit) : emptyVoxelMaterial());
    }
    // NOTE: there is no fire side channel here any more, and its absence is the point.
    //
    // The prototype accumulated a flame's emission into a file-scope static that every scene query
    // reset -- so the shadow ray below wiped what the camera ray had gathered, and the flame vanished
    // against anything it was not standing directly in front of. An advected parcel is now an
    // ordinary hit carrying an ordinary material, and its emission arrives through `layerEmission`
    // like any other transparent layer's does.

    // Trust the march's own hit data rather than re-intersecting foundBox analytically: a fresh slab
    // test disagrees with the march by ULPs on a boundary-exact hit and misclassifies it.
    vec3 n = hit.normal;

    // Miss / degenerate boundary hit -> sky background.
    if (hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(n, n) < 0.5) {
        // Fire accumulates along the ray and never occludes, so a flame seen against the sky lands
        // here rather than on the surface path -- it has to be added on both.
        // The sky the ray is looking at AFTER any bend, not the one it set out towards. A ray that
        // left through the far side of a glass sphere is pointing somewhere else, and that is the
        // whole visible effect of refraction against a sky background.
        vec3 sky = skyColor(hitRay.direction);
        // ---- WHY IS THIS PIXEL SKY? -------------------------------------------------------------
        //
        // A miss carries no reason and the reasons are not close together, so a hole in the canopy
        // and empty air look identical on screen. This separates them.
        //
        // Read off SceneHit::stopReason rather than off the four file-scope statics the prototype
        // kept for it. That is the whole shape of the promotion in miniature: the traversal already
        // knew why it stopped, and the statics existed only because the hit record had nowhere to
        // put it -- with each one needing its own save/restore across the chunk loop, and one of them
        // (the fire emission) being wiped by the shadow ray that ran after it.
        //
        // The other half of this diagnostic is engine-side and is the one to reach for first:
        // PJV_Q_ANIM_DEBUG_SOLID draws every envelope cell as a solid block, which splits the failure
        // in half -- blobs mean the bake, the upload and the envelope march are all sound and the
        // fault is in the resolve; nothing at all means the geometry never had anywhere to be drawn.
        if (animDebugMode() == 1 && stopReason == PJV_STOP_STEPS) {
            sky = vec3(1.0, 0.0, 1.0);          // magenta -- the march ran out of steps
        } else if (animDebugMode() == 2 && stopReason == PJV_STOP_ITERATIONS) {
            sky = vec3(1.0, 0.55, 0.0);         // orange -- the peel ran out of passes
        } else if (animDebugMode() == 2 && stopReason == PJV_STOP_TMIN_STALL) {
            sky = vec3(0.0, 0.15, 1.0);         // blue -- the resume floor failed to advance
        }
        gl_FragData[0] = vec4(hitRay.origin + hitRay.direction * 1e5, -1.0); // a<0 => sky
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);                     // no surface, no voxel size
        gl_FragData[2] = vec4(0.0);
        // The background filtered by whatever transparency stands between it and the camera, plus
        // what that transparency emitted. This is what makes a pane of glass against the sky read as
        // glass rather than as nothing at all, and it is why a glowing pane in front of NOTHING still
        // glows. Both terms are identities on an opaque scene.
        gl_FragData[3] = vec4(sky * layerTransmittance + layerEmission, 0.0);
        gl_FragData[4] = vec4(0.0);
        // A key no real face can hold, so nothing ever matches against the background. NOT zero:
        // zero is voxel (0,0,0) face 0, which is a real face in every scene with a chunk at the
        // origin, and the sky would then share its converged lighting.
        gl_FragData[5] = vec4(FACE_KEY_NONE, FACE_KEY_NONE, FACE_KEY_NONE, FACE_KEY_NONE);
        return;
    }
    n = normalize(n);

    vec3 P = hitRay.origin + hitRay.direction * hit.rayT;

    // `material` came back from raySceneIntersectWaving above rather than being fetched here. That
    // query had to read a material to decide which displacement class every candidate hit belonged
    // to, so the two texelFetches this line used to cost have already been paid -- and on a swaying
    // hit they cannot be re-derived from `hit` anyway without knowing which class march found it.
    //
    // A metal has no diffuse lobe -- what it shows is its reflection, tinted by its own albedo -- so
    // leaving the albedo in would draw a chrome voxel as a bright grey one. This renderer has no
    // mirror reflection to put back in its place (compose's rough specular reads the same cascade),
    // so a metal reads as dark and reflective rather than as bright and flat. A default material has
    // metallic 0 and is untouched.
    // Dimmed and tinted by the transparent layers in front of it. Folded into the ALBEDO rather than
    // applied at the end because compose builds the image as `gDirect + gAlbedo * R`: putting it here
    // filters the indirect term too, so a surface seen through green glass is green in its bounce
    // light as well as in its direct light. Exactly one on an opaque scene.
    vec3 albedo = material.albedo * (1.0 - material.metallic) * layerTransmittance;


    // Direct sun, soft-shadowed against the sun disk. Kept out of the GI's temporal accumulation on
    // purpose: this is the crisp half of the image and compose adds it back after the indirect term
    // has been denoised.
    // The sun's VISIBILITY, on its own, rather than the lit colour it produces.
    //
    // Kept separate so compose.frag can blur it. Softening a shadow by widening the sun makes the one
    // ray per pixel stochastic and the penumbra noisy; blurring a sharp binary visibility is
    // deterministic and needs no temporal convergence, which is the only kind that works on geometry
    // that moves. What must NOT be blurred is the shading -- (albedo/PI) * NdotL is per-face and is
    // most of the crispness in the image -- so the two are multiplied back together in compose, after
    // the blur, and only the shadow is soft.
    float NdotL = dot(n, SUN_DIR);
    // ---- SHADOW RAYS OFF (T key): WHICH RAY IS THE RENDER SCALE ACTUALLY BUYING ------------------
    // This pass casts TWO voxel rays per pixel -- the camera ray that found P, and the sun shadow ray
    // inside sunTransmittance() -- and the render scale moves both at once, so the measured win is their
    // sum and says nothing about the split. Dropping the shadow ray leaves the camera ray and the
    // whole rest of the frame untouched, so scaling with this ON measures the primary alone, and the
    // difference between the two sweeps is what the shadow ray was contributing.
    //
    // The && already short-circuits on a back-facing surface, so this is the same shape of skip: the
    // trace does not happen, it is not traced and then discarded. Everything downstream still gets a
    // well-formed visibility term, just an unshadowed one -- compose blurs a constant and gets a
    // constant, and no branch anywhere else has to know.
    //
    // NdotL is still honoured, so surfaces facing away from the sun stay dark and the image remains
    // readable enough to steer the camera to the view being measured. What is gone is cast shadow
    // only, which is exactly the ray whose cost is in question.
    bool traceShadows = debugParams.y < 0.5;

    // ---- IS THIS VOXEL IN SUNLIGHT, RATHER THAN THIS FACE ----------------------------------------
    // The per-face term below is gated on NdotL, and that gate is why the answer cannot simply be
    // pooled downstream. `sunVisibility` as written conflates two different facts -- "something is
    // between me and the sun" and "I am facing away from the sun" -- because both come out as zero.
    // Averaging a voxel's faces from that channel would count orientation twice: once in the mean,
    // and again in compose's cosine term, and every voxel with a lit and an unlit face on screen
    // would come out half dark in open sunlight.
    //
    // Asked of the VOXEL instead: trace from the voxel's CENTRE, pushed out along the sun far enough
    // to clear the voxel's own body (half a diagonal is sqrt(3)/2 ~ 0.87 of an edge), and do not gate
    // on facing at all. The answer then depends only on the voxel and the sun, so every face of it
    // gets the same number and that number means purely "is this voxel in sunlight". `n` is handed
    // the sun direction so sunTransmittance's own origin bias pushes along the direction the ray is about
    // to travel, which is what it wants here.
    //
    // TWO CASES TAKE IT, and the second is the point of this branch existing outside per-voxel mode.
    //
    //   * PER-VOXEL LIGHTING (U), where every surface is lit as a unit by definition.
    //
    //   * FOLIAGE, ALWAYS -- because the translucency downstream is a question about the voxel's FAR
    //     SIDE. A backlit blade glows precisely when the sun is striking the face you cannot see, and
    //     a per-face shadow ray can never report that: it is not cast at all when NdotL <= 0, which
    //     is every backlit blade, which is the entire case the effect exists for. compose.frag was
    //     approximating the answer by averaging its neighbours' visibility over a screen-space ring,
    //     and a backlit blade's neighbours are all backlit too, so the ring reported "no sun"
    //     exactly where the truth was "full sun on the other side". This replaces that guess with
    //     the ray that actually answers it.
    //
    // COST, stated plainly: a back-facing foliage pixel now casts a shadow ray it previously
    // short-circuited past. Front-facing foliage is unchanged in count (the per-voxel ray replaces
    // the per-face one and they agree on a one-voxel-thick blade), and nothing else in the scene is
    // touched. So the bill is one ray per visible backlit foliage pixel, which is what buying a real
    // answer instead of a screen-space one costs.
    //
    // The diffuse term does NOT become wrong from this. compose multiplies by max(dot(N, L), 0),
    // which is still zero on the face you cannot see, so a lit voxel does not start lighting its own
    // shaded side -- only the translucency, which is supposed to read the far side, benefits.
    // Animated by a DISPLACEMENT field. An advecting parcel is excluded on purpose: it is a medium
    // rather than a one-voxel-thick blade, so the "light reaches the far side" argument this switch
    // rests on does not apply to it.
    bool foliage = pjvMaterialIsAnimated(material) && !pjvMaterialIsAdvected(material);

    // No longer 0 or 1: a transparent occluder returns what fraction of the sun got through, so a
    // surface under glass is dimmed rather than blacked out. Fully opaque geometry still returns
    // exactly 0 and open sky exactly 1, so nothing without transparent materials moves.
    float sunVisibility;
    if (PJV_PER_VOXEL_LIGHTING || foliage) {
        vec3 voxelCentre = hit.foundBox.position + hit.foundBox.size * 0.5;
        vec3 origin = voxelCentre + SUN_DIR * (hit.foundBox.size * 0.87);
        sunVisibility = traceShadows ? sunTransmittance(origin, SUN_DIR, hit.foundBox.size, seed) : 1.0;
    } else {
        // The NdotL gate stays a hard zero: a face turned away from the sun is unlit regardless of
        // what stands between it and the sun, and tracing that ray would answer a question about the
        // wrong hemisphere.
        sunVisibility = (NdotL > 0.0)
                      ? (traceShadows ? sunTransmittance(P, n, hit.foundBox.size, seed) : 1.0)
                      : 0.0;
    }
    vec3 direct = vec3(0.0);
    // An emitter's own glow is not scaled by its albedo and does not care where the sun is, so it is
    // added here rather than folded into the term above. The cascade gather picks the same emission
    // up from the other side (see pjv_cascade_ws.sc), which is what spreads an emissive voxel's
    // light onto its neighbours instead of leaving it a bright patch in an unlit room.
    direct += material.emission;
    // The surface's own emission is dimmed by the transparency in front of it, and the layers' own
    // emission is added on top -- already attenuated layer by layer by the peel, so a flame behind
    // glass behind more glass arrives correctly darkened without this having to know how deep it was.
    // `direct` is the right target because compose adds it unmodified and never shadows or blurs it.
    direct *= layerTransmittance;
    direct += layerEmission;

    // ---- ...AND ON THE SURFACE PATH TOO -----------------------------------------------------
    //
    // The sky branch alone can only ever show a camera ray that found NOTHING, and the failure that
    // matters most is not a miss: a ray that gave up can carry on and hit something far behind, and
    // that pixel is a hit with the wrong geometry in it. Marking only the sky branch could never see
    // it -- which is why the prototype's first two debug modes missed every hole that reacted to the
    // sun angle.
    //
    // Written into `direct`, which is emission-only and is deliberately neither shadowed nor blurred
    // downstream, so the marker survives compose unmodified rather than being multiplied by whatever
    // lighting is under suspicion.
    if (animDebugMode() == 1 && stopReason == PJV_STOP_STEPS) {
        direct += vec3(6.0, 0.0, 6.0);
    }
    if (animDebugMode() == 2 && stopReason == PJV_STOP_ITERATIONS) {
        direct += vec3(6.0, 3.3, 0.0);
    }

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
    // a = DOES THIS SURFACE FAIL TO STAY STILL. Stated outright rather than inferred downstream,
    // because inferring it is not possible: taa tried to spot a displaced voxel by watching its face
    // centre move, and a face centre also moves when the sub-pixel jitter simply samples the
    // NEIGHBOURING voxel -- by about the same distance, so the two cases cannot be separated by any
    // tolerance. Here the answer is already known for free.
    //
    // waveIsMoving, not waveIsAnimated: fire resolves by backward advection rather than by
    // displacement and carries its own flag bit, so the narrow test reported every fire parcel as
    // stationary. Both consumers of this channel -- taa's history length and accumulate's choice of
    // gate -- want "may I reuse last frame here", which fire answers no to just as firmly as grass.
    //
    // THREE states, not two, because grass and fire fail to hold still in different ways and want
    // different treatment. A swaying blade keeps its SOURCE VOXEL (the envelope march reports it on
    // purpose), so accumulate can gate it on an exact voxel identity and give it a long mean. A fire
    // parcel has no such anchor -- it advects through space, so the voxel under it is genuinely a
    // different one each frame -- and can only be matched positionally, with a looser radius and a
    // shorter mean. Collapsing the two into one flag is why fire was still flickering after grass
    // stopped: it was being offered grass's gate, which nothing about fire can pass.
    //   0.0 = stationary   0.5 = displaced (sway)   1.0 = advected (fire)
    float motionClass = pjvMaterialIsAdvected(material) ? 1.0
                      : (pjvMaterialIsAnimated(material) ? 0.5 : 0.0);
    gl_FragData[2] = vec4(albedo, motionClass);
    // rgb = EMISSION ONLY (an emitter's glow, which must not be shadowed or blurred), a = the sun's
    // binary visibility, which compose blurs and multiplies back against per-pixel shading. The alpha
    // channel here previously carried a constant 1.0 that nothing read.
    gl_FragData[3] = vec4(direct, sunVisibility);
    gl_FragData[4] = vec4(faceCentre, voxelSize);
    gl_FragData[5] = pjvFaceKey(hit.voxelCoord, hit.headerIndex, n);
}
