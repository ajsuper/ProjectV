$input v_color0
$input v_texcoord0

// temporal.frag -- pass 3, GI resolution: ReSTIR GI temporal reuse.
//
// The candidate (pass 2) is combined with last frame's reservoir for the same surface: the candidate's
// visible point is carried through last frame's camera to a GI pixel, whose reservoir is kept only if
// it was formed on this surface (normal and plane). Its M is capped (restirReuse.w) so old samples cannot
// drown new ones, and its sample is weighted by the target here times the reconnection Jacobian (restirJacobian).
// The result is this pixel's reservoir for this frame and the history for the next.
//
// Inputs: 0-3 cand0..3 (pass 2), 4-7 res0..3 (this pass, last frame). Outputs: res0..3.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(cand0, 0);
SAMPLER2D(cand1, 1);
SAMPLER2D(cand2, 2);
SAMPLER2D(cand3, 3);
SAMPLER2D(res0, 4);
SAMPLER2D(res1, 5);
SAMPLER2D(res2, 6);
SAMPLER2D(res3, 7);

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    Reservoir fresh = restirUnpack(texelFetch(cand0, p, 0), texelFetch(cand1, p, 0), texelFetch(cand2, p, 0), texelFetch(cand3, p, 0));
    if (fresh.t1 < 0.0 || restirReuse.x < 0.5 || restirJitter.w > 0.5) { restirWrite(fresh); return; }

    uint seed = uint(p.x) * 2654435761u + uint(p.y) * 40503u + uint(frameCount.x) * 9176u + 7u;
    restirPcg(seed);
    Reservoir r = restirEmpty();
    restirUpdate(r, fresh, restirTarget(fresh, fresh.x1, fresh.n1) * fresh.W * fresh.M, restirRandom(seed));

    vec2 res = passTargetRes.xy;
    bool inFront;
    vec2 then = restirWorldToUV(fresh.x1, prevCameraPos.xyz, prevCameraDir.xyz, res, inFront);
    ivec2 pp = ivec2(floor(then * res));
    if (inFront && pp.x >= 0 && pp.y >= 0 && pp.x < int(res.x) && pp.y < int(res.y)) {
        Reservoir h = restirUnpack(texelFetch(res0, pp, 0), texelFetch(res1, pp, 0), texelFetch(res2, pp, 0), texelFetch(res3, pp, 0));
        if (h.t1 >= 0.0 && h.M > 0.0 && restirSameSurface(h.x1, h.n1, fresh.x1, fresh.n1, fresh.t1)) {
            h.M = min(h.M, restirReuse.w);
            float jacobian = restirJacobian(h, fresh.x1);
            float target = restirTarget(h, fresh.x1, fresh.n1) * jacobian;
            restirUpdate(r, h, target * h.W * h.M, restirRandom(seed));
        }
    }

    float target = restirTarget(r, fresh.x1, fresh.n1);
    r.W = target > 0.0 ? r.wSum / (r.M * target) : 0.0;
    r.x1 = fresh.x1;
    r.n1 = fresh.n1;
    r.t1 = fresh.t1;
    restirWrite(r);
}
