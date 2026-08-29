$input v_color0
$input v_texcoord0

// =============================================================================
// accumulate.frag  --  Pass 2 of the scene previewer.
//
// Anti-aliasing, and nothing else. The albedo pass is already noise-free -- its
// only stochastic input is the sub-pixel jitter -- so this does not need to be a
// denoiser or a reprojecting TAA. It is a plain running mean of a still camera's
// jittered samples, which resolves into supersampled edges, and a hard reset the
// moment the camera moves.
//
// That reset is why there is no reprojection here: while moving you get exactly
// the current frame, which is correct-but-aliased rather than smeared, and the
// image is clean again within a few frames of stopping. For a previewer that is
// the right trade -- reprojection exists to preserve expensive light transport
// across frames, and there is none here to preserve.
//
// Inputs:  FBO 1 slot 0 = previewColor (this frame), FBO 2 slot 1 = accumColor
//          (history; this pass both reads and writes FBO 2, which the engine
//          detects and ping-pongs automatically -- see setPingPongTextures).
// Output:  FBO 2.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(previewColor, 0);
SAMPLER2D(accumColor,   1);

uniform vec4 frameCount; // x = frame index, y = camera moved, z = frame the camera last moved on

// Jitter converges well before this; the cap keeps the running mean from going
// so long that single-precision accumulation starts losing the new sample.
#define MAX_ACCUM_FRAMES 64.0

void main() {
    vec3 current = texture2D(previewColor, v_texcoord0).rgb;
    vec3 history = texture2D(accumColor,   v_texcoord0).rgb;

    // Frames since the camera last moved. On the frame of a move this is 0, and
    // the history is meaningless, so the current frame is taken whole.
    float samples = min(frameCount.x - frameCount.z, MAX_ACCUM_FRAMES);

    vec3 accumulated = current;
    if (frameCount.y == 0.0 && samples >= 1.0) {
        accumulated = (history * samples + current) / (samples + 1.0);
    }

    gl_FragColor = vec4(accumulated, 1.0);
}
