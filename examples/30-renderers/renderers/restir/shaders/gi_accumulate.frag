$input v_color0
$input v_texcoord0

// accum.frag -- pass 5, GI resolution: temporal accumulation of the indirect light (SVGF's first half).
//
// Like accumulate.frag, at GI resolution: the pixel's visible point is carried through last frame's
// camera, the four history texels round it are each checked against this surface, and the ones that
// pass are blended bilinearly. History is capped at restirDenoise.w samples. It also keeps the first two
// moments of luminance, for the variance the a-trous filter scales its luminance test by; a pixel with
// little history takes a 3x3 spatial variance instead, so a new pixel is blurred harder.
//
// Inputs: 0 giIrr (pass 4); 1-4 res0..3 (this frame's visible points); 5-8 accIrr, accMom, accPos, accNrm
// (last frame). Outputs: accIrr (rgb, history length), accMom (m1, m2, variance), accPos, accNrm.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(giIrr, 0);
SAMPLER2D(res2, 3);
SAMPLER2D(res3, 4);
SAMPLER2D(accIrr, 5);
SAMPLER2D(accMom, 6);
SAMPLER2D(accPos, 7);
SAMPLER2D(accNrm, 8);

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 current = texelFetch(giIrr, p, 0).rgb;
    vec4 surface = texelFetch(res2, p, 0);
    vec3 N = restirOctDecode(texelFetch(res3, p, 0).xy);
    vec3 P = surface.xyz;
    float t = surface.w;
    gl_FragData[2] = surface;
    gl_FragData[3] = vec4(N, 0.0);
    float l = restirLum(current);

    if (t < 0.0) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    vec2 res = passTargetRes.xy;
    vec3 colour = vec3(0.0, 0.0, 0.0);
    vec2 moments = vec2(0.0, 0.0);
    float historyLength = 0.0;
    float weight = 0.0;
    if (restirDenoise.y > 0.5 && restirJitter.w < 0.5) {
        bool nowInFront;
        bool thenInFront;
        vec2 uvNow = restirWorldToUV(P, cameraPos.xyz, cameraDir.xyz, res, nowInFront);
        vec2 uvThen = restirWorldToUV(P, prevCameraPos.xyz, prevCameraDir.xyz, res, thenInFront);
        vec2 pix = ((vec2(p) + 0.5) / res + (uvThen - uvNow)) * res - 0.5;
        if (thenInFront) {
            ivec2 base = ivec2(floor(pix));
            vec2 f = pix - floor(pix);
            for (int k = 0; k < 4; k++) {
                ivec2 o = ivec2(k & 1, k >> 1);
                ivec2 q = base + o;
                if (q.x < 0 || q.y < 0 || q.x >= int(res.x) || q.y >= int(res.y)) continue;
                vec4 was = texelFetch(accPos, q, 0);
                if (was.w < 0.0 || !restirSameSurface(was.xyz, texelFetch(accNrm, q, 0).xyz, P, N, t)) continue;
                float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
                vec4 h = texelFetch(accIrr, q, 0);
                colour += h.rgb * w;
                moments += texelFetch(accMom, q, 0).xy * w;
                historyLength += h.a * w;
                weight += w;
            }
        }
    }
    if (weight > 1e-3) {
        colour /= weight;
        moments /= weight;
        historyLength /= weight;
    } else {
        historyLength = 0.0;
    }

    float newLength = min(historyLength + 1.0, restirDenoise.y > 0.5 ? restirDenoise.w : 1.0);
    float a = historyLength > 0.0 ? 1.0 / newLength : 1.0;
    colour = mix(colour, current, a);
    moments = mix(moments, vec2(l, l * l), a);
    float variance = max(moments.y - moments.x * moments.x, 0.0);
    if (newLength < 4.0) {   // too little history for its moments: the neighbourhood's
        vec2 m = vec2(0.0, 0.0);
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++) {
                float lq = restirLum(texelFetch(giIrr, clamp(p + ivec2(x, y), ivec2(0, 0), ivec2(res) - ivec2(1, 1)), 0).rgb);
                m += vec2(lq, lq * lq);
            }
        m /= 9.0;
        variance = max(variance, max(m.y - m.x * m.x, 0.0) * (4.0 / newLength));
    }
    gl_FragData[0] = vec4(colour, newLength);
    gl_FragData[1] = vec4(moments, variance, 0.0);
}
