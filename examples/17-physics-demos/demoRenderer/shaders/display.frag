$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag -- pass 5, at output resolution: the reconstructed frame, and the crosshair.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(reconstructed, 0);
uniform vec4 passTargetRes;

void main() {
    vec3 colour = texture2D(reconstructed, v_texcoord0).rgb;
    // Two short bars through the centre, inverted against what is behind.
    vec2 offset = abs(v_texcoord0 * passTargetRes.xy - passTargetRes.xy * 0.5);
    bool bar = (offset.x < 1.0 && offset.y < 9.0 && offset.y > 3.0) ||
               (offset.y < 1.0 && offset.x < 9.0 && offset.x > 3.0);
    if (bar) colour = vec3(1.0) - colour * 0.8;
    gl_FragColor = vec4(clamp(colour, 0.0, 1.0), 1.0);
}
