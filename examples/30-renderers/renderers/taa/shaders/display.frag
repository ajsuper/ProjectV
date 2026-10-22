$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag  --  Pass 3 of the reprojection renderer.
//
// Reads the accumulated HDR radiance from the ping-pong history buffer, applies
// ACES tone mapping + gamma, and writes to the back buffer.
//
// Input (FBO 3): 0 denoisedColor.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_color.sc>

SAMPLER2D(accumColor, 0);

uniform vec4 windowRes;


void main() {
    vec3 hdr = texture2D(accumColor, v_texcoord0).rgb;
    gl_FragColor = vec4(pjvDisplay(hdr, 1.0, gl_FragCoord.xy), 1.0);   // the engine's neutral transform
}
