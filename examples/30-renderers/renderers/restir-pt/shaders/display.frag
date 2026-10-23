$input v_color0
$input v_texcoord0

// display.frag -- the last pass, window: linear light to the screen.
//
//   post on (P):  bloom (N) -> vignette (V) -> auto-exposure (E) -> grade (B) -> pjvDisplay
//   post off:     pjvDisplay, the engine's neutral transform: tone curve, sRGB encoding, dither
//
// Every step of the post is in linear light, before the tone curve, and each is the identity when off,
// so with all three off the image is exactly the neutral one. The views (G) other than the image skip it:
// they are for reading one part of the light, which a look would only distort.
// Inputs: 0 dofColor (the image, after the depth of field), 1 exposure, 2 bloomUp1 (bloom.sc: the sum of
// its five levels, half the window's resolution).

#include <bgfx_shader.sh>
#include <pjv_color.sc>

SAMPLER2D(dofColor, 0);
SAMPLER2D(exposure, 1);
SAMPLER2D(bloomUp1, 2);

uniform vec4 restirPost;      // x vignette strength (0 off), y grade on, z auto-exposure on, w seconds since last frame
uniform vec4 restirDenoise;   // z the view (0 the image)
uniform vec4 restirDof;       // y the bloom's share of the image (0: off)

// The grade: a touch of contrast about middle grey, a touch of colour, and a slightly warm white.
#define GRADE_CONTRAST   1.08
#define GRADE_SATURATION 1.10
#define GRADE_WHITE      vec3(1.02, 1.0, 0.97)

void main() {
    vec2 size = vec2(textureSize(dofColor, 0));
    vec3 c = texelFetch(dofColor, ivec2(v_texcoord0 * size), 0).rgb;
    if (restirDenoise.z < 0.5) {
        // The glow: the bloom's five levels, averaged, mixed in -- a share of the light moved, not added.
        if (restirDof.y > 0.0) c = mix(c, texture2D(bloomUp1, v_texcoord0).rgb * 0.2, restirDof.y);
        if (restirPost.x > 0.0) c *= pjvVignette(v_texcoord0, size.x / size.y, restirPost.x);
        if (restirPost.z > 0.5) c *= texelFetch(exposure, ivec2(0, 0), 0).x;
        if (restirPost.y > 0.5) c = pjvGrade(c, GRADE_CONTRAST, GRADE_SATURATION, GRADE_WHITE);
    }
    gl_FragColor = vec4(pjvDisplay(c, 1.0, gl_FragCoord.xy), 1.0);
}
