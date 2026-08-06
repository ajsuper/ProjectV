$input v_color0
$input v_texcoord0

// =============================================================================
// taa.frag  --  Pass 2 of the scene editor's Render mode.
//
// The only thing between path_trace.frag's one-sample-per-pixel output and the
// screen. There is no spatial denoiser in this renderer -- no a-trous, no bilateral
// blur, no SVGF -- deliberately: every spatial filter trades detail it cannot
// distinguish from noise, and Render mode exists to show what the scene actually
// looks like. Temporal averaging is the one operation that removes noise without
// removing anything else, because it converges to the true answer instead of an
// estimate of it.
//
// So this pass is two behaviours with one buffer between them.
//
// STILL CAMERA -- a plain, uncapped running mean.
//
//     accumulated = (history * n + current) / (n + 1)
//
//   with n counted from the frame the camera last moved on. Every frame is worth
//   exactly as much as every other, so after a thousand frames the image is a
//   thousand-sample render and the noise is down by a factor of about 32. Nothing
//   is clamped, rejected or reprojected on this path: the camera has not moved, so
//   the history is this pixel's own history and there is no ghost to guard against.
//   This is the path that produces the "very high quality image" -- point the
//   camera, stop touching it, watch it resolve.
//
//   The alternative -- an exponential moving average with a fixed alpha, which is
//   what most TAA does -- would stop converging at an effective 1/alpha samples,
//   somewhere around 20 to 60. The image would go quiet and then stay slightly
//   noisy forever. A true mean has no floor.
//
// MOVING CAMERA -- reprojection, neighbourhood clamping, fixed blend.
//
//   Every pixel's world position is known (the trace pass writes it), so it can be
//   projected into the previous frame's image with the previous frame's camera and
//   the history read from where this surface actually was. That history is then
//   clamped into the colour range of the current frame's 3x3 neighbourhood, which
//   is what rejects the parts of it that belong to a surface no longer there --
//   disocclusion behind a moving edge, geometry that left the screen. What survives
//   is blended at a fixed rate.
//
//   This path is a preview, not the product. It exists so flying the camera shows
//   something coherent rather than a blizzard, and it is thrown away the moment the
//   camera stops: the mean above restarts from n = 0 on the first still frame, so
//   no ghost, no smear and no clamped history ever reaches the converged image.
//
// Inputs:  FBO 1 slot 0 = traceColor, 1 = traceNormal, 2 = tracePosition
//          FBO 2 slot 3 = taaColor (the history; this pass both reads and writes
//          FBO 2, which the engine detects and ping-pongs -- see setPingPongTextures)
// Output:  FBO 2.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(traceColor,    0);
SAMPLER2D(traceNormal,   1);
SAMPLER2D(tracePosition, 2);
SAMPLER2D(taaColor,      3);

uniform vec4 windowRes;
uniform vec4 texelSize;
// x = frame index, y = 1.0 on a frame the camera moved, z = the frame it last moved on,
// w = 1.0 when there is no usable history at all and this frame must be taken whole. That last one
// is not the same statement as "the camera moved": it is raised on the first frame, on a tab switch
// and after a resize -- cases where the accumulation targets hold an image of something else, or in
// the resize case have just been allocated and never written. An uninitialised RGBA32F can be NaN,
// and a single NaN entering a running mean never leaves it.
uniform vec4 frameCount;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
// Last frame's camera, for the reprojection. w carries last frame's orthographic
// height (on cameraPos) and backoff (on cameraDir), so an orthographic view reprojects
// against the parameters it was actually drawn with rather than this frame's.
uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;
// x = 1.0 orthographic, y = ortho height, z = ortho backoff. Matches path_trace.frag.
uniform vec4 cameraProjection;
// x = bounces, y = sun intensity, z = sky intensity, w = firefly clamp. Unused here;
// declared because every pass in a renderer shares one uniform set.
uniform vec4 renderParams;

#define FOV 60.0

// How much of the current frame a MOVING camera takes each frame. Only the moving path
// uses it -- the still path is a true mean with no blend factor at all. Low enough that
// flying looks smooth, high enough that the preview keeps up with what is in front of
// the camera rather than trailing it.
#define MOVING_BLEND 0.12

// How far the clamped history may sit outside the current neighbourhood's range, as a
// multiple of that neighbourhood's standard deviation. Variance clipping rather than a
// hard min/max box: a hard box over a noisy 1-spp neighbourhood is enormous and clamps
// almost nothing, which is how ghosting gets through.
#define CLAMP_SIGMA 1.6

// Projects a world position into the previous frame's image. Returns false when it
// lands behind the previous camera or off the edge of its image, in which case there is
// no history to read and the current frame is taken whole.
//
// This is the exact inverse of the ray generators in path_trace.frag -- the same basis,
// the same handedness, the same UV flip. It has to be: an inverse that disagrees by a
// fraction of a pixel smears the image every frame the camera moves.
bool projectIntoPreviousFrame(vec3 worldPosition, out vec2 previousUV) {
    vec3 forward = normalize(prevCameraDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));
    float aspectRatio = windowRes.x / windowRes.y;

    vec2 ndc;
    if (cameraProjection.x > 0.5) {
        // Parallel rays: the plane the origins slide across sits prevCameraDir.w behind
        // the camera, and the position maps onto it linearly. No depth term at all.
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
    vec4 traced = texture2D(traceColor, v_texcoord0);
    vec3 current = traced.rgb;

    // Nothing to average against, and nothing safe to read. Before either path.
    if (frameCount.w != 0.0) {
        gl_FragColor = vec4(current, 1.0);
        return;
    }

    // Frames since the camera last moved. Zero on the frame of a move, which is the
    // condition both paths below key off.
    float samples = frameCount.x - frameCount.z;

    // --- Still camera: the running mean -------------------------------------
    if (frameCount.y == 0.0 && samples >= 1.0) {
        vec3 history = texture2D(taaColor, v_texcoord0).rgb;
        gl_FragColor = vec4((history * samples + current) / (samples + 1.0), 1.0);
        return;
    }

    // --- Moving camera: reprojected preview ----------------------------------
    vec4 surface = texture2D(tracePosition, v_texcoord0);

    vec2 previousUV;
    bool haveHistory = surface.w > 0.5 &&
                       projectIntoPreviousFrame(surface.xyz, previousUV);
    if (!haveHistory) {
        // Background, or nowhere to read from. The sky is noise-free anyway.
        gl_FragColor = vec4(current, 1.0);
        return;
    }

    // The current neighbourhood's mean and variance, from the raw trace rather than the
    // accumulated buffer: the clamp has to describe what is in front of the camera
    // *now*, and the accumulated buffer is by construction a description of the past.
    vec3 moment1 = vec3(0.0);
    vec3 moment2 = vec3(0.0);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 tapUV = clamp(v_texcoord0 + vec2(float(x), float(y)) * texelSize.xy,
                               texelSize.xy * 0.5, 1.0 - texelSize.xy * 0.5);
            vec3 tap = texture2D(traceColor, tapUV).rgb;
            moment1 += tap;
            moment2 += tap * tap;
        }
    }
    moment1 /= 9.0;
    moment2 /= 9.0;
    vec3 sigma = sqrt(max(moment2 - moment1 * moment1, vec3(0.0)));

    vec3 history = texture2D(taaColor, previousUV).rgb;
    history = clamp(history, moment1 - sigma * CLAMP_SIGMA, moment1 + sigma * CLAMP_SIGMA);

    gl_FragColor = vec4(mix(history, current, MOVING_BLEND), 1.0);
}
