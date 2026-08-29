$input v_color0
$input v_texcoord0

// =============================================================================
// face_accum.frag  --  Pass 3 of the face renderer (temporal accumulation).
//
// face_light produces a flat-but-slightly-noisy colour per face each frame (only a
// few GI rays). This pass averages that over time into a clean, converged per-face
// colour. Because a face's lit colour is VIEW-INDEPENDENT (it doesn't depend on where
// the camera is) and the scene is static, the same face's history is always valid --
// we just keep averaging it. This is the cheapest, most robust denoiser of the four
// renderers: there is no spatial blur at all, and history is gated by EXACT FACE
// IDENTITY rather than the fuzzy position/normal/colour heuristics the path tracers
// need. Two faces either are the same face (accumulate) or aren't (reset) -- no
// ghosting, no darkening-while-moving, no clamps.
//
// Two regimes (chosen by frameCount.y = camera moved this frame):
//   STILL  -> identity: read this pixel's own history texel, unbounded running mean
//             -> converges to zero temporal noise (needs a 32F accumulator).
//   MOVING -> reproject this pixel's world hit into last frame's camera, snap to the
//             nearest history texel, and accept the history ONLY if the face key
//             stored there equals this pixel's face key. Exact match => same face =>
//             its accumulated colour is still exactly correct, so keep averaging
//             (under a moving age cap for responsiveness). Any mismatch => a different
//             face is now under this pixel => start its average fresh.
//
// Inputs (flattened: FBO 1 [0..3], FBO 2 [4], FBO 3 [5,6]):
//   0 gAlbedo 1 gPos 2 gFace 3 gKey    (gPos, gKey used)
//   4 litRaw                            (this frame's flat face colour, a = hit mask)
//   5 histLit 6 histKey                 (previous frame -- ping-pong)
// Outputs (FBO 3, ping-pong):
//   0 accumLit  rgb = converged face colour, a = age
//   1 histKey   the face key, carried forward for next frame's validation
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos,    1);
SAMPLER2D(gKey,    3);
SAMPLER2D(litRaw,  4);
SAMPLER2D(histLit, 5);
SAMPLER2D(histKey, 6);

uniform vec4 windowRes;
uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;
uniform vec4 frameCount;

#define FOV 60.0
#define MOVING_MAX_AGE 600.0

// Neighbourhood (in texels) searched around the reprojected point for a history texel
// carrying THIS face's key. A face's colour is flat + view-independent, so any matching
// texel holds its exact converged value: searching recovers the history when the
// reprojection lands a texel or two off (the usual case at voxel edges / under motion)
// instead of discarding it and re-flashing the raw few-ray sample. Unique face keys mean
// a match is never a false positive, so a wider search only ever preserves more data.
#define SEARCH_RADIUS 3

// Project a world point into a camera -> screen UV. Exact inverse of
// pjv_utils_DDA.sc::rayStartDirection, identical to the copy in the other renderers.
vec2 worldToUV(vec3 P, vec3 camPos, vec3 camDir, vec2 res, float fov, out bool valid) {
    vec3 forward = normalize(camDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));

    vec3 v = P - camPos;
    float z = dot(v, forward);
    if (z <= 1e-4) { valid = false; return vec2(-1.0); }

    float scale  = tan(radians(fov * 0.5));
    float aspect = res.x / res.y;
    float ndcx = (dot(v, right) / z) / (scale * aspect);
    float ndcy = (dot(v, up)    / z) / scale;

    vec2 flip = (vec2(ndcx, ndcy) + 1.0) * 0.5;
    vec2 uv = vec2(flip.x, 1.0 - flip.y);
    valid = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    return uv;
}

// Two face keys refer to the same face iff every component matches. The components are
// integers (voxel index + face id 0..5) so a 0.5 tolerance is an exact comparison.
bool sameFace(vec4 a, vec4 b) {
    return all(lessThan(abs(a - b), vec4(0.5)));
}

void main() {
    vec2 uv = v_texcoord0;

    vec4 lit = texture2D(litRaw, uv);   // rgb = face colour, a = hit mask

    // Sky / background: view-dependent, never accumulate. Pass through, and store an
    // impossible key so nothing ever validates against it.
    if (lit.a < 0.5) {
        gl_FragData[0] = vec4(lit.rgb, 0.0);
        gl_FragData[1] = vec4(1e20, 1e20, 1e20, 1e20);
        return;
    }

    vec4 curKey = texture2D(gKey, uv);
    vec3 curP   = texture2D(gPos, uv).xyz;

    // Default (first frame / disocclusion / face changed): the fresh sample at age 1.
    vec3  accum = lit.rgb;
    float age   = 1.0;

    bool moving = frameCount.y != 0.0;

    if (frameCount.x > 0.0) {
        if (!moving) {
            // ---- STILL: identity, unbounded running mean -> zero noise. ----
            vec2 ownUV = (floor(uv * windowRes.xy) + 0.5) / windowRes.xy;
            vec4 hKey  = texture2D(histKey, ownUV);
            vec4 hLit  = texture2D(histLit, ownUV);
            if (sameFace(hKey, curKey) && !any(isnan(hLit))) {
                age   = max(hLit.a, 1.0) + 1.0;
                accum = mix(hLit.rgb, lit.rgb, 1.0 / age);
            }
        } else {
            // ---- MOVING: reproject, then SEARCH a neighbourhood for the same face. ----
            bool valid;
            vec2 prevUV = worldToUV(curP, prevCameraPos.xyz, prevCameraDir.xyz,
                                    windowRes.xy, FOV, valid);
            if (valid) {
                vec2 texel  = 1.0 / windowRes.xy;
                // Snap to the texel centre so every probe reads a key point-sampled
                // (never interpolated across a face boundary) for an exact comparison.
                vec2 baseUV = (floor(prevUV * windowRes.xy) + 0.5) / windowRes.xy;

                // Walk outward from the reprojected texel and take the FIRST history
                // texel whose key matches. Because the face is flat, that texel's value
                // is exactly this face's converged colour no matter where in the window
                // it is -- so this both fixes sub-texel reprojection slop and pulls a
                // still-visible face's history back even when its own reprojected texel
                // now sits on a neighbour. Only a genuine disocclusion (the face is
                // nowhere nearby in history) leaves us on the fresh sample -> a reset.
                bool found = false;
                vec4 hLit  = vec4(0.0);
                for (int dy = -SEARCH_RADIUS; dy <= SEARCH_RADIUS; dy++) {
                    for (int dx = -SEARCH_RADIUS; dx <= SEARCH_RADIUS; dx++) {
                        if (found) continue;
                        vec2 sUV = baseUV + vec2(float(dx), float(dy)) * texel;
                        if (sameFace(texture2D(histKey, sUV), curKey)) {
                            hLit  = texture2D(histLit, sUV);
                            found = true;
                        }
                    }
                }

                if (found && !any(isnan(hLit))) {
                    float hAge = max(hLit.a, 1.0);
                    age   = min(min(hAge, MOVING_MAX_AGE) + 1.0, MOVING_MAX_AGE);
                    accum = mix(hLit.rgb, lit.rgb, 1.0 / age);
                }
            }
        }
    }

    gl_FragData[0] = vec4(accum, age);
    gl_FragData[1] = curKey;
}
