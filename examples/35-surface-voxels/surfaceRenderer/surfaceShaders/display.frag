$input v_color0
$input v_texcoord0

// Copies the rendered frame to the back buffer. The frame is drawn into a texture first only so the
// viewer can read it back (SURFACE_CAPTURE) without screenshotting the desktop.

#include <bgfx_shader.sh>

SAMPLER2D(frameColour, 0);

void main() {
    gl_FragColor = vec4(texture2D(frameColour, v_texcoord0).rgb, 1.0);
}
