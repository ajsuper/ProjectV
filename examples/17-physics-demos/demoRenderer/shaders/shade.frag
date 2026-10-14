$input v_color0
$input v_texcoord0

// =============================================================================
// shade.frag -- pass 3, at render resolution. Lights the frame: the AO blurred, the hard shadow
// softened, the sandbox's lighting on top, and the fog.
//
// The AO is averaged over a 4x4 window around the pixel -- exactly one period of ssao.frag's rotation
// pattern, so its noise cancels -- taking only taps on the same surface (a comparable depth and
// normal), so a crate's AO does not bleed onto the wall behind it.
//
// The shadow ray was a hard yes/no. Magnified, that would stair-step, and the aliasing would be in
// the signal where no reconstruction can reach it; so its visibility is averaged over a small ring
// first (as 30-renderers' reconstruction renderer does), depth-gated the same way.
//
// Inputs: FBO 1 (the G-buffer, slots 0..5), FBO 2 (the raw AO, slot 6).
// Output (FBO 3): the lit frame, linear.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos,    0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gAlbedo, 2);
SAMPLER2D(gLight,  3);
SAMPLER2D(aoRaw,   6);

uniform vec4 cameraPos;
uniform vec4 passTargetRes;

#define SHADOW_RING_TAPS   8
#define SHADOW_RING_PIXELS 1.5
#define DEPTH_TOLERANCE    0.05

vec3 sky(vec3 direction) {
    float height = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.10, 0.11, 0.15), vec3(0.42, 0.55, 0.72), pow(height, 1.5));
}

bool sameSurface(float depth, vec3 normal, vec2 uv) {
    float d = texture2D(gPos, uv).w;
    if (d < 0.0 || abs(d - depth) > depth * DEPTH_TOLERANCE + 0.02) return false;
    return dot(texture2D(gNormal, uv).xyz, normal) > 0.8;
}

void main() {
    vec4 pos = texture2D(gPos, v_texcoord0);
    vec3 albedo = texture2D(gAlbedo, v_texcoord0).rgb;
    if (pos.w < 0.0) { gl_FragColor = vec4(albedo, 1.0); return; }   // sky: its colour is in albedo
    float depth = pos.w;
    vec3 normal = texture2D(gNormal, v_texcoord0).xyz;
    vec2 light = texture2D(gLight, v_texcoord0).xy;
    vec2 texel = passTargetRes.zw;

    // AO: a 4x4 window around the pixel, same surface only. Any 4x4 window holds each of the
    // pattern's sixteen turns once; sliding it with the pixel (rather than taking the fixed block the
    // pixel is in) keeps the result smooth instead of constant per block.
    float ao = 0.0, aoWeight = 0.0;
    for (int j = -2; j < 2; j++)
        for (int i = -2; i < 2; i++) {
            vec2 uv = v_texcoord0 + vec2(float(i), float(j)) * texel;
            if (!sameSurface(depth, normal, uv)) continue;
            ao += texture2D(aoRaw, uv).r;
            aoWeight += 1.0;
        }
    ao = aoWeight > 0.0 ? ao / aoWeight : texture2D(aoRaw, v_texcoord0).r;

    // Sun visibility: a ring, divided by the whole ring (a tap on another surface counts as unlit,
    // so a lone voxel is not brighter than a wall in the same light).
    float visibility = light.x;
    for (int k = 0; k < SHADOW_RING_TAPS; k++) {
        float a = (float(k) + 0.5) * (6.2831853 / float(SHADOW_RING_TAPS));
        vec2 uv = v_texcoord0 + vec2(cos(a), sin(a)) * SHADOW_RING_PIXELS * texel;
        if (sameSurface(depth, normal, uv)) visibility += texture2D(gLight, uv).x;
    }
    visibility /= float(SHADOW_RING_TAPS + 1);

    // The sandbox's light, split so the shadow takes only the sun's share and the AO only the sky's:
    // a wrapped diffuse (its 0.4 floor is light from everywhere, so AO dims it) and a little sky from
    // above.
    float facing = light.y;
    float wrapped = clamp(0.4 + 0.6 * min(facing, 0.0), 0.0, 0.4) * ao;
    float direct = 0.6 * max(facing, 0.0) * visibility;
    float skyLight = (0.25 + 0.15 * normal.y) * ao;
    vec3 colour = albedo * ((wrapped + direct) * 0.85 + skyLight);

    vec3 view = normalize(pos.xyz - cameraPos.xyz);
    float fog = 1.0 - exp(-max(depth - 40.0, 0.0) * 0.008);
    gl_FragColor = vec4(mix(colour, sky(view), fog), 1.0);
}
