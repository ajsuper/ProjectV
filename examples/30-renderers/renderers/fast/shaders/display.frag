$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag  --  Pass 3 of the fast renderer.
//
// Reads the anti-aliased HDR radiance from the ping-pong history buffer, applies
// ACES tone mapping + gamma, and writes to the back buffer.
//
// Input (FBO 2): 0 shaded.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_color.sc>

SAMPLER2D(shaded, 0);

uniform vec4 windowRes;


void main() {
    vec3 hdr = texture2D(shaded, v_texcoord0).rgb;
    gl_FragColor = vec4(pjvDisplay(hdr, 1.0, gl_FragCoord.xy), 1.0);   // the engine's neutral transform
}
