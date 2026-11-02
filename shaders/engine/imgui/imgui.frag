$input v_color0, v_texcoord0

// =============================================================================
// imgui.frag  --  Fragment shader for ImGui draw lists.
//
// Every ImGui draw call — text, panel backgrounds, and the scene image in the
// Viewport panel — is a textured, vertex-coloured triangle, so one shader covers
// all of them. Untextured geometry samples ImGui's atlas at a white texel, which
// leaves the vertex colour untouched.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(s_imguiTexture, 0);

void main() {
    gl_FragColor = texture2D(s_imguiTexture, v_texcoord0) * v_color0;
}
