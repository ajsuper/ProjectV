$input a_position, a_texcoord0, a_color0
$output v_color0, v_texcoord0

// =============================================================================
// vs_imgui.sc  --  Vertex shader for ImGui draw lists.
//
// ImGui emits vertices already in framebuffer pixel coordinates, so the only
// transform is the orthographic projection the backend sets as the view
// transform (see imgui_impl_bgfx.cpp). u_viewProj carries it.
//
// These shaders live in their own directory because of the varying.def.sc beside
// them: ImDrawVert fixes ImGui's vertex as a 2D position, one UV and a packed
// RGBA byte colour, which cannot share the viewport renderer's varying
// definition, where a_position is the vec3 of a fullscreen quad. Note that the
// varying file itself carries no comments -- shaderc's parser for it does not
// accept them.
// =============================================================================

#include <bgfx_shader.sh>

void main() {
    gl_Position = mul(u_viewProj, vec4(a_position.xy, 0.0, 1.0));
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
}
