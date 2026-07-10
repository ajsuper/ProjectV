$input v_color0
$input v_texcoord0

// =============================================================================
// denoise.frag  --  Pass 3 of the reprojection renderer.
//
// Edge-aware spatial denoiser (bilateral / à-trous-style single pass) applied to
// the temporally accumulated color. Its job is to clean the residual noise that
// the (deliberately light, non-smearing) temporal history leaves while the camera
// moves, so we get sharp AND clean motion without leaning on heavy temporal
// blending. When the camera is still the history has already converged, and the
// edge-stopping weights keep this pass from softening that result.
//
// Filtering is guided by the current, noise-free G-buffer: samples are only
// blended when they share a surface (matching world position + normal) and a
// similar albedo, which preserves geometry edges and voxel-color detail.
//
// Inputs (flattened FBO dependency order):
//   0 curColor 1 curPos 2 curNormal 3 curAlbedo   (FBO 1, current frame)
//   4 histColor 5 histPos 6 histNormal            (FBO 2, accumulated)
// Output (FBO 3): denoised color.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(curColor,   0);
SAMPLER2D(curPos,     1);
SAMPLER2D(curNormal,  2);
SAMPLER2D(curAlbedo,  3);
SAMPLER2D(histColor,  4);
SAMPLER2D(histPos,    5);
SAMPLER2D(histNormal, 6);

uniform vec4 windowRes;
uniform vec4 frameCount;

#define RADIUS 2     // 5x5 taps — kept small so the filter stays subtle
#define STEP   1.0   // no dilation: gentle, local smoothing only

void main() {
    vec2 uv = v_texcoord0;

    vec4  hc0 = texture2D(histColor, uv);
    vec3  c0  = hc0.rgb;
    float age = hc0.a;                       // temporal sample count at this pixel
    vec4  p0 = texture2D(curPos,     uv);   // xyz = world pos, w = depth
    vec3  n0 = texture2D(curNormal,  uv).xyz;
    vec3  a0 = texture2D(curAlbedo,  uv).rgb;

    // Sky / background has no surface to filter against; pass it straight through.
    if (dot(n0, n0) < 0.01) {
        gl_FragColor = vec4(c0, 1.0);
        return;
    }

    // How much to trust the temporal result vs. spatially filter it. While moving,
    // history is short and noisy -> filter fully. When parked, the temporal average
    // keeps converging; once its sample count is high the pixel is already clean, so
    // we fade the spatial filter OUT and pass the converged (and jitter-anti-aliased)
    // result through untouched -- no residual blur, no edge shimmer, true convergence.
    float converged = (frameCount.y != 0.0) ? 0.0 : smoothstep(8.0, 48.0, age);
    if (converged >= 0.999) {
        gl_FragColor = vec4(c0, 1.0);
        return;
    }

    vec2  texel  = 1.0 / windowRes.xy;
    float sigmaP = max(0.1, p0.w * 0.05); // position tolerance grows with distance

    vec3  sum  = c0;
    float wsum = 1.0;

    for (int y = -RADIUS; y <= RADIUS; y++) {
        for (int x = -RADIUS; x <= RADIUS; x++) {
            if (x == 0 && y == 0) continue;
            vec2 o = vec2(float(x), float(y)) * STEP * texel;

            vec3 ni = texture2D(curNormal, uv + o).xyz;
            if (dot(ni, ni) < 0.01) continue; // neighbour is sky

            vec3 ci = texture2D(histColor, uv + o).rgb;
            vec3 pi = texture2D(curPos,    uv + o).xyz;
            vec3 ai = texture2D(curAlbedo, uv + o).rgb;

            // Edge-stopping weights: same face, same surface point, same material.
            float wn = pow(max(0.0, dot(n0, ni)), 32.0);
            float dp = distance(p0.xyz, pi);
            float wp = exp(-(dp * dp) / (2.0 * sigmaP * sigmaP));
            float da = distance(a0, ai);
            float wa = exp(-da * 8.0);
            float ws = exp(-float(x * x + y * y) / (2.0 * float(RADIUS * RADIUS)));

            float w = wn * wp * wa * ws;
            sum  += ci * w;
            wsum += w;
        }
    }

    // Fade the filtered result back toward the raw converged pixel as it settles.
    vec3 filtered = sum / wsum;
    gl_FragColor = vec4(mix(filtered, c0, converged), 1.0);
}
