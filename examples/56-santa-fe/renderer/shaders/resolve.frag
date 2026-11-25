$input v_color0
$input v_texcoord0

// =============================================================================
// resolve.frag -- the Santa Fe renderer's composite, at the window's resolution (santa-fe-reference S22).
//
// The frame comes in layers, each upscaled by its own setting (reconstruct.frag, sfUpscale), and is put
// together here:
//
//   colour   the sky light, the bounce, the air (the reconstruction's layer 0), darkened by the AO over its
//            sky light's share (a):
//              traced   ao_trace.frag's fixed rays, filtered (ao_atrous.sc), read at this pixel's voxel FACE's
//                       centre -- every pixel of a face agrees -- or at the pixel itself (sfUpscale2.x: 0 per
//                       face, 1 spatial, 2 spatial and temporal)
//              screen   an SSAO from this resolution's depth and normals, per pixel, a fixed kernel
//   sun      the sun's light (layer 1) times its visibility: the A-Trous filtered shadow (shadow_atrous.sc) or
//            the reconstruction's hard one (sfUpscale2.y off), plus the leaf glow's extra (layer 1's a)
//   reflect  reflect.frag's light (layer 2), under the AO too (a sheen under a crown sees little sky)
//   clouds   already in the layers (shadow_soft.frag puts them on at the trace's resolution: composited here,
//            per pixel after the history, a voxel's silhouette against a cloud flickered with the jitter)
//
// No history unless a layer asks for it (sfUpscale 2, "temporal"): the sum of those layers is accumulated here
// by restir-pt's rules -- still: a running mean up to sfTemporal.y frames (64; the reference mode's sample
// count); moving: reprojected by the nearest surface's motion, clamped into this frame's 3 x 3 neighbourhood,
// at most sfTemporal.z (16); uncovered by something moving: clamped, 4 -- and the rest is this frame's. The reference mode sets its
// colour temporal (everything rides in it there) and the jitter on.
//
// Inputs: the reconstruction (colour 0, sun 1, reflections + visibility 2, blocker / depth / motion 3, face 4,
// normal + depth 5), the shadow 6, the traced AO 7, this pass's own last output (the frame 8, the history 9). Outputs: 0 the frame, 1 the history (rgb the temporal layers' sum,
// a its length).
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>
#include <pjv_camera.sc>

SAMPLER2D(rColour, 0);
SAMPLER2D(rSun, 1);
SAMPLER2D(rReflVis, 2);
SAMPLER2D(rAux, 3);
SAMPLER2D(rFace, 4);
SAMPLER2D(rND, 5);
SAMPLER2D(shadowF, 6);
SAMPLER2D(aoF, 7);
SAMPLER2D(history, 9);

uniform vec4 frameCount;   // y: the camera moved (or the scene changed) this frame
uniform vec4 sfTemporal;   // x: reset, y: frames kept when still, z: frames kept when moving
uniform vec4 sfUpscale;    // x colour, y sun, z reflections, w the sun's visibility: 2 temporal
uniform vec4 sfUpscale2;   // x the traced AO (0 per face, 1 spatial, 2 temporal), y the shadow's A-Trous, z the AO's
uniform vec4 sfSSAO;       // x radius (m), y strength, z most pixels across, w on
uniform vec4 sfFlags;      // y bit 7: SF2_AO

#define SF_SSAO_TAPS 16
#define SF_SSAO_MIN_PIXELS 4.0   // far off, the SSAO keeps at least this many pixels' reach (its metres shrink under one)

// ---- The traced AO ----
// Bilinear at uv over the AO's texels, each held to `depth` (a texel of another surface counts for little).
float aoBilateral(vec2 uv, float depth) {
    vec2 size = vec2(textureSize(aoF, 0));
    vec2 g = uv * size - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2 f = g - vec2(i0);
    float sum = 0.0, wsum = 0.0, nearest = 1.0, nearRel = 1e30;
    for (int k = 0; k < 4; k++) {
        ivec2 o = ivec2(k & 1, k >> 1);
        vec4 a = texelFetch(aoF, clamp(i0 + o, ivec2(0, 0), ivec2(size) - 1), 0);
        if (a.g < 0.0) continue;
        float rel = abs(a.g - depth) / max(depth * 0.03, 0.05);
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) * exp(-rel * rel);
        sum += a.r * w; wsum += w;
        if (rel < nearRel) { nearRel = rel; nearest = a.r; }
    }
    return wsum > 1e-4 ? sum / wsum : nearest;
}

float tracedAO(vec2 uv, float depth, vec4 face) {
    if (sfUpscale2.x < 0.5 && face.w > 0.0) {
        // Per face: the AO where the face's centre is on the screen, if what is there is that face.
        bool inFront;
        vec3 camP = pjvViewPosition();
        vec2 fuv = pjvPerspectiveUV(face.xyz, passTargetRes.xy, camP, pjvViewDirection(), pjvViewTanHalfFov(), inFront);
        float fd = length(face.xyz - camP);
        if (inFront && all(greaterThanEqual(fuv, vec2(0.0, 0.0))) && all(lessThan(fuv, vec2(1.0, 1.0)))) {
            float d = texture2D(rND, fuv).w;
            if (d >= 0.0 && abs(d - fd) < max(fd * 0.02, face.w)) return aoBilateral(fuv, fd);
        }
    }
    return aoBilateral(uv, depth);
}

// ---- The screen-space AO, per pixel: SF_SSAO_TAPS taps on a fixed spiral over sfSSAO.x metres ----
vec3 positionAt(ivec2 q, float depth) {
    return pjvViewPosition() + pjvViewRayDirection((vec2(q) + 0.5) / passTargetRes.xy, passTargetRes.xy) * depth;
}

float screenAO(ivec2 at, vec3 p0, vec3 n0, float pixelWorld) {
    if (sfSSAO.w < 0.5) return 1.0;
    ivec2 size = ivec2(passTargetRes.xy);
    float rPix = clamp(sfSSAO.x / max(pixelWorld, 1e-4), SF_SSAO_MIN_PIXELS, sfSSAO.z);
    float radius = rPix * pixelWorld;
    float occ = 0.0;
    for (int i = 0; i < SF_SSAO_TAPS; i++) {
        float rr = sqrt((float(i) + 0.5) / float(SF_SSAO_TAPS)) * rPix;
        float a = float(i) * 2.39996323;
        ivec2 q = clamp(at + ivec2(round(vec2(cos(a), sin(a)) * rr)), ivec2(0, 0), size - 1);
        float dq = texelFetch(rND, q, 0).w;
        if (dq < 0.0) continue;
        vec3 v = positionAt(q, dq) - p0;
        float l = length(v);
        if (l < 1e-4) continue;
        float fall = clamp(1.0 - l / (radius * 2.0), 0.0, 1.0);
        occ += max(dot(n0, v / l) - 0.15, 0.0) * fall;
    }
    return clamp(1.0 - sfSSAO.y * occ / float(SF_SSAO_TAPS) * 1.6, 0.0, 1.0);
}

// ---- The layers at a pixel: what is accumulated (temporal) and what is this frame's ----
struct Parts { vec3 stable; vec3 temporal; };

Parts partsAt(ivec2 q, float ao) {
    vec4 c0 = texelFetch(rColour, q, 0);
    vec4 c1 = texelFetch(rSun, q, 0);
    vec4 c2 = texelFetch(rReflVis, q, 0);
    Parts p;
    p.stable = vec3(0.0, 0.0, 0.0);
    p.temporal = vec3(0.0, 0.0, 0.0);
    if (c0.a < -0.5) { p.stable = c0.rgb; return p; }   // a cloud box in front (shadow_soft.frag): whole already
    float vis = sfUpscale2.y > 0.5 ? texelFetch(shadowF, q, 0).r : clamp(c2.a, 0.0, 1.0);
    vec3 A = c0.rgb * (1.0 - clamp(c0.a, 0.0, 1.0) * (1.0 - ao));
    vec3 S = c1.rgb * min(vis + c1.a, 1.0);
    vec3 R = c2.rgb * ao;
    if (sfUpscale.x > 1.5 || sfUpscale2.x > 1.5) p.temporal += A; else p.stable += A;
    if (sfUpscale.y > 1.5 || sfUpscale.w > 1.5) p.temporal += S; else p.stable += S;
    if (sfUpscale.z > 1.5) p.temporal += R; else p.stable += R;
    return p;
}

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec2 res = passTargetRes.xy;
    vec2 uv = (vec2(at) + 0.5) / res;
    vec4 nd = texelFetch(rND, at, 0);
    float depth = nd.w;
    float ao = 1.0;
    if ((uint(sfFlags.y + 0.5) & 128u) != 0u && depth >= 0.0) {
        float pixelWorld = max(depth, 1e-3) * 2.0 * pjvViewTanHalfFov() / res.y;
        vec3 p0 = positionAt(at, depth);
        ao = tracedAO(uv, depth, texelFetch(rFace, at, 0)) * screenAO(at, p0, nd.xyz, pixelWorld);
    }
    Parts parts = partsAt(at, ao);
    bool anyTemporal = sfUpscale2.w > 0.5;

    // The temporal layers' history: restir-pt's resolve (30-renderers' restir-pt/shaders/resolve.frag), rule for rule.
    vec3 accumulated = parts.temporal;
    float newLength = 1.0;
    bool objectMoved = false;
    if (anyTemporal && sfTemporal.x < 0.5) {
        ivec2 last = ivec2(res) - ivec2(1, 1);
        // The motion of the nearest surface in the 3 x 3 pixels round this one: at an edge the pixel follows
        // what is in front, not the background behind it, so nothing trails or flickers along silhouettes.
        vec2 motion = texelFetch(rAux, at, 0).zw;
        float nearest = 1e30;
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++) {
                ivec2 q = clamp(at + ivec2(x, y), ivec2(0, 0), last);
                float d = texelFetch(rND, q, 0).w;
                if (d >= 0.0 && d < nearest) { nearest = d; motion = texelFetch(rAux, q, 0).zw; }
            }
        bool moved = frameCount.y > 0.5 || length(motion * res) > 0.01;
        vec3 historyColour = vec3(0.0, 0.0, 0.0);
        float historyLength = 0.0;
        bool uncovered = false;
        bool clampIt = false;
        if (!moved) {
            // Still: its own history, always kept -- it settles to the jitter's average and stays. A pixel that
            // showed something moving last frame (stored negative) is what a moving thing just uncovered.
            vec4 h = texelFetch(history, at, 0);
            historyColour = h.rgb;
            historyLength = abs(h.a);
            uncovered = h.a < 0.0;
            if (uncovered) { historyLength = min(historyLength, 4.0); clampIt = true; }
        } else {
            // Moving: where it was last frame, bilinearly, clamped into this frame's neighbourhood.
            vec2 pix = (uv + motion) * res - 0.5;
            float weight = 0.0;
            ivec2 base = ivec2(floor(pix));
            vec2 f = pix - floor(pix);
            for (int k = 0; k < 4; k++) {
                ivec2 o = ivec2(k & 1, k >> 1);
                ivec2 q = base + o;
                if (q.x < 0 || q.y < 0 || q.x > last.x || q.y > last.y) continue;
                float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
                vec4 h = texelFetch(history, q, 0);
                historyColour += h.rgb * w;
                historyLength += abs(h.a) * w;
                weight += w;
            }
            if (weight > 1e-3) {
                historyColour /= weight;
                historyLength = min(historyLength / weight, sfTemporal.z);
                clampIt = true;
            } else {
                historyLength = 0.0;
            }
        }
        if (clampIt) {
            // The colour range of this frame's 3 x 3 neighbourhood (TAA's clamp; the AO taken as this pixel's).
            vec3 lo = parts.temporal, hi = parts.temporal;
            for (int y = -1; y <= 1; y++)
                for (int x = -1; x <= 1; x++) {
                    if (x == 0 && y == 0) continue;
                    vec3 c = partsAt(clamp(at + ivec2(x, y), ivec2(0, 0), last), ao).temporal;
                    lo = min(lo, c);
                    hi = max(hi, c);
                }
            historyColour = clamp(historyColour, lo, hi);
        }
        newLength = min(historyLength + 1.0, moved ? sfTemporal.z : sfTemporal.y);
        accumulated = historyLength > 0.0 ? mix(historyColour, parts.temporal, 1.0 / newLength) : parts.temporal;
        // Marked only under a still camera: a moving one moves every pixel, and its stopping uncovers nothing.
        objectMoved = frameCount.y < 0.5 && length(motion * res) > 0.01;
    }
    vec3 colour = parts.stable + accumulated;
    if (any(isnan(colour)) || any(isinf(colour))) colour = vec3(0.0, 0.0, 0.0);
    if (any(isnan(accumulated)) || any(isinf(accumulated))) accumulated = vec3(0.0, 0.0, 0.0);
    gl_FragData[0] = vec4(max(colour, vec3(0.0, 0.0, 0.0)), 1.0);
    gl_FragData[1] = vec4(accumulated, objectMoved ? -newLength : newLength);
}
