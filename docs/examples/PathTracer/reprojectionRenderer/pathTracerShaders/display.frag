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

SAMPLER2D(accumColor, 0);

uniform vec4 windowRes;

vec3 acesToneMap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture2D(accumColor, v_texcoord0).rgb;
    vec3 mapped = acesToneMap(hdr);
    mapped = pow(mapped, vec3(1.0 / 2.2));
    gl_FragColor = vec4(mapped, 1.0);
}
