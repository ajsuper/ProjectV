$input v_color0
$input v_texcoord0

// accumulate.frag -- pass 2, window resolution: the running mean of every path since the view last
// changed. A move of the camera or the sun (frameCount.y) starts it over; nothing else does, so a still
// view keeps converging for as long as it is left. The mean is exact (each frame weighs 1 / n), not an
// exponential average, which would never settle.
//
// Inputs: 0 radiance (pass 1), 1 accumulated (last frame: this pass's own output, ping-ponged).
// Output: rgb the mean, a the number of paths in it.

#include <bgfx_shader.sh>

SAMPLER2D(radiance, 0);
SAMPLER2D(accumulated, 1);

uniform vec4 frameCount;   // y 1 when the camera or the sun moved this frame

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 current = texelFetch(radiance, p, 0).rgb;
    vec4 history = texelFetch(accumulated, p, 0);
    float n = frameCount.y > 0.5 || frameCount.x < 1.5 ? 0.0 : history.a;
    gl_FragColor = vec4(mix(history.rgb, current, 1.0 / (n + 1.0)), n + 1.0);
}
