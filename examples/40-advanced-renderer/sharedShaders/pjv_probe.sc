// =============================================================================
// pjv_probe.sc  --  Per-voxel-face hemispherical gather. The whole of the indirect light.
//
// This replaces the four-cascade radiance-cascade gather (pjv_cascade_ws.sc) with one Monte Carlo
// integration per voxel face, converged over time rather than over a merge hierarchy.
//
// -----------------------------------------------------------------------------
// WHY THE CASCADES WENT
// -----------------------------------------------------------------------------
// The cascades were a way to get many effective directions per probe cheaply: sixteen rays at
// cascade 0, merged with progressively coarser-in-space / finer-in-angle cascades above. It works,
// but it pays for those directions with a structure that has to decide, every frame, WHICH FACES
// OWN A PROBE -- and it decided that with a lattice in SCREEN space: probe p sampled whatever face
// sat at (p + 0.5) * 16 pixels.
//
// That is a viewpoint-dependent answer to a viewpoint-independent question, and it was the source
// of every symptom worth caring about. At distance nearly every face was interpolated from
// neighbours on OTHER faces; up close most faces owned a probe; walking towards a wall re-lit it.
// resolve.frag's own comments trace two separate attempts to work around it (the per-face shortcut,
// then its removal) and neither could succeed, because the problem was upstream of both.
//
// -----------------------------------------------------------------------------
// WHAT REPLACES THEM, AND WHY IT CONVERGES WITHOUT A MERGE
// -----------------------------------------------------------------------------
// A voxel face's indirect light is one number: the cosine-weighted mean radiance R arriving over
// its hemisphere. Sample it with cosine-weighted directions and the estimator is just the
// arithmetic mean of the radiance those rays find -- no weights, no merge, no interpolation.
//
// Only PROBE_RAYS of them are cast per frame, which on its own is far too few. What makes that
// enough is that the sample sequence is seeded by THE FACE KEY and advanced by the FRAME COUNTER:
//
//   * Every pixel looking at one face draws from the SAME sequence, so the answer belongs to the
//     face rather than to the pixel or the viewpoint. A face is flat-shaded in the indirect term
//     with no spatial noise, and -- the part the cascades could never manage -- its value does not
//     depend on where the camera is standing.
//
//   * Consecutive frames draw DIFFERENT directions, from a low-discrepancy sequence that keeps
//     filling the hemisphere evenly rather than clumping. accumulate.frag's running mean turns
//     PROBE_RAYS per frame into PROBE_RAYS * age effective samples, and its per-face identity gate
//     (gKey) is exactly the right gate for a per-face quantity.
//
//   * Each probe TEXEL offsets that sequence differently (pjvProbeSample), so the several texels
//     that land on one face are independent estimates rather than duplicates, and resolve.frag
//     averages them. That multiplies the per-frame sample count by the face's texel coverage
//     without casting a single extra ray beyond the budget already spent.
//
// So the convergence the cascade merge bought in ANGLE, this buys in TIME and in COVERAGE, and both
// are budgets a renderer with a temporal pass and a quarter-res probe buffer already has. Eight rays
// a frame against a 128-frame mean is a thousand directions per face, against the cascades' sixteen.
//
// The corollary is that this design LIVES ON ITS TEMPORAL MEAN in a way the cascades did not: their
// per-frame output was deterministic and looked fine at age 1, this one is a Monte Carlo estimate and
// does not. Anything that silently breaks accumulation shows up here as flicker rather than as mild
// banding -- which is exactly how the two bugs found during its bring-up announced themselves. If
// this ever looks noisy, render accumR's alpha (the age) before touching anything in this file.
//
// -----------------------------------------------------------------------------
// MULTI-BOUNCE, FOR ONE TEXTURE READ
// -----------------------------------------------------------------------------
// A gather ray that lands on a lit surface returns that surface's DIRECT light. Under the cascades
// that was the end of it -- one bounce, and enclosed spaces came out too dark, which is what the
// merge's transparent-ray sky term was quietly compensating for. Here, the ray also looks up the
// hit point in last frame's accumulated indirect buffer and adds it. That buffer is itself the
// result of this same gather, so the feedback loop converges to the full multi-bounce solution at
// the cost of one texture fetch per ray. It is the single largest quality difference in this file.
//
// Requires (declared/included by the including file BEFORE this include):
//   SAMPLER2D gPos (0), and prevIndirect at PROBE_PREV_SLOT if PROBE_MULTIBOUNCE is on
//   #include <pjv_utils_DDA.sc>       (Ray/RayQuery/raySceneIntersect/materials + scene samplers)
//   #include <pjv_cascade_common.sc>  (worldToUV, PI, WORLD_SCALE, and pjv_sun_sky beneath it)
//   #include <pjv_face_key.sc>        (the face identity this seeds its sequence with)
// =============================================================================

#ifndef PJV_PROBE_SC
#define PJV_PROBE_SC

// ---- Ray budget -------------------------------------------------------------
// Rays cast per face per FRAME. The effective sample count is this times accumulate's age, so the
// knob to reach for when the GI looks noisy is usually STILL_MAX_AGE over there, not this one --
// raising this costs linearly every frame, raising that costs nothing.
#ifndef PROBE_RAYS
#define PROBE_RAYS 8
#endif

// Max DDA steps per gather ray. One long ray now does the job four cascades' intervals used to
// share, so this is larger than any single WS_STEPS was; it is still the main cost knob.
#define PROBE_STEPS 96u
// Coarsen distant geometry so the long tail of each ray stays cheap. Same values the cascades used.
#define PROBE_FINISH_LOD  1
#define PROBE_LOD_DIST    45

// Sun-shadow ray budget on a gather hit. Fewer steps is SAFE: a ray that runs out returns a MISS
// (== lit), so shortening it errs toward slightly over-lighting long-shadowed nooks rather than the
// over-darkening a coarse LOD would cause -- which is why the LOD stays finest here.
#define PROBE_SHADOW_STEPS 40u

// ---- THE GATHER DOES NOT ASK FOR ANIMATION, AND THAT IS THE WHOLE OF IT ---------------------
//
// Whether a blade is at its rest position or a voxel or two away is not a distinction indirect light
// can carry. So neither the gather nor its shadow ray sets PJV_Q_ANIMATION, and the geometry march
// then draws animated voxels where they are STORED -- one traversal, no envelope, no resolve.
//
// This used to be a `resolveDistance` argument to a forked traversal, with 0 meaning the same thing.
// It is a flag the query simply does not set now, which is the difference the promotion was for.

// Lift the ray origin this many VOXELS off the face along N before tracing. A near-tangent ray then
// skims ABOVE the face's own coplanar relief -- clearing the false self-occlusion streak with no
// per-hit test -- while a wall standing perpendicular on the surface still rises into the lifted ray
// and blocks it, so nothing leaks. Inherited from the cascade gather, where it was load-bearing for
// exactly the same reason: every probe on a face shares one origin, so a false self-hit is not a
// speckle on one pixel, it is the whole face going dark at once.
#define PROBE_ORIGIN_LIFT 0.5

// Luminance ceiling on a SINGLE ray's returned radiance. With only PROBE_RAYS samples in a frame,
// one ray catching a brilliant sunlit bounce lands in the mean at 1/PROBE_RAYS weight and pops the
// whole face. Clamping per ray (rather than the frame's mean, as resolve used to) kills the outlier
// before it is averaged in, and preserves hue by scaling rather than clipping channels.
#define PROBE_FIREFLY_MAX 6.0

// Feed each ray's hit point back through last frame's accumulated indirect, turning one bounce into
// all of them. 0 falls back to single-bounce (visibly darker interiors).
#ifndef PROBE_MULTIBOUNCE
#define PROBE_MULTIBOUNCE 1
#endif

// How far a second-bounce lookup's own G-buffer position may sit from the hit point before the
// lookup is rejected as "that pixel is not this surface". In voxels: the reprojected hit is only
// valid if the thing the camera sees at that pixel IS the thing the ray hit, and this is the
// occlusion test that decides it.
#define PROBE_REPROJ_TOL 1.5

// ---- The sample sequence ----------------------------------------------------
// A face's directions must be identical for every pixel that sees it (so the face is flat and
// camera-independent) and well spread both within a frame and across frames (so the temporal mean
// converges rather than orbiting a few directions). That is two different requirements and they are
// met by two different halves of the same expression.
//
// ACROSS SAMPLES: the R2 low-discrepancy sequence. Its two irrational increments fill the unit
// square far more evenly than a hash at the small sample counts a young face has, which is exactly
// when evenness matters -- a hash gives clumps, and a clump of directions is a wrong answer that
// takes many frames to average away.
//
// PER FACE: a hash of the face key, added as a CYCLIC OFFSET. Two adjacent faces therefore walk the
// same evenly-spread sequence from unrelated starting points, so neither their per-frame error nor
// their convergence is correlated -- if they shared an offset, a whole wall would flicker in unison
// and the eye would read it as the light itself changing.
#define PROBE_R2_X 0.7548776662466927
#define PROBE_R2_Y 0.5698402909980532

// Hash a face key to a stable per-face offset in [0,1)^2. Integer-exact input (see pjv_face_key.sc),
// so the offset a face gets is the same one every frame for the whole run.
vec2 pjvProbeKeyOffset(vec4 key) {
    // Fold the four components into one number first: hashing them independently would let two faces
    // that share three components share an axis of the offset, and voxel faces come in exactly that
    // kind of run.
    float f = key.x * 12.9898 + key.y * 78.233 + key.z * 37.719 + key.w * 4.7853;
    return fract(vec2(sin(f) * 43758.5453, cos(f) * 27183.1234));
}

// Sample i of this face's sequence, at this frame, for one probe texel.
//
// stratumOffset is a SECOND cyclic offset, per probe TEXEL rather than per face. Several quarter-res
// texels usually land on one face, and without this they would all draw the identical directions and
// so all compute the identical number -- perfectly consistent, and a complete waste of the other
// texels' rays. Offsetting them instead makes each an independent estimate of the same quantity, and
// resolve.frag averages every texel that matched this face. A face covered by nine texels is
// therefore sampled at nine times the ray budget in a single frame, which is the difference between
// a face that visibly pulses frame to frame and one that does not.
//
// The frame index is wrapped (see PROBE_FRAME_PERIOD) before it reaches here. R2's increments are
// irrational, so fract() of R2 * index is only meaningful while index * R2 stays small enough for
// float32 to resolve its fractional part -- by index ~100k there are barely a hundred distinct
// values left and the "sequence" collapses into a handful of directions. Wrapping keeps it honest
// at the cost of repeating a 1024-direction cycle, which no accumulation window is long enough to
// notice.
vec2 pjvProbeSample(vec2 keyOffset, vec2 stratumOffset, float sampleIndex) {
    return fract(keyOffset + stratumOffset + vec2(PROBE_R2_X, PROBE_R2_Y) * sampleIndex);
}

// Frames before the direction sequence repeats. See pjvProbeSample for why it must wrap at all.
#define PROBE_FRAME_PERIOD 256.0

// A probe texel's cyclic offset into the sequence, from its position in the probe buffer. Any
// well-scattered function of the texel does; this is the same hash shape as the key offset so that
// neighbouring texels land nowhere near each other.
vec2 pjvProbeStratum(ivec2 probeTexel) {
    float f = float(probeTexel.x) * 21.9898 + float(probeTexel.y) * 61.233;
    return fract(vec2(sin(f) * 21387.6543, cos(f) * 15731.9871));
}

// Cosine-weighted direction in the hemisphere about N. Cosine-weighted is what makes the estimator a
// plain mean: the pdf is cos/PI, the integrand carries a cos, and the two cancel, so no per-sample
// weight has to be tracked and one dark ray cannot be scaled up into a firefly by a small pdf.
vec3 pjvProbeCosineDir(vec3 N, vec2 u) {
    float r   = sqrt(u.x);
    float phi = 2.0 * PI * u.y;
    // Any tangent will do as long as it is not parallel to N; picking the axis N is LEAST aligned
    // with keeps the cross product well conditioned. Voxel normals are exactly axis-aligned, which is
    // the degenerate case a naive fixed up-vector gets wrong.
    vec3 t  = abs(N.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tx = normalize(cross(t, N));
    vec3 ty = cross(N, tx);
    return normalize(tx * (r * cos(phi)) + ty * (r * sin(phi)) + N * sqrt(max(0.0, 1.0 - u.x)));
}

// ---- Shading a gather hit ---------------------------------------------------
// Hard sun shadow ray from a bounce point. Visibility only, and at the FINEST LOD on purpose: a
// coarse-LOD shadow ray tests against merged boxes that over-occlude, producing a false shadow and
// so a dim bounce, which is the classic cause of a world-space gather coming out too dark. Cost is
// cut through the step count instead.
//
// The bug this used to have is worth keeping, because it was silent and it was expensive to be wrong
// in the cheap direction. When the envelope lived INSIDE the geometry tree, the swept volume around
// every blade was opaque to this ray: a bounce point in or near grass reported "shadowed" immediately
// and contributed no bounced sunlight at all -- a whole grass field's worth of indirect light
// missing, while the ray terminated on the first shell voxel it met, so nothing about the frame time
// pointed at it. An adjacent envelope cannot cause that: this ray sees the rest pose and nothing else.
bool pjvProbeSunVisible(vec3 p, vec3 n) {
    Ray shadow;
    shadow.origin    = p + n * (0.02 * WORLD_SCALE);
    shadow.direction = SUN_DIR;
    RayQuery rq = pjvPrimaryQuery(100u);
    rq.maxRaySteps = PROBE_SHADOW_STEPS;
    rq.startLOD = 0u; rq.finishLOD = 0u; rq.distanceToFinishLOD = 100000u;
    // rayT < 0 == genuine miss == lit.
    //
    // NO PJV_Q_ANIMATION, which is the engine's spelling of the resolve distance this used to pass:
    // with animation off the geometry march stops skipping animated voxels and draws them at their
    // REST position, so a blade still blocks this ray, it is just not asked which voxel it occupies
    // this frame. A bounce's shadow does not need that answer, and resolving it is the expensive half.
    return raySceneIntersectFrom(shadow, rq, 0.0).rayT < 0.0;
}

// Last frame's converged indirect at a world point, found by projecting it into the camera. Returns
// false when the point is off-screen or hidden, which is most of the time on a ray that went behind
// something -- the caller then gets one bounce there and the full solution everywhere visible.
//
// This is a screen-space lookup for a world-space quantity, and it is sound here only because what
// it reads is per-FACE: the value at that pixel belongs to the face the ray hit, not to the pixel,
// so borrowing it across a reprojection loses nothing as long as the pixel really is that face.
// The distance test below is what establishes "really is".
#if PROBE_MULTIBOUNCE
bool pjvProbeSecondBounce(vec3 hitP, float voxelSize, out vec3 indirect) {
    indirect = vec3(0.0);
    bool valid;
    vec2 uv = worldToUV(hitP, cameraPos.xyz, cameraDir.xyz, passInputRes[0].xy, FOV, valid);
    if (!valid) return false;

    // Is the surface the camera sees here the surface the ray hit? Point-sampled: a bilinear read
    // straddling a depth discontinuity returns a position on neither surface and would pass a test
    // that both should fail.
    vec2  snapped = pjvSnapToTexel(uv, passInputRes[0].xy);
    vec4  gp = texture2D(gPos, snapped);
    if (gp.a < 0.0) return false;                                  // sky pixel
    if (distance(gp.xyz, hitP) > PROBE_REPROJ_TOL * voxelSize) return false;

    indirect = max(texture2D(prevIndirect, snapped).rgb, vec3(0.0));
    return true;
}
#endif

// Radiance arriving along one gather ray from (P, N). This is the integrand; the caller averages.
//
// ---- THE GATHER IS ONE TRAVERSAL, AND THE HISTORY IS WORTH KEEPING ---------------------------
//
// Two arrangements preceded this one and both were structurally expensive rather than badly tuned.
//
// FIRST, this called the plain engine march and then stepped past each envelope voxel it landed on by
// re-issuing the query with a larger tMin. The prototype dilated the swept volume into the GEOMETRY
// tree, so a march that treated it as solid found a wall a few voxels from every blade in every
// direction and the grass came out pitch black -- skipping was not optional. But every skip was a
// fresh scene query (loose-chunk broadphase, grid slab tests, grid DDA, descent from the root), tMin
// advances past exactly ONE cell per restart, and the shell around a blade is one to two voxels
// thick. The skip budget was spent within a few voxels of the origin, at up to seven full traversals
// per ray -- and then it FAILED, because exhausting the loop falls through to the sky return below,
// so a ray that could not punch through the grass reported the brightest answer available after
// paying seven traversals for it.
//
// SECOND, it called the prototype's forked envelope march, which stepped through scaffolding inside
// its own DDA. Correct and one traversal, at the cost of a second copy of the entire scene query
// living in this example.
//
// NEITHER IS NEEDED NOW. The envelope is an adjacent structure, so the geometry tree holds the rest
// pose and nothing else: a query that does not set PJV_Q_ANIMATION sees ordinary geometry, in one
// traversal, with no scaffolding to see through and no fork to maintain. The loop below survives for
// one unrelated reason.
//
// ---- WHY THIS IS STILL A LOOP ------------------------------------------------------------------
// Only for DEGENERATE HITS -- rayT >= 0 with a zero normal -- which mean the ray started inside solid,
// usually a false grazing self-hit into the surface's own relief. There is no valid normal to shade
// with, and the cascade gather this replaces learned the hard way that the answer is NOT to return
// black: "Stamping opaque black here produced coherent black bands; treat it as transparent instead
// -- a false near-occlusion should pass light, not paint black."
//
// Advancing the ORIGIN rather than raising a tMin, which is normally the wrong way round here -- an
// advanced origin can land inside the next voxel or exactly on a boundary, and this marcher is
// degenerate at both. It is acceptable only because the case is RARE: the envelope, which was the
// reason this loop ran at all, is no longer visible to this query. The epsilon past exitT is scaled by
// the hit's own box so it clears the boundary at any voxel size, and the budget is small because
// anything needing more than a couple of retries is stuck rather than progressing.
#define PROBE_MAX_SKIPS 2
#define PROBE_SKIP_EPS  0.05

vec3 pjvProbeTraceRay(vec3 P, vec3 N, vec3 dir, float voxelSize) {
    float lift = max(PROBE_ORIGIN_LIFT * voxelSize, NORMAL_BIAS);

    Ray ray;
    ray.origin    = P + N * lift;
    ray.direction = dir;

    RayQuery rq = pjvPrimaryQuery(100u);
    rq.maxRaySteps         = PROBE_STEPS;
    rq.startLOD            = 0u;
    rq.finishLOD           = uint(PROBE_FINISH_LOD);
    rq.distanceToFinishLOD = uint(PROBE_LOD_DIST);
    // The traversal had the leaf in registers when it decided to stop, so asking it for the material
    // is strictly cheaper than the descent this used to do afterwards. See PJV_Q_WANT_MATERIAL.
    pjvQueryWantMaterial(rq);

    for (int skip = 0; skip <= PROBE_MAX_SKIPS; skip++) {
        // Animation is NOT requested -- see pjvProbeSunVisible for why -- so animated geometry is
        // gathered at its rest position. That is a deliberate approximation and a cheap one: a bounce
        // ray does not care which voxel a blade occupies this frame, only that a blade is there.
        SceneHit sh = raySceneIntersect(ray, rq);
        SceneIntersectData h = sh.hit;

        // Escaped the scene (or ran out of steps, which reports the same way): the true sky. This is
        // the dominant light source outdoors and the reason the gather has to be world-space at all --
        // a screen-space march cannot tell "blocked" from "sees sky" and defaults to sky, which
        // flattens the whole image.
        if (h.rayT < 0.0 || h.foundBox.size < 0.0) break;

        // Started inside solid: no usable normal. Step the origin past the cell and carry on. A step
        // that cannot advance means the march is stuck, and continuing would spin the loop for nothing.
        if (dot(h.normal, h.normal) < 0.5) {
            float advance = h.exitT + max(h.foundBox.size, voxelSize) * PROBE_SKIP_EPS;
            if (advance <= 0.0) break;
            ray.origin += dir * advance;
            continue;
        }

        // Straight off the march's own decision, never a re-descent from a rebuilt voxel coordinate:
        // that float32 round trip mis-shades a growing percentage of voxels with their neighbour's
        // colour as a chunk moves away from the origin, and is simply wrong once a chunk is rotated. A coarsened hit names no single material, and the
        // fallback descends on its minimum corner -- which is the behaviour a coarsened hit has always
        // had here.
        //
        // NOTE what is no longer needed: the scaffolding skip. The prototype dilated the envelope into
        // the GEOMETRY tree and spent a palette entry on it, so every blade sat inside a sealed opaque
        // box and this gather had to step past each one by hand or return black. The envelope is an
        // adjacent structure now and a renderer that does not ask for animation cannot tell it exists,
        // so there is nothing here to see through.
        VoxelMaterial m = sh.materialValid ? sh.material : fetchVoxelMaterialFromHit(h);

        vec3 hP = ray.origin + dir * h.rayT;
        vec3 hN = normalize(h.normal);

        // A metal has no diffuse lobe, so it bounces nothing on its own account; what it contributes
        // is its emission. Same subtraction the editor's preview and Render mode make.
        vec3 albedo = m.albedo * (1.0 - m.metallic);

        // Bounced sunlight leaving the hit surface. albedo/PI * NdotL * sun, shadowed.
        vec3 L = vec3(0.0);
        float NdotL = dot(hN, SUN_DIR);
        if (NdotL > 0.0 && pjvProbeSunVisible(hP, hN)) {
            L += (albedo / PI) * NdotL * SUN_COLOR;
        }

        // Emission is not scaled by albedo and does not care where the sun is: this is what makes an
        // emissive voxel an actual GI light source rather than just a bright pixel.
        L += m.emission;

        // ...and everything that surface is itself receiving indirectly. See the note on the function.
        #if PROBE_MULTIBOUNCE
        vec3 bounce;
        if (pjvProbeSecondBounce(hP, h.foundBox.size, bounce)) L += albedo * bounce;
        #endif

        // Per-ray firefly clamp -- see PROBE_FIREFLY_MAX. Scale, do not clip, so hue survives.
        float lum = dot(L, vec3(0.299, 0.587, 0.114));
        if (lum > PROBE_FIREFLY_MAX) L *= PROBE_FIREFLY_MAX / lum;
        return L;
    }

    #if SKY_GI
    return skyGradient(dir);
    #else
    return vec3(0.0);
    #endif
}

// ---- The gather -------------------------------------------------------------
// Cosine-weighted mean radiance R over the hemisphere of face (P, N), from PROBE_RAYS samples of
// this face's own sequence at this frame. E = PI*R and diffuse Lo = albedo/PI * E = albedo * R, so
// what comes out of here multiplies straight into albedo downstream with no further constants.
// The six axis directions, indexed the way pjvFaceId numbers them (0..5 = -X +X -Y +Y -Z +Z), so
// "face i" means the same thing here as it does in a key.
vec3 pjvAxisNormal(int faceId) {
    int axis = faceId / 2;
    float sgn = (faceId - axis * 2) == 1 ? 1.0 : -1.0;
    return axis == 0 ? vec3(sgn, 0.0, 0.0)
         : axis == 1 ? vec3(0.0, sgn, 0.0)
                     : vec3(0.0, 0.0, sgn);
}

// `P` and `N` describe the face this probe stands on. `voxelCentre` is that voxel's centre, which
// only the per-voxel path uses -- see PJV_PER_VOXEL_LIGHTING in pjv_face_key.sc.
vec3 pjvProbeGather(vec3 P, vec3 N, vec3 voxelCentre, float voxelSize, vec4 faceKey,
                    ivec2 probeTexel) {
    vec2  keyOffset = pjvProbeKeyOffset(faceKey);
    vec2  stratum   = pjvProbeStratum(probeTexel);
    // Every face advances by the same stride each frame, so the sequence a face is on is a pure
    // function of (key, texel, frame) -- no per-pixel state, nothing to reproject, and two pixels
    // that disagree about which face they are on cannot end up averaging different frames together.
    float base = mod(floor(frameCount.x), PROBE_FRAME_PERIOD) * float(PROBE_RAYS);

    vec3 acc = vec3(0.0);
    for (int i = 0; i < PROBE_RAYS; i++) {
        vec2 u = pjvProbeSample(keyOffset, stratum, base + float(i));

        // ---- PER-VOXEL: integrate the whole voxel, not one of its faces ------------------------
        // The quantity wanted is the mean irradiance over the voxel's SURFACE. All six faces have
        // equal area, so that mean is the plain average of the six face irradiances -- and picking a
        // face per ray, then cosine-sampling its hemisphere from ITS centre, is an unbiased estimator
        // of exactly that average. No new trace, no new weighting, and the same ray budget.
        //
        // The face is chosen by the ray's own sample rather than by a separate random stream: u.x is
        // already a well-stratified value in [0,1) that advances with the frame, so slicing it into
        // six gives each face a turn in a fixed rotation instead of clumping. u.x is then rescaled
        // back to fill [0,1) so the cosine sample below still gets a full-range pair -- dropping that
        // rescale would confine every ray to one sixth of the azimuth.
        vec3 rayN = N;
        vec3 rayP = P;
        if (PJV_PER_VOXEL_LIGHTING) {
            float scaled = u.x * 6.0;
            int   faceId = int(min(floor(scaled), 5.0));
            u.x   = scaled - float(faceId);
            rayN  = pjvAxisNormal(faceId);
            rayP  = voxelCentre + rayN * (voxelSize * 0.5);
        }

        vec3 dir = pjvProbeCosineDir(rayN, u);
        acc += pjvProbeTraceRay(rayP, rayN, dir, voxelSize);
    }
    vec3 R = acc / float(PROBE_RAYS);

    // Defensive: a NaN reaching the temporal mean is permanent -- it poisons the running average and
    // every neighbourhood search that later borrows from it.
    if (any(isnan(R)) || any(isinf(R))) return vec3(0.0);
    return max(R, vec3(0.0));
}

#endif // PJV_PROBE_SC
