$input v_color0
$input v_texcoord0

// =============================================================================
// shade.frag  --  Pass 6 of the scene editor's viewport renderer.
//
// The four viewport toggles, and nothing else. All read the geometry targets the
// albedo pass wrote and multiply a scalar into its colour; with all off this is
// a copy, and the image the editor shows is byte for byte the unmodulated albedo
// the previewer shows.
//
//   renderSettings.x  ambient occlusion -- contact darkening from the depth of the
//                     surroundings, so creases and the ground under an object read
//                     as attached rather than floating.
//   renderSettings.y  normal shading    -- a fixed brightness per face axis, so the
//                     two faces meeting at a voxel edge differ and form is legible
//                     in a flat-coloured region.
//   renderSettings.z  sun shadow        -- one visibility ray towards a fixed sun,
//                     so an object reads as sitting at a place in the scene rather
//                     than as a shape floating in front of it.
//   renderSettings.w  ray occlusion     -- no longer read by this pass. It fed
//                     occlusion.frag, whose rays the gi.frag / gi_temporal.frag /
//                     denoise.frag chain replaced: a traced occlusion term is a traced
//                     light with the colour thrown away, so there is no reason to trace
//                     both. The lane stays declared because main.cpp still packs it.
//
// None of it is light transport and none of it is meant to be: they are readability
// aids for judging shape, sitting in front of a renderer whose entire output is
// stored reflectance. The shadow in particular is a visibility test and not a light:
// it darkens what the sun cannot see and leaves everything else exactly as it was,
// so a colour judged in the lit part of the scene is still the stored colour.
//
// There is a fifth toggle -- the advanced preview -- and it is a different kind of thing.
// It has two halves, and they arrive here separately:
//
//   THE GLOW, from albedo.frag, which writes the emission and the specular reflection to
//   a target of its own rather than into the colour. All this pass does with it is ADD
//   it, once, after every multiply below. That ordering is the whole reason for the
//   separate target: every term here darkens DIFFUSE REFLECTANCE, and multiplying a glow
//   by one of them dims a light source because it happens to sit in a crease or point
//   down. An emitter is not shaded by the aids that exist to make unlit geometry
//   legible. With the toggle off the target is zero and that line is a no-op.
//
//   THE LIGHT, which REPLACES the fixed aids (.y and .z) rather than joining them --
//   they stand in for light, and there is now real light to stand in front of. It
//   arrives in two halves: the sun's direct term, traced in THIS pass at full
//   resolution because its shadow is the sharpest thing in the image, and everything
//   softer, traced coarse by gi.frag and reconstructed here. The screen-space
//   occlusion in .x is deliberately NOT replaced -- it is the one aid that adds
//   something neither half can produce, detail at the scale of a single voxel -- and
//   it multiplies the soft half only. See directSunLight and the apply site in main().
//
// This runs *before* the accumulate pass, which is what makes the sampled
// occlusion viable at 16 taps. The estimator's tap pattern is rotated per pixel
// and per frame, so a still camera averages a different rotation every frame into
// the running mean and resolves within a few dozen frames -- the same machinery
// that resolves the sub-pixel jitter into anti-aliased edges. While the camera is
// moving you get one frame's 16 taps, which is visibly grainy in the creases and
// converges the moment you stop.
//
// Inputs (FBO 1): 0 = previewColor, 1 = previewNormal, 2 = previewPosition,
//                 3 = previewGlow.
//                 (FBO 6): 4 = giLight, the traced, reprojected, denoised irradiance.
//                 The slots are the engine's, not this file's: it binds every
//                 texture of every input framebuffer in order, so FBO 1's fourth
//                 attachment is what pushed the light buffer from 3 to 4.
//                 The shadow ray additionally reads the scene itself, through the
//                 samplers pjv_utils_DDA.sc declares at slots 9-15; the engine binds
//                 those for every pass, so nothing in render.json changes for it.
// Output (FBO 4): shadedColor.
// =============================================================================

#include <bgfx_shader.sh>

// The direct sun ray sees THROUGH transparent voxels rather than treating them as opaque, so a window
// does not cast the shadow of a wall. Matched to gi.frag's budget, not Render mode's much larger one:
// the two shadow rays are halves of one estimate now -- this one the primary hit's, that one every
// vertex after it -- and if they disagreed about what glass does, the split would show as a seam
// between the direct and indirect terms rather than as a slightly cheaper approximation.
// Must be defined BEFORE the include to override the header's #ifndef default.
#define MAX_PEEL_ITERATIONS 9
#include <pjv_utils_DDA.sc>
#define TRANSPARENT_LAYERS 8u

SAMPLER2D(previewColor,    0);
SAMPLER2D(previewNormal,   1);
SAMPLER2D(previewPosition, 2);
SAMPLER2D(previewGlow,     3);
SAMPLER2D(giLight,         4);

// 8 must equal PROJV_MAX_PASS_INPUTS in constructedRenderer.h -- the engine sets that many.
uniform vec4 passInputRes[8];  // Per input slot; [4] is the traced light.

uniform vec4 passTargetRes;   // Engine-set: (w, h, 1/w, 1/h) of THIS pass's target.
uniform vec4 cameraPos;
uniform vec4 frameCount;     // x = frame index
// x = ambient occlusion, y = normal shading, z = sun shadow, w = ray occlusion
uniform vec4 renderSettings;
// x = advanced preview. When it is on, the path traced light replaces the four aids above rather
// than joining them -- see the apply site in main().
uniform vec4 previewSettings;
// x = 1.0 under an orthographic projection, y = the world height the image spans when
// it is. Only the occlusion estimator reads it, and only to convert a world radius into
// screen pixels -- see ambientOcclusion. Matches albedo.frag's uniform of the same name.
uniform vec4 cameraProjection;
// Render mode's light rig, and gi.frag's: xyz = the sun's direction, w = its angular RADIUS in
// radians, which is the softness of its shadow. Read only by directSunLight below.
uniform vec4 sunDir;
// x = diffuse bounces, y = sun intensity, z = sky intensity, w = the firefly clamp. Only y is read
// here (through sunRadianceColor); the rest belong to the passes that trace.
uniform vec4 renderParams;

// Matches albedo.frag's primary ray. Only the vertical FOV is needed here, to turn
// a world-space radius into the screen-space one the taps walk.
#define FOV 60.0

#define TAU 6.28318530718
#define PI 3.14159265359

// --- Ambient occlusion -------------------------------------------------------

// 16 taps is the point where a still image resolves in well under a second of
// accumulation while a moving one is grainy rather than blotchy. Raising it trades
// frame time for how the image looks *while flying*, which is the only thing it
// buys -- the settled image is already converged.
#define AO_SAMPLES 16

// The sampling radius, in edge lengths of the voxel being shaded. Expressing it
// this way rather than in world units is what lets one constant serve every scene:
// a Minecraft import whose voxels are 1.0 across and a mesh voxelization whose
// voxels are 0.01 across both get occlusion that reaches the same few voxels out,
// which is the distance the eye reads as a crease.
#define AO_RADIUS_VOXELS 3.0

// How dark a fully enclosed point goes. 1.0 would take it to black; short of that
// keeps the albedo readable in the creases, which is the whole point of the pass.
#define AO_INTENSITY 0.9

// Ignores taps within this sine of the shading point's own tangent plane. A flat
// wall's taps all land exactly on it, and without the bias every one of them
// contributes whatever side of zero the arithmetic rounded to -- which is the
// familiar flat-surface self-occlusion grey.
#define AO_BIAS 0.12

// Below the minimum the disc is too small to sample distinctly and the pass would
// be spending taps on the pixel it is already shading; above the maximum a tap
// pattern near the camera spills across the screen and thrashes the texture cache
// for occlusion that reads as a smear rather than a crease.
#define AO_MIN_RADIUS_PIXELS 2.0
#define AO_MAX_RADIUS_PIXELS 48.0

// Turns of the sampling spiral. Coprime-ish with the tap count so the arms do not
// line up into visible spokes.
#define AO_SPIRAL_TURNS 7.0

// --- Path traced light --------------------------------------------------------
//
// gi.frag traces the light at a fraction of this pass's resolution and writes it DEMODULATED: what
// arrives here is irradiance, with the shaded surface's own albedo deliberately left out. This pass
// puts the albedo back at full resolution, which is the entire reason the trace is allowed to be
// coarse -- light is low frequency, a voxel boundary is not, and multiplying a smooth light term by
// a sharp albedo keeps the boundary sharp. Tracing the product instead would smear voxel colours
// into each other wherever the light was upsampled.

// How far apart two samples' surfaces may be before the upsample stops mixing them, in edge lengths
// of the voxel being shaded -- the same scene-independent unit the occlusion radius uses. Matches
// the denoiser's ATROUS_PLANE_VOXELS in spirit: a plane distance, so two coplanar samples at
// different depths still combine.
#define UPSAMPLE_PLANE_VOXELS 1.0

// How much a normal may differ before a sample is rejected, as 1 - dot.
#define UPSAMPLE_NORMAL_SIGMA 0.20

// How much of the screen-space occlusion term survives when it is applied ON TOP of the traced
// light -- see the apply site in main() for why it is applied there at all.
//
// Not the full strength the flat-albedo path uses, because the two are not standing in for the same
// thing here. The trace already contains real occlusion; what it does not contain is occlusion at
// the frequency of a single voxel, because it is traced at a quarter of this pass's resolution and
// then upsampled across voxel boundaries. So this term is not being asked to darken the creases --
// the trace has done that -- it is being asked for the contact detail the trace cannot resolve. At
// full strength it would darken every crease a second time and the image would read as much dirtier
// than the render it is previewing.
#define AO_OVER_TRACED_LIGHT 0.55

// --- The sun's direct term, at full resolution --------------------------------
//
// The other half of the split gi.frag's header describes. That pass traces every soft, low-frequency
// term at a quarter of this resolution and lets the denoiser and the bilateral upsample reconstruct
// it, which is the right treatment for light that has bounced, for the sky, and for anything else
// with no edge to lose. It is the wrong treatment for exactly one term: the sun's shadow.
//
// A shadow edge is as sharp as the sun is small. At quarter resolution it is sampled at one point per
// sixteen pixels, then a-trous filtered across surfaces, then bilinearly reconstructed -- three stages
// each of which is a low-pass filter -- and what comes out is a soft grey suggestion of the shadow
// with no edge in it anywhere. That reads as haze, not as a shadow, and a hard shadow is how the eye
// reads where a thing is standing, so it is also the single term that decides whether the preview
// looks like the render.
//
// So it is computed here instead: one ray per screen pixel, unfiltered, added to the upsampled
// remainder. That is the same cost as the fixed-sun shadow toggle above, which this replaces while
// the advanced preview is on -- and it buys a shadow from the SCENE'S OWN sun, at its own colour and
// its own softness, rather than from a hardcoded direction that agrees with nothing downstream.
//
// The estimator is one sample of the sun's disc per pixel per frame, so the penumbra is stochastic
// rather than analytic and resolves through the accumulate pass exactly the way the occlusion taps
// and the sub-pixel jitter do. That is deliberate: it means the penumbra's WIDTH comes from the sun's
// angular radius -- the same control the render uses, so a soft sun previews soft -- instead of from
// a filter kernel that would have to be tuned to imitate one. While the camera moves you see one
// sample's worth, which is a hard edge jittered inside the penumbra band; it settles within a second
// of stopping. Nothing about it is filtered spatially, which is the whole point.

// Value and reasoning identical to SHADOW_ORIGIN_BIAS_VOXELS below, and to gi.frag's
// GI_ORIGIN_BIAS_VOXELS: a quarter of a voxel along the normal, scaled by the voxel rather than fixed
// in world units, because editor scenes sit at coordinates where a float32 ULP is not small.
#define SUN_ORIGIN_BIAS_VOXELS 0.25

// Matched to gi.frag's GI_SHADOW_STEPS rather than to the primary march's 256. A shadow ray that runs
// out of steps reports a miss and lights the pixel, so the budget is the distance over which this
// agrees with the render; 128 covers any scene the editor opens at one ray per pixel.
#define SUN_SHADOW_STEPS 128u

// Lifted from gi.frag, which lifted it from renderRenderer/path_trace.frag, for the reason given
// there: a light rig is not part of the engine and an example does not reach into another example's
// files. All three copies must agree -- this is the sun the render will use.
vec3 sunRadianceColor() {
    float elevation = sunDir.y;
    vec3  tint      = mix(vec3(1.0, 0.42, 0.15), vec3(1.0, 0.96, 0.90),
                          smoothstep(0.0, 0.35, elevation));
    float intensity = renderParams.y * smoothstep(-0.05, 0.15, elevation);
    return tint * intensity;
}

// PCG, one round. The same generator and the same (pixel, frame) hash gi.frag seeds from, which is
// safe to reuse rather than something to decorrelate from: the two halves of the split no longer
// estimate any term in common -- that is the whole content of the split -- so there is no shared
// estimator whose noise could correlate. They also run on different pixel grids, this one at full
// resolution and that one at a quarter of it.
float randomUnit(inout uint seed) {
    seed = seed * 747796405u + 2891336453u;
    uint word = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    return float((word >> 22u) ^ word) * (1.0 / 4294967296.0);
}

uint hashSeed(uvec3 v) {
    uint h = v.x * 1973u + v.y * 9277u + v.z * 26699u;
    h = (h ^ (h >> 15u)) * 2246822519u;
    h = (h ^ (h >> 13u)) * 3266489917u;
    return h ^ (h >> 16u);
}

mat3 basisAround(vec3 n) {
    vec3 tangent = abs(n.z) < 0.999 ? normalize(cross(vec3(0.0, 0.0, 1.0), n))
                                    : normalize(cross(vec3(0.0, 1.0, 0.0), n));
    return mat3(tangent, cross(n, tangent), n);
}

// A direction inside the sun's cone, uniformly. This is what makes the shadow's edge as soft as the
// sun is wide, and why widening sunDir.w softens every shadow without changing the exposure.
vec3 sampleSunCone(inout uint seed) {
    vec3 axis = normalize(sunDir.xyz);
    float cosMax = cos(sunDir.w);
    float cosTheta = mix(cosMax, 1.0, randomUnit(seed));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 2.0 * PI * randomUnit(seed);
    return normalize(basisAround(axis) * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
}

// The sun's contribution at this pixel, DEMODULATED the same way gi.frag's output is -- irradiance
// times cos over pi, with the surface's own albedo left out for main() to supply at full resolution.
// The two are in the same units by construction, which is what makes adding them legitimate.
//
// Transmittance rather than a visibility bit, so coloured glass puts coloured light on the floor
// instead of casting a black shadow. Opaque geometry returns exactly zero.
vec3 directSunLight(vec2 uv, vec3 position, vec3 normal, float voxelSize) {
    vec3 irradiance = sunRadianceColor();
    if (dot(irradiance, irradiance) <= 0.0) {
        return vec3(0.0);   // Sun below the horizon: no ray, and nothing to add.
    }

    // Per pixel and per frame, as everywhere else in this renderer: the pixel term decorrelates
    // neighbours so the penumbra is noise rather than a pattern, the frame term is what the
    // accumulate pass averages away. Rebuilt from the UV because bgfx does not expose gl_FragCoord
    // to a fragment shader on the SPIR-V path.
    ivec2 pixel = ivec2(uv * passTargetRes.xy);
    uint seed = hashSeed(uvec3(uint(pixel.x), uint(pixel.y), uint(frameCount.x)));

    vec3 lightDirection = sampleSunCone(seed);
    float cosLight = dot(normal, lightDirection);
    // Self-shadowing resolved before a ray is spent on it, and it is also what keeps the terminator
    // on the geometry rather than on the shadow ray's epsilon.
    if (cosLight <= 0.0) {
        return vec3(0.0);
    }

    Ray shadowRay;
    shadowRay.origin = position + normal * (SUN_ORIGIN_BIAS_VOXELS * voxelSize);
    shadowRay.direction = lightDirection;

    RayQuery query;
    query.maxRaySteps = SUN_SHADOW_STEPS;
    // No LOD, for gi.frag's reason: a shadow cast by a coarsened version of the scene is the wrong
    // shape, and unlike a primary ray there is nothing on screen that tells you it happened.
    query.startLOD = 0;
    query.finishLOD = 0;
    query.distanceToFinishLOD = 100000;

    vec3 visibility = raySceneTransmittance(shadowRay, query, 1e30, TRANSPARENT_LAYERS);
    return irradiance * visibility * (cosLight / PI);
}

// --- Normal shading ----------------------------------------------------------

// Brightness of a face pointing down each world axis. Y is left at 1.0 so an
// up-facing surface is the unmodified albedo and the effect only ever darkens;
// X and Z differ from each other so two perpendicular walls separate, which is
// the case a single light direction cannot distinguish.
#define SHADE_AXIS vec3(0.80, 1.00, 0.90)

// Undersides, darker again. Not physics -- just the assumption every eye brings to
// a lit scene, which is what makes an overhang read as an overhang.
#define SHADE_UNDERSIDE 0.68

// --- Sun shadow ---------------------------------------------------------------

// World-fixed, not camera-relative. A shadow that swung round with the camera would
// be useless for the thing shadows are here to do -- telling you where an object is
// standing -- because the cue would move with the eye rather than stay with the
// scene. High and off to one side, so a vertical wall and a floor get visibly
// different amounts of it and the direction is legible from the shadow's shape.
#define SUN_DIRECTION normalize(vec3(0.38, 0.82, 0.43))

// How far a shadowed point falls. Deliberately a long way short of black: this pass
// sits in front of a renderer whose whole output is stored reflectance, and a shadow
// dark enough to hide a material would defeat that. At 0.55 a shadowed albedo is
// still being judged, just against a darker version of itself.
#define SHADOW_DARKENING 0.55

// Where the visibility ray starts, in edge lengths of the voxel being shaded --
// the same scene-independent unit AO_RADIUS_VOXELS uses, and for the same reason.
// A quarter of a voxel along the normal is inside the empty cell adjoining the
// surface, which is far enough out that the ray cannot re-hit the voxel it left
// (the self-hit that shows as uniform stippled darkening across every lit face)
// and near enough in that it cannot step over an occluder sitting against it.
//
// A fixed world-space epsilon does not work here. Editor scenes sit at coordinates
// in the thousands, where a float32 ULP is around a thousandth of a unit, so an
// epsilon small enough for a 0.01-unit voxel is below the noise floor of the
// position it is being added to. Scaling by the voxel keeps the offset meaningful
// at both ends of the range of scenes the editor opens.
#define SHADOW_ORIGIN_BIAS_VOXELS 0.25

// Matches albedo.frag's primary march, so the shadow reaches as far as the geometry
// you can see casting it. This is the pass's whole cost: one scene ray per lit pixel,
// which is why the toggle is off until asked for. Faces pointing away from the sun
// are darkened without a ray at all, so in practice a little under half the pixels
// pay it.
#define SHADOW_MAX_STEPS 256u

// Interleaved gradient noise (Jimenez 2014). One rotation per pixel, decorrelated
// from its neighbours, and cheap enough to be free next to the taps it drives.
float interleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Occlusion at a point, from the depths of its screen-space neighbourhood: 1.0 is
// unoccluded, 0.0 fully enclosed.
//
// Every tap is measured as the sine of the neighbour's elevation above this point's
// tangent plane -- normalised by the distance between them -- so a tap contributes
// between 0 and 1 whatever the scene's scale. That is the difference from the usual
// Alchemy-style estimator, whose unnormalised v.n / v.v needs an epsilon retuned to
// the size of a scene's voxels to keep near-coincident taps from exploding.
float ambientOcclusion(vec2 uv, vec3 position, vec3 normal, float voxelSize) {
    float worldRadius = AO_RADIUS_VOXELS * voxelSize;

    // World-to-pixel, which is the one thing the two projections disagree about here.
    // Under parallel rays the scale is constant across the whole image -- no distance
    // term at all -- so distant geometry keeps its occlusion instead of losing it to a
    // radius that shrank away. Under perspective it is pixels covered by a unit length
    // one unit from the camera, under the same vertical-FOV convention
    // rayStartDirection builds the primary ray with.
    float radiusPixels;
    if (cameraProjection.x > 0.5) {
        radiusPixels = worldRadius * passTargetRes.y / max(cameraProjection.y, 0.0001);
    } else {
        float distanceToCamera = max(length(position - cameraPos.xyz), 0.0001);
        float projectionScale = passTargetRes.y * 0.5 / tan(radians(FOV * 0.5));
        radiusPixels = worldRadius * projectionScale / distanceToCamera;
    }
    if (radiusPixels < AO_MIN_RADIUS_PIXELS) {
        return 1.0;   // Far enough away that the whole neighbourhood is one pixel.
    }
    radiusPixels = min(radiusPixels, AO_MAX_RADIUS_PIXELS);

    vec2 texelSize = passTargetRes.zw;

    // Per pixel and per frame: the pixel term breaks up the pattern spatially, the
    // frame term is what the accumulate pass averages away. The pixel coordinate is
    // rebuilt from the UV rather than read from gl_FragCoord, which bgfx does not
    // expose to a fragment shader on the HLSL/SPIR-V path.
    float rotation = interleavedGradientNoise(uv * passTargetRes.xy + frameCount.x * 5.588238) * TAU;
    // The render targets are created without a clamp sampler, so a tap off the edge
    // of the screen would wrap around and occlude against the opposite side.
    vec2 uvMin = texelSize * 0.5;
    vec2 uvMax = 1.0 - texelSize * 0.5;

    float radiusSquared = worldRadius * worldRadius;
    float occlusion = 0.0;

    for (int i = 0; i < AO_SAMPLES; i++) {
        // alpha spreads the taps along the disc's radius while the turns spread them
        // around it, so the pattern covers the disc evenly instead of clumping at the
        // centre the way uniform-angle sampling does.
        float alpha = (float(i) + 0.5) / float(AO_SAMPLES);
        float angle = alpha * AO_SPIRAL_TURNS * TAU + rotation;
        vec2 tapUV = uv + vec2(cos(angle), sin(angle)) * alpha * radiusPixels * texelSize;
        tapUV = clamp(tapUV, uvMin, uvMax);

        vec4 tap = texture2D(previewPosition, tapUV);
        if (tap.w < 0.5) {
            continue;   // Background: there is nothing there to be occluded by.
        }

        vec3 toSample = tap.xyz - position;
        float distanceSquared = dot(toSample, toSample);
        float elevation = dot(toSample, normal) / max(sqrt(distanceSquared), 0.0001);
        // Linear in the squared distance: full weight at the point itself, none at
        // the radius, so geometry entering and leaving the disc does not pop.
        float falloff = clamp(1.0 - distanceSquared / radiusSquared, 0.0, 1.0);
        occlusion += clamp(elevation - AO_BIAS, 0.0, 1.0) * falloff;
    }

    return clamp(1.0 - AO_INTENSITY * occlusion / float(AO_SAMPLES), 0.0, 1.0);
}

// Fixed brightness for a face, from its normal alone. No light source and no rays:
// the dot with the per-axis constants reduces to picking one of them for an
// axis-aligned voxel face, and interpolates sensibly for anything that is not.
float normalShade(vec3 normal) {
    float shade = dot(abs(normal), SHADE_AXIS);
    return shade * mix(1.0, SHADE_UNDERSIDE, clamp(-normal.y, 0.0, 1.0));
}

// The traced light at this pixel, reconstructed from the smaller buffer gi.frag wrote.
//
// A joint bilateral upsample, not a bilinear one. The four low-resolution samples around this pixel
// may sit on different surfaces -- at a silhouette, one of them is the wall behind the object -- and
// blending them by distance alone drags that wall's light onto this object's edge. That is the halo
// every naively upsampled lighting buffer has. Weighting each sample by how well its surface agrees
// with THIS pixel's full-resolution surface rejects the ones that belong to something else.
//
// Falls back to the centre sample if every neighbour is rejected, which happens on a one-pixel
// feature whose low-resolution neighbours are all elsewhere. Slightly blocky beats haloed.
vec3 upsampleIndirectLight(vec2 uv, vec3 position, vec3 normal, float voxelSize) {
    vec2 sourceRes = passInputRes[4].xy;
    vec2 sourceTexel = passInputRes[4].zw;
    if (sourceRes.x < 0.5) {
        return texture2D(giLight, uv).rgb;   // Size unknown: nothing better to do than sample.
    }

    // The four source texels surrounding this pixel's position in the source grid.
    vec2 sourceCoord = uv * sourceRes - 0.5;
    vec2 baseCoord = floor(sourceCoord);
    vec2 fraction = sourceCoord - baseCoord;

    float planeTolerance = max(UPSAMPLE_PLANE_VOXELS * voxelSize, 1e-6);

    vec3 lightSum = vec3(0.0);
    float weightSum = 0.0;

    for (int y = 0; y <= 1; y++) {
        for (int x = 0; x <= 1; x++) {
            vec2 tapCoord = baseCoord + vec2(float(x), float(y));
            vec2 tapUV = (tapCoord + 0.5) * sourceTexel;
            if (tapUV.x < 0.0 || tapUV.x > 1.0 || tapUV.y < 0.0 || tapUV.y > 1.0) continue;

            // The geometry the sample was traced FROM, point-sampled the same way gi.frag point
            // sampled it, so the comparison is against the surface that sample actually used.
            vec2 guide = (floor(tapUV * passInputRes[2].xy) + 0.5) * passInputRes[2].zw;
            vec4 tapSurface = texture2D(previewPosition, guide);
            if (tapSurface.w < 0.5) continue;       // That sample saw the background.

            vec2 normalGuide = (floor(tapUV * passInputRes[1].xy) + 0.5) * passInputRes[1].zw;
            vec3 tapNormal = normalize(texture2D(previewNormal, normalGuide).xyz);

            // Bilinear weight, then the two geometric ones.
            float bilinear = (x == 0 ? 1.0 - fraction.x : fraction.x) *
                             (y == 0 ? 1.0 - fraction.y : fraction.y);

            float planeDistance = abs(dot(tapSurface.xyz - position, normal));
            float planeWeight = exp(-planeDistance / planeTolerance);

            float normalDifference = 1.0 - max(dot(normal, tapNormal), 0.0);
            float normalWeight = exp(-normalDifference / UPSAMPLE_NORMAL_SIGMA);

            float weight = bilinear * planeWeight * normalWeight;
            lightSum += texture2D(giLight, tapUV).rgb * weight;
            weightSum += weight;
        }
    }

    if (weightSum < 1e-5) {
        return texture2D(giLight, uv).rgb;
    }
    return lightSum / weightSum;
}

// One ray towards the sun: 1.0 if it gets away, SHADOW_DARKENING if something stops
// it. Hard-edged by construction, and intentionally so -- the ray direction carries
// no per-frame jitter, so unlike the occlusion above this contributes nothing for the
// accumulate pass to resolve and looks the same in the first frame of a camera move
// as in the hundredth. Softening it would mean jittering the direction within a cone
// and letting the running mean average it, but a binary test averaged one sample per
// frame is far noisier while flying than the 16-tap occlusion is, so it stays hard.
//
// Only visibility is asked for, never a material, so the march can stop at the first
// thing it touches -- rayT < 0.0 is the genuine miss.
float sunShadow(vec3 position, vec3 normal, float voxelSize) {
    // Self-shadowing, resolved before spending a ray on it: a face turned away from
    // the sun cannot see it whatever the rest of the scene does. This is also what
    // keeps the terminator on the geometry rather than on the shadow ray's epsilon,
    // where a grazing NdotL would otherwise put it.
    if (dot(normal, SUN_DIRECTION) <= 0.0) {
        return SHADOW_DARKENING;
    }

    Ray shadowRay;
    shadowRay.origin = position + normal * (SHADOW_ORIGIN_BIAS_VOXELS * voxelSize);
    shadowRay.direction = SUN_DIRECTION;

    RayQuery shadowQuery;
    shadowQuery.maxRaySteps = SHADOW_MAX_STEPS;
    shadowQuery.startLOD = 0;
    shadowQuery.finishLOD = 0;
    shadowQuery.distanceToFinishLOD = 10000;

    SceneIntersectData shadowHit = raySceneIntersect(shadowRay, shadowQuery);
    return shadowHit.rayT < 0.0 ? 1.0 : SHADOW_DARKENING;
}

void main() {
    vec4 color    = texture2D(previewColor,    v_texcoord0);
    vec4 geometry = texture2D(previewNormal,   v_texcoord0);
    vec4 surface  = texture2D(previewPosition, v_texcoord0);
    // Zero unless the advanced preview is on. Nothing below branches on the toggle: adding zero is
    // the same image, and a uniform branch around one texture fetch is not worth the two code paths.
    vec3 glow     = texture2D(previewGlow,     v_texcoord0).rgb;

    // The background passes through untouched. Shading it would move the backdrop
    // every stored colour is being judged against, which the toggles have no
    // business doing.
    //
    // The glow is added even here, because a transparent pane in front of the sky is a background
    // pixel by this target's reckoning -- the peel found no opaque surface -- and a glowing one
    // still glows. See albedo.frag's miss path, which is what writes it.
    if (surface.w < 0.5) {
        gl_FragColor = vec4(color.rgb + glow, color.a);
        return;
    }

    vec3 normal = normalize(geometry.xyz);
    vec3 shaded = color.rgb;

    // Computed here rather than at either apply site because BOTH paths below want it -- the flat
    // one at full strength, the advanced preview at a fraction of it. One estimator, sixteen taps,
    // whichever mode is on.
    float occlusion = renderSettings.x > 0.5
                        ? ambientOcclusion(v_texcoord0, surface.xyz, normal, geometry.w)
                        : 1.0;

    // All three multiply, so the order between them does not change the result. They
    // are applied cheapest first anyway, which is also roughly least to most likely to
    // be turned off.
    if (renderSettings.y > 0.5) {
        shaded *= normalShade(normal);
    }

    shaded *= occlusion;

    if (renderSettings.z > 0.5) {
        shaded *= sunShadow(surface.xyz, normal, geometry.w);
    }

    // The advanced preview replaces the fixed aids -- the per-axis brightness and the fixed-sun
    // shadow -- rather than joining them. Those two stand in for light, and once there is real light
    // there is nothing left for them to stand in for; a shadow from a sun the trace does not have
    // would contradict it outright.
    //
    // SCREEN-SPACE OCCLUSION IS THE EXCEPTION, and it is kept because it is not standing in for
    // anything. The traced light is correct and it is also LOW FREQUENCY -- quarter resolution,
    // temporally reprojected, then denoised and bilaterally upsampled -- and that chain is very good
    // at where the light comes from and structurally incapable of resolving one voxel's own contact
    // shading. The result reads beautifully at the scale of a room and leaves individual voxels
    // nearly indistinguishable, which for an editor is the wrong trade: the thing being edited is the
    // voxel. So the occlusion term goes on top, at AO_OVER_TRACED_LIGHT of its flat-mode strength,
    // putting the per-voxel definition back at full resolution without darkening the creases the
    // trace has already darkened a second time over.
    //
    // It stays on the toggle it has always been on. Turning it off gives the untouched traced light,
    // which is what to look at when judging the lighting itself rather than the geometry.
    //
    // THE LIGHT ARRIVES IN TWO HALVES, split by frequency rather than by the textbook direct/indirect
    // line, and they are added:
    //
    //   direct    the sun at this surface, traced HERE, one ray per screen pixel, unfiltered. Sharp,
    //             because a shadow edge is the highest-frequency thing in a lit image and nothing
    //             that has been through a quarter-resolution buffer still has an edge. See
    //             directSunLight above, and gi.frag's header for the other side of the argument.
    //   indirect  everything else -- bounced sunlight, the sky, emitters -- traced coarse and
    //             reconstructed here. None of it has an edge to lose.
    //
    // Only the indirect half is occluded. That is not a shortcut, it is what the occlusion term means:
    // it estimates how much of the surrounding HEMISPHERE is blocked, which is a statement about
    // ambient light and says nothing about whether one particular direction -- the sun -- is clear.
    // The shadow ray answers that, exactly and per pixel. Multiplying it in as well would darken a
    // sunlit crease that is provably lit, and the giveaway is that the darkening would not move when
    // the sun did.
    //
    // This is the remodulation: albedo (full resolution, straight from the G-buffer) times the light.
    // See upsampleIndirectLight and the note above it.
    if (previewSettings.x > 0.5) {
        vec3 direct = directSunLight(v_texcoord0, surface.xyz, normal, geometry.w);
        vec3 indirect = upsampleIndirectLight(v_texcoord0, surface.xyz, normal, geometry.w);
        shaded = color.rgb * (direct + indirect * mix(1.0, occlusion, AO_OVER_TRACED_LIGHT));
    }

    // Added, not multiplied, and added last -- after every darkening above has had its say about the
    // diffuse reflectance and none of them has had any say about this. Zero with the advanced
    // preview off, which makes this line a no-op and the pass byte for byte what it always was.
    gl_FragColor = vec4(shaded + glow, color.a);
}
