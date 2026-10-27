$input v_color0
$input v_texcoord0

// ao_apply.frag -- pass 12, render resolution: the lit frame, for the reconstruction. The SSAO is
// averaged over the 4x4 texels round this one -- its pattern's period -- counting only those at about
// this one's distance, so it does not bleed across silhouettes; then it darkens the indirect light, by
// restirLook.y of the way.
//
// The sun's light joins here, its shadow softened (shadow_soft.frag), and last the distance fog
// (restirFog) and the light shafts (shafts.frag), so the reconstruction and the resolve carry them like any
// other light.
//
// The views (G, restirDenoise.z): 0 the image, 1 the indirect light alone, 2 the direct alone (the sun
// included), 3 the ambient occlusion alone, 4 and 5 the indirect diffuse and specular alone (the composite
// hands on only that one as the indirect light), 6 the light shafts alone. Only the image is fogged and has the shafts.
//
// Inputs: 0 litDirect, 1 litIndirect (composite), 2 ao (g: the distance, < 0 sky), 3 sunLight; 4 shafts,
// 5 shaftDepth, 6 gAlbedo (resources).
// Output: litHDR.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(litDirect, 0);
SAMPLER2D(litIndirect, 1);
SAMPLER2D(ao, 2);
SAMPLER2D(sunLight, 3);
SAMPLER2D(shafts, 4);
SAMPLER2D(shaftDepth, 5);
SAMPLER2D(gAlbedo, 6);

// The light shafts (shafts.frag, GI resolution) here, filtered two ways.
//
// First a 4x4 tent of GI pixels round this texel, each weighted by how near its view ray's length is to
// this one's, so a shaft does not bleed across a silhouette onto what stands in front of it.
//
// Then a radial blur: a shaft is a streak along the line from the sun (its point on screen, or the point
// opposite it when the sun is behind the camera -- shafts converge there), so the traced shafts are
// averaged along that line, towards the sun, with weights that decay. That is where their noise goes:
// along the streak, not across it, so the shadows' edges stay. The march stops at the first tap whose view
// ray is clearly shorter than this one's -- something stands in front there, and the little air in front of
// it says nothing about the air in front of this pixel -- and a tap that sees further counts only the share
// of its air this pixel has (restirShaftReach(t) / restirShaftReach(tq)), so the sky's long column does not
// brighten a mountain in front of it. That share is the hazes' weights, not t / tq: the near haze is all
// within a few dozen voxels, so past that a longer ray has no more of it, and t / tq made a tap seeing the
// sky count as nothing -- lit air with the sky beyond it came out darker than a shadow, whose march stops at
// what casts it.
#define RESTIR_SHAFT_TAPS 16
#define RESTIR_SHAFT_SPAN 0.2      // the march's longest reach, as a share of the screen's height
#define RESTIR_SHAFT_DECAY 0.93    // each tap's weight against the one before
uniform vec4 restirShafts;      // shafts.frag's: x the near haze's strength, y its lambda
uniform vec4 restirShaftsAir;   // x the aerial haze's strength, y its lambda (0: none), z its onset length
// What the hazes can scatter along a view ray t long (shafts.frag's W_near + W_air), the air's onset averaged
// over the ray (min(s / onset, 2) about halves at the ray's middle).
float restirShaftReach(float t) {
    float near = restirShafts.x * (1.0 - exp(-restirShafts.y * t));
    float air = restirShaftsAir.y > 0.0
              ? restirShaftsAir.x * (1.0 - exp(-restirShaftsAir.y * t)) * min(0.5 * t / max(restirShaftsAir.z, 1e-3), 2.0)
              : 0.0;
    return near + air;
}
vec3 restirShaftsHere(ivec2 p, float t) {
    float stride = restirJitter.z;
    vec2 res = vec2(textureSize(shafts, 0));
    vec2 f = (vec2(p) + 0.5) / stride - 0.5;
    ivec2 base = ivec2(floor(f));
    ivec2 last = ivec2(res) - ivec2(1, 1);
    vec3 sum = vec3(0.0, 0.0, 0.0);
    float total = 0.0;
    for (int y = -1; y <= 2; y++)
        for (int x = -1; x <= 2; x++) {
            ivec2 g = clamp(base + ivec2(x, y), ivec2(0, 0), last);
            vec2 d = abs(vec2(base + ivec2(x, y)) - f);
            float w = max(1.0 - d.x * 0.5, 0.0) * max(1.0 - d.y * 0.5, 0.0);
            float tq = texelFetch(shaftDepth, g, 0).x;
            w *= exp(-abs(tq - t) / (0.05 * t + 1e-3));
            sum += texelFetch(shafts, g, 0).rgb * w;
            total += w;
        }
    vec3 centre = total > 1e-6 ? sum / total : vec3(0.0, 0.0, 0.0);

    // The sun's point on screen, in GI pixels (or the antisolar point's).
    vec3 toSun = normalize(SUN_DIR);
    bool inFront;
    vec2 sunUV = restirWorldToUV(cameraPos.xyz + toSun * 1.0e4, cameraPos.xyz, cameraDir.xyz, res, inFront);
    if (!inFront) sunUV = restirWorldToUV(cameraPos.xyz - toSun * 1.0e4, cameraPos.xyz, cameraDir.xyz, res, inFront);
    vec2 here = f + 0.5;
    vec2 toward = sunUV * res - here;
    float reach = sqrt(dot(toward, toward));
    if (reach < 1.0) return centre;
    vec2 direction = toward / reach;
    float step = min(reach, RESTIR_SHAFT_SPAN * res.y) / float(RESTIR_SHAFT_TAPS);
    float offset = fract(52.9829189 * fract(dot(vec2(p), vec2(0.06711056, 0.00583715))) + frameCount.x * 0.618034);

    vec3 radial = centre;
    float weights = 1.0;
    float reachHere = restirShaftReach(t);
    float w = 1.0;
    for (int i = 1; i <= RESTIR_SHAFT_TAPS; i++) {
        vec2 q = here + direction * (step * (float(i) - 1.0 + offset));
        if (q.x < 0.0 || q.y < 0.0 || q.x >= res.x || q.y >= res.y) break;
        ivec2 g = ivec2(floor(q));
        float tq = texelFetch(shaftDepth, g, 0).x;
        if (tq < 0.8 * t) break;
        w *= RESTIR_SHAFT_DECAY;
        radial += texelFetch(shafts, g, 0).rgb * (w * min(reachHere / max(restirShaftReach(tq), 1e-6), 1.0));
        weights += w;
    }
    return radial / weights;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 direct = texelFetch(litDirect, p, 0).rgb + texelFetch(sunLight, p, 0).rgb;
    vec3 indirect = texelFetch(litIndirect, p, 0).rgb;
    float t = texelFetch(ao, p, 0).g;
    float open = 1.0;
    if (t >= 0.0) {
        ivec2 last = ivec2(passTargetRes.xy) - ivec2(1, 1);
        float sum = 0.0;
        float count = 0.0;
        for (int y = -1; y <= 2; y++)
            for (int x = -1; x <= 2; x++) {
                vec2 q = texelFetch(ao, clamp(p + ivec2(x, y), ivec2(0, 0), last), 0).rg;
                if (q.g < 0.0 || abs(q.g - t) > 0.05 * t + 4.0 * restirLook.x) continue;
                sum += q.r;
                count += 1.0;
            }
        open = count > 0.0 ? sum / count : 1.0;
    }
    float shade = mix(1.0, open, restirLook.y);
    vec3 colour = direct + indirect * shade;
    vec2 uv = v_texcoord0 + jitterParams.xy * passTargetRes.zw;   // as the G-buffer traced it (same resolution)
    // How lit the air in front is: the light this surface receives (its colour over its albedo) against the
    // air's own brightness -- a shadowed outdoor wall is about half, an interior next to nothing -- and over
    // a long distance the air is the open air's, whatever is at its end.
    vec3 d = restirViewDirection(uv);
    float received = restirLum(colour) / max(restirLum(texelFetch(gAlbedo, p, 0).rgb), 0.05);
    float lit = smoothstep(0.0, 0.5, received / max(restirLum(pathAir(d)), 1e-4));
    lit = max(lit, smoothstep(0.0, 1.0, t * (restirHaze.x + restirAir.x) / 0.6));   // t over the fog's length
    colour = restirFog(colour, d, t, lit);
    vec3 shaftLight = restirShaftsHere(p, t < 0.0 ? 1.0e4 : t);   // all zero when they are off (L, P)
    colour += shaftLight;
    if (restirView(RESTIR_VIEW_INDIRECT) || restirView(RESTIR_VIEW_DIFFUSE) || restirView(RESTIR_VIEW_SPECULAR)) colour = indirect * shade;
    else if (restirView(RESTIR_VIEW_DIRECT)) colour = direct;
    else if (restirView(RESTIR_VIEW_AO)) colour = vec3(shade, shade, shade);
    else if (restirView(RESTIR_VIEW_SHAFTS)) colour = shaftLight;
    gl_FragColor = vec4(colour, 1.0);
}
