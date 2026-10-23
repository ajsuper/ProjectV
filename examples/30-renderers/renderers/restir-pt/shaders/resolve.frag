$input v_color0
$input v_texcoord0

// resolve.frag -- pass 14, window resolution: temporal accumulation of the reconstructed image. The
// G-buffer traces each render texel through the centre of a different one of the window pixels it covers
// every frame (2x2 of them, in turn), and the reconstruction rebuilds the window from each; this averages
// those four, and what noise the denoiser left.
//
//   * camera still: the pixel's own history is always kept. A still view repeats the same four samples,
//     so it settles to their average and stays. (Rejecting history whenever the hit changed surface threw
//     it away every frame wherever voxels are smaller than a pixel: the distant shimmer.)
//   * camera or sun moving (frameCount.y): the pixel's point -- the nearest render texel's G-buffer hit --
//     is carried through last frame's camera, the history read bilinearly there and clamped into the
//     colour range of this frame's 3x3 neighbourhood (TAA's neighbourhood clamp), which drops what was
//     uncovered or changed without asking which surface is which; and the history is held short
//     (restirSettings.z) so changing light is not dragged along.
//
// Inputs: 0 reconstructed; 1-6 the G-buffer (gPos 1); 7 resolved (last frame, this pass's own output).
// Output: rgb the colour, a its history length in frames.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(reconstructed, 0);
SAMPLER2D(gPos, 1);
SAMPLER2D(resolved, 7);

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
    if (frameCount.y < 0.5) {
        vec4 h = texelFetch(resolved, at, 0);
        historyColour = h.rgb;
        historyLength = h.a;
    } else {
        vec2 centre = (vec2(at) + 0.5) / res;
        vec2 renderRes = vec2(textureSize(gPos, 0));
        vec4 surface = texelFetch(gPos, min(ivec2(centre * renderRes), ivec2(renderRes) - ivec2(1, 1)), 0);
        vec3 P = surface.xyz;
        // The sky has no point: a far one along the ray, which moves only with the camera's turn.
        if (surface.w < 0.0) P = cameraPos.xyz + normalize(P - cameraPos.xyz) * 1.0e4;
        bool nowInFront;
        bool thenInFront;
        vec2 uvNow = restirWorldToUV(P, cameraPos.xyz, cameraDir.xyz, res, nowInFront);
        vec2 uvThen = restirWorldToUV(P, prevCameraPos.xyz, prevCameraDir.xyz, res, thenInFront);
        vec2 pix = (centre + (uvThen - uvNow)) * res - 0.5;   // in texel-centre space
        float weight = 0.0;
        if (thenInFront) {
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
        }
        if (weight > 1e-3) {
            historyColour /= weight;
            historyLength /= weight;
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

    float newLength = min(historyLength + 1.0, restirSettings.z);
    vec3 colour = historyLength > 0.0 ? mix(historyColour, current, 1.0 / newLength) : current;
    gl_FragColor = vec4(colour, newLength);
}
