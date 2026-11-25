$input v_color0
$input v_texcoord0

// =============================================================================
// cloud_shadow.frag -- the clouds' shadow, as a map once a frame (santa-fe-reference S14).
//
// A 256 x 256 map, 32 km across, centred on the camera (snapped to its texels, so it does not swim): each
// texel the transmittance of the sun's path up through the whole cloud layer from where that path crosses the
// layer's base, in 32 steps. Every texel of the trace then reads its cloud shadow with one lookup, projecting
// its point along the sun to the base (sfCloudShadow). It was 6 samples a texel over at most 12 km of the
// path: a lowered (so thicker) layer put them kilometres apart, and the shadows thinned to nothing.
//
// Output: r the transmittance.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>
#include <pjv_camera.sc>
#include <pjv_color.sc>

SAMPLER2D(cloudShadowPrev, 0);   // this pass's own last output (frame-buffer inputs bind first)
SAMPLER2D(cloudNoise, 1);
SAMPLER2D(farHeight, 2);
SAMPLER2D(farColour, 3);
SAMPLER2D(farState, 4);
SAMPLER2D(farCanopy, 5);
uniform vec4 sfCloudMapPrev;     // xy: last frame's corner, z: the frame (which sixteenth refreshes), w: 1 all of it

#define PI 3.14159265358979
#include <mountains_sky.sc>
#include <mountains_far.sc>
#include <santa_fe_clouds.sc>

void main() {
    vec2 xz = sfCloudMap.xy + v_texcoord0 * sfCloudMap.z;
    // A sixteenth of the texels a frame (4 x 4 blocks, one a frame in turn: the clouds drift metres in sixteen
    // frames, a texel is 125 m); the rest carry last frame's over,
    // read where the same ground was in last frame's map (its corner moves with the camera, a texel at a time).
    ivec2 at = ivec2(gl_FragCoord.xy);
    int phase = int(sfCloudMapPrev.z) & 15;
    if (sfCloudMapPrev.w < 0.5 && (at.x & 3) + 4 * (at.y & 3) != phase) {
        vec2 prevUV = (xz - sfCloudMapPrev.xy) / sfCloudMap.z;
        if (all(greaterThan(prevUV, vec2(0.0, 0.0))) && all(lessThan(prevUV, vec2(1.0, 1.0)))) {
            gl_FragColor = vec4(texelFetch(cloudShadowPrev, ivec2(prevUV * 256.0), 0).r, 0.0, 0.0, 1.0);
            return;
        }
    }
    vec3 sun = SUN_DIR;
    float tau = 0.0;
    if (sun.y > 0.01 && sfOn(SF_CLOUDS)) {
        float base = mtnClouds.y, top = mtnClouds.z;
        float len = min((top - base) / sun.y, 60000.0);
        float dt = len / 32.0;
        vec3 p0 = vec3(xz.x, base, xz.y);
        for (int i = 0; i < 32; i++) {
            vec3 q = p0 + sun * ((float(i) + 0.5) * dt);
            float c = sfCoverage(q.xz);
            if (c > 0.02) tau += sfCloudDensity(q, c, false) * mtnClouds.w * dt;
        }
    }
    gl_FragColor = vec4(exp(-tau), 0.0, 0.0, 1.0);
}
