$input v_color0
$input v_texcoord0

// reflect_temporal.frag -- render resolution, after reflect_spatial.frag: the glossy reflection accumulated
// over frames, so the one ray a texel (turned each frame) averages over the lobe while the camera moves,
// not only when it stops (the image's own history, resolve.frag, keeps 16 frames at most while it moves and
// clamps them to a neighbourhood the ray's noise widens).
//
// The history is read where this texel's surface was last frame (gMotion, bilinearly), each of the four
// texels kept only when it holds this same surface (its stored point on this plane, within a few voxels),
// and clamped to this frame's 3x3 (mean +- 1.25 sigma, YCoCg): what the lobe saw a moment ago, unless that
// changed. The history follows the surface, not the reflected point, so a sharp lobe's reflection -- which
// slides across its surface as the camera moves -- would lag: the cap runs from RESTIR_REFLECT_HISTORY
// frames on a lobe at the paths' cap (blurred: it barely slides) to 4 just under the mirrors.
// Mirrors and water pass through, with no history: their reflection is one exact ray, and the image's
// resolve has their own rule.
//
// Inputs: 0 reflectFinal, 1 reflectHistory (last frame: this pass's own, ping-ponged); 2 reflectSpatial;
// 3 gPos, 4 gNormal, 5 gDirect, 6 gMotion (resources).
// Outputs: reflectFinal (as reflection: rgb, a how much it replaces), reflectHistory (xyz the surface, w the
// history's length in frames; 0 none).

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(reflectFinal, 0);
SAMPLER2D(reflectHistory, 1);
SAMPLER2D(reflectSpatial, 2);
SAMPLER2D(gPos, 3);
SAMPLER2D(gNormal, 4);
SAMPLER2D(gDirect, 5);
SAMPLER2D(gMotion, 6);

#define RESTIR_REFLECT_HISTORY 32.0

vec3 restirToYCoCg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 restirFromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 current = texelFetch(reflectSpatial, p, 0);
    vec4 surface = texelFetch(gPos, p, 0);
    float packed = texelFetch(gDirect, p, 0).a;
    vec2 material = restirUnpackMaterial(packed);
    if (current.a <= 0.0 || surface.w < 0.0 || restirIsWater(packed) || material.x >= RESTIR_MIRROR_GLOSSINESS) {
        gl_FragData[0] = current;
        gl_FragData[1] = vec4(surface.xyz, 0.0);
        return;
    }
    vec3 x = surface.xyz;
    vec3 n = normalize(texelFetch(gNormal, p, 0).xyz + vec3(0.0, 1e-6, 0.0));
    float t = surface.w;
    vec2 res = passTargetRes.xy;
    ivec2 last = ivec2(res) - ivec2(1, 1);

    vec3 history = vec3(0.0, 0.0, 0.0);
    float historyLength = 0.0;
    float weight = 0.0;
    if (restirJitter.w < 0.5) {
        vec2 motion = texelFetch(gMotion, p, 0).xy;
        vec2 pix = ((vec2(p) + 0.5) / res + motion) * res - 0.5;
        ivec2 base = ivec2(floor(pix));
        vec2 f = pix - floor(pix);
        float plane = 0.004 * t + 20.0 * restirSettings.y + 2.0 * t * restirSettings.w;
        for (int k = 0; k < 4; k++) {
            ivec2 o = ivec2(k & 1, k >> 1);
            ivec2 q = base + o;
            if (q.x < 0 || q.y < 0 || q.x > last.x || q.y > last.y) continue;
            vec4 was = texelFetch(reflectHistory, q, 0);
            if (was.w <= 0.0 || abs(dot(was.xyz - x, n)) > plane || distance(was.xyz, x) > 4.0 * plane) continue;
            float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) + 1e-4;
            history += texelFetch(reflectFinal, q, 0).rgb * w;
            historyLength += was.w * w;
            weight += w;
        }
    }

    vec3 colour = current.rgb;
    float newLength = 1.0;
    if (weight > 1e-3) {
        history /= weight;
        historyLength /= weight;
        // Clamped to what this frame's neighbourhood allows: light that changed, or a surface newly seen.
        vec3 m1 = vec3(0.0, 0.0, 0.0);
        vec3 m2 = vec3(0.0, 0.0, 0.0);
        for (int y = -1; y <= 1; y++)
            for (int x2 = -1; x2 <= 1; x2++) {
                vec3 c = restirToYCoCg(texelFetch(reflectSpatial, clamp(p + ivec2(x2, y), ivec2(0, 0), last), 0).rgb);
                m1 += c;
                m2 += c * c;
            }
        vec3 mean = m1 / 9.0;
        vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0, 0.0, 0.0)));
        vec3 h = restirToYCoCg(history);
        vec3 d = h - mean;
        vec3 extent = 1.25 * sigma + vec3(1e-4, 1e-4, 1e-4);
        vec3 r = abs(d) / extent;
        float k = max(r.x, max(r.y, r.z));
        if (k > 1.0) h = mean + d / k;
        history = max(restirFromYCoCg(h), vec3(0.0, 0.0, 0.0));

        float cap = mix(RESTIR_REFLECT_HISTORY, 4.0,
                        smoothstep(RESTIR_GI_MAX_GLOSSINESS, RESTIR_MIRROR_GLOSSINESS, material.x));
        newLength = min(historyLength + 1.0, cap);
        colour = mix(history, current.rgb, 1.0 / newLength);
    }
    gl_FragData[0] = vec4(colour, current.a);
    gl_FragData[1] = vec4(x, newLength);
}
