$input v_color0
$input v_texcoord0

// gi_accumulate.frag -- pass 5, GI resolution: temporal accumulation of the indirect light (SVGF's first
// half), the diffuse and the specular apart.
//
// The pixel's visible point is carried through last frame's camera, the four history texels round it are
// each checked against this surface, and the ones that pass are blended bilinearly. The diffuse history is
// capped at restirDenoise.w frames. The specular's cap follows the roughness: a mirror's reflection moves
// with the view, so it keeps about two frames; a rough lobe's barely does, and keeps as many as the
// diffuse. (A glossy reflection reprojected at the reflected point's depth, not the surface's, would let
// it keep more -- not done.)
//
// It also keeps each one's second moment of luminance, for the variance the a-trous filter scales its
// luminance test by; a pixel with little history takes its 3x3 neighbourhood's variance instead, so a
// new pixel is blurred harder.
//
// Inputs: 0-3 accDiff, accSpec, accMom, accSurf (last frame: this pass's own, ping-ponged); 4 giDiff,
// 5 giSpec (pass 4), 6 surf0, 7 surf1 (pass 2). Outputs: accDiff (rgb, history length), accSpec (rgb,
// history length), accMom (diffuse m2, specular m2, diffuse variance, specular variance), accSurf (x1,
// dir(n1); -1 none) -- this frame's surfaces, for next frame's reprojection.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(accDiff, 0);
SAMPLER2D(accSpec, 1);
SAMPLER2D(accMom, 2);
SAMPLER2D(accSurf, 3);
SAMPLER2D(giDiff, 4);
SAMPLER2D(giSpec, 5);
SAMPLER2D(surf0, 6);
SAMPLER2D(surf1, 7);

// The specular history's cap for a lobe of roughness alpha.
float restirSpecularCap(float alpha) { return mix(2.0, restirDenoise.w, smoothstep(0.0, 0.3, alpha)); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 diffuse = texelFetch(giDiff, p, 0).rgb;
    vec3 specular = texelFetch(giSpec, p, 0).rgb;
    RestirSurface s = restirSurface(texelFetch(surf0, p, 0), texelFetch(surf1, p, 0));
    gl_FragData[3] = vec4(s.x, s.t >= 0.0 ? restirPackDir(s.n) : -1.0);
    if (s.t < 0.0) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[2] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    vec2 res = passTargetRes.xy;
    vec3 diffuseHistory = vec3(0.0, 0.0, 0.0);
    vec3 specularHistory = vec3(0.0, 0.0, 0.0);
    vec2 moments = vec2(0.0, 0.0);
    float historyLength = 0.0;
    float weight = 0.0;
    if (restirDenoise.y > 0.5 && restirJitter.w < 0.5 && frameCount.z < 0.5) {   // the sun moved: start over
        bool nowInFront;
        bool thenInFront;
        vec2 uvNow = restirWorldToUV(s.x, cameraPos.xyz, cameraDir.xyz, res, nowInFront);
        vec2 uvThen = restirWorldToUV(s.x, prevCameraPos.xyz, prevCameraDir.xyz, res, thenInFront);
        vec2 pix = ((vec2(p) + 0.5) / res + (uvThen - uvNow)) * res - 0.5;
        if (thenInFront) {
            ivec2 base = ivec2(floor(pix));
            vec2 f = pix - floor(pix);
            for (int k = 0; k < 4; k++) {
                ivec2 o = ivec2(k & 1, k >> 1);
                ivec2 q = base + o;
                if (q.x < 0 || q.y < 0 || q.x >= int(res.x) || q.y >= int(res.y)) continue;
                vec4 was = texelFetch(accSurf, q, 0);
                if (was.w < 0.0 || !restirSameSurface(was.xyz, restirUnpackDir(was.w), s.x, s.n, s.t)) continue;
                float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
                vec4 hd = texelFetch(accDiff, q, 0);
                diffuseHistory += hd.rgb * w;
                specularHistory += texelFetch(accSpec, q, 0).rgb * w;
                moments += texelFetch(accMom, q, 0).xy * w;
                historyLength += hd.a * w;
                weight += w;
            }
        }
    }
    if (weight > 1e-3) {
        diffuseHistory /= weight;
        specularHistory /= weight;
        moments /= weight;
        historyLength /= weight;
    } else {
        historyLength = 0.0;
    }

    bool accumulate = restirDenoise.y > 0.5;
    float diffuseLength = min(historyLength + 1.0, accumulate ? restirDenoise.w : 1.0);
    float specularLength = min(historyLength + 1.0, accumulate ? restirSpecularCap(pjvBsdfAlpha(s.glossiness)) : 1.0);
    float a = historyLength > 0.0 ? 1.0 / diffuseLength : 1.0;
    float b = historyLength > 0.0 ? 1.0 / specularLength : 1.0;
    diffuseHistory = mix(diffuseHistory, diffuse, a);
    specularHistory = mix(specularHistory, specular, b);
    float ld = restirLum(diffuse);
    float ls = restirLum(specular);
    moments = vec2(mix(moments.x, ld * ld, a), mix(moments.y, ls * ls, b));
    // The first moment is the luminance of the mean: luminance is linear.
    float md = restirLum(diffuseHistory);
    float ms = restirLum(specularHistory);
    vec2 variance = max(moments - vec2(md * md, ms * ms), vec2(0.0, 0.0));
    if (diffuseLength < 4.0) {   // too little history for its moments: the neighbourhood's
        vec4 m = vec4(0.0, 0.0, 0.0, 0.0);
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++) {
                ivec2 q = clamp(p + ivec2(x, y), ivec2(0, 0), ivec2(res) - ivec2(1, 1));
                float lq = restirLum(texelFetch(giDiff, q, 0).rgb);
                float sq = restirLum(texelFetch(giSpec, q, 0).rgb);
                m += vec4(lq, lq * lq, sq, sq * sq);
            }
        m /= 9.0;
        variance = max(variance, vec2(max(m.y - m.x * m.x, 0.0), max(m.w - m.z * m.z, 0.0)) * (4.0 / diffuseLength));
    }
    gl_FragData[0] = vec4(diffuseHistory, diffuseLength);
    gl_FragData[1] = vec4(specularHistory, diffuseLength);
    gl_FragData[2] = vec4(moments, variance);
}
