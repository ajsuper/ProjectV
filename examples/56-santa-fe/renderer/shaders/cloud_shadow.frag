$input v_color0
$input v_texcoord0

// =============================================================================
// cloud_shadow.frag -- the clouds' shadow, as a map once a frame (santa-fe-reference S14).
//
// A 256 x 256 map, 32 km across, drawn from the sun (S29a: sfSunBasis) and centred on the camera (snapped to its
// texels, so it does not swim): each texel a line toward the sun, and the transmittance of the sun's path along
// it through the whole cloud layer -- from where it rises through the base to where it leaves the top, over the
// curved earth (sfAltitude) -- in 48 steps. Every texel of the trace then reads its cloud shadow with one lookup
// (sfCloudShadow). It was laid on the ground, a texel the path up from the base above it: a low sun's land got its
// shadow from 150 km off the map, by a 6-sample march.
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
    vec3 right, up;
    sfSunBasis(SUN_DIR, right, up);
    vec2 uvw = sfCloudMap.xy + v_texcoord0 * sfCloudMap.z;   // this texel's line, in the map's frame
    // A sixteenth of the texels a frame (4 x 4 blocks, one a frame in turn: the clouds drift metres in sixteen
    // frames, a texel is 125 m); the rest carry last frame's over,
    // read where the same ground was in last frame's map (its corner moves with the camera, a texel at a time).
    ivec2 at = ivec2(gl_FragCoord.xy);
    int phase = int(sfCloudMapPrev.z) & 15;
    if (sfCloudMapPrev.w < 0.5 && (at.x & 3) + 4 * (at.y & 3) != phase) {
        vec2 prevUV = (uvw - sfCloudMapPrev.xy) / sfCloudMap.z;
        if (all(greaterThan(prevUV, vec2(0.0, 0.0))) && all(lessThan(prevUV, vec2(1.0, 1.0)))) {
            gl_FragColor = vec4(texelFetch(cloudShadowPrev, ivec2(prevUV * 256.0), 0).r, 0.0, 0.0, 1.0);
            return;
        }
    }
    vec3 sun = SUN_DIR;
    float tau = 0.0;
    if (sun.y > 0.01 && sfOn(SF_CLOUDS)) {
        float base = mtnClouds.y, top = mtnClouds.z;
        // The line o + sun t; its height over the curved ground is A t^2 + B t + C (sfAltitude). Where it rises
        // through the base and the top: the larger roots (the line climbs toward the sun from its lowest point).
        vec3 o = right * uvw.x + up * uvw.y;
        vec2 h0 = o.xz - pjvViewPosition().xz;
        float A = dot(sun.xz, sun.xz) / (2.0 * SF_EARTH_RADIUS);
        float B = sun.y + dot(h0, sun.xz) / SF_EARTH_RADIUS;
        float C0 = o.y + dot(h0, h0) / (2.0 * SF_EARTH_RADIUS);
        float tB, tT;
        if (A < 1e-12) { tB = (base - C0) / B; tT = (top - C0) / B; }
        else {
            tB = (-B + sqrt(max(B * B - 4.0 * A * (C0 - base), 0.0))) / (2.0 * A);
            tT = (-B + sqrt(max(B * B - 4.0 * A * (C0 - top), 0.0))) / (2.0 * A);
        }
        float len = clamp(tT - tB, 0.0, 200000.0);
        float dt = len / 48.0;
        for (int i = 0; i < 48; i++) {
            vec3 q = o + sun * (tB + (float(i) + 0.5) * dt);
            float c = sfCoverage(q.xz);
            if (c > 0.02) {
                q.y = sfAltitude(q);   // the density by height over the curved ground
                tau += sfCloudDensity(q, c, false) * mtnClouds.w * dt;
            }
        }
    }
    gl_FragColor = vec4(exp(-tau), 0.0, 0.0, 1.0);
}
