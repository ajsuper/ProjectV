$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag  --  Final pass of the world-space radiance-cascade renderer.
//
// Everything upstream is done: compose recombined the HDR frame and taa anti-aliased it. This pass
// turns that linear HDR into the final displayed pixel and is where ALL colour grading lives.
//
// -----------------------------------------------------------------------------
// HOW TO CHANGE THE COLOUR GRADING
// -----------------------------------------------------------------------------
// The pipeline below runs in this fixed order:
//
//   linear HDR  ->  EXPOSURE + white-balance TINT   (still linear / scene-referred)
//               ->  ACES tone map                   (HDR -> [0,1] display range)
//               ->  gamma encode (^(1/2.2))         (linear -> sRGB-ish)
//               ->  CONTRAST + SATURATION           (display-referred trim)
//
// Every knob is a #define right here; all default to a no-op, so out of the box the image is
// unchanged. Recompile the renderer (compWorldCascade.sh) after editing.
//
//   EXPOSURE   - overall brightness BEFORE tone mapping. This is the main "brighter/darker" dial;
//                because it is applied in linear light before ACES, it rolls highlights off
//                naturally instead of clipping. 1.0 = default, 2.0 = one stop brighter.
//   TINT       - per-channel white balance multiplier, also pre-tone-map. Warm the image with e.g.
//                vec3(1.05, 1.0, 0.95); cool it with vec3(0.95, 1.0, 1.05). Keep values near 1.
//   CONTRAST   - S-curve strength around mid-grey, applied AFTER tone map. 1.0 = none, >1 punchier.
//   SATURATION - colourfulness AFTER tone map. 1.0 = none, 0.0 = greyscale, >1 = more saturated.
//
// To swap the TONE MAPPER itself, replace the body of acesToneMap() (e.g. Reinhard
// `x / (1.0 + x)`, or a filmic/AgX curve) -- everything else stays the same. To change the sky/sun
// look with the time of day, that lives in sharedShaders/pjv_sun_sky.sc, not here.
//
// Input (FBO 9): 0 accumColor (anti-aliased HDR).
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(accumColor, 0);

// ---- Colour grading knobs (all defaults are no-ops) -------------------------
#define EXPOSURE    0.55
#define TINT        vec3(1.0, 1.0, 1.0)
#define CONTRAST    1.12
#define SATURATION  1.2

vec3 acesToneMap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture2D(accumColor, v_texcoord0).rgb;

    // Scene-referred (linear): exposure + white balance.
    hdr *= EXPOSURE * TINT;

    // HDR -> display range, then gamma encode.
    vec3 col = acesToneMap(hdr);
    col = pow(col, vec3(1.0 / 2.2));

    // Display-referred trim: contrast around mid-grey, then saturation about luma.
    col = (col - 0.5) * CONTRAST + 0.5;
    float luma = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(luma), col, SATURATION);

    gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
