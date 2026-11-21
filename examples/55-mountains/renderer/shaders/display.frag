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
uniform vec4 mtnSun;   // w: exposure


void main() {
    vec3 hdr = texture2D(shaded, v_texcoord0).rgb;
    // The mountains' look: the clear air of 2 km up reads saturated and a little contrasty, which the
    // neutral tone curve alone flattens to teal. Then the engine's neutral transform.
    hdr = pjvGrade(hdr * mtnSun.w, 1.08, 1.2, vec3(0.97, 1.0, 1.04));
    gl_FragColor = vec4(pjvDisplay(hdr, 1.0, gl_FragCoord.xy), 1.0);
}
