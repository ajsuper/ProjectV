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
    // Set by resolve: did this sample come from a probe on this pixel's own face?
    bool exactSample = curSample.a > 0.5;
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

    vec3  accum = cur;
    // Negative until a probe has landed on this face -- see the note above. A first sample that is
    // already exact starts positive.
    float age   = exactSample ? 1.0 : -1.0;

    // Branch on whether the CAMERA moved, not on frameCount.y -- that flag also rises when only the
    // sun moved, and a lighting change under a still camera should keep the identity-reprojected
    // still path rather than fall into the reprojecting one.
    bool camMoved = any(notEqual(prevCameraPos.xyz, cameraPos.xyz)) ||
                    any(notEqual(prevCameraDir.xyz, cameraDir.xyz));
    // The scene-changed signal is up but the camera did not move, so it was a light. Reuses the
    // existing frameCount.y fold rather than adding a per-light flag, and being event-driven it is
    // immune to the per-frame gather jitter that would otherwise trip it every frame.
    bool lightMoved = (frameCount.y != 0.0) && !camMoved;

    if (frameCount.x > 0.0) {
        if (!camMoved) {
            // STILL: this pixel's own texel, no resampling at all. The key still has to match --
            // the sun can move under a still camera, but the geometry cannot, so a mismatch here
            // means the previous frame wrote something else and is not ours to average.
            vec2 ownUV = (floor(uv * passTargetRes.xy) + 0.5) * passTargetRes.zw;
            vec4 hKey = texture2D(histKey, ownUV);
            vec4 h    = texture2D(accumR,  ownUV);
            if (pjvSameFace(hKey, curKey) && !any(isnan(h))) {
                bool  hadExact = h.a > 0.0;   // sign carries "a probe has landed here before"
                float hAge = min(max(abs(h.a), 1.0), STILL_MAX_AGE);
                if (lightMoved) hAge = min(hAge, LIGHT_CHANGE_AGE);
                if (exactSample) {
                    age   = min(hAge + 1.0, STILL_MAX_AGE);
                    accum = mix(h.rgb, cur, 1.0 / age);
                } else if (!hadExact) {
                    // Still bootstrapping: no probe has ever landed on this face, so the
                    // interpolation is the best estimate available and keeps averaging.
                    age   = -min(hAge + 1.0, STILL_MAX_AGE);
                    accum = mix(h.rgb, cur, 1.0 / min(hAge + 1.0, STILL_MAX_AGE));
                } else {
                    // Hold. This face owns no probe right now, and the interpolated stand-in is
                    // whatever the neighbouring faces happen to look like from here.
                    age   = hAge;
                    accum = h.rgb;
                }
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

                vec4  bestExact = vec4(0.0); float bestExactDist = 1e9;
                vec4  bestGeom  = vec4(0.0); float bestGeomDist  = 1e9;

                for (int dy = -SEARCH_RADIUS; dy <= SEARCH_RADIUS; dy++)
                for (int dx = -SEARCH_RADIUS; dx <= SEARCH_RADIUS; dx++) {
                    vec2 sUV = baseUV + vec2(float(dx), float(dy)) * passTargetRes.zw;
                    vec4 hKey = texture2D(histKey, sUV);
                    float d = float(dx * dx + dy * dy);

                    if (pjvSameFace(hKey, curKey)) {
                        if (d < bestExactDist) { bestExactDist = d; bestExact = texture2D(accumR, sUV); }
                    } else if (bestExactDist > 1e8) {
                        // Only worth evaluating while no exact match has been found -- an exact
                        // match always wins, so the geometric candidate would be discarded anyway.
                        vec4 hPos = texture2D(histPos, sUV);
                        if (sameSurface(hKey, hPos, curKey, curP, curN, curDepth) && d < bestGeomDist) {
                            bestGeomDist = d; bestGeom = texture2D(accumR, sUV);
                        }
                    }
                }

                bool found = bestExactDist < 1e8 || bestGeomDist < 1e8;
                vec4 h = bestExactDist < 1e8 ? bestExact : bestGeom;
                if (found && !any(isnan(h))) {
                    bool  hadExact = h.a > 0.0;
                    float hAge = min(max(abs(h.a), 1.0), MOVING_MAX_AGE);
                    if (lightMoved) hAge = min(hAge, LIGHT_CHANGE_AGE);
                    if (exactSample) {
                        age   = min(hAge + 1.0, MOVING_MAX_AGE);
                        accum = mix(h.rgb, cur, 1.0 / age);
                    } else if (!hadExact) {
                        age   = -min(hAge + 1.0, MOVING_MAX_AGE);
                        accum = mix(h.rgb, cur, 1.0 / min(hAge + 1.0, MOVING_MAX_AGE));
                    } else {
                        age   = hAge;
                        accum = h.rgb;
                    }
                }
            }
        }
    }

    gl_FragData[0] = vec4(accum, age);
    gl_FragData[1] = curKey;
    gl_FragData[2] = vec4(curP, curDepth);
}
