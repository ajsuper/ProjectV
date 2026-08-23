$input v_color0
$input v_texcoord0

// =============================================================================
// godrays.frag  --  Crepuscular rays, in screen space, at a quarter of the pixels.
//
// An emissive voxel's halo briefly lived here too, on the argument that it is the same phenomenon
// with a different source. It was moved out to a proper bloom (bloomdown/bloomblur): a halo wants a
// WIDE, smooth falloff, and a single-pass kernel at this resolution cannot produce one -- every
// attempt read as either speckle or blocks. Shafts are different in kind: they are a directional
// march whose structure IS the effect, so they belong here and look right here.
//
// The fog in pjv_atmosphere.sc adds inscattered sunlight along every view ray as if the sun reached
// every parcel of air on it. It does not: the air behind a tree is in the tree's shadow, and the air
// between its leaves is not. The visible consequence of that difference is the shaft -- the bright
// wedge of lit air next to a dark one -- and it is entirely absent from an unshadowed fog term,
// which is why haze alone still reads as flat.
//
// This pass computes the correction. It is the same quantity the fog computes, restricted to the
// part of the inscatter that came from the SUN along an unoccluded path, and it is added on top of
// the fog rather than replacing it. A scene with no air in it still gets neither -- `air` below is
// measured from the fog's own optical depth, so turning the fog off takes the shafts with it, which
// is correct rather than tidy. What is NOT shared any more is the two effects' STRENGTH: the shafts
// have their own gain (volParams.w) so that thickening the haze and brightening the shafts are two
// separate requests. See the note on that uniform in volumetric.frag.
//
// -----------------------------------------------------------------------------
// THE METHOD, AND WHAT IT IS AN APPROXIMATION OF
// -----------------------------------------------------------------------------
// The honest integral marches the view ray and asks, at each step, whether that point in space can
// see the sun -- one shadow ray per step per pixel, which is the most expensive thing this renderer
// could possibly do. The standard substitute (Mitchell, GPU Gems 3) makes one observation: every
// point on a view ray that ends at the sun projects onto the SAME LINE in screen space -- the line
// from this pixel to the sun's screen position. So marching that 2D line and asking "is this texel
// sky" samples the same occlusion the 3D march would, using the depth buffer that already exists,
// at the cost of one texture fetch per step.
//
// What it gets wrong is what it cannot see: an occluder off the side of the frame casts no shadow,
// and geometry nearer than the sampled surface is invisible to it. Both failures are graceful --
// they make a shaft that should be dark bright -- and neither is visible without an A/B against a
// reference. What it gets RIGHT is the thing that matters: shafts appear between the leaves of a
// canopy and beside a silhouette, they move correctly as the camera turns, and they cost about a
// quarter of a millisecond.
//
// RESOLUTION. Written at half the render target on each axis, so a quarter of the pixels, because
// the output is a smooth low-frequency field -- there is nothing in a light shaft above the
// resolution of the blur that makes it. It is read back bilinearly by display.frag, where the
// upsample is exactly right rather than a compromise: interpolating between two samples of a smooth
// field is what interpolation is for. (Contrast upscale.frag, which cannot interpolate a G-buffer.)
//
// DETERMINISTIC. The march pattern is fixed and the dither that breaks its banding is hashed from
// the pixel and NOT the frame, so two frames of a still scene are bit-identical here as everywhere
// else. This pass is upstream of taa, but it does not need taa, and that is the point.
//
// Input (FBO 1): 0 gPos (a < 0 => sky). The rest of the G-buffer arrives with the framebuffer and
// is left undeclared.
// Output (FBO 11): 0 the shaft radiance, already tinted by the sun's colour, for display.frag to add
// into the linear HDR frame before it tone maps.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos,    0);   // FBO1[0]: xyz world pos, a = camDist (a < 0 => sky)

// This pass addresses no probe grid, but pjv_cascade_common.sc requires both to be named rather than
// guessed at, so both are its own target.
#define CASCADE_SCREEN_RES passTargetRes.xy
#define CASCADE_ATLAS_RES  passTargetRes.xy
#include <pjv_cascade_common.sc>
#include <pjv_atmosphere.sc>

// Shared with volumetric.frag, which is the other half of this effect. Only .w is read here -- the
// GOD-RAY INSCATTER STRENGTH -- and the uniform is already uploaded every frame, so taking it is
// free. See the note on the uniform in volumetric.frag for why the shafts no longer take their gain
// off fogParams.w, and FrameState::godrayScale in main.cpp for the knob.
uniform vec4 volParams;

// ---- Knobs ------------------------------------------------------------------------------------
// Steps along the screen-space line. This is the whole cost of the pass: one gPos fetch each, at a
// quarter of the pixels. 48 is enough that the dither below has something to dither BETWEEN; much
// below 32 and the shafts start to show the individual taps as rungs.
#define GODRAY_SAMPLES 48
// How far along the line to the sun the march reaches, as a fraction. Short of 1.0 on purpose: the
// last stretch approaching the sun is where every pixel's line converges on the same few texels, so
// it contributes the same value to everything and buys nothing but a bright wash.
#define GODRAY_DENSITY 0.82
// Per-step falloff of a sample's weight. Samples near THIS pixel matter most, which is what makes a
// shaft appear to be anchored to the occluder that casts it rather than smeared uniformly along its
// length.
#define GODRAY_DECAY 0.965
// Radial falloff away from the sun's screen position, in screen widths. Without it the normalised
// march below would paint the entire open sky at full strength, because open sky is 100% sky the
// whole way along. This is what confines the effect to the sun's neighbourhood.
#define GODRAY_FALLOFF 3.0
// Contrast on the sky fraction. Above 1 it widens the gap between a line that is mostly sky and one
// that is partly occluded, which is what separates individual shafts out of a general glow.
#define GODRAY_CONTRAST 1.7
// Overall gain, applied on top of volParams.w. This is the SHAPE constant -- the fixed part of how
// bright a fully-lit shaft is relative to the medium -- and volParams.w is the runtime knob that
// rides on it. Shafts still scale with how hazy the scene is, but through `air` below, which is a
// per-pixel measurement of the medium rather than a second copy of the fog's strength dial.
#define GODRAY_GAIN 2.6
// How far off screen the sun may be before the shafts are gone. They must fade rather than stop: a
// hard cut at the frame edge would make the whole effect blink as the camera turns.
#define GODRAY_EDGE_FADE_START 1.0
#define GODRAY_EDGE_FADE_END   2.4

// Is this texel sky? One fetch, and a binary answer, which is the right shape for it -- an occluder
// either blocks the sun or it does not, and every softening this needs comes from the 48 taps and
// the dither rather than from a soft test.
//
// SNAPPED to the input's texel grid, for the reason everything in this renderer snaps: the G-buffer
// filters bilinearly, and a tap between a sky texel (camDist -1) and a surface texel at 200 returns a
// positive depth belonging to no surface. Here that reads as "occluded", so unsnapped taps would
// quietly erode every thin gap in a canopy -- exactly the gaps the shafts come through.
//
// CLAMPED to the frame rather than rejected outside it. When the sun is off screen the near end of
// every march runs past the edge, and returning zero there would draw a hard line across the image
// where the marches start falling off it. Clamping instead extends the edge texel, which is wrong in
// a way nobody can see and continuous in a way everybody would.
float skyAt(vec2 uv) {
    vec2 snapped = pjvSnapToTexel(clamp(uv, vec2(0.0), vec2(1.0)), passInputRes[0].xy);
    return texture2D(gPos, snapped).a < 0.0 ? 1.0 : 0.0;
}

// Interleaved gradient noise, as display.frag uses for its debanding, and frame-independent for the
// same reason. Here it offsets each pixel's march by a fraction of one step, so the taps of
// neighbouring pixels interleave and the concentric rings a fixed pattern would produce dissolve
// into a texture too fine to see.
float godrayDither(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
    // The sun's screen position. worldToUV divides through by the depth along the view axis, so any
    // point on the ray gives the same answer and a point one unit along SUN_DIR is the cheapest one
    // to name. `valid` is ignored on purpose -- an off-screen sun still casts shafts INTO the frame,
    // and the edge fade below is what handles it. What cannot be ignored is the sun being BEHIND the
    // camera, where the projection is meaningless; that is the `facing` test.
    vec3  forward = normalize(cameraDir.xyz);
    float facing  = dot(forward, SUN_DIR);
    bool  unused;
    vec2  sunUV   = worldToUV(cameraPos.xyz + SUN_DIR, cameraPos.xyz, cameraDir.xyz,
                              passTargetRes.xy, FOV, unused);

    vec2 uv = v_texcoord0;

    // No air, nothing to scatter with: neither shafts nor emissive halo, and the whole pass is a
    // constant. This one early-out still applies to everything.
    //
    // fogParams.x, not fogParams.w -- the question is whether there is any AIR, which is the
    // density. Turning the god-ray gain to zero gets the same shortcut.
    if (fogParams.x <= 0.0 || volParams.w <= 0.0) {
        // See the note at the final write: alpha carries depth, and -1 means "no surface".
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    // ---- How much air is in front of this pixel -------------------------------------------------
    // The factor that keeps both terms from being a screen-space smear. Scattered light is lit AIR;
    // a pixel with a wall a metre in front of the camera has none of it, however bright the sky or an
    // emitter behind that wall may be. fogInscatterFraction answers exactly that from the same
    // optical depth the fog uses, so this pass and compose.frag cannot disagree about where the air is.
    //
    // gPos.xyz is usable in both cases without a branch: gbuffer.frag writes the sky's placeholder as
    // `origin + direction * 1e5`, so the direction back out of it is the true view direction either
    // way. Only the DISTANCE has to be substituted.
    vec4  gp   = texture2D(gPos, pjvSnapToTexel(uv, passInputRes[0].xy));
    vec3  dir  = normalize(gp.xyz - cameraPos.xyz);
    float dist = gp.a < 0.0 ? FOG_SKY_DISTANCE : gp.a;
    float air  = fogInscatterFraction(dir, dist);

    // Two more ways to have no SHAFTS specifically: the sun behind the camera, or the sun set. Each
    // is most of the remaining work, so they are worth taking before the march.
    float faceFade = smoothstep(0.0, 0.30, facing);
    if (faceFade <= 0.0) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    // ---- The march ------------------------------------------------------------------------------
    // From this pixel toward the sun, accumulating how much of the line was sky. Normalised by the
    // same weights it accumulates, so the result is a decay-weighted FRACTION in [0,1] and is
    // independent of GODRAY_SAMPLES and GODRAY_DECAY -- change either and the picture does not
    // change brightness, which is what makes them tunable separately from the gain.
    vec2  delta = (uv - sunUV) * (GODRAY_DENSITY / float(GODRAY_SAMPLES));
    vec2  p     = uv - delta * godrayDither(gl_FragCoord.xy);
    float decay = 1.0;
    float lit   = 0.0;
    float total = 0.0;
    for (int i = 0; i < GODRAY_SAMPLES; i++) {
        p     -= delta;
        lit   += skyAt(p) * decay;
        total += decay;
        decay *= GODRAY_DECAY;
    }
    float shaft = pow(lit / max(total, 1e-5), GODRAY_CONTRAST);

    // ---- What that fraction is worth ------------------------------------------------------------
    // Radially, away from the sun. Aspect-corrected so the falloff is a circle on screen rather than
    // an ellipse -- passTargetRes carries this pass's own size, and it is the same aspect as the
    // window because every relative target is scaled on both axes together.
    float aspect = passTargetRes.x / max(passTargetRes.y, 1.0);
    float radial = exp(-length((uv - sunUV) * vec2(aspect, 1.0)) * GODRAY_FALLOFF);

    // And by how far off screen the sun has gone, measured in half-frames so 1.0 is the frame edge.
    vec2  off      = abs(sunUV - 0.5) * 2.0;
    float edgeFade = 1.0 - smoothstep(GODRAY_EDGE_FADE_START, GODRAY_EDGE_FADE_END,
                                      max(off.x, off.y));

    // Tinted by the sun's own colour, so the shafts warm and redden with the day cycle along with
    // everything else, and scaled by the medium's scattering strength (`air`, computed above and
    // computed above) so they are a property of the atmosphere rather than a decal on top of it.
    vec3 shafts = SUN_COLOR * (shaft * radial * edgeFade * faceFade * air *
                               volParams.w * GODRAY_GAIN);

    // ---- ALPHA CARRIES THE DEPTH THIS PIXEL WAS COMPUTED AT -----------------------------------
    // display.frag magnifies this buffer from half resolution and ADDS it to the full-resolution
    // frame. A depth-blind filter blending across a depth discontinuity puts a distant pixel's
    // inscatter onto near geometry, and over sub-pixel foliage that wash covers the geometry
    // entirely -- a fog-coloured hole that pulses as the geometry moves against the fixed half-res
    // grid. Recording the depth here is what lets the magnify reject a tap that belongs to a
    // different surface; the channel was carrying an unread 1.0.
    gl_FragData[0] = vec4(shafts, gp.a);
}
