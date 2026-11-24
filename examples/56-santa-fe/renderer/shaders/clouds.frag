$input v_color0
$input v_texcoord0

// =============================================================================
// clouds.frag -- the cumulus, at a quarter of the trace pass's texels, accumulated over frames
// (santa-fe-reference S11: the clouds were the costliest part of a sky view, marched again by every texel
// every frame with few long steps, and grainy at their edges).
//
// Each texel marches its view ray through the layer (santa_fe_clouds.sc) to the farthest surface the trace
// pass saw in its footprint, the steps offset per frame, and puts the air in front of the cloud on it. Then it
// blends with its own history, read where the cloud was last frame (reprojected at the cloud's own distance:
// clouds are far, so a turning camera moves them by its turn), unless the surface behind changed depth (a
// mountain's edge moving across it), or a setting changed. A still view converges to many steps a texel; a
// moving one keeps most of them.
//
// Inputs: the trace pass (colour 0, gPos 1, ...), this pass's own last output (clouds 7, cloudDepth 8).
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

SAMPLER2D(gPos, 1);
SAMPLER2D(gNormal, 2);
SAMPLER2D(cloudNoise, 9);
SAMPLER2D(farHeight, 10);
SAMPLER2D(farColour, 11);
SAMPLER2D(farState, 12);
SAMPLER2D(farCanopy, 13);
SAMPLER2D(farMax, 14);
SAMPLER2D(cloudHist, 7);
SAMPLER2D(cloudHistDepth, 8);

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
    vec2 uv = v_texcoord0 + jitterParams.xy * passTargetRes.zw;
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
    vec3 groundUp = MTN_GROUND_ALBEDO * SUN_COLOR * max(SUN_DIR.y, 0.0) * 0.3183;
    // Interleaved gradient noise, turned by the frame: the steps' offsets cover the step over a few frames.
    float jitter = fract(52.9829189 * fract(dot(vec2(at) + float(int(frameCount.x) % 64) * vec2(5.588238, 5.588238), vec2(0.06711056, 0.00583715))));
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

    // The history, where this cloud was last frame.
    vec4 outC = cur;
    float outFront = front;
    float outBox = frontBox;
    if (sfTemporal.x < 0.5) {
        bool nowIn, thenIn;
        // From the texel's centre, not its jittered sample: a still camera then reads its own texel exactly
        // (reading it bilinearly at the jittered point blurred the clouds a little more every frame).
        vec3 dirC = pjvViewRayDirection(v_texcoord0, res);
        vec3 p = camO + dirC * (cl.a < 1.0 ? tCloud : 30000.0);
        vec2 prevUV = pjvPrevViewUV(p, res, thenIn);
        if (thenIn && all(greaterThanEqual(prevUV, vec2(0.0, 0.0))) && all(lessThanEqual(prevUV, vec2(1.0, 1.0)))) {
            vec4 h = texture2D(cloudHist, prevUV);
            vec4 hd = texture2D(cloudHistDepth, prevUV);
            // The surface behind it moved a long way in depth: what the history holds is another ray's.
            float a = max(hd.y, 1.0), b = max(tMax, 1.0);
            bool same = abs(log(min(a, 1e9) / min(b, 1e9))) < 0.2;
            if (same) {
                outC = mix(h, cur, 0.12);
                outBox = front <= hd.x ? frontBox : hd.z;
                outFront = min(hd.x, front);
            }
        }
    }
    gl_FragData[0] = outC;
    gl_FragData[1] = vec4(outFront, tMax, outBox, farSunShadow(t0, ratio, tsize));   // z: the front box's size (blocky; 0 smooth)
}
