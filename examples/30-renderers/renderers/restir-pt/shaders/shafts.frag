$input v_color0
$input v_texcoord0

// shafts.frag -- GI resolution: the sun's light scattered towards the camera by the air -- light shafts --
// in world space, so they come through a skylight or between columns whether or not the sun is on screen.
//
// One shadow ray per GI pixel per frame, from a point on its view ray: the air's contribution is weighted
// by exp(-lambda s) (restirShafts.y: it fades out a few dozen voxels from the camera, so open sunlit air
// is a light haze rather than a wall of glare), and the point is drawn from exactly that falloff, so the
// estimate's only noise is whether the sun is seen there:
//
//   in-scatter = visible(s) * phase(view . sun) * SUN_COLOR * strength * (1 - exp(-lambda t))
//
// with t the distance to what the pixel sees, strength restirShafts.x (the haze's optical depth over the
// falloff) and phase Henyey-Greenstein (restirShafts.z), forward-scattering: shafts glow most looking
// towards the sun. The points are stratified per pixel and per frame, and accumulated here over
// restirShafts.w frames, carried through last frame's camera by the view ray's end point (refused where
// that distance changed: a silhouette); ao_apply upsamples and blurs what is left, depth-aware.
//
// Two hazes scatter it: the shafts' own (above: dense, gone a few dozen voxels out -- light through a
// skylight) and the aerial haze (restirShaftsAir: x its strength at full depth, y its density at the
// camera's height -- thin, but as long as the fog, so the sun streams between ridges a valley away). One
// point per frame still: a haze is picked in proportion to what it can scatter along this ray (W_i =
// strength_i (1 - exp(-lambda_i t))), the point drawn from its falloff, and then the estimate is exactly
// visible(s) * phase * SUN_COLOR * (W_near + W_air) -- the two falloffs' mixture is the pdf.
//
// The shadow ray is the bounces' kind, coarse past 16 voxels: a shaft's edge is soft anyway.
//
// Inputs: 0 shafts, 1 shaftDepth (last frame: this pass's own, ping-ponged), 2 surf0 (resource).
// Outputs: shafts (rgb the in-scattered light, a its history length), shaftDepth (t; 1e4 the sky).

#include <bgfx_shader.sh>
#include <pjv_random.sc>     // pjvInterleavedGradientNoise
#include "restir_trace.sc"

SAMPLER2D(shafts, 0);
SAMPLER2D(shaftDepth, 1);
SAMPLER2D(surf0, 2);

uniform vec4 restirShafts;   // x strength (0: off), y lambda (1 / falloff distance), z phase g, w history cap
uniform vec4 restirShaftsAir; // x the aerial haze's strength, y its lambda (0: none), z its onset length

float restirPhase(float c, float g) {
    float gg = g * g;
    return (1.0 - gg) / (4.0 * RESTIR_PI * pow(max(1.0 + gg - 2.0 * g * c, 1e-4), 1.5));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 surface = texelFetch(surf0, p, 0);
    float t = surface.w < 0.0 ? 1.0e4 : surface.w;   // a miss holds a point 10^4 out along the ray
    vec3 view = normalize(surface.xyz - cameraPos.xyz);
    vec3 toSun = normalize(SUN_DIR);
    if (restirShafts.x <= 0.0 || restirLum(SUN_COLOR) <= 0.0) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[1] = vec4(t, 0.0, 0.0, 0.0);
        return;
    }

    // A point drawn from the falloff (truncated at t), stratified: interleaved gradient noise per pixel,
    // turned by the golden ratio each frame.
    float u = fract(pjvInterleavedGradientNoise(vec2(p)) + frameCount.x * 0.618034);
    float pick = fract(pjvInterleavedGradientNoise((vec2(p)).yx) + frameCount.x * 0.7548777);
    float nearWeight = restirShafts.x * (1.0 - exp(-restirShafts.y * t));
    float airWeight = restirShaftsAir.y > 0.0 ? restirShaftsAir.x * (1.0 - exp(-restirShaftsAir.y * t)) : 0.0;
    float total = nearWeight + airWeight;
    float lambda = pick * total < nearWeight ? restirShafts.y : restirShaftsAir.y;
    float reach = 1.0 - exp(-lambda * t);
    float s = -log(max(1.0 - u * reach, 1e-6)) / lambda;
    RestirHit hit = restirTrace(cameraPos.xyz + view * s, toSun, RESTIR_KIND_SHADOW_INDIRECT, 1e30);
    // The aerial haze thickens with distance as the fog's does (restirFog): its point counts by s over the
    // onset length, so the open air just in front of the camera is clear rather than a sunlit veil.
    float onset = lambda == restirShafts.y ? 1.0 : min(s / max(restirShaftsAir.z, 1e-3), 2.0);
    vec3 light = hit.hit ? vec3(0.0, 0.0, 0.0)
                         : SUN_COLOR * (restirPhase(dot(view, toSun), restirShafts.z) * total * onset);

    // History, through last frame's camera, unless the sun moved (told) or this is the first frame.
    vec2 res = passTargetRes.xy;
    float n = 0.0;
    vec3 history = vec3(0.0, 0.0, 0.0);
    if (restirJitter.w < 0.5 && frameCount.z < 0.5) {
        bool inFront;
        vec2 then = restirWorldToUV(cameraPos.xyz + view * min(t, 1.0e3), prevCameraPos.xyz, prevCameraDir.xyz, res, inFront);
        ivec2 q = ivec2(floor(then * res));
        if (inFront && q.x >= 0 && q.y >= 0 && q.x < int(res.x) && q.y < int(res.y)) {
            float was = texelFetch(shaftDepth, q, 0).x;
            if (abs(was - t) < 0.1 * t) {
                vec4 h = texelFetch(shafts, q, 0);
                history = h.rgb;
                n = h.a;
            }
        }
    }
    n = min(n + 1.0, restirShafts.w);
    gl_FragData[0] = vec4(mix(history, light, 1.0 / n), n);
    gl_FragData[1] = vec4(t, 0.0, 0.0, 0.0);
}
