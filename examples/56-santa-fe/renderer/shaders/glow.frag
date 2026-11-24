$input v_color0
$input v_texcoord0

// =============================================================================
// glow.frag -- the glow round bright things (the sun, sunlit cloud and snow), at a quarter of the window each
// way (santa-fe-reference S12: it was 32 taps a window pixel, and too strong -- it overexposed the image).
//
// Only what is well over the scene's whites contributes, through a soft knee; spread by 9 bilinear taps 2.5
// texels apart (a wide, soft kernel at this resolution). display.frag adds it, faintly.
//
// Input: 0 resolved. Output: rgb the glow.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>
#include <pjv_camera.sc>

SAMPLER2D(resolved, 0);
SAMPLER2D(gPos, 2);   // the trace pass's (its colour 1, gPos 2): w < 0 the sky

uniform vec4 mtnSun;   // w: exposure
uniform vec4 sunDir;
uniform vec4 sfFlags;   // y bit 6: the light shafts
uniform vec4 sfRays;    // x strength, y decay a step, z length (share of the way to the sun)
uniform vec4 sfGlow;   // x: threshold (exposed), y: knee, z: spread (texels between taps)

vec3 bright(vec3 c) {
    c *= mtnSun.w;
    float l = max(c.r, max(c.g, c.b));
    float knee = max(sfGlow.y, 1e-3);
    float threshold = sfGlow.x;
    float soft = clamp(l - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float w = max(soft, l - threshold) / max(l, 1e-4);
    return c * w / mtnSun.w;
}

void main() {
    vec2 texel = passTargetRes.zw;
    vec3 sum = vec3(0.0, 0.0, 0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec2 o = vec2(float(x), float(y));
            float w = exp(-dot(o, o) / 2.0);
            sum += bright(texture2D(resolved, v_texcoord0 + o * texel * sfGlow.z).rgb) * w;
            wsum += w;
        }
    vec3 outC = sum / wsum;
    // Light shafts (SF2_GODRAYS; santa-fe-reference S16): from each pixel toward the sun on screen, the open sky
    // along the way, fading as it goes -- ridges, crowns and the clouds' dark bases cut it into rays. Strongest
    // looking toward the sun, gone looking away.
    if ((uint(sfFlags.y + 0.5) & 64u) != 0u && sunDir.y > -0.05) {
        vec3 camO = pjvViewPosition();
        bool inFront;
        vec2 sunUV = pjvPerspectiveUV(camO + sunDir.xyz * 1.0e6, passTargetRes.xy, camO, pjvViewDirection(), pjvViewTanHalfFov(), inFront);
        float facing = smoothstep(-0.1, 0.7, dot(pjvViewDirection(), sunDir.xyz));
        if (facing > 0.0) {
            vec2 delta = (sunUV - v_texcoord0) * (sfRays.z / 24.0);
            vec2 p = v_texcoord0;
            vec2 gsize = vec2(textureSize(gPos, 0));
            float illum = 1.0, acc = 0.0;
            for (int i = 0; i < 24; i++) {
                p += delta;
                if (any(lessThan(p, vec2(0.0, 0.0))) || any(greaterThan(p, vec2(1.0, 1.0)))) break;
                float sky = texelFetch(gPos, ivec2(p * gsize), 0).w < 0.0 ? 1.0 : 0.0;
                vec3 c = texture2D(resolved, p).rgb * mtnSun.w;
                acc += sky * min(dot(c, vec3(0.2126, 0.7152, 0.0722)), 4.0) * illum;
                illum *= sfRays.y;
            }
            outC += vec3(1.0, 0.93, 0.8) * (acc / 24.0) * sfRays.x * facing / mtnSun.w;
        }
    }
    gl_FragColor = vec4(outC, 1.0);
}
