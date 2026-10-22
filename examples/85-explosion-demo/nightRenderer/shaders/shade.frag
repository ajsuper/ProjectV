$input v_color0
$input v_texcoord0

// =============================================================================
// shade.frag -- pass 3, at render resolution. Lights the frame.
//
//   colour = albedo * ( sun * N.L * shadow  +  ambient(N) * AO  +  bounce * strength )  +  emission,
//   then fog.
//
// The AO and the bounce are filtered 5x5 with weights [1 2 2 2 1] on each axis, over taps on the same
// surface (depth and normal both close), so a crate's AO does not bleed onto the wall behind it. Those
// weights give each of ssao.frag's 16 rotations (a 4x4 pattern) exactly equal weight, so its pattern
// cancels wherever the window lies on one surface, and the window's soft edge hides its own shape. The hard shadow is averaged over a small same-surface ring for the same reason: magnified,
// a binary shadow would stair-step in the signal itself, where no reconstruction can reach it.
//
// With the ray GI on (giParams.y), the ambient is the traced, accumulated, blurred light of raygi.frag
// instead -- what the sky and the world round a point actually send it -- times the contact AO only: the
// wide AO's job is done better by the rays. The screen-space bounce stays, halved, for the near detail
// the quarter-resolution rays blur away.
//
// Inputs: FBO 1 (G-buffer, slots 0..5), FBO 2 (AO slot 6, bounce slot 7), FBO 10 (ray GI, slot 8).
// Output (FBO 3): linear HDR.
// =============================================================================

#include <bgfx_shader.sh>
#include <grr_common.sc>

SAMPLER2D(gPos,    0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gAlbedo, 2);
SAMPLER2D(gLight,  3);
SAMPLER2D(aoRaw,   6);
SAMPLER2D(giRaw,   7);
SAMPLER2D(rayGI,   8);

uniform vec4 passTargetRes;
uniform vec4 renderParams;   // x = shadows, y = AO, w = debug view (2 = AO only, 4 = bounce only)
uniform vec4 giParams;       // x = screen-space bounce strength (0: off), y = ray GI on

#define SHADOW_RING_TAPS   8
#define SHADOW_RING_PIXELS 1.25
#define DEPTH_TOLERANCE    0.04
// How much the wide scale darkens. Below 1 so an enclosed space is dim rather than black -- the light
// that real bounce would carry in is what this lacks, and leaving some of the ambient is its stand-in.
#define WIDE_AO_STRENGTH   0.85
// A small sheen at grazing angles, the one view-dependent term: without anything that changes as you
// move, a lit scene reads as painted cardboard.
#define SHEEN_STRENGTH     0.05

// The ray GI at this pixel, from its quarter-resolution target: the four nearest texels, bilinear, each
// kept only if the render pixel it was traced from is on this surface.
vec3 rayGIAt(vec2 uv, vec3 P, vec3 N, float depth) {
    vec2 res = passTargetRes.xy * 0.5;
    vec2 f = uv * res - 0.5;
    vec2 base = floor(f), frac = f - base;
    vec3 sum = vec3(0.0, 0.0, 0.0), nearest = vec3(0.0, 0.0, 0.0);
    float weights = 0.0, best = -1.0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec2 q = clamp(base + vec2(float(i), float(j)), vec2(0.0, 0.0), res - 1.0);
            vec4 source = texture2D(gPos, (q + 0.25) / res);
            vec3 ns = texture2D(gNormal, (q + 0.25) / res).xyz;
            float bilinear = (i == 0 ? 1.0 - frac.x : frac.x) * (j == 0 ? 1.0 - frac.y : frac.y);
            float same = source.w < 0.0 ? 0.0 : pow(clamp(dot(ns, N), 0.0, 1.0), 8.0) *
                         (1.0 - smoothstep(0.02, 0.1 + depth * 0.01, abs(dot(source.xyz - P, N))));
            vec3 light = texture2D(rayGI, (q + 0.5) / res).rgb;
            sum += light * bilinear * same;
            weights += bilinear * same;
            if (same > best) { best = same; nearest = light; }
        }
    return weights > 1e-3 ? sum / weights : nearest;
}

bool sameSurface(float depth, vec3 normal, vec2 uv) {
    float d = texture2D(gPos, uv).w;
    if (d < 0.0 || abs(d - depth) > depth * DEPTH_TOLERANCE + lightParams.w) return false;
    return dot(texture2D(gNormal, uv).xyz, normal) > 0.8;
}

void main() {
    vec4 pos = texture2D(gPos, v_texcoord0);
    vec4 albedoE = texture2D(gAlbedo, v_texcoord0);
    vec3 view = normalize(pos.xyz - cameraPos.xyz);
    if (pos.w < 0.0) {
        // Sky: its colour is in gAlbedo. Fogged at a long distance so the horizon and the far
        // geometry in front of it meet at the same colour.
        vec3 dir = grrCameraRay(v_texcoord0, passTargetRes.xy, cameraDir.xyz);
        gl_FragColor = vec4(grrApplyFog(albedoE.rgb, cameraPos.xyz, dir, 1.0e5), 1.0);
        return;
    }
    float depth = pos.w;
    vec3 normal = normalize(texture2D(gNormal, v_texcoord0).xyz);
    vec2 light = texture2D(gLight, v_texcoord0).xy;
    vec2 texel = passTargetRes.zw;

    // AO and bounce: 5x5, [1 2 2 2 1] on each axis, centred on the pixel, same surface only.
    vec2 centre = (floor(v_texcoord0 * passTargetRes.xy) + 0.5) * texel;
    vec2 ao = vec2(0.0, 0.0);
    vec3 bounce = vec3(0.0, 0.0, 0.0);
    float aoWeight = 0.0;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++) {
            vec2 uv = centre + vec2(float(i), float(j)) * texel;
            if (!sameSurface(depth, normal, uv)) continue;
            float w = (abs(i) == 2 ? 1.0 : 2.0) * (abs(j) == 2 ? 1.0 : 2.0);
            ao += texture2D(aoRaw, uv).rg * w;
            bounce += texture2D(giRaw, uv).rgb * w;
            aoWeight += w;
        }
    if (aoWeight > 0.0) {
        ao /= aoWeight;
        bounce /= aoWeight;
    } else {
        ao = texture2D(aoRaw, v_texcoord0).rg;
        bounce = texture2D(giRaw, v_texcoord0).rgb;
    }
    // (ssao.frag sets each scale's response; no squaring here.)
    bool rayTraced = giParams.y > 0.5;
    float occlusion = rayTraced ? ao.x : ao.x * mix(1.0, ao.y, WIDE_AO_STRENGTH);

    // Sun visibility: a ring, divided by the whole ring, so a tap on another surface counts as unlit and
    // a lone voxel is not brighter than a wall in the same light.
    vec3 lightSample = texture2D(gLight, v_texcoord0).xyz;
    float visibility = lightSample.x, flashVisibility = lightSample.z;
    for (int k = 0; k < SHADOW_RING_TAPS; k++) {
        float a = (float(k) + 0.5) * (6.2831853 / float(SHADOW_RING_TAPS));
        vec2 uv = v_texcoord0 + vec2(cos(a), sin(a)) * SHADOW_RING_PIXELS * texel;
        uv = (floor(uv * passTargetRes.xy) + 0.5) * texel;   // a G-buffer value, not a blend of two
        if (sameSurface(depth, normal, uv)) {
            vec3 l = texture2D(gLight, uv).xyz;
            visibility += l.x;
            flashVisibility += l.z;
        }
    }
    visibility /= float(SHADOW_RING_TAPS + 1);
    flashVisibility /= float(SHADOW_RING_TAPS + 1);

    vec3 albedo = albedoE.rgb;
    vec3 sun = grrSunColor();
    vec3 direct = sun * (max(light.y, 0.0) * visibility / GRR_PI)
                + grrFlashLight(pos.xyz, normal) * (flashVisibility / GRR_PI);
    vec3 traced = rayTraced ? rayGIAt(v_texcoord0, pos.xyz, normal, depth) : vec3(0.0, 0.0, 0.0);
    vec3 ambient = (rayTraced ? traced : grrAmbient(normal)) * occlusion;

    // Fresnel-gated sun sheen: nearly nothing head-on, a rim at grazing angles, only where the sun is.
    vec3 H = normalize(sunDir.xyz - view);
    float fresnel = 0.04 + 0.96 * pow(1.0 - clamp(dot(normal, -view), 0.0, 1.0), 5.0);
    vec3 sheen = sun * (SHEEN_STRENGTH * fresnel * pow(max(dot(normal, H), 0.0), 48.0) *
                        max(light.y, 0.0) * visibility);

    // The bounce, held back a little by the contact AO (a crease gets less of it than open ground).
    vec3 indirect = bounce * (giParams.x * mix(1.0, ao.x, 0.5) * (rayTraced ? 0.5 : 1.0));
    vec3 colour = albedo * (direct + ambient + indirect) + sheen + albedo * albedoE.a;

    if (renderParams.w > 6.5) {                                    // debug: the ray GI alone, on white
        gl_FragColor = vec4(traced, 1.0);
        return;
    }
    if (renderParams.w > 4.5) {                                    // debug: raw contact (5) / wide (6) AO, unblurred
        float raw = renderParams.w < 5.5 ? texture2D(aoRaw, centre).r : texture2D(aoRaw, centre).g;
        gl_FragColor = vec4(raw, raw, raw, 1.0);
        return;
    }
    if (renderParams.w > 3.5 && renderParams.w < 4.5) {           // debug: the bounce alone
        gl_FragColor = vec4(indirect * 4.0, 1.0);   // brightened: on its own it is a fraction of the frame
        return;
    }

    if (renderParams.w > 2.5 && renderParams.w < 3.5) {           // debug: raw sun visibility
        gl_FragColor = vec4(light.x, light.x, light.x, 1.0);
        return;
    }
    if (renderParams.w > 1.5 && renderParams.w < 2.5) {           // debug: the AO alone
        gl_FragColor = vec4(vec3(occlusion, occlusion, occlusion), 1.0);
        return;
    }
    gl_FragColor = vec4(grrApplyFog(colour, cameraPos.xyz, view, depth), 1.0);
}
