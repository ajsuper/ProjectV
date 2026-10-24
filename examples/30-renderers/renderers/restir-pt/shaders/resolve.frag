$input v_color0
$input v_texcoord0

// resolve.frag -- pass 14, window resolution: temporal accumulation of the reconstructed image. The
// G-buffer traces each render texel through the centre of a different one of the window pixels it covers
// every frame (2x2 of them, in turn), and the reconstruction rebuilds the window from each; this averages
// those four, and what noise the denoiser left.
//
//   * a pixel that did not move (its motion vector under a hundredth of a pixel, the camera still): its own
//     history is always kept. A still view repeats the jitter's sixteen points, so it settles to their
//     average and stays. (Rejecting history whenever the hit changed surface threw it away every frame
//     wherever voxels are smaller than a pixel: the distant shimmer.)
//   * a pixel that moved -- the camera, or the object under it (gMotion: its chunk's own motion and the
//     camera's) -- reads its history where it was last frame, bilinearly, clamped into the colour range of
//     this frame's 3x3 neighbourhood (TAA's neighbourhood clamp: drops what was uncovered or changed), and
//     held to 16 frames so changing light and moving things are not dragged along. (Before motion vectors a
//     still camera kept every pixel's history, and anything that moved smeared.)
//   * a still pixel that showed something moving last frame (the history's length is stored negative for
//     a pixel that moved) is what a moving thing just uncovered: its history is the thing, so it is clamped
//     too, and cut to 4 frames. Without this a body crossing a still view left a trail behind it.

// Inputs: 0 reconstructed; 1-7 the G-buffer (gPos 1, gMotion 7); 8 resolved (last frame, this pass's own output).
// Output: rgb the colour, a its history length in frames (negative: something moved under it this frame,
// the camera still).

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(reconstructed, 0);
SAMPLER2D(gPos, 1);
SAMPLER2D(gMotion, 7);
SAMPLER2D(resolved, 8);

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec3 current = texelFetch(reconstructed, at, 0).rgb;
    if (restirJitter.w > 0.5) {   // the first frame: no history yet
        gl_FragColor = vec4(current, 1.0);
        return;
    }
    vec2 res = passTargetRes.xy;
    ivec2 last = ivec2(res) - ivec2(1, 1);

    vec3 historyColour = vec3(0.0, 0.0, 0.0);
    float historyLength = 0.0;
    vec2 centre = (vec2(at) + 0.5) / res;
    // The motion of the nearest surface in the 3x3 render texels round this pixel: at a moving object's
    // edge the pixel follows the object, not the background behind it, so the object leaves no trail.
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
    bool uncovered = false;
    if (!moved) {
        vec4 h = texelFetch(resolved, at, 0);
        historyColour = h.rgb;
        historyLength = abs(h.a);
        uncovered = h.a < 0.0;
    }
    if (uncovered) {
        historyLength = min(historyLength, 4.0);
        vec3 lo = current;
        vec3 hi = current;
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++) {
                vec3 c = texelFetch(reconstructed, clamp(at + ivec2(x, y), ivec2(0, 0), last), 0).rgb;
                lo = min(lo, c);
                hi = max(hi, c);
            }
        historyColour = clamp(historyColour, lo, hi);
    } else if (moved) {
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
            historyLength += abs(h.a) * w;
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

    float newLength = min(historyLength + 1.0, moved ? min(restirSettings.z, 16.0) : restirSettings.z);
    vec3 colour = historyLength > 0.0 ? mix(historyColour, current, 1.0 / newLength) : current;
    // Marked only under a still camera: a moving one moves every pixel, and its stopping uncovers nothing.
    bool objectMoved = frameCount.y < 0.5 && length(motion * res) > 0.01;
    gl_FragColor = vec4(colour, objectMoved ? -newLength : newLength);
}
