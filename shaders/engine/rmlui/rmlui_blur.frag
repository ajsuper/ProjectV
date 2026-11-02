$input v_color0, v_texcoord0

// rmlui_blur.frag -- one direction of a separable Gaussian: seven taps along u_rmluiTexel.xy, four
// distinct weights (u_rmluiWeights), taps outside u_rmluiLimits (min xy, max xy, in uv) counted as
// transparent so nothing bleeds in from outside the region (RmlUi's GL3 blur).

#include <bgfx_shader.sh>

SAMPLER2D(s_rmluiTexture, 0);
uniform vec4 u_rmluiTexel;
uniform vec4 u_rmluiLimits;
uniform vec4 u_rmluiWeights;

void main() {
    vec4 colour = vec4(0.0, 0.0, 0.0, 0.0);
    for (int i = 0; i < 7; i++) {
        vec2 uv = v_texcoord0 - float(i - 3) * u_rmluiTexel.xy;
        vec2 inside = step(u_rmluiLimits.xy, uv) * step(uv, u_rmluiLimits.zw);
        int k = i < 3 ? 3 - i : i - 3;
        float w = k == 0 ? u_rmluiWeights.x : (k == 1 ? u_rmluiWeights.y : (k == 2 ? u_rmluiWeights.z : u_rmluiWeights.w));
        colour += texture2D(s_rmluiTexture, uv) * (inside.x * inside.y * w);
    }
    gl_FragColor = colour;
}
