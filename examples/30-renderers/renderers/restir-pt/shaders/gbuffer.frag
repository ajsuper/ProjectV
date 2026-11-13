$input v_color0
$input v_texcoord0

// gbuffer.frag -- pass 1, render resolution: the camera ray and one sun shadow ray, through one
// restirTrace call site. The shadow ray goes straight at the sun's centre, not to a random point of its
// disc: hard shadows, but no noise in the direct light. (The bounces' sun rays, in sample.frag, still
// sample the disc; their light is denoised.)
//
// Each texel is traced through the centre of one of the window pixels it covers (jitterParams: 2x2 of
// them at half scale, in turn), so over four frames every window pixel is sampled exactly, and a still
// view repeats the same four: the resolve settles to them and stays.
//
// The sun's light is not added here: shadow_soft.frag softens the shadow first (a penumbra sized by the
// distance to what casts it), so this pass hands on whether the sun is seen and, if not, how far away the
// blocker is.
//
// Outputs (in the slots the voxel renderer's reconstruction reads them from):
//   0 gPos     xyz the hit, w distance (< 0 sky)
//   1 gNormal  xyz the face's normal, w the sun's blocker: its distance along the shadow ray, -1 none
//   2 gAlbedo  rgb albedo, a the sun's visibility (1 seen, 0 blocked or facing away)
//   3 gDirect  rgb emission; the sky's radiance for a miss. a the material: restirPackMaterial, plus
//              RESTIR_WATER_BIT for water (RGBA32F, so its seventeen bits are exact)
//   4 gFace    xyz the face's centre, w the voxel's edge (0 sky)
//   5 gRot     the chunk's rotation
//   6 gMotion  xy where the hit was on screen last frame minus where it is now (uv; jitter-free: both are
//              projections of the hit itself), through its chunk's own motion and the camera's -- the motion
//              vectors the resolve reprojects by and the motion blur follows
//
// Input: prevPoses (resource, slot 0): every chunk's pose last frame (restir_trace.sc).

#include <bgfx_shader.sh>
#include <pjv_camera.sc>      // the engine camera: pjvView*, pjvPrevView*
SAMPLER2D(prevPoses, 0);
#define RESTIR_MOTION
#include "restir_trace.sc"

// Overcast's wet ground (pjvOvercast, sunDir.w): only the material written here changes -- no ray. From
// RESTIR_WET_FROM of overcast up, every face turned up is damp (darker, a sheen) and puddles open in it,
// more and wider as the cloud thickens: world-space fractal noise over the ground (RESTIR_PUDDLE_OCTAVES,
// each turned against the last so no axis shows, the finest about a voxel across), a puddle where it passes
// a threshold that falls with the wetness. The edge band is narrow, so the fine octaves fray the shore
// rather than a smooth curve; the same noise above the threshold is the puddle's depth -- a shallow one is
// darker ground showing through and a little rougher, a deep one a still mirror -- and a margin just below
// it is soaked ground around the shore. A puddle has no metal, its glossiness rising through the glossy
// range to RESTIR_PUDDLE_GLOSS, so the specular passes that draw every glossy surface (reflect.frag, its
// filters, the mirrors' sharp ray) draw it too. Nothing tests for a roof: a covered floor gets puddles as an
// open one does.
#define RESTIR_WET_FROM       0.35
#define RESTIR_PUDDLE_SIZE    32.0    // finest voxels across the coarsest octave's noise cell
#define RESTIR_PUDDLE_OCTAVES 6
#define RESTIR_PUDDLE_GLOSS   0.97
float restirPuddleNoise(vec2 p) {
    // fBm, in [0, 1] about 0.5. Each octave turned by ~37 degrees and offset, lacunarity 2.03 (not 2: the
    // lattices never line up).
    float sum = 0.0;
    float amplitude = 0.5;
    float norm = 0.0;
    for (int i = 0; i < RESTIR_PUDDLE_OCTAVES; i++) {
        sum += amplitude * pjvValueNoise(vec3(p.x, 0.0, p.y));
        norm += amplitude;
        p = vec2(0.8 * p.x - 0.6 * p.y, 0.6 * p.x + 0.8 * p.y) * 2.03 + vec2(17.3, 5.9);
        amplitude *= 0.52;
    }
    return 0.5 + 0.5 * sum / norm;
}
void restirWetGround(vec3 x, vec3 n, float finest, inout vec3 albedo, inout float glossiness, inout float metallic) {
    float wet = smoothstep(RESTIR_WET_FROM, 1.0, pjvOvercast());
    if (wet <= 0.0 || n.y < 0.95) return;
    float noise = restirPuddleNoise(x.xz / (finest * RESTIR_PUDDLE_SIZE));
    float threshold = mix(0.72, 0.47, wet);
    float puddle = smoothstep(threshold, threshold + 0.012, noise) * wet;
    float depth = smoothstep(threshold, threshold + 0.12, noise);
    float soaked = smoothstep(threshold - 0.06, threshold, noise) * wet;
    // Damp: wet porous ground darkens and takes a sheen; more so by the shore.
    albedo *= mix(1.0, 0.75, wet) * mix(1.0, 0.8, soaked);
    glossiness = max(glossiness, mix(0.3, 0.5, soaked) * wet);
    // The puddle: water over the floor, the floor showing through where it is shallow.
    albedo *= mix(1.0, mix(0.6, 0.3, depth), puddle);
    glossiness = mix(glossiness, mix(0.8, RESTIR_PUDDLE_GLOSS, depth), puddle);
    metallic *= 1.0 - puddle;
}

void main() {
    vec2 uv = v_texcoord0 + jitterParams.xy * passTargetRes.zw;
    vec3 direction = pjvViewRayDirection(uv, passTargetRes.xy);
    vec3 toSun = normalize(SUN_DIR);

    vec3 origin = pjvViewPosition();
    vec3 rayDir = direction;
    int kind = RESTIR_KIND_CAMERA;
    vec4 position = vec4(pjvViewPosition() + direction * 1.0e4, -1.0);
    vec3 normal = vec3(0.0, 0.0, 0.0);
    vec3 albedo = vec3(0.0, 0.0, 0.0);
    vec3 direct = pathSkyView(direction);   // the sky, its clouds and the sun
    float material = 0.0;
    vec4 face = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 rotation = vec4(0.0, 0.0, 0.0, 1.0);
    float facing = 0.0;
    float visible = 0.0;
    float blocker = -1.0;
    vec3 previous = pjvPrevViewPosition() + direction * 1.0e4;   // the sky: moves with the camera's turn only
    int rays = min(int(restirSettings.x), 2);
    for (int i = 0; i < rays; i++) {
        RestirHit hit = restirTrace(origin, rayDir, kind, 1e30);
        if (kind == RESTIR_KIND_CAMERA) {
            if (!hit.hit) break;
            normal = dot(hit.normal, hit.normal) > 0.5 ? normalize(hit.normal) : -direction;
            position = vec4(pjvViewPosition() + direction * hit.t, hit.t);
            albedo = hit.albedo;
            direct = hit.emission;
            bool water = restirIsWaterMaterial(hit.transparency, hit.glossiness);
            float glossiness = hit.glossiness;
            float metallic = hit.metallic;
            if (!water && hit.transparency <= 0.0) restirWetGround(position.xyz, normal, 20.0 * restirSettings.y, albedo, glossiness, metallic);
            material = restirPackMaterial(glossiness, metallic) + (water ? RESTIR_WATER_BIT : 0.0);
            face = hit.face;
            rotation = hit.rotation;
            previous = hit.previous;
            facing = dot(normal, toSun);
            if (facing <= 0.0) break;
            origin = position.xyz + normal * restirSettings.y;
            rayDir = toSun;
            kind = RESTIR_KIND_SHADOW;
        } else {
            visible = hit.hit ? 0.0 : 1.0;
            blocker = hit.hit ? hit.t : -1.0;
        }
    }
    gl_FragData[0] = position;
    gl_FragData[1] = vec4(normal, blocker);
    gl_FragData[2] = vec4(albedo, visible);
    gl_FragData[3] = vec4(direct, material);
    gl_FragData[4] = face;
    gl_FragData[5] = rotation;
    bool nowInFront;
    bool thenInFront;
    vec2 now = restirWorldToUV(position.xyz, pjvViewPosition(), pjvViewDirection(), passTargetRes.xy, nowInFront);
    vec2 then = restirWorldToUV(previous, pjvPrevViewPosition(), pjvPrevViewDirection(), passTargetRes.xy, thenInFront);
    gl_FragData[6] = vec4(thenInFront ? then - now : vec2(0.0, 0.0), 0.0, 0.0);
}
