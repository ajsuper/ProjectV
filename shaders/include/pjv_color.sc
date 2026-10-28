#ifndef PJV_COLOR_SC
#define PJV_COLOR_SC

#include <pjv_random.sc>   // pjvInterleavedGradientNoise, for the dither

// =============================================================================
// pjv_color.sc -- ProjectV's colour convention, in one place (docs/plans/color-convention.md)
// =============================================================================
//
//   stored colour (palette "color" / "emission")   sRGB-encoded, 10 bits a channel
//        | pjvSrgbToLinear, in decodeMaterial -- every renderer gets linear albedo
//   shading                                        linear light
//        | pjvDisplay: exposure -> pjvTonemapNeutral -> pjvLinearToSrgb -> pjvDither
//   back buffer                                    RGBA8, sRGB-encoded by the shader
//
// The tone curve is the identity up to PJV_TONEMAP_START and Khronos PBR Neutral's highlight shoulder
// above it, without PBR Neutral's toe and 0.04 offset (those assume a 4% Fresnel layer on every surface,
// which most renderers here do not shade). So a surface lit to radiance = albedo shows its stored
// colour, and dark interiors are not crushed. A look (contrast, saturation, vignette) is optional, on
// top: pjvGrade and pjvVignette, both in linear light, both the identity at their neutral settings.

#define PJV_TONEMAP_START 0.76
#define PJV_TONEMAP_DESATURATION 0.15

// The sRGB transfer functions, piecewise (IEC 61966-2-1), not a 2.2 power.
float pjvSrgbToLinear1(float c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float pjvLinearToSrgb1(float c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
vec3 pjvSrgbToLinear(vec3 c) {
    return vec3(pjvSrgbToLinear1(c.r), pjvSrgbToLinear1(c.g), pjvSrgbToLinear1(c.b));
}
vec3 pjvLinearToSrgb(vec3 c) {
    c = max(c, vec3(0.0, 0.0, 0.0));
    return vec3(pjvLinearToSrgb1(c.r), pjvLinearToSrgb1(c.g), pjvLinearToSrgb1(c.b));
}

// Rec. 709 luminance of linear light.
float pjvLuminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Identity below PJV_TONEMAP_START; above it, the peak channel is compressed towards 1 and the colour
// pulled a little towards white as it is (PBR Neutral's shoulder), so a bright saturated light does not
// clip to a flat primary.
vec3 pjvTonemapNeutral(vec3 c) {
    c = max(c, vec3(0.0, 0.0, 0.0));
    float peak = max(c.r, max(c.g, c.b));
    if (peak < PJV_TONEMAP_START) return c;
    float d = 1.0 - PJV_TONEMAP_START;
    float newPeak = 1.0 - d * d / (peak + d - PJV_TONEMAP_START);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (PJV_TONEMAP_DESATURATION * (peak - newPeak) + 1.0);
    return mix(c, vec3(newPeak, newPeak, newPeak), g);
}

// Half a quantum of noise either way, hashed from the pixel (interleaved gradient noise), so a still
// frame stays still. Not a look: an 8-bit target bands a smooth sky or fog ramp without it.
vec3 pjvDither(vec3 encoded, vec2 fragCoord) {
    float noise = pjvInterleavedGradientNoise(fragCoord);
    return encoded + vec3(noise - 0.5, noise - 0.5, noise - 0.5) / 255.0;
}

// Linear HDR to what the back buffer holds.
vec3 pjvDisplay(vec3 hdr, float exposure, vec2 fragCoord) {
    vec3 encoded = pjvLinearToSrgb(pjvTonemapNeutral(hdr * exposure));
    return clamp(pjvDither(encoded, fragCoord), 0.0, 1.0);
}

// ---- Looks: optional, linear light, before the tone curve ------------------------------------------

// Contrast as a power about middle grey (0.18): black stays black and nothing clips, unlike a linear
// stretch about display 0.5, which pushes every dark value below the pivot's reach to zero. Saturation
// against luminance; whiteBalance multiplies (1,1,1 = none). contrast = saturation = 1 is the identity.
vec3 pjvGrade(vec3 c, float contrast, float saturation, vec3 whiteBalance) {
    c = max(c, vec3(0.0, 0.0, 0.0)) * whiteBalance;
    float l = pjvLuminance(c);
    if (l > 1e-6) c *= 0.18 * pow(l / 0.18, contrast) / l;
    float l2 = pjvLuminance(c);
    return max(mix(vec3(l2, l2, l2), c, saturation), vec3(0.0, 0.0, 0.0));
}

// A lens's corner falloff, as a factor on linear light: 1 at the centre, 1 - strength in the corners,
// flat over the middle. uv in [0, 1]; aspect = width / height, so the falloff is round, not oval.
float pjvVignette(vec2 uv, float aspect, float strength) {
    vec2 q = (uv - 0.5) * vec2(aspect, 1.0) / sqrt(aspect * aspect + 1.0) * 2.0;   // corner at |q| = 1
    float r2 = dot(q, q);
    return 1.0 - strength * r2 * r2;
}

#endif
