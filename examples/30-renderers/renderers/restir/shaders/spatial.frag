$input v_color0
$input v_texcoord0

// spatial.frag -- pass 4, GI resolution: ReSTIR GI spatial reuse, then the resolve.
//
// The pixel's reservoir (pass 3) is combined with up to five neighbours' from a disc round it, each kept
// only if formed on a similar surface and weighted by the target here times the Jacobian. A sample taken
// from a neighbour is then checked for visibility from this pixel's point (one shadow-kind ray, through
// the one restirTrace call site) -- without it, light leaks round corners -- and a blocked one is replaced by
// the pixel's own reservoir. (The result is not fed back: the
// temporal history stays pass 3's, as in the paper.)
//
// Output, giIrr: the indirect light leaving the surface, divided by its albedo -- Lo cos W / pi -- which
// the denoiser filters and the composite multiplies back by the full-res albedo.
//
// Inputs: 0-3 res0..3 (pass 3, this frame).

#include <bgfx_shader.sh>
#include "restir_trace.sc"

SAMPLER2D(res0, 0);
SAMPLER2D(res1, 1);
SAMPLER2D(res2, 2);
SAMPLER2D(res3, 3);

#define RESTIR_SPATIAL_SAMPLES 5
#define RESTIR_SPATIAL_RADIUS 24.0   // in full-res pixels; divided by the stride below

Reservoir restirLoad(ivec2 at) {
    return restirUnpack(texelFetch(res0, at, 0), texelFetch(res1, at, 0), texelFetch(res2, at, 0), texelFetch(res3, at, 0));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    Reservoir self = restirLoad(p);
    if (self.t1 < 0.0) { gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0); return; }
    vec3 x1 = self.x1;
    vec3 n1 = self.n1;

    Reservoir r = self;
    bool borrowed = false;
    if (restirReuse.y > 0.5) {
        uint seed = uint(p.x) * 73856093u + uint(p.y) * 19349663u + uint(frameCount.x) * 83492791u;
        restirPcg(seed);
        r = restirEmpty();
        restirUpdate(r, self, restirTarget(self, x1, n1) * self.W * self.M, restirRandom(seed));
        float radius = RESTIR_SPATIAL_RADIUS / restirJitter.z;
        ivec2 size = ivec2(passTargetRes.xy);
        for (int k = 0; k < RESTIR_SPATIAL_SAMPLES; k++) {
            float a = 2.0 * RESTIR_PI * restirRandom(seed);
            float d = radius * sqrt(restirRandom(seed));
            ivec2 at = clamp(p + ivec2(round(vec2(cos(a), sin(a)) * d)), ivec2(0, 0), size - ivec2(1, 1));
            if (at.x == p.x && at.y == p.y) continue;
            Reservoir s = restirLoad(at);
            if (s.t1 < 0.0 || s.M <= 0.0 || dot(s.n1, n1) < 0.9 || abs(s.t1 - self.t1) > 0.1 * self.t1) continue;
            float jacobian = restirJacobian(s, x1);
            float target = restirTarget(s, x1, n1) * jacobian;
            // restirUpdate, open-coded to know whether the neighbour's sample won
            float w = target * s.W * s.M;
            r.wSum += w;
            r.M += s.M;
            if (w > 0.0 && restirRandom(seed) * r.wSum < w) {
                r.x2 = s.x2;
                r.n2 = s.n2;
                r.Lo = s.Lo;
                borrowed = true;
            }
        }
        float target = restirTarget(r, x1, n1);
        r.W = target > 0.0 ? r.wSum / (r.M * target) : 0.0;
    }

    vec3 toSample = r.x2 - x1;
    float distance = length(toSample);
    vec3 direction = toSample / max(distance, 1e-6);
    if (borrowed && restirReuse.z > 0.5 && r.W > 0.0) {
        RestirHit hit = restirTrace(x1 + n1 * restirSettings.y, direction, RESTIR_KIND_SHADOW, max(distance - 2.0 * restirSettings.y - 0.05, 0.0));
        // A borrowed sample this point cannot see: fall back to the pixel's own reservoir, which it can.
        // (Zeroing the weight instead -- the textbook biased variant -- darkened the image by 1.5-3 levels
        // in 255, most where small lights sit behind corners, as in the tunnel.)
        if (hit.hit) {
            r = self;
            toSample = r.x2 - x1;
            distance = length(toSample);
            direction = toSample / max(distance, 1e-6);
        }
    }
    vec3 light = r.Lo * max(0.0, dot(n1, direction)) * r.W / RESTIR_PI;
    gl_FragColor = vec4(light, 1.0);
}
