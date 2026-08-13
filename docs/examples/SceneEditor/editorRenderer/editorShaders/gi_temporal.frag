$input v_color0
$input v_texcoord0

// =============================================================================
// gi_temporal.frag  --  Pass 3 of the scene editor's viewport renderer.
//
// Temporal reprojection of the traced light: last frame's result, followed to where
// this frame's geometry has moved to on screen, blended with this frame's fresh
// samples. Runs at the tracer's resolution, between gi.frag and the a-trous levels.
//
// This is what makes the path traced viewport usable while flying. gi.frag casts one
// path per pixel per frame, which the spatial filter widens and the accumulate pass
// averages -- but averaging only works while the camera is still, because the moment
// it moves the pixel under a piece of geometry is a different pixel and the running
// mean has to be thrown away. Reprojection is how the history survives that: the light
// arriving at a surface does not change because the camera moved, so last frame's
// estimate is still a valid estimate, it is just somewhere else on screen now.
//
// =============================================================================
// WHY THIS REPROJECTS LIGHT AND NOT COLOUR
// =============================================================================
//
// The obvious place for reprojection is the final image, which is what taa.frag does
// for Render mode. Doing it here instead, on the DEMODULATED light gi.frag writes, is
// better for one specific reason: albedo cannot ghost. A reprojected colour buffer
// drags a voxel's colour a fraction of a pixel every frame the camera moves, and the
// error is a smear along the direction of travel that never quite settles. Here the
// albedo is not in the buffer at all -- shade.frag applies it fresh at full resolution
// from this frame's G-buffer -- so a slightly stale reprojection produces slightly
// stale *lighting*, which is low frequency and invisible, while every edge in the image
// stays exactly where this frame's geometry says it is.
//
// It is also why the validation below can be cheap. A colour history needs a
// neighbourhood clamp to catch disocclusion; a light history only needs to know whether
// it belongs to the same surface, which one number answers.
//
// =============================================================================
// THE HISTORY IS THE FILTERED BUFFER
// =============================================================================
//
// This pass writes giLight, and so do the three a-trous levels after it, and giLight is
// what this pass reads as history. So the history is the *filtered* result of the
// previous frame, not the raw trace -- which is the SVGF arrangement and it is what
// makes convergence fast: each frame's estimate starts from something already smooth.
//
// The engine detects the ping-pong automatically (this pass names its own output among
// its inputs), so the read and the write are different buffers and nothing here has to
// know which of the two it is looking at.
//
// Inputs (FBO 1): 0 = previewColor, 1 = previewNormal, 2 = previewPosition,
//                 3 = previewGlow.
//                 (FBO 5): 4 = giRaw, this frame's trace.
//                 (FBO 6): 5 = giLight, the previous frame's result.
// Output (FBO 6): giLight -- rgb = integrated light, a = the depth key (see below).
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(previewColor,    0);
SAMPLER2D(previewNormal,   1);
SAMPLER2D(previewPosition, 2);
SAMPLER2D(previewGlow,     3);
SAMPLER2D(giRaw,           4);
SAMPLER2D(giLight,         5);

uniform vec4 passTargetRes;   // (w, h, 1/w, 1/h) of THIS pass's target.
// 8 must equal PROJV_MAX_PASS_INPUTS in constructedRenderer.h -- the engine sets that many.
uniform vec4 passInputRes[8];  // Per input slot; [2] is previewPosition, at full resolution.
// x = frame index, y = the camera moved this frame, z = the frame it last moved on,
// w = nonzero when there is no usable history at all (first frame, a resize, a scene load).
uniform vec4 frameCount;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
// The previous frame's camera. w lanes carry that frame's orthographic extent and backoff, because
// an orthographic view has to be reprojected against the extent it was actually drawn with.
uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;
// x = 1.0 under an orthographic projection, y = the world height the image spans when it is.
uniform vec4 cameraProjection;
// x = advanced preview. Off means gi.frag wrote black and there is nothing to integrate.
uniform vec4 previewSettings;

// Matches albedo.frag's primary ray and taa.frag's inverse of it.
#define FOV 60.0

// The weight this frame's samples carry when the history is good: an exponential moving average with
// a window of roughly 1/HISTORY_ALPHA frames.
//
// A fixed window rather than a running sample count, and that is a deliberate trade. A count would
// converge further on a still camera, but it needs a channel to live in and the accumulate pass
// already does the long-window averaging for the still case -- it means 12 frames of light feeding a
// mean of up to 64 frames of image. What this pass has to be good at is the case that pass cannot
// help with: moving. A short window follows the light as it changes instead of lagging behind it.
#define HISTORY_ALPHA 0.08

// How far the history's surface may sit from the expected one before it is rejected, as a fraction
// of the distance to it. Relative rather than absolute because a scene is metres across at one end
// of the editor's range and thousands of units at the other, and a fixed tolerance would be either
// permanently open or permanently shut.
#define DEPTH_TOLERANCE 0.03

// The key stored in alpha: how far the shaded point sits along the view axis. Written with respect to
// THIS frame's camera and compared next frame against the same quantity recomputed with respect to
// the camera it was written with, so the comparison is apples to apples.
//
// One number, and it is enough. A reprojected sample that belongs to a different surface is at a
// different distance along the view ray almost by definition -- that is what being a different
// surface means from the camera's point of view. The case it cannot catch is two surfaces at
// identical depth, which is the case where taking the wrong one is harmless.
float depthKey(vec3 worldPosition, vec3 origin, vec3 forward) {
    return dot(worldPosition - origin, normalize(forward));
}

// Projects a world position into the previous frame's image. False when it lands behind that camera
// or off the edge of its image, in which case there is no history and this frame is taken whole.
//
// The exact inverse of albedo.frag's ray generator -- same basis, same handedness, same UV flip -- and
// identical to taa.frag's, which inverts the same camera for Render mode. It has to be exact: an
// inverse that disagrees by a fraction of a pixel smears every frame the camera moves.
bool projectIntoPreviousFrame(vec3 worldPosition, out vec2 previousUV) {
    vec3 forward = normalize(prevCameraDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));
    float aspectRatio = passTargetRes.x / passTargetRes.y;

    vec2 ndc;
    if (cameraProjection.x > 0.5) {
        // Parallel rays: the plane the origins slide across sits prevCameraDir.w behind the camera,
        // and the position maps onto it linearly. No depth term at all.
        float halfHeight = max(prevCameraPos.w, 1e-6) * 0.5;
        vec3 toPoint = worldPosition - (prevCameraPos.xyz - forward * prevCameraDir.w);
        ndc = vec2(dot(toPoint, right) / (halfHeight * aspectRatio),
                   dot(toPoint, up) / halfHeight);
    } else {
        vec3 toPoint = worldPosition - prevCameraPos.xyz;
        float depth = dot(toPoint, forward);
        if (depth <= 1e-4) return false;   // Behind the previous camera.
        float scale = tan(radians(FOV * 0.5));
        ndc = vec2(dot(toPoint, right) / (depth * scale * aspectRatio),
                   dot(toPoint, up) / (depth * scale));
    }

    previousUV = vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
    return previousUV.x >= 0.0 && previousUV.x <= 1.0 &&
           previousUV.y >= 0.0 && previousUV.y <= 1.0;
}

void main() {
    vec3 current = texture2D(giRaw, v_texcoord0).rgb;

    // POINT-SAMPLED, for the reason gi.frag point samples the same buffer: previewPosition is four
    // times this pass's width, and a filtered fetch returns a blend of sixteen texels that, across a
    // silhouette, is a position on neither surface. Reprojecting that lands the history lookup
    // somewhere neither surface is.
    vec2 surfaceUV = v_texcoord0;
    if (passInputRes[2].x > 0.5) {
        surfaceUV = (floor(v_texcoord0 * passInputRes[2].xy) + 0.5) * passInputRes[2].zw;
    }
    vec4 surface = texture2D(previewPosition, surfaceUV);

    // No geometry, or the preview is off: gi.frag wrote black and there is nothing to integrate.
    // A zero key marks "no surface here", which is what the validation below tests for.
    if (previewSettings.x < 0.5 || surface.w < 0.5) {
        gl_FragColor = vec4(current, 0.0);
        return;
    }

    float currentKey = depthKey(surface.xyz, cameraPos.xyz, cameraDir.xyz);

    // Nothing safe to read at all -- the first frame, or the frame after the targets were
    // reallocated, when the history buffer holds whatever the driver left in it. An uninitialised
    // RGBA16F can be NaN, and one NaN entering a running mean never leaves it.
    if (frameCount.w != 0.0) {
        gl_FragColor = vec4(current, currentKey);
        return;
    }

    vec2 previousUV;
    if (!projectIntoPreviousFrame(surface.xyz, previousUV)) {
        gl_FragColor = vec4(current, currentKey);   // Off screen last frame: newly visible.
        return;
    }

    vec4 history = texture2D(giLight, previousUV);

    // Was that sample written by a surface, and by THIS surface? The key it stored is its distance
    // along the previous frame's view axis, so recomputing that for the point being shaded now gives
    // the value it must have had if the two are the same surface. A disocclusion -- geometry that has
    // slid aside to reveal what was behind it -- fails this, which is exactly what it is for.
    float expectedKey = depthKey(surface.xyz, prevCameraPos.xyz, prevCameraDir.xyz);
    bool wroteASurface = history.a != 0.0;
    bool sameSurface = abs(history.a - expectedKey) <= DEPTH_TOLERANCE * max(abs(expectedKey), 1e-4);

    if (!wroteASurface || !sameSurface) {
        gl_FragColor = vec4(current, currentKey);
        return;
    }

    // Rejecting a negative or non-finite history rather than propagating it. The trace clamps its own
    // output, but a reprojection reads through a filter chain and one bad texel would otherwise be
    // spread by the a-trous levels and then held by the mean.
    vec3 previous = history.rgb;
    if (!(previous.r >= 0.0) || !(previous.g >= 0.0) || !(previous.b >= 0.0)) {
        gl_FragColor = vec4(current, currentKey);
        return;
    }

    gl_FragColor = vec4(mix(previous, current, HISTORY_ALPHA), currentKey);
}
