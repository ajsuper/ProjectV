$input v_color0
$input v_texcoord0

// =============================================================================
// shade.frag  --  Pass 2 of the scene editor's viewport renderer.
//
// The two viewport toggles, and nothing else. Both read the geometry targets the
// albedo pass wrote and multiply a scalar into its colour; with both off this is
// a copy, and the image the editor shows is byte for byte the unmodulated albedo
// the previewer shows.
//
//   renderSettings.x  ambient occlusion -- contact darkening from the depth of the
//                     surroundings, so creases and the ground under an object read
//                     as attached rather than floating.
//   renderSettings.y  normal shading    -- a fixed brightness per face axis, so the
//                     two faces meeting at a voxel edge differ and form is legible
//                     in a flat-coloured region.
//
// Neither is light transport and neither is meant to be: they are readability
// aids for judging shape, sitting in front of a renderer whose entire output is
// stored reflectance.
//
// This runs *before* the accumulate pass, which is what makes the sampled
// occlusion viable at 16 taps. The estimator's tap pattern is rotated per pixel
// and per frame, so a still camera averages a different rotation every frame into
// the running mean and resolves within a few dozen frames -- the same machinery
// that resolves the sub-pixel jitter into anti-aliased edges. While the camera is
// moving you get one frame's 16 taps, which is visibly grainy in the creases and
// converges the moment you stop.
//
// Inputs (FBO 1): 0 = previewColor, 1 = previewNormal, 2 = previewPosition.
// Output (FBO 4): shadedColor.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(previewColor,    0);
SAMPLER2D(previewNormal,   1);
SAMPLER2D(previewPosition, 2);

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 frameCount;     // x = frame index
uniform vec4 renderSettings; // x = ambient occlusion, y = normal shading

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

// --- Normal shading ----------------------------------------------------------

// Brightness of a face pointing down each world axis. Y is left at 1.0 so an
// up-facing surface is the unmodified albedo and the effect only ever darkens;
// X and Z differ from each other so two perpendicular walls separate, which is
// the case a single light direction cannot distinguish.
#define SHADE_AXIS vec3(0.80, 1.00, 0.90)

// Undersides, darker again. Not physics -- just the assumption every eye brings to
// a lit scene, which is what makes an overhang read as an overhang.
#define SHADE_UNDERSIDE 0.68

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
    float distanceToCamera = max(length(position - cameraPos.xyz), 0.0001);

    // Pixels covered by a unit length one unit from the camera, under the same
    // vertical-FOV convention rayStartDirection builds the primary ray with.
    float projectionScale = windowRes.y * 0.5 / tan(radians(FOV * 0.5));
    float radiusPixels = worldRadius * projectionScale / distanceToCamera;
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

void main() {
    vec4 color    = texture2D(previewColor,    v_texcoord0);
    vec4 geometry = texture2D(previewNormal,   v_texcoord0);
    vec4 surface  = texture2D(previewPosition, v_texcoord0);

    // The background passes through untouched. Shading it would move the backdrop
    // every stored colour is being judged against, which the toggles have no
    // business doing.
    if (surface.w < 0.5) {
        gl_FragColor = color;
        return;
    }

    vec3 normal = normalize(geometry.xyz);
    vec3 shaded = color.rgb;

    if (renderSettings.y > 0.5) {
        shaded *= normalShade(normal);
    }

    if (renderSettings.x > 0.5) {
        shaded *= ambientOcclusion(v_texcoord0, surface.xyz, normal, geometry.w);
    }

    gl_FragColor = vec4(shaded, color.a);
}
