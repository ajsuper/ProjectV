$input v_color0
$input v_texcoord0

// =============================================================================
// probes.frag -- the GI probes' update, one fragment a probe (grr_gi.sc has the grid). An eighth of the
// probes each frame, in turn, trace GI_RAYS voxel rays each, in directions turned every update. A ray
// brings back the light arriving along it:
//   a miss    the sky's, in that direction
//   a hit     the light leaving what it hit: the sun on it (one shadow ray from there), the nearest
//             probe's own light there (so each update carries one more bounce: light goes round
//             corners, and a lit wall lights the room), and its emission
// projected onto L1 harmonics and blended into the probe's history. The probes are in the world, not on
// the screen: turning, jitter and moving bodies on screen change nothing they hold, so the light they
// give is steady. A ray that starts inside a solid voxel means the probe is inside the scenery; the
// share of those makes it less usable (grr_gi.sc), and a probe all inside is not used at all.
//
// Every ray and shadow ray goes through ONE call of the march, in a loop: a second inlined march costs
// every ray in the shader (as gbuffer.frag).
//
// Inputs: FBO 12 (its own last output, slots 0..3: the history, unfiltered), FBO 13 (giPacked, the
// filtered probes, slot 4: the light a ray finds where it lands). Outputs (FBO 12, one texel a probe):
// 0-2 the L1 coefficients of red, green, blue; 3 (cell, usable). pack.frag filters them and lays them
// side by side into giPacked. History is kept unfiltered: filtered, each frame would blur it again.
// =============================================================================

#include <bgfx_shader.sh>
#define PJV_NO_ANIMATION
#include <pjv_utils_DDA.sc>
#include <grr_common.sc>

SAMPLER2D(histR,    0);
SAMPLER2D(histG,    1);
SAMPLER2D(histB,    2);
SAMPLER2D(histMeta, 3);
SAMPLER2D(giPacked, 4);
#include <grr_gi.sc>

#define GI_RAYS        6
#define UPDATE_PHASES  8
// The share of the old light an update keeps: steady; and when the light has plainly changed.
#define HYSTERESIS        0.9
#define HYSTERESIS_CHANGE 0.5
// How far a probe ray looks, in probe spacings; it coarsens to 16^3 blocks with distance.
#define RAY_SPACINGS   12.0
#define RAY_STEPS      128u
#define SHADOW_STEPS   96u
#define BIAS_VOXELS    0.3

bool isMiss(SceneIntersectData h) {
    return h.foundBox.size < 0.0 || h.rayT <= 0.0 || dot(h.normal, h.normal) < 0.5;
}

float hash11(float p) { return fract(sin(p * 127.1) * 43758.5453); }

// Ray i of n on a spherical Fibonacci lattice, the lattice turned by `turn`.
vec3 rayDirection(int i, int n, vec3 turn) {
    float golden = 2.39996323;
    float z = 1.0 - (2.0 * float(i) + 1.0) / float(n);
    float r = sqrt(max(1.0 - z * z, 0.0));
    float phi = float(i) * golden + turn.x * 6.2831853;
    vec3 d = vec3(r * cos(phi), r * sin(phi), z);
    // Turn the lattice's pole as well, so successive updates do not share an axis.
    float a = turn.y * 6.2831853, b = turn.z * 3.1415927;
    float ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b);
    d = vec3(d.x, d.y * cb - d.z * sb, d.y * sb + d.z * cb);
    return vec3(d.x * ca - d.z * sa, d.y, d.x * sa + d.z * ca);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec3 slot = ivec3(pixel.x % GI_X, pixel.y, pixel.x / GI_X);
    // The cell this slot holds now: the one in the grid's span whose slot it is.
    ivec3 origin = ivec3(giOrigin.xyz);
    ivec3 cell = origin + giSlot(slot - origin);
    vec4 was0 = texelFetch(histR, pixel, 0), was1 = texelFetch(histG, pixel, 0), was2 = texelFetch(histB, pixel, 0);
    vec4 meta = texelFetch(histMeta, pixel, 0);
    bool known = all(equal(ivec3(meta.xyz), cell));
    int frame = int(giParams.z);
    // Which frame of the cycle updates this probe: by 8x8 block of the texture, so the GPU's waves (a
    // block of fragments each) either all trace or all copy. Scattered, every wave held a few tracing
    // probes and waited on them -- the cost of updating every probe.
    int phase = (pixel.x / 8 + pixel.y / 8 + frame) % UPDATE_PHASES;
    if (giParams.x < 0.5 || (known && phase != 0)) {
        gl_FragData[0] = was0; gl_FragData[1] = was1; gl_FragData[2] = was2;
        gl_FragData[3] = known ? meta : vec4(vec3(cell), 0.0);
        return;
    }

    float spacing = giParams.y;
    vec3 probe = (vec3(cell) + 0.5) * spacing;
    vec3 sun = normalize(sunDir.xyz);
    vec3 turn = vec3(hash11(float(frame) * 0.618 + 0.1), hash11(float(frame) * 0.371 + 0.7), hash11(float(frame) * 0.853 + 0.3));

    vec4 shR = vec4(0.0, 0.0, 0.0, 0.0), shG = shR, shB = shR;
    float inside = 0.0;
    // One loop, one march: each ray, and the shadow ray from its hit when it needs one.
    int ray = 0;
    bool shadowing = false;
    vec3 dir = rayDirection(0, GI_RAYS, turn);
    vec3 radiance = vec3(0.0, 0.0, 0.0), albedo = vec3(0.0, 0.0, 0.0), hitNormal = vec3(0.0, 1.0, 0.0), hitPoint = probe;
    for (int step = 0; step < GI_RAYS * 2; step++) {
        if (ray >= GI_RAYS) break;
        Ray r;
        RayQuery query;
        if (!shadowing) {
            dir = rayDirection(ray, GI_RAYS, turn);
            r.origin = probe;
            r.direction = dir;
            query = pjvShadowQuery(RAY_STEPS, RAY_SPACINGS * spacing);
            pjvQueryLinearLOD(query, 2u, uint(2.0 * spacing / max(lightParams.w, 1e-4)));
        } else {
            r.origin = hitPoint + hitNormal * (BIAS_VOXELS * lightParams.w);
            r.direction = sun;
            query = pjvShadowQuery(SHADOW_STEPS, 1e6);
            pjvQueryLinearLOD(query, 2u, uint(32.0 / max(dot(hitNormal, sun), 0.05)));
        }
        SceneIntersectData found = raySceneIntersectFrom(r, query, 0.0);
        bool done = true;
        if (!shadowing) {
            if (isMiss(found)) {
                radiance = grrSky(dir);
            } else if (found.rayT < 0.02 * spacing) {
                inside += 1.0;                       // started in solid: no light from this one
                radiance = vec3(0.0, 0.0, 0.0);
            } else {
                hitNormal = normalize(found.normal);
                hitPoint = probe + dir * found.rayT;
                VoxelMaterial material = fetchVoxelMaterialFromHit(found);
                albedo = material.albedo;
                float bounceWeight;
                vec3 bounce = giNearest(hitPoint, hitNormal, bounceWeight);
                bounce = mix(grrAmbient(hitNormal) * 0.5, bounce, bounceWeight);
                radiance = albedo * bounce + material.emission;
                if (dot(hitNormal, sun) > 0.0) { shadowing = true; done = false; }
            }
        } else {
            if (isMiss(found)) radiance += albedo * grrSunColor() * (max(dot(hitNormal, sun), 0.0) / GRR_PI);
            shadowing = false;
        }
        if (done) {
            // Onto L1 harmonics: Y00 and the three Y1m, in (y, z, x) order.
            vec4 basis = vec4(0.282095, 0.488603 * dir.y, 0.488603 * dir.z, 0.488603 * dir.x);
            shR += basis * radiance.r;
            shG += basis * radiance.g;
            shB += basis * radiance.b;
            ray++;
        }
    }
    float norm = 4.0 * 3.1415927 / float(GI_RAYS);
    shR *= norm; shG *= norm; shB *= norm;
    float usable = 1.0 - inside / float(GI_RAYS);

    if (known) {
        // Hold most of the old light, less where it has plainly changed (a light came on, a wall fell).
        float before = dot(vec3(was0.x, was1.x, was2.x), vec3(0.2126, 0.7152, 0.0722));
        float now = dot(vec3(shR.x, shG.x, shB.x), vec3(0.2126, 0.7152, 0.0722));
        float change = abs(now - before) / (max(now, before) + 0.05);
        float keep = mix(HYSTERESIS, HYSTERESIS_CHANGE, smoothstep(0.25, 0.6, change));
        shR = mix(shR, was0, keep);
        shG = mix(shG, was1, keep);
        shB = mix(shB, was2, keep);
        usable = mix(usable, meta.w, keep);
    }
    gl_FragData[0] = shR;
    gl_FragData[1] = shG;
    gl_FragData[2] = shB;
    gl_FragData[3] = vec4(vec3(cell), max(usable, 0.06));
}
