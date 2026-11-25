$input v_color0
$input v_texcoord0

// =============================================================================
// clouds.frag -- the cumulus, at a quarter of the trace pass's texels (santa-fe-reference S11, S24).
//
// Each texel marches its view ray through the layer (santa_fe_clouds.sc) to the farthest surface the trace
// pass saw in its footprint, the steps offset by a dither turned each frame, and puts the air in front of the
// cloud on it. cloud_temporal.frag accumulates it (S24c; it had a history of its own here before, unclipped).
//
// Inputs: the trace pass's gPos 0 and gNormal 1 (fbo 17), the far field's textures (2-6), the sky's light 7.
// Outputs: 0 rgb the clouds' light (with the air in front of them), a what they let through; 1 x the
// clouds' front distance, y the farthest surface it marched to, z (blocky) the size of the box it met first.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>
#include <pjv_camera.sc>
#include <pjv_color.sc>   // pjvSrgbToLinear (mountains_far.sc)

uniform vec4 frameCount;     // x: frame index
uniform vec4 renderParams;   // y: cloud steps
uniform vec4 sfTemporal;     // x: reset
uniform vec4 jitterParams;
uniform vec4 sfShadowLOD;    // x: the trace's shadow rays go through voxels this far (m); the far field's part starts there

SAMPLER2D(gPos, 0);      // fbo 17: the trace pass's gPos and gNormal alone (S23: a slot for the sky's light)
SAMPLER2D(gNormal, 1);
SAMPLER2D(cloudNoise, 2);
SAMPLER2D(farHeight, 3);
SAMPLER2D(farColour, 4);
SAMPLER2D(farState, 5);
SAMPLER2D(farCanopy, 6);
#define SF_SKY_TEX_SLOT 7   // sky_ambient.frag (S23)

#define PI 3.14159265358979
#include <mountains_sky.sc>
#include <mountains_far.sc>
#include <santa_fe_clouds.sc>

// The far field's part of the sun's shadow (SF2_FAR_SHADOW_LOW, santa-fe-reference S18): one shadow march over
// the heightfield and crowns for this texel's nearest surface of the trace texels under it -- the hills' and the
// far trees' shadows, smooth enough at a quarter of the trace's texels. The trace marched voxels for the first
// sfShadowLOD.x metres (all of them when 0: from where the ray leaves them). shadow_soft multiplies it in.
// Out: floor(depth * 2) + the visibility * 0.99, depth the surface's (shadow_soft weighs by it); 0 with none.
float farSunShadow(ivec2 t0, vec2 ratio, ivec2 tsize) {
    if (!sfOn2(SF2_FAR_SHADOW_LOW) || !sfOn(SF_SHADOW_BLUR) || !sfOn(SF_SHADOWS) || SUN_DIR.y <= -0.02) return 0.0;
    float best = 1e30;
    ivec2 bq = t0;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            if (float(x) >= ratio.x || float(y) >= ratio.y) continue;
            ivec2 q = clamp(t0 + ivec2(x, y), ivec2(0, 0), tsize - 1);
            float d = texelFetch(gPos, q, 0).w;
            if (d >= 0.0 && d < best) { best = d; bq = q; }
        }
    if (best > 1e29) return 0.0;
    vec4 p = texelFetch(gPos, bq, 0);
    vec3 n = texelFetch(gNormal, bq, 0).xyz;
    float key = floor(min(p.w, 30000.0) * 2.0);
    if (dot(n, SUN_DIR) <= 0.0) return key + 0.99;   // facing away: the trace's own answer (dark) stands
    float t0r = mtnLeaveVoxels(p.xyz, SUN_DIR);
    bool far = t0r <= 0.0;
    int f0 = mtnFarFirstRing(p.xz);
    float edge = mtnFarRing(f0).z / float(MTN_FAR_RES);
    vec3 o = far ? p.xyz + (n + vec3(0.0, 1.0, 0.0)) * (edge * 0.5) : p.xyz + n * 0.1;
    float start = far ? 0.0 : (sfShadowLOD.x > 0.0 ? min(t0r, sfShadowLOD.x) : t0r);
    mtnLodSlope = 0.004;
    mtnLodBase = max(p.w, 1.0) * 2.0 * pjvViewTanHalfFov() / passTargetRes.y;
    float tF;
    int ring;
    bool hit = mtnFarMarch(o, SUN_DIR, start, sfQuality.z, true, tF, ring);
    return key + (hit ? 0.0 : 0.99);
}

void main() {
    vec2 res = passTargetRes.xy;
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec2 uv = v_texcoord0;   // not jittered: nothing averages the frames here now (S24)
    vec3 camO = pjvViewPosition();
    vec3 dir = pjvViewRayDirection(uv, res);

    // The farthest surface in this texel's footprint of trace texels (the sky: none).
    ivec2 tsize = ivec2(textureSize(gPos, 0));
    vec2 ratio = vec2(tsize) / res;
    ivec2 t0 = ivec2(floor(vec2(at) * ratio));
    float tMax = 0.0;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++) {
            if (float(x) >= ratio.x + 0.5 || float(y) >= ratio.y + 0.5) continue;
            float d = texelFetch(gPos, clamp(t0 + ivec2(x, y), ivec2(0, 0), tsize - 1), 0).w;
            tMax = max(tMax, d < 0.0 ? 1e9 : d);
        }

    vec3 skyUp = mtnSkyFrom(camO, vec3(0.0, 1.0, 0.0));
    vec3 groundUp = mtnGroundUp();   // in the clouds' shadow (S23b)
    // Interleaved gradient noise, turned each frame by the golden ratio (S24c): neighbouring texels and successive
    // frames start their steps at different offsets; cloud_temporal.frag averages them.
    float jitter = fract(52.9829189 * fract(dot(vec2(at), vec2(0.06711056, 0.00583715))) + float(int(frameCount.x) % 1024) * 0.61803399);
    float tCloud;
    vec4 cl = sfCloudMarch(camO, dir, tMax, SUN_DIR, mtnSunAir.rgb, skyUp, groundUp, jitter, int(renderParams.y), tCloud);
    vec4 cur = vec4(0.0, 0.0, 0.0, 1.0);
    if (cl.a < 1.0) {
        vec3 T, Lair;
        mtnAtmosphere(camO, dir, tCloud, T, Lair);
        cur = vec4(sfOn(SF_AERIAL) ? cl.rgb * T + Lair * (1.0 - cl.a) : cl.rgb, cl.a);
    }
    float front = sfCloudFront;
    float frontBox = sfCloudFrontBox;

    // This frame's march; cloud_temporal.frag accumulates it (S24c).
    vec4 outC = cur;
    float outFront = front;
    float outBox = frontBox;
    gl_FragData[0] = outC;
    gl_FragData[1] = vec4(outFront, tMax, outBox, farSunShadow(t0, ratio, tsize));   // z: the front box's size (blocky; 0 smooth)
}
