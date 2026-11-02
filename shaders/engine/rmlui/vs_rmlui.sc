$input a_position, a_texcoord0, a_color0
$output v_color0, v_texcoord0

// =============================================================================
// vs_rmlui.sc  --  Vertex shader for RmlUi geometry (the UI's user layer).
//
// RmlUi's vertex is a 2D position in window pixels, a premultiplied RGBA byte
// colour and one UV (Rml::Vertex). The backend folds RmlUi's transform and the
// geometry's translation into the model matrix (bgfx::setTransform) and sets the
// window's orthographic projection as the view transform, so u_modelViewProj
// carries all of it. Its own directory, for the varying.def.sc beside it.
// =============================================================================

#include <bgfx_shader.sh>

void main() {
    gl_Position = mul(u_modelViewProj, vec4(a_position.xy, 0.0, 1.0));
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
}
