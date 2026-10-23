$input v_color0
$input v_texcoord0

// sample.frag -- pass 3, GI resolution: a new path, and ReSTIR PT's temporal reuse.
//
//   1. A candidate: a path from this GI pixel's visible point x1 (restir_path.sc), with fresh random
//      numbers. A reservoir of one: M = 1, W = 1 / the density of its last free direction (the
//      reconnection's, or 1 for a path replayed whole).
//   2. Validation (restirPath.w; a quarter of the history's pixels a frame, in turn): the history's path
//      suffix re-traced from its reconnection vertex with its own random numbers. Unchanged light comes
//      back identical; light that changed -- a lamp switched off, a door opened, nothing the renderer is
//      told of -- replaces the cached light and drops the history's M to 1, so a reservoir's stale light
//      is gone within four frames of its age. (The sun moving is told: every history starts over then.)
//   3. Temporal reuse: last frame's reservoir for the same surface -- the one the spatial pass ended last
//      frame with, so what spatial reuse found carries forward -- shifted here (reconnected, or replayed
//      and then reconnected, or replayed whole), weighted by the target here times the shift's Jacobian,
//      its M capped (restirReuse.w) so old samples cannot drown new ones.
//
// Every ray, all three steps', goes through one restirTrace call site in one loop (restirPath.y rays). The
// history goes first and the candidate last, and neither reservoir is held whole across the traversal:
// the history is read again from its texture when it is needed, the candidate is the walk itself.
//
// Inputs: 0 surf0, 1 surf1 (pass 2); 2-6 sres0..4 (the spatial pass, last frame). Outputs: res0..4.

#include <bgfx_shader.sh>
#include "restir_path.sc"

// Validation (step 2) found nothing worth its cost in what was measured (docs/plans/restir-pt.md), and
// compiled in it keeps a second walk's state on every ray. 1 builds it; restirPath.w (RESTIR_VALIDATE=1
// in the host) then turns it on.
#ifndef RESTIR_VALIDATE
#define RESTIR_VALIDATE 0
#endif

SAMPLER2D(surf0, 0);
SAMPLER2D(surf1, 1);
SAMPLER2D(sres0, 2);
SAMPLER2D(sres1, 3);
SAMPLER2D(sres2, 4);
SAMPLER2D(sres3, 5);
SAMPLER2D(sres4, 6);

Reservoir restirHistory(ivec2 at) {
    return restirUnpack(texelFetch(sres0, at, 0), texelFetch(sres1, at, 0), texelFetch(sres2, at, 0),
                        texelFetch(sres3, at, 0), texelFetch(sres4, at, 0));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    RestirSurface s = restirSurface(texelFetch(surf0, p, 0), texelFetch(surf1, p, 0));
    if (s.t < 0.0) { Reservoir none = restirEmpty(); restirWrite(none); return; }
    vec3 wo1 = normalize(cameraPos.xyz - s.x);

    uint seed = uint(p.x) * 7919u + uint(p.y) * 104729u + uint(frameCount.x) * 15485863u;
    restirPcg(seed);
    seed = seed & 0xFFFFFu;   // what the reservoir keeps of it
    uint rng = seed * 2654435761u + 7u;
    restirPcg(rng);

    // The history, if this surface was seen last frame. Not on the first frame, nor when the sun moved: a
    // reservoir's path carries the light it saw, which a new sun has made wrong.
    bool history = false;
    bool validate = false;
    ivec2 pp = ivec2(0, 0);
    if (restirReuse.x > 0.5 && restirJitter.w < 0.5 && frameCount.z < 0.5) {
        vec2 res = passTargetRes.xy;
        bool inFront;
        vec2 then = restirWorldToUV(s.x, prevCameraPos.xyz, prevCameraDir.xyz, res, inFront);
        pp = ivec2(floor(then * res));
        if (inFront && pp.x >= 0 && pp.y >= 0 && pp.x < int(res.x) && pp.y < int(res.y)) {
            Reservoir h = restirHistory(pp);
            // An empty history (its path found no light: W = 0) is still a history -- see spatial.frag.
            history = h.formed && h.M > 0.0 && restirSameSurface(h.x1, h.n1, s.x, s.n, s.t);
#if RESTIR_VALIDATE
            validate = history && restirPath.w > 0.0 && h.k >= 2 && h.q >= 0.0 &&
                       ((pp.x & 1) + 2 * (pp.y & 1)) == (int(frameCount.x) & 3);
#endif
        }
    }

    // The resampling, kept as numbers: which sample won (1 the history, 2 the candidate), and what
    // validation changed in the history's path.
    float wSum = 0.0;
    float M = 0.0;
    float selected = 0.0;   // the winner's target, here
    int winner = 0;
    float historyQ = -1.0;
    vec3 historyL = vec3(0.0, 0.0, 0.0);
    float historyVis = 0.0;
    bool validated = false;

#if RESTIR_REPLAY
    int job = validate ? 0 : (history ? 1 : 2);   // 0 validating the history, 1 shifting it here, 2 the candidate
#else
    int job = validate ? 0 : 2;   // the history needs no ray to shift: it is weighed after the loop
    PtShift candidate = ptNoShift(s.x, s.n);
    float candidateTarget = 0.0;
    float candidateW = 0.0;
#endif
    PtWalk w;
#if RESTIR_VALIDATE
    if (job == 0) w = ptSuffix(restirHistory(pp));
    else
#endif
    if (job == 1) { Reservoir h = restirHistory(pp); w = ptReplay(h.seed, h.k, s, wo1); }
    else w = ptCandidate(seed, s, wo1);
    int rays = int(restirPath.y);
    for (int i = 0; i <= rays; i++) {
        // Settle what is done and start what comes next. None of this traces.
        for (int j = 0; j < 3; j++) {
            if (job >= 3 || w.state == PT_RUNNING) break;
#if RESTIR_VALIDATE
            if (job == 0) {
                Reservoir h = restirHistory(pp);
                float was = restirLum(h.L);
                float now = restirLum(w.Ls);
                if (w.state == PT_DONE &&
                    (abs(now - was) > restirPath.w * max(now, was) + 1e-4 || abs(w.rcVis - h.vis) > 0.5)) {
                    validated = true;
                    historyL = w.Ls;
                    historyVis = w.rcVis;
                }
#if RESTIR_REPLAY
                job = 1;
                w = ptReplay(h.seed, h.k, s, wo1);
#else
                job = 2;
                w = ptCandidate(seed, s, wo1);
#endif
            } else
#endif
            if (job == 1) {
                Reservoir h = restirHistory(pp);
                if (validated) {
                    h.L = historyL;
                    h.vis = historyVis;
                    h.M = 1.0;
                }
                h.M = min(h.M, restirReuse.w);
                PtShift shifted = ptSettleReplay(w, h);
                float target = ptTarget(s, wo1, shifted);
                // Shifts that change the reconnection's geometry more than 8x either way are refused:
                // the near-grazing ones, whose weights blow up.
                float jacobian = shifted.jacobian;
                float weight = jacobian < 0.125 || jacobian > 8.0 ? 0.0 : target * jacobian * h.W * h.M;
                // Only a history that could be shifted here counts towards M: one that could not would
                // only darken the result (1/M weights; pairwise MIS is not done).
                if (weight > 0.0) {
                    wSum += weight;
                    M += h.M;
                    winner = 1;
                    selected = target;
                    historyQ = shifted.q;
                }
                job = 2;
                w = ptCandidate(seed, s, wo1);
            } else {
                PtShift c = ptSettleCandidate(w);
                float target = ptTarget(s, wo1, c);
                float W = target > 0.0 && c.sourcePdf > 0.0 ? 1.0 / c.sourcePdf : 0.0;
#if RESTIR_REPLAY
                float weight = target * W;
                wSum += weight;
                M += 1.0;
                if (weight > 0.0 && restirRandom(rng) * wSum < weight) {
                    winner = 2;
                    selected = target;
                }
#else
                candidate = c;
                candidateTarget = target;
                candidateW = W;
#endif
                job = 3;
            }
        }
        if (job >= 3 || i == rays) break;
        RestirHit hit = restirTrace(ptOrigin(w), w.dir, ptKind(w), 1e30);
        ptFeed(w, hit);
    }

#if !RESTIR_REPLAY
    // The candidate and the history, weighed by the generalized balance heuristic over their two domains --
    // this frame's visible point, and the one the history was formed at (its point and normal, seen from last
    // frame's camera; the material is this one's, the same surface). Each one's MIS weight is its own
    // confidence-weighted target over both domains', so a history that cannot be shifted here adds nothing
    // rather than biasing the result (see spatial.frag).
    {
        Reservoir yc = restirEmpty();
        yc.k = w.k;
        yc.x = w.rcX;
        yc.n = w.rcN;
        yc.albedo = w.rcAlbedo;
        yc.glossiness = w.rcGlossiness;
        yc.metallic = w.rcMetallic;
        yc.Le = w.rcLe;
        yc.vis = w.rcVis;
        yc.wk = w.rcWk;
        yc.L = w.Ls;
        yc.q = w.rcQ;
        yc.W = candidateW;
        float mc = 1.0;
        float mh = 0.0;
        Reservoir h = restirEmpty();
        RestirSurface then = s;
        vec3 woThen = wo1;
        if (history) {
            h = restirHistory(pp);
            if (validated) {
                h.L = historyL;
                h.vis = historyVis;
                h.M = 1.0;
            }
            mh = min(h.M, restirReuse.w);
            then.x = h.x1;
            then.n = h.n1;
            then.t = length(h.x1 - prevCameraPos.xyz);
            woThen = normalize(prevCameraPos.xyz - h.x1);
        }
        PtShift unused;
        float cThen = history ? ptReconnectTarget(yc, then, woThen, unused) : 0.0;
        float weightC = candidateTarget > 0.0 ? mc * candidateTarget / (mc * candidateTarget + mh * cThen) * candidateTarget * candidateW : 0.0;
        wSum += weightC;
        M += mc;
        if (weightC > 0.0) {
            winner = 2;
            selected = candidateTarget;
        }
        if (history) {
            PtShift hNow;
            float hHere = ptReconnectTarget(h, s, wo1, hNow);   // p here |J|
            float hOwn = ptReconnectTarget(h, then, woThen, unused);
            float denominator = mh * hOwn + mc * hHere;
            float weightH = denominator > 0.0 ? mh * hOwn / denominator * hHere * h.W : 0.0;
            wSum += weightH;
            M += mh;
            if (weightH > 0.0 && restirRandom(rng) * wSum < weightH) {
                winner = 1;
                selected = hHere / max(hNow.jacobian, 1e-12);
                historyQ = hNow.q;
            }
        }
    }
#endif

    Reservoir r = restirEmpty();
    if (winner == 2) {
        r.seed = seed;
        r.k = w.k;
        r.x = w.rcX;
        r.n = w.rcN;
        r.albedo = w.rcAlbedo;
        r.glossiness = w.rcGlossiness;
        r.metallic = w.rcMetallic;
        r.Le = w.rcLe;
        r.vis = w.rcVis;
        r.wk = w.rcWk;
        r.L = w.Ls;
        r.q = w.rcQ;
    } else if (winner == 1) {
        r = restirHistory(pp);
        if (validated) {
            r.L = historyL;
            r.vis = historyVis;
        }
        r.q = historyQ;
    }
    r.M = M;
#if RESTIR_REPLAY
    r.W = selected > 0.0 ? wSum / (M * selected) : 0.0;
#else
    r.W = selected > 0.0 ? wSum / selected : 0.0;   // the MIS weights already sum to one
#endif
    r.x1 = s.x;
    r.n1 = s.n;
    r.formed = true;
    restirWrite(r);
}
