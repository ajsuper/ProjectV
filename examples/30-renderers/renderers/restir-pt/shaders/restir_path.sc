// restir_path.sc -- the paths of ReSTIR PT: one walk, run three ways, and the reconnection.
//
// A path starts at a GI pixel's visible point x1 and takes up to restirPath.x bounces (x2 .. x4), each
// direction sampled from the surface's BSDF (pjv_bsdf.sc), with next-event estimation to the sun at every
// vertex after x1 (the sun at x1 is the direct light's: shadow_soft.frag) and emission wherever it meets
// it. What it carries is kept with x1's BSDF left out -- "rest" below -- so the pixel that reuses it
// multiplies its own in (and splits it into diffuse and specular).
//
// The walk is a state machine, fed one ray at a time, because each pass traces from ONE call site inside
// a loop (a second inlined traversal slows every ray in the shader). It runs as
//
//   PT_CANDIDATE  a new path, with fresh random numbers. It finds its reconnection vertex x_k -- the first
//                 rough vertex more than a few voxels from a rough vertex before it (the sky counts as a
//                 rough vertex infinitely far away) -- records it, and samples the rest of the path from it
//                 as the light arriving along wk.
//   PT_REPLAY     a stored path from another visible point: its vertices before x_{k-1} re-traced with the
//                 path's own random numbers (random replay), stopping at x_{k-1}; or, with k = 0, the
//                 whole path. It fails if the replayed path would reconnect earlier, or ends early: the
//                 shift must be one the other pixel could invert.
//   PT_SUFFIX     validation: a stored path's suffix re-traced from x_k with the same random numbers. On an
//                 unchanged scene it comes back identical; what it brings back differently is light that
//                 changed (docs/plans/restir-pt.md, "Stale light").
//
// Random numbers: each vertex has its own stream (the seed hashed with its index), two draws for the
// sun's disc and three for the bounce, so a replay takes the same decisions vertex by vertex however many
// numbers an earlier vertex used.

#include "restir_trace.sc"

#define PT_CANDIDATE 0
#define PT_REPLAY    1
#define PT_SUFFIX    2

#define PT_RUNNING 0
#define PT_DONE    1
#define PT_FAILED  2

#define RESTIR_ROUGH_ALPHA 0.2   // rough enough to reconnect at (pjv_bsdf.sc's alpha): glossiness under ~0.55
#define RESTIR_RC_VOXELS   0.0   // ...across more than this many of the finest voxels

// Random replay: shifting a path whose reconnection vertex is past x2 re-traces its prefix. With the
// paths' glossiness capped (restir_common.sc) every vertex is rough and every path reconnects at x2, so
// nothing is ever replayed -- and compiling the replay out takes its state off every ray (the spatial
// pass then traces only a visibility ray). 1 restores it, for paths through glossy vertices.
#ifndef RESTIR_REPLAY
#define RESTIR_REPLAY 0
#endif
#define RESTIR_SKY_FAR     1.0e4 // a reconnection to the sky: a point this far out along it

// Everything below is held across the traversal, which is large and inlined: what is live there costs
// occupancy on every ray. So the walk keeps a material rather than BSDFs, derives each ray's origin and
// kind, and reduces a new path's reconnection to two vectors and a density when it is found.

bool ptRough(float glossiness) { return pjvBsdfAlpha(glossiness) > RESTIR_ROUGH_ALPHA; }
float ptMinDistance() { return RESTIR_RC_VOXELS * 20.0 * restirSettings.y; }
float ptSunPdf() { return 1.0 / pathSunSolidAngle(); }
int ptLastVertex() { return 1 + int(restirPath.x); }

uint ptVertexStream(uint seed, int vertex) {
    uint s = seed * 2654435761u + uint(vertex) * 374761393u + 1013904223u;
    pjvPcg(s);
    return s;
}

struct PtWalk {
    int   mode;
    int   state;
    uint  seed;
    int   stopAt;        // PT_REPLAY: the vertex to stop at (k - 1); 0: the whole path
    // the vertex the walk is at
    int   vertex;
    vec3  x;
    vec3  n;
    vec3  wo;            // towards the vertex before
    vec3  albedo;
    float glossiness;
    float metallic;
    bool  sharp;         // seen sharply: every vertex before it glossy (its rays are exact)
    bool  atRc;          // it is the reconnection vertex
    bool  suffix;        // past the reconnection vertex
    // what the path has gathered, x1's BSDF left out
    vec3  T;             // prefix: throughput to this vertex (not through its own BSDF)
    vec3  S;             // prefix: light gathered
    vec3  Ts;            // suffix: throughput from the light arriving at x_k along wk
    vec3  Ls;            // suffix: that light
    vec3  step;          // the bounce in flight's weight: f cos / pdf, applied when it arrives
    vec3  firstDir;      // out of x1
    float firstPdf;
    float sunWeight;     // the MIS weight if the path ray in flight escapes onto the sun's disc
    // the ray in flight: from x along dir (a shadow ray: to the sun)
    vec3  dir;
    bool  shadowRay;
    vec3  pending;       // the shadow ray's light, if nothing blocks it
    // the reconnection vertex: recorded (PT_CANDIDATE) or given (PT_SUFFIX). A candidate's contribution
    // is then rcBase + rcCoef * (the light leaving x_k along -rcDir); rcPdf the density rcDir had.
    int   k;
    vec3  rcX;
    vec3  rcN;
    vec3  rcAlbedo;
    float rcGlossiness;
    float rcMetallic;
    vec3  rcLe;
    float rcVis;
    vec3  rcWk;
    float rcQ;
    vec3  rcDir;
    vec3  rcBase;
    vec3  rcCoef;
    float rcPdf;
};

PjvBsdf ptBsdf(PtWalk w) { return pjvBsdf(w.albedo, w.glossiness, w.metallic); }

// The ray in flight, for restirTrace.
vec3 ptOrigin(PtWalk w) { return w.x + w.n * restirSettings.y; }
int ptKind(PtWalk w) {
    if (w.shadowRay) return w.sharp ? RESTIR_KIND_SHADOW : RESTIR_KIND_SHADOW_INDIRECT;
    return w.sharp && !ptRough(w.glossiness) ? RESTIR_KIND_BOUNCE_SHARP : RESTIR_KIND_BOUNCE;
}

// The bounce out of the current vertex, or the end of the path.
void ptSampleNext(inout PtWalk w) {
    if (w.vertex >= ptLastVertex()) {
        w.state = w.mode == PT_REPLAY && w.stopAt > 0 ? PT_FAILED : PT_DONE;
        return;
    }
    uint s = ptVertexStream(w.seed, w.vertex);
    pjvRandomUnit24(s);   // the sun's two
    pjvRandomUnit24(s);
    float uLobe = pjvRandomUnit24(s);
    float u1 = pjvRandomUnit24(s);
    float u2 = pjvRandomUnit24(s);
    vec3 d;
    if (w.atRc) {
        // Cosine-weighted, not from the BSDF: the light found along wk is then reflected towards ANY
        // x_{k-1} as pi f L, with no density that depends on where the path came from. Rounded to what
        // the reservoir keeps of it before it is traced, so validation re-traces exactly this ray.
        d = w.mode == PT_SUFFIX ? w.rcWk : restirUnpackDir(restirPackDir(pjvSampleCosine(w.n, u1, u2)));
        float c = dot(w.n, d);
        if (c <= 0.0) { w.state = PT_DONE; return; }
        w.rcWk = d;
        w.Ts = vec3(1.0, 1.0, 1.0);
        w.step = vec3(1.0, 1.0, 1.0);   // the suffix gathers the light arriving along wk itself
        w.suffix = true;
        w.sunWeight = pjvPowerHeuristic(c / RESTIR_PI, ptSunPdf());
    } else {
        PjvBsdf b = ptBsdf(w);
        d = pjvBsdfSample(b, w.n, w.wo, uLobe, u1, u2);
        float c = dot(w.n, d);
        vec3 f;
        float pdf;
        pjvBsdfEvaluate(b, w.n, w.wo, d, f, pdf);
        if (c <= 0.0 || pdf <= 0.0) {
            w.state = w.mode == PT_REPLAY && w.stopAt > 0 ? PT_FAILED : PT_DONE;
            return;
        }
        if (w.vertex == 1) {
            // x1's BSDF is the reusing pixel's to apply: only the cosine and the density stay.
            w.step = vec3(c / pdf, c / pdf, c / pdf);
            w.firstDir = d;
            w.firstPdf = pdf;
            w.sunWeight = 0.0;   // the sun at x1 is the direct light's
        } else {
            w.step = f * (c / pdf);
            w.sunWeight = pjvPowerHeuristic(pdf, ptSunPdf());
        }
    }
    w.dir = d;
    w.shadowRay = false;
    w.atRc = false;
}

// After a vertex's light is in: stop there (a replay's x_{k-1}), or bounce on.
void ptAfterVertex(inout PtWalk w) {
    if (w.mode == PT_REPLAY && w.stopAt > 0 && w.vertex == w.stopAt) { w.state = PT_DONE; return; }
    ptSampleNext(w);
}

// Next-event estimation at the current vertex: a point of the sun's disc, weighted against the bounce
// finding it -- or, at the reconnection vertex, only whether the sun is seen (a shift re-evaluates its
// BSDF for the new incoming direction).
void ptSunAtVertex(inout PtWalk w) {
    uint s = ptVertexStream(w.seed, w.vertex);
    float u1 = pjvRandomUnit24(s);
    float u2 = pjvRandomUnit24(s);
    vec3 toSun = pathSampleSun(u1, u2);
    float c = dot(w.n, toSun);
    if (c > 0.0 && restirLum(SUN_COLOR) > 0.0) {
        if (!w.atRc) {
            vec3 f;
            float pdf;
            pjvBsdfEvaluate(ptBsdf(w), w.n, w.wo, toSun, f, pdf);
            // L / pdf of the uniform sample of the disc is its irradiance, SUN_COLOR.
            w.pending = (w.suffix ? w.Ts : w.T) * f * (c * pjvPowerHeuristic(ptSunPdf(), pdf)) * SUN_COLOR;
        }
        if (w.atRc || restirLum(w.pending) > 0.0) {
            w.dir = toSun;
            w.shadowRay = true;
            return;
        }
    }
    ptAfterVertex(w);
}

void ptOnShadow(inout PtWalk w, RestirHit hit) {
    if (!hit.hit) {
        if (w.atRc) w.rcVis = 1.0;
        else if (w.suffix) w.Ls += w.pending;
        else w.S += w.pending;
    }
    ptAfterVertex(w);
}

// A new path found its reconnection vertex, from the current vertex x_{k-1} along dir: reduce what it
// needs of x_{k-1} to the contribution's affine form (rest = base + coef * light at x_k).
void ptRecordReconnection(inout PtWalk w, vec3 x, vec3 n) {
    vec3 d = x - w.x;
    float len2 = max(dot(d, d), 1e-8);
    vec3 dir = d * inversesqrt(len2);
    float cosPrev = max(dot(w.n, dir), 0.0);
    w.k = w.vertex + 1;
    w.rcDir = dir;
    if (w.vertex == 1) {
        w.rcBase = vec3(0.0, 0.0, 0.0);
        w.rcCoef = vec3(cosPrev, cosPrev, cosPrev);
        w.rcPdf = w.firstPdf;
        w.firstDir = dir;
    } else {
        vec3 f;
        float pdf;
        pjvBsdfEvaluate(ptBsdf(w), w.n, w.wo, dir, f, pdf);
        // The light gathered before x_{k-1} does not depend on the reconnecting direction; times its
        // density, it integrates over that direction to itself.
        w.rcBase = w.S * pdf;
        w.rcCoef = w.T * f * cosPrev;
        w.rcPdf = pdf;
    }
    w.rcVis = 0.0;
    w.rcQ = abs(dot(n, dir)) / len2;
}

void ptOnPath(inout PtWalk w, RestirHit hit) {
    if (!hit.hit) {
        vec3 sky = pathSky(w.dir) + (pathInSun(w.dir) ? pathSunRadiance() * w.sunWeight : vec3(0.0, 0.0, 0.0));
        if (w.suffix) { w.Ls += w.Ts * w.step * sky; w.state = PT_DONE; return; }
        // The next vertex is the sky: a reconnection, if this vertex is rough.
        if (ptRough(w.glossiness)) {
            if (w.mode != PT_CANDIDATE) { w.state = PT_FAILED; return; }
            ptRecordReconnection(w, w.x + w.dir * RESTIR_SKY_FAR, -w.dir);
            w.rcX = w.x + w.dir * RESTIR_SKY_FAR;
            w.rcN = -w.dir;
            w.rcAlbedo = vec3(0.0, 0.0, 0.0);
            w.rcGlossiness = 0.0;
            w.rcMetallic = 0.0;
            w.rcLe = pathSky(w.dir);
            w.rcQ = -1.0;
            w.state = PT_DONE;
            return;
        }
        if (w.mode == PT_REPLAY && w.stopAt > 0) { w.state = PT_FAILED; return; }
        w.S += w.T * w.step * sky;
        w.state = PT_DONE;
        return;
    }
    vec3 x = ptOrigin(w) + w.dir * hit.t;
    vec3 n = dot(hit.normal, hit.normal) > 0.5 ? normalize(hit.normal) : -w.dir;
    bool rough = ptRough(hit.glossiness);
    bool wasRough = ptRough(w.glossiness);
    bool reconnect = !w.suffix && wasRough && rough && hit.t > ptMinDistance();
    if (reconnect && w.mode != PT_CANDIDATE) { w.state = PT_FAILED; return; }
    if (reconnect) {
        n = restirUnpackDir(restirPackDir(n));   // as the reservoir keeps it: what validation starts from
        ptRecordReconnection(w, x, n);
        w.rcX = x;
        w.rcN = n;
        w.rcAlbedo = hit.albedo;
        w.rcGlossiness = hit.glossiness;
        w.rcMetallic = hit.metallic;
        w.rcLe = hit.emission;
        w.atRc = true;
    }
    if (w.suffix) w.Ts *= w.step;
    else w.T *= w.step;
    w.sharp = w.sharp && !wasRough;
    w.vertex += 1;
    w.x = x;
    w.n = n;
    w.wo = -w.dir;
    w.albedo = hit.albedo;
    w.glossiness = hit.glossiness;
    w.metallic = hit.metallic;
    if (w.suffix) w.Ls += w.Ts * hit.emission;
    else if (!w.atRc) w.S += w.T * hit.emission;
    ptSunAtVertex(w);
}

void ptFeed(inout PtWalk w, RestirHit hit) {
    if (w.shadowRay) ptOnShadow(w, hit);
    else ptOnPath(w, hit);
}

PtWalk ptWalk(int mode, uint seed, vec3 x1, vec3 n1, vec3 wo1, vec3 albedo, float glossiness, float metallic) {
    PtWalk w;
    w.mode = mode;
    w.state = PT_RUNNING;
    w.seed = seed;
    w.stopAt = 0;
    w.vertex = 1;
    w.x = x1;
    w.n = n1;
    w.wo = wo1;
    w.albedo = albedo;
    w.glossiness = glossiness;
    w.metallic = metallic;
    w.sharp = true;
    w.atRc = false;
    w.suffix = false;
    w.T = vec3(1.0, 1.0, 1.0);
    w.S = vec3(0.0, 0.0, 0.0);
    w.Ts = vec3(0.0, 0.0, 0.0);
    w.Ls = vec3(0.0, 0.0, 0.0);
    w.step = vec3(1.0, 1.0, 1.0);
    w.firstDir = n1;
    w.firstPdf = 0.0;
    w.sunWeight = 0.0;
    w.dir = n1;
    w.shadowRay = false;
    w.pending = vec3(0.0, 0.0, 0.0);
    w.k = 0;
    w.rcX = x1;
    w.rcN = n1;
    w.rcAlbedo = vec3(0.0, 0.0, 0.0);
    w.rcGlossiness = 0.0;
    w.rcMetallic = 0.0;
    w.rcLe = vec3(0.0, 0.0, 0.0);
    w.rcVis = 0.0;
    w.rcWk = n1;
    w.rcQ = -1.0;
    w.rcDir = n1;
    w.rcBase = vec3(0.0, 0.0, 0.0);
    w.rcCoef = vec3(0.0, 0.0, 0.0);
    w.rcPdf = 0.0;
    return w;
}

// A new path from the visible point.
PtWalk ptCandidate(uint seed, RestirSurface s, vec3 wo1) {
    PtWalk w = ptWalk(PT_CANDIDATE, seed, s.x, s.n, wo1, s.albedo, s.glossiness, s.metallic);
    ptSampleNext(w);
    return w;
}

// A stored path (its seed and k) replayed from this visible point: its prefix up to x_{k-1}, or all of
// it (k = 0). k = 2 has nothing to replay: the walk is done before it starts, at x1.
PtWalk ptReplay(uint seed, int k, RestirSurface s, vec3 wo1) {
    PtWalk w = ptWalk(PT_REPLAY, seed, s.x, s.n, wo1, s.albedo, s.glossiness, s.metallic);
    w.k = k;
    w.stopAt = k >= 2 ? k - 1 : 0;
    if (k == 2) w.state = PT_DONE;
#if RESTIR_REPLAY
    else ptSampleNext(w);
#else
    else w.state = PT_FAILED;
#endif
    return w;
}

// Reservoir h's suffix re-traced from its reconnection vertex (validation). Only for k >= 2, not the sky.
PtWalk ptSuffix(Reservoir h) {
    PtWalk w = ptWalk(PT_SUFFIX, h.seed, h.x, h.n, h.n, h.albedo, h.glossiness, h.metallic);
    w.vertex = h.k;
    w.k = h.k;
    w.sharp = false;
    w.atRc = true;
    w.rcWk = h.wk;
    ptSunAtVertex(w);
    return w;
}

// The light leaving a reconnection vertex towards wo: its emission, the sun through its BSDF (if it sees
// it), and the light along wk through its BSDF -- cosine-sampled, so f cos L / (cos / pi).
vec3 ptReconnectionLight(vec3 n, vec3 albedo, float glossiness, float metallic, vec3 Le, float vis, vec3 wk, vec3 L,
                         float q, vec3 wo) {
    if (q < 0.0) return Le;
    PjvBsdf b = pjvBsdf(albedo, glossiness, metallic);
    vec3 f;
    float pdf;
    pjvBsdfEvaluate(b, n, wo, wk, f, pdf);
    vec3 light = Le + RESTIR_PI * f * L;
    vec3 toSun = normalize(SUN_DIR);
    float c = dot(n, toSun);
    if (vis > 0.5 && c > 0.0) {
        pjvBsdfEvaluate(b, n, wo, toSun, f, pdf);
        light += f * (c * pjvPowerHeuristic(ptSunPdf(), c / RESTIR_PI)) * SUN_COLOR;
    }
    return light;
}

// What a path brings to a visible point, x1's BSDF left out.
struct PtShift {
    bool  ok;
    vec3  rest;       // the contribution without x1's BSDF: with it, f(x1) rest is the integrand
    vec3  firstDir;   // out of x1, for that BSDF
    float sourcePdf;  // the density the path's last free direction was sampled with (1: sampled whole)
    float q;          // cos / d^2 at x_k from here (-1: the sky)
    float jacobian;   // the reconnection's: q here over q where the path was formed
    vec3  px;         // x_{k-1} here, for the reconnection's visibility ray
};

PtShift ptNoShift(vec3 x1, vec3 n1) {
    PtShift r;
    r.ok = false;
    r.rest = vec3(0.0, 0.0, 0.0);
    r.firstDir = n1;
    r.sourcePdf = 1.0;
    r.q = -1.0;
    r.jacobian = 1.0;
    r.px = x1;
    return r;
}

// A new path, done: its contribution here, its own (Jacobian 1).
PtShift ptSettleCandidate(PtWalk w) {
    PtShift r = ptNoShift(w.rcX, w.rcN);
    r.ok = w.state == PT_DONE;
    r.firstDir = w.firstDir;
    if (!r.ok) return r;
    if (w.k < 2) {   // no reconnection: what it gathered is all of it, sampled whole
        r.rest = w.S;
        return r;
    }
    vec3 light = ptReconnectionLight(w.rcN, w.rcAlbedo, w.rcGlossiness, w.rcMetallic, w.rcLe, w.rcVis, w.rcWk, w.Ls,
                                     w.rcQ, -w.rcDir);
    r.rest = w.rcBase + w.rcCoef * light;
    r.sourcePdf = w.rcPdf;
    r.q = w.rcQ;
    return r;
}

// A replay of h, done (at x_{k-1}, or at the end of a path replayed whole): its contribution here.
PtShift ptSettleReplay(PtWalk w, Reservoir h) {
    PtShift r = ptNoShift(w.x, w.n);
    r.ok = w.state == PT_DONE;
    r.firstDir = w.firstDir;
    r.px = w.x;
    if (!r.ok) return r;
    if (h.k < 2) {
        r.rest = w.S;
        return r;
    }
    vec3 d = h.x - w.x;
    float len2 = max(dot(d, d), 1e-8);
    vec3 dir = d * inversesqrt(len2);
    float cosPrev = dot(w.n, dir);
    // The shifted path must reconnect where the stored one did: x_{k-1} rough, far enough from x_k.
    if (cosPrev <= 0.0 || !ptRough(w.glossiness) || (h.q >= 0.0 && len2 <= ptMinDistance() * ptMinDistance())) {
        r.ok = false;
        return r;
    }
    r.q = h.q < 0.0 ? -1.0 : abs(dot(h.n, dir)) / len2;
    r.jacobian = h.q < 0.0 ? 1.0 : r.q / max(h.q, 1e-12);
    vec3 light = ptReconnectionLight(h.n, h.albedo, h.glossiness, h.metallic, h.Le, h.vis, h.wk, h.L, h.q, -dir);
    if (h.k == 2) {
        r.firstDir = dir;
        r.rest = cosPrev * light;
    } else {
        vec3 f;
        float pdf;
        pjvBsdfEvaluate(ptBsdf(w), w.n, w.wo, dir, f, pdf);
        r.rest = w.S * pdf + w.T * f * cosPrev * light;
        r.sourcePdf = pdf;
    }
    return r;
}

// The target function at a visible point: the luminance of the full contribution, x1's BSDF included --
// view-dependent, so a shift re-evaluates it.
float ptTarget(RestirSurface s, vec3 wo1, PtShift p) {
    if (!p.ok) return 0.0;
    vec3 f;
    float pdf;
    pjvBsdfEvaluate(pjvBsdf(s.albedo, s.glossiness, s.metallic), s.n, wo1, p.firstDir, f, pdf);
    return max(restirLum(f * p.rest), 0.0);
}

// A stored path (k = 2: every path, with replay compiled out) reconnected from another visible point (s,
// seen along wo), without a walk: what the shift brings there, and its target pulled back into the path's
// own domain -- p(shifted) |J|, 0 if the shift is refused. What the reuse's MIS weights are built from.
float ptReconnectTarget(Reservoir y, RestirSurface s, vec3 wo, out PtShift r) {
    r = ptNoShift(s.x, s.n);
    if (y.k != 2 || y.W <= 0.0) return 0.0;
    vec3 d = y.x - s.x;
    float len2 = max(dot(d, d), 1e-8);
    vec3 dir = d * inversesqrt(len2);
    float cosPrev = dot(s.n, dir);
    if (cosPrev <= 0.0 || !ptRough(s.glossiness)) return 0.0;
    r.q = y.q < 0.0 ? -1.0 : abs(dot(y.n, dir)) / len2;
    r.jacobian = y.q < 0.0 ? 1.0 : r.q / max(y.q, 1e-12);
    if (r.jacobian < 0.125 || r.jacobian > 8.0) return 0.0;
    r.ok = true;
    r.firstDir = dir;
    r.rest = cosPrev * ptReconnectionLight(y.n, y.albedo, y.glossiness, y.metallic, y.Le, y.vis, y.wk, y.L, y.q, -dir);
    return ptTarget(s, wo, r) * r.jacobian;
}
