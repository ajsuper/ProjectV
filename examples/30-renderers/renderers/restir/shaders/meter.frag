$input v_color0
$input v_texcoord0

// meter.frag -- a fixed 64x64 target: the image's brightness, for the auto-exposure. Each texel
// is the mean log2 luminance of a 4x4 grid of samples across its block of the window, so the whole frame
// is metered from 64K samples, wherever the window's size. Log, so a small bright sky does not outweigh
// a large dim room the way a mean of the luminance itself would let it.
// Input: 0 resolved (the accumulated image, linear, before any exposure).

#include <bgfx_shader.sh>
#include <pjv_color.sc>

SAMPLER2D(resolved, 0);

#define METER_TAPS 4

void main() {
    ivec2 window = textureSize(resolved, 0);
    vec2 block = vec2(window) / vec2(64.0, 64.0);
    vec2 corner = floor(gl_FragCoord.xy) * block;
    float sum = 0.0;
    for (int y = 0; y < METER_TAPS; y++)
        for (int x = 0; x < METER_TAPS; x++) {
            vec2 at = corner + (vec2(float(x), float(y)) + 0.5) * block / float(METER_TAPS);
            vec3 c = texelFetch(resolved, clamp(ivec2(at), ivec2(0, 0), window - ivec2(1, 1)), 0).rgb;
            sum += log2(max(pjvLuminance(c), 1.0e-4));
        }
    gl_FragColor = vec4(sum / float(METER_TAPS * METER_TAPS), 0.0, 0.0, 1.0);
}
