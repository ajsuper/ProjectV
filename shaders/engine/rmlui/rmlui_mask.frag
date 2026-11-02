$input v_color0, v_texcoord0

// rmlui_mask.frag -- a layer through a mask image's alpha (RmlUi's mask-image property).

#include <bgfx_shader.sh>

SAMPLER2D(s_rmluiTexture, 0);
SAMPLER2D(s_rmluiMask, 1);

void main() {
    gl_FragColor = texture2D(s_rmluiTexture, v_texcoord0) * texture2D(s_rmluiMask, v_texcoord0).a;
}
