$input v_color0
$input v_texcoord0

// =============================================================================
// ao_trace.frag -- the traced AO at half the trace's resolution (santa-fe-reference S18).
//
// SF_AO_RAYS short cosine-weighted rays a texel from the trace's hit (gPos, gNormal), voxels and props only,
// capped in reach (sfAO.x) and steps (sfAO.z), out to sfAO.w metres from the camera. A quarter of the trace's
// texels. The rays are fixed to the ground (S22d: nothing random per frame, nothing kept between frames);
// ao_atrous.sc filters them, and resolve.frag holds the result per voxel face and multiplies in a
// screen-space AO per pixel.
//
// Inputs: the trace pass (gPos 1, gNormal 2). Output: r the AO (1 open), g the depth it is for.
// =============================================================================

#include <bgfx_shader.sh>

#define PJV_SCENE_GRIDS_FIRST
#include <pjv_scene.sc>
#include <pjv_camera.sc>

uniform vec4 sfAO;           // x reach (m), y strength, z steps, w out to (m)
uniform vec4 sfFlags;        // y bit 7: SF2_AO; x 32768: props (SF_PROPS)

SAMPLER2D(gPos, 1);
SAMPLER2D(gNormal, 2);

#define PI 3.14159265358979
#define SF_AO_RAYS 2   // rays a texel
// The reach grows with distance (santa-fe-reference S22, distant AO): sfAO.x metres near, this share of the distance
// far -- a 6 m ray is under a pixel a kilometre out, and a distant slope or crown would get no occlusion at all.
#define SF_AO_REACH_PER_M 0.012

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

    // ---- Two AO rays, fixed: their directions come from where this texel's ground is, quantized to about its
    // own size (a power of two, so the cells hold still), never from the frame. The same ground asks the same
    // rays every frame; ao_atrous.sc filters the pattern away. ----
    float texelWorld = max(p.w, 1e-3) * 2.0 * pjvViewTanHalfFov() / float(gsize.y) * 2.0;
    float cellSize = exp2(ceil(log2(max(texelWorld, 1e-3))));
    ivec3 cell = ivec3(floor((p.xyz + nf * (cellSize * 0.5)) / cellSize));
    uint h = aoHash(uint(cell.x) * 1973u ^ aoHash(uint(cell.y) * 9277u ^ aoHash(uint(cell.z) * 26699u)));
    float reach = max(sfAO.x, p.w * SF_AO_REACH_PER_M);
    float ao = 0.0;
    for (int k = 0; k < SF_AO_RAYS; k++) {
        uint hk = aoHash(h + uint(k) * 0x9e3779b9u);
        // Stratified: ray k takes its share of the cosine lobe's radius, and the turn opposite the last.
        float u1 = (float(k) + float(hk & 0xffffu) / 65536.0) / float(SF_AO_RAYS);
        float u2 = fract(float(hk >> 16u) / 65536.0 + float(k) / float(SF_AO_RAYS));
        vec3 d = aoCosineDir(nf, u1, u2);
        PjvRay ray;
        ray.origin = p.xyz + nf * max(0.01, p.w * 0.0004);
        ray.direction = d;
        PjvRayQuery rq = pjvPrimaryQuery(100u);
        rq.maxRaySteps = uint(sfAO.z);
        rq.startLOD = 0u;
        rq.finishLOD = 2u;
        rq.distanceToFinishLOD = 24u;
        rq.maxDistance = reach;
        pjvQueryLooseReach(rq, (uint(sfFlags.x + 0.5) & 32768u) != 0u ? min(reach, sfAO.x * 2.0) : 0.0);
        PjvSceneIntersectData found = pjvRaySceneIntersect(ray, rq).hit;
        bool hit = !(found.foundBox.size < 0.0 || found.rayT <= 0.0 || dot(found.normal, found.normal) < 0.5) && found.rayT < reach;
        ao += hit ? mix(1.0, pow(clamp(found.rayT / max(reach, 1e-3), 0.0, 1.0), 0.6), sfAO.y) : 1.0;
    }
    // Faded out over the last fifth of the range, not cut.
    ao = mix(ao / float(SF_AO_RAYS), 1.0, smoothstep(0.8 * sfAO.w, sfAO.w, p.w));
    gl_FragColor = vec4(ao, p.w, 0.0, 1.0);
}
