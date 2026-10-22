$input v_color0
$input v_texcoord0

// =============================================================================
// raygi.frag -- the large-scale indirect light: one real voxel ray per pixel, at a quarter of the render
// resolution (one ray per 2x2 render pixels).
//
// The ray leaves the surface in a cosine-weighted direction, turned every frame along a low-discrepancy
// sequence so giaccum's history sees a different direction each frame. What it returns is the light
// arriving along it:
//   a miss    the sky's radiance in that direction -- so a point the sky cannot see goes dark, however
//             far away what blocks it is: the courtyard, under the Ferris wheel, inside the gateway
//   a hit     the light leaving what it hit: the sun on it (one shadow ray from there), a little ambient
//             for the light that surface itself receives, and its emission
// Both rays go through ONE call of the march, in a loop, as gbuffer.frag does: a second inlined march
// costs every ray in the shader.
//
// Inputs: FBO 1 (G-buffer). Output (FBO 7): rgb the radiance along the ray (sky: 0, never read).
// =============================================================================

#include <bgfx_shader.sh>
#define PJV_NO_ANIMATION
#include <pjv_utils_DDA.sc>
#include <grr_common.sc>

SAMPLER2D(gPos,    0);
SAMPLER2D(gNormal, 1);

uniform vec4 passTargetRes;
uniform vec4 frameParams;   // x = frame
uniform vec4 giParams;      // y = ray GI on

#define GI_DISTANCE_VOXELS 192.0   // how far the ray looks, in the finest voxels (24 m of 12.5 cm ones)
#define GI_STEPS        256u
#define SHADOW_STEPS    160u
#define BIAS_VOXELS     0.3
// What a hit surface gets from the sky and its surroundings, as a share of the cheap ambient: the
// second bounce, roughly.
#define HIT_AMBIENT     0.4

bool isMiss(SceneIntersectData h) {
    return h.foundBox.size < 0.0 || h.rayT <= 0.0 || dot(h.normal, h.normal) < 0.5;
}

float ign(vec2 pixel) { return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715)))); }

vec3 cosineDir(float u, float v, vec3 n) {
    float r = sqrt(u), phi = 6.2831853 * v;
    vec3 t = abs(n.y) > 0.999 ? vec3(1.0, 0.0, 0.0) : normalize(cross(vec3(0.0, 1.0, 0.0), n));
    vec3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u)));
}

void main() {
    // The render pixel at this 2x2 block's corner: the same one every frame, so its history lines up.
    vec2 pixel = floor(v_texcoord0 * passTargetRes.xy);
    vec2 uv = (pixel + 0.25) * passTargetRes.zw;
    vec4 pos = texture2D(gPos, uv);
    if (pos.w < 0.0 || giParams.y < 0.5) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    vec4 nv = texture2D(gNormal, uv);
    vec3 N = normalize(nv.xyz);

    // R2 over frames, from a per-pixel start: each pixel walks its own even sequence of directions.
    float frame = mod(frameParams.x, 1024.0);
    float u = fract(ign(pixel) + frame * 0.7548777);
    float v = fract(ign(pixel + vec2(31.0, 17.0)) + frame * 0.5698403);
    vec3 dir = cosineDir(u, v, N);

    Ray ray;
    ray.origin = pos.xyz + N * (BIAS_VOXELS * nv.w);
    ray.direction = dir;
    RayQuery query = pjvShadowQuery(GI_STEPS, GI_DISTANCE_VOXELS * max(lightParams.w, 1e-4));
    vec3 sun = normalize(sunDir.xyz);

    vec3 radiance = grrSky(dir);
    vec3 albedo = vec3(0.0, 0.0, 0.0), hitNormal = vec3(0.0, 1.0, 0.0);
    float emission = 0.0;
    for (int i = 0; i < 2; i++) {
        SceneIntersectData found = raySceneIntersectFrom(ray, query, 0.0);
        if (i == 0) {
            if (isMiss(found)) break;   // the sky
            hitNormal = normalize(found.normal);
            VoxelMaterial material = fetchVoxelMaterialFromHit(found);
            albedo = material.albedo;
            emission = max(max(material.emission.r, material.emission.g), material.emission.b);
            vec3 hitPoint = ray.origin + dir * found.rayT;
            radiance = albedo * (grrAmbient(hitNormal) * HIT_AMBIENT + grrFlashLight(hitPoint, hitNormal) * (0.5 / GRR_PI)) + material.emission;
            float facing = dot(hitNormal, sun);
            if (facing <= 0.0) break;   // facing away from the sun: no shadow ray needed
            ray.origin = ray.origin + dir * found.rayT + hitNormal * (BIAS_VOXELS * found.foundBox.size);
            ray.direction = sun;
            query = pjvShadowQuery(SHADOW_STEPS, 1e6);
            pjvQueryLinearLOD(query, 2u, uint(32.0 / max(facing, 0.05)));
        } else if (isMiss(found)) {
            radiance += albedo * grrSunColor() * (max(dot(hitNormal, sun), 0.0) / GRR_PI);
        }
    }
    gl_FragColor = vec4(radiance, 1.0);
}
