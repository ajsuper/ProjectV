// =============================================================================
// mountains_far.sc  --  the far field: the heightfield rings past the voxels (plan D14-D17).
//
// Each far ring f has a square of the height atlas (R32F) and the colour atlas (RGBA8, sRGB): its columns'
// 64 x 64 tiles at (column mod 16, column mod 16), so a ring moving with the camera rewrites only the
// tiles it moves onto. The state texture says which column each tile holds (the low bytes of its ix, iz,
// and b = 1 once it holds one): a stale tile -- a column the ring has left, not yet replaced -- is not read.
//
// mtnFarN: ring N's origin x, z (m), its column's size (m; a texel is that / 64), and whether it is placed.
// mtnVoxelBox: the box (min x, min z, max x, max z) the voxel rings cover: the heightfield is not marched
// inside it, where the voxels are the ground.
//
// A height is bilinear between texel centres, a normal from the heights round it: a smooth surface.
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

// Ring f's height at xz, if it holds all four texels round it.
bool mtnFarHeightIn(int f, vec2 xz, out float h) {
    h = 0.0;
    vec4 R = mtnFarRing(f);
    if (R.w < 0.5) return false;
    float texel = R.z / float(MTN_FAR_RES);
    vec2 g = xz / texel - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2 t = g - vec2(i0);
    ivec2 a, b, c, d;
    if (!mtnFarQuad(f, R, i0, a, b, c, d)) return false;
    float ha = texelFetch(farHeight, a, 0).x, hb = texelFetch(farHeight, b, 0).x;
    float hc = texelFetch(farHeight, c, 0).x, hd = texelFetch(farHeight, d, 0).x;
    h = mix(mix(ha, hb, t.x), mix(hc, hd, t.x), t.y);
    return true;
}

// The finest far ring's height at xz; `ring` gets which. False beyond them all.
bool mtnFarHeight(vec2 xz, out float h, out int ring) {
    ring = -1;
    h = 0.0;
    // Rings are centred on the camera, each twice as wide as the last: start at the one this far out
    // first lies in (ring 0 reaches 8 of its columns out, less a column of margin).
    vec2 rel = abs(xz - pjvViewPosition().xz);
    float reach = max(rel.x, rel.y) / (mtnFar0.z * 7.0);
    int f0 = reach <= 1.0 ? 0 : int(clamp(ceil(log2(reach)), 0.0, float(MTN_FAR_RINGS - 1)));
    for (int f = f0; f < MTN_FAR_RINGS; f++) {
        if (mtnFarHeightIn(f, xz, h)) { ring = f; return true; }
    }
    return false;
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

vec3 mtnFarNormal(int f, vec2 xz) {
    float texel = mtnFarRing(f).z / float(MTN_FAR_RES);
    float hl, hr, hd, hu;
    if (!mtnFarHeightIn(f, xz - vec2(texel, 0.0), hl) || !mtnFarHeightIn(f, xz + vec2(texel, 0.0), hr) ||
        !mtnFarHeightIn(f, xz - vec2(0.0, texel), hd) || !mtnFarHeightIn(f, xz + vec2(0.0, texel), hu))
        return vec3(0.0, 1.0, 0.0);
    return normalize(vec3(hl - hr, 2.0 * texel, hd - hu));
}

// ---- Detail finer than the heightfield ----
//
// The far rings hold the ground at 8 to 256 m a texel: smooth. What is finer -- gullies, ribs of rock,
// the grain of a slope -- is put back in the normal alone: octaves of gradient noise from just under a
// texel down to a pixel and a half (faded out below, so it never shimmers), stretched down the slope so it
// reads as gullies and ribs rather than bumps, strongest where the ground is steep; `tone` darkens the
// gullies and lightens the ribs a little, for the albedo.

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
    float amp = 1.1 * (0.35 + 0.65 * smoothstep(0.1, 0.8, steep));
    tone = 0.0;
    for (int i = 0; i < 8; i++) {
        float fade = smoothstep(1.5 * pixel, 3.0 * pixel, wavelength);
        if (fade <= 0.0) break;
        vec3 nd = mtnNoised(q / wavelength + vec2(float(i) * 17.31, float(i) * 3.7));
        // d(height)/dq = amp * wavelength * dnoise/dp / wavelength: a slope of about `amp` per octave.
        grad += nd.yz * amp * fade;
        tone += nd.x * fade * (i < 3 ? 0.5 : 0.25);   // gullies a little darker, ribs lighter
        wavelength *= 0.5;
        amp *= 0.7;
    }
    // Back to world x, z (q = (along across, along down / 3)).
    vec2 g = across * grad.x + down * (grad.y / 3.0);
    return normalize(n + vec3(-g.x, 0.0, -g.y) * n.y);
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

// The heightfield along a ray from t0 to tMax: steps of at least most of a texel, longer the higher the
// ray is over the ground; a crossing is found by halving. `ring` gets the ring it hit.
bool mtnFarMarch(vec3 o, vec3 d, float t0, float tMax, out float tHit, out int ring) {
    tHit = -1.0;
    ring = -1;
    float t = t0;
    float prevT = t0;
    for (int i = 0; i < 400; i++) {
        if (t > tMax) return false;
        vec3 p = o + d * t;
        if (p.y > MTN_FAR_TOP && d.y >= 0.0) return false;
        float h;
        int f;
        if (!mtnFarHeight(p.xz, h, f)) return false;
        float above = p.y - h;
        if (above <= 0.0) {
            float lo = prevT, hi = t;
            for (int j = 0; j < 6; j++) {
                float m = 0.5 * (lo + hi);
                vec3 q = o + d * m;
                float hm;
                int fm;
                if (mtnFarHeight(q.xz, hm, fm) && q.y <= hm) hi = m; else lo = m;
            }
            tHit = hi;
            ring = f;
            return true;
        }
        float texel = mtnFarRing(f).z / float(MTN_FAR_RES);
        prevT = t;
        t += max(max(texel * 0.7, t * 0.0012), above * 0.45);   // never finer than ~half a pixel out there
    }
    return false;
}

#endif
