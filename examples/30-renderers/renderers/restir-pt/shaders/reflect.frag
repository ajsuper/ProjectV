$input v_color0
$input v_texcoord0

// reflect.frag -- render resolution, before the composite: what mirrors (glossiness RESTIR_MIRROR_GLOSSINESS
// and up) and water reflect. The paths see every surface with its glossiness capped, so what they bring a
// mirror is a blurred reflection; this replaces it.
//
// One ray, reflected about the shading normal (gSoftNormal: the waves, on water), finds what is reflected.
// Its light then comes from one of two places:
//   * the screen, where that point is on it: last frame's image (resolved), read bilinearly where the point
//     was last frame -- the real light, shadows and GI included. The point counts as on screen when this
//     frame's G-buffer shows that same point there (within a voxel), not something in front of it;
//   * off the screen or hidden: the ray's own estimate -- the albedo under the sun (one shadow ray), the sky
//     above it, and its emission. Rough, but of the right colour and brightness, so where a reflection
//     leaves the screen it changes a little rather than snapping to the paths' blur.
// The two are blended by how far from the screen's edge the point is. Each texel's answer comes from its
// own ray, not from a march through the screen whose hit or miss turns on a texel's worth of depth, so it
// holds still while the camera moves (screen-space reflection flickered there).
//
// Output: reflection -- rgb the reflected light times the mirror's specular albedo (its Fresnel, for the
// view and the shading normal), a 1 where it replaces the paths' specular (0: not a mirror).
// Inputs (resources): 0 gPos, 1 gNormal, 2 gAlbedo, 3 gDirect, 4 gSoftNormal, 5 resolved (last frame),
// 6 gFace (w: a voxel's edge, the "same point" tolerance).

#include <bgfx_shader.sh>
#include "restir_trace.sc"

SAMPLER2D(gPos, 0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gAlbedo, 2);
SAMPLER2D(gDirect, 3);
SAMPLER2D(gSoftNormal, 4);
SAMPLER2D(resolved, 5);
SAMPLER2D(gFace, 6);

// Last frame's image at uv, bilinearly (it is RGBA32F: filtered here, not by the sampler).
vec3 restirLastFrame(vec2 uv) {
    vec2 size = vec2(textureSize(resolved, 0));
    vec2 pix = uv * size - 0.5;
    ivec2 base = ivec2(floor(pix));
    vec2 f = pix - floor(pix);
    ivec2 last = ivec2(size) - ivec2(1, 1);
    vec3 a = texelFetch(resolved, clamp(base, ivec2(0, 0), last), 0).rgb;
    vec3 b = texelFetch(resolved, clamp(base + ivec2(1, 0), ivec2(0, 0), last), 0).rgb;
    vec3 c = texelFetch(resolved, clamp(base + ivec2(0, 1), ivec2(0, 0), last), 0).rgb;
    vec3 d = texelFetch(resolved, clamp(base + ivec2(1, 1), ivec2(0, 0), last), 0).rgb;
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 surface = texelFetch(gPos, p, 0);
    float packed = texelFetch(gDirect, p, 0).a;
    vec2 material = restirUnpackMaterial(packed);
    if (surface.w < 0.0 || (material.x < RESTIR_MIRROR_GLOSSINESS && !restirIsWater(packed))) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    vec3 P = surface.xyz;
    vec3 face = normalize(texelFetch(gNormal, p, 0).xyz + vec3(0.0, 1e-6, 0.0));
    vec3 N = normalize(texelFetch(gSoftNormal, p, 0).xyz + vec3(0.0, 1e-6, 0.0));
    vec3 V = normalize(cameraPos.xyz - P);
    vec3 R = reflect(-V, N);
    // A wave tilted far enough sends the ray into the face: fold it back out, about the face's plane.
    if (dot(R, face) < 0.0) R = R - 2.0 * dot(R, face) * face;
    vec3 origin = P + face * restirSettings.y;

    vec3 toSun = normalize(SUN_DIR);
    RestirHit hit = restirTrace(origin, R, RESTIR_KIND_BOUNCE_SHARP, 1e30);
    vec3 light;
    if (!hit.hit) {
        float cover;
        light = pathClouds(R, pathSky(R), cover);   // the disc is the direct light's highlight (shadow_soft)
    } else {
        vec3 H = origin + R * hit.t;
        vec3 n = normalize(hit.normal);
        // The ray's own estimate: the sun through a shadow ray, the sky from above, the emission.
        float facing = max(dot(n, toSun), 0.0);
        float sun = 0.0;
        if (facing > 0.0) sun = restirTrace(H + n * restirSettings.y, toSun, RESTIR_KIND_SHADOW_INDIRECT, 1e30).hit ? 0.0 : facing;
        vec3 sky = pathSky(normalize(n + vec3(0.0, 1.0, 0.0))) * (0.5 + 0.5 * n.y);
        vec3 estimate = hit.emission + hit.albedo * (SUN_COLOR * (sun / RESTIR_PI) + sky);
        light = estimate;
        // The screen, where this frame shows the same point: last frame's light there.
        vec2 res = passTargetRes.xy;
        bool inFront;
        vec2 uv = restirWorldToUV(H, cameraPos.xyz, cameraDir.xyz, res, inFront);
        if (inFront && uv.x >= 0.0 && uv.y >= 0.0 && uv.x < 1.0 && uv.y < 1.0) {
            vec4 seen = texelFetch(gPos, ivec2(uv * res), 0);
            float within = 1.5 * texelFetch(gFace, ivec2(uv * res), 0).w + restirSettings.w * seen.w * 2.0;
            vec2 then = restirWorldToUV(H, prevCameraPos.xyz, prevCameraDir.xyz, res, inFront);
            if (seen.w >= 0.0 && length(seen.xyz - H) < within && inFront &&
                then.x >= 0.0 && then.y >= 0.0 && then.x < 1.0 && then.y < 1.0) {
                vec2 edge = min(then, 1.0 - then);
                float trust = smoothstep(0.0, 0.08, min(edge.x, edge.y));
                light = mix(estimate, restirLastFrame(then), trust);
            }
        }
    }
    PjvBsdf mirror = pjvBsdf(texelFetch(gAlbedo, p, 0).rgb, material.x, material.y);
    gl_FragColor = vec4(pjvSpecularAlbedo(mirror, max(dot(N, V), 0.0)) * light, 1.0);
}
