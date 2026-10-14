$input v_color0
$input v_texcoord0

// =============================================================================
// ssao.frag -- pass 2, at render resolution. Screen-space ambient occlusion: no scene rays, only the
// G-buffer the previous pass wrote (the same method as 30-renderers' fast renderer).
//
// For each of AO_SAMPLES points in the hemisphere over the surface, project it to the screen and ask
// the G-buffer whether something nearer the camera is there: if so, that direction is blocked. The
// sample directions are turned per pixel by a 4x4 ordered pattern, which shade.frag's 4x4 blur then
// averages away exactly -- every rotation once -- so the result is smooth without being temporal.
//
// Output (FBO 2): r = ambient visibility, 1 open, 0 shut in.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos,    0);
SAMPLER2D(gNormal, 1);

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
// y: AO on (1) or off (0).
uniform vec4 renderParams;
// Engine-set: this pass's target, (w, h, 1/w, 1/h).
uniform vec4 passTargetRes;

#define FOV 60.0
#define AO_SAMPLES 12
// World metres: the scale of what counts as a crease. A crate is a metre across.
#define AO_RADIUS 1.0
#define AO_BIAS 0.03
#define AO_STRENGTH 1.0

// The inverse of rayStartDirection: where on screen a world point is.
vec2 worldToUV(vec3 P, out bool valid) {
    vec3 forward = normalize(cameraDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));
    vec3 v = P - cameraPos.xyz;
    float z = dot(v, forward);
    if (z <= 1e-4) { valid = false; return vec2(-1.0); }
    float scale  = tan(radians(FOV * 0.5));
    float aspect = windowRes.x / windowRes.y;
    vec2 ndc = vec2((dot(v, right) / z) / (scale * aspect), (dot(v, up) / z) / scale);
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    valid = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    return uv;
}

// 4x4 Bayer order, 0..15: which of sixteen rotations this pixel takes.
float bayer4(vec2 pixel) {
    ivec2 p = ivec2(mod(pixel, 4.0));
    // The 2x2 matrix [0 2; 3 1] is 2 (x ^ y) + y; the 4x4 nests it in itself.
    int fine   = 2 * ((p.x ^ p.y) & 1) + (p.y & 1);
    int coarse = 2 * (((p.x ^ p.y) >> 1) & 1) + ((p.y >> 1) & 1);
    return float(4 * fine + coarse);
}

vec3 hemisphereDir(float u, float v, vec3 n) {
    float r = sqrt(u);
    float phi = 6.2831853 * v;
    vec3 t = abs(n.y) > 0.999 ? vec3(1.0, 0.0, 0.0) : normalize(cross(vec3(0.0, 1.0, 0.0), n));
    vec3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u)));
}

void main() {
    vec4 pos = texture2D(gPos, v_texcoord0);
    if (pos.w < 0.0 || renderParams.y < 0.5) { gl_FragColor = vec4(1.0); return; }
    vec3 P = pos.xyz;
    vec3 N = normalize(texture2D(gNormal, v_texcoord0).xyz);

    float turn = (bayer4(floor(v_texcoord0 * passTargetRes.xy)) + 0.5) / 16.0;
    float occlusion = 0.0;
    for (int i = 0; i < AO_SAMPLES; i++) {
        // An even spread (golden-ratio angles, stratified radii), turned per pixel.
        float u = (float(i) + 0.5) / float(AO_SAMPLES);
        float v = fract(float(i) * 0.618034 + turn);
        vec3 dir = hemisphereDir(u, v, N);
        float reach = AO_RADIUS * (0.2 + 0.8 * fract(float(i) * 0.7548777 + turn));
        vec3 probe = P + N * AO_BIAS + dir * reach;

        bool valid;
        vec2 uv = worldToUV(probe, valid);
        if (!valid) continue;
        vec4 there = texture2D(gPos, uv);
        if (there.w < 0.0) continue;
        float behind = length(probe - cameraPos.xyz) - there.w;
        // Blocked when the surface there is in front of the sample -- but only by something near:
        // a wall far in front is not shading this point.
        if (behind > AO_BIAS) occlusion += smoothstep(0.0, 1.0, AO_RADIUS / max(behind, 1e-4));
    }
    float ao = 1.0 - AO_STRENGTH * occlusion / float(AO_SAMPLES);
    gl_FragColor = vec4(clamp(ao, 0.0, 1.0), 0.0, 0.0, 1.0);
}
