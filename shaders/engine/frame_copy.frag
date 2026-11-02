$input v_color0, v_texcoord0

// =============================================================================
// frame_copy.frag  --  The frame texture to the window (docs/plans/ui.md, D18).
//
// While the UI asks for the frame as a texture, the renderer's window passes
// write it; a frame the UI did not draw from is copied to the window with this,
// over the engine's fullscreen quad (vs_quad). Colour and alpha as they are.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(s_frame, 0);

void main() {
    gl_FragColor = texture2D(s_frame, v_texcoord0);
}
