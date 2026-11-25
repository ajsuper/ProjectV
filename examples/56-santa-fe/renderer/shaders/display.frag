$input v_color0
$input v_texcoord0

// =============================================================================
// display.frag  --  the accumulated radiance to the screen: exposure, the mountains' grade (55's), a soft
// glow round bright things (the sun, sunlit cloud and snow), a vignette, and the engine's display transform.
//
// Input: 0 accumulated.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_color.sc>
#include <pjv_camera.sc>

SAMPLER2D(accumulated, 0);   // resolve.frag's output
SAMPLER2D(glow, 2);          // glow.frag's light shafts (1: the resolve's history)
SAMPLER2D(bloomUp1, 3);      // the bloom chain's top (bloom.sc: its five levels summed)
SAMPLER2D(gPos, 5);          // the trace pass's (colour 4, gPos 5): how far each pixel sees

uniform vec4 windowRes;
uniform vec4 mtnSun;      // rgb the sun's light, w: exposure
vec3 mtnSunWarm() { return mtnSun.rgb; }
uniform vec4 sfDisplay;   // x: the bloom's share of the image, y: vignette, z: contrast, w: saturation
uniform vec4 sunDir;
uniform vec4 sfFlags;     // y bit 5: the motes
uniform vec4 sfMotes;     // x density (0..1), y size (m), z brightness, w drift (m/s)
uniform vec4 sfTime;      // x seconds
uniform vec4 sfDisplay2;  // x: vibrance (saturation that lifts dull colours more than vivid ones)

// Dust and pollen near the camera (SF2_MOTES; santa-fe-reference S16): motes placed in a grid of cells fixed in
// the world, one or none a cell, each drifting and bobbing on its own; the view ray walks the cells within
// ~25 m, hidden behind what the pixel sees. Each is a soft dot no smaller than a pixel (fainter if it would be),
// lit by the sun: faint against the light, bright looking toward the sun (forward scattering), as dust in a
// sunbeam. Drawn here, after the resolve, so they move crisp rather than smeared by the history.
vec3 motes(vec2 uv, vec2 res, float depth) {
    vec3 o = pjvViewPosition();
    vec3 d = pjvViewRayDirection(uv, res);
    float C = 1.6;
    vec3 stepDir = vec3(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    vec3 invD = vec3(abs(d.x) > 1e-6 ? 1.0 / abs(d.x) : 1e30, abs(d.y) > 1e-6 ? 1.0 / abs(d.y) : 1e30, abs(d.z) > 1e-6 ? 1.0 / abs(d.z) : 1e30);
    vec3 cell = floor(o / C);
    vec3 tNext = abs((cell + max(stepDir, vec3(0.0, 0.0, 0.0))) * C - o) * invD;
    vec3 tDelta = C * invD;
    float pixel = 2.0 * pjvViewTanHalfFov() / res.y;
    float mu = dot(d, sunDir.xyz);
    float g = 0.75;
    float phase = 0.0795775 * (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5);
    vec3 light = vec3(0.0, 0.0, 0.0);
    float t = 0.0;
    for (int i = 0; i < 28; i++) {
        if (t > min(depth, 26.0)) break;
        vec3 h = fract(sin(vec3(dot(cell, vec3(127.1, 311.7, 74.7)), dot(cell, vec3(269.5, 183.3, 246.1)), dot(cell, vec3(113.5, 271.9, 124.6)))) * 43758.5453);
        if (h.x < sfMotes.x) {
            float s = sfTime.x * sfMotes.w;
            vec3 drift = vec3(s, sin(s * 0.7 + h.y * 6.28) * 0.35, s * 0.6) + vec3(sin(s * 0.9 + h.z * 6.28), 0.0, cos(s * 1.1 + h.x * 6.28)) * 0.4;
            vec3 m = (cell + h.yzx) * C + mod(drift, vec3(C, C, C));
            float tm = dot(m - o, d);
            if (tm > 0.3 && tm < depth) {
                float dist = length(o + d * tm - m);
                float r = sfMotes.y * (0.5 + h.z);
                float re = max(r, tm * pixel * 0.6);
                float a = (1.0 - smoothstep(0.0, re, dist)) * (r * r) / (re * re);
                light += vec3(a, a, a) * (1.0 - smoothstep(18.0, 26.0, tm));
            }
        }
        if (tNext.x <= tNext.y && tNext.x <= tNext.z) { t = tNext.x; cell.x += stepDir.x; tNext.x += tDelta.x; }
        else if (tNext.y <= tNext.z) { t = tNext.y; cell.y += stepDir.y; tNext.y += tDelta.y; }
        else { t = tNext.z; cell.z += stepDir.z; tNext.z += tDelta.z; }
    }
    return light * (0.04 + 6.0 * phase) * mtnSunWarm() * sfMotes.z;
}

// The tone curve (santa-fe-reference S24g): identity up to SF_TONE_KNEE, then Reinhard on the peak channel with
// slope 1 at the knee (hue kept, a little desaturated as it rises). The engine's Khronos Neutral shoulder (0.76
// to 1) put everything over about 3x white at 0.97-1: sunlit clouds near the sun were one flat cream. Here 3x
// white is 0.92, 10x 0.97 -- the gradation stays.
#define SF_TONE_KNEE 0.5
#define SF_TONE_DESAT 0.12
vec3 sfTonemap(vec3 c) {
    c = max(c, vec3(0.0, 0.0, 0.0));
    float peak = max(c.r, max(c.g, c.b));
    if (peak <= SF_TONE_KNEE) return c;
    float t = (peak - SF_TONE_KNEE) / (1.0 - SF_TONE_KNEE);
    float newPeak = SF_TONE_KNEE + (1.0 - SF_TONE_KNEE) * t / (1.0 + t);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (SF_TONE_DESAT * (peak - newPeak) + 1.0);
    return mix(c, vec3(newPeak, newPeak, newPeak), g);
}

void main() {
    vec2 uv = v_texcoord0;
    vec3 hdr = texture2D(accumulated, uv).rgb;
    // A meter (SANTA_FE_RAW, sfDisplay2.y): the scene's linear light times the exposure, sRGB-encoded, nothing else
    // -- no bloom, grade, vignette or tone curve -- to measure what the renderer makes before the look.
    if (sfDisplay2.y > 0.5) { gl_FragColor = vec4(clamp(pjvLinearToSrgb(max(hdr * mtnSun.w, vec3(0.0, 0.0, 0.0))), 0.0, 1.0), 1.0); return; }
    // The bloom (S24, restir-pt's): its five levels averaged, mixed in -- a share of the light moved, not added, so
    // every bright thing glows in proportion and nothing gets a halo of its own. Then the light shafts.
    hdr = mix(hdr, texture2D(bloomUp1, uv).rgb * 0.2, sfDisplay.x);
    hdr += texture2D(glow, uv).rgb;
    if ((uint(sfFlags.y + 0.5) & 32u) != 0u) {
        float depth = texture2D(gPos, uv).w;
        hdr += motes(uv, windowRes.xy, depth < 0.0 ? 1e9 : depth);
    }
    hdr = pjvGrade(hdr * mtnSun.w, sfDisplay.z, sfDisplay.w, vec3(0.97, 1.0, 1.04));
    // Vibrance: dull colours gain saturation, vivid ones (the gold aspens, the blue) keep theirs.
    {
        float mx = max(hdr.r, max(hdr.g, hdr.b)), mn = min(hdr.r, min(hdr.g, hdr.b));
        float sat = (mx - mn) / max(mx, 1e-4);
        float luma = dot(hdr, vec3(0.2126, 0.7152, 0.0722));
        hdr = max(mix(vec3(luma, luma, luma), hdr, 1.0 + sfDisplay2.x * (1.0 - sat)), vec3(0.0, 0.0, 0.0));
    }
    vec2 v = uv - 0.5;
    hdr *= 1.0 - sfDisplay.y * dot(v, v) * 1.6;
    gl_FragColor = vec4(clamp(pjvDither(pjvLinearToSrgb(sfTonemap(hdr)), gl_FragCoord.xy), 0.0, 1.0), 1.0);
}
