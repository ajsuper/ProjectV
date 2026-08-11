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
//   renderSettings.w  ray occlusion     -- the same question as .x asked of the scene
//                     instead of the screen. Unlike the other three this one is not
//                     computed here: occlusion.frag casts the rays and denoise.frag
//                     filters them, and this pass only reads the result and applies
//                     the strength. See those two files for why it has to be split up.
//
// .x and .w are two estimators of one effect and the editor keeps them mutually
// exclusive, so nothing here has to decide how two occlusion terms should combine.
//
// None of it is light transport and none of it is meant to be: they are readability
// aids for judging shape, sitting in front of a renderer whose entire output is
// stored reflectance. The shadow in particular is a visibility test and not a light:
// it darkens what the sun cannot see and leaves everything else exactly as it was,
// so a colour judged in the lit part of the scene is still the stored colour.
//
// There is a fifth toggle, and it is not one of these. The advanced preview lives
// entirely in albedo.frag, which writes what it produces -- emission and the
// specular reflection -- to a target of its own rather than into the colour. All
// this pass does with it is ADD it, once, after the four multiplies above. That
// ordering is the whole reason for the separate target: every term here darkens
// DIFFUSE REFLECTANCE, and multiplying a glow by one of them dims a light source
// because it happens to sit in a crease, face away from a sun this mode does not
// have, or point down. An emitter is not shaded by the aids that exist to make
// unlit geometry legible. With the toggle off the target is zero and every line
// below behaves exactly as it did before it existed.
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
//                 (FBO 5): 4 = occlusion, denoise.frag's filtered ray occlusion.
//                 The slots are the engine's, not this file's: it binds every
//                 texture of every input framebuffer in order, so FBO 1's fourth
//                 attachment is what pushed the occlusion buffer from 3 to 4.
//                 The shadow ray additionally reads the scene itself, through the
//                 samplers pjv_utils_DDA.sc declares at slots 9-15; the engine binds
//                 those for every pass, so nothing in render.json changes for it.
// Output (FBO 4): shadedColor.
// =============================================================================

#include <bgfx_shader.sh>

#include <pjv_utils_DDA.sc>

SAMPLER2D(previewColor,    0);
SAMPLER2D(previewNormal,   1);
SAMPLER2D(previewPosition, 2);
SAMPLER2D(previewGlow,     3);
SAMPLER2D(occlusion,       4);

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 frameCount;     // x = frame index
// x = ambient occlusion, y = normal shading, z = sun shadow, w = ray occlusion
uniform vec4 renderSettings;
// x = 1.0 under an orthographic projection, y = the world height the image spans when
// it is. Only the occlusion estimator reads it, and only to convert a world radius into
// screen pixels -- see ambientOcclusion. Matches albedo.frag's uniform of the same name.
uniform vec4 cameraProjection;

// Matches albedo.frag's primary ray. Only the vertical FOV is needed here, to turn
// a world-space radius into the screen-space one the taps walk.
#define FOV 60.0

#define TAU 6.28318530718

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

// --- Ray ambient occlusion -----------------------------------------------------

// How dark a fully enclosed point goes under the ray estimator, matching AO_INTENSITY's
// reasoning: short of black, so the albedo stays readable in the creases.
//
// The strength is applied here rather than in occlusion.frag because that buffer is filtered
// on its way over: a filter is entitled to smooth an estimate, not to scale it, and keeping
// the two apart means this constant can be changed without reasoning about the denoiser.
#define AO_RAY_INTENSITY 0.9

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
        radiusPixels = worldRadius * windowRes.y / max(cameraProjection.y, 0.0001);
    } else {
        float distanceToCamera = max(length(position - cameraPos.xyz), 0.0001);
        float projectionScale = windowRes.y * 0.5 / tan(radians(FOV * 0.5));
        radiusPixels = worldRadius * projectionScale / distanceToCamera;
    }
    if (radiusPixels < AO_MIN_RADIUS_PIXELS) {
        return 1.0;   // Far enough away that the whole neighbourhood is one pixel.
    }
    radiusPixels = min(radiusPixels, AO_MAX_RADIUS_PIXELS);

    vec2 texelSize = 1.0 / windowRes.xy;

    // Per pixel and per frame: the pixel term breaks up the pattern spatially, the
    // frame term is what the accumulate pass averages away. The pixel coordinate is
    // rebuilt from the UV rather than read from gl_FragCoord, which bgfx does not
    // expose to a fragment shader on the HLSL/SPIR-V path.
    float rotation = interleavedGradientNoise(uv * windowRes.xy + frameCount.x * 5.588238) * TAU;
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

    // All three multiply, so the order between them does not change the result. They
    // are applied cheapest first anyway, which is also roughly least to most likely to
    // be turned off.
    if (renderSettings.y > 0.5) {
        shaded *= normalShade(normal);
    }

    if (renderSettings.x > 0.5) {
        shaded *= ambientOcclusion(v_texcoord0, surface.xyz, normal, geometry.w);
    }

    // The editor never has both occlusion toggles on at once, so this is an alternative to
    // the branch above rather than something layered on top of it. Already estimated and
    // already filtered by the time it gets here -- all that is left is the strength.
    if (renderSettings.w > 0.5) {
        float rayOcclusion = texture2D(occlusion, v_texcoord0).r;
        shaded *= clamp(1.0 - AO_RAY_INTENSITY * (1.0 - rayOcclusion), 0.0, 1.0);
    }

    if (renderSettings.z > 0.5) {
        shaded *= sunShadow(surface.xyz, normal, geometry.w);
    }

    // Added, not multiplied, and added last -- after every darkening above has had its say about the
    // diffuse reflectance and none of them has had any say about this. Zero with the advanced
    // preview off, which makes this line a no-op and the pass byte for byte what it always was.
    gl_FragColor = vec4(shaded + glow, color.a);
}
