$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag  --  Pass 4 of the scene editor's viewport renderer.
//
// Copies the accumulated image to the viewport texture, deliberately doing
// nothing to it. (In the standalone previewer this pass targets the back buffer;
// here it targets FBO 3, which the editor hands to ImGui as the image drawn in
// the Viewport panel. Everything below is otherwise unchanged.) The lit renderers ACES tone-map and gamma-encode here because they produce
// open-ended HDR radiance; this one produces reflectance, which is already in
// [0, 1] and is exactly the value stored in the scene's material palette
// (fetchVoxelColor decodes R10G10B10 straight to 0..1).
//
// Tone mapping that would desaturate and lift it, and a gamma curve would wash
// it out relative to the source texture the voxelizer sampled -- either one
// means the previewer is no longer showing you the colour that is in the file.
// So: straight through.
//
// If your scene's palette was authored in linear rather than sRGB, set
// PREVIEW_APPLY_GAMMA to 1 to encode on the way out.
//
// --- Except with the advanced preview on -------------------------------------
//
// That toggle is the one thing upstream that produces values above 1. Emission is
// radiance and its strength control reaches 245; a reflection off a glossy surface
// lands wherever the lobe's weight puts it. Left alone those clip, and clipping is
// the worst possible answer here: every channel that overshoots saturates to the
// same 1.0, so a bright red emitter and a bright blue one both arrive on screen as
// a white blob -- and "what colour is this emitter" is precisely the question the
// toggle was added to answer.
//
// So the overshoot is rolled off instead. What is NOT done is the ACES-plus-gamma
// pipeline Render mode's display pass uses, even though it is sitting right there:
// that curve lifts and desaturates the whole image, including the 99% of pixels
// that are plain stored reflectance and were correct already. Toggling the preview
// would then appear to change every material in the scene, which is exactly the
// misreading this file's straight-through rule exists to prevent.
//
// The rolloff below is built the other way round -- from that rule outwards:
//
//   IDENTITY BELOW THE KNEE. A pixel whose brightest channel is under KNEE passes
//   through untouched, bit for bit. Stored reflectance is what lives down there,
//   so the guarantee this file makes about it survives the toggle intact.
//
//   HUE PRESERVED ABOVE IT. The curve is applied to the brightest channel and the
//   others are scaled by the same ratio, rather than each being mapped on its own.
//   Per-channel compression pulls an overshooting colour towards white as it
//   brightens (the hue shift ACES exists to soften and Reinhard is notorious for);
//   scaling by the peak keeps a red emitter red however far past 1 it goes, which
//   is the entire content of the answer.
//
//   ASYMPTOTIC, NOT CLAMPED. Above the knee the remaining headroom is spent on an
//   exponential approach to 1.0, so there is no second brightness at which things
//   start clipping after all. An emitter at strength 200 and one at strength 20 are
//   both near the top of the range and still distinguishable in hue, which is as
//   much as an 8-bit target can be asked for.
//
// Input (FBO 2): slot 0 = accumColor.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(accumColor, 0);
// x = advanced preview. Only the rolloff below reads it, and only to stay out of the way when it is
// off: with no emission and no reflection upstream there is nothing above 1 to roll off, and running
// the curve anyway would put a knee in an image that has no need of one.
uniform vec4 previewSettings;

#define PREVIEW_APPLY_GAMMA 0

// Where the rolloff starts. High enough that it is above anything a scene's reflectance is doing --
// a stored albedo of 0.85 is already an unusually bright material -- and low enough to leave a
// quarter of the range as headroom for the curve to be smooth in. Lowering it would start bending
// colours that are correct; raising it leaves too little room and the approach to 1.0 gets abrupt.
#define GLOW_ROLLOFF_KNEE 0.75

void main() {
    vec3 color = texture2D(accumColor, v_texcoord0).rgb;

    if (previewSettings.x > 0.5) {
        // The brightest channel decides for all three. max() with the knee rather than a branch:
        // below it the ratio below is exactly 1.0, so the identity case falls out of the arithmetic
        // instead of needing its own path.
        float peak = max(color.r, max(color.g, color.b));
        if (peak > GLOW_ROLLOFF_KNEE) {
            float headroom = 1.0 - GLOW_ROLLOFF_KNEE;
            // exp() approaches the ceiling without reaching it, and its slope at the knee is 1 --
            // the same slope the identity below has -- so the two meet without a visible crease
            // across the transition.
            float rolled = GLOW_ROLLOFF_KNEE +
                           headroom * (1.0 - exp(-(peak - GLOW_ROLLOFF_KNEE) / headroom));
            color *= rolled / peak;
        }
    }

#if PREVIEW_APPLY_GAMMA
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
#endif

    gl_FragColor = vec4(color, 1.0);
}
