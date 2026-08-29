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

SAMPLER2D(reconstructed, 0);

// ACES filmic approximation (Narkowicz).
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture2D(reconstructed, v_texcoord0).rgb;
    vec3 mapped = aces(hdr);
    mapped = pow(mapped, vec3(1.0 / 2.2));
    gl_FragColor = vec4(mapped, 1.0);
}
