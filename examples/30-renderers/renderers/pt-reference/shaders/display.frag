$input v_color0
$input v_texcoord0

// display.frag -- pass 3: the engine's neutral display transform (pjvDisplay: tone curve, sRGB, dither),
// at exposure 1 and with no look on top -- what restir-pt shows with its post off (P), so the two compare.
// Input: 0 accumulated.

#include <bgfx_shader.sh>
#include <pjv_color.sc>

SAMPLER2D(accumulated, 0);

void main() {
    vec3 c = texelFetch(accumulated, ivec2(gl_FragCoord.xy), 0).rgb;
    gl_FragColor = vec4(pjvDisplay(c, 1.0, gl_FragCoord.xy), 1.0);
}
