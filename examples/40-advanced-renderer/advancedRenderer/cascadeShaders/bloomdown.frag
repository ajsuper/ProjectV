$input v_color0
$input v_texcoord0

// =============================================================================
// bloomdown.frag  --  Bright-pass, quarter-resolution downsample, and the TIGHT half of the bloom.
//
// TWO SCALES, and the reason is the "it stands out as a square" failure of the single-scale version.
// A one-voxel emitter covers a handful of pixels, which at an eighth resolution is a fraction of ONE
// texel. Blur that and magnify it eight times with a bilinear filter and what reaches the screen is a
// bilinear TENT over a single texel -- a diamond with straight edges and no bright centre. No amount
// of blurring fixes it, because the shape is coming from the magnification, not from the kernel.
//
// Real bloom is not one Gaussian. It is a bright, tight core with a faint, wide tail, and summing two
// very different scales is the cheap way to get that: this pass writes the tight half at a QUARTER
// resolution, bloomblur takes it down to an eighth for the wide half, and display.frag adds both. The
// tight component supplies the core and the definition, which is exactly what the wide one cannot,
// and the four-times magnification it is subject to shows far less than eight.
//
// It also carries the DEPTH the occlusion test in bloomblur needs; see the alpha channel below.
//
// THE DOWNSAMPLE TAPS. A 4x downsample has a 4x4 footprint. The 3x3 grid below is spaced two source
// texels apart on texel CORNERS, so every bilinear fetch averages 2x2 and the set covers 6x6 -- a
// little wider than the footprint, which overlaps neighbouring output texels slightly and is a mild
// blur in its own right. That overlap is deliberate: it costs nothing and it takes the hard edges off
// the quarter-resolution image before anything magnifies it.
//
// Why the corners land there for free: this target is a quarter of its input on each axis, so an
// output texel centre maps to the centre of a 4x4 source block, and 4 is even -- that point is a
// corner between texels. Even offsets keep it on corners.
//
// Input (FBO 10): 0 upscaledColor (the reconstructed HDR frame).
// Input (FBO 1):  1 gPos (a = camDist, a < 0 => sky). Bound for its DEPTH alone.
// Output (FBO 12): 0 rgb the tight bloom, a the scene depth, for bloomblur and for display.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(upscaledColor, 0);
SAMPLER2D(gPos,     1);

uniform vec4 passTargetRes;
uniform vec4 passInputRes[8];
// x = the debug view selector. The bright-pass must not run on a debug view: those are
// CLASSIFICATIONS drawn as colour, and blooming them would smear one category into the next.
uniform vec4 renderParams;

// ---- Knobs, and the arithmetic behind them -----------------------------------------------------
// Where the bloom starts, in linear HDR. Everything upstream is scene-referred and the tone map comes
// later, so this is a real radiance rather than a fraction of white -- which means it has to be set
// against the actual values in the frame, not guessed. Measured, in luminance:
//
//     lit surface   0.46      sky zenith    1.08      sky horizon   1.32
//     sky near sun  2.77      sun disk      6.02
//
// The first version of this was 1.15, which is BELOW THE ENTIRE SKY. The bright-pass therefore passed
// the whole sky -- half the screen on an outdoor view -- the blur smeared it across everything, and
// display added most of it back. That is where "washed out" came from: it was not a halo around
// anything, it was a screen-sized wash of sky colour laid over the frame.
//
// 2.6 sits above the open sky and below the sun disk.
#define BLOOM_THRESHOLD 2.6
// Soft knee. Without it a surface drifting across the threshold as the camera moves would switch its
// bloom on rather than fading it in.
#define BLOOM_KNEE      0.8
// Gaussian over the 3x3 grid, in grid units. Small: this is the TIGHT half, and its job is to keep a
// core, not to spread.
#define BLOOM_DOWN_SIGMA 1.1

// Sky's stand-in depth. A large finite number rather than an infinity because this rides in an
// RGBA16_FLOAT alpha channel, whose maximum is 65504 -- an inf or a 1e9 would come back as garbage,
// and every occlusion comparison downstream would be against a value that is not what was written.
#define BLOOM_FAR_DEPTH 60000.0

// The bright-pass, SUBTRACTIVE rather than a weighted pass-through.
//
// A weighted pass-through multiplies the whole colour by a 0..1 factor, so anything past the knee
// contributes its FULL brightness -- a surface a hair over the threshold and the sun disk become the
// same kind of thing, differing only in how much of each is let through. Taking the EXCESS over the
// threshold instead means brightness in the bloom scales with how bright something actually is, which
// is the behaviour the word names.
//
// Keyed on LUMINANCE and applied as a scale to the colour, so a saturated red blooms red rather than
// being desaturated by clamping each channel against the same threshold independently.
vec3 brightPass(vec3 c) {
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    float over = max(luma - BLOOM_THRESHOLD, 0.0);
    // over^2 / (over + knee) is ~0 just past the threshold and approaches `over` well beyond it, so
    // the curve leaves the threshold smoothly instead of with a corner.
    float keep = over * over / (over + BLOOM_KNEE);
    return c * (keep / max(luma, 1e-4));
}

void main() {
    if (renderParams.x > 0.5) {          // a debug view: nothing to bloom
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, BLOOM_FAR_DEPTH);
        return;
    }

    vec2 srcTexel = passInputRes[0].zw;

    vec3  sum    = vec3(0.0);
    float weight = 0.0;
    for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++) {
        vec2  o = vec2(float(x), float(y));
        float w = exp(-dot(o, o) / (2.0 * BLOOM_DOWN_SIGMA * BLOOM_DOWN_SIGMA));
        // Two source texels per grid step, which keeps every tap on a corner.
        sum    += brightPass(texture2D(upscaledColor, v_texcoord0 + o * 2.0 * srcTexel).rgb) * w;
        weight += w;
    }

    // The scene depth at this texel, carried alongside so bloomblur can ask what is between two
    // points without binding the G-buffer itself. Point-sampled at the centre rather than averaged:
    // an average of two depths across a silhouette is a distance belonging to neither surface, and
    // the occlusion test below compares against it.
    float depth = texture2D(gPos, v_texcoord0).a;

    gl_FragData[0] = vec4(sum / max(weight, 1e-5),
                          depth < 0.0 ? BLOOM_FAR_DEPTH : depth);
}
