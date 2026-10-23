$input v_color0
$input v_texcoord0

// composite.frag -- pass 11, render resolution: the two halves of the image. The direct light from the
// G-buffer (emission; the sun joins in ao_apply), and the indirect light: the denoised diffuse times the
// texel's diffuse albedo plus the denoised specular times its specular albedo (pjvSpecularAlbedo, for
// this texel's material and view) -- the demodulation the spatial pass divided out, put back at full
// resolution. Both are upsampled edge-aware: the 2x2 GI pixels round this one, bilinear weights times
// "is that GI pixel's visible point on my surface" (normal and plane). A texel no neighbour matches (a
// thin edge) widens to the 4x4 round it, then falls back to the nearest. They are kept apart for
// ao_apply, which darkens only the indirect.
//
// Mirrors (glossiness RESTIR_MIRROR_GLOSSINESS and up) take their specular from the screen instead: the
// paths see them with their glossiness capped, so what the paths bring is a blurred reflection. A ray
// reflected about the texel's normal is marched through the G-buffer (geometric steps, then a bisection);
// where it meets a surface, that point's light is read from last frame's image, carried through last
// frame's camera. Off the screen, behind something, or facing away, it fades back to the paths' blurred
// reflection. The sun's own highlight on a mirror is the direct light's (shadow_soft.frag), as before.
//
// The views (G): 4 shows the indirect diffuse alone, 5 the indirect specular, as the indirect light.
//
// Inputs, each bound by itself (resourceTexturesIDs): 0 giDiffFinal, 1 giSpecFinal (the last a-trous
// level), 2 surf0, 3 surf1 (the GI pixels' surfaces), 4 gPos, 5 gNormal, 6 gAlbedo, 7 gDirect, 8 resolved
// (last frame's image, window resolution: what the reflection shows).
// Outputs: litDirect, litIndirect.

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(giDiffFinal, 0);
SAMPLER2D(giSpecFinal, 1);
SAMPLER2D(surf0, 2);
SAMPLER2D(surf1, 3);
SAMPLER2D(gPos, 4);
SAMPLER2D(gNormal, 5);
SAMPLER2D(gAlbedo, 6);
SAMPLER2D(gDirect, 7);
SAMPLER2D(resolved, 8);

#define SSR_STEPS 32
#define SSR_REFINE 5

// Where the reflected ray from P along R meets the screen's surfaces: rgb the light there (last frame's),
// a how much to trust it (0: missed).
vec4 restirReflection(vec3 P, vec3 R, float t) {
    vec2 res = passTargetRes.xy;
    float finest = 20.0 * restirSettings.y;
    float reach = 1.5 * t + 64.0 * finest;
    float first = 0.5 * finest;
    float growth = pow(reach / first, 1.0 / float(SSR_STEPS - 1));
    float before = 0.0;
    float along = first;
    float hitAt = -1.0;
    for (int i = 0; i < SSR_STEPS; i++) {
        vec3 Q = P + R * along;
        bool inFront;
        vec2 uv = restirWorldToUV(Q, cameraPos.xyz, cameraDir.xyz, res, inFront);
        if (!inFront || uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0) break;
        vec4 g = texelFetch(gPos, ivec2(uv * res), 0);
        float depth = length(Q - cameraPos.xyz);
        // Behind what the screen shows there, by no more than the step (a surface's thickness): a hit.
        if (g.w >= 0.0 && depth > g.w && depth - g.w < (along - before) * 1.5 + 2.0 * finest) { hitAt = along; break; }
        before = along;
        along *= growth;
    }
    if (hitAt < 0.0) return vec4(0.0, 0.0, 0.0, 0.0);
    float lo = before;
    float hi = hitAt;
    for (int i = 0; i < SSR_REFINE; i++) {
        float mid = 0.5 * (lo + hi);
        bool inFront;
        vec2 uv = restirWorldToUV(P + R * mid, cameraPos.xyz, cameraDir.xyz, res, inFront);
        vec4 g = texelFetch(gPos, ivec2(clamp(uv, 0.0, 0.999) * res), 0);
        if (g.w >= 0.0 && length(P + R * mid - cameraPos.xyz) > g.w) hi = mid;
        else lo = mid;
    }
    bool inFront;
    vec2 uv = restirWorldToUV(P + R * hi, cameraPos.xyz, cameraDir.xyz, res, inFront);
    ivec2 at = ivec2(clamp(uv, 0.0, 0.999) * res);
    vec4 hit = texelFetch(gPos, at, 0);
    if (hit.w < 0.0 || dot(texelFetch(gNormal, at, 0).xyz, R) > 0.0) return vec4(0.0, 0.0, 0.0, 0.0);   // a back face
    vec2 then = restirWorldToUV(hit.xyz, prevCameraPos.xyz, prevCameraDir.xyz, res, inFront);
    if (!inFront || then.x < 0.0 || then.y < 0.0 || then.x >= 1.0 || then.y >= 1.0) return vec4(0.0, 0.0, 0.0, 0.0);
    vec2 size = vec2(textureSize(resolved, 0));
    vec3 light = texelFetch(resolved, ivec2(then * size), 0).rgb;
    // Fade out towards the screen's edges, where the next ray would leave it.
    vec2 edge = min(uv, 1.0 - uv);
    float trust = smoothstep(0.0, 0.08, min(edge.x, edge.y));
    return vec4(light, trust);
}

bool restirMatches(ivec2 g, vec3 P, vec3 N, float t) {
    vec4 gs = texelFetch(surf0, g, 0);
    return gs.w >= 0.0 && restirSameSurface(gs.xyz, restirOctDecode(texelFetch(surf1, g, 0).xy), P, N, t);
}

void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    vec4 surface = texelFetch(gPos, q, 0);
    vec3 N = texelFetch(gNormal, q, 0).xyz;
    vec3 albedo = texelFetch(gAlbedo, q, 0).rgb;
    vec4 direct = texelFetch(gDirect, q, 0);
    if (surface.w < 0.0) {
        gl_FragData[0] = vec4(direct.rgb, 1.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 P = surface.xyz;
    float t = surface.w;
    ivec2 giSize = ivec2(textureSize(giDiffFinal, 0));
    float stride = restirJitter.z;
    vec2 f = (vec2(q) + 0.5) / stride - 0.5;
    ivec2 base = ivec2(floor(f));
    vec2 frac = f - floor(f);

    vec3 sumD = vec3(0.0, 0.0, 0.0);
    vec3 sumS = vec3(0.0, 0.0, 0.0);
    float total = 0.0;
    for (int k = 0; k < 4; k++) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 g = clamp(base + o, ivec2(0, 0), giSize - ivec2(1, 1));
        if (!restirMatches(g, P, N, t)) continue;
        float w = (o.x == 1 ? frac.x : 1.0 - frac.x) * (o.y == 1 ? frac.y : 1.0 - frac.y) + 1e-3;
        sumD += texelFetch(giDiffFinal, g, 0).rgb * w;
        sumS += texelFetch(giSpecFinal, g, 0).rgb * w;
        total += w;
    }
    if (total <= 0.0) {
        for (int y = -1; y <= 2; y++)
            for (int x = -1; x <= 2; x++) {
                ivec2 g = clamp(base + ivec2(x, y), ivec2(0, 0), giSize - ivec2(1, 1));
                if (!restirMatches(g, P, N, t)) continue;
                sumD += texelFetch(giDiffFinal, g, 0).rgb;
                sumS += texelFetch(giSpecFinal, g, 0).rgb;
                total += 1.0;
            }
    }
    if (total <= 0.0) {
        ivec2 g = clamp(ivec2(round(f)), ivec2(0, 0), giSize - ivec2(1, 1));
        sumD = texelFetch(giDiffFinal, g, 0).rgb;
        sumS = texelFetch(giSpecFinal, g, 0).rgb;
        total = 1.0;
    }

    vec2 material = restirUnpackMaterial(direct.a);
    vec3 V = normalize(cameraPos.xyz - P);
    // The paths' light comes back multiplied by what the paths saw: the capped glossiness.
    PjvBsdf paths = pjvBsdf(albedo, min(material.x, RESTIR_GI_MAX_GLOSSINESS), material.y);
    vec3 diffuse = paths.diffuseAlbedo * (sumD / total);
    vec3 specular = pjvSpecularAlbedo(paths, dot(N, V)) * (sumS / total);
    if (material.x >= RESTIR_MIRROR_GLOSSINESS) {
        vec4 reflection = restirReflection(P + N * restirSettings.y, reflect(-V, N), t);
        PjvBsdf mirror = pjvBsdf(albedo, material.x, material.y);
        specular = mix(specular, pjvSpecularAlbedo(mirror, dot(N, V)) * reflection.rgb, reflection.a);
    }
    vec3 indirect = diffuse + specular;
    if (restirView(RESTIR_VIEW_DIFFUSE)) indirect = diffuse;
    else if (restirView(RESTIR_VIEW_SPECULAR)) indirect = specular;

    gl_FragData[0] = vec4(direct.rgb, 1.0);
    gl_FragData[1] = vec4(indirect, 1.0);
}
