$input v_color0, v_texcoord0

// rmlui_matrix.frag -- the colour-matrix filters: brightness, contrast, invert, grayscale, sepia,
// hue-rotate, saturate (RmlUi's GL3 colour matrix). RGB only, in premultiplied space: the matrix's
// constant column is scaled by alpha, which it is by working on premultiplied colour directly.

#include <bgfx_shader.sh>

SAMPLER2D(s_rmluiTexture, 0);
uniform mat4 u_rmluiMatrix;

void main() {
    vec4 colour = texture2D(s_rmluiTexture, v_texcoord0);
    vec3 transformed = mul(u_rmluiMatrix, colour).xyz;
    gl_FragColor = vec4(transformed, colour.a);
}
