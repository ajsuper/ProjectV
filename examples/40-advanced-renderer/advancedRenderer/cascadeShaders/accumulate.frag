$input v_color0
$input v_texcoord0

// =============================================================================
// accumulate.frag  --  Pass 7. Temporal mean of the indirect irradiance R.
//
// The cascade GI is largely deterministic, but with ~16 directions per probe and a per-frame
// sub-tile jitter it carries mild angular banding. This pass averages R over time to smooth it.
// R alone -- direct sun, emission and albedo are all kept out of it upstream, so what is being
// filtered here is the one term in the frame that is genuinely low frequency.
//
// -----------------------------------------------------------------------------
// THE HISTORY GATE, AND WHY IT HAS TWO HALVES
// -----------------------------------------------------------------------------
// Deciding whether a pixel may reuse last frame's value is the whole difficulty of a temporal
// filter, and a per-face renderer has a sharper tool for it than a per-pixel one: the exact voxel
// face this pixel is looking at, carried in gKey. Two pixels with the same key are looking at the
// same face and its indirect light is the same number, whatever either of them did last frame.
//
// The obvious thing -- gate on the exact key and nothing else -- was tried in this renderer's
// ancestor and had to be backed out, for a reason worth writing down because it is not obvious:
//
//   Under camera motion, a pixel SWEEPS ACROSS MANY FACES of the same wall -- especially on far or
//   oblique surfaces, where many voxels project into one pixel. The exact key then changes every
//   single frame, the test fails every frame, history is discarded every frame, and the raw noisy
//   sample is re-exposed. Accumulation worked while still and vanished the moment you moved.
//
// The fix is not to weaken the key, it is to notice that the two regimes want different tests:
//
//   BIG FACES (several pixels across) -- the key is stable frame to frame and is the best gate
//   there is. It cannot cross to a neighbouring face, so no light leaks between voxels, and the
//   neighbourhood search below finds the face's converged value even when reprojection lands a
//   texel or two off.
//
//   SUB-PIXEL FACES -- the key is meaningless, but the SURFACE is not. Gate geometrically instead:
//   same face orientation (the key's own face id supplies it -- six axes, no stored normal needed),
//   same plane at about the same depth, no gross positional jump. That keeps the wall's converged
//   GI alive as the pixel slides across its voxels.
//
// So: exact key preferred, geometry as the fallback. Which regime a pixel is in is not a decision
// this pass has to make -- it searches for an exact match first and takes a geometric one only if
// no exact match exists, which is precisely the condition that distinguishes them.
//
// -----------------------------------------------------------------------------
// AND WHY AN INTERPOLATED SAMPLE IS NOT ALLOWED TO MOVE A CONVERGED FACE
// -----------------------------------------------------------------------------
// resolve flags each sample: alpha 1 means it came from a probe standing on this very face, alpha 0
// means it had to be interpolated from probes on OTHER faces because this one owned none. The first
// is camera-independent, the second is not -- see resolve.frag for why. Blending them into the same
// running mean is what made the lighting change as the camera moved: the interpolated value drifts
// with the viewpoint and drags the mean along with it.
//
// So an interpolated sample may BOOTSTRAP a face that has never been sampled properly, and stops
// counting the moment one has been. After that the stored value is held until a probe lands on the
// face again. Because every such landing computes the same number, a face's value stops depending
// on where you are standing and starts merely being refreshed at irregular intervals, which is
// invisible.
//
// "Has never been sampled properly" is a per-face state, and it is carried in the SIGN of the age.
// A negative age means no probe has ever landed here and interpolated samples still count; the
// first exact sample flips it positive and they stop. It has to be a state rather than "was the
// first frame a bootstrap", because the first frame is exactly when it must not be trusted: the
// cascade textures have not been rendered yet, so frame zero's interpolation is of nothing at all.
// Holding that would freeze every under-sampled face at black, permanently, which is precisely
// what happened when this rule was first written without the state.
//
// The cost of holding is staleness, and it has exactly one case that matters: the sun moving while
// a face owns no probe. That is handled explicitly rather than by leaking interpolated samples back
// in -- a lighting change knocks the age down (LIGHT_CHANGE_AGE) so the next samples rebuild fast.
//
// -----------------------------------------------------------------------------
// THE GATE ABOVE HAD NEVER ACTUALLY RUN
// -----------------------------------------------------------------------------
// Worth recording, because everything below was written for a test that was being silently skipped.
//
// This pass binds ten input textures. bgfx addresses a texture binding by STAGE and the later call
// on a stage replaces the earlier, and the engine parks the scene's own textures on stages 9..15 --
// so gKey, as the tenth input, landed on stage 9 underneath materialIDs. SAMPLER2D(gKey, 9) was
// reading a uint scene texture through a float sampler. curKey was garbage, histKey was last frame's
// copy of the same garbage, and the identity half of the gate collapsed into a comparison of two
// meaningless numbers that happened to agree.
//
// What was left running was the geometric fallback alone -- which is precisely the weaker test this
// file's header explains it is not safe to rely on, and it is why an earlier edit concluded the exact
// key was "fragile" and deleted the exact-match pass from the moving branch. It was not fragile; it
// was disconnected. performRenderPasses now binds the scene FIRST so a pass's own inputs win the
// collision (see the note there), and the exact-match pass is restored below.
//
// This pass still overruns the nine-input budget, deliberately. It is safe here because it never
// traces a ray, so the materialIDs binding it costs is one it would not have used -- but the engine
// warns about it at startup, and that warning is correct: a pass that DID trace a ray must stay
// inside nine inputs.
//
// Inputs (FBO 6 [0], FBO 7 [1..3], FBO 1 [4..9]):
//   0 indirect  1 accumR(hist) 2 histKey 3 histPos  4 gPos 5 gNormal 6 gAlbedo 7 gDirect
//   8 gFace 9 gKey
// Outputs (FBO 7, ping-pong):
//   0 accumR   rgb = converged R, a = age
//   1 histKey  this frame's face key, for next frame's gate
//   2 histPos  this frame's world position + camera depth, for the geometric fallback
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(indirect, 0);
SAMPLER2D(accumR,   1);
SAMPLER2D(histKey,  2);
SAMPLER2D(histPos,  3);
SAMPLER2D(gPos,     4);
SAMPLER2D(gNormal,  5);   // FBO1[1]: a = the hit voxel's edge length, the unit the sway tolerance uses
SAMPLER2D(gAlbedo,  6);   // FBO1[2]: a = 1 on an ANIMATED surface (see gbuffer.frag)
SAMPLER2D(gKey,     9);

uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;

// Must match the cascades' face-anchored probe spacing (unused here directly, but the header's
// probe helpers are compiled with it and a mismatch would be a trap for anyone editing this file).
#define PROBE_SPACING0 16
// This pass never addresses a cascade atlas -- it works entirely on screen-resolution buffers --
// but the header requires both grids to be named rather than guessed at, so both are its own.
#define CASCADE_SCREEN_RES passTargetRes.xy
#define CASCADE_ATLAS_RES  passTargetRes.xy
#include <pjv_cascade_common.sc>
#include <pjv_face_key.sc>

// Cap on the still-camera running mean. This used to be unbounded, which sounds better and is not:
// once a pixel reached an age in the hundreds a new sample blended in at 1/age ~ 0 and the GI was
// frozen, so a real lighting change took a quarter of a minute to appear. Capped, the mean stays
// very smooth and still responds.
#define STILL_MAX_AGE 128.0
// Shorter while moving, where the reprojection is approximate and staleness shows.
#define MOVING_MAX_AGE 50.0
// Texels searched around the reprojected point for a matching history texel. 2 = a 5x5 window.
// Voxel silhouettes are a staircase of micro-faces, so the exact reprojected texel often lands on
// the wrong one; the face this pixel wants is usually a texel or two away, holding its converged
// value. Borrowing it fills the edge with a stable number instead of a fresh noisy one.
#define SEARCH_RADIUS 2
// Where a face's age is knocked back to when the LIGHT changes. Low enough that the next few
// samples dominate the mean, so a sun move takes effect in a handful of frames rather than over the
// full window -- and it is the only thing that refreshes a face which currently owns no probe.
#define LIGHT_CHANGE_AGE 4.0

// ---- ANIMATED SURFACES ------------------------------------------------------------------------
// A swaying blade of grass defeats BOTH halves of the gate above, and it is worth being precise
// about why, because it looks like a bug in the accumulator and is not one.
//
// The blade is not geometry that moves; it is geometry that is CARVED OUT OF A STATIC ENVELOPE at
// shade time (the envelope resolve in pjv_utils_DDA.sc). So from one frame to the next the
// primary ray lands on a
// different envelope voxel and a differently-oriented piece of blade. gKey changes -- so the exact
// match fails -- and the face id packed in it changes too, so sameSurface's same-facing test fails
// as well. Every frame, for every blade pixel, history is discarded and the raw gather is re-exposed.
//
// Rendering the temporal age as a ramp shows this exactly: static rock reaches STILL_MAX_AGE and sits
// there, while every grass and tree pixel is pinned at 1. It was survivable under the cascades, whose
// per-frame output was deterministic; against a stochastic gather it is the whole of the flicker.
//
// The fix is to gate animated pixels on POSITION ALONE, within a few voxels. What that averages is
// no longer "the light on this face" but "the light in this small patch of air", which is the right
// quantity for foliage anyway -- a blade's indirect light is ambient, it is not a property of which
// particular voxel the blade happens to occupy this frame.
//
// Fallback radius for an animated pixel whose VOXEL identity did not match -- in practice fire,
// whose parcels advect through space and have no fixed source voxel to key on. Kept tight and paired
// with the neighbourhood clamp: this was 6 and unclamped, which reached across whole blades and
// borrowed light from foliage that was genuinely differently lit, visible as the moving grass
// SMEARING its old brightness along behind it. A loose radius and no clamp is what ghosting is made
// of. Grass no longer reaches this path at all -- see pjvSameVoxel.
#define ANIM_POS_TOL_VOXELS 2.5
// Fire's own radius. Wider because a parcel ADVECTS -- it is somewhere else next frame, by design,
// where a blade merely leans -- so the tolerance has to cover a frame's travel or the gate rejects
// every time and fire accumulates nothing at all. Paired with the neighbourhood clamp, which is what
// stops a radius this loose from smearing the flame upward behind itself.
#define FIRE_POS_TOL_VOXELS 6.0
// And a shorter mean than the sway gets: a flame's light genuinely changes fast, so a long average
// is not denoising it, it is lagging it into a smear.
#define FIRE_MAX_AGE 12.0
// Animated surfaces cap lower than static ones: their light really does change as they move, so a
// long mean lags the sway instead of denoising it. Long enough to kill the noise, short enough to
// follow.
#define ANIM_MAX_AGE 32.0

// ---- SILHOUETTE EDGES -------------------------------------------------------------------------
// A pixel on a voxel's silhouette straddles two faces of DIFFERENT orientation, and the sub-pixel
// jitter lands it on one or the other from frame to frame. Neither gate above can hold: the exact
// key changes, and sameSurface's same-facing test fails precisely because the two faces face
// different ways. So the pixel reset to age 1 every frame and re-exposed the raw gather -- a bright
// crawling fringe along every voxel edge, which is the "flicker along voxel edges, as if it had to
// do with the jitter" exactly.
//
// The value such a pixel WANTS is the coverage-weighted mix of the two faces, and a running mean
// over jittered frames is precisely how to compute that -- but only if the history is allowed to
// survive the flip. So there is a third tier: same point in space, any facing. It is deliberately
// the tightest radius of the three, because "different face at the same point" is only innocent
// when the point really is the same one.
#define EDGE_POS_TOL_VOXELS 1.5
// Capped well below a static face: this mean is averaging two different faces together on purpose,
// and how much of each depends on where the geometry sits under the pixel, so it must re-settle
// quickly when that changes.
#define EDGE_MAX_AGE 24.0
// ...but that reasoning has a range, and past the crossover it inverts.
//
// It assumes the coverage mix is volatile: with a voxel a few pixels across, a small shift in the
// geometry under the pixel changes how much of each face it sees by a lot, so a long mean would lag.
// Once the voxel is SMALLER than the pixel, the pixel contains many faces rather than two, the jitter
// samples them in proportion to their coverage, and that proportion is statistically stationary under
// a still camera -- there is nothing left for a short mean to stay responsive to, and averaging more
// of it is strictly closer to the right answer.
//
// This matters far more than it used to. The edge tier was a rare fallback when it was written; once
// the tolerance below is measured in pixel footprints, it is the ONLY tier that can match past the
// crossover, so its cap is the far field's entire history length. 24 frames of an 8-ray gather is
// visibly noisy; 128 is what the resolved case already gets and there is no reason the far field
// should get less.
#define EDGE_SUBPIXEL_MAX_AGE 128.0

// World size of one output pixel at a given depth -- the scale that decides which of the two regimes
// above a pixel is in, and the same quantity taa.frag and upscale.frag measure themselves against.
float pixelFootprint(float curDepth) {
    return curDepth * (2.0 * tan(radians(FOV * 0.5))) / passTargetRes.y;
}

// How completely the source grid resolves a voxel of this size at this depth: 0 sub-pixel, 1 several
// pixels across. Same 0.75..2.0 crossover the other passes fade over, so the whole renderer changes
// regime at one distance rather than three.
float geometryResolved(float voxelSize, float curDepth) {
    return smoothstep(0.75, 2.0, voxelSize / max(pixelFootprint(curDepth), 1e-6));
}

// How many PIXEL FOOTPRINTS of separation still count as the same spot, once the voxel grid is finer
// than the pixel grid. A jittered ray stays inside its own pixel by construction, so one footprint is
// the whole range the hit point can move without anything having changed; the margin above that
// covers an oblique surface, where the same angular offset slides further along the plane.
#define SPOT_TOL_PIXELS 3.0

// Near enough to be the same point on the surface, whatever face the jitter happened to land on.
//
// The tolerance is the LARGER of a voxel count and the pixel's own world footprint, and carrying both
// is what makes this tier work at every distance rather than only up close.
//
// A voxel count is the right unit while a voxel is bigger than a pixel: the jitter then moves the hit
// point by a fraction of a voxel, and a voxel and a half is a generous bound on "the same spot".
//
// It is the wrong unit as soon as the projection inverts. Far away one pixel covers MANY voxels, so a
// +-0.5px jitter lands on a genuinely different voxel every frame and displaces the hit point by
// several voxels of world distance -- routinely past 1.5 of them. This tier, the one written for
// "different face, same point", then rejected every distant pixel, and the indirect term fell back to
// a raw gather every frame. That is the far-field flicker: not a different failure from the close-up
// one that the identity search fixed, but the same failure measured in a unit that stops applying.
// Keying on the face cannot rescue it either -- at that scale the face genuinely IS different each
// frame, so there is no identity left to match on and position is all that remains.
//
// Scaling by the footprint says the physically true thing: everything within one pixel footprint is
// inside this pixel, and therefore part of what this pixel sees. Its correct value is the
// coverage-weighted mix of all of it, and a running mean over jittered frames is exactly how that
// mix is computed -- provided the history is allowed to survive. Meanwhile a real disocclusion moves
// the hit point by a depth discontinuity, which is orders of magnitude beyond a footprint, so this
// stays as good a rejector of those as it ever was. The tier is still clamped to the current
// neighbourhood by its caller, which bounds whatever it does let through.
bool sameSpot(vec4 hPos, vec3 curP, float voxelSize, float curDepth, float tolVoxels) {
    if (hPos.a < 0.0) return false;                       // that texel was sky
    float footprint = curDepth * (2.0 * tan(radians(FOV * 0.5))) / passTargetRes.y;
    float tol = max(tolVoxels * max(voxelSize, 1e-4), SPOT_TOL_PIXELS * footprint);
    return distance(hPos.xyz, curP) < tol;
}

// ---- NEIGHBOURHOOD CLAMP ----------------------------------------------------------------------
// Bound a history value to the range the CURRENT frame actually shows nearby, the standard cure for
// a temporal filter that reuses across a loose gate. Where the gate matched on position rather than
// identity, "the same patch" is a guess, and a wrong guess drags a stale brightness along behind
// moving geometry for as many frames as the mean is long.
//
// Applied ONLY on the loose tiers. Clamping an exact-face match would be actively harmful: the box
// is built from the raw per-frame gather, which at 8 rays is noisy, so clamping a converged
// 128-frame mean into it would inject that noise straight back and undo the accumulation this whole
// design rests on. Identity matches need no such guess and get no clamp.
vec3 clampToNeighbourhood(vec3 history, vec2 uv) {
    vec3 lo = vec3(1e9), hi = vec3(-1e9);
    float n = 0.0;
    for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++) {
        vec2 sUV = uv + vec2(float(dx), float(dy)) * passTargetRes.zw;
        // SKY IS NOT A NEIGHBOUR VALUE. resolve.frag writes exactly zero indirect on a background
        // texel -- correctly, since the sky has no bounce to gather -- but that zero is not a plausible
        // reading of the surface this pixel is on, and letting it into the box drops `lo` to black.
        // A converged history beside a silhouette was then clamped toward zero, darkening the indirect
        // term along every voxel-to-sky edge in the frame. Same mistake as the one in compose.frag's
        // filteredIndirect, in the other direction: there sky taps were admitted by an interpolated
        // depth, here by not being asked about at all.
        if (texture2D(gPos, sUV).a < 0.0) continue;
        vec3 s = texture2D(indirect, sUV).rgb;
        lo = min(lo, s);
        hi = max(hi, s);
        n += 1.0;
    }
    // Every neighbour was sky: nothing to bound against, so leave the history as it is rather than
    // clamping it into an empty box (lo > hi, which would pin it to a corner of nonsense).
    return n > 0.0 ? clamp(history, lo, hi) : history;
}

// The geometric fallback: is a history texel on a surface this pixel could legitimately inherit
// from? Same facing (from the key's face id, so no normal has to be stored), close to this pixel's
// tangent plane, and not a gross positional jump.
bool sameSurface(vec4 hKey, vec4 hPos, vec4 curKey, vec3 curP, vec3 curN, float curDepth) {
    if (hKey.w >= FACE_KEY_NONE || curKey.w >= FACE_KEY_NONE) return false;
    // Same one of the six axes. mod(w, 6) recovers the face id from the packed chunk+face word.
    if (mod(hKey.w, 6.0) != mod(curKey.w, 6.0)) return false;
    // On this pixel's plane: the off-plane distance is what separates a parallel wall behind an
    // edge from the same wall one voxel over. Scaled by depth because a fixed world tolerance is
    // too tight up close and too loose across a room.
    float planeTol = max(0.5, curDepth * 0.01);
    if (abs(dot(hPos.xyz - curP, curN)) > planeTol) return false;
    // And not somewhere else entirely.
    float posTol = max(2.0, curDepth * 0.05);
    return distance(hPos.xyz, curP) < posTol;
}

void main() {
    vec2 uv  = v_texcoord0;
    vec4 curSample = texture2D(indirect, uv);
    vec3 cur = curSample.rgb;
    // resolve still tags each sample with whether it came from a probe on this pixel's own face. That
    // distinction drove the per-face store and is deliberately ignored here; the tag is left in place
    // because resolve is unchanged and it costs nothing to carry.
    vec4 gp  = texture2D(gPos, uv);
    vec4 curKey = texture2D(gKey, pjvSnapToTexel(uv, passTargetRes.xy));

    // Sky / background: nothing to accumulate, and nothing that may ever be matched against.
    if (gp.a < 0.0) {
        gl_FragData[0] = vec4(cur, 0.0);
        gl_FragData[1] = vec4(FACE_KEY_NONE, FACE_KEY_NONE, FACE_KEY_NONE, FACE_KEY_NONE);
        gl_FragData[2] = vec4(gp.xyz, -1.0);
        return;
    }

    vec3  curP = gp.xyz;
    float curDepth = gp.a;
    // The face normal comes from the key rather than from gNormal: it is the same six values, and
    // taking it from the key guarantees the orientation test and the identity test cannot disagree.
    vec3 curN = pjvFaceNormal(curKey.w);

    // Is this a swaying blade rather than a fixed voxel face? gbuffer.frag states it outright in
    // gAlbedo's alpha, because it cannot be inferred here -- a face centre moves when a blade sways
    // AND when the sub-pixel jitter simply samples the neighbouring voxel, by about the same
    // distance. See ANIM_POS_TOL_VOXELS for what this changes and why it has to.
    // 0 stationary, 0.5 displaced (sway), 1 advected (fire). See gbuffer.frag for why fire is its
    // own class: it is the one moving thing with no stable voxel to key on.
    float motionClass = texture2D(gAlbedo, uv).a;
    bool  animated  = motionClass > 0.25;
    bool  advected  = motionClass > 0.75;
    float voxelSize = texture2D(gNormal, uv).a;

    vec3  accum = cur;
    // Plain frame count now. It used to be signed, the sign carrying "a probe has landed on this face
    // before" -- a per-face notion with nothing left to mean.
    float age   = 1.0;

    // Branch on whether the CAMERA moved, not on frameCount.y -- that flag also rises when only the
    // sun moved, and a lighting change under a still camera should keep the identity-reprojected
    // still path rather than fall into the reprojecting one.
    bool camMoved = any(notEqual(prevCameraPos.xyz, cameraPos.xyz)) ||
                    any(notEqual(prevCameraDir.xyz, cameraDir.xyz));
    // The SUN moved, from its own flag rather than inferred from the scene-changed fold.
    //
    // It used to be (frameCount.y != 0 && !camMoved), and frameCount.y is raised by the grass sway on
    // every single frame -- so this read "the light changed" forever, LIGHT_CHANGE_AGE knocked the
    // history back to 4 every frame, and STILL_MAX_AGE 128 was unreachable. That is why the GI had no
    // temporal accumulation at all: not because the mean was wrong, but because it was reset before it
    // could ever build.
    bool lightMoved = frameCount.w != 0.0;

    if (frameCount.x > 0.0) {
        // An animated pixel takes the SEARCHING branch even when the camera is still, because the
        // thing that moved was the geometry. Its own texel almost never holds last frame's blade --
        // the blade swayed off it -- but a texel a pixel or two away usually does. With a still
        // camera the reprojection below is the identity, so this costs nothing but the search.
        if (!camMoved && !animated) {
            // STILL: this pixel's own texel, no resampling at all. The key still has to match --
            // the sun can move under a still camera, but the geometry cannot, so a mismatch here
            // means the previous frame wrote something else and is not ours to average.
            vec2 ownUV = (floor(uv * passTargetRes.xy) + 0.5) * passTargetRes.zw;
            vec4 hKey = texture2D(histKey, ownUV);
            vec4 h    = texture2D(accumR,  ownUV);
            vec4 hPos = texture2D(histPos, ownUV);
            // EXACT FACE IDENTITY FIRST, geometry only if that fails. R is a per-face quantity again
            // (probe.frag seeds its directions from the key, so every pixel on a face computes the
            // same number from the same origin) and an identical key is therefore proof that last
            // frame's value answers this frame's question -- the strongest gate available, and one
            // that cannot pool two neighbouring faces' light the way the geometric test can.
            //
            // The fallback still matters and is not a weaker version of the same thing: it covers the
            // sub-pixel regime, where many voxels project into one pixel and the exact key genuinely
            // does change every frame for reasons that have nothing to do with the lighting. Which
            // regime a pixel is in needs no decision -- an exact match existing IS the distinction.
            bool tight = pjvSameFace(hKey, curKey) ||
                         pjvSameVoxel(hKey, curKey) ||
                         sameSurface(hKey, hPos, curKey, curP, curN, curDepth);

            // ---- THE OWN TEXEL IS NOT THE ONLY PLACE THIS FACE'S HISTORY CAN BE -------------------
            // This is what makes the indirect term converge with the jitter on, and its absence is the
            // whole of the edge flicker.
            //
            // R is a per-FACE quantity, but it is stored per PIXEL. That is exact while a pixel keeps
            // landing on the same face, and the sub-pixel jitter is precisely what stops it: at any
            // face boundary the +-0.5px offset puts the primary ray on face A one frame and face B the
            // next. Neither is a disocclusion -- both faces are permanently visible at that pixel, and
            // the jitter alternating between them IS the sub-pixel signal. But the own texel holds
            // whichever face last frame landed on, so every tier here fails on the flip, age resets to
            // 1, and the pixel emits a raw 8-ray gather. Every frame, forever, on every edge in the
            // frame. With the jitter off nothing ever flips and the same code converges perfectly,
            // which is exactly the difference observed.
            //
            // The face this pixel wants has not been lost, though -- it is one texel away, in a pixel
            // that landed on it this frame and has been converging it all along. The MOVING branch
            // below already searches for exactly this, for exactly this reason: "voxel silhouettes are
            // a staircase of micro-faces, so the exact reprojected texel routinely lands on the wrong
            // one while the face this pixel wants sits a texel or two away holding its fully converged
            // value". Camera motion and jitter produce the same situation; only the moving branch was
            // given the means to handle it.
            //
            // Radius 1 because the jitter is bounded by half a pixel, so the alternate face is always
            // an immediate neighbour -- a wider window would only admit faces the ray could not have
            // reached. IDENTITY ONLY: this feeds a 128-frame mean, and an exact key is proof, whereas
            // the positional tiers are guesses that would be far more expensive to get wrong here than
            // in the short moving history. Those stay on the own texel, below, exactly as before.
            //
            // Runs only when the own texel already failed, so a pixel sitting still on its face -- the
            // overwhelming majority of the frame -- pays nothing for it.
            if (!tight) {
                float bestD = 1e9;
                for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;      // already tested, and it failed
                    float d = float(dx * dx + dy * dy);
                    if (d >= bestD) continue;
                    vec2 sUV  = ownUV + vec2(float(dx), float(dy)) * passTargetRes.zw;
                    vec4 sKey = texture2D(histKey, sUV);
                    if (pjvSameFace(sKey, curKey) || pjvSameVoxel(sKey, curKey)) {
                        bestD = d;
                        h     = texture2D(accumR,  sUV);
                        hPos  = texture2D(histPos, sUV);
                        tight = true;
                    }
                }
            }
            // Last tier: a silhouette between two DIFFERENT voxels, where nothing identifies the
            // pair and only proximity is left. See EDGE_POS_TOL_VOXELS.
            bool edge = !tight && sameSpot(hPos, curP, voxelSize, curDepth, EDGE_POS_TOL_VOXELS);

            if ((tight || edge) && !any(isnan(h))) {
                // Every frame contributes. The three-way branch this replaces -- take it, bootstrap
                // it, or HOLD the old value -- existed to protect a per-face store: a face that owned
                // no probe this frame had to keep its last real value rather than accept a neighbour's
                // interpolated stand-in. With the value no longer per face there is no such thing as
                // "this face's own probe", every pixel has a usable estimate every frame, and holding
                // would only freeze noise in place.
                // The edge tier's cap fades with the geometry: short while a voxel is resolved and the
                // coverage mix is volatile, long once it is sub-pixel and the mix is stationary. See
                // EDGE_SUBPIXEL_MAX_AGE.
                float maxAge = tight ? STILL_MAX_AGE
                                     : mix(EDGE_SUBPIXEL_MAX_AGE, EDGE_MAX_AGE,
                                           geometryResolved(voxelSize, curDepth));
                // Only the guessed match is clamped -- see clampToNeighbourhood.
                vec3  hRGB   = tight ? h.rgb : clampToNeighbourhood(h.rgb, uv);
                float hAge = min(max(abs(h.a), 1.0), maxAge);
                if (lightMoved) hAge = min(hAge, LIGHT_CHANGE_AGE);
                age   = min(hAge + 1.0, maxAge);
                accum = mix(hRGB, cur, 1.0 / age);
            }
        } else {
            // MOVING: reproject this pixel's world position into last frame's camera, then search a
            // small neighbourhood around where it landed. Two passes over the window: take the
            // nearest EXACT face match if there is one, otherwise the nearest geometric match.
            //
            // Deliberately NO adaptAge here. While moving, the fresh sample differs frame to frame
            // from noise rather than from real change, and an age that collapses on a luminance
            // jump cannot tell the two apart -- it would defeat the mean on exactly the stable
            // moving surfaces it is meant to help. The moving history is already short.
            bool valid;
            vec2 pUV = worldToUV(curP, prevCameraPos.xyz, prevCameraDir.xyz,
                                 passTargetRes.xy, FOV, valid);
            if (valid) {
                vec2 baseUV = (floor(pUV * passTargetRes.xy) + 0.5) * passTargetRes.zw;

                // Two searches over one window: the nearest EXACT face match wins outright if there is
                // one, and the nearest geometric match is kept as the fallback. Both are tracked in a
                // single pass over the taps -- the window is 5x5 and reading it twice would cost more
                // than carrying the second candidate.
                //
                // The exact pass is what makes the neighbourhood search worth having. Voxel silhouettes
                // are a staircase of micro-faces, so the exact reprojected texel routinely lands on the
                // wrong one while the face this pixel wants sits a texel or two away holding its fully
                // converged value. Finding it by identity is exact; finding it by geometry is a guess
                // that can just as easily land on the neighbouring face.
                vec4  bestExact = vec4(0.0); float bestExactDist = 1e9;
                vec4  bestGeom  = vec4(0.0); float bestGeomDist  = 1e9;
                vec4  bestEdge  = vec4(0.0); float bestEdgeDist  = 1e9;

                for (int dy = -SEARCH_RADIUS; dy <= SEARCH_RADIUS; dy++)
                for (int dx = -SEARCH_RADIUS; dx <= SEARCH_RADIUS; dx++) {
                    vec2 sUV = baseUV + vec2(float(dx), float(dy)) * passTargetRes.zw;
                    float d = float(dx * dx + dy * dy);
                    if (d >= bestExactDist && d >= bestGeomDist && d >= bestEdgeDist) continue;

                    vec4 hKey = texture2D(histKey, sUV);

                    // An animated pixel gates on the VOXEL, not the face: the envelope march holds a
                    // blade's source voxel still on purpose and only its normal churns, so this is an
                    // exact identity rather than the positional guess it used to be -- which is why
                    // it neither ghosts nor needs the clamp. Position is kept only as the fallback,
                    // for fire, whose parcels genuinely advect through space and have no fixed voxel.
                    if (animated) {
                        if (d < bestExactDist && pjvSameVoxel(hKey, curKey)) {
                            bestExactDist = d; bestExact = texture2D(accumR, sUV);
                            continue;
                        }
                        if (d >= bestEdgeDist) continue;
                        vec4 hPosA = texture2D(histPos, sUV);
                        // Fire gets the wider radius: a parcel advects a real distance every frame,
                        // so the tolerance has to cover how far it travelled, not merely how far a
                        // blade leaned. Grass only reaches this line when its voxel match failed.
                        float tol = advected ? FIRE_POS_TOL_VOXELS : ANIM_POS_TOL_VOXELS;
                        if (sameSpot(hPosA, curP, voxelSize, curDepth, tol)) {
                            bestEdgeDist = d; bestEdge = texture2D(accumR, sUV);
                        }
                        continue;
                    }

                    if (d < bestExactDist &&
                        (pjvSameFace(hKey, curKey) || pjvSameVoxel(hKey, curKey))) {
                        bestExactDist = d; bestExact = texture2D(accumR, sUV);
                        continue;   // an identity match is never also wanted as a weaker candidate
                    }
                    if (d >= bestGeomDist && d >= bestEdgeDist) continue;
                    vec4 hPos = texture2D(histPos, sUV);
                    if (d < bestGeomDist && sameSurface(hKey, hPos, curKey, curP, curN, curDepth)) {
                        bestGeomDist = d; bestGeom = texture2D(accumR, sUV);
                        continue;
                    }
                    // Silhouette tier, as in the still branch: same point, any facing.
                    if (d < bestEdgeDist && sameSpot(hPos, curP, voxelSize, curDepth, EDGE_POS_TOL_VOXELS)) {
                        bestEdgeDist = d; bestEdge = texture2D(accumR, sUV);
                    }
                }

                // Strongest available match wins, and the cap and the clamp follow from which one it
                // was: an identity match is proof and is trusted whole, the two positional matches are
                // guesses and are both capped short and bounded to what this frame shows nearby.
                bool  isExact = bestExactDist < 1e8;
                bool  isGeom  = !isExact && bestGeomDist < 1e8;
                bool  isEdge  = !isExact && !isGeom && bestEdgeDist < 1e8;
                bool  found   = isExact || isGeom || isEdge;
                vec4  h       = isExact ? bestExact : (isGeom ? bestGeom : bestEdge);

                // Moving surfaces cap short whichever tier matched, because their light genuinely
                // changes as they move -- that is a property of the surface, not of the match. Fire
                // caps shortest of all; see FIRE_MAX_AGE.
                float maxAge = advected ? FIRE_MAX_AGE
                             : (animated ? ANIM_MAX_AGE
                             : (isEdge   ? mix(EDGE_SUBPIXEL_MAX_AGE, EDGE_MAX_AGE,
                                               geometryResolved(voxelSize, curDepth))
                                         : MOVING_MAX_AGE));
                // Only a POSITIONAL match is a guess and needs bounding. An animated pixel that
                // matched on its voxel identity is as trustworthy as a static one that matched on its
                // face, and clamping it would feed the raw gather's noise back into a mean that was
                // busy removing it -- the opposite of what is wanted on the noisiest surfaces here.
                bool  guessed = isEdge;

                if (found && !any(isnan(h))) {
                    vec3  hRGB = guessed ? clampToNeighbourhood(h.rgb, uv) : h.rgb;
                    float hAge = min(max(abs(h.a), 1.0), maxAge);
                    if (lightMoved) hAge = min(hAge, LIGHT_CHANGE_AGE);
                    age   = min(hAge + 1.0, maxAge);
                    accum = mix(hRGB, cur, 1.0 / age);
                }
            }
        }
    }

    gl_FragData[0] = vec4(accum, age);
    gl_FragData[1] = curKey;
    gl_FragData[2] = vec4(curP, curDepth);
}
