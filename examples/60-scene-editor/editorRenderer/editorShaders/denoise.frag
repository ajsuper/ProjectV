$input v_color0
$input v_texcoord0

// =============================================================================
// denoise.frag  --  Pass 3 of the scene editor's viewport renderer, run three times.
//
// One level of an edge-avoiding a-trous wavelet filter (Dammertz et al. 2010) over
// the raw occlusion occlusion.frag wrote. Three levels run back to back at strides
// 1, 2 and 4, each filtering the previous level's output, which is what turns one
// ray per pixel into something worth looking at while the camera is moving.
//
// The "a-trous" (with holes) part is the stride: the kernel stays 5x5 and 25 taps,
// but the gap between taps doubles each level. Support therefore grows as
// 2*(1 + 2 + 4) = +/-14 pixels for the price of 75 taps, where reaching that far
// with a solid kernel would cost 841. That geometric growth is the whole technique,
// and it is why the levels have to be separate passes: each one needs the previous
// one's *filtered* result as its input, not the raw buffer.
//
// This file is compiled three times, once per level, with ATROUS_STRIDE defined on
// the shaderc command line -- see compEditor.sh. The stride is a compile-time
// constant rather than a uniform for two reasons: the engine's multiPass counter is
// published under a per-pass uniform name (`multiPassPassNumber<index>`), so a
// single shader cannot read its own iteration number, and a constant stride lets
// the 5x5 loop unroll with constant texture offsets.
//
// The buffer management *is* the engine's, though: this pass names its own output
// framebuffer among its inputs, which is what makes it a ping-pong FBO
// (setPingPongFrameBuffers), so each level automatically reads the buffer the
// previous level wrote. Nothing here has to know which of the two it is looking at.
//
// Inputs (FBO 1): 0 = previewColor, 1 = previewNormal, 2 = previewPosition -- the
//                 guides. Slot 3 is FBO 1's fourth attachment, previewGlow, which
//                 this pass has no use for; the engine binds every texture of every
//                 input framebuffer in order, so it is what pushed the occlusion
//                 buffer to 4. (FBO 5): 4 = occlusion, the level's input.
// Output (FBO 5): occlusion.r, filtered.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(previewColor,     0);
SAMPLER2D(previewNormal,    1);
SAMPLER2D(previewPosition,  2);
SAMPLER2D(giLight,          4);

uniform vec4 passTargetRes;                        // (w, h, 1/w, 1/h) of THIS pass's target.
// 8 must equal PROJV_MAX_PASS_INPUTS in constructedRenderer.h -- the engine sets that many.
uniform vec4 passInputRes[8];  // Per input slot; [1] and [2] are the G-buffer.
// x = advanced preview. This pass filters the path traced light, so it is gated on the toggle that
// produces it rather than on one of shade.frag's four readability aids.
uniform vec4 previewSettings;

// The G-buffer is larger than this pass -- the light is traced at a fraction of the viewport while
// geometry stays sharp -- so a guide has to be POINT-sampled. An ordinary fetch would return a
// bilinear blend of several full-resolution texels, and a normal or a position averaged across a
// silhouette belongs to neither surface, which is exactly the discontinuity these weights exist to
// find. Snapping the UV to one source texel's centre asks about a real sample instead.
vec2 guideUV(vec2 uv, int slot) {
    vec2 sourceRes = passInputRes[slot].xy;
    if (sourceRes.x < 0.5) return uv;   // Size unknown (slot above PROJV_MAX_PASS_INPUTS).
    return (floor(uv * sourceRes) + 0.5) * passInputRes[slot].zw;
}

// Set per level on the shaderc command line. Guarded so the file still compiles alone.
#ifndef ATROUS_STRIDE
#define ATROUS_STRIDE 1
#endif

// --- Edge stopping -------------------------------------------------------------

// How far off the centre pixel's tangent plane a tap may sit and still count, in edge lengths of
// the voxel being filtered -- the same scene-independent unit the occlusion radius uses.
//
// A *plane* distance rather than a distance between the two points, and that choice is what lets
// the widest level work at all. Moving along a surface is free under this test however far the tap
// travels, so a stride-4 kernel still filters across a large flat floor; moving off the surface is
// what gets rejected, which is exactly the wall-behind-a-railing case an occlusion filter must not
// blur across. A plain |p - pTap| test would instead reject the outer taps of every level on every
// surface, and the filter would quietly collapse to its innermost ring.
#define ATROUS_PLANE_VOXELS 1.0

// Falloff of the normal term, as the tolerated 1 - dot(n, nTap). Deliberately soft, and this is the
// one weight where the usual choice is wrong for voxel geometry.
//
// The standard normal weight is pow(max(0, dot), 32) or similar, which is essentially binary: it
// refuses to filter across any change of face. On a smooth mesh that is right. On a voxelized
// sphere every "curve" is a staircase of mutually *perpendicular* faces, each only a few pixels
// wide on screen, and dot is exactly 0 across each step -- so a strict weight rejects every
// neighbour a noisy pixel has, and the filter does nothing at all precisely where it is needed.
//
// At 0.5 a perpendicular face still contributes exp(-2) = 0.14 and a 45-degree one 0.55, so a
// staircase filters as the surface it approximates while a genuine deep crease -- where the plane
// term above is also rejecting -- still holds its darkening.
#define ATROUS_NORMAL_SIGMA 0.5

// The 5-tap B3 spline, [1 4 6 4 1]/16, as a function of the offset. Separable in principle, but an
// edge-stopped filter is not separable in practice -- the weights depend on the taps, not only on
// their positions -- so it is evaluated as the 2D outer product.
float atrousKernel(int offset) {
    int magnitude = abs(offset);
    if (magnitude == 0) return 0.375;    // 6/16
    if (magnitude == 1) return 0.25;     // 4/16
    return 0.0625;                       // 1/16
}

void main() {
    vec4 centerSample = texture2D(giLight, v_texcoord0);
    vec3 centerLight = centerSample.rgb;
    // Carried through unfiltered. Alpha is gi_temporal.frag's depth key, not part of the signal:
    // filtering it would average the depths of every surface in the kernel and produce a number
    // belonging to none of them, which the next frame's reprojection would then validate against.
    float depthKey = centerSample.a;

    vec4 centerGeometry = texture2D(previewNormal,   guideUV(v_texcoord0, 1));
    vec4 centerSurface  = texture2D(previewPosition, guideUV(v_texcoord0, 2));

    // Nothing to filter, and nothing that may be filtered *into*: the background carries no
    // surface for the edge-stopping weights to compare against, and gi.frag has already written
    // black there. Passing it straight through also keeps the toggled-off case free -- with the
    // whole buffer at zero this pass is a copy.
    if (previewSettings.x < 0.5 || centerSurface.w < 0.5) {
        gl_FragColor = vec4(centerLight, depthKey);
        return;
    }

    vec3 centerNormal = normalize(centerGeometry.xyz);
    float planeTolerance = max(ATROUS_PLANE_VOXELS * centerGeometry.w, 1e-6);

    vec2 texelSize = passTargetRes.zw;

    // The centre tap seeds both sums at its full kernel weight, so a pixel every one of whose
    // neighbours is rejected keeps its own value rather than dividing by zero.
    float weightSum = atrousKernel(0) * atrousKernel(0);
    vec3 lightSum = centerLight * weightSum;

    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            if (x == 0 && y == 0) {
                continue;
            }

            vec2 tapUV = v_texcoord0 + vec2(float(x * ATROUS_STRIDE), float(y * ATROUS_STRIDE)) * texelSize;
            // The render targets carry no clamp sampler, so a tap off the edge of the screen wraps
            // around and filters against the opposite side of the image.
            if (tapUV.x < 0.0 || tapUV.x > 1.0 || tapUV.y < 0.0 || tapUV.y > 1.0) {
                continue;
            }

            vec4 tapSurface = texture2D(previewPosition, guideUV(tapUV, 2));
            if (tapSurface.w < 0.5) {
                continue;   // Background: no surface, so no comparison and no contribution.
            }

            vec4 tapGeometry = texture2D(previewNormal, guideUV(tapUV, 1));
            vec3 tapNormal = normalize(tapGeometry.xyz);

            // Distance from the tap to the centre pixel's tangent plane. See ATROUS_PLANE_VOXELS.
            float planeDistance = abs(dot(tapSurface.xyz - centerSurface.xyz, centerNormal));
            float planeWeight = exp(-planeDistance / planeTolerance);

            float normalDifference = 1.0 - max(dot(centerNormal, tapNormal), 0.0);
            float normalWeight = exp(-normalDifference / ATROUS_NORMAL_SIGMA);

            float weight = atrousKernel(x) * atrousKernel(y) * planeWeight * normalWeight;

            lightSum += texture2D(giLight, tapUV).rgb * weight;
            weightSum += weight;
        }
    }

    gl_FragColor = vec4(lightSum / weightSum, depthKey);
}
