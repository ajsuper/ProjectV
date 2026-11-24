// =============================================================================
// mountains_far.sc  --  the far field (55's D14-D17), as terrain (santa-fe-reference R7, R8).
//
// Each far ring f has a square of three atlases: its columns' 64 x 64 tiles at (column mod 16) in it, and a
// state texture saying which column each tile holds (a stale tile is not read).
//
//   farHeight  R32F   the ground's height (no canopy)
//   farColour  RGBA8  rgb the ground's colour (soil, grass, rock, snow: everything but the trees), sRGB;
//                     a the canopy cover, 0..1
//   farCanopy  RGBA8  rgb the canopy's colour, sRGB; a: bit 7 conifers (pointed crowns), bits 0-6 the
//                     trees' height in 0.35 m steps
//
// The trees are geometry (R7): crowns on a jittered grid of mtnCanopy.x metres, a cell's tree kept with the
// probability that makes the crowns cover what the cover says, round-topped or pointed, of the height the
// canopy atlas gives. They are marched and cast shadows; where a crown is well under a pixel it is its mean
// height instead (the accumulation averages the rest). The ground between them is in patches (R8).
//
// mtnLodBase + mtnLodSlope * t: how much ground a pixel covers t along the ray being marched (set by the
// caller before a march).
// =============================================================================

#ifndef MOUNTAINS_FAR_SC
#define MOUNTAINS_FAR_SC

uniform vec4 mtnFar0;
uniform vec4 mtnFar1;
uniform vec4 mtnFar2;
uniform vec4 mtnFar3;
uniform vec4 mtnFar4;
uniform vec4 mtnFar5;
uniform vec4 mtnVoxelBox;

#define MTN_FAR_RINGS 6
#define MTN_FAR_SPAN 16      // columns along a ring's side
#define MTN_FAR_RES 64       // texels along a column's side
#define MTN_FAR_TOP 4700.0   // nothing stands higher

vec4 mtnFarRing(int f) {
    if (f == 0) return mtnFar0;
    if (f == 1) return mtnFar1;
    if (f == 2) return mtnFar2;
    if (f == 3) return mtnFar3;
    if (f == 4) return mtnFar4;
    return mtnFar5;
}

// Whether ring f holds column col, and where its tile starts in the atlas.
bool mtnFarColumn(int f, vec4 R, ivec2 col, out ivec2 tile) {
    tile = ivec2(0, 0);
    ivec2 rel = col - ivec2(floor(R.xy / R.z + 0.5));
    if (rel.x < 0 || rel.y < 0 || rel.x >= MTN_FAR_SPAN || rel.y >= MTN_FAR_SPAN) return false;
    ivec2 slot = ivec2(mod(vec2(col), float(MTN_FAR_SPAN)));
    vec4 st = texelFetch(farState, ivec2(f * MTN_FAR_SPAN + slot.x, slot.y), 0);
    ivec2 id = ivec2(floor(st.xy * 255.0 + 0.5));
    if (st.z < 0.5 || id.x != (col.x & 255) || id.y != (col.y & 255)) return false;
    tile = ivec2((f - (f / 3) * 3) * MTN_FAR_SPAN * MTN_FAR_RES, (f / 3) * MTN_FAR_SPAN * MTN_FAR_RES) + slot * MTN_FAR_RES;
    return true;
}

// The atlas texels of ring f round texel i0 (i0, +x, +z, +xz), one state lookup per column they fall in
// (almost always one). False if the ring does not hold them all.
bool mtnFarQuad(int f, vec4 R, ivec2 i0, out ivec2 a, out ivec2 b, out ivec2 c, out ivec2 d) {
    a = ivec2(0, 0); b = a; c = a; d = a;
    ivec2 c0 = ivec2(floor(vec2(i0) / float(MTN_FAR_RES)));
    ivec2 c1 = ivec2(floor(vec2(i0 + ivec2(1, 1)) / float(MTN_FAR_RES)));
    ivec2 t00, t10, t01, t11;
    if (!mtnFarColumn(f, R, c0, t00)) return false;
    t10 = t00; t01 = t00; t11 = t00;
    if (c1.x != c0.x && !mtnFarColumn(f, R, ivec2(c1.x, c0.y), t10)) return false;
    if (c1.y != c0.y && !mtnFarColumn(f, R, ivec2(c0.x, c1.y), t01)) return false;
    bool crossX = c1.x != c0.x, crossZ = c1.y != c0.y;
    if (crossX && crossZ && !mtnFarColumn(f, R, c1, t11)) return false;
    if (crossX && !crossZ) t11 = t10;
    if (crossZ && !crossX) t11 = t01;
    ivec2 i1 = i0 + ivec2(1, 1);
    a = t00 + (i0 - c0 * MTN_FAR_RES);
    b = a + ivec2(1, 0);
    if (crossX) b = t10 + ivec2(i1.x - c1.x * MTN_FAR_RES, i0.y - c0.y * MTN_FAR_RES);
    c = a + ivec2(0, 1);
    if (crossZ) c = t01 + ivec2(i0.x - c0.x * MTN_FAR_RES, i1.y - c1.y * MTN_FAR_RES);
    d = t11 + (i1 - c1 * MTN_FAR_RES);
    return true;
}


// ---- Feature switches (main.cpp's panel; also read by trace.frag) ----
uniform vec4 sfFlags;     // x: the bits below
uniform vec4 mtnCanopy;   // x: tree cell (m), y: crown size scale, z: far detail strength, w: ground patches strength
uniform vec4 sfQuality;   // x: smoothing radius (voxels, 1 or 2), y: cloud light steps, z: far shadow reach (m),
                          // w: far detail octaves (share of the full count)

#define SF_SHADOWS      1u
#define SF_SOFT_SHADOWS 2u
#define SF_DIFFUSE      4u
#define SF_BOUNCE_SUN   8u
#define SF_SMOOTH       16u
#define SF_CLOUDS       32u
#define SF_CLOUD_SHADOW 64u
#define SF_HAZE         128u
#define SF_SHAFTS       256u
#define SF_FAR_TREES    512u
#define SF_FAR_DETAIL   1024u
#define SF_FAR_PATCHES  2048u
#define SF_AERIAL       4096u
#define SF_GLOSS        8192u
#define SF_TRANSLUCENT  16384u
#define SF_PROPS        32768u
#define SF_CLOUD_DETAIL 65536u
#define SF_SKIP         131072u
#define SF_FAR_BLOCKY   262144u
#define SF_FACE_SHADING 524288u
#define SF_SHADOW_BLUR  1048576u

uniform vec4 sfFar;       // x: blocky distance's block, in far texels (1, 2, 4, 8)

bool sfOn(uint bit) { return (uint(sfFlags.x + 0.5) & bit) != 0u; }
// The second word (sfFlags.y): a float holds 24 bits exactly, and the first is full.
#define SF2_CLOUD_BLOCKY  1u    // box clouds (apart from the blocky far field)
#define SF2_SMOOTH_GROUND 2u    // coarse ground's normals smoothed (was part of SF_SMOOTH)
#define SF2_SMOOTH_MORE   4u    // everything not flagged smooth (leaves, wood, props) smoothed by sfBevel.z
#define SF2_BEVEL         8u    // voxels' outside edges rounded in the shading (after restir-pt's normal_blur)
#define SF2_SSS           16u   // leaves: wrap lighting and light through them (trace.frag)
#define SF2_MOTES         32u   // floating dust and pollen in the air near the camera (display.frag)
#define SF2_GODRAYS       64u   // screen-space light shafts from the sun (glow.frag)
#define SF2_AO            128u  // a short traced AO ray per 2 x 2 texels (ao_trace.frag), times a screen-space AO (shadow_soft)
#define SF2_GLINTS        512u  // sun glints on snow and rock (trace.frag)
#define SF2_FAR_SHADOW_LOW 256u // the far field's share of the sun's shadow at the clouds' resolution (clouds.frag; with the shadow blur)
bool sfOn2(uint bit) { return (uint(sfFlags.y + 0.5) & bit) != 0u; }
uniform vec4 sfBevel;     // x: bevel width (share of a voxel's edge), y: its strength, z: "smooth more"'s amount

// `static` where the shader language makes a bare global a uniform (HLSL, and SPIR-V built from it).
#if BGFX_SHADER_LANGUAGE_GLSL
#define SF_GLOBAL
#else
#define SF_GLOBAL static
#endif
SF_GLOBAL float mtnLodBase = 0.0;
SF_GLOBAL float mtnLodSlope = 0.001;
SF_GLOBAL float mtnFarCanopyLast = 0.0;
SF_GLOBAL vec3 mtnFarHitNormal = vec3(0.0, 1.0, 0.0);   // blocky distance: the face the march hit
SF_GLOBAL vec2 mtnFarHitBlock = vec2(0.0, 0.0);        // ... and its block's centre
SF_GLOBAL float mtnFarHitSize = 0.0;                   // ... and its block's size (the reconstruction's voxel edge)   // how much of the last mtnFarSurface was crown

// Ring f at xz: the ground's height and the cover (bilinear), the trees' height and whether they are
// conifers (the nearest texel's), if it holds all four texels round it.
bool mtnFarSampleIn(int f, vec2 xz, out float h, out float cover, out float treeH, out bool conifer) {
    h = 0.0; cover = 0.0; treeH = 0.0; conifer = false;
    vec4 R = mtnFarRing(f);
    if (R.w < 0.5) return false;
    float texel = R.z / float(MTN_FAR_RES);
    vec2 g = xz / texel - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2 t = g - vec2(i0);
    ivec2 a, b, c, d;
    if (!mtnFarQuad(f, R, i0, a, b, c, d)) return false;
    h = mix(mix(texelFetch(farHeight, a, 0).x, texelFetch(farHeight, b, 0).x, t.x),
            mix(texelFetch(farHeight, c, 0).x, texelFetch(farHeight, d, 0).x, t.x), t.y);
    cover = mix(mix(texelFetch(farColour, a, 0).a, texelFetch(farColour, b, 0).a, t.x),
                mix(texelFetch(farColour, c, 0).a, texelFetch(farColour, d, 0).a, t.x), t.y);
    ivec2 n = t.y < 0.5 ? (t.x < 0.5 ? a : b) : (t.x < 0.5 ? c : d);
    float code = floor(texelFetch(farCanopy, n, 0).a * 255.0 + 0.5);
    conifer = code >= 128.0;
    treeH = mod(code, 128.0) * 0.35;
    return true;
}

bool mtnFarHeightIn(int f, vec2 xz, out float h) {
    float cover, treeH;
    bool conifer;
    return mtnFarSampleIn(f, xz, h, cover, treeH, conifer);
}

// The first far ring this far out from the camera that could hold xz.
int mtnFarFirstRing(vec2 xz) {
    vec2 rel = abs(xz - pjvViewPosition().xz);
    float reach = max(rel.x, rel.y) / (mtnFar0.z * 7.0);
    return reach <= 1.0 ? 0 : int(clamp(ceil(log2(reach)), 0.0, float(MTN_FAR_RINGS - 1)));
}

bool mtnFarSample(vec2 xz, out float h, out float cover, out float treeH, out bool conifer, out int ring) {
    ring = -1;
    h = 0.0; cover = 0.0; treeH = 0.0; conifer = false;
    for (int f = mtnFarFirstRing(xz); f < MTN_FAR_RINGS; f++) {
        if (mtnFarSampleIn(f, xz, h, cover, treeH, conifer)) { ring = f; return true; }
    }
    return false;
}

// The finest far ring's ground height at xz; `ring` gets which. False beyond them all.
bool mtnFarHeight(vec2 xz, out float h, out int ring) {
    float cover, treeH;
    bool conifer;
    return mtnFarSample(xz, h, cover, treeH, conifer, ring);
}

vec3 mtnFarColour(int f, vec2 xz) {
    vec4 R = mtnFarRing(f);
    float texel = R.z / float(MTN_FAR_RES);
    vec2 g = xz / texel - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2 t = g - vec2(i0);
    ivec2 a, b, c, d;
    if (!mtnFarQuad(f, R, i0, a, b, c, d)) return vec3(0.5, 0.45, 0.4);
    vec3 ca = texelFetch(farColour, a, 0).rgb, cb = texelFetch(farColour, b, 0).rgb;
    vec3 cc = texelFetch(farColour, c, 0).rgb, cd = texelFetch(farColour, d, 0).rgb;
    return mix(mix(ca, cb, t.x), mix(cc, cd, t.x), t.y);
}

vec3 mtnFarCanopyColour(int f, vec2 xz) {
    vec4 R = mtnFarRing(f);
    float texel = R.z / float(MTN_FAR_RES);
    vec2 g = xz / texel - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2 t = g - vec2(i0);
    ivec2 a, b, c, d;
    if (!mtnFarQuad(f, R, i0, a, b, c, d)) return vec3(0.2, 0.3, 0.2);
    vec3 ca = texelFetch(farCanopy, a, 0).rgb, cb = texelFetch(farCanopy, b, 0).rgb;
    vec3 cc = texelFetch(farCanopy, c, 0).rgb, cd = texelFetch(farCanopy, d, 0).rgb;
    return mix(mix(ca, cb, t.x), mix(cc, cd, t.x), t.y);
}

vec3 mtnFarNormal(int f, vec2 xz) {
    float texel = mtnFarRing(f).z / float(MTN_FAR_RES);
    float hl, hr, hd, hu;
    if (!mtnFarHeightIn(f, xz - vec2(texel, 0.0), hl) || !mtnFarHeightIn(f, xz + vec2(texel, 0.0), hr) ||
        !mtnFarHeightIn(f, xz - vec2(0.0, texel), hd) || !mtnFarHeightIn(f, xz + vec2(0.0, texel), hu))
        return vec3(0.0, 1.0, 0.0);
    return normalize(vec3(hl - hr, 2.0 * texel, hd - hu));
}

// ---- The trees (R7) ----

vec3 sfHash32(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yxz + 33.33);
    return fract((q.xxy + q.yzz) * q.zyx);
}

// About the near trees' crowns (trees.hpp): round ones ~0.45 of their height across, at most most of a cell.
float mtnCrownRadius(float treeH) { return clamp(treeH * 0.45, 1.6, mtnCanopy.x * 0.8) * mtnCanopy.y; }

// How high the canopy stands over the ground at xz (m), given the cover and the trees' height there; how
// much of xz is crown (`inCrown`, 0..1) and how far up its crown (`crownT`, 0 rim .. 1 top). Without the
// trees as geometry: the cover raises the ground by its mean, as 55 did.
float mtnCanopyAt(vec2 xz, float cover, float treeH, bool conifer, float footprint, out float inCrown, out float crownT) {
    inCrown = 0.0;
    crownT = 0.0;
    if (cover < 0.004 || treeH < 0.5) return 0.0;
    if (!sfOn(SF_FAR_TREES)) { inCrown = min(cover * 1.7, 1.0); crownT = 0.6; return cover * treeH * 0.7; }
    float C = mtnCanopy.x;
    float r0 = mtnCrownRadius(treeH);
    float keep = clamp(cover * C * C / (3.14159 * r0 * r0 * 0.9), 0.0, 1.0);
    float meanH = min(cover, 0.95) * treeH * (conifer ? 0.4 : 0.6);
    float fade = smoothstep(0.2, 0.6, r0 / max(footprint, 1e-3));
    if (fade <= 0.0) { inCrown = min(cover, 1.0); crownT = 0.5; return meanH; }
    // The 2 x 2 cells nearest xz (the quadrant of its cell it is in): a crown reaches at most 0.8 of a cell
    // from a centre jittered over the middle 0.8, so farther cells almost never reach it -- 4 crowns
    // tested, not 9.
    vec2 g = xz / C;
    vec2 cell = floor(g - 0.5);
    float best = 0.0;
    float bestT = 0.0;
    for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++) {
            vec2 c = cell + vec2(float(dx), float(dy));
            vec3 hsh = sfHash32(c + vec2(0.37, 91.1));
            if (hsh.z > keep) continue;
            vec2 pos = (c + 0.1 + 0.8 * hsh.xy) * C;
            float size = 0.55 + 0.9 * fract(hsh.x * 7.31 + hsh.y * 3.17);   // many small, some big
            float r = r0 * sqrt(size);
            vec2 rel = xz - pos;
            float d2 = dot(rel, rel) / (r * r);
            if (d2 >= 1.0) continue;
            float d = sqrt(d2);
            float th = treeH * (0.55 + 0.45 * size);
            // Ragged rims: the crown's edge wanders with the angle round it.
            float ang = atan2(rel.y, rel.x);
            float rag = 0.85 + 0.15 * sin(ang * 5.0 + hsh.y * 20.0) * sin(ang * 3.0 + hsh.x * 11.0);
            float dd = clamp(d / rag, 0.0, 1.0);
            float prof = conifer ? pow(1.0 - dd, 0.85) : sqrt(max(1.0 - dd * dd, 0.0)) * (1.0 - 0.15 * dd);
            float hh = th * mix(0.25, 1.0, prof);
            if (dd >= 1.0) hh = 0.0;
            if (hh > best) { best = hh; bestT = prof; }
        }
    float inside = best > 0.0 ? 1.0 : 0.0;
    inCrown = mix(min(cover, 1.0), inside, fade);
    crownT = mix(0.5, bestT, fade);
    return mix(meanH, best, fade);
}

// The ground plus the canopy at xz: the surface the far march meets.
bool mtnFarTotal(vec2 xz, float footprint, out float h, out int ring, out float treeH) {
    float cover;
    bool conifer;
    if (!mtnFarSample(xz, h, cover, treeH, conifer, ring)) return false;
    float inC, ct;
    h += mtnCanopyAt(xz, cover, treeH, conifer, footprint, inC, ct);
    if (cover < 0.004) treeH = 0.0;
    return true;
}

// ---- Detail finer than the heightfield (55's, with a switch) ----

vec3 mtnNoised(vec2 p) {   // gradient noise and its derivative (Quilez)
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    vec2 ga = sin(vec2(dot(i, vec2(127.1, 311.7)), dot(i, vec2(269.5, 183.3))) * 43758.5453) * 2.0;
    ga = fract(ga) * 2.0 - 1.0;
    vec2 ib = i + vec2(1.0, 0.0), ic = i + vec2(0.0, 1.0), id = i + vec2(1.0, 1.0);
    vec2 gb = fract(sin(vec2(dot(ib, vec2(127.1, 311.7)), dot(ib, vec2(269.5, 183.3))) * 43758.5453) * 2.0) * 2.0 - 1.0;
    vec2 gc = fract(sin(vec2(dot(ic, vec2(127.1, 311.7)), dot(ic, vec2(269.5, 183.3))) * 43758.5453) * 2.0) * 2.0 - 1.0;
    vec2 gd = fract(sin(vec2(dot(id, vec2(127.1, 311.7)), dot(id, vec2(269.5, 183.3))) * 43758.5453) * 2.0) * 2.0 - 1.0;
    float va = dot(ga, f), vb = dot(gb, f - vec2(1.0, 0.0));
    float vc = dot(gc, f - vec2(0.0, 1.0)), vd = dot(gd, f - vec2(1.0, 1.0));
    float v = va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd);
    vec2 d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
             du * (u.yx * (va - vb - vc + vd) + vec2(vb, vc) - va);
    return vec3(v, d);
}

// `n` the heightfield's normal at xz, `texel` its texel (m), `pixel` how much ground a pixel covers (m).
vec3 mtnFarDetail(vec3 n, vec2 xz, float texel, float pixel, out float tone) {
    vec2 slope = -n.xz / max(n.y, 0.2);                 // the heightfield's gradient
    float steep = length(slope);
    vec2 down = steep > 1e-4 ? -slope / steep : vec2(1.0, 0.0);
    vec2 across = vec2(-down.y, down.x);
    // In a frame along the slope, stretched three times down it: gullies, not bumps.
    vec2 q = vec2(dot(xz, across), dot(xz, down) / 3.0);
    vec2 grad = vec2(0.0, 0.0);
    float wavelength = texel * 1.2;
    float amp = 1.5 * (0.35 + 0.65 * smoothstep(0.1, 0.8, steep));
    tone = 0.0;
    int octaves = int(8.0 * sfQuality.w + 0.5);
    for (int i = 0; i < 8; i++) {
        if (i >= octaves) break;
        float fade = smoothstep(1.5 * pixel, 3.0 * pixel, wavelength);
        if (fade <= 0.0) break;
        vec3 nd = mtnNoised(q / wavelength + vec2(float(i) * 17.31, float(i) * 3.7));
        // d(height)/dq = amp * wavelength * dnoise/dp / wavelength: a slope of about `amp` per octave.
        grad += nd.yz * amp * fade;
        tone += nd.x * fade * (i < 3 ? 0.5 : 0.25);   // gullies a little darker, ribs lighter
        wavelength *= 0.5;
        amp *= 0.7;
    }
    // And a grain to the colour, finer than the colour atlas, not stretched: the specks of trees, rock and
    // bare ground a far slope is made of. Down to a pixel; TAA averages what is below.
    float speck = 0.0;
    float w2 = texel * 0.6;
    float a2 = 1.0;
    int specks = int(6.0 * sfQuality.w + 0.5);
    for (int i = 0; i < 6; i++) {
        if (i >= specks) break;
        float fade = smoothstep(1.0 * pixel, 2.0 * pixel, w2);
        if (fade <= 0.0) break;
        speck += mtnNoised(xz / w2 + vec2(31.7 + float(i) * 5.1, 7.3)).x * a2 * fade;
        w2 *= 0.5;
        a2 *= 0.8;
    }
    tone += 0.8 * speck;
    // Back to world x, z (q = (along across, along down / 3)).
    vec2 g = across * grad.x + down * (grad.y / 3.0);
    return normalize(n + vec3(-g.x, 0.0, -g.y) * n.y);
}

// The ground's normal at xz on ring f, with the detail below its texels: for coarse ground voxels to shade
// by, so the steps of a slope do not show as stripes.
vec3 mtnFarGroundNormal(int f, vec2 xz, float pixel) {
    float texel = mtnFarRing(f).z / float(MTN_FAR_RES);
    vec3 n = mtnFarNormal(f, xz);
    if (sfOn(SF_FAR_DETAIL)) {
        float tone;
        n = normalize(mix(n, mtnFarDetail(n, xz, texel, pixel, tone), mtnCanopy.z));
    }
    return n;
}

// The far field's look at xz on ring f: its normal (the ground's, with detail, and the crowns') and its
// albedo (linear): crowns lighter toward their tops, the ground between them in patches of bare earth and
// grass. `pixel`: how much ground a pixel covers there.
void mtnFarSurface(int f, vec2 xz, float pixel, out vec3 normal, out vec3 albedo) {
    float texel = mtnFarRing(f).z / float(MTN_FAR_RES);
    if (sfOn(SF_FAR_BLOCKY)) {
        // A block: the face the march hit, one colour -- its ground and its trees by their cover, a little
        // darker on the sides, with a faint per-block tone.
        float h, cover, treeH;
        bool conifer;
        mtnFarSampleIn(f, mtnFarHitBlock, h, cover, treeH, conifer);
        vec3 ground = pjvSrgbToLinear(mtnFarColour(f, mtnFarHitBlock));
        vec3 crown = pjvSrgbToLinear(mtnFarCanopyColour(f, mtnFarHitBlock));
        float tone = fract(sin(dot(floor(mtnFarHitBlock / max(texel, 1e-3)), vec2(12.9898, 78.233))) * 43758.5453);
        // Seen from afar a woodland is mostly crowns: they hide the ground between them and shade what shows
        // (an 18% pinyon-juniper cover reads as half green). So the trees' share is what is seen, not the cover.
        float seen = 1.0 - exp(-8.0 * cover);   // matched by eye to the smooth far field's crowns and their shade
        albedo = mix(ground * (1.0 - 0.45 * seen), crown * 0.62, seen) * (0.92 + 0.16 * tone);
        normal = mtnFarHitNormal;
        mtnFarCanopyLast = 0.0;
        return;
    }
    float tone = 0.0;
    vec3 nG = mtnFarNormal(f, xz);
    if (sfOn(SF_FAR_DETAIL)) {
        vec3 nd = mtnFarDetail(nG, xz, texel, pixel, tone);
        nG = normalize(mix(nG, nd, mtnCanopy.z));
        tone *= mtnCanopy.z;
    }
    float h, cover, treeH;
    bool conifer;
    mtnFarSampleIn(f, xz, h, cover, treeH, conifer);
    vec3 ground = pjvSrgbToLinear(mtnFarColour(f, xz));
    // Patches of bare earth and of grass, tens of metres across, down to a few pixels.
    if (sfOn(SF_FAR_PATCHES)) {
        float k = mtnCanopy.w;
        float n1 = mtnNoised(xz / 55.0 + vec2(3.1, 7.7)).x * 0.6 + mtnNoised(xz / 190.0 + vec2(1.3, 2.9)).x * 0.4;
        float fine = mtnNoised(xz / 14.0 + vec2(5.3, 1.1)).x * smoothstep(2.0 * pixel, 5.0 * pixel, 14.0);
        float bare = smoothstep(-0.15, 0.35, n1 + 0.25 * fine);
        ground *= mix(vec3(1.0, 1.0, 1.0), vec3(1.18, 1.02, 0.88), bare * 0.7 * k);           // pale pink earth
        ground *= mix(vec3(1.0, 1.0, 1.0), vec3(0.78, 0.84, 0.66), (1.0 - bare) * 0.45 * k);   // darker grass
        ground *= 1.0 + 0.12 * fine * k;
    }
    ground *= clamp(1.0 + 0.35 * tone, 0.6, 1.3);
    float inC = 0.0, ct = 0.0;
    float c0 = mtnCanopyAt(xz, cover, treeH, conifer, pixel, inC, ct);
    vec3 n = nG;
    if (c0 > 0.0 && sfOn(SF_FAR_TREES)) {
        float e = max(0.2, mtnCrownRadius(treeH) * 0.12);
        float i1, i2, t1, t2;
        float cx = mtnCanopyAt(xz + vec2(e, 0.0), cover, treeH, conifer, pixel, i1, t1);
        float cz = mtnCanopyAt(xz + vec2(0.0, e), cover, treeH, conifer, pixel, i2, t2);
        vec2 gG = -nG.xz / max(nG.y, 0.1);
        vec2 g = gG + vec2(cx - c0, cz - c0) / e;
        n = normalize(vec3(-g.x, 1.0, -g.y));
    }
    // The light does the shading now (crowns are geometry, and the diffuse rays see them): only a little
    // darker toward the crown's foot, for the shade inside it.
    vec3 crown = pjvSrgbToLinear(mtnFarCanopyColour(f, xz)) * mix(0.8, 1.05, ct);
    albedo = mix(ground, crown, inC);
    normal = n;
    mtnFarCanopyLast = inC;
}

// Where a ray leaves the voxel rings' box (in x and z), or 0 if it starts outside it or there is none.
float mtnLeaveVoxels(vec3 o, vec3 d) {
    vec4 B = mtnVoxelBox;
    if (B.x >= B.z) return 0.0;
    if (o.x < B.x || o.x > B.z || o.z < B.y || o.z > B.w) return 0.0;
    float tx = d.x > 0.0 ? (B.z - o.x) / d.x : (d.x < 0.0 ? (B.x - o.x) / d.x : 1e30);
    float tz = d.z > 0.0 ? (B.w - o.z) / d.z : (d.z < 0.0 ? (B.y - o.z) / d.z : 1e30);
    return min(tx, tz);
}


// The far field (ground and crowns) along a ray from t0 to tMax: steps of most of a texel, longer the
// higher the ray is over the ground, shorter than a crown where it is among the trees; a crossing is found
// by halving. `ring` gets the ring it hit. A shadow ray (`shadow`) needs no exact crossing.
// Blocky distance (santa-fe-reference S10): the far field as flat-topped columns, a block sfFar.x texels of the
// ring a point falls in -- 25 m 3 km out, 800 m on the horizon at 4 -- marched exactly, block by block: one
// height a block (its centre's, the trees' mean standing on it). Where the ring changes, the march restarts on
// the new ring's blocks (their grids nest).
// A block stands at the lowest of its centre and four points toward its corners: where one ring's blocks
// meet the next's (twice the size), the step then falls away from the camera rather than standing up as a
// dark wall.
float mtnBlockHeight(int f, vec2 centre, float B, out bool held) {
    float h, cover, treeH;
    bool conifer;
    held = mtnFarSampleIn(f, centre, h, cover, treeH, conifer);
    float lowest = h;
    for (int c = 0; c < 4; c++) {
        vec2 o = vec2(c == 0 || c == 2 ? -0.4 : 0.4, c < 2 ? -0.4 : 0.4) * B;
        float hc, cc, tc;
        bool cf;
        if (mtnFarSampleIn(f, centre + o, hc, cc, tc, cf)) lowest = min(lowest, hc);
    }
    return lowest + min(cover, 1.0) * treeH * 0.2;   // the trees a little
}

// The highest ring f's column col stands (ground and trees), or 1e9 if it does not hold it.
float mtnFarColumnMax(int f, ivec2 col) {
    ivec2 tile;
    if (!mtnFarColumn(f, mtnFarRing(f), col, tile)) return 1e9;
    ivec2 slot = ivec2(mod(vec2(col), float(MTN_FAR_SPAN)));
    return texelFetch(farMax, ivec2(f * MTN_FAR_SPAN + slot.x, slot.y), 0).x;
}

bool mtnFarBlockyMarch(vec3 o, vec3 d, float t0, float tMax, out float tHit, out int ring) {
    tHit = -1.0;
    ring = -1;
    float t = t0;
    int f = -1;
    float B = 1.0;
    vec2 cell = vec2(0.0, 0.0), stepDir = vec2(0.0, 0.0), tDelta = vec2(0.0, 0.0), tNext = vec2(0.0, 0.0);
    bool first = true;
    int axis = 1;
    float tStart = t0;   // where the march last (re)started on a ring's blocks
    for (int i = 0; i < 600; i++) {
        if (t > tMax) return false;
        vec3 p = o + d * t;
        if (p.y > MTN_FAR_TOP && d.y >= 0.0) return false;
        // The finest ring that holds the column here (by distance first, then outward: near a ring's edge its
        // column may not be held, and the march must not slide past the ground looking for it).
        int fp = mtnFarFirstRing(p.xz);
        for (int k = 0; k < MTN_FAR_RINGS; k++) {
            if (fp >= MTN_FAR_RINGS - 1) break;
            vec4 Rk = mtnFarRing(fp);
            if (Rk.w > 0.5 && mtnFarColumnMax(fp, ivec2(floor(p.xz / Rk.z))) < 1e8) break;
            fp++;
        }
        if (fp != f) {
            // (Re)start on this ring's blocks.
            f = fp;
            tStart = t;
            if (mtnFarRing(f).w < 0.5) return false;
            B = mtnFarRing(f).z / float(MTN_FAR_RES) * sfFar.x;
            vec2 q = p.xz + d.xz * 1e-4;
            cell = floor(q / B);
            stepDir = vec2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
            vec2 invD = vec2(abs(d.x) > 1e-7 ? 1.0 / abs(d.x) : 1e30, abs(d.z) > 1e-7 ? 1.0 / abs(d.z) : 1e30);
            tDelta = B * invD;
            vec2 edge = (cell + max(stepDir, vec2(0.0, 0.0))) * B;
            tNext = vec2(t, t) + abs(edge - p.xz) * invD;
        }
        // Over a whole column's highest point all the way across it: skip the column.
        {
            float C = mtnFarRing(f).z;
            vec2 colF = floor((p.xz + d.xz * 1e-3) / C);
            float cmax = mtnFarColumnMax(f, ivec2(colF));
            if (cmax < 1e8) {
                vec2 lo = colF * C, hi = lo + C;
                float tx = d.x > 0.0 ? (hi.x - o.x) / d.x : (d.x < 0.0 ? (lo.x - o.x) / d.x : 1e30);
                float tz = d.z > 0.0 ? (hi.y - o.z) / d.z : (d.z < 0.0 ? (lo.y - o.z) / d.z : 1e30);
                float tCol = min(tx, tz);
                if (min(p.y, o.y + d.y * tCol) > cmax + 0.5) {
                    t = tCol + 1e-3 * C;
                    f = -1;
                    first = false;
                    continue;
                }
            }
        }
        bool held;
        vec2 centre = (cell + 0.5) * B;
        float hb = mtnBlockHeight(f, centre, B, held);
        if (!held) {   // a block straddling the held edge: take it from the next ring out
            if (f + 1 >= MTN_FAR_RINGS) return false;
            float Bn = B * 2.0;
            hb = mtnBlockHeight(f + 1, (floor(centre / Bn) + 0.5) * Bn, Bn, held);
            if (!held) return false;
        }
        float tExit = min(tNext.x, tNext.y);
        float yIn = o.y + d.y * t;
        if (yIn <= hb) {
            tHit = t;
            ring = f;
            mtnFarHitBlock = centre;
            mtnFarHitSize = B;
            // Within the first block past where the march began (the voxels' edge), a ray below a block's top is
            // landing on it, not meeting a wall: shaded as a top (a side there drew a dark wall along the edge).
            bool entering = first || t < tStart + 1.5 * B;   // also where a ring's blocks meet the next's
            mtnFarHitNormal = entering ? vec3(0.0, 1.0, 0.0) : (axis == 0 ? vec3(-stepDir.x, 0.0, 0.0) : vec3(0.0, 0.0, -stepDir.y));
            return true;
        }
        float yOut = o.y + d.y * tExit;
        if (yOut <= hb && d.y < 0.0) {
            tHit = (hb - o.y) / d.y;
            ring = f;
            mtnFarHitBlock = centre;
            mtnFarHitSize = B;
            mtnFarHitNormal = vec3(0.0, 1.0, 0.0);
            return true;
        }
        first = false;
        if (tNext.x < tNext.y) { cell.x += stepDir.x; t = tNext.x; tNext.x += tDelta.x; axis = 0; }
        else { cell.y += stepDir.y; t = tNext.y; tNext.y += tDelta.y; axis = 1; }
    }
    return false;
}

bool mtnFarMarch(vec3 o, vec3 d, float t0, float tMax, bool shadow, out float tHit, out int ring) {
    if (sfOn(SF_FAR_BLOCKY)) return mtnFarBlockyMarch(o, d, t0, tMax, tHit, ring);
    tHit = -1.0;
    ring = -1;
    float t = t0;
    float prevT = t0;
    for (int i = 0; i < 700; i++) {
        if (t > tMax) return false;
        vec3 p = o + d * t;
        if (p.y > MTN_FAR_TOP && d.y >= 0.0) return false;
        float fp = mtnLodBase + mtnLodSlope * t;
        float h, treeH, cover;
        bool conifer;
        int f;
        if (!mtnFarSample(p.xz, h, cover, treeH, conifer, f)) return false;
        // The crowns only where the ray could meet them: well above the tallest, the ground is enough.
        if (cover >= 0.004 && p.y - h < treeH * 1.05 + 1.0) {
            float inC, ct;
            h += mtnCanopyAt(p.xz, cover, treeH, conifer, fp, inC, ct);
        } else if (cover < 0.004) {
            treeH = 0.0;
        }
        float above = p.y - h;
        if (above <= 0.0) {
            if (shadow) { tHit = t; ring = f; return true; }
            float lo = prevT, hi = t;
            for (int j = 0; j < 7; j++) {
                float m = 0.5 * (lo + hi);
                vec3 q = o + d * m;
                float hm, tm;
                int fm;
                if (mtnFarTotal(q.xz, mtnLodBase + mtnLodSlope * m, hm, fm, tm) && q.y <= hm) hi = m; else lo = m;
            }
            tHit = hi;
            ring = f;
            return true;
        }
        float texel = mtnFarRing(f).z / float(MTN_FAR_RES);
        float step = max(max(texel * 0.7, t * 0.0012), above * 0.45);
        // Among the crowns: steps shorter than a crown, or the ray slips between them.
        // Among the crowns: steps shorter than a crown, or the ray slips between them -- only where crowns
        // are drawn as crowns (well under a pixel they are their mean height, a smooth surface).
        if (sfOn(SF_FAR_TREES) && treeH > 0.5 && above < treeH * 1.3) {
            float r0 = mtnCrownRadius(treeH);
            if (r0 > 0.2 * fp) step = min(step, max(r0 * 0.6, max(fp * 0.5, 0.25)));
        }
        prevT = t;
        t += step;
    }
    return false;
}

#endif
