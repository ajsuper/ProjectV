$input v_color0
$input v_texcoord0

// =============================================================================
// resolve.frag -- fast's pass 5, window resolution: temporal accumulation (TAA).
//
// restir-pt's resolve.frag, copied (docs/plans/fast-renderer.md, D3) without its water and mirror
// histories, which fast has no material for. The G-buffer's camera rays jitter over 16 phases inside each
// render texel, and this averages them into a supersampled frame:
//
// - Still: the history at the same pixel, up to fastTemporal.x frames (64), blended with this frame.
// - Moving: the history reprojected by the G-buffer's motion vectors (the camera's: fast does not track
//   moving chunks) and clamped to this frame's 3x3 neighbourhood, so a surface the camera uncovers takes
//   no trail from what stood in front of it. At most 16 frames while moving.
//
// The motion read is the nearest surface's in the 3x3 render texels round the pixel: at an object's edge
// the pixel follows the object, not the background behind it.
//
// Inputs: 0 reconstructed; 1-7 the G-buffer (gPos 1, gMotion 7); 8 resolved (last frame, this pass's own
// output). Output: rgb the colour, a its history length in frames.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>   // passTargetRes

SAMPLER2D(reconstructed, 0);
SAMPLER2D(gPos, 1);
SAMPLER2D(gMotion, 7);
SAMPLER2D(resolved, 8);

uniform vec4 frameCount;     // y: the camera (or the sun, or the renderer) changed this frame
uniform vec4 fastTemporal;   // x: the history's length when still, in frames (1: TAA off); y: first frame

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec3 current = texelFetch(reconstructed, at, 0).rgb;
    if (fastTemporal.y > 0.5 || fastTemporal.x <= 1.0) {   // the first frame, or no TAA: no history
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
    bool moved = frameCount.y > 0.5 || length(motion * res) > 0.01;

    vec3 historyColour = vec3(0.0, 0.0, 0.0);
    float historyLength = 0.0;
    if (!moved) {
        vec4 h = texelFetch(resolved, at, 0);
        historyColour = h.rgb;
        historyLength = h.a;
    } else {
        vec2 pix = (centre + motion) * res - 0.5;   // in texel-centre space
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
            historyLength = min(historyLength / weight, 16.0);
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

    float newLength = min(historyLength + 1.0, moved ? min(fastTemporal.x, 16.0) : fastTemporal.x);
    vec3 colour = historyLength > 0.0 ? mix(historyColour, current, 1.0 / newLength) : current;
    gl_FragColor = vec4(colour, newLength);
}
