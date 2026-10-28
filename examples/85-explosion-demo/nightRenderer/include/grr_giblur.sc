// =============================================================================
// grr_giblur.sc -- one pass of the ray GI's blur: a 5x5 a-trous filter (B3-spline weights) at
// GIBLUR_STRIDE pixels, stopped at surface changes -- a tap counts only where its surface faces the same
// way and lies on this pixel's plane. Two passes, stride 1 then 3, make a wide, soft, edge-aware blur:
// the large-scale light it is for has no fine detail to lose. A pixel with few frames of history (it
// moved, or was just uncovered) takes more from its neighbours.
//
// The including shader declares giIn (the light, a = frames) and gPos / gNormal, then includes this.
// =============================================================================
#ifndef GRR_GIBLUR_SC
#define GRR_GIBLUR_SC

#include <pjv_engine.sc>   // passTargetRes, passInputRes: engine-set
uniform vec4 giParams;   // y = ray GI on

void main() {
    vec2 pixel = floor(v_texcoord0 * passTargetRes.xy);
    vec2 uv = (pixel + 0.25) * passTargetRes.zw;
    vec4 pos = texture2D(gPos, uv);
    if (pos.w < 0.0 || giParams.y < 0.5) { gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0); return; }
    vec3 N = normalize(texture2D(gNormal, uv).xyz);
    // How far off the plane a tap may lie: a few pixels' worth at this distance.
    float pixelWorld = pos.w * (2.0 * tan(radians(GRR_FOV * 0.5))) / passTargetRes.y;
    float tolerance = pixelWorld * GIBLUR_STRIDE * 1.5 + 0.02;

    vec4 centre = texture2D(giIn, (pixel + 0.5) * passTargetRes.zw);
    vec3 sum = vec3(0.0, 0.0, 0.0);
    float weights = 0.0;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++) {
            vec2 tap = pixel + vec2(float(i), float(j)) * GIBLUR_STRIDE;
            if (tap.x < 0.0 || tap.y < 0.0 || tap.x >= passTargetRes.x || tap.y >= passTargetRes.y) continue;
            vec4 q = texture2D(gPos, (tap + 0.25) * passTargetRes.zw);
            if (q.w < 0.0) continue;
            vec3 nq = texture2D(gNormal, (tap + 0.25) * passTargetRes.zw).xyz;
            float sameFacing = pow(clamp(dot(N, nq), 0.0, 1.0), 16.0);
            float onPlane = 1.0 - smoothstep(tolerance, tolerance * 2.0, abs(dot(q.xyz - pos.xyz, N)));
            float k = (i == 0 ? 0.375 : abs(i) == 1 ? 0.25 : 0.0625) * (j == 0 ? 0.375 : abs(j) == 1 ? 0.25 : 0.0625);
            vec4 light = texture2D(giIn, (tap + 0.5) * passTargetRes.zw);
            // Taps with more history are surer of themselves.
            float w = k * sameFacing * onPlane * (0.25 + min(light.a, 24.0) / 24.0);
            sum += light.rgb * w;
            weights += w;
        }
    gl_FragColor = vec4(weights > 0.0 ? sum / weights : centre.rgb, centre.a);
}

#endif
