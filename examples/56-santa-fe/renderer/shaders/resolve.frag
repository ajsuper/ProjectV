$input v_color0
$input v_texcoord0

// =============================================================================
// resolve.frag -- the Santa Fe renderer's temporal resolve (santa-fe-reference S6), at window resolution,
// after restir-pt's.
//
//   still     the history at the same pixel, a running mean up to sfTemporal.y frames: a parked view
//             converges -- the jitter's sixteen points (the upscale) and every stochastic choice the trace
//             makes (clouds, soft shadows, the diffuse ray)
//   moving    the history where the pixel was last frame (the nearest surface's motion in the 3x3 render
//             texels round it, bilinear), clamped into this frame's 3x3 neighbourhood, at most sfTemporal.z
//             frames: no smear, and not the noise of a single frame
//   reset     sfTemporal.x: a setting changed -- the history is not this image any more
//
// Inputs: 0 reconstructed; 1-7 the trace pass (colour 1, gPos 2, gNormal 3, gFace 4, gRot 5, gMotion 6, gSun 7);
// 8 resolved (last frame, this pass's own output). Output: rgb the colour, a the history length.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>

SAMPLER2D(reconstructed, 0);
SAMPLER2D(gPos, 2);
SAMPLER2D(gMotion, 6);
SAMPLER2D(resolved, 8);   // after the trace pass's seven targets

uniform vec4 frameCount;   // y: the camera moved (or the scene changed) this frame
uniform vec4 sfTemporal;   // x: reset, y: frames kept when still, z: frames kept when moving

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec3 current = texelFetch(reconstructed, at, 0).rgb;
    if (sfTemporal.x > 0.5) {
        gl_FragColor = vec4(current, 1.0);
        return;
    }
    vec2 res = passTargetRes.xy;
    ivec2 last = ivec2(res) - ivec2(1, 1);
    vec2 centre = (vec2(at) + 0.5) / res;
    vec2 renderRes = vec2(textureSize(gMotion, 0));
    ivec2 texel = min(ivec2(centre * renderRes), ivec2(renderRes) - ivec2(1, 1));
    vec2 motion = texelFetch(gMotion, texel, 0).xy;
    float nearest = 1e30;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(texel + ivec2(x, y), ivec2(0, 0), ivec2(renderRes) - ivec2(1, 1));
            float d = texelFetch(gPos, q, 0).w;
            if (d >= 0.0 && d < nearest) {
                nearest = d;
                motion = texelFetch(gMotion, q, 0).xy;
            }
        }
    bool moved = frameCount.y > 0.5;
    vec3 historyColour = vec3(0.0, 0.0, 0.0);
    float historyLength = 0.0;
    if (!moved) {
        vec4 h = texelFetch(resolved, at, 0);
        historyColour = h.rgb;
        historyLength = h.a;
    } else {
        vec2 pix = (centre + motion) * res - 0.5;
        float weight = 0.0;
        ivec2 base = ivec2(floor(pix));
        vec2 f = pix - floor(pix);
        for (int k = 0; k < 4; k++) {
            ivec2 o = ivec2(k & 1, k >> 1);
            ivec2 q = base + o;
            if (q.x < 0 || q.y < 0 || q.x > last.x || q.y > last.y) continue;
            float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
            vec4 h = texelFetch(resolved, q, 0);
            historyColour += h.rgb * w;
            historyLength += h.a * w;
            weight += w;
        }
        if (weight > 1e-3) {
            historyColour /= weight;
            historyLength = min(historyLength / weight, sfTemporal.z);
            vec3 lo = current;
            vec3 hi = current;
            for (int y = -1; y <= 1; y++)
                for (int x = -1; x <= 1; x++) {
                    vec3 c = texelFetch(reconstructed, clamp(at + ivec2(x, y), ivec2(0, 0), last), 0).rgb;
                    lo = min(lo, c);
                    hi = max(hi, c);
                }
            historyColour = clamp(historyColour, lo, hi);
        } else {
            historyLength = 0.0;
        }
    }
    float newLength = min(historyLength + 1.0, moved ? sfTemporal.z : sfTemporal.y);
    vec3 colour = historyLength > 0.0 ? mix(historyColour, current, 1.0 / newLength) : current;
    gl_FragColor = vec4(colour, newLength);
}
