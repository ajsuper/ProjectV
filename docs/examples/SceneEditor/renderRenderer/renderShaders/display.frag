$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag  --  Pass 3 of the scene editor's Render mode.
//
// Lens dispersion, exposure, tone map, gamma, and out to the texture the Render tab draws.
//
// This is the one place Render mode and the Viewport genuinely disagree about what
// a pixel means. The Viewport's display pass is a straight copy and says so at
// length: that renderer's output is stored reflectance, already in [0, 1], and
// tone mapping it would mean the editor is no longer showing the colour that is in
// the file.
//
// This renderer's output is radiance. It is open-ended -- a sunlit white voxel and
// the sun disk behind it differ by four orders of magnitude -- so it has to be
// mapped into a display range before it can be looked at, and ACES is the curve
// that does it without the highlight hue shifts a simple Reinhard gives you (the
// classic one being sunlit skin and orange-lit stone going yellow as they clip).
//
// Exposure is applied before the curve, in stops, because that is what an exposure
// control is: a multiply on the light reaching the sensor, not a lift on the
// developed image. Applying it afterwards would only stretch the already-clipped
// highlights.
//
// Everything in this pass is downstream of the accumulation, which is why none of these three
// controls restarts it: exposure, dispersion and the tone curve can all be dragged on an image
// that has been converging for a minute without costing that minute. Depth of field and the
// atmosphere are the opposite -- they change what is traced, so they do.
//
// Input (FBO 2): slot 0 = taaColor, the accumulated HDR radiance.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(taaColor, 0);

uniform vec4 texelSize;
// x = exposure in stops, y = chromatic aberration -- the red/blue separation at the corners of
// the image, in pixels. z/w spare. Kept as its own uniform rather than folded into renderParams so
// dragging any of it does not have to invalidate anything the trace pass reads.
uniform vec4 displayParams;

// ACES filmic, Narkowicz's fit. Cheap, and close enough to the reference curve that the
// difference does not survive an 8-bit target.
vec3 acesToneMap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Transverse chromatic aberration: a real lens does not bring every wavelength to the same
// magnification, so the image formed in red is very slightly larger than the one formed in blue.
// The mismatch is zero on the optical axis and grows towards the edge of the frame, which is why
// the offset here is radial and scaled by the squared radius rather than applied uniformly.
//
// Done as a resampling of the finished image rather than by tracing three wavelengths through the
// path tracer. Spectral rendering would be the honest version and would also give dispersion
// through refractive materials -- but this renderer has no refractive materials, so all it would
// buy is three times the noise for an effect that is, by construction, a property of the lens
// rather than of the light transport.
//
// The offset is expressed in PIXELS at the corner, not in UV, so the look does not change when the
// window is resized -- which it would if the separation were a fraction of the image.
vec3 sampleWithDispersion(vec2 uv, float cornerPixels) {
    if (cornerPixels <= 0.0) return texture2D(taaColor, uv).rgb;

    // -1 to 1 across the image, so `radial` is the direction to displace along and `falloff` is 0
    // at the centre and 1 at the corners.
    vec2 radial = (uv - 0.5) * 2.0;
    float falloff = clamp(dot(radial, radial) * 0.5, 0.0, 1.0);
    vec2 offset = radial * falloff * cornerPixels * texelSize.xy;

    // The render targets carry no clamp sampler, so an offset past the edge would wrap around and
    // fringe the top of the image with the bottom of it.
    vec2 low = texelSize.xy * 0.5;
    vec2 high = 1.0 - texelSize.xy * 0.5;

    // Red long, blue short, green undisplaced: the ordering of the visible spectrum, and the
    // reason the fringing reads as a lens artefact rather than as a colour bug.
    return vec3(texture2D(taaColor, clamp(uv + offset, low, high)).r,
                texture2D(taaColor, uv).g,
                texture2D(taaColor, clamp(uv - offset, low, high)).b);
}

void main() {
    vec3 hdr = max(sampleWithDispersion(v_texcoord0, displayParams.y), vec3(0.0));

    hdr *= exp2(displayParams.x);

    vec3 mapped = acesToneMap(hdr);

    // sRGB-ish encode. The target is a plain RGBA8 texture that ImGui samples without a
    // sRGB view, so the curve has to be applied here rather than left to the hardware.
    mapped = pow(mapped, vec3(1.0 / 2.2));

    gl_FragColor = vec4(mapped, 1.0);
}
