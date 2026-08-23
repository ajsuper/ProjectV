$input v_color0
$input v_texcoord0

// =============================================================================
// volumetric.frag  --  God rays with REAL shadow rays, one per sample, in world space.
//
// The companion to godrays.frag rather than its replacement, and the two fail in opposite directions,
// which is why running both is worth a key.
//
//   SCREEN SPACE (godrays.frag) marches the line from a pixel to the sun through the DEPTH BUFFER. It
//   is cheap and completely smooth, and it can only see what is on screen: an occluder off the side
//   of the frame casts no shadow, and geometry in front of the sampled surface is invisible to it. So
//   it gets the broad glow right and the individual shafts approximately.
//
//   THIS traces an actual ray into the scene from a point in the air and asks whether the sun reaches
//   it. Nothing is off screen, nothing is hidden behind a nearer surface, and the shaft edges land
//   exactly where the geometry casting them is. What it is instead is NOISY -- one ray per sample is a
//   one-sample estimate of an integral -- and expensive, because that ray is a full scene traversal.
//
// -----------------------------------------------------------------------------
// WHY THE NOISE IS AFFORDABLE HERE
// -----------------------------------------------------------------------------
// Because a shaft is COHERENT ALONG ONE SCREEN DIRECTION. Every point on a view ray that ends at the
// sun projects onto the same line from this pixel to the sun's screen position, so the inscatter is
// nearly constant along that line and varies sharply only ACROSS it. volblur.frag exploits exactly
// that: it averages a long way along the line to the sun and barely at all perpendicular to it, so
// the noise integrates away while the shaft edges -- the only high-frequency detail in the signal --
// stay sharp. An isotropic blur wide enough to remove the same noise would destroy them, which is
// what "much higher definition" comes down to.
//
// THE ESTIMATOR. VOL_SAMPLES stratified points along the view ray, each contributing
//
//     density(x) * transmittance(eye -> x) * visibility(x)
//
// which is the inscattering integrand; multiplied by the segment length it is an unbiased estimate of
// the integral. density and transmittance come from pjv_atmosphere.sc, so this and the fog agree
// about the medium by construction -- the same reason godrays.frag reads them.
//
// DETERMINISTIC. The stratum offset is hashed from the pixel and NOT the frame, like everything else
// here, so two frames of a still scene are identical. The blur, not time, is what removes the noise.
//
// Input (FBO 1): 0 gPos (a < 0 => sky). Plus the scene textures, for the shadow rays.
// Output (FBO 14): 0 the inscattered radiance, unblurred and noisy, for volblur.frag.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos, 0);

#include <pjv_utils_DDA.sc>

#define CASCADE_SCREEN_RES passTargetRes.xy
#define CASCADE_ATLAS_RES  passTargetRes.xy
#include <pjv_cascade_common.sc>
#include <pjv_atmosphere.sc>
// The animation controls only. This pass's shadow ray does not ask for animation -- see volSunVisible
// -- so it needs no envelope, no field and no resolve.
#include <pjv_anim_controls.sc>

// x = enabled (0 skips the whole pass), y = gain, z = temporal filter off,
// w = GOD-RAY INSCATTER STRENGTH -- the shaft passes' own version of fogParams.w.
//
// Both shaft passes used to take their overall strength straight off fogParams.w, which is the
// FOG's inscatter and moves with the V key. That made one knob for two effects: thickening the haze
// also brightened the shafts, and there was no way to ask for strong shafts in light air or the
// reverse. The shafts keep their PHYSICAL dependence on the medium -- `air` below and in
// godrays.frag is computed from the fog's own optical depth, so no air still means no shafts -- but
// the gain is now its own number. See FrameState::godrayScale in main.cpp.
uniform vec4 volParams;

// ---- Knobs ------------------------------------------------------------------------------------
// Shadow rays per pixel. This IS the cost of the effect -- each one is a full scene traversal -- and
// it is why the whole thing is behind a key. Three is enough for the anisotropic blur to work with;
// one is visibly stripy even after it.
#define VOL_SAMPLES 3
// Step budget per shadow ray. A shaft's occluder is usually near the sample point, and a ray that
// runs out reports a MISS (== lit), so a short budget errs toward brighter rather than toward false
// shadow -- the safe direction, the same way PROBE_SHADOW_STEPS is argued.
#define VOL_SHADOW_STEPS 128u
// LOD for the shadow ray. Coarser than the primary on purpose: a shaft edge is a soft feature many
// pixels wide, so resolving its occluder to the voxel buys nothing visible and costs the deepest part
// of every traversal.
#define VOL_SHADOW_LOD      2
#define VOL_SHADOW_LOD_DIST 48
// How far along the view ray to integrate, as a multiple of the scene's fog height. Unbounded would
// have every sky pixel marching to FOG_SKY_DISTANCE, where the samples are so far apart that the
// estimate is meaningless; the haze has thinned to nothing well before then anyway.
#define VOL_MAX_DISTANCE (fogParams.y * 12.0)
// ---- THE SHAFTS GET THEIR OWN, GENTLER PHASE LOBE ----------------------------------------------
// The fog scatters with FOG_PHASE_G = 0.62, which is strongly forward-peaked: 0.856 straight at the
// sun against 0.025 at ninety degrees off it, a factor of thirty-four. That is right for the haze,
// where the sun's glare genuinely is concentrated around the sun -- and it makes shafts nearly
// invisible, because a shaft is normally LOOKED AT from the side. Measured against a lit surface of
// 0.46, the first version of this pass produced 0.013 to 0.067 at sixty degrees off the sun. The
// screen-space pass has no phase term at all, which is why it never had the problem.
//
// So this uses its own, much gentler lobe: 0.35 gives a peak-to-perpendicular ratio of about four
// instead of thirty-four, so a shaft stays visible from the side where it is actually seen. It is a
// deliberate departure from the fog's medium rather than an oversight -- a real atmosphere scatters
// by more than one mechanism and is nowhere near as forward-peaked as a single Mie lobe at 0.62.
#define VOL_PHASE_G 0.35

// Interleaved gradient noise, frame-independent, as everywhere else in this renderer. It offsets the
// stratified samples so neighbouring pixels sample different points along their rays -- which is what
// gives the blur something to average rather than a coherent pattern to smear.
float volDither(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Can the sun reach this point in the air? No surface, so no normal and no origin bias to apply --
// the sample point is already in open space by construction.
bool volSunVisible(vec3 p) {
    Ray shadow;
    shadow.origin    = p;
    shadow.direction = SUN_DIR;

    RayQuery rq = pjvPrimaryQuery(100u);
    rq.maxRaySteps         = VOL_SHADOW_STEPS;
    rq.startLOD            = 0u;
    rq.finishLOD           = uint(VOL_SHADOW_LOD);
    rq.distanceToFinishLOD = uint(VOL_SHADOW_LOD_DIST);

    // NO PJV_Q_ANIMATION, for the reason the probe's rays leave it off too: whether a blade is at its
    // rest position or a voxel from it does not change whether it casts a shadow into fog, and the
    // resolve is the expensive half. With animation off the geometry march draws animated voxels
    // where they are stored, so a blade still occludes -- it just is not asked which voxel it is in.
    return raySceneIntersectFrom(shadow, rq, 0.0).rayT < 0.0;
}

void main() {
    // ---- NO "IS THE SUN IN FRONT OF THE CAMERA" TEST, DELIBERATELY -----------------------------
    // There was one, briefly, copied from godrays.frag, and it was wrong twice over.
    //
    // It was wrong in PRACTICE because of what it actually gates on. With the sun high, SUN_DIR is
    // mostly vertical -- at the default elevation it is (0.66*ax, 0.75, 0.66*az) -- so
    // dot(forward, SUN_DIR) is dominated by 0.75 * forward.y. The test therefore reduces to "is the
    // camera pointing upward", and the whole effect switched off whenever it was not. Level and
    // looking across the sun's azimuth gives exactly 0.
    //
    // It was wrong in PRINCIPLE because this pass integrates inscatter along the VIEW RAY, and the
    // angle that matters is between that ray and the sun -- which the phase function already applies
    // per pixel, and which is not the same question as where the camera happens to be aimed. Air in
    // front of you is still lit when the sun is behind you; it scatters back toward the eye more
    // weakly, and the phase lobe is precisely the term that says by how much.
    //
    // The test does belong in godrays.frag, where the shafts are marched toward the sun's SCREEN
    // POSITION and there is genuinely nothing to march toward once it is behind you. The other thing
    // it was covering -- volblur.frag's filter direction going meaningless when the sun does not
    // project on screen -- is a real problem, and it is fixed there, where it is.
    // fogParams.x, not fogParams.w: the question this early-out asks is "is there any air to
    // scatter with", and that is the DENSITY. It used to ask it of the fog's inscatter strength,
    // which answered the same way only because the two moved together.
    if (volParams.x < 0.5 || fogParams.x <= 0.0 || volParams.w <= 0.0) {
        // -1.0, not 1.0: the term is off, and a depth of -1 reads as "sky/no surface" to the
        // depth-aware magnify in display.frag rather than as a real surface at 1 unit.
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    vec2 uv = v_texcoord0;

    vec4  gp   = texture2D(gPos, pjvSnapToTexel(uv, passInputRes[0].xy));
    vec3  dir  = normalize(gp.xyz - cameraPos.xyz);
    float dist = min(gp.a < 0.0 ? FOG_SKY_DISTANCE : gp.a, VOL_MAX_DISTANCE);
    if (dist <= 0.0) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, gp.a);
        return;
    }

    float H      = max(fogParams.y, 1e-4);
    float offset = volDither(gl_FragCoord.xy);

    // ---- A RATIO ESTIMATOR, WHICH IS WHY THIS CANNOT PRODUCE FIREFLIES -------------------------
    // The obvious estimator sums density * transmittance * visibility and multiplies by the segment
    // length. It is unbiased and it throws fireflies, badly, and the arithmetic says why: the density
    // exponent is clamped at +6, which permits 403x the reference density, and that multiplies a
    // distance that reaches VOL_MAX_DISTANCE. One sample can therefore return 733 -- where the true
    // value of the integral it is estimating is bounded by 1 - exp(-tau), which cannot exceed 1.
    // Three samples per pixel and a handful of pixels catching that spike is exactly the reported
    // "a few fireflies ruin the image".
    //
    // Clamping the result would work and would be a lie: it would darken the honest bright samples by
    // the same rule. The better move is to stop asking Monte Carlo for the MAGNITUDE at all.
    //
    // Split the integral into a magnitude and a fraction:
    //
    //     inscatter  =  (how much light the air scatters along this ray)  x  (what fraction is lit)
    //
    // The first factor is the UNSHADOWED integral, and it has a closed form -- it is exactly
    // 1 - exp(-tau), which fogInscatterFraction already computes for the fog and the screen-space
    // shafts. No sampling, no variance, and it is the same number the fog uses, so the two still agree
    // about the medium. The second is a weighted average of a BINARY visibility, so it lives in [0,1]
    // by construction whatever the density does.
    //
    // The density spike now appears in the numerator and the denominator of that fraction and cancels
    // out. A firefly is not clamped away here; it is arithmetically impossible.
    float lit   = 0.0;
    float total = 0.0;
    for (int i = 0; i < VOL_SAMPLES; i++) {
        // Stratified: one sample per equal segment, offset by the same per-pixel fraction so the
        // strata stay ordered and no two samples of one pixel can collide.
        float t = (float(i) + offset) / float(VOL_SAMPLES) * dist;
        vec3  x = cameraPos.xyz + dir * t;

        // How much this point contributes to what the eye receives: the medium's density there, times
        // what survives the trip back. Used as the WEIGHT of this sample's visibility rather than as a
        // radiance, which is the whole point above.
        float density = fogParams.x * exp(clamp(-(x.y - fogParams.z) / H, -30.0, 6.0));
        float T       = exp(-fogOpticalDepth(cameraPos.xyz, dir, t));
        float w       = density * T;
        if (w <= 0.0) continue;

        total += w;
        if (volSunVisible(x)) lit += w;
    }

    // The closed-form magnitude, and the sampled fraction of it that the sun actually reaches.
    float inscatter = fogInscatterFraction(dir, dist) * (total > 1e-12 ? lit / total : 0.0);

    // Same scattering strength the fog uses, so the shafts belong to the same medium -- but a gentler
    // phase lobe, for the reason set out at VOL_PHASE_G.
    float phase = pjvMiePhase(dot(dir, SUN_DIR), VOL_PHASE_G);

    // ---- ALPHA CARRIES THE DEPTH THIS PIXEL WAS COMPUTED AT -----------------------------------
    // display.frag magnifies this buffer from half resolution and ADDS it to the full-resolution
    // frame. A depth-blind filter blending across a depth discontinuity puts a distant pixel's
    // inscatter onto near geometry, and over sub-pixel foliage that wash covers the geometry
    // entirely -- a fog-coloured hole that pulses as the geometry moves against the fixed half-res
    // grid. Recording the depth here is what lets the magnify reject a tap that belongs to a
    // different surface; the channel was carrying an unread 1.0.
    gl_FragData[0] = vec4(SUN_COLOR * (inscatter * phase * volParams.w * volParams.y), gp.a);
}
