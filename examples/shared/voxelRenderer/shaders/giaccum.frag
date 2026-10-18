$input v_color0
$input v_texcoord0

// =============================================================================
// giaccum.frag -- raygi's history, at its resolution. Each pixel's surface point is found in last frame's
// image through the previous camera, and that pixel's average is carried on if it was the same surface
// (the stored world position within a few centimetres of this one): averaged over up to 24 frames, an
// exponential tail after that. Anything that moved -- a body, debris, the camera's disocclusions -- fails
// the check and starts again from this frame's one ray; the blur that follows hides most of that.
//
// Inputs: FBO 7 (this frame's rays, slot 0), FBO 1 (G-buffer, slots 1..6), FBO 8 (its own last output,
// slots 7..8). Outputs (FBO 8): 0 rgb the average, a how many frames are in it; 1 xyz the surface point.
// =============================================================================

#include <bgfx_shader.sh>
#include <grr_common.sc>

SAMPLER2D(giRay,     0);
SAMPLER2D(gPos,      1);
SAMPLER2D(gNormal,   2);
SAMPLER2D(giHist,    7);
SAMPLER2D(giHistPos, 8);

uniform vec4 passTargetRes;
uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;

#define MAX_FRAMES 24.0

void main() {
    vec2 pixel = floor(v_texcoord0 * passTargetRes.xy);
    vec2 uv = (pixel + 0.25) * passTargetRes.zw;   // the render pixel raygi traced from
    vec4 pos = texture2D(gPos, uv);
    vec2 here = (pixel + 0.5) * passTargetRes.zw;
    if (pos.w < 0.0) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    vec3 current = texture2D(giRay, here).rgb;
    float voxel = texture2D(gNormal, uv).w;

    float frames = 0.0;
    vec3 history = current;
    bool valid;
    vec2 prevUV = grrWorldToUV(pos.xyz, prevCameraPos.xyz, prevCameraDir.xyz, passTargetRes.xy, valid);
    if (valid) {
        prevUV = (floor(prevUV * passTargetRes.xy) + 0.5) * passTargetRes.zw;
        vec4 was = texture2D(giHistPos, prevUV);
        if (was.w > 0.5 && distance(was.xyz, pos.xyz) < 0.04 + 0.5 * voxel) {
            vec4 h = texture2D(giHist, prevUV);
            history = h.rgb;
            frames = h.a;
        }
    }
    frames = min(frames + 1.0, MAX_FRAMES);
    gl_FragData[0] = vec4(mix(history, current, 1.0 / frames), frames);
    gl_FragData[1] = vec4(pos.xyz, 1.0);
}
