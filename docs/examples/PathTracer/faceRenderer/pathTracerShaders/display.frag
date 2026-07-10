$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag  --  Pass 4 of the face renderer.
//
// Reads the accumulated per-face HDR colour from the ping-pong history buffer,
// applies ACES tone mapping + gamma, and writes to the back buffer. Identical to
// the other renderers' display pass -- the whole point of the face renderer is
// that everything upstream is already clean, so display has nothing to do but
// tonemap.
//
// Input (FBO 3): 0 accumLit.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(accumColor, 0);

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
