// trace.sc -- the trace pass's body (trace.frag, and trace_lean.frag with SF_LEAN: see there).

// =============================================================================
// trace.frag  --  the Santa Fe reference renderer's one real pass (plan R1-R6), full resolution.
//
// One path per pixel per frame; accumulate.frag averages the frames while nothing changes. Per pixel:
//
//   0  the camera ray, jittered inside the pixel (antialiasing), from where the depth pre-pass says the
//      ground can begin; voxels first, then the far field past them
//   1  a shadow ray from its hit toward a random point of the sun's disc (soft shadows)
//   2  a cosine-weighted diffuse ray from its hit (sky occlusion and one bounce)
//   3  a shadow ray from the diffuse ray's hit (the bounce is lit by the sun only where the sun reaches)
//   4  a shadow ray from a random point along the camera ray (the haze's light shafts)
//   5  the sun's ray from 1 on past foliage that let it partly through (what is behind the leaves)
//
// All five through ONE voxel-march call site and one far-field march (the shader library's rule: a second
// inlined march doubles the cost of every mode). Then the clouds in front of the hit (santa_fe_clouds.sc),
// their shadows on everything the sun lights, and the air (mountains_sky.sc).
//
// Smooth surfaces (R6): a hit on a palette entry whose `metallic` byte is set (rock, boulders, snow; the
// amount) takes its shading normal from the gradient of the voxel occupancy round it.
//
// Glints and sheen (santa-fe-reference S21), all at render time from where a ray meets a voxel's face (nothing
// is stored): each face of snow and rock is a 16 x 16 grid, a few of its cells -- alone, or in sections of a
// few cells -- small mirrors tilted at random; they reflect the sun's disc (through the sun's shadow ray) and,
// through reflect.frag, what is on the screen and the sky. Snow, rock and leaves also have a faint rough sheen
// of the sun (an exact GGX lobe) and the sky (along the mirror direction, blurred by reflect.frag): nothing
// random (S22e). The tunables are the SF_GLINT_ and SF_SHEEN_ defines below.
//
// Output: rgb the radiance (linear, before exposure); gGloss (target 7): xyz the reflected ray's direction
// times 1 + its roughness (1: a glint's mirror), w its weight (0: nothing reflects here), for reflect.frag.
// =============================================================================

#include <bgfx_shader.sh>

#define PJV_SCENE_GRIDS_FIRST   // props: many small loose chunks over grid terrain (pjv_scene.sc)
#include <pjv_scene.sc>
#include <pjv_camera.sc>

uniform vec4 frameCount;     // x: frame index
uniform vec4 jitterParams;   // xy: this frame's offset inside the render texel, in texels (the reconstruction's too)
uniform vec4 renderParams;   // x: sun disc scale, y: cloud steps, z: preview (moving: cheaper), w: smooth ground
uniform vec4 sfSSS;          // leaves: x glow through them, y how far light gets through a crown (m), z wrap, w highlight
uniform vec4 sfSnow;         // x: the snowline (m)
uniform vec4 sfShadowLOD;    // the sun's shadow ray: x voxels for this far (m), the far field's heightfield past it; y coarse past this many voxels, z props (loose objects) this far (m)
uniform vec4 mtnFog;         // x: haze extinction per m at its base, y: base (m), z: scale height (m), w: g
uniform vec4 sfFog2;         // the second layer: x extinction per m at its base, y base (m), z scale height (m)
uniform vec4 sfFog3;         // x start distance (m), y max opacity, z the sky's light in it, w the sun's (directional inscattering)
uniform vec4 sfFogTint;      // rgb the inscattering tint, w albedo

SAMPLER2D(preDepth, 0);
SAMPLER2D(farHeight, 1);
SAMPLER2D(farColour, 2);
SAMPLER2D(farState, 3);
SAMPLER2D(cloudNoise, 4);
SAMPLER2D(farCanopy, 5);
SAMPLER2D(cloudShadowMap, 6);   // cloud_shadow.frag
#define SF_HAS_CLOUD_SHADOW_MAP 1
#define SF_SKY_TEX_SLOT 7           // sky_ambient.frag: the sky's light by facing, and the overcast (S23)

#define PI 3.14159265358979

#include <mountains_sky.sc>
#include <mountains_far.sc>
#include <santa_fe_clouds.sc>
#include <pjv_microfacet.sc>   // pjvSampleGgxHalf

// ---- Glints and sheen: the tunables (S21) ------------------------------------------------------------
// Glints: a voxel face is a SF_GLINT_GRID x SF_GLINT_GRID grid of cells. A cell is a mirror when its own
// hash says so (CELLS: the share of single cells), or when it lies in a mirror section -- a square of
// SF_GLINT_SECTION cells (SECTIONS: the share of them), of which FILL of the cells are mirrors, all tilted
// alike (they flash together). Each mirror is tilted up to TILT degrees off the surface at random.
#define SF_GLINT_GRID             16.0
#define SF_GLINT_SECTION          4.0
// Rock goes from DULL (shine 0: granite, limestone) to SHINY (shine SF_ROCK_SHINY and up: mica, pyrite, quartz,
// quartzite, slate) -- a rock's shine is its palette glossiness (main.cpp; the boulders' roles from their brush).
#define SF_ROCK_SHINY             0.6
#define SF_GLINT_CELLS_SNOW       0.04
#define SF_GLINT_CELLS_ROCK_DULL  0.02
#define SF_GLINT_CELLS_ROCK_SHINY 0.12
#define SF_GLINT_SECTIONS_SNOW    0.05
#define SF_GLINT_SECTIONS_ROCK_DULL  0.03
#define SF_GLINT_SECTIONS_ROCK_SHINY 0.2
#define SF_GLINT_FILL             0.55
#define SF_GLINT_TILT             14.0     // degrees
// The sun in a mirror: its disc as the sky draws it (SF_GLINT_SUN times the sun's irradiance, mountains_sky.sc
// draws 40), seen SF_GLINT_SUN_ANGLE degrees across (wider than the real disc: the mirrors are not flat) with
// a soft rim of SF_GLINT_SUN_SOFT, times the mirror's Fresnel (F0: ice, mica and quartz).
#define SF_GLINT_SUN              40.0
#define SF_GLINT_SUN_ANGLE        1.2      // degrees, half-angle
#define SF_GLINT_SUN_SOFT         0.6      // degrees
#define SF_GLINT_F0_SNOW          0.03
#define SF_GLINT_F0_ROCK_DULL     0.06
#define SF_GLINT_F0_ROCK_SHINY    0.15
// At most this bright (exposed): under the glow's threshold (3, its knee 1), so a glint barely blooms.
#define SF_GLINT_MAX_EXPOSED      1.8
// What else a mirror cell reflects (the screen and the sky, reflect.frag), times its Fresnel.
#define SF_GLINT_ENV              1.0
#define SF_GLINT_MAX_DISTANCE     300.0    // m: past this no glints (fading from 0.4 of it)
// Sheen: a rough reflection of the sky (and the screen) on snow, rock and leaves -- GGX roughness ALPHA,
// strength times a rough surface's Schlick Fresnel from F0 (at grazing it rises only to 1 - ALPHA: a rough
// surface never turns into a mirror).
#define SF_SHEEN_SNOW             0.22
#define SF_SHEEN_ROCK_DULL        0.2
#define SF_SHEEN_ROCK_SHINY       0.9
#define SF_SHEEN_LEAF             0.2
#define SF_SHEEN_ALPHA_SNOW       0.5
#define SF_SHEEN_ALPHA_ROCK_DULL  0.6
#define SF_SHEEN_ALPHA_ROCK_SHINY 0.25
#define SF_SHEEN_ALPHA_LEAF       0.5
#define SF_SHEEN_F0               0.04
#define SF_SHEEN_SUN              1.0      // the sun's highlight in the sheen, times its exact GGX lobe (S22e)
#define SF_SHEEN_MAX_DISTANCE     1500.0   // m: past this no sheen (fading from 0.5 of it)

uint sfHash3(ivec3 c, uint salt) {
    uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ salt * 2654435761u;
    h = h * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    return (h >> 22u) ^ h;
}
float sfHashUnit(uint h) { return float(h >> 8u) * (1.0 / 16777216.0); }

// ---- Random numbers ----------------------------------------------------------------------------------
SF_GLOBAL uint sfState = 0u;
uint sfPcg() {
    sfState = sfState * 747796405u + 2891336453u;
    uint w = ((sfState >> ((sfState >> 28u) + 4u)) ^ sfState) * 277803737u;
    return (w >> 22u) ^ w;
}
float sfRand() { return float(sfPcg() >> 8u) * (1.0 / 16777216.0); }

// How far this pixel's camera ray may skip: a little short of the nearest of the nine pre-pass rays
// round it (a pre-pass texel is 8 x 8 pixels).
float skipDistance(vec2 fragCoord) {
    ivec2 size = ivec2(textureSize(preDepth, 0));
    float nearest = 1e9;
    ivec2 c = ivec2(fragCoord * vec2(size) / passTargetRes.xy);   // the pre-pass texel this one falls in
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            nearest = min(nearest, texelFetch(preDepth, clamp(c + ivec2(x, y), ivec2(0, 0), size - 1), 0).x);
    if (nearest >= 1e8) return -1.0;   // all nine went to the sky: so does this one (S27, -1.1 ms in the forest)
    return max(0.0, nearest * 0.96 - 2.0);
}

vec3 sfCosineDir(vec3 n, float u1, float u2) {
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    vec3 t = abs(n.y) < 0.99 ? normalize(cross(n, vec3(0.0, 1.0, 0.0))) : normalize(cross(n, vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

// A direction toward a random point of the sun's disc (angular radius ~0.27 degrees).
vec3 sfSunSample(float u1, float u2) {
    vec3 n = SUN_DIR;
    vec3 t = abs(n.y) < 0.99 ? normalize(cross(n, vec3(0.0, 1.0, 0.0))) : vec3(1.0, 0.0, 0.0);
    vec3 b = cross(n, t);
    float r = 0.0047 * renderParams.x * sqrt(u1), phi = 2.0 * PI * u2;
    return normalize(n + t * (r * cos(phi)) + b * (r * sin(phi)));
}

// The shading normal of a smooth voxel surface: the gradient of occupancy over a sphere of radius ~2.3
// voxels round the hit voxel, probed in its own chunk. A probe outside the chunk counts as solid below
// and empty above (the ground; props have room round them).
vec3 sfSmoothNormal(PjvSceneIntersectData h, vec3 faceN) {
    PjvChunkHeader hd = pjvHeader(int(h.headerIndex));
    ivec3 c = h.voxelCoord;
    int res = int(hd.resolution);
    vec3 g = vec3(0.0, 0.0, 0.0);
    float reach2 = sfQuality.x > 1.5 ? 5.5 : 3.5;   // radius 2 (93 probes) or 1 (26)
    int r = sfQuality.x > 1.5 ? 2 : 1;
    // In opposite pairs (o, -o): where either falls outside the hit's chunk, the pair is left out -- a guess
    // there (55's "solid below, empty above") drew bright lines along every chunk seam.
    for (int z = 0; z <= 2; z++)
        for (int y = -2; y <= 2; y++)
            for (int x = -2; x <= 2; x++) {
                if (z == 0 && (y < 0 || (y == 0 && x <= 0))) continue;   // one of each pair
                if (abs(x) > r || abs(y) > r || z > r) continue;
                vec3 o = vec3(float(x), float(y), float(z));
                float d2 = dot(o, o);
                if (d2 > reach2) continue;
                ivec3 qa = c + ivec3(x, y, z), qb = c - ivec3(x, y, z);
                if (any(lessThan(qa, ivec3(0, 0, 0))) || any(greaterThanEqual(qa, ivec3(res, res, res))) ||
                    any(lessThan(qb, ivec3(0, 0, 0))) || any(greaterThanEqual(qb, ivec3(res, res, res)))) continue;
                uint off;
                float occA = pjvProbeTree(hd.geometryStartIndex, hd.leafStartIndex, hd.resolution, qa, off) ? 1.0 : 0.0;
                float occB = pjvProbeTree(hd.geometryStartIndex, hd.leafStartIndex, hd.resolution, qb, off) ? 1.0 : 0.0;
                g += o * ((occA - occB) * exp(-d2 * 0.35));
            }
    if (dot(g, g) < 1e-6) return faceN;
    vec3 n = pjvRotationFromQuat(hd.rotation) * normalize(-g);
    // Never turned more than ~80 degrees from the face the ray actually entered.
    if (dot(n, faceN) < 0.15) n = normalize(n + faceN * (0.15 - dot(n, faceN) + 0.05));
    return n;
}

// Rounded outside edges (after restir-pt's normal_blur; analytic here, where the shading is): within sfBevel.x
// of a voxel's edge on its face, the shading normal leans toward that side -- by sfBevel.y, fading to nothing at
// the width -- where the neighbouring voxel that way is empty: an outside edge. Round an inside corner the
// neighbour is solid, and it stays crisp. Probes outside the chunk count as solid (no rounding at its seams).
vec3 sfBevelNormal(PjvSceneIntersectData h, vec3 p, vec3 faceN, vec3 n) {
    PjvChunkHeader hd = pjvHeader(int(h.headerIndex));
    mat3 R = pjvRotationFromQuat(hd.rotation);
    float vs = hd.scale / float(hd.resolution);
    vec3 local = (p - vec3(hd.positionX, hd.positionY, hd.positionZ)) * R / vs;   // row-vector: transpose(R) * d
    vec3 f = local - vec3(h.voxelCoord);
    vec3 nl = faceN * R;
    int res = int(hd.resolution);
    vec3 lean = vec3(0.0, 0.0, 0.0);
    float width = max(sfBevel.x, 1e-3);
    for (int a = 0; a < 3; a++) {
        float na = a == 0 ? nl.x : (a == 1 ? nl.y : nl.z);
        if (abs(na) > 0.5) continue;   // the face's own axis
        float u = clamp(a == 0 ? f.x : (a == 1 ? f.y : f.z), 0.0, 1.0);
        float side = u < 0.5 ? -1.0 : 1.0;
        float d = min(u, 1.0 - u);
        if (d >= width) continue;
        ivec3 e = ivec3(a == 0 ? 1 : 0, a == 1 ? 1 : 0, a == 2 ? 1 : 0) * int(side);
        ivec3 q = h.voxelCoord + e;
        if (any(lessThan(q, ivec3(0, 0, 0))) || any(greaterThanEqual(q, ivec3(res, res, res)))) continue;
        uint off;
        if (pjvProbeTree(hd.geometryStartIndex, hd.leafStartIndex, hd.resolution, q, off)) continue;   // an inside edge
        float w = 1.0 - d / width;
        lean += vec3(e) * (w * w * sfBevel.y);
    }
    if (dot(lean, lean) <= 0.0) return n;
    return normalize(n + R * lean);
}

struct Surf {
    bool hit;
    bool far;
    float t;
    vec3 p;
    vec3 n;        // shading normal
    vec3 nf;       // the face (or the far field's own) normal
    vec3 albedo;
    float edge;    // the voxel's edge (m); a far hit: its texel
    float gloss;
    float trans;   // foliage: light through the leaves
    bool smooth;
    float sparkle; // sun glints: 1 snow, 0.35 rock (mica, quartz), 0 none
    bool rock;     // rock: terrain rock and the boulders (S21)
    float shine;   // a rock's shine, 0..0.9: its palette glossiness (main.cpp; the boulders' from their brush)
    vec4 face;     // a voxel's face centre and edge (0: not a voxel) -- the reconstruction's
    vec4 rot;      // its chunk's rotation
};

Surf sfNoSurf() {
    Surf s;
    s.hit = false; s.far = false; s.t = -1.0;
    s.p = vec3(0.0, 0.0, 0.0); s.n = vec3(0.0, 1.0, 0.0); s.nf = s.n;
    s.albedo = vec3(0.0, 0.0, 0.0); s.edge = 1.0; s.gloss = 0.0; s.trans = 0.0; s.smooth = false; s.sparkle = 0.0; s.rock = false; s.shine = 0.0;
    s.face = vec4(0.0, 0.0, 0.0, 0.0); s.rot = vec4(0.0, 0.0, 0.0, 1.0);
    return s;
}

// Where a shadow ray from s starts: off the surface by a share of its voxel, over it if the face turns
// from the sun, and further for a smooth surface (whose shading normal may face the sun where its
// voxel's face does not).
// Where a point sits on its voxel face, as a leaf-glow multiplier (40-advanced-renderer's foliageEdgeGlow):
// 0.7 at the face's centre rising to 1 at its rim (0.35 drew every voxel's outline once the glow spread) (squared), the rim on the sun's side brighter. Light
// that entered a leaf's far side reaches the near side's edge sooner than its middle.
float sfLeafEdgeGlow(vec3 P, vec3 N, vec3 faceCentre, float edge) {
    vec3 d = P - faceCentre;
    d -= N * dot(d, N);
    float halfEdge = max(edge * 0.5, 1e-6);
    vec3 an = abs(N);
    vec3 t = abs(d) / halfEdge;
    float rim = clamp(max(max(t.x * (1.0 - an.x), t.y * (1.0 - an.y)), t.z * (1.0 - an.z)), 0.0, 1.0);
    vec3 Lt = SUN_DIR - N * dot(SUN_DIR, N);
    float ltLen = sqrt(max(dot(Lt, Lt), 0.0));
    float along = dot(d, Lt) / max(halfEdge * ltLen, 1e-6);
    float skew = clamp(0.5 + 0.5 * along, 0.0, 1.0);
    float ramp = rim * rim * mix(0.45, 1.0, skew);
    return mix(0.7, 1.0, clamp(ramp, 0.0, 1.0));
}

// How snowy the ground is at xz (0..1): the finest far ring's ground colour there, as white and grey-free as
// the snow's (sRGB, x 0.84 like every far colour: ~0.78) rather than sand's or pale rock's.
float sfSnowAt(vec2 xz) {
    for (int f = mtnFarFirstRing(xz); f < MTN_FAR_RINGS; f++) {
        float h, cover, treeH;
        bool conifer;
        if (!mtnFarSampleIn(f, xz, h, cover, treeH, conifer)) continue;
        vec3 c = mtnFarColour(f, xz);
        float lo = min(min(c.r, c.g), c.b), hi = max(max(c.r, c.g), c.b);
        return smoothstep(0.5, 0.68, lo) * (1.0 - smoothstep(0.15, 0.28, hi - lo));
    }
    return 0.0;
}

// The far field's sky light, occluded by the land round it (santa-fe-reference S22: the AO rays see only voxels, so
// distant hills were lit as if on a plain). A horizon AO on the heightfield: SF_FAR_AO_DIRS directions, in each the
// highest the ground and its canopy rise above this point's tangent plane at SF_FAR_AO_STEPS distances (from a few
// pixels' worth of ground, each SF_FAR_AO_GROWTH times the last); what of the sky that horizon leaves, averaged.
#define SF_FAR_AO_DIRS      6
#define SF_FAR_AO_STEPS     3
#define SF_FAR_AO_GROWTH    3.0
#define SF_FAR_AO_NEAREST   20.0   // m: the first step at least this far (and at least four pixels' ground)
#define SF_FAR_AO_STRENGTH  1.0
float sfFarAO(vec3 p, vec3 n, float pixel) {
    float r0 = max(pixel * 4.0, SF_FAR_AO_NEAREST);
    float vis = 0.0;
    for (int k = 0; k < SF_FAR_AO_DIRS; k++) {
        float a = (float(k) + 0.5) * (6.2831853 / float(SF_FAR_AO_DIRS));
        vec2 d = vec2(cos(a), sin(a));
        float horizon = 0.0;   // the sine of the highest rise above the tangent plane
        float r = r0;
        for (int j = 0; j < SF_FAR_AO_STEPS; j++) {
            float h, cover, treeH;
            bool conifer;
            int ring;
            if (mtnFarSample(p.xz + d * r, h, cover, treeH, conifer, ring)) {
                vec3 v = vec3(d.x * r, h + cover * treeH * 0.7 - p.y, d.y * r);
                horizon = max(horizon, dot(v, n) / max(length(v), 1e-3));
            }
            r *= SF_FAR_AO_GROWTH;
        }
        vis += 1.0 - horizon;
    }
    return mix(1.0, vis / float(SF_FAR_AO_DIRS), SF_FAR_AO_STRENGTH);
}

// The sky light with no diffuse ray (SF2_SKY_LIGHT): sky_ambient.frag's cube (uniforms, mountains_sky.sc).
#define SF_SKY_LIGHT 1.0   // its strength (the diffuse ray's own is 1: the cube is the hemisphere's cosine-weighted mean)

vec3 sfShadowOrigin(Surf s, vec3 L, float pixel) {
    if (s.far) return s.p + (s.n + vec3(0.0, 1.0, 0.0)) * (s.edge * 0.5);
    // From the face's centre, not the hit: every texel that sees a face then gets the same answer, and the
    // reconstruction rebuilds it exactly on the first frame (a per-hit origin made each face's texels
    // disagree, and the resolve was seen averaging them as the jitter moved).
    vec3 o = (s.face.w > 0.0 && sfOn(SF_FACE_SHADING) ? s.face.xyz : s.p) + s.nf * (s.edge * 0.05);
    if (dot(s.nf, L) <= 0.05) o += vec3(0.0, s.edge * 0.55, 0.0);
    float small = 1.0 - smoothstep(1.5, 3.5, s.edge / max(pixel, 1e-6));
    o += (vec3(0.0, 1.0, 0.0) + L) * (s.edge * 1.1 * small);
    // A smoothed surface's shading normal can face the sun where its voxel steps do not: start clear of
    // the next step up (else each step's riser draws a dark contour line on the tread below it).
    if (s.smooth) o += (s.n + vec3(0.0, 1.0, 0.0)) * (s.edge * 1.6) + L * (s.edge * 1.5);
    return o;
}

void main() {
    vec2 res = passTargetRes.xy;
    ivec2 pix = ivec2(gl_FragCoord.xy);
    sfState = uint(pix.x) * 1973u + uint(pix.y) * 9277u + uint(frameCount.x) * 26699u;
    sfState = sfState ^ (sfState >> 13u);
    sfPcg();
    bool preview = renderParams.z > 0.5;

    // One offset for the whole frame (a Halton sequence on the CPU), so the reconstruction knows where every
    // texel's ray went, and the resolve's history supersamples it.
    sfRand(); sfRand();
    vec2 uv = v_texcoord0 + jitterParams.xy * passTargetRes.zw;
    vec3 camO = pjvViewPosition();
    vec3 camD = pjvViewRayDirection(uv, res);
    float tanHalf = pjvViewTanHalfFov();

    vec3 sunJ = sfOn(SF_SOFT_SHADOWS) ? sfSunSample(sfRand(), sfRand()) : SUN_DIR;
    float skip = sfOn(SF_SKIP) ? skipDistance(gl_FragCoord.xy) : 0.0;
#ifdef PJV_MARCH_STATS
    uint statSteps0 = 0u, statVisits0 = 0u;   // the camera ray's (S27b0: SANTA_FE_DUMP_GBUFFER with the define)
#endif

    Surf s0 = sfNoSurf();
    Surf s1 = sfNoSurf();
    float vis0 = 0.0, vis1 = 0.0, visFog = 0.0;
    vec3 sssOut = vec3(0.0, 0.0, 0.0);   // the leaf glow's part of the sun's light (gMotion.w)
    float blocker0 = 0.0;   // how far the sun's shadow ray went before it was blocked (shadow_soft's penumbra)
    // A foliage voxel (SF2_SSS): its sun ray asks whether the VOXEL is in sunlight, not this face -- from its
    // centre, out past its own body along the sun, whichever way the face points (as 40-advanced-renderer's
    // gbuffer.frag): a backlit leaf glows exactly when the sun strikes the side you cannot see.
    bool leafVoxel = false;
    // The far field's part of the sun's shadow left to clouds.frag, at its resolution (SF2_FAR_SHADOW_LOW, with the blur).
    bool lowFar = sfOn2(SF2_FAR_SHADOW_LOW) && sfOn(SF_SHADOW_BLUR) && sfOn(SF_SHADOWS);
    vec3 inDir = vec3(0.0, 1.0, 0.0);
    vec3 inSky = vec3(0.0, 0.0, 0.0);
    bool fogOn = mtnFog.x > 0.0 && sfOn(SF_HAZE);
    float fogRange = 0.0, fogT = 0.0;
    vec3 fogP = vec3(0.0, 0.0, 0.0);
    float pixel0 = 1.0;

    bool sunOn = false;   // the sun's ray stopped in foliage and was let partly through: segment 5 goes on past it
    vec3 sunOnFrom = vec3(0.0, 0.0, 0.0);
#ifdef SF_LEAN
    for (int si = 0; si < 3; si++) {
        int seg = si == 2 ? 5 : si;   // the camera's ray, the sun's, and the sun's on past foliage
#else
    for (int seg = 0; seg < 6; seg++) {
#endif
        // ---- This segment's ray ----
#ifdef PJV_MARCH_STATS
        if (seg == 1) { statSteps0 = pjvStatSteps; statVisits0 = pjvStatVisits; }
#endif
        bool run = false;
        bool skipVoxels = false;
        bool shadow = seg != 0 && seg != 2;
        PjvRay ray;
        ray.origin = camO;
        ray.direction = camD;
        float farMax = 2.0e5;
        float looseReach = 1e30;
        uint steps = 768u;
        if (seg == 0) {
            run = skip >= 0.0;
            ray.origin = camO + camD * max(skip, 0.0);
        } else if (seg == 1) {
            run = s0.hit && sfOn(SF_SHADOWS) && (dot(s0.n, sunJ) > 0.0 || s0.trans > 0.0) && SUN_DIR.y > -0.02;
            if (run && lowFar && s0.far) { run = false; vis0 = 1.0; }
            ray.origin = sfShadowOrigin(s0, sunJ, pixel0);
            leafVoxel = run && sfOn2(SF2_SSS) && s0.trans > 0.2 && !s0.far && s0.face.w > 0.0;
            if (leafVoxel) {
                // Half a diagonal (0.87 of an edge) clears the voxel's own body, plus a little.
                vec3 centre = s0.face.xyz - s0.nf * (s0.face.w * 0.5);
                ray.origin = centre + sunJ * (s0.face.w * 0.95);
            }
            ray.direction = sunJ;
            skipVoxels = s0.far || sfShadowLOD.w == 2.0;
            farMax = sfShadowLOD.w == 1.0 || lowFar ? 0.0 : sfQuality.z;
            steps = 384u;
            looseReach = min(45.0 / max(sunJ.y, 0.08), sfShadowLOD.z);   // trees (props in ring 0) stand to ~40 m
        } else if (seg == 2) {
            run = s0.hit && sfOn(SF_DIFFUSE);
            inDir = sfCosineDir(s0.n, sfRand(), sfRand());
            if (dot(inDir, s0.nf) < 0.02) inDir = normalize(inDir - s0.nf * (2.0 * dot(inDir, s0.nf) - 0.04));
            ray.origin = s0.far ? s0.p + (s0.n + vec3(0.0, 1.0, 0.0)) * (s0.edge * 0.3)
                                : s0.p + s0.nf * (s0.edge * 0.05) + (s0.smooth ? s0.n * s0.edge : vec3(0.0, 0.0, 0.0));
            ray.direction = inDir;
            skipVoxels = s0.far;
            farMax = 2.5e4;
            steps = preview ? 128u : 320u;
            looseReach = 80.0;
        } else if (seg == 3) {
            run = s1.hit && sfOn(SF_BOUNCE_SUN) && sfOn(SF_SHADOWS) && dot(s1.n, sunJ) > 0.0 && SUN_DIR.y > -0.02;
            ray.origin = sfShadowOrigin(s1, sunJ, s1.edge * 2.0);
            ray.direction = sunJ;
            skipVoxels = s1.far;
            farMax = 3.0e4;
            steps = 256u;
            looseReach = 20.0;
        } else if (seg == 5) {
            // The sun's ray, on past the foliage that stopped it (segment 1 let it partly through on the guess
            // that the light goes on): a trunk, a crown or a rock behind it is a shadow, and the leaves' soft
            // shadow is only soft where the sun really is beyond them.
            run = sunOn;
            ray.origin = sunOnFrom;
            ray.direction = sunJ;
            farMax = lowFar ? 0.0 : sfQuality.z;
            steps = 256u;
            looseReach = min(45.0 / max(sunJ.y, 0.08), sfShadowLOD.z);
        } else {
            fogRange = min(s0.hit ? s0.t : 6.0e4, 4.0e4);
            run = fogOn && sfOn(SF_SHAFTS) && !preview && SUN_DIR.y > -0.02;
            fogT = fogRange * sfRand();
            fogP = camO + camD * fogT;
            ray.origin = fogP;
            ray.direction = sunJ;
            farMax = 3.0e4;
            steps = 256u;
            looseReach = 0.0;
        }
        if (!run) continue;

        // ---- The march: voxels, then the far field past them ----
        PjvSceneIntersectData found;
        found.foundBox.size = -1.0;
        found.rayT = -1.0;
        bool vHit = false;
        if (!skipVoxels) {
            PjvRayQuery rq = pjvPrimaryQuery(100u);
            rq.maxRaySteps = steps;
            rq.startLOD = 0u;
            // Coarse with distance: the camera ray over 10 km; the sun's shadow ray past sfShadowLOD.y voxels (its
            // far occluders merge into coarser bricks), and through voxels only for sfShadowLOD.x metres -- past
            // that the far field's heightfield and crowns stand in for them (a ray out of a lit clearing no
            // longer crosses kilometres of rings to learn it is lit); the rest past 30.
            rq.finishLOD = seg == 0 || seg == 1 || seg == 5 ? 2u : 0u;
            rq.distanceToFinishLOD = seg == 0 ? 10000u : (seg == 1 || seg == 5 ? uint(sfShadowLOD.y) : 30u);
            if ((seg == 1 || seg == 5) && sfShadowLOD.x > 0.0) rq.maxDistance = sfShadowLOD.x;
            pjvQueryLooseReach(rq, sfOn(SF_PROPS) ? looseReach : 0.0);
            found = pjvRaySceneIntersect(ray, rq).hit;
            vHit = !(found.foundBox.size < 0.0 || found.rayT <= 0.0 || dot(found.normal, found.normal) < 0.5);
        }
        bool fHit = false;
        float tF = -1.0;
        int ring = -1;
        if (!vHit) {
            float t0 = skipVoxels ? 0.0 : mtnLeaveVoxels(ray.origin, ray.direction);
            if ((seg == 1 || seg == 5) && !skipVoxels && sfShadowLOD.x > 0.0) t0 = min(t0, sfShadowLOD.x);
            // How much ground a pixel covers along this ray: the camera's footprint, or a shadow or diffuse
            // ray's, from the footprint where it starts.
            mtnLodSlope = seg == 0 ? 2.0 * tanHalf / res.y : 0.004;
            mtnLodBase = seg == 0 ? 0.0 : (seg == 3 ? s1.edge : pixel0);
            fHit = mtnFarMarch(ray.origin, ray.direction, t0, farMax, shadow, tF, ring);
        }

        // ---- What it found ----
        if (shadow && seg == 5) {
            // Past the first foliage: open, the partial light stands; more foliage, half of it (another layer);
            // anything else, or the march starting inside the next voxel (it was solid too: rayT 0), a shadow.
            bool inside = !skipVoxels && found.foundBox.size >= 0.0 && found.rayT <= 0.0;
            if (fHit) vis0 = 0.0;
            else if (vHit) vis0 *= pjvFetchVoxelMaterialFromHit(found).transmission >= 0.2 ? 0.5 : 0.0;
            else if (inside) vis0 *= 0.5;
            continue;
        }
        if (shadow) {
            float v = (vHit || fHit) ? 0.0 : 1.0;
            // Stopped close by something thin -- a grass blade, a leaf -- the sun gets partly through (SF2_SSS):
            // grass casts soft shadows, and a little light dapples through a crown onto what is under it. Far
            // blockers (a hill) stay dark.
            if (seg == 1 && vHit && sfOn2(SF2_SSS)) {
                float bt = pjvFetchVoxelMaterialFromHit(found).transmission;
                if (bt > 0.0 && bt < 0.25 && found.rayT < 2.5) v = 0.55;
                // A leaf voxel behind other leaves: some light still comes through the crown to it, less the
                // further its ray got into foliage before it stopped (sfSSS.y: how far light gets through a crown).
                else if (bt >= 0.25 && leafVoxel) v = 0.3 * exp(-found.rayT / max(sfSSS.y, 0.05));
                else if (bt >= 0.25) v = 0.18 * exp(-found.rayT / (3.0 * max(sfSSS.y, 0.05)));
            }
            if (seg == 1) {
                vis0 = v;
                blocker0 = vHit ? found.rayT : (fHit ? tF : 0.0);
                if (vHit && v > 0.0) {   // let partly through: segment 5 asks what is beyond (from just past that voxel)
                    sunOn = true;
                    sunOnFrom = ray.origin + sunJ * (found.rayT + max(found.foundBox.size, 0.05) * 1.8);
                }
            }
            if (seg == 3) vis1 = v;
            else visFog = v;
            continue;
        }
        Surf s = sfNoSurf();
        if (vHit || fHit) {
            s.hit = true;
            s.far = !vHit;
            s.t = (vHit ? found.rayT : tF) + (seg == 0 ? max(skip, 0.0) : 0.0);
            vec3 base = seg == 0 ? camO : ray.origin;
            s.p = base + ray.direction * s.t;
            float pixel = s.t * 2.0 * tanHalf / res.y;
            if (seg == 0) pixel0 = pixel;   // per face below, once the face is known
            if (vHit) {
                s.nf = normalize(found.normal);
                s.n = s.nf;
                PjvVoxelMaterial mat = pjvFetchVoxelMaterialFromHit(found);
                s.albedo = mat.albedo;
                s.gloss = mat.glossiness;
                // What main.cpp's palette says (S21): water glossiness over 0.9; snow drawn 0.78 smooth with a
                // little gloss; rock drawn fully smooth (the terrain's) or any other glossy entry (the boulders),
                // its glossiness its shine.
                bool waterM = mat.glossiness > 0.9;
                bool snowM = !waterM && mat.metallic > 0.7 && mat.metallic < 0.85 && mat.glossiness > 0.1;
                s.rock = !waterM && !snowM && (mat.metallic > 0.9 || mat.glossiness > 0.0);
                s.shine = s.rock ? mat.glossiness : 0.0;
                s.sparkle = snowM ? 1.0 : (s.rock ? 0.35 : 0.0);
                s.trans = sfOn(SF_TRANSLUCENT) ? mat.transmission : 0.0;
                s.edge = found.foundBox.size;
                if (seg == 0) {
                    s.rot = pjvHeader(int(found.headerIndex)).rotation;
                    vec3 centre = found.foundBox.position + pjvRotationFromQuat(s.rot) * vec3(s.edge * 0.5, s.edge * 0.5, s.edge * 0.5);
                    s.face = vec4(centre + s.nf * (s.edge * 0.5), s.edge);
                    if (sfOn(SF_FACE_SHADING)) pixel0 = length(s.face.xyz - camO) * 2.0 * tanHalf / res.y;
                }
                // Normal blending (the panel's Normals): rock and snow (their palette entries' `metallic` byte),
                // everything else by a share of its own, the "smooth everything" slider over all.
                float smoothAmt = sfOn(SF_SMOOTH) ? mat.metallic : 0.0;
                if (sfOn2(SF2_SMOOTH_MORE) && mat.metallic == 0.0) smoothAmt = max(smoothAmt, sfBevel.z);
                smoothAmt = max(smoothAmt, renderParams.w);
                // Coarse ground (not leaves: they have transmission): the steps of a slope would light up as
                // stripes; it is smoothed, and from 1 m voxels shaded by the landform's own normal.
                float ground = (sfOn2(SF2_SMOOTH_GROUND) && mat.transmission == 0.0 && found.foundBox.size > 0.09) ? smoothstep(0.12, 0.45, s.edge) : 0.0;
                smoothAmt = max(smoothAmt, ground);
                if (seg == 0 && smoothAmt > 0.0) {
                    s.n = normalize(mix(s.nf, sfSmoothNormal(found, s.nf), smoothAmt));
                    s.smooth = true;
                    float hG;
                    int fG;
                    float wG = smoothstep(0.6, 2.5, s.edge) * ground;
                    vec3 at = s.face.w > 0.0 && sfOn(SF_FACE_SHADING) ? s.face.xyz : s.p;   // per face (see sfShadowOrigin)
                    if (wG > 0.0 && mtnFarHeight(at.xz, hG, fG) && abs(at.y - hG) < 1.5 * s.edge + 2.0) {
                        vec3 nG = mtnFarGroundNormal(fG, at.xz, pixel);
                        s.n = normalize(mix(s.n, nG, wG));
                    }
                }
                if (seg == 0 && sfOn2(SF2_BEVEL)) s.n = sfBevelNormal(found, s.p, s.nf, s.n);
                // Toward the voxels' outer edge their look turns into the far field's (as in 55), so only
                // the silhouette changes where the far field takes over.
                if (seg == 0) {
                    vec4 B = mtnVoxelBox;
                    if (B.x < B.z) {
                        vec2 halfSize = 0.5 * (B.zw - B.xy);
                        vec2 rel = abs(s.p.xz - 0.5 * (B.xy + B.zw)) / halfSize;
                        float w = smoothstep(0.6, 0.97, max(rel.x, rel.y));
                        float hFar;
                        int fFar;
                        if (w > 0.0 && !sfOn(SF_FAR_BLOCKY) && mtnFarHeight(s.p.xz, hFar, fFar) && abs(s.p.y - hFar) < 3.0 * s.edge + 15.0) {
                            vec3 nFar, aFar;
                            mtnFarSurface(fFar, s.p.xz, pixel, nFar, aFar);
                            s.albedo = mix(s.albedo, aFar, w);
                            s.n = normalize(mix(s.n, nFar, w));
                        }
                    }
                }
            } else {
                mtnFarSurface(ring, s.p.xz, pixel, s.n, s.albedo);
                s.nf = s.n;
                s.edge = mtnFarRing(ring).z / float(MTN_FAR_RES);
                s.trans = (sfOn(SF_TRANSLUCENT) && mtnFarCanopyLast > 0.5) ? 0.15 : 0.0;
                // A block is a voxel to the reconstruction: its face's centre and edge, so its edges stay sharp.
                if (seg == 0 && sfOn(SF_FAR_BLOCKY)) {
                    float half = 0.5 * mtnFarHitSize;
                    vec3 centre = vec3(mtnFarHitBlock.x, s.p.y, mtnFarHitBlock.y);
                    if (s.n.y > 0.5) centre.y = s.p.y;
                    else centre += s.n * half;
                    s.face = vec4(centre, mtnFarHitSize);
                }
            }
        }
        if (seg == 0) s0 = s;
#ifndef SF_LEAN
        else {
            s1 = s;
            if (!s.hit) {
                vec3 sky = mtnSkyFrom(s0.p, inDir);
                vec4 cl = sfCloudSky(s0.p, inDir, SUN_DIR, mtnSunAir.rgb, mtnSkyFrom(s0.p, vec3(0.0, 1.0, 0.0)));
                inSky = mix(sky, cl.rgb, cl.a);
            }
        }
#endif
    }

    // ---- Light ----
    vec3 colour;
    bool blur = sfOn(SF_SHADOW_BLUR) && sfOn(SF_SHADOWS);
    vec3 sunRad = vec3(0.0, 0.0, 0.0);
    vec4 glossOut = vec4(0.0, 0.0, 0.0, 0.0);   // gGloss: what reflect.frag follows (S21)
    vec3 ambRad = vec3(0.0, 0.0, 0.0);   // the sky light's share (for shadow_soft's AO)
    vec3 skyUp = mtnSkyFrom(camO, vec3(0.0, 1.0, 0.0));
    vec3 groundUp = mtnGroundUp();   // in the clouds' shadow (S23b)
    float cloudJ = sfRand();
    if (!s0.hit) {
        colour = mtnSkyView(camD);
        // High cirrus over the sky (behind the cumulus, composited below them).
        vec4 ci = sfCirrus(camO, camD, SUN_DIR, mtnSunAir.rgb, mtnSkyFrom(camO, vec3(0.0, 1.0, 0.0)));
        colour = mix(colour, ci.rgb, ci.a);
    } else {
        if (!sfOn(SF_SHADOWS)) vis0 = 1.0;
        // Snow on the trees (SF_GLOSS's neighbour, always on): where the ground under a crown is snow -- read off
        // the far field's colour there, white and grey-free -- the tops of its leaves and needles carry snow,
        // the sides a little, in clumps. Every tree at every ring, no snowy variants (the palette is full).
        if (s0.hit && !s0.far && s0.trans > 0.2) {
            // The snowline (surface.hpp snowlineAt, sfSnow.x), ragged by a few tens of metres; or snow under it.
            float ragged = 60.0 * sin(s0.p.x * 0.004 + sin(s0.p.z * 0.003) * 2.0);
            float sn = max(smoothstep(sfSnow.x - 320.0 + ragged, sfSnow.x - 80.0 + ragged, s0.p.y), sfSnowAt(s0.p.xz));
            if (sn > 0.0) {
                float clump = fract(sin(dot(floor(s0.p / max(s0.edge, 0.05)), vec3(12.9898, 78.233, 37.719))) * 43758.5453);
                float up = smoothstep(0.3, 0.9, s0.nf.y) + 0.25 * (1.0 - abs(s0.nf.y));
                float k = clamp(sn * up * (0.6 + 0.8 * clump), 0.0, 1.0);
                s0.albedo = mix(s0.albedo, vec3(0.86, 0.88, 0.93), smoothstep(0.35, 0.6, k));
                s0.trans *= 1.0 - smoothstep(0.35, 0.6, k);   // snow does not glow
                s0.sparkle = max(s0.sparkle, smoothstep(0.35, 0.6, k));   // and glints as the ground's snow does
            }
        }
        // Per face too, with a jitter fixed per face: a face's texels agree, and still faces do not shimmer.
        // Per face, a jitter fixed per face; or per hit, a jitter fixed in the world (either way still, so the
        // reconstruction's average of a face's texels does not change as the camera jitter moves).
        bool perFace = s0.face.w > 0.0 && sfOn(SF_FACE_SHADING);
        vec3 cloudAt = perFace ? s0.face.xyz : s0.p;
        vec3 jitterCell = perFace ? floor(s0.face.xyz / max(s0.face.w, 1e-3)) : floor(s0.p / 0.05);
        float faceJ = s0.hit && !s0.far ? fract(sin(dot(jitterCell, vec3(12.9898, 78.233, 37.719))) * 43758.5453) : 0.5;   // fixed (S22f): past the cloud-shadow map, the march's mid-points
        // Shadow blur (optimized mode; shadow_soft.frag): the sun's light goes out apart, unshadowed, with its
        // visibility and blocker distance, and is added back after a screen-space blur sized by that distance.
        float visUse = blur ? 1.0 : vis0;
        float cloud0 = visUse > 0.0 ? sfCloudShadow(cloudAt, SUN_DIR, faceJ) : 0.0;
        vec3 sunAt = SUN_COLOR * visUse * cloud0;
        // Leaves wrap the light round their terminator (SF2_SSS): a crown is many leaves, lit past its edge.
        bool leafy = s0.trans > 0.2 && sfOn2(SF2_SSS);   // foliage (grass has a little transmission too: it only softens shadows)
        float ndl = leafy ? max((dot(s0.n, sunJ) + sfSSS.z) / (1.0 + sfSSS.z), 0.0) : max(dot(s0.n, sunJ), 0.0);
        vec3 direct = s0.albedo / PI * sunAt * ndl;
        // Glints (SF2_GLINTS, S21): snow's crystals, mica and quartz in the granite, as a grid of SF_GLINT_GRID^2
        // cells on each voxel face (in its chunk's frame, from where this ray meets the face: it holds still as
        // the camera moves, and scales with the voxel). A few cells -- alone, or most of a section -- are small
        // mirrors, each tilted at random off the shading normal. A mirror shows the sun's disc where it reflects
        // the eye toward it, with the sun's light (so under its shadow, and the shadow blur), held under the
        // glow's threshold; and, through reflect.frag, the screen and the sky along its mirror direction.
        bool glintCell = false;
        float glintShare = 1.0;   // a merged cell's mirror share (below)
        float glintFade = smoothstep(SF_GLINT_MAX_DISTANCE, SF_GLINT_MAX_DISTANCE * 0.4, s0.t);
        bool rocky = s0.rock;
        float shiny = smoothstep(0.0, SF_ROCK_SHINY, s0.shine);   // dull rock 0 .. shiny rock 1
        if (sfOn2(SF2_GLINTS) && s0.sparkle > 0.0 && !s0.far && s0.face.w > 0.0 && glintFade > 0.0) {
            mat3 M = pjvRotationFromQuat(s0.rot);
            vec3 l = (s0.p - s0.face.xyz) * M;   // chunk-local, from the face's centre
            vec3 nl = s0.nf * M;
            vec3 an = abs(nl);
            vec2 uvF = an.x >= an.y && an.x >= an.z ? l.yz : (an.y >= an.z ? l.xz : l.xy);
            ivec3 faceKey = ivec3(floor(s0.face.xyz / (s0.face.w * 0.5) + 0.5));   // face centres sit on half voxels
            float share = rocky ? 1.0 : s0.sparkle;
            float cellsK = (rocky ? mix(SF_GLINT_CELLS_ROCK_DULL, SF_GLINT_CELLS_ROCK_SHINY, shiny) : SF_GLINT_CELLS_SNOW) * share;
            float secK = (rocky ? mix(SF_GLINT_SECTIONS_ROCK_DULL, SF_GLINT_SECTIONS_ROCK_SHINY, shiny) : SF_GLINT_SECTIONS_SNOW) * share;
            // Cells smaller than a texel would alias (S22: no history averages them now): the grid is taken at the
            // level whose cells are about a texel -- 2^level cells a side merged -- where a merged cell is a mirror
            // if any of its cells would be, as bright as the share of it that is mirror. Same light, no sparkle.
            // (Two texels a cell, not one: the jitter moves a texel's sample across its cell each frame, and a cell
            // the size of a texel flickers between neighbours as it does.)
            float level = clamp(ceil(log2(max(2.0 * pixel0 * SF_GLINT_GRID / s0.face.w, 1.0))), 0.0, 4.0);
            float grid = SF_GLINT_GRID / exp2(level);
            ivec2 cell = ivec2(clamp(floor((uvF / s0.face.w + 0.5) * grid), 0.0, grid - 1.0));
            ivec2 sec = ivec2(floor(vec2(cell) / SF_GLINT_SECTION));
            uint hc = sfHash3(faceKey, uint(cell.x + cell.y * 64) + 1u + uint(level) * 4099u);
            uint hs = sfHash3(faceKey, uint(sec.x + sec.y * 64) + 7919u);
            uint ht = 0u;
            if (level < 0.5) {
                if (sfHashUnit(hs) < secK && sfHashUnit(hc * 1664525u + 1013904223u) < SF_GLINT_FILL) { glintCell = true; ht = hs; }
                else if (sfHashUnit(hc) < cellsK) { glintCell = true; ht = hc; }
            } else {
                float pCell = min(cellsK + secK * SF_GLINT_FILL, 0.95);
                float pAny = 1.0 - pow(1.0 - pCell, exp2(2.0 * level));
                if (sfHashUnit(hc) < pAny) { glintCell = true; ht = hc; glintShare = pCell / max(pAny, 1e-4); }
            }
            if (glintCell) {
                float r1 = sfHashUnit(ht * 2246822519u + 1u), r2 = sfHashUnit(ht * 3266489917u + 7u);
                float tilt = radians(SF_GLINT_TILT) * sqrt(r1), ph = 6.2831853 * r2;
                vec3 t = abs(s0.n.y) < 0.99 ? normalize(cross(s0.n, vec3(0.0, 1.0, 0.0))) : vec3(1.0, 0.0, 0.0);
                vec3 b = cross(s0.n, t);
                vec3 fn = normalize(s0.n * cos(tilt) + (t * cos(ph) + b * sin(ph)) * sin(tilt));
                vec3 rr = reflect(camD, fn);
                if (dot(rr, s0.nf) < 0.0) rr = rr - 2.0 * dot(rr, s0.nf) * s0.nf;
                float f0 = rocky ? mix(SF_GLINT_F0_ROCK_DULL, SF_GLINT_F0_ROCK_SHINY, shiny) : SF_GLINT_F0_SNOW;
                float F = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(-camD, fn), 0.0, 1.0), 5.0);
                float inner = cos(radians(SF_GLINT_SUN_ANGLE)), outer = cos(radians(SF_GLINT_SUN_ANGLE + SF_GLINT_SUN_SOFT));
                float disc = smoothstep(outer, inner, dot(rr, SUN_DIR)) * glintFade * glintShare;
                if (disc > 0.0 && dot(s0.nf, SUN_DIR) > 0.0) {
                    vec3 g = sunAt * (SF_GLINT_SUN * F * disc);
                    float lumMax = SF_GLINT_MAX_EXPOSED / max(mtnSun.w, 1e-6);
                    float lg = dot(g, vec3(0.2126, 0.7152, 0.0722));
                    if (lg > lumMax) g *= lumMax / lg;
                    direct += g;
                }
                glossOut = vec4(rr, F * SF_GLINT_ENV * glintFade * glintShare);
            }
        }
        // Sheen (SF_GLOSS, S21, S22e, S24): a rough reflection of the lights -- the sun and the sky -- on snow, rock and
        // leaves: the sun as an exact GGX lobe (with the sun's light, so under its shadow), and the sky along one
        // direction drawn from the GGX lobe a frame (S24: the mirror direction of S22 flickered as the jitter moved
        // the smoothed normal under it; random, the reflections' history averages the lobe), which reflect.frag
        // blurs a little by the roughness. gGloss carries the direction scaled by 1 + roughness (a glint's mirror
        // is 1: reflect.frag marches it through the screen instead).
        if (!glintCell && sfOn(SF_GLOSS) && !s0.far) {
            float k = 0.0, alpha = 0.5;
            if (s0.sparkle > 0.0 && !rocky) { k = SF_SHEEN_SNOW * s0.sparkle; alpha = SF_SHEEN_ALPHA_SNOW; }
            else if (s0.trans > 0.2) { k = SF_SHEEN_LEAF; alpha = SF_SHEEN_ALPHA_LEAF; }
            else if (rocky) { k = mix(SF_SHEEN_ROCK_DULL, SF_SHEEN_ROCK_SHINY, shiny); alpha = mix(SF_SHEEN_ALPHA_ROCK_DULL, SF_SHEEN_ALPHA_ROCK_SHINY, shiny); }
            float fade = smoothstep(SF_SHEEN_MAX_DISTANCE, SF_SHEEN_MAX_DISTANCE * 0.5, s0.t);
            if (k > 0.0 && fade > 0.0) {
                vec3 hs = pjvSampleGgxHalf(s0.n, alpha, sfRand(), sfRand());
                vec3 rr = reflect(camD, hs);
                if (dot(rr, s0.n) <= 0.0) rr = reflect(camD, s0.n);   // a half vector that sends it under: the mirror's
                if (dot(rr, s0.nf) < 0.0) rr = rr - 2.0 * dot(rr, s0.nf) * s0.nf;
                float nv = clamp(dot(-camD, s0.n), 1e-3, 1.0);
                float F = SF_SHEEN_F0 + (max(1.0 - alpha, SF_SHEEN_F0) - SF_SHEEN_F0) * pow(1.0 - nv, 5.0);
                glossOut = vec4(rr * (1.0 + alpha), k * F * fade);
                // The sun's highlight: D (GGX) F / (4 n.v), times n.l (which the geometry term's n.l cancels).
                float nl = dot(s0.n, sunJ);
                if (nl > 0.0) {
                    vec3 hv = normalize(sunJ - camD);
                    float nh = max(dot(s0.n, hv), 0.0);
                    float a2 = alpha * alpha;
                    float dd = nh * nh * (a2 - 1.0) + 1.0;
                    float D = a2 / (PI * dd * dd);
                    float Fs = SF_SHEEN_F0 + (max(1.0 - alpha, SF_SHEEN_F0) - SF_SHEEN_F0) * pow(1.0 - clamp(dot(-camD, hv), 0.0, 1.0), 5.0);
                    vec3 g = sunAt * (k * Fs * D * SF_SHEEN_SUN / (4.0 * nv)) * fade;
                    float lumMax = SF_GLINT_MAX_EXPOSED / max(mtnSun.w, 1e-6);
                    float lg = dot(g, vec3(0.2126, 0.7152, 0.0722));
                    if (lg > lumMax) g *= lumMax / lg;
                    direct += g;
                }
            }
        }
        // Leaves let light through: the glow of a backlit crown (SF2_SSS, no extra rays -- the sun's shadow ray,
        // from the leaf voxel's far side when it faces away). Seen most looking toward the sun through it, and on
        // the side away from the sun; yellower and more saturated, as light through a leaf is.
        vec3 sss = vec3(0.0, 0.0, 0.0);
        if (leafy && SUN_DIR.y > -0.02) {
            // Light through the leaf (santa-fe-reference S18, after 40-advanced-renderer's compose.frag): straight
            // back along the sun, asked how much of it points at the eye, plus a floor for what leaves a leaf every
            // way; tinted as chlorophyll passes light; brightest at the voxel face's rim, skewed toward the sun.
            // Driven by the voxel's own sun ray (leafVoxel): a leaf glows where the sun reaches it at all -- a
            // crown's edges and gaps -- and not behind other leaves, a trunk or a ridge.
            float cloudS = cloud0 > 0.0 ? cloud0 : sfCloudShadow(cloudAt, SUN_DIR, faceJ);
            float lobe = pow(clamp(dot(camD, sunJ), 0.0, 1.0), 3.0) + 0.22;
            float rim = s0.face.w > 0.0 && !s0.far ? sfLeafEdgeGlow(s0.p, s0.nf, s0.face.xyz, s0.face.w) : 1.0;
            sss = s0.albedo * vec3(1.3, 1.08, 0.55) / PI * SUN_COLOR * cloudS * (lobe * rim * sfSSS.x);
            // With the shadow blur, it goes out with the sun's light and takes the blurred visibility (it fades
            // across a shadow's edge rather than switching), and the far field's shadow; else this ray's own.
            if (blur) { direct += sss; sssOut = sss; sss = vec3(0.0, 0.0, 0.0); }
            else sss *= vis0;
        } else if (s0.trans > 0.0) {
            direct += s0.albedo * sunAt * s0.trans * max(-dot(s0.n, sunJ), 0.0) / PI;
        }
        // The light the diffuse ray brought back (cosine-weighted: albedo times what it saw).
        vec3 inL;
#ifndef SF_LEAN
        if (s1.hit) {
            if (!sfOn(SF_SHADOWS) && sfOn(SF_BOUNCE_SUN)) vis1 = 1.0;
            float cloud1 = vis1 > 0.0 ? sfCloudShadow(s1.p, SUN_DIR, fract(cloudJ + 0.5)) : 0.0;
            vec3 b = s1.albedo / PI * SUN_COLOR * vis1 * cloud1 * max(dot(s1.n, sunJ), 0.0);
            // The bounce's own ambient: the sky over it, partly hidden (a second bounce, roughly).
            b += s1.albedo * mtnSkyFrom(s1.p, normalize(s1.n + vec3(0.0, 1.0, 0.0))) * 0.55;
            inL = mtnAerial(b, s0.p, inDir, s1.t);
        } else if (sfOn(SF_DIFFUSE)) {
            inL = inSky;
        } else
#endif
        {
            // No diffuse rays: the open sky over the normal, unoccluded.
            inL = sfOn2(SF2_SKY_LIGHT) && mtnSkyCubeReady() ? mtnSkyCube(s0.n) * SF_SKY_LIGHT
                                       : mtnSkyFrom(s0.p, normalize(s0.n + vec3(0.0, 1.0, 0.0))) * 0.8;
        }
        vec3 indirect = s0.albedo * inL;
        // The far field's AO (no rays reach it): the land round it hides some of the sky (sfFarAO).
        if (s0.far && sfOn2(SF2_FAR_AO) && !sfOn(SF_DIFFUSE)) indirect *= sfFarAO(s0.p, s0.n, pixel0);
        ambRad = indirect;
        // A leaf's highlight (SF2_SSS): a sheen where it faces between the sun and the eye -- a sunlit crown's top
        // and rim catch the light. With the sun (and so under its shadow and the shadow blur).
        if (leafy) {
            vec3 hh = normalize(sunJ - camD);
            vec3 sheen = sqrt(max(s0.albedo, vec3(0.0, 0.0, 0.0))) * SUN_COLOR * cloud0 * pow(max(dot(s0.n, hh), 0.0), 16.0) * sfSSS.w * 0.18;
            if (blur) direct += sheen; else direct += sheen * vis0;
        }
        colour = (blur ? indirect : direct + indirect) + sss;
        if (blur) sunRad = direct;
        // Gloss: water mirrors the sky (snow's sheen is the S21 sheen above, through reflect.frag).
        if (s0.gloss > 0.9 && sfOn(SF_GLOSS)) {
            vec3 r = reflect(camD, s0.n);
            float cosV = clamp(dot(-camD, s0.n), 0.0, 1.0);
            float F = 0.02 + 0.98 * pow(1.0 - cosV, 5.0);
            float k = s0.gloss;
            vec3 spec = mtnSkyFrom(s0.p, r) * F;
            float sharp = mix(8.0, 3000.0, k * k);
            spec += sunAt * vis0 * F * pow(max(dot(r, sunJ), 0.0), sharp) * (sharp + 2.0) / (2.0 * PI) * 0.15;
            colour = mix(colour, colour * (1.0 - F) + spec, k);
        }
        if (sfOn(SF_AERIAL)) {
            vec3 T, Lair;
            mtnAtmosphere(camO, camD, s0.t, T, Lair);
            colour = colour * T + Lair * mtnAirLit();   // under the clouds the air is lit less (S23c)
            sunRad *= T;
            ambRad *= T;
            glossOut.w *= dot(T, vec3(0.2126, 0.7152, 0.0722));   // what is reflected crosses the same air
        }
    }

    // ---- The haze in the valleys (santa-fe-reference S23d: after Unreal's exponential height fog) ----
    // Two exponential layers (mtnFog: density at its base, base, scale height; sfFog2: a second), from a start
    // distance (sfFog3.x), held under a max opacity (sfFog3.y). Its light: the sky's, as the scene's sky light
    // sees it (the ambient cube: clouds and the land in their shadow included; sfFog3.z of it), and the sun's,
    // scattered toward the eye by the haze's phase (mtnFog.w, Henyey-Greenstein) where the sun reaches the air
    // (the clouds' shadow at the camera; sfFog3.w of it: Unreal's directional inscattering); all through the
    // inscattering tint (sfFogTint.rgb) and the haze's albedo (sfFogTint.w). Its traced shafts as before.
    if (fogOn) {
        float H = mtnFog.z;
        float D = mtnFog.x * exp(mtnFog.y / H);
        float tEnd = s0.hit ? s0.t : 6.0e4;
        float tStart = min(sfFog3.x, tEnd);
        float y0 = camO.y + camD.y * tStart;
        float len = max(tEnd - tStart, 0.0);
        float tauAll = D * mtnLayer(y0, camD.y, len, H);
        if (sfFog2.x > 0.0) tauAll += sfFog2.x * exp(sfFog2.y / max(sfFog2.z, 1.0)) * mtnLayer(y0, camD.y, len, max(sfFog2.z, 1.0));
        float opacity = min(1.0 - exp(-tauAll), sfFog3.y);
        float Tall = 1.0 - opacity;
        colour *= Tall;
        sunRad *= Tall;
        ambRad *= Tall;
        glossOut.w *= Tall;
        vec3 skyIn = mtnSkyCubeReady() ? mtnSkyCube(vec3(0.0, 1.0, 0.0)) * 0.75 + mtnSkyCube(vec3(0.0, -1.0, 0.0)) * 0.5
                                       : skyUp * 0.75 + groundUp * 0.5;
        float sunLit = sfCloudShadow(camO + camD * min(tStart + 0.5 * len, 4000.0), SUN_DIR, 0.5);
        vec3 sunIn = SUN_COLOR * (4.0 * PI * sfHG(dot(camD, SUN_DIR), mtnFog.w)) * sunLit * max(sign(SUN_DIR.y + 0.02), 0.0);
        colour += (skyIn * sfFog3.z + sunIn * sfFog3.w) * sfFogTint.rgb * sfFogTint.w * opacity;
#ifndef SF_LEAN
        if (fogRange > 0.0 && visFog > 0.0) {
            float sigma = D * exp(-max(fogP.y, -1000.0) / H);
            float Tf = exp(-D * mtnLayer(camO.y, camD.y, fogT, H));
            float ph = sfHG(dot(camD, SUN_DIR), mtnFog.w);
            float cl = sfCloudShadow(fogP, SUN_DIR, cloudJ);
            vec3 T, Lair;
            mtnAtmosphere(camO, camD, fogT, T, Lair);
            colour += SUN_COLOR * (sigma * ph * Tf * fogRange * cl) * T * sfFogTint.rgb;
        }
#endif
    }

    // ---- The clouds in front of it all: clouds.frag now (a quarter of these texels, accumulated over frames),
    // composited by shadow_soft.frag. Kept here, off, for comparison: SF_CLOUDS_INLINE. ----
    if (false) {
        float tCloud;
        vec4 cl = sfCloudMarch(camO, camD, s0.hit ? s0.t : 1.0e9, SUN_DIR, mtnSunAir.rgb, skyUp, groundUp,
                               sfRand(), preview ? 20 : int(renderParams.y), tCloud);
        if (cl.a < 1.0) {
            vec3 T, Lair;
            mtnAtmosphere(camO, camD, tCloud, T, Lair);
            colour = colour * cl.a + (sfOn(SF_AERIAL) ? cl.rgb * T + Lair * (1.0 - cl.a) : cl.rgb);
            sunRad *= cl.a;
        }
    }

    if (any(isnan(colour)) || any(isinf(colour))) colour = vec3(0.0, 0.0, 0.0);
    if (any(isnan(sunRad)) || any(isinf(sunRad))) sunRad = vec3(0.0, 0.0, 0.0);
    // Fireflies: nothing but the sun's disc is this bright.
    float lum = dot(colour, vec3(0.2126, 0.7152, 0.0722));
    if (s0.hit && lum > 40.0) colour *= 40.0 / lum;
    // The camera's motion at this point (uv now minus uv last frame), for the resolve's reprojection.
    bool nowInFront, thenInFront;
    vec3 at = s0.hit ? s0.p : camO + camD * 1e4;
    vec3 then = s0.hit ? s0.p : pjvPrevViewPosition() + camD * 1e4;
    vec2 nowUV = pjvPerspectiveUV(at, res, pjvViewPosition(), pjvViewDirection(), pjvViewTanHalfFov(), nowInFront);
    vec2 thenUV = pjvPrevViewUV(then, res, thenInFront);
    // Debug (bit 22): which ring each hit is in -- 0.1 m red, 0.2 orange, 0.4 yellow, 0.8 green, 1.6 cyan,
    // 3.2 blue, the far field magenta -- over the image's brightness.
    if (sfOn(4194304u) && s0.hit) {
        float k = s0.far ? 6.0 : clamp(floor(log2(max(s0.edge, 0.05) / 0.1) + 0.5), 0.0, 5.0);
        vec3 tint = k < 0.5 ? vec3(1.0, 0.2, 0.2) : (k < 1.5 ? vec3(1.0, 0.6, 0.2) : (k < 2.5 ? vec3(1.0, 1.0, 0.2) :
                    (k < 3.5 ? vec3(0.2, 1.0, 0.2) : (k < 4.5 ? vec3(0.2, 1.0, 1.0) : (k < 5.5 ? vec3(0.3, 0.4, 1.0) : vec3(1.0, 0.3, 1.0))))));
        colour = mix(colour, tint * dot(colour, vec3(0.3, 0.6, 0.1)) * 2.0, 0.6);
        sunRad *= 0.0;
    }
    // Debug (bit 21): the cloud-shadow map's value at what this texel sees, as grey.
    if (sfOn(2097152u) && s0.hit) { colour = vec3(sfCloudShadow(s0.p, SUN_DIR, 0.5)) * 2.0; sunRad = vec3(0.0, 0.0, 0.0); }
    gl_FragData[0] = vec4(colour, 1.0);
    gl_FragData[1] = vec4(at, s0.hit ? s0.t : -1.0);
    gl_FragData[2] = vec4(s0.nf, blocker0);
#ifdef PJV_MARCH_STATS
    // In gFace's place: the camera ray's DDA steps and scene visits, then every later segment's (the sun's)
    gl_FragData[3] = vec4(float(statSteps0), float(statVisits0), float(pjvStatSteps - statSteps0), float(pjvStatVisits - statVisits0));
#else
    gl_FragData[3] = s0.face;
#endif
    gl_FragData[4] = s0.rot;
    // z: the sky light's share of the colour -- shadow_soft darkens it by the AO (ao_trace.frag's, and its own
    // screen-space one).
    float lumC = dot(colour, vec3(0.2126, 0.7152, 0.0722));
    float share = sfOn2(SF2_AO) && s0.hit && !s0.far ? clamp(dot(ambRad, vec3(0.2126, 0.7152, 0.0722)) / max(lumC, 1e-4), 0.0, 1.0) : 0.0;
    // w: the leaf glow's share of the colour (shadow_soft darkens it by the far field's shadow, SF2_FAR_SHADOW_LOW).
    // w: the leaf glow's share of the sun's light (gSun.rgb, with the shadow blur) -- shadow_soft spreads its
    // visibility across the crown (the light the edge leaves found reaches the leaves round them).
    float sssShare = clamp(dot(sssOut, vec3(0.2126, 0.7152, 0.0722)) / max(dot(sunRad, vec3(0.2126, 0.7152, 0.0722)), 1e-4), 0.0, 1.0);
    gl_FragData[5] = vec4(thenInFront && nowInFront ? thenUV - nowUV : vec2(0.0, 0.0), share, sssShare);
    // a: the sun's visibility, + 2 on leaves.
    gl_FragData[6] = vec4(sunRad, vis0 + (s0.hit && s0.trans > 0.2 ? 2.0 : 0.0));
    if (any(isnan(glossOut)) || any(isinf(glossOut))) glossOut = vec4(0.0, 0.0, 0.0, 0.0);
    gl_FragData[7] = glossOut;
}
