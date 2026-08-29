$input v_color0
$input v_texcoord0

// =============================================================================
// accumulate.frag  --  Pass 7 of the world-space PER-FACE radiance-cascade renderer (renderer 6).
//
// Temporal denoise of the resolved indirect irradiance R.
//
// HISTORY GATE -- GEOMETRY, not exact voxel key. The first version keyed history on the EXACT voxel-
// face identity (voxel index + face id), borrowed from renderer 4. That works for renderer 4 because
// it has no jitter and shades per-face-centre, so a pixel's voxel identity never churns. Renderer 6
// has a JITTERED, PER-PIXEL GI: under camera motion a pixel sweeps across MANY voxels of the same
// wall (especially on far / oblique surfaces where many voxels map per pixel), so the exact voxel
// index changes every frame -> the exact-key test fails -> history is discarded and the raw noisy
// sample re-exposed every frame == no accumulation under movement (stable only when still, because
// then the per-pixel voxel identity is fixed). That was the root cause of the "history destroyed
// under movement, worst on far/slow-moving surfaces" behaviour.
//
// The GI is view-independent and low-frequency across a surface, so the correct gate is GEOMETRIC:
// accept history if it is the SAME FACE ORIENTATION (voxel-agnostic -> tolerates crossing between
// adjacent voxels of one wall) AND on the SAME PLANE at ~the same depth (rejects a parallel wall
// behind an edge) AND not a gross positional jump (rejects gross disocclusion). This keeps the wall's
// converged GI alive as the pixel slides across its voxels under motion.
//
//   STILL  -> identity own-texel, capped running mean; a light-move event knocks the age down for fast
//             recovery (light-agnostic, see below).
//   MOVING -> reproject into last frame, SEARCH a small neighbourhood for the NEAREST same-surface
//             history texel and borrow its converged R. Only a true disocclusion (no same-surface
//             texel near the reprojected point) resets to the fresh sample.
//
// Inputs (FBO 6 [0], FBO 7 [1,2], FBO 1 [3..8]):
//   0 resolveR   1 accumR(hist)  2 histInfo   3 gPos  4 gNormal ... 8 gKey
// Outputs (FBO 7, ping-pong):
//   0 accumR    rgb = converged indirect irradiance R, a = age
//   1 histInfo  rgb = world position, a = face id       (carried for next frame's geometry gate)
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(resolveR, 0);   // FBO6[0]: this frame's indirect irradiance R
SAMPLER2D(accumR,   1);   // FBO7[0] prev: converged R + age
SAMPLER2D(histInfo, 2);   // FBO7[1] prev: rgb = world pos, a = face id
SAMPLER2D(gPos,     3);   // FBO1[0]: xyz world pos, a = camDist (a < 0 => sky)
SAMPLER2D(gNormal,  4);   // FBO1[1]: xyz world normal
SAMPLER2D(gKey,     8);   // FBO1[5]: rgb voxel index, a = face id (0..5)

uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;

#include <pjv_cascade_common.sc>

#define STILL_MAX_AGE  96.0
#define MOVING_MAX_AGE 48.0
// Effective age a still pixel is knocked down to on the frame a light moves, so a lighting change
// converges in a few frames instead of over STILL_MAX_AGE. Small = snappier (but noisier during the
// change); larger = smoother recovery. Not 1 -- a little history keeps the change from flashing raw.
#define LIGHT_CHANGE_AGE 4.0
// Neighbourhood (texels) searched around the reprojected point for a same-surface history texel.
#define SEARCH_RADIUS 3

// Geometry gate: does history hInfo (rgb = world pos, a = face id) describe the SAME surface this
// pixel (curP, curN, curFaceId, camera depth curD) is on? Face-orientation match is voxel-agnostic so
// it tolerates crossing between adjacent voxels of one wall; the plane term rejects a parallel wall at
// a different depth; the distance term rejects gross disocclusion. Tolerances scale with depth so far
// pixels (large world footprint) are not falsely rejected.
bool sameSurface(vec4 hInfo, vec3 curP, vec3 curN, float curFaceId, float curD) {
    if (abs(hInfo.a - curFaceId) > 0.5) return false;                    // same face orientation
    vec3 hP = hInfo.rgb;
    if (abs(dot(hP - curP, curN)) > max(0.3, curD * 0.02)) return false; // same plane (no depth ghost)
    if (length(hP - curP)         > max(1.0, curD * 0.06)) return false; // not a gross jump
    return true;
}

void main() {
    vec2 uv = v_texcoord0;
    vec4 gp = texture2D(gPos, uv);
    vec3 cur = texture2D(resolveR, uv).rgb;   // fresh indirect irradiance R

    // Sky / background: no indirect term. Pass through R (0), store an impossible face id so nothing
    // ever validates against it.
    if (gp.a < 0.0) {
        gl_FragData[0] = vec4(cur, 0.0);
        gl_FragData[1] = vec4(1e20, 1e20, 1e20, 1e20);
        return;
    }

    vec3  curP      = gp.xyz;
    float curD      = gp.a;
    vec3  curN      = normalize(texture2D(gNormal, uv).xyz);
    float curFaceId = texture2D(gKey, uv).a;

    vec3  accum = cur;
    float age   = 1.0;

    // Branch on whether the CAMERA actually moved (from the camera uniforms), NOT frameCount.y -- that
    // flag also rises when only the lighting changes, and a lighting change with a still camera should
    // keep accumulating from the identity-reprojected STILL path. camMoved==false => identity reproj.
    bool camMoved = any(notEqual(prevCameraPos.xyz, cameraPos.xyz)) ||
                    any(notEqual(prevCameraDir.xyz, cameraDir.xyz));
    // A LIGHT (not the camera) moved iff the scene-changed signal is up but the camera did not move --
    // reuses the existing frameCount.y fold (no new per-light flag; new lights raise it like the sun).
    // Event-driven, so immune to the per-frame gather jitter that would otherwise defeat the mean.
    bool lightMoved = (frameCount.y != 0.0) && !camMoved;

    if (frameCount.x > 0.0) {
        if (!camMoved) {
            // ---- STILL (camera static): identity own texel, capped running mean -> zero noise. ----
            vec2 ownUV = (floor(uv * windowRes.xy) + 0.5) / windowRes.xy;
            vec4 hInfo = texture2D(histInfo, ownUV);
            vec4 hR    = texture2D(accumR,   ownUV);
            if (sameSurface(hInfo, curP, curN, curFaceId, curD) && !any(isnan(hR))) {
                float hAge = min(max(hR.a, 1.0), STILL_MAX_AGE);
                if (lightMoved) hAge = min(hAge, LIGHT_CHANGE_AGE);  // fast lighting-change recovery
                age   = min(hAge + 1.0, STILL_MAX_AGE);
                accum = mix(hR.rgb, cur, 1.0 / age);
            }
        } else {
            // ---- MOVING: reproject, then SEARCH for the NEAREST same-surface history texel. ----
            bool valid;
            vec2 prevUV = worldToUV(curP, prevCameraPos.xyz, prevCameraDir.xyz,
                                    windowRes.xy, FOV, valid);
            if (valid) {
                vec2 texel  = 1.0 / windowRes.xy;
                vec2 baseUV = (floor(prevUV * windowRes.xy) + 0.5) / windowRes.xy;

                bool  found    = false;
                float bestDist = 1e9;
                vec4  hR       = vec4(0.0);
                for (int dy = -SEARCH_RADIUS; dy <= SEARCH_RADIUS; dy++)
                for (int dx = -SEARCH_RADIUS; dx <= SEARCH_RADIUS; dx++) {
                    vec2 sUV = baseUV + vec2(float(dx), float(dy)) * texel;
                    if (sameSurface(texture2D(histInfo, sUV), curP, curN, curFaceId, curD)) {
                        float d = float(dx * dx + dy * dy);   // prefer the nearest match
                        if (d < bestDist) {
                            bestDist = d;
                            hR       = texture2D(accumR, sUV);
                            found    = true;
                        }
                    }
                }

                if (found && !any(isnan(hR))) {
                    float hAge = min(max(hR.a, 1.0), MOVING_MAX_AGE);
                    age   = min(hAge + 1.0, MOVING_MAX_AGE);
                    accum = mix(hR.rgb, cur, 1.0 / age);
                }
            }
        }
    }

    gl_FragData[0] = vec4(accum, age);
    gl_FragData[1] = vec4(curP, curFaceId);
}
