$input v_color0
$input v_texcoord0

// gi_accumulate.frag -- pass 5, GI resolution: temporal accumulation of the indirect light (SVGF's first
// half), the diffuse and the specular apart.
//
// The pixel's visible point is carried through last frame's camera, the four history texels round it are
// each checked against this surface, and the ones that pass are blended bilinearly. The diffuse history is
// capped at restirDenoise.w frames, and cut pixel by pixel where the light changed (below). The specular's cap follows the roughness: a mirror's reflection moves
// with the view, so it keeps about two frames; a rough lobe's barely does, and keeps as many as the
// diffuse. (A glossy reflection reprojected at the reflected point's depth, not the surface's, would let
// it keep more -- not done.)
//
// Where the light changed -- the sun moved, something burst into flame, a body's bounce light moved on -- the
// history is not this light any more, and keeping the full cap would drag the old one along. So each lobe
// compares its history with this frame's light: the mean over the 3x3 GI pixels of this surface (the paths
// after spatial reuse), the difference measured in the history's own noise (the standard deviation its
// moments carry, scaled down for a mean of a few correlated neighbours, plus RESTIR_CHANGE_FLOOR of the
// light: a few percent is never news). From RESTIR_CHANGE_FROM deviations the cap falls, reaching
// RESTIR_FAST_HISTORY frames at RESTIR_CHANGE_FULL; a pixel whose light is steady keeps all of it. (kajiya's
// rtdgi temporal filter does the same, its cap 32 down to 4; NRD takes a confidence for this from re-traced
// rays.) A still scene therefore keeps a long history -- quiet -- and changing light a short one, only where
// it changes. docs/plans/restir-gi-confidence.md.
//
// It also keeps each one's second moment of luminance, for the variance the a-trous filter scales its
// luminance test by; a pixel with little history takes its 3x3 neighbourhood's variance instead, so a
// new pixel is blurred harder.
//
// Inputs: 0-3 accDiff, accSpec, accMom, accSurf (last frame: this pass's own, ping-ponged); 4 giDiff,
// 5 giSpec (pass 4), 6 surf0, 7 surf1 (pass 2). Outputs: accDiff (rgb, history length), accSpec (rgb,
// history length), accMom (diffuse m2, specular m2, diffuse variance, specular variance), accSurf (x1,
// dir(n1); -1 none) -- this frame's surfaces, for next frame's reprojection.

#include <bgfx_shader.sh>
#include <pjv_camera.sc>      // the engine camera: pjvView*, pjvPrevView*
#include "restir_common.sc"

SAMPLER2D(accDiff, 0);
SAMPLER2D(accSpec, 1);
SAMPLER2D(accMom, 2);
SAMPLER2D(accSurf, 3);
SAMPLER2D(giDiff, 4);
SAMPLER2D(giSpec, 5);
SAMPLER2D(surf0, 6);
SAMPLER2D(surf1, 7);

#define RESTIR_FAST_HISTORY  3.0    // frames: the cap where the light has changed
#define RESTIR_CHANGE_FROM   2.0    // the history this many deviations from this frame's light: the cap starts to fall
#define RESTIR_CHANGE_FULL   5.0    // ... and is RESTIR_FAST_HISTORY from here
#define RESTIR_CHANGE_FLOOR  0.03   // of the light: never counted as a change
#define RESTIR_MEAN_NOISE    0.6    // a 3x3 mean's deviation, of one pixel's: nine neighbours sharing paths

#define RESTIR_BORROWED_HISTORY 4.0   // frames, at most, for history taken from further away

// The specular history's cap for a lobe of roughness alpha.
float restirSpecularCap(float alpha) { return mix(2.0, restirDenoise.w, smoothstep(0.0, 0.3, alpha)); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 diffuse = texelFetch(giDiff, p, 0).rgb;
    vec3 specular = texelFetch(giSpec, p, 0).rgb;
    RestirSurface s = restirSurface(texelFetch(surf0, p, 0), texelFetch(surf1, p, 0));
    gl_FragData[3] = vec4(s.x, s.t >= 0.0 ? restirPackDir(s.n) : -1.0);
    if (s.t < 0.0) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[2] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    vec2 res = passTargetRes.xy;
    vec3 diffuseHistory = vec3(0.0, 0.0, 0.0);
    vec3 specularHistory = vec3(0.0, 0.0, 0.0);
    vec2 moments = vec2(0.0, 0.0);
    float historyLength = 0.0;
    float weight = 0.0;
    if (restirDenoise.y > 0.5 && restirJitter.w < 0.5) {   // the sun moving shortens it, below
        bool nowInFront;
        bool thenInFront;
        vec2 uvNow = restirWorldToUV(s.x, pjvViewPosition(), pjvViewDirection(), res, nowInFront);
        vec2 uvThen = restirWorldToUV(s.x, pjvPrevViewPosition(), pjvPrevViewDirection(), res, thenInFront);
        vec2 pix = ((vec2(p) + 0.5) / res + (uvThen - uvNow)) * res - 0.5;
        if (thenInFront) {
            ivec2 base = ivec2(floor(pix));
            vec2 f = pix - floor(pix);
            for (int k = 0; k < 4; k++) {
                ivec2 o = ivec2(k & 1, k >> 1);
                ivec2 q = base + o;
                if (q.x < 0 || q.y < 0 || q.x >= int(res.x) || q.y >= int(res.y)) continue;
                vec4 was = texelFetch(accSurf, q, 0);
                if (was.w < 0.0 || !restirSameSurface(was.xyz, restirUnpackDir(was.w), s.x, s.n, s.t)) continue;
                float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
                vec4 hd = texelFetch(accDiff, q, 0);
                diffuseHistory += hd.rgb * w;
                specularHistory += texelFetch(accSpec, q, 0).rgb * w;
                moments += texelFetch(accMom, q, 0).xy * w;
                historyLength += hd.a * w;
                weight += w;
            }
            // None of the four is this surface -- on busy geometry and at grazing angles the visible point
            // lands on another face than last frame's, or the bilinear footprint straddles an edge: the
            // 3x3 round them, each that is this surface counting equally (SVGF's fallback). Without it
            // those pixels restart from one path every few frames, and flicker as they move.
            if (weight <= 1e-3) {
                ivec2 centre = ivec2(floor(pix + 0.5));
                for (int y = -1; y <= 1; y++)
                    for (int x = -1; x <= 1; x++) {
                        ivec2 q = centre + ivec2(x, y);
                        if (q.x < 0 || q.y < 0 || q.x >= int(res.x) || q.y >= int(res.y)) continue;
                        vec4 was = texelFetch(accSurf, q, 0);
                        if (was.w < 0.0 || !restirSameSurface(was.xyz, restirUnpackDir(was.w), s.x, s.n, s.t)) continue;
                        vec4 hd = texelFetch(accDiff, q, 0);
                        diffuseHistory += hd.rgb;
                        specularHistory += texelFetch(accSpec, q, 0).rgb;
                        moments += texelFetch(accMom, q, 0).xy;
                        historyLength += hd.a;
                        weight += 1.0;
                    }
            }
            // Still nothing: uncovered, most likely -- the wall behind a post the camera passed. The same
            // surface was usually on screen a few pixels away last frame: the 5x5 at a stride of 2 (eight GI
            // pixels across), its history counted as at most RESTIR_BORROWED_HISTORY frames, since it is
            // light from beside here. Better than one path, which the filter turns into blotches that change
            // every frame.
            if (weight <= 1e-3) {
                ivec2 centre = ivec2(floor(pix + 0.5));
                for (int y = -2; y <= 2; y++)
                    for (int x = -2; x <= 2; x++) {
                        ivec2 q = centre + ivec2(x, y) * 2;
                        if (q.x < 0 || q.y < 0 || q.x >= int(res.x) || q.y >= int(res.y)) continue;
                        vec4 was = texelFetch(accSurf, q, 0);
                        if (was.w < 0.0 || !restirSameSurface(was.xyz, restirUnpackDir(was.w), s.x, s.n, s.t)) continue;
                        vec4 hd = texelFetch(accDiff, q, 0);
                        diffuseHistory += hd.rgb;
                        specularHistory += texelFetch(accSpec, q, 0).rgb;
                        moments += texelFetch(accMom, q, 0).xy;
                        historyLength += min(hd.a, RESTIR_BORROWED_HISTORY);
                        weight += 1.0;
                    }
            }
        }
    }
    if (weight > 1e-3) {
        diffuseHistory /= weight;
        specularHistory /= weight;
        moments /= weight;
        historyLength /= weight;
    } else {
        historyLength = 0.0;
    }

    // How far the history is from this frame's light, per lobe (above).
    vec2 cap = vec2(restirDenoise.w, restirSpecularCap(pjvBsdfAlpha(s.glossiness)));
    if (historyLength > RESTIR_FAST_HISTORY) {
        vec2 now = vec2(0.0, 0.0);
        float n = 0.0;
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++) {
                ivec2 q = clamp(p + ivec2(x, y), ivec2(0, 0), ivec2(res) - ivec2(1, 1));
                RestirSurface o = restirSurface(texelFetch(surf0, q, 0), texelFetch(surf1, q, 0));
                if (o.t < 0.0 || !restirSameSurface(o.x, o.n, s.x, s.n, s.t)) continue;
                now += vec2(restirLum(texelFetch(giDiff, q, 0).rgb), restirLum(texelFetch(giSpec, q, 0).rgb));
                n += 1.0;
            }
        now /= max(n, 1.0);
        vec2 then = vec2(restirLum(diffuseHistory), restirLum(specularHistory));
        vec2 deviation = sqrt(max(moments - then * then, vec2(0.0, 0.0))) * RESTIR_MEAN_NOISE
                       + RESTIR_CHANGE_FLOOR * max(then, now) + vec2(1e-4, 1e-4);
        vec2 z = abs(then - now) / deviation;
        vec2 confidence = vec2(1.0, 1.0) - smoothstep(vec2(RESTIR_CHANGE_FROM, RESTIR_CHANGE_FROM),
                                                      vec2(RESTIR_CHANGE_FULL, RESTIR_CHANGE_FULL), z);
        cap = min(cap, mix(vec2(RESTIR_FAST_HISTORY, RESTIR_FAST_HISTORY), cap, confidence));
    }
    bool accumulate = restirDenoise.y > 0.5;
    float diffuseLength = min(historyLength + 1.0, accumulate ? cap.x : 1.0);
    float specularLength = min(historyLength + 1.0, accumulate ? cap.y : 1.0);
    float a = historyLength > 0.0 ? 1.0 / diffuseLength : 1.0;
    float b = historyLength > 0.0 ? 1.0 / specularLength : 1.0;
    diffuseHistory = mix(diffuseHistory, diffuse, a);
    specularHistory = mix(specularHistory, specular, b);
    float ld = restirLum(diffuse);
    float ls = restirLum(specular);
    moments = vec2(mix(moments.x, ld * ld, a), mix(moments.y, ls * ls, b));
    // The first moment is the luminance of the mean: luminance is linear.
    float md = restirLum(diffuseHistory);
    float ms = restirLum(specularHistory);
    vec2 variance = max(moments - vec2(md * md, ms * ms), vec2(0.0, 0.0));
    if (diffuseLength < 4.0) {   // too little history for its moments: the neighbourhood's
        vec4 m = vec4(0.0, 0.0, 0.0, 0.0);
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++) {
                ivec2 q = clamp(p + ivec2(x, y), ivec2(0, 0), ivec2(res) - ivec2(1, 1));
                float lq = restirLum(texelFetch(giDiff, q, 0).rgb);
                float sq = restirLum(texelFetch(giSpec, q, 0).rgb);
                m += vec4(lq, lq * lq, sq, sq * sq);
            }
        m /= 9.0;
        variance = max(variance, vec2(max(m.y - m.x * m.x, 0.0), max(m.w - m.z * m.z, 0.0)) * (4.0 / diffuseLength));
    }
    gl_FragData[0] = vec4(diffuseHistory, diffuseLength);
    gl_FragData[1] = vec4(specularHistory, diffuseLength);
    gl_FragData[2] = vec4(moments, variance);
}
