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
// Mirrors (glossiness RESTIR_MIRROR_GLOSSINESS and up) and water take their specular from reflect.frag
// instead: the paths see them with their glossiness capped, so what the paths bring is a blurred
// reflection. The sun's own highlight on a mirror is the direct light's (shadow_soft.frag), as before.
//
// The views (G): 4 shows the indirect diffuse alone, 5 the indirect specular, as the indirect light.
//
// Simple lighting (restirPreset.w: a preset's clean look, with the path passes skipped): no paths, so
// nothing noisy. The diffuse is the sky above the normal (pathSky, opened by how far the normal faces up)
// and the sun's light bounced off the ground below it (an albedo of RESTIR_SIMPLE_GROUND); the specular the same
// sky (a rough reflection of it). The SSAO (ao_apply) shades it into corners, as it does the paths' light.
//
// Inputs, each bound by itself (resourceTexturesIDs): 0 giDiffFinal, 1 giSpecFinal (the last a-trous
// level), 2 surf0, 3 surf1 (the GI pixels' surfaces), 4 gPos, 5 gNormal, 6 gAlbedo, 7 gDirect, 8 reflection
// (reflect.frag: a mirror's specular, a 1 where it replaces the paths').
// Outputs: litDirect, litIndirect.

#include <bgfx_shader.sh>
#include <pjv_camera.sc>      // the engine camera: pjvView*, pjvPrevView*
#include "restir_common.sc"

SAMPLER2D(giDiffFinal, 0);
SAMPLER2D(giSpecFinal, 1);
SAMPLER2D(surf0, 2);
SAMPLER2D(surf1, 3);
SAMPLER2D(gPos, 4);
SAMPLER2D(gNormal, 5);
SAMPLER2D(gAlbedo, 6);
SAMPLER2D(gDirect, 7);
SAMPLER2D(reflection, 8);

#define RESTIR_SIMPLE_GROUND 0.3
#define RESTIR_SIMPLE_SKY 0.6      // the share of the open sky a surface sees, on average: hills and trees hide the rest

bool restirMatches(ivec2 g, vec3 P, vec3 N, float t) {
    vec4 gs = texelFetch(surf0, g, 0);
    return gs.w >= 0.0 && restirSameSurface(gs.xyz, pjvOctDecode(texelFetch(surf1, g, 0).xy), P, N, t);
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
    if (restirPreset.w > 0.5) {
        vec2 mat = restirUnpackMaterial(direct.a);
        vec3 V = normalize(pjvViewPosition() - P);
        vec3 n = normalize(N + vec3(0.0, 1e-6, 0.0));
        vec3 sunLight = SUN_COLOR * max(normalize(SUN_DIR).y, 0.0);
        vec3 sky = RESTIR_SIMPLE_SKY * pathSky(normalize(n + vec3(0.0, 1.0, 0.0))) * (0.5 + 0.5 * n.y);
        vec3 ground = sunLight * (RESTIR_SIMPLE_GROUND / RESTIR_PI) * (0.5 - 0.5 * n.y);
        PjvBsdf bsdf = pjvBsdf(albedo, mat.x, mat.y);
        vec3 simpleDiffuse = bsdf.diffuseAlbedo * (sky + ground);
        vec3 simpleSpecular = pjvSpecularAlbedo(bsdf, max(dot(n, V), 0.0)) * sky;   // the soft sky: no streaks of blue
        vec4 mirror = texelFetch(reflection, q, 0);
        simpleSpecular = mix(simpleSpecular, mirror.rgb, mirror.a);
        vec3 simple = simpleDiffuse + simpleSpecular;
        if (restirView(RESTIR_VIEW_DIFFUSE)) simple = simpleDiffuse;
        else if (restirView(RESTIR_VIEW_SPECULAR)) simple = simpleSpecular;
        gl_FragData[0] = vec4(direct.rgb, 1.0);
        gl_FragData[1] = vec4(simple, 1.0);
        return;
    }
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
    vec3 V = normalize(pjvViewPosition() - P);
    // The paths' light comes back multiplied by what the paths saw: the capped glossiness.
    PjvBsdf paths = pjvBsdf(albedo, min(material.x, RESTIR_GI_MAX_GLOSSINESS), material.y);
    vec3 diffuse = paths.diffuseAlbedo * (sumD / total);
    vec3 specular = pjvSpecularAlbedo(paths, dot(N, V)) * (sumS / total);
    vec4 mirrored = texelFetch(reflection, q, 0);
    specular = mix(specular, mirrored.rgb, mirrored.a);
    vec3 indirect = diffuse + specular;
    if (restirView(RESTIR_VIEW_DIFFUSE)) indirect = diffuse;
    else if (restirView(RESTIR_VIEW_SPECULAR)) indirect = specular;

    gl_FragData[0] = vec4(direct.rgb, 1.0);
    gl_FragData[1] = vec4(indirect, 1.0);
}
