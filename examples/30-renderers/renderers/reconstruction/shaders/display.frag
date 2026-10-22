$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag  --  Pass 4. Tone map and gamma, at OUTPUT resolution.
//
// Runs after the reconstruction, not before, so the tone curve is applied once to the final
// magnified image. Tone mapping at render resolution and then magnifying would interpolate between
// already-compressed values, which is not the same thing and loses highlight detail at every edge
// the reconstruction is trying to rebuild.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_color.sc>

SAMPLER2D(reconstructed, 0);


void main() {
    vec3 hdr = texture2D(reconstructed, v_texcoord0).rgb;
    gl_FragColor = vec4(pjvDisplay(hdr, 1.0, gl_FragCoord.xy), 1.0);   // the engine's neutral transform
}
