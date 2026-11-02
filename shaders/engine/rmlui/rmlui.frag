$input v_color0, v_texcoord0

// =============================================================================
// rmlui.frag  --  Fragment shader for RmlUi geometry.
//
// Textured and untextured geometry alike: untextured draws bind a 1x1 white
// texture, which leaves the vertex colour as it is. Colours are premultiplied
// (RmlUi's convention); the backend blends ONE, INV_SRC_ALPHA.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(s_rmluiTexture, 0);

void main() {
    gl_FragColor = texture2D(s_rmluiTexture, v_texcoord0) * v_color0;
}
