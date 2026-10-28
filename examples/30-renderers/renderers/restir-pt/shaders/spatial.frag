$input v_color0
$input v_texcoord0

// spatial.frag -- pass 4, GI resolution: ReSTIR PT spatial reuse, then the resolve.
//
// This pixel's reservoir (pass 3) and up to five neighbours' from a disc round it, each on a similar
// surface (normal, distance), every path shifted to this pixel's visible point -- reconnected, replayed
// then reconnected, or replayed whole (restir_path.sc) -- and weighted by the target here times the
// shift's Jacobian. A path's contribution here needs its own path shifted too (a replayed one is only
// known by re-tracing it), so this pixel's reservoir goes through the same shift, to itself.
//
// A neighbour's path that won is checked for visibility: its reconnection, from this pixel's x_{k-1}
// to x_k (one shadow-kind ray). Without it light leaks round corners; a blocked one falls back to this
// pixel's own reservoir, as restir did (zeroing it darkened the image where small lights sit behind
// corners). A path replayed whole needs no check: its every segment was traced here.
//
// The result IS fed back: it is next frame's temporal history (sample.frag reads it), as ReSTIR DI does.
// That is what spreads a rare good path a neighbourhood further every frame. The temporal M cap keeps the
// feedback from piling up.
//
// Outputs: 0-4 the reservoir (sres0..4); 5 giDiff, the indirect diffuse light divided by the diffuse
// albedo; 6 giSpec, the indirect specular light divided by the specular lobe's directional albedo
// (pjvSpecularAlbedo) -- both what the denoiser filters and the composite multiplies back at full res.
//
// Inputs: 0-4 res0..4 (pass 3), 5 surf0, 6 surf1 (pass 2). Every ray through one restirTrace call site,
// restirPath.z of them at most. No reservoir is held whole across the traversal: each is read again from
// its texture when its shift settles, and the winner's at the end.

#include <bgfx_shader.sh>
#include "restir_path.sc"

SAMPLER2D(res0, 0);
SAMPLER2D(res1, 1);
SAMPLER2D(res2, 2);
SAMPLER2D(res3, 3);
SAMPLER2D(res4, 4);
SAMPLER2D(surf0, 5);
SAMPLER2D(surf1, 6);

#define RESTIR_SPATIAL_SAMPLES 5
#define RESTIR_GBH_MAX 11          // candidates at most: a pixel's own, and ten neighbours' where it is new
#define RESTIR_SPATIAL_RADIUS 24.0   // divided by the GI stride: 12 GI pixels

Reservoir restirLoad(ivec2 at) {
    return restirUnpack(texelFetch(res0, at, 0), texelFetch(res1, at, 0), texelFetch(res2, at, 0),
                        texelFetch(res3, at, 0), texelFetch(res4, at, 0));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    RestirSurface s = restirSurface(texelFetch(surf0, p, 0), texelFetch(surf1, p, 0));
    if (s.t < 0.0) {
        Reservoir none = restirLoad(p);
        restirWrite(none);
        gl_FragData[5] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[6] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    vec3 wo1 = normalize(cameraPos.xyz - s.x);
    uint rng = uint(p.x) * 73856093u + uint(p.y) * 19349663u + uint(frameCount.x) * 83492791u;
    pjvPcg(rng);

    float wSum = 0.0;
    float M = 0.0;
    float selected = 0.0;   // the winner's target, here
    ivec2 winAt = p;
    PtShift own = ptNoShift(s.x, s.n);   // until this pixel's own path is shifted
    PtShift win = own;
    float radius = RESTIR_SPATIAL_RADIUS / restirJitter.z;
    ivec2 size = ivec2(passTargetRes.xy);
    int samples = restirReuse.y > 0.5 ? RESTIR_SPATIAL_SAMPLES : 0;

    bool blocked = false;
#if !RESTIR_REPLAY
    // Every path reconnects at x2 (restir_path.sc): each shift is arithmetic, and the one ray is the
    // winner's visibility. The candidates: this pixel's own path, then neighbours' on a similar surface --
    // twice as many, from twice as far, where the pixel has no history of its own (just uncovered, or new
    // on screen), so it does not show one noisy path for its first frames.
    //
    // Weighted by the generalized balance heuristic: candidate i's MIS weight is its confidence-weighted
    // target in its own domain over the sum of every candidate domain's -- M_j p_j(y_i shifted to j) |J| --
    // so a domain the path cannot be shifted to simply adds nothing. (Counting only the shifts that worked
    // in M, as before, was biased bright, and the spatial->temporal feedback carried that bias for dozens
    // of frames after a camera move: what made uncovered areas flare and fade.)
    int job = samples + 1;   // as the replaying version leaves it: everything weighed
    ivec2 at[RESTIR_GBH_MAX];
    float conf[RESTIR_GBH_MAX];
    int count = 0;
    at[0] = p;
    conf[0] = restirLoad(p).M;
    count = 1;
    bool fresh = conf[0] <= 1.5;
    int wanted = samples > 0 ? (fresh ? RESTIR_GBH_MAX - 1 : samples) : 0;
    float reachOut = fresh ? radius * 2.0 : radius;
    for (int k = 0; k < 2 * (RESTIR_GBH_MAX - 1); k++) {
        if (count > wanted) break;
        float a = 2.0 * RESTIR_PI * pjvRandomUnit24(rng);
        float d = reachOut * sqrt(pjvRandomUnit24(rng));
        ivec2 q = clamp(p + ivec2(round(vec2(cos(a), sin(a)) * d)), ivec2(0, 0), size - ivec2(1, 1));
        RestirSurface o = restirSurface(texelFetch(surf0, q, 0), texelFetch(surf1, q, 0));
        if ((q.x == p.x && q.y == p.y) || o.t < 0.0 || dot(o.n, s.n) <= 0.9 || abs(o.t - s.t) >= 0.1 * s.t) continue;
        // A reservoir whose path found no light (W = 0) still counts: it is a sample of "nothing here", with
        // its confidence. Skipping it -- as this did -- weighed the bright paths as if dark ones never
        // happened: in a room where most paths come back dark, borrowed light was several times too bright
        // wherever the temporal history had not yet diluted it (uncovered areas, a fresh start).
        Reservoir n = restirLoad(q);
        if (n.M <= 0.0) continue;
        at[count] = q;
        conf[count] = n.M;
        count += 1;
    }
    for (int i = 0; i < RESTIR_GBH_MAX; i++) {
        if (i >= count) break;
        Reservoir y = restirLoad(at[i]);
        M += conf[i];
        float mine = 0.0;
        float all = 0.0;
        float here = 0.0;
        PtShift shiftedHere = ptNoShift(s.x, s.n);
        for (int j = 0; j < RESTIR_GBH_MAX; j++) {
            if (j >= count) break;
            RestirSurface sj = restirSurface(texelFetch(surf0, at[j], 0), texelFetch(surf1, at[j], 0));
            PtShift sh;
            float t = ptReconnectTarget(y, sj, normalize(cameraPos.xyz - sj.x), sh);
            all += conf[j] * t;
            if (j == i) mine = conf[j] * t;
            if (j == 0) { here = t; shiftedHere = sh; }
        }
        if (i == 0) own = shiftedHere;
        float weight = all > 0.0 ? mine / all * here * y.W : 0.0;
        wSum += weight;
        if (weight > 0.0 && pjvRandomUnit24(rng) * wSum < weight) {
            selected = here / max(shiftedHere.jacobian, 1e-12);   // the target itself, without |J|
            win = shiftedHere;
            winAt = at[i];
        }
    }
    // A borrowed path's reconnection must be visible from here.
    Reservoir winner = restirLoad(winAt);
    if ((winAt.x != p.x || winAt.y != p.y) && restirReuse.z > 0.5 && winner.k >= 2 && selected > 0.0) {
        vec3 d = winner.x - win.px;
        float reach = length(d);
        vec3 toX = d / max(reach, 1e-6);
        reach = winner.q < 0.0 ? 1e30 : max(reach - 2.0 * restirSettings.y - 0.05, 0.0);
        RestirHit hit = restirTrace(win.px + toX * (2.0 * restirSettings.y), toX, RESTIR_KIND_SHADOW, reach);
        blocked = hit.hit;
    }
#else
    // Jobs: 0 this pixel's own path, 1..samples the neighbours', then the visibility ray, then done.
    int job = 0;
    ivec2 at = p;
    Reservoir first = restirLoad(p);
    PtWalk w = ptReplay(first.seed, first.k, s, wo1);
    bool visibilityRay = false;
    vec3 toX = vec3(0.0, 1.0, 0.0);
    float reach = 0.0;
    int rays = int(restirPath.z);
    for (int i = 0; i <= rays; i++) {
        for (int j = 0; j < RESTIR_SPATIAL_SAMPLES + 3; j++) {
            if (job > samples || w.state == PT_RUNNING) break;
            // The shift in hand is done: weigh it.
            Reservoir shifting = restirLoad(at);
            PtShift shifted = ptSettleReplay(w, shifting);
            float target = ptTarget(s, wo1, shifted);
            float jacobian = shifted.jacobian;
            float weight = jacobian < 0.125 || jacobian > 8.0 ? 0.0 : target * jacobian * shifting.W * shifting.M;
            if (job == 0) own = shifted;
            // A neighbour whose path cannot be shifted here does not count towards M: it would only darken
            // the result (1/M weights; pairwise MIS is not done).
            if (job == 0 || weight > 0.0) M += shifting.M;
            wSum += weight;
            if (weight > 0.0 && pjvRandomUnit24(rng) * wSum < weight) {
                selected = target;
                win = shifted;
                winAt = at;
            }
            // The next neighbour on a similar surface.
            job += 1;
            for (int k = 0; k < RESTIR_SPATIAL_SAMPLES; k++) {
                if (job > samples) break;
                float a = 2.0 * RESTIR_PI * pjvRandomUnit24(rng);
                float d = radius * sqrt(pjvRandomUnit24(rng));
                ivec2 q = clamp(p + ivec2(round(vec2(cos(a), sin(a)) * d)), ivec2(0, 0), size - ivec2(1, 1));
                RestirSurface o = restirSurface(texelFetch(surf0, q, 0), texelFetch(surf1, q, 0));
                Reservoir n = restirLoad(q);
                if ((q.x != p.x || q.y != p.y) && o.t >= 0.0 && n.M > 0.0 && dot(o.n, s.n) > 0.9 &&
                    abs(o.t - s.t) < 0.1 * s.t) {
                    at = q;
                    w = ptReplay(n.seed, n.k, s, wo1);
                    break;
                }
                job += 1;
            }
        }
        bool borrowed = winAt.x != p.x || winAt.y != p.y;
        if (job > samples && !visibilityRay) {
            // All weighed. A borrowed path's reconnection must be visible from here.
            Reservoir winner = restirLoad(winAt);
            visibilityRay = borrowed && restirReuse.z > 0.5 && winner.k >= 2 && selected > 0.0;
            if (!visibilityRay) break;
            vec3 d = winner.x - win.px;
            reach = length(d);
            toX = d / max(reach, 1e-6);
            reach = winner.q < 0.0 ? 1e30 : max(reach - 2.0 * restirSettings.y - 0.05, 0.0);
        } else if (job > samples) {
            break;
        }
        if (i == rays) break;
        if (visibilityRay) {
            // From x_{k-1} here, which lies on a surface facing x_k: offset along the way to x_k.
            RestirHit hit = restirTrace(win.px + toX * (2.0 * restirSettings.y), toX, RESTIR_KIND_SHADOW, reach);
            blocked = hit.hit;
            break;
        }
        RestirHit hit = restirTrace(ptOrigin(w), w.dir, ptKind(w), 1e30);
        ptFeed(w, hit);
    }

#endif

    Reservoir r = restirLoad(winAt);
    if (selected > 0.0) r.q = win.q;   // the reconnection's geometry from here
    r.M = M;
#if RESTIR_REPLAY
    r.W = selected > 0.0 ? wSum / (M * selected) : 0.0;
#else
    r.W = selected > 0.0 ? wSum / selected : 0.0;   // the MIS weights already sum to one
#endif
    if (restirReuse.y < 0.5 || blocked || job <= samples) {
        // No reuse, a blocked borrowed path, or out of rays: this pixel's own reservoir, as it was.
        r = restirLoad(p);
        win = own;
    }
    r.x1 = s.x;
    r.n1 = s.n;
    r.formed = true;

    // The resolve: the path's contribution, x1's BSDF split into its lobes, each divided by what the
    // composite multiplies it back by.
    vec3 diffuse = vec3(0.0, 0.0, 0.0);
    vec3 specular = vec3(0.0, 0.0, 0.0);
    if (win.ok && r.W > 0.0) {
        PjvBsdf b = pjvBsdf(s.albedo, s.glossiness, s.metallic);
        vec3 diffuseFactor;
        vec3 spec;
        float pdf;
        pjvBsdfEvaluateSplit(b, s.n, wo1, win.firstDir, diffuseFactor, spec, pdf);
        vec3 light = win.rest * r.W;
        diffuse = diffuseFactor * light;
        vec3 specularAlbedo = pjvSpecularAlbedo(b, dot(s.n, wo1));
        specular = spec * light / max(specularAlbedo, vec3(1e-3, 1e-3, 1e-3));
    }
    // NaN never enters the histories: it would never leave.
    diffuse = vec3(diffuse.r > 0.0 ? diffuse.r : 0.0, diffuse.g > 0.0 ? diffuse.g : 0.0, diffuse.b > 0.0 ? diffuse.b : 0.0);
    specular = vec3(specular.r > 0.0 ? specular.r : 0.0, specular.g > 0.0 ? specular.g : 0.0, specular.b > 0.0 ? specular.b : 0.0);
    restirWrite(r);
    gl_FragData[5] = vec4(diffuse, 1.0);
    gl_FragData[6] = vec4(specular, 1.0);
}
