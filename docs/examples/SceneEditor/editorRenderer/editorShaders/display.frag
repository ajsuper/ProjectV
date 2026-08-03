$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag  --  Pass 4 of the scene editor's viewport renderer.
//
// Copies the accumulated image to the viewport texture, deliberately doing
// nothing to it. (In the standalone previewer this pass targets the back buffer;
// here it targets FBO 3, which the editor hands to ImGui as the image drawn in
// the Viewport panel. Everything below is otherwise unchanged.) The lit renderers ACES tone-map and gamma-encode here because they produce
// open-ended HDR radiance; this one produces reflectance, which is already in
// [0, 1] and is exactly the value stored in the scene's material palette
// (fetchVoxelColor decodes R10G10B10 straight to 0..1).
//
// Tone mapping that would desaturate and lift it, and a gamma curve would wash
// it out relative to the source texture the voxelizer sampled -- either one
// means the previewer is no longer showing you the colour that is in the file.
// So: straight through.
//
// If your scene's palette was authored in linear rather than sRGB, set
// PREVIEW_APPLY_GAMMA to 1 to encode on the way out.
//
// Input (FBO 2): slot 0 = accumColor.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(accumColor, 0);

#define PREVIEW_APPLY_GAMMA 0

void main() {
    vec3 color = texture2D(accumColor, v_texcoord0).rgb;

#if PREVIEW_APPLY_GAMMA
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
#endif

    gl_FragColor = vec4(color, 1.0);
}
