$input v_color0
$input v_texcoord0

// display.frag -- pass 15: the voxel renderer's grading (examples/shared/voxelRenderer/shaders/
// display.frag) without its vignette: ACES, gamma, then a little contrast and saturation -- and a dither,
// which is not a look: the sky is a smooth ramp over hundreds of pixels, and eight bits band it without.
// Input: 0 resolved.

#include <bgfx_shader.sh>

SAMPLER2D(resolved, 0);

#ifndef RESTIR_GRADE
#define RESTIR_GRADE 1
#endif
#define CONTRAST   1.12
#define SATURATION 1.25

void main() {
    vec2 size = vec2(textureSize(resolved, 0));
    vec3 c = texelFetch(resolved, ivec2(v_texcoord0 * size), 0).rgb;
    c = clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
    c = pow(c, vec3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
#if RESTIR_GRADE
    c = (c - 0.5) * CONTRAST + 0.5;
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(luma, luma, luma), c, SATURATION);
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    c += (noise - 0.5) / 255.0;
#endif
    gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
