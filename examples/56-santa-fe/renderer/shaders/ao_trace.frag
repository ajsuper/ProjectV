$input v_color0
$input v_texcoord0

// =============================================================================
// ao_trace.frag -- the traced AO at half the trace's resolution (santa-fe-reference S18).
//
// One short cosine-weighted ray a texel from the trace's hit (gPos, gNormal), voxels and props only, capped
// in reach (sfAO.x) and steps (sfAO.z), out to sfAO.w metres from the camera. A quarter of the trace's
// texels, so a quarter of the rays it cost inside trace.frag; and out of the trace, its march no longer
// widens the trace's registers. Its answers accumulate here over frames (reprojected by the trace's motion,
// dropped where the depth disagrees); shadow_soft upsamples them along surfaces and multiplies in a
// screen-space AO for the detail under a texel of this.
//
// Inputs: the trace pass (gPos 1, gNormal 2, gMotion 5), this pass's last output (7).
// Output: r the AO (1 open), g the depth it is for, b how many frames it holds.
// =============================================================================

#include <bgfx_shader.sh>

#define PJV_SCENE_GRIDS_FIRST
#include <pjv_scene.sc>
#include <pjv_camera.sc>

uniform vec4 frameCount;     // x: frame index
uniform vec4 sfAO;           // x reach (m), y strength, z steps, w out to (m)
uniform vec4 sfTemporal;     // x: reset
uniform vec4 sfFlags;        // y bit 7: SF2_AO; x 32768: props (SF_PROPS)

SAMPLER2D(gPos, 1);
SAMPLER2D(gNormal, 2);
SAMPLER2D(gMotion, 5);
SAMPLER2D(aoHist, 7);

#define PI 3.14159265358979

uint aoHash(uint x) {
    x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu; x ^= x >> 16u;
    return x;
}

vec3 aoCosineDir(vec3 n, float u1, float u2) {
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    vec3 t = abs(n.y) < 0.99 ? normalize(cross(n, vec3(0.0, 1.0, 0.0))) : normalize(cross(n, vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

void main() {
    vec2 uv = v_texcoord0;
    ivec2 gsize = ivec2(textureSize(gPos, 0));
    ivec2 g = clamp(ivec2(uv * vec2(gsize)), ivec2(0, 0), gsize - 1);
    vec4 p = texelFetch(gPos, g, 0);
    vec3 nf = texelFetch(gNormal, g, 0).xyz;
    uint flagsY = uint(sfFlags.y + 0.5);
    if ((flagsY & 128u) == 0u || p.w < 0.0 || p.w > sfAO.w || dot(nf, nf) < 0.5) {
        gl_FragColor = vec4(1.0, p.w, 0.0, 1.0);
        return;
    }

    // ---- One AO ray ----
    ivec2 pix = ivec2(gl_FragCoord.xy);
    uint h = aoHash(uint(pix.x) * 1973u + uint(pix.y) * 9277u + uint(frameCount.x) * 26699u);
    float u1 = float(h & 0xffffu) / 65536.0, u2 = float(h >> 16u) / 65536.0;
    vec3 d = aoCosineDir(nf, u1, u2);
    PjvRay ray;
    ray.origin = p.xyz + nf * max(0.01, p.w * 0.0004);
    ray.direction = d;
    PjvRayQuery rq = pjvPrimaryQuery(100u);
    rq.maxRaySteps = uint(sfAO.z);
    rq.startLOD = 0u;
    rq.finishLOD = 2u;
    rq.distanceToFinishLOD = 24u;
    rq.maxDistance = sfAO.x;
    pjvQueryLooseReach(rq, (uint(sfFlags.x + 0.5) & 32768u) != 0u ? sfAO.x : 0.0);
    PjvSceneIntersectData found = pjvRaySceneIntersect(ray, rq).hit;
    bool hit = !(found.foundBox.size < 0.0 || found.rayT <= 0.0 || dot(found.normal, found.normal) < 0.5) && found.rayT < sfAO.x;
    float ao = hit ? mix(1.0, pow(clamp(found.rayT / max(sfAO.x, 1e-3), 0.0, 1.0), 0.6), sfAO.y) : 1.0;

    // ---- Over frames: where this ground was last frame, if the depth agrees ----
    vec2 then = uv + texelFetch(gMotion, g, 0).xy;
    float n = 0.0;
    float acc = ao;
    if (sfTemporal.x < 0.5 && all(greaterThan(then, vec2(0.0, 0.0))) && all(lessThan(then, vec2(1.0, 1.0)))) {
        ivec2 hsize = ivec2(textureSize(aoHist, 0));
        vec4 hist = texelFetch(aoHist, clamp(ivec2(then * vec2(hsize)), ivec2(0, 0), hsize - 1), 0);
        if (hist.b > 0.0 && abs(hist.g - p.w) < max(0.04 * p.w, 0.1)) {
            n = min(hist.b, 15.0);
            acc = (hist.r * n + ao) / (n + 1.0);
        }
    }
    gl_FragColor = vec4(acc, p.w, n + 1.0, 1.0);
}
