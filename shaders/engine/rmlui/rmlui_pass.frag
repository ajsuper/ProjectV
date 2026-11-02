$input v_color0, v_texcoord0

// rmlui_pass.frag -- a texture, scaled by u_rmluiColour: copies, opacity, the layers' composites
// (docs/plans/ui.md, D20; RmlUi's GL3 passthrough). Premultiplied in, premultiplied out.

#include <bgfx_shader.sh>

SAMPLER2D(s_rmluiTexture, 0);
uniform vec4 u_rmluiColour;

void main() {
    gl_FragColor = texture2D(s_rmluiTexture, v_texcoord0) * u_rmluiColour;
}
