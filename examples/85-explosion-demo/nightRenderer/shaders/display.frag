$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag -- the last pass, at output resolution: the smoke composited over the frame, then linear
// HDR to the screen. All the grading is here, and the crosshair when one is asked for.
//
//   exposure (lightParams.z) -> vignette -> grade (pjvGrade: contrast, saturation) -> pjvDisplay
//
// All in linear light, before the engine's neutral transform (include/pjv_color.sc). The grade is this
// renderer's look; its contrast is a power about middle grey, so it cannot crush the shadows to black.
//
// The dither is not a look: the sky and the fog are smooth ramps over hundreds of pixels, and eight bits
// band them without it. It is hashed from the pixel only, so a still frame stays still.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_color.sc>
#include <grr_common.sc>

SAMPLER2D(frame, 0);
SAMPLER2D(smokeColor, 1);   // smoke.frag: rgb the gas's light, a what still shows through it
uniform vec4 smokeParams;   // z = smoke on
uniform vec4 renderParams;   // w = debug view: views 1 and 2 pass through ungraded

#define CONTRAST   1.08
#define SATURATION 1.10
#define VIGNETTE   0.18

void main() {
    vec3 hdr = texture2D(frame, v_texcoord0).rgb;
    if (smokeParams.z > 0.5) {
        vec4 smoke = texture2D(smokeColor, v_texcoord0);
        hdr = hdr * smoke.a + smoke.rgb;
    }
    if (renderParams.w > 0.5) {
        gl_FragColor = vec4(clamp(hdr, 0.0, 1.0), 1.0);
        return;
    }
    vec2 q = v_texcoord0 - 0.5;
    float r2 = dot(q, q) * 2.0;
    hdr *= lightParams.z * (1.0 - VIGNETTE * r2 * r2);

    vec3 col = pjvDisplay(pjvGrade(hdr, CONTRAST, SATURATION, vec3(1.0, 1.0, 1.0)), 1.0, gl_FragCoord.xy);

    // A crosshair, when asked for (sunDir.w): two short bars through the centre, inverted against
    // what is behind them.
    if (sunDir.w > 0.5) {
        vec2 size = vec2(dFdx(v_texcoord0.x), dFdy(v_texcoord0.y));
        vec2 offset = abs((v_texcoord0 - 0.5) / abs(size));
        bool bar = (offset.x < 1.0 && offset.y < 9.0 && offset.y > 3.0) || (offset.y < 1.0 && offset.x < 9.0 && offset.x > 3.0);
        if (bar) col = vec3(1.0, 1.0, 1.0) - col * 0.8;
    }
    gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
