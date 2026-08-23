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
//   linear HDR  ->  VIGNETTE                        (still linear -- a lens effect, not a paint)
//               ->  EXPOSURE + white-balance TINT   (still linear / scene-referred)
//               ->  ACES tone map                   (HDR -> [0,1] display range)
//               ->  gamma encode (^(1/2.2))         (linear -> sRGB-ish)
//               ->  CONTRAST + SATURATION           (display-referred trim)
//               ->  DITHER                          (last, at the output quantum)
//
// Every knob is a #define right here. Recompile the renderer (compAdvanced.sh) after editing.
//
//   EXPOSURE   - overall brightness BEFORE tone mapping. This is the main "brighter/darker" dial;
//                because it is applied in linear light before ACES, it rolls highlights off
//                naturally instead of clipping. 1.0 = default, 2.0 = one stop brighter.
//   TINT       - per-channel white balance multiplier, also pre-tone-map. Warm the image with e.g.
//                vec3(1.05, 1.0, 0.95); cool it with vec3(0.95, 1.0, 1.05). Keep values near 1.
//                NOTE that most of this renderer's warmth is NOT here: it is SUN_WARMTH in
//                pjv_sun_sky.sc, which tints the key light and leaves the sky-lit shadows cool. A
//                white balance cannot do that -- it moves lit and shadowed pixels by the same
//                factor -- so this one stays small and only finishes the job.
//   CONTRAST   - S-curve strength around mid-grey, applied AFTER tone map. 1.0 = none, >1 punchier.
//   SATURATION - colourfulness AFTER tone map. 1.0 = none, 0.0 = greyscale, >1 = more saturated.
//   VIGNETTE   - corner falloff, applied in LINEAR light before the tone map, which is what a lens
//                actually does: the corners lose light and the tone curve then rolls what is left,
//                so bright corners darken gracefully instead of being multiplied down after
//                clipping. 0.0 = off.
//   DITHER     - amplitude, in output quanta, of the noise added at the very end. This is not a
//                look; it is what stops the sky gradient and the distance fog from banding. Both
//                are smooth ramps over hundreds of pixels, an 8-bit output quantises them into
//                visible steps, and a fraction of a quantum of noise makes the step boundary
//                dither across a few pixels instead of falling on one line. Costs three multiplies.
//
// To swap the TONE MAPPER itself, replace the body of acesToneMap() (e.g. Reinhard
// `x / (1.0 + x)`, or a filmic/AgX curve) -- everything else stays the same. To change the sky/sun
// look with the time of day, that lives in sharedShaders/pjv_sun_sky.sc, not here.
//
// Input (FBO 9): 0 accumColor (anti-aliased HDR).
// =============================================================================

#include <bgfx_shader.sh>

// taaColor is now at OUTPUT resolution and is read 1:1 -- the magnify that used to live in this pass
// moved ahead of taa into upscale.frag, so that the edges it reconstructs are averaged by the temporal
// filter instead of being created after it. All this pass does is grade.
SAMPLER2D(accumColor, 0);

// FBO 9[1]: taa's position target -- xyz the face centre, a = camDist (a < 0 => sky). Already bound
// by the framebuffer, just never declared until the shaft magnify below needed a full-resolution
// depth to reject half-resolution taps against.
SAMPLER2D(taaPos, 1);

// FBO 11: the crepuscular shafts, at half the render resolution and already tinted by the sun's own
// colour -- see godrays.frag. It arrives here, after taa, rather than being folded into compose for
// two reasons. It is a smooth low-frequency field with no edges for taa to resolve, so running it
// through the temporal filter would cost a full-resolution pass of work to change nothing; and
// adding it here keeps it OUT of the history, which matters because taa's history is what would
// otherwise smear a shaft across the screen as the camera turns.
//
// It is added in LINEAR HDR, before the tone map below, which is the only place it can go and be
// right: shafts are light, so they must roll off through the same curve as every other bright thing
// in the frame rather than being pasted onto an already-compressed image.
SAMPLER2D(godrays, 2);

// FBO 13: the bloom, at an eighth of the render resolution on each axis. Added here alongside the
// shafts and for the same reasons -- in LINEAR HDR before the tone map, so it rolls off through the
// same curve as everything else rather than being pasted onto an already-compressed image, and after
// taa, so it stays out of the history.
//
// Bilinear magnification from an eighth is not a compromise here either: the signal was blurred at
// that resolution, so there is no detail left in it to lose, and the smoothness of the upsample is
// exactly the smoothness the halo wants.
// TWO SCALES. bloomTight is a quarter resolution and supplies the core; bloomWide is an eighth and
// supplies the tail. Real bloom is a bright tight centre with a faint wide skirt, and a single
// Gaussian is neither -- at one scale a small emitter magnifies into a flat-edged diamond, which is
// what "it stands out as a square" was. See the note at the top of bloomdown.frag.
SAMPLER2D(bloomTight, 3);
SAMPLER2D(bloomWide,  4);

// FBO 15: the traced volumetric shafts, filtered along the shaft direction. Zero unless the `.` key
// has them on -- volumetric.frag returns black immediately when volParams.x is 0, so the whole thing
// costs nothing while it is off.
SAMPLER2D(volumetric, 5);
uniform vec4 volParams;

// x = the debug view selector (see the P key in main.cpp). Declared directly rather than pulled in
// with pjv_sun_sky.sc, which is where the other passes get it: this pass only grades, and has no
// other business with the light rig.
uniform vec4 renderParams;

// The resolution of each bound input, so the magnifying fetches below know what grid they are
// reconstructing from. Slots follow the bind order: 2 godrays, 3 bloomTight, 4 bloomWide,
// 5 volumetric.
uniform vec4 passInputRes[8];

// .y is the `/` diagnostic cycle. Mode 5 exists because every earlier mode marked the G-BUFFER, and
// the half-resolution shaft terms are added HERE -- after compose, after taa -- so a defect in them
// is downstream of every instrument in this renderer and shows up in none of them. Instrumenting the
// pass that writes the pixel, rather than the pass suspected of causing it, is the lesson.
uniform vec4 animDebug;

// ---- MAGNIFYING A LOW-RESOLUTION BUFFER WITHOUT SHOWING ITS GRID -------------------------------
// Every additive term this pass reads is computed at a fraction of the frame: god rays and the traced
// volumetric at a half, the bloom at a quarter and an eighth. That is what makes them affordable, and
// it is invisible on a still image -- the quantisation is constant, so it just reads as a slightly
// soft glow.
//
// Under CAMERA MOTION it stops being invisible, and the reason is worth stating exactly, because it
// is not simply "the resolution is low". The low-resolution grid is anchored to the SCREEN. The world
// slides across it while it stays put, so every feature has to cross the same fixed lattice of texel
// boundaries -- and a BILINEAR magnify has a derivative discontinuity at each of those boundaries. A
// smooth ramp reconstructed bilinearly is a chain of straight segments meeting at kinks, and the
// kinks sit at fixed screen positions. Still, they are a static, unnoticeable faceting. Moving, the
// image crawls over them and the eye reads exactly what was reported: the same parts of the frame
// changing, locked to the screen rather than to the scene.
//
// A CUBIC B-SPLINE reconstruction has no such boundaries. It is C2 continuous everywhere, so there is
// no crease anywhere in the magnified image for motion to reveal -- the result swims smoothly instead
// of stepping. B-spline rather than Catmull-Rom deliberately: Catmull-Rom is interpolating and
// sharpens, which on a signal that has no detail at this scale means ringing and overshoot around
// bright shafts. B-spline is approximating, so it smooths slightly, which is precisely the right
// behaviour when the source genuinely has no more information in it.
//
// FOUR TAPS, not sixteen. The classic trick: a cubic kernel over a 4x4 neighbourhood factors into
// four BILINEAR fetches placed at weighted offsets, letting the sampler do the inner interpolation.
// So this costs four fetches per buffer rather than one -- against a pass that was doing almost
// nothing, on a signal where it removes the single most visible artefact left in the frame.
// ---- MAGNIFYING A HALF-RESOLUTION TERM WITHOUT BLEEDING IT ACROSS A DEPTH EDGE ----------------
// The shaft buffers are the one pair of inputs here that must NOT be magnified blind, and the reason
// is specific to how they are produced. The bloom below is genuinely band-limited -- it was blurred
// at the resolution it is stored at, so a smooth filter reconstructs it. The screen-space god-ray
// march is smooth too. But volumetric.frag fires three voxel shadow rays per pixel, so its output
// carries OCCLUSION DETAIL correlated with the geometry, at frequencies half resolution cannot hold.
//
// Magnified smoothly and added to the full-resolution frame, that detail lands where the filter puts
// it rather than where the geometry is. Over thin foliage the inscatter from a distant tap washes
// across a near surface; where the geometry is sub-pixel the wash covers it entirely, and the pixel
// becomes fog-coloured -- indistinguishable from a hole with sky behind it. It pulses, because the
// geometry moves against a fixed half-resolution grid, and it is worst at distance, where the
// geometry is thinnest relative to a half-res texel. On fire it reads as refraction.
//
// BILATERAL, not nearest-depth. Picking the single closest-depth tap reintroduces half-resolution
// blockiness everywhere, to fix an artefact that only exists at depth edges. Bilinear weights scaled
// by a depth-similarity term keep the smooth reconstruction where depth is continuous and fall back
// toward the matching taps only where it is not.
//
// The tolerance is PROPORTIONAL to depth. A fixed world epsilon is wrong at both ends: far too tight
// out where the artefact actually lives, and far too loose up close. 5% reads as "the same surface".
//
// The weights are a soft reciprocal rather than a hard cutoff, so they never all reach zero and the
// normalisation cannot divide by nothing -- no guard needed, and no discontinuity introduced by the
// filter that is here to remove one.
#define SHAFT_UPSAMPLE_DEPTH_TOLERANCE 0.05

vec3 sampleShaftDepthAware(sampler2D tex, vec2 uv, vec4 texRes, float refDepth) {
    vec2 res   = texRes.xy;
    vec2 texel = texRes.zw;

    vec2 p    = uv * res - 0.5;
    vec2 i    = floor(p);
    vec2 f    = p - i;
    vec2 base = (i + 0.5) * texel;

    vec4 s00 = texture2D(tex, base);
    vec4 s10 = texture2D(tex, base + vec2(texel.x, 0.0));
    vec4 s01 = texture2D(tex, base + vec2(0.0, texel.y));
    vec4 s11 = texture2D(tex, base + texel);

    float tol = max(abs(refDepth) * SHAFT_UPSAMPLE_DEPTH_TOLERANCE, 1e-3);
    float w00 = (1.0 - f.x) * (1.0 - f.y) / (1.0 + abs(s00.a - refDepth) / tol);
    float w10 =        f.x  * (1.0 - f.y) / (1.0 + abs(s10.a - refDepth) / tol);
    float w01 = (1.0 - f.x) *        f.y  / (1.0 + abs(s01.a - refDepth) / tol);
    float w11 =        f.x  *        f.y  / (1.0 + abs(s11.a - refDepth) / tol);

    float sum = w00 + w10 + w01 + w11;
    return (s00.rgb * w00 + s10.rgb * w10 + s01.rgb * w01 + s11.rgb * w11) / max(sum, 1e-6);
}

vec3 sampleBSpline(sampler2D tex, vec2 uv, vec4 texRes) {
    vec2 res = texRes.xy;
    vec2 inv = texRes.zw;

    vec2 p = uv * res - 0.5;
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;

    // The four cubic B-spline basis functions, evaluated at the sub-texel position.
    vec2 w0 = (1.0 / 6.0) * (-f3 + 3.0 * f2 - 3.0 * f + 1.0);
    vec2 w1 = (1.0 / 6.0) * (3.0 * f3 - 6.0 * f2 + 4.0);
    vec2 w2 = (1.0 / 6.0) * (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0);
    vec2 w3 = (1.0 / 6.0) * f3;

    // Each PAIR of taps is folded into one bilinear fetch positioned so the hardware's own
    // interpolation reproduces their weighted sum. g is the pair's total weight; h is where to put
    // the fetch. Neither g can be zero -- both stay within [1/6, 5/6] over f in [0,1] -- so the
    // divisions are safe without a guard.
    vec2 g0 = w0 + w1;
    vec2 g1 = w2 + w3;
    vec2 h0 = (w1 / g0) - 1.0;
    vec2 h1 = (w3 / g1) + 1.0;

    vec2 uv0 = (i + h0 + 0.5) * inv;
    vec2 uv1 = (i + h1 + 0.5) * inv;

    vec3 t00 = texture2D(tex, vec2(uv0.x, uv0.y)).rgb;
    vec3 t10 = texture2D(tex, vec2(uv1.x, uv0.y)).rgb;
    vec3 t01 = texture2D(tex, vec2(uv0.x, uv1.y)).rgb;
    vec3 t11 = texture2D(tex, vec2(uv1.x, uv1.y)).rgb;

    // The basis is a partition of unity, so these weights already sum to one and no normalisation is
    // needed -- which is also why it cannot brighten or darken what it magnifies.
    return (t00 * g0.x + t10 * g1.x) * g0.y
         + (t01 * g0.x + t11 * g1.x) * g1.y;
}

// ---- Colour grading knobs ---------------------------------------------------
#define EXPOSURE    0.8
#define TINT        vec3(1.02, 1.00, 0.97)
#define CONTRAST    1.1
#define SATURATION  1.3
#define VIGNETTE    0.20
#define DITHER      1.0
// How much of the blurred bright-pass is added back. The bloom's own threshold and width live in
// bloomdown.frag and bloomblur.frag; this is only the mix. Emissive voxels are usually the brightest
// things in a voxel scene, so this is what makes them read as light sources rather than bright pixels.
//
// This was 0.9, then 0.15, and neither number was the real fault on its own -- see BLOOM_THRESHOLD in
// bloomdown.frag for what was actually being added at 0.9, and the two-scale note there for why the
// shape was wrong at 0.15.
#define BLOOM_STRENGTH 0.08
// Balance between the two scales. 1.0 is core only (tight and hard), 0.0 is tail only (the flat
// diamond the single-scale version produced). Toward the core, because the core is what was missing
// and the tail is what was too strong.
#define BLOOM_CORE     0.65
// How much of the screen-space god rays survives when the traced volumetric ones are switched on.
// Not zero: the depth-buffer march is smooth everywhere and sees the broad glow the sparse traced
// samples are weakest at. Not one either -- both estimate the same inscatter, so two full copies
// would just double the sunward haze.
//
// Raised from 0.35, which combined with a too-dim volumetric pass to make pressing the key REDUCE the
// visible shafts -- the cheap pass was cut to a third and what replaced it was fainter still.
#define GODRAY_SS_MIX_WITH_VOLUMETRIC 0.5


vec3 acesToneMap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Quartic falloff from the frame centre: flat across the middle two-thirds, then dropping into the
// corners. Quartic rather than the usual quadratic because a quadratic starts falling immediately and
// reads as an exposure error near the centre; this one is imperceptible until it is meant to be seen.
float vignetteAt(vec2 uv) {
    vec2  q  = uv - 0.5;
    float r2 = dot(q, q) * 2.0;                     // ~1 at the corners of a square frame
    return 1.0 - VIGNETTE * r2 * r2;
}

vec3 gradeToDisplay(vec3 hdr, vec2 uv) {
    hdr *= EXPOSURE * TINT * vignetteAt(uv);
    return pow(acesToneMap(hdr), vec3(1.0 / 2.2));
}

// Interleaved gradient noise -- Jorge Jimenez's, and the reason for choosing it over a sin/fract hash
// is that its output is well distributed over any small NEIGHBOURHOOD rather than merely over the
// whole frame, which is exactly what a dither has to be to break up a band.
//
// FRAME-INDEPENDENT, deliberately, like the AO rotation in compose.frag. A dither that changes every
// frame is the textbook version and averages to nothing, but this renderer's whole claim is that two
// frames of a still scene are bit-identical; per-frame noise here would be the only thing in the
// pipeline that shimmers on a static image, and it would do it in the flat sky where it is most
// visible. Static noise costs nothing and cannot flicker.
float interleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
    // ---- TAA AGE VIEW (P key, renderParams.x == 2) ----------------------------------------------
    // The companion to compose's GI age view, and the one that settles what a silhouette edge is
    // actually doing. taa.frag writes its own running-mean age into this target's alpha.
    //
    // Why it matters at an edge: a pixel on a true silhouette straddles two DIFFERENT surfaces, so
    // the value it wants is a coverage-weighted mix of them. Nothing upstream can compute that --
    // the GI is per face, and spatially blending across the silhouette would just be a leak. The
    // sub-pixel jitter plus this running mean IS the mechanism that resolves it, so if edges still
    // crawl, the question is whether this age holds up along them or collapses. Dark threads on
    // silhouettes here mean taa is rejecting its history and the jitter is all cost and no benefit;
    // bright ones mean taa is averaging and the flicker is entering after it.
    //
    // Same ramp as the GI view: black = 1, red ~8, gold ~16, green ~32, white = 64 (STILL_MAX_AGE).
    // BOUNDED at the top, not just the bottom. This was `> 1.5`, which claimed every mode above it and
    // would have swallowed the AO view compose adds at 3 -- compose would render it and this pass would
    // paint over it with an age ramp.
    if (renderParams.x > 1.5 && renderParams.x < 2.5) {
        float t = clamp(abs(texture2D(accumColor, v_texcoord0).a) / 64.0, 0.0, 1.0);
        gl_FragColor = vec4(clamp(t * 4.0, 0.0, 1.0),
                            clamp(t * 2.0, 0.0, 1.0),
                            clamp((t - 0.5) * 2.0, 0.0, 1.0), 1.0);
        return;
    }

    // ---- UPSCALE VIEW PASSTHROUGH (P key, renderParams.x == 4) ----------------------------------
    // upscale.frag paints its own diagnostic and it is a CLASSIFICATION, not a radiance -- so it must
    // reach the screen unmodified. Tone mapping and the trim below would slide the three flat colours
    // around and make "green" a judgement call. taa sits in between and averages it, which is harmless
    // and in fact useful: the view is constant per pixel over time, so any part of it that shimmers is
    // the reconstruction changing its mind frame to frame.
    if (renderParams.x > 3.5) {
        gl_FragColor = vec4(texture2D(accumColor, v_texcoord0).rgb, 1.0);
        return;
    }

    vec3 hdr = texture2D(accumColor, v_texcoord0).rgb;

    // The shafts, bilinearly magnified from half resolution -- which is the one place in this
    // renderer where a plain bilinear upsample is exactly right rather than a compromise. There is
    // nothing in the signal above the resolution it was computed at, so interpolating between two
    // samples of it reconstructs it rather than inventing anything, and no G-buffer is being read
    // through the filter. Skipped for every debug view: views 1 and 3 are drawn by compose and reach
    // this pass through the normal grading path, so without this test the age ramp and the AO map
    // would both get a sun glow painted across them.
    if (renderParams.x < 0.5) {
        // ---- The two god-ray paths, combined ----------------------------------------------------
        // They are complements rather than alternatives, so with the volumetric on BOTH are kept and
        // the screen-space one is turned down instead of off. Each covers the other's blind spot: the
        // traced pass sees occluders that are off screen or hidden behind nearer geometry, which the
        // depth-buffer march is structurally unable to; the depth-buffer march is smooth and free of
        // sampling noise everywhere, including the places three shadow rays per pixel are thin.
        //
        // Turned DOWN rather than left alone because both are estimating the same inscatter -- adding
        // two full-strength copies would simply double the haze on the sunward half of the frame.
        // The full-resolution depth this pixel actually belongs to, which is what decides whether a
        // half-resolution tap is describing the same surface. taaPos carries it in alpha (curD, which
        // is gPos.a) at output resolution, and it was already bound here -- see sampleShaftDepthAware.
        float refDepth = texture2D(taaPos, v_texcoord0).a;

        vec3 shafts = sampleShaftDepthAware(godrays, v_texcoord0, passInputRes[2], refDepth);
        if (volParams.x > 0.5) {
            shafts = shafts * GODRAY_SS_MIX_WITH_VOLUMETRIC
                   + sampleShaftDepthAware(volumetric, v_texcoord0, passInputRes[5], refDepth);
        }
        // ---- MODE 5: THE HALF-RESOLUTION SHAFT TERM, ALONE ----------------------------------
        // Both terms are magnified from half resolution and ADDED to a full-resolution image with no
        // reference to depth. That is sound for the screen-space march, which is smooth by
        // construction -- but volumetric.frag fires three shadow rays per pixel against the voxel
        // scene, so its output carries occlusion detail correlated with THIN GEOMETRY, at frequencies
        // the half-resolution grid cannot hold. Magnified smoothly and added, that detail lands
        // wherever the filter puts it rather than where the geometry is: a fog-coloured wash over
        // sub-pixel foliage, pulsing as the geometry moves against the fixed half-res grid, and
        // strongest at distance where the geometry is thinnest. On fire, which is bright and moving,
        // it reads as refraction.
        //
        // Shown on its own so the question is settled by looking rather than by argument: if the holes
        // are visible in this term by itself, they are made here.
        if (int(animDebug.y + 0.5) == 5) {
            gl_FragColor = vec4(shafts * 4.0, 1.0);
            return;
        }
        hdr += shafts;
        // The tight half carries the alpha channel bloomdown used for depth; sampleBSpline returns
        // rgb only, so that never reaches the frame.
        vec3 tight = sampleBSpline(bloomTight, v_texcoord0, passInputRes[3]);
        vec3 wide  = sampleBSpline(bloomWide,  v_texcoord0, passInputRes[4]);
        hdr += mix(wide, tight, BLOOM_CORE) * BLOOM_STRENGTH;
    }

    vec3 col = gradeToDisplay(hdr, v_texcoord0);

    // Display-referred trim: contrast around mid-grey, then saturation about luma.
    col = (col - 0.5) * CONTRAST + 0.5;
    float luma = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(luma), col, SATURATION);

    // Debanding, after every other operation and before the clamp -- it has to be the last thing,
    // because it is sized to the OUTPUT quantum and anything applied afterwards would rescale it.
    // Centred on zero so it does not shift the mean brightness.
    col += (interleavedGradientNoise(gl_FragCoord.xy) - 0.5) * (DITHER / 255.0);

    gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
