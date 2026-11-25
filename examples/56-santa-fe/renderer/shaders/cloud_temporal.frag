$input v_color0
$input v_texcoord0

// =============================================================================
// cloud_temporal.frag -- the clouds over frames (santa-fe-reference S24c), at their own resolution.
//
// What every real-time cloud renderer surveyed does (Horizon, Unreal, Frostbite): the march is amortised over
// frames. clouds.frag turns its steps' offset each frame; here last frame's clouds are read where this texel's
// cloud was (reprojected at the cloud's own distance -- clouds are far, a turning camera moves them by its turn)
// and clipped to this frame's 3 x 3 neighbourhood (mean +- SF_CLOUD_CLIP sigma: variance clipping, so a moving
// camera or a drifting cloud leaves no smear), then blended in: SF_CLOUD_STILL a frame still, SF_CLOUD_MOVING
// moving. A history whose surface behind changed depth (a ridge's edge crossing) is dropped. The box clouds
// (blocky style) are exact and pass through.
//
// Inputs: clouds.frag (clouds 0, cloudDepth 1), this pass's own last output (2, 3). Outputs: the same as clouds.frag.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>
#include <pjv_camera.sc>

SAMPLER2D(cloudsNow, 0);
SAMPLER2D(cloudDepthNow, 1);
SAMPLER2D(cloudsHist, 2);
SAMPLER2D(cloudDepthHist, 3);

uniform vec4 frameCount;   // y: the camera moved
uniform vec4 sfTemporal;   // x: reset
uniform vec4 sfFlags;      // y bit 0: the box clouds

#define SF_CLOUD_CLIP   1.25
#define SF_CLOUD_STILL  0.05
#define SF_CLOUD_MOVING 0.15

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec2 res = passTargetRes.xy;
    vec4 c = texelFetch(cloudsNow, at, 0);
    vec4 cd = texelFetch(cloudDepthNow, at, 0);
    gl_FragData[1] = cd;
    if (sfTemporal.x > 0.5 || (uint(sfFlags.y + 0.5) & 1u) != 0u) { gl_FragData[0] = c; return; }
    vec4 m1 = vec4(0.0, 0.0, 0.0, 0.0), m2 = vec4(0.0, 0.0, 0.0, 0.0);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec4 s = texelFetch(cloudsNow, clamp(at + ivec2(x, y), ivec2(0, 0), ivec2(res) - 1), 0);
            m1 += s;
            m2 += s * s;
        }
    vec4 mean = m1 / 9.0;
    vec4 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec4(0.0, 0.0, 0.0, 0.0)));
    vec3 camO = pjvViewPosition();
    vec3 dir = pjvViewRayDirection((vec2(at) + 0.5) / res, res);
    float dist = cd.x < 1.0e8 ? cd.x : 30000.0;
    bool inFront;
    vec2 prevUV = pjvPrevViewUV(camO + dir * dist, res, inFront);
    vec4 outC = c;
    if (inFront && all(greaterThanEqual(prevUV, vec2(0.0, 0.0))) && all(lessThanEqual(prevUV, vec2(1.0, 1.0)))) {
        vec4 h = texture2D(cloudsHist, prevUV);
        vec4 hd = texture2D(cloudDepthHist, prevUV);
        // The surface behind it moved a long way in depth: what the history holds is another ray's.
        bool same = abs(log(min(max(hd.y, 1.0), 1e9) / min(max(cd.y, 1.0), 1e9))) < 0.2;
        if (same) {
            h = clamp(h, mean - SF_CLOUD_CLIP * sigma, mean + SF_CLOUD_CLIP * sigma);
            outC = mix(h, c, frameCount.y > 0.5 ? SF_CLOUD_MOVING : SF_CLOUD_STILL);
        }
    }
    if (any(isnan(outC)) || any(isinf(outC))) outC = c;
    gl_FragData[0] = outC;
}
