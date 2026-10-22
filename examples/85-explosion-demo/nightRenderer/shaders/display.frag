$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag -- the last pass, at output resolution: the smoke composited over the frame, then linear
// HDR to the screen. All the grading is here, and the crosshair when one is asked for.
//
//   exposure (lightParams.z) -> vignette -> ACES -> gamma -> contrast, saturation -> dither
//
// The dither is not a look: the sky and the fog are smooth ramps over hundreds of pixels, and eight bits
// band them without it. It is hashed from the pixel only, so a still frame stays still.
// =============================================================================

#include <bgfx_shader.sh>
#include <grr_common.sc>

SAMPLER2D(frame, 0);
SAMPLER2D(smokeColor, 1);   // smoke.frag: rgb the gas's light, a what still shows through it
uniform vec4 smokeParams;   // z = smoke on
uniform vec4 renderParams;   // w = debug view: views 1 and 2 pass through ungraded

#define CONTRAST   1.12
#define SATURATION 1.25
#define VIGNETTE   0.18

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

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

    vec3 col = pow(aces(hdr), vec3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
    col = (col - 0.5) * CONTRAST + 0.5;
    float luma = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(luma, luma, luma), col, SATURATION);

    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    col += (noise - 0.5) / 255.0;

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
