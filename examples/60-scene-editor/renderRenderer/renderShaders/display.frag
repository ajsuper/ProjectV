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

// x = exposure in stops, y = chromatic aberration -- the red/blue separation at the corners of
// the image, in pixels. z/w spare. Kept as its own uniform rather than folded into renderParams so
// dragging any of it does not have to invalidate anything the trace pass reads.
uniform vec4 displayParams;
// The grade, split by *where in the pipeline it applies* rather than by what the controls are called.
//
//   gradeTint.rgb   a colour cast, multiplied into linear radiance BEFORE the tone curve. Temperature
//                   is folded into it on the CPU, since it is a fixed pair of channel gains. A white
//                   balance belongs here because it is a property of the light: applying it after the
//                   curve tints the clipped highlights too, which is what a filter over a photograph
//                   looks like rather than what a filter over a lens looks like.
//   gradeParams     x = saturation, y = contrast, z = lift, w = vignette. All applied AFTER the curve,
//                   on the mapped image. Contrast and saturation in linear light push values out of
//                   gamut and clip rather than shape; in display space they do what the sliders'
//                   names promise, around a pivot that means something.
uniform vec4 gradeParams;
uniform vec4 gradeTint;
// Engine-set per pass: (w, h, 1/w, 1/h) of the target THIS pass writes. The aberration offsets and
// the edge clamp below are in this pass's own texels, which is why they take the target's size and
// not the traced image's -- the two differ as soon as the trace runs at a fraction of the display.
uniform vec4 passTargetRes;

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
    vec2 offset = radial * falloff * cornerPixels * passTargetRes.zw;

    // The render targets carry no clamp sampler, so an offset past the edge would wrap around and
    // fringe the top of the image with the bottom of it.
    vec2 low = passTargetRes.zw * 0.5;
    vec2 high = 1.0 - passTargetRes.zw * 0.5;

    // Red long, blue short, green undisplaced: the ordering of the visible spectrum, and the
    // reason the fringing reads as a lens artefact rather than as a colour bug.
    return vec3(texture2D(taaColor, clamp(uv + offset, low, high)).r,
                texture2D(taaColor, uv).g,
                texture2D(taaColor, clamp(uv - offset, low, high)).b);
}

// Rec. 709 luminance. Saturation is measured against this rather than against the channel average so
// that pulling the colour out of an image leaves it at the brightness the eye reads it at -- a flat
// mean makes blues too light and greens too dark, which shows up as a desaturated render that does not
// match the brightness of the one it came from.
float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    vec3 hdr = max(sampleWithDispersion(v_texcoord0, displayParams.y), vec3(0.0));

    hdr *= exp2(displayParams.x);

    // ---- Before the curve: the light ----
    //
    // The colour cast goes in here, while the image is still radiance. See the uniform's comment.
    hdr *= gradeTint.rgb;

    vec3 mapped = acesToneMap(hdr);

    // ---- After the curve: the picture ----
    //
    // Contrast about a mid-grey pivot rather than about black, so raising it darkens the shadows and
    // lifts the highlights instead of just making everything brighter. 0.5 is mid grey in the encoded
    // space this is about to be written into -- which is the space the eye judges the result in, and
    // the reason all of this sits after the tone map rather than before it.
    mapped = clamp((mapped - 0.5) * gradeParams.y + 0.5, 0.0, 1.0);

    // Saturation, against luminance.
    mapped = clamp(mix(vec3_splat(luminance(mapped)), mapped, gradeParams.x), 0.0, 1.0);

    // Lift: raise the floor without moving white, which is the compression a print has and a render
    // does not. Scaling the range down as the floor comes up is what keeps white at white -- adding a
    // constant instead would push the top of the image off the end and clip it flat.
    mapped = mapped * (1.0 - gradeParams.z) + vec3_splat(gradeParams.z);

    // Vignette. Radial like the aberration above, and for the same reason: it is what a lens does at
    // the edge of its own coverage. Squared falloff so the darkening starts gently rather than at a
    // visible ring.
    if (gradeParams.w > 0.0) {
        vec2 radial = (v_texcoord0 - 0.5) * 2.0;
        float falloff = clamp(dot(radial, radial) * 0.5, 0.0, 1.0);
        mapped *= 1.0 - falloff * falloff * gradeParams.w;
    }

    // sRGB-ish encode. The target is a plain RGBA8 texture that ImGui samples without a
    // sRGB view, so the curve has to be applied here rather than left to the hardware.
    mapped = pow(max(mapped, vec3(0.0)), vec3(1.0 / 2.2));

    gl_FragColor = vec4(mapped, 1.0);
}
