$input v_color0
$input v_texcoord0

// =============================================================================
// bloomblur.frag  --  The WIDE half of the bloom, and the pass that makes it respect geometry.
//
// Reads bloomdown's quarter-resolution tight bloom and writes the eighth-resolution wide one, so it
// downsamples 2x and blurs in the same pass. display.frag then adds both halves; see the note at the
// top of bloomdown.frag for why one scale could never look right.
//
// -----------------------------------------------------------------------------
// OCCLUSION, WHICH A LENS BLOOM DOES NOT HAVE
// -----------------------------------------------------------------------------
// A plain bloom is a camera effect: light scatters inside the lens, so it spills over everything in
// the frame regardless of what is in front of what. That is why the first version put a halo above a
// lamp's cover -- correctly, for a lens, and wrongly for what is actually wanted here, which is the
// glow of lit AIR around an emitter. Air behind a solid object is not lit by the emitter, so the
// cover should shadow the glow above it.
//
// The test that gets this right is not the obvious one. Comparing the source's depth against the
// receiver's does not work: the lamp is NEARER than the sky above the cover, and "nearer" is exactly
// the case a glow is supposed to spill across. What separates the two cases is what lies BETWEEN
// them, so that is what this samples -- the depth at the MIDPOINT of the line from receiver to
// source. Walking the cases:
//
//   * Receiver in the sky above the cover, source the lamp below it. The midpoint lands on the cover,
//     whose depth is far nearer than the sky's. Something is in between -> blocked.
//   * Receiver on the lamp post beside the lamp. The midpoint is on the post, at much the same depth
//     as both ends. Nothing in between -> full weight.
//   * Receiver in open sky to the SIDE of the lamp. The midpoint is sky as well, so it is not nearer
//     than either end -> full weight, and the lamp glows into the sky as it should.
//
// One extra fetch per tap, and it is the whole feature.
//
// WHAT IT CANNOT DO. The occluder has to be resolvable at this resolution. An eighth-resolution texel
// is eight pixels across, so a cover only a few pixels wide on screen -- a distant lamp, a thin
// canopy -- falls between the samples and its shadow is missed. The failure is graceful (the glow
// simply carries on through, which is what it did before) and it gets better as you walk toward the
// thing, which is also when you would notice.
//
// Input (FBO 12): 0 rgb the tight bloom, a the scene depth (sky as BLOOM_FAR_DEPTH).
// Output (FBO 13): 0 the wide bloom, for display.frag to add alongside the tight one.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(bloomDown, 0);

uniform vec4 passTargetRes;
uniform vec4 passInputRes[8];

// Gaussian width, in texels of the INPUT (quarter resolution). One of those is four output pixels, so
// this covers roughly fifty pixels of the final image -- wide, but no longer the only thing there is,
// since the tight half now supplies the core.
#define BLOOM_SIGMA 1.9
// Taps per axis, at half-integer offsets so each fetch averages 2x2 of the input.
#define BLOOM_TAPS 6

// How much nearer than both endpoints the midpoint has to be before it counts as blocking, relative
// to the endpoint distance. Relative because the G-buffer's own precision is, and soft-edged so a
// silhouette fades the glow out rather than cutting it.
#define BLOOM_OCCL_FADE 0.10

void main() {
    // Taps are spaced in the INPUT's texels, not this target's: the kernel's width is a property of
    // the image being filtered, and this pass happens to write at half of it.
    vec2 texel = passInputRes[0].zw;

    // The depth at the receiving point, which the midpoint below is compared against.
    float receiverDepth = texture2D(bloomDown, v_texcoord0).a;

    vec3  sum    = vec3(0.0);
    float weight = 0.0;

    // ---- HALF-INTEGER OFFSETS, SYMMETRIC ABOUT THE CENTRE --------------------------------------
    // Each tap sits on a texel CORNER so its bilinear fetch averages 2x2, which doubles the effective
    // footprint for free. The offsets must also be balanced around the centre: an earlier version
    // added 0.5 to a symmetric range, which put every tap half a texel toward +x and +y and shifted
    // the ENTIRE kernel diagonally, so every halo sat visibly offset from the thing casting it.
    for (int y = 0; y < BLOOM_TAPS; y++)
    for (int x = 0; x < BLOOM_TAPS; x++) {
        vec2  o = vec2(float(x), float(y)) - (float(BLOOM_TAPS) - 1.0) * 0.5;
        float w = exp(-dot(o, o) / (2.0 * BLOOM_SIGMA * BLOOM_SIGMA));

        vec2  tapUV = v_texcoord0 + o * texel;
        vec4  tap   = texture2D(bloomDown, tapUV);

        // Is anything between the two? Sampled halfway along the line joining them.
        float midDepth = texture2D(bloomDown, (v_texcoord0 + tapUV) * 0.5).a;
        float nearest  = min(receiverDepth, tap.a);
        float blocked  = smoothstep(0.0, 1.0,
                                    (nearest - midDepth) / max(nearest * BLOOM_OCCL_FADE, 1e-4));

        // The kernel weight goes into the denominator whatever happens, so an occluded tap contributes
        // DARKNESS rather than being edited out of the average -- otherwise a single unblocked
        // direction would read as bright as an unobstructed emitter.
        sum    += tap.rgb * (w * (1.0 - blocked));
        weight += w;
    }

    gl_FragData[0] = vec4(sum / max(weight, 1e-5), 1.0);
}
