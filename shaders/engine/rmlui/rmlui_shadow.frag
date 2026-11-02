$input v_color0, v_texcoord0

// rmlui_shadow.frag -- a drop shadow: the image's alpha, in the shadow's (premultiplied) colour,
// inside the region (RmlUi's GL3 drop shadow). The offset is in the quad's uv.

#include <bgfx_shader.sh>

SAMPLER2D(s_rmluiTexture, 0);
uniform vec4 u_rmluiColour;
uniform vec4 u_rmluiLimits;

void main() {
    vec2 inside = step(u_rmluiLimits.xy, v_texcoord0) * step(v_texcoord0, u_rmluiLimits.zw);
    gl_FragColor = texture2D(s_rmluiTexture, v_texcoord0).a * inside.x * inside.y * u_rmluiColour;
}
