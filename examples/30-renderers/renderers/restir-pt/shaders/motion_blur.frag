$input v_color0
$input v_texcoord0

// motion_blur.frag -- window, after the depth of field: a very subtle motion blur along each pixel's motion
// (the G-buffer's gMotion: its chunk's own motion and the camera's, so a falling crate blurs and the still
// ground under it does not).
//
// Eight taps along the pixel's travel this frame, centred on it, over restirDof.z of it -- a short shutter,
// not a smear -- and no more than restirDof.w pixels. A tap on a surface much nearer the camera than this
// pixel counts only if it is moving about as much: a still foreground is not dragged across a moving
// background. The motion is the nearest surface's in the 3x3 render texels round the pixel, as the resolve
// takes it. Under a third of a pixel of blur, the pixel passes through untouched.
//
// Inputs: 0 dofColor, 1-7 the G-buffer (gPos 1, gMotion 7). Output: mbColor.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(dofColor, 0);
SAMPLER2D(gPos, 1);
SAMPLER2D(gMotion, 7);

uniform vec4 restirDof;   // z the shutter: the share of a frame's motion blurred over (0: off), w its most, in pixels

#define BLUR_TAPS 8

void main() {
    vec2 res = passTargetRes.xy;
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec3 centreColour = texelFetch(dofColor, at, 0).rgb;
    if (restirDof.z <= 0.0) { gl_FragColor = vec4(centreColour, 1.0); return; }

    vec2 uv = (vec2(at) + 0.5) / res;
    vec2 renderRes = vec2(textureSize(gMotion, 0));
    ivec2 texel = min(ivec2(uv * renderRes), ivec2(renderRes) - ivec2(1, 1));
    vec2 motion = texelFetch(gMotion, texel, 0).xy;
    float depth = texelFetch(gPos, texel, 0).w;
    float nearest = depth >= 0.0 ? depth : 1e30;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(texel + ivec2(x, y), ivec2(0, 0), ivec2(renderRes) - ivec2(1, 1));
            float d = texelFetch(gPos, q, 0).w;
            if (d >= 0.0 && d < nearest) {
                nearest = d;
                motion = texelFetch(gMotion, q, 0).xy;
            }
        }

    // This frame's travel, in pixels (the motion vector points back to where it was).
    vec2 travel = -motion * res * restirDof.z;
    float length_ = length(travel);
    if (length_ < 0.33) { gl_FragColor = vec4(centreColour, 1.0); return; }
    travel *= min(length_, restirDof.w) / length_;
    float travelled = length(travel);
    if (depth < 0.0) depth = 1e30;

    vec3 sum = centreColour;
    float total = 1.0;
    for (int i = 0; i < BLUR_TAPS; i++) {
        float s = (float(i) + 0.5) / float(BLUR_TAPS) - 0.5;
        if (abs(s) < 1e-3) continue;
        vec2 p = uv + travel * s / res;
        ivec2 q = min(ivec2(p * renderRes), ivec2(renderRes) - ivec2(1, 1));
        float d = texelFetch(gPos, q, 0).w;
        if (d < 0.0) d = 1e30;
        float w = 1.0;
        if (d < depth * 0.95) {   // nearer than this pixel: only if it moves about as much
            float itsTravel = length(texelFetch(gMotion, q, 0).xy * res * restirDof.z);
            w = smoothstep(0.5, 1.0, itsTravel / max(travelled, 1e-3));
        }
        sum += texture2D(dofColor, p).rgb * w;
        total += w;
    }
    gl_FragColor = vec4(sum / total, 1.0);
}
