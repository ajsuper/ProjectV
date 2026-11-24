$input v_color0
$input v_texcoord0

// =============================================================================
// shadow_soft.frag -- soft sun shadows from one hard shadow ray a texel (santa-fe-reference S11), at the trace
// pass's resolution, in optimized mode (reference mode traces the sun's disc instead).
//
// The trace writes the sun's light apart (unshadowed, already through the air and the clouds in front), with
// whether its shadow ray got through and how far it went before it was blocked. Here, PCSS-style: the mean
// blocker distance of the shadowed texels round this one sets the penumbra (`blocker * sfShadow.x` metres: a
// contact shadow stays sharp, a shadow cast from far is soft), and the visibility is averaged over that radius
// on the same surface only (plane distance and normal), so a shadow does not leak across an edge. Then the sun
// is added back: the frame the reconstruction rebuilds.
//
// Inputs: the trace pass (colour 0, gPos 1, gNormal 2 -- w the blocker distance --, gFace 3, gRot 4, gMotion 5,
// gSun 6: rgb the sun's light, a its visibility), then the clouds (clouds 7, cloudDepth 8): composited last.
// Outputs: 0 the lit frame; 1-3 the G-buffer the reconstruction reads (normal, face, rotation): the trace
// pass's, or a cloud box's where the blocky style's clouds are in front.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>
#include <pjv_camera.sc>

SAMPLER2D(traced, 0);
SAMPLER2D(gPos, 1);
SAMPLER2D(gNormal, 2);
SAMPLER2D(gFace, 3);
SAMPLER2D(gRot, 4);
SAMPLER2D(gMotion, 5);    // z the sky light's share of the colour, w the leaf glow's
SAMPLER2D(gSun, 6);       // a: the sun's visibility, + 2 on leaves
SAMPLER2D(clouds, 7);       // clouds.frag: rgb their light, a what they let through
SAMPLER2D(cloudDepth, 8);   // x their front
SAMPLER2D(aoTraced, 9);     // ao_trace.frag: r the traced AO at half this resolution, g its depth

// The clouds over a lit texel, where they are in front of what it sees.
// Catmull-Rom from the clouds' quarter of the texels (bilinear made them soft; their edges are hard).
vec4 cloudsAt(vec2 uv) {
    vec2 size = vec2(textureSize(clouds, 0));
    vec2 p = uv * size - 0.5;
    vec2 f = fract(p);
    vec2 i = floor(p) + 0.5;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            float wx = x == 0 ? w0.x : (x == 1 ? w1.x : (x == 2 ? w2.x : w3.x));
            float wy = y == 0 ? w0.y : (y == 1 ? w1.y : (y == 2 ? w2.y : w3.y));
            sum += texture2D(clouds, (i + vec2(float(x) - 1.0, float(y) - 1.0)) / size) * (wx * wy);
        }
    return vec4(max(sum.rgb, vec3(0.0, 0.0, 0.0)), clamp(sum.a, 0.0, 1.0));
}

vec3 withClouds(vec3 c, vec2 uv, float depth) {
    vec4 cl = cloudsAt(uv);
    float front = texture2D(cloudDepth, uv).x;
    if (depth >= 0.0 && depth < front) return c;
    return c * cl.a + cl.rgb;
}

uniform vec4 sunDir;
uniform vec4 mtnSun;     // rgb the sun's light
uniform vec4 sfAOBlur;   // x radius (texels)
uniform vec4 sfSSAO;     // x radius (m), y strength, z most texels across, w on
uniform vec4 frameCount;
uniform vec4 sfLeafSpread;   // x: how far the leaf glow's light spreads through a crown (m), y: how much, z: most texels
// gSun.a: the visibility, + 2 on leaves.
float knownVis(float a) { return a >= 1.5 ? a - 2.0 : a; }
uniform vec4 sfShadow;   // x: penumbra per metre of blocker distance (0: hard), y: largest radius (texels)
uniform vec4 sfFlags;    // the feature bits (mountains_far.sc): y bit 0 the box clouds
uniform vec4 jitterParams;

// The blocky style's clouds, rebuilt at this resolution (santa-fe-reference S13): each cloud texel recorded the
// box its ray met first; this texel's own ray is tested against the boxes its neighbours recorded, and the
// nearest it enters is what it sees -- that box's light, and its face handed to the voxel reconstruction (the
// G-buffer outputs below), so box clouds get the voxels' hard, antialiased edges. False: no box in front.
bool cloudBox(vec2 uv, float depth, out vec4 cl, out vec3 n, out vec4 face) {
    cl = vec4(0.0, 0.0, 0.0, 1.0); n = vec3(0.0, 1.0, 0.0); face = vec4(0.0, 0.0, 0.0, 0.0);
    vec2 csize = vec2(textureSize(clouds, 0));
    vec3 camO = pjvViewPosition();
    vec3 dir = pjvViewRayDirection(uv + jitterParams.xy * passTargetRes.zw, passTargetRes.xy);
    vec3 invD = 1.0 / dir;
    ivec2 c0 = ivec2(floor(uv * csize));
    float best = 1e30;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(c0 + ivec2(x, y), ivec2(0, 0), ivec2(csize) - 1);
            vec4 cd = texelFetch(cloudDepth, q, 0);
            float size = cd.z;
            if (size <= 0.0 || cd.x >= 1e8) continue;
            vec3 dq = pjvViewRayDirection((vec2(q) + 0.5) / csize, csize);
            vec3 cell = floor((camO + dq * (cd.x + size * 0.02)) / size);
            vec3 lo = cell * size, hi = lo + size;
            vec3 t0 = (lo - camO) * invD, t1 = (hi - camO) * invD;
            vec3 tn = min(t0, t1), tf = max(t0, t1);
            float tEnter = max(max(tn.x, tn.y), tn.z), tExit = min(min(tf.x, tf.y), tf.z);
            if (tExit <= max(tEnter, 0.0) || tEnter >= best) continue;
            if (depth >= 0.0 && tEnter > depth) continue;   // behind what this texel sees
            // A box under two texels: left to the smooth composite (rebuilt edges that small only sparkle).
            if (size < 2.0 * tEnter * 2.0 * pjvViewTanHalfFov() / passTargetRes.y) continue;
            best = tEnter;
            cl = texelFetch(clouds, q, 0);
            n = tEnter == tn.x ? vec3(-sign(dir.x), 0.0, 0.0) : (tEnter == tn.y ? vec3(0.0, -sign(dir.y), 0.0) : vec3(0.0, 0.0, -sign(dir.z)));
            vec3 centre = lo + 0.5 * size;
            face = vec4(centre + n * (0.5 * size), size);
        }
    return best < 1e29;
}

// The AO (SF2_AO), two parts multiplied:
//  - the traced AO (ao_trace.frag, half this resolution, accumulated over frames), upsampled and blurred
//    along this texel's surface (normal and plane) over sfAOBlur.x texels: the big, dark shapes -- under a
//    crown, behind a boulder;
//  - a screen-space AO from this resolution's depth (gPos), 8 taps over sfSSAO.x metres: the creases a half-
//    resolution texel blurs away -- grass roots, a rock's foot.
float tracedAO(ivec2 q) {
    vec2 lsize = vec2(textureSize(aoTraced, 0));
    return texelFetch(aoTraced, clamp(ivec2((vec2(q) + 0.5) * lsize / passTargetRes.xy), ivec2(0, 0), ivec2(lsize) - 1), 0).r;
}

float screenAO(ivec2 at, vec4 p0, vec3 n0, float texelWorld) {
    if (sfSSAO.w < 0.5) return 1.0;
    ivec2 size = ivec2(passTargetRes.xy);
    float radius = sfSSAO.x;
    float rPix = min(radius / max(texelWorld, 1e-4), sfSSAO.z);
    if (rPix < 1.5) return 1.0;
    radius = rPix * texelWorld;
    // A rotation per texel and frame (interleaved gradient noise); the resolve averages the frames.
    float rot = fract(52.9829189 * fract(dot(vec2(at) + float(int(frameCount.x) % 64) * 5.588238, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    float occ = 0.0;
    for (int i = 0; i < 8; i++) {
        float rr = sqrt((float(i) + 0.5) / 8.0) * rPix;
        float a = float(i) * 2.39996323 + rot;
        ivec2 q = clamp(at + ivec2(round(vec2(cos(a), sin(a)) * rr)), ivec2(0, 0), size - 1);
        vec4 pq = texelFetch(gPos, q, 0);
        if (pq.w < 0.0) continue;
        vec3 v = pq.xyz - p0.xyz;
        float l = length(v);
        if (l < 1e-4) continue;
        float fall = clamp(1.0 - l / (radius * 2.0), 0.0, 1.0);
        occ += max(dot(n0, v / l) - 0.15, 0.0) * fall;
    }
    return clamp(1.0 - sfSSAO.y * occ / 8.0 * 1.6, 0.0, 1.0);
}

float aoAt(ivec2 at, float depth) {
    ivec2 size = ivec2(passTargetRes.xy);
    vec4 p0 = texelFetch(gPos, at, 0);
    vec3 n0 = texelFetch(gNormal, at, 0).xyz;
    float texelWorld = max(depth, 1e-3) * 2.0 * pjvViewTanHalfFov() / passTargetRes.y;
    float sum = tracedAO(at), wsum = 1.0;
    float r = sfAOBlur.x;
    for (int i = 0; i < 12; i++) {
        float rr = sqrt((float(i) + 0.5) / 12.0) * r;
        float a = float(i) * 2.39996323;
        ivec2 q = clamp(at + ivec2(round(vec2(cos(a), sin(a)) * rr)), ivec2(0, 0), size - 1);
        vec4 pq = texelFetch(gPos, q, 0);
        if (pq.w < 0.0) continue;
        float plane = abs(dot(pq.xyz - p0.xyz, n0)) / max(texelWorld * 2.0, 0.05);
        float w = max(dot(texelFetch(gNormal, q, 0).xyz, n0), 0.0);
        w = w * w * exp(-plane * plane);
        sum += tracedAO(q) * w;
        wsum += w;
    }
    return (sum / wsum) * screenAO(at, p0, n0, texelWorld);
}

// The lit texel out, with the clouds over it, and the G-buffer the reconstruction rebuilds from.
void finish(ivec2 at, vec2 uv, vec3 c, float depth, out vec4 o0, out vec4 o1, out vec4 o2, out vec4 o3) {
    if ((uint(sfFlags.y + 0.5) & 128u) != 0u && depth >= 0.0) {
        float share = texelFetch(gMotion, at, 0).z;
        if (share > 0.0) c *= 1.0 - share * (1.0 - aoAt(at, depth));
    }
    vec4 nrm = texelFetch(gNormal, at, 0);
    vec4 face = texelFetch(gFace, at, 0);
    vec4 rot = texelFetch(gRot, at, 0);
    vec4 cl;
    vec3 bn;
    vec4 bface;
    if ((uint(sfFlags.y + 0.5) & 1u) != 0u && cloudBox(uv, depth, cl, bn, bface)) {   // box clouds (SF2_CLOUD_BLOCKY)
        c = c * cl.a + cl.rgb;
        nrm = vec4(bn, 0.0);
        face = bface;
        rot = vec4(0.0, 0.0, 0.0, 1.0);
    } else {
        c = withClouds(c, uv, depth);
    }
    o0 = vec4(c, 1.0);
    o1 = nrm;
    o2 = face;
    o3 = rot;
}
// (bgfx's HLSL takes the outputs in main only.)
#define FINISH(c, d) { vec4 o0, o1, o2, o3; finish(at, v_texcoord0, c, d, o0, o1, o2, o3); gl_FragData[0] = o0; gl_FragData[1] = o1; gl_FragData[2] = o2; gl_FragData[3] = o3; }

// The far field's shadow (clouds.frag: floor(depth * 2) + visibility * 0.99 in cloudDepth.w) at this texel: the
// four cloud texels round it, bilinear, each weighed by how near its surface's depth is to this texel's.
float farVisAt(ivec2 at) {
    vec2 csize = vec2(textureSize(cloudDepth, 0));
    float d0 = texelFetch(gPos, at, 0).w;
    if (d0 < 0.0) return 1.0;
    vec2 g = (vec2(at) + 0.5) * csize / passTargetRes.xy - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2 f = g - vec2(i0);
    float sum = 0.0, wsum = 0.0, nearest = 1.0, nearW = 1e30;
    for (int k = 0; k < 4; k++) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 q = clamp(i0 + o, ivec2(0, 0), ivec2(csize) - 1);
        float w4 = texelFetch(cloudDepth, q, 0).w;
        if (w4 <= 0.0) continue;
        float key = floor(w4);
        float v = clamp((w4 - key) / 0.99, 0.0, 1.0);
        float dq = key * 0.5;
        float rel = abs(dq - d0) / max(d0, 1.0);
        float wb = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float w = wb * exp(-rel * rel * 400.0);
        sum += v * w; wsum += w;
        if (rel < nearW) { nearW = rel; nearest = v; }
    }
    return wsum > 1e-4 ? sum / wsum : nearest;
}

// The sun's light at a texel whose visibility is `vis`. On a leaf (gSun.a + 2) the glow's part of it
// (gMotion.w) is lit by the better of that and the light the leaves round it found -- the visibility of the
// leaf texels within sfLeafSpread.x metres at about this depth, nearer counting more: light that reached a
// crown's edge goes on through the leaves inside it, so a backlit crown glows through its body, not only
// at the voxels the sun reaches.
vec3 sunLight(ivec2 at, vec4 sun, float vis) {
    float share = texelFetch(gMotion, at, 0).w;
    if (share <= 0.0 || sun.a < 1.5 || sfLeafSpread.y <= 0.0) return sun.rgb * vis;
    vec4 p0 = texelFetch(gPos, at, 0);
    if (p0.w < 0.0) return sun.rgb * vis;
    ivec2 size = ivec2(passTargetRes.xy);
    float texelWorld = max(p0.w, 1e-3) * 2.0 * pjvViewTanHalfFov() / passTargetRes.y;
    float rPix = min(sfLeafSpread.x / texelWorld, sfLeafSpread.z);
    float spread = vis;
    if (rPix >= 1.5) {
        float turn = fract(52.9829189 * fract(dot(vec2(at), vec2(0.06711056, 0.00583715)))) * 6.2831853;
        float sum = vis, wsum = 1.0;
        for (int i = 0; i < 16; i++) {
            float r = sqrt((float(i) + 0.5) / 16.0) * rPix;
            float a = float(i) * 2.39996323 + turn;
            ivec2 q = clamp(at + ivec2(round(vec2(cos(a), sin(a)) * r)), ivec2(0, 0), size - 1);
            float aq = texelFetch(gSun, q, 0).a;
            if (aq < 1.5) continue;   // not a leaf
            vec4 pq = texelFetch(gPos, q, 0);
            if (pq.w < 0.0 || length(pq.xyz - p0.xyz) > sfLeafSpread.x * 1.5) continue;   // another crown
            float w = 1.0 - 0.7 * r / rPix;
            sum += (aq - 2.0) * w;
            wsum += w;
        }
        spread = sum / wsum;
    }
    float leafVis = max(vis, min(spread * sfLeafSpread.y * 2.0, 1.0));
    return sun.rgb * ((1.0 - share) * vis + share * leafVis);
}

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec3 colour = texelFetch(traced, at, 0).rgb;
    vec4 sun = texelFetch(gSun, at, 0);
    // The far field's share of the shadow (clouds.frag, SF2_FAR_SHADOW_LOW): on the sun's light (the leaf glow
    // rides in it: a crown in a ridge's shadow does not glow).
    if ((uint(sfFlags.y + 0.5) & 256u) != 0u) {
        float fv = farVisAt(at);
        sun.rgb *= fv;
    }
    if (dot(sun.rgb, sun.rgb) <= 0.0 || sfShadow.x <= 0.0) {
        FINISH(colour + sunLight(at, sun, knownVis(sun.a)), texelFetch(gPos, at, 0).w);
        return;
    }
    ivec2 size = ivec2(passTargetRes.xy);
    vec4 p0 = texelFetch(gPos, at, 0);
    vec3 n0 = texelFetch(gNormal, at, 0).xyz;
    float texelWorld = max(p0.w, 1e-3) * 2.0 * pjvViewTanHalfFov() / passTargetRes.y;
    float maxR = sfShadow.y;

    // Golden-angle spiral taps, the pattern turned per texel (a fixed, interleaved turn: no per-frame noise).
    float turn = fract(52.9829189 * fract(dot(vec2(at), vec2(0.06711056, 0.00583715)))) * 6.2831853;

    // 1. The blockers round here: their mean distance (shadowed texels only).
    float blockSum = 0.0, blockN = 0.0;
    for (int i = 0; i < 12; i++) {
        float r = sqrt((float(i) + 0.5) / 12.0) * maxR;
        float a = float(i) * 2.39996323 + turn;
        ivec2 q = clamp(at + ivec2(round(vec2(cos(a), sin(a)) * r)), ivec2(0, 0), size - 1);
        vec4 sq = texelFetch(gSun, q, 0);
        float b = texelFetch(gNormal, q, 0).w;
        if (knownVis(sq.a) < 0.5 && b > 0.0) { blockSum += b; blockN += 1.0; }
    }
    if (blockN < 0.5) {   // nothing shadowed near: as it is
        FINISH(colour + sunLight(at, sun, knownVis(sun.a)), texelFetch(gPos, at, 0).w);
        return;
    }
    float penumbra = (blockSum / blockN) * sfShadow.x;           // metres
    float radius = clamp(penumbra / texelWorld, 0.0, maxR);      // texels
    if (radius < 0.75) {
        FINISH(colour + sunLight(at, sun, knownVis(sun.a)), texelFetch(gPos, at, 0).w);
        return;
    }
    // 2. The visibility over that radius, on this surface only.
    float vis = knownVis(sun.a), wsum = 1.0;
    for (int i = 0; i < 16; i++) {
        float r = sqrt((float(i) + 0.5) / 16.0) * radius;
        float a = float(i) * 2.39996323 + turn;
        ivec2 q = clamp(at + ivec2(round(vec2(cos(a), sin(a)) * r)), ivec2(0, 0), size - 1);
        vec4 pq = texelFetch(gPos, q, 0);
        if (pq.w < 0.0) continue;
        vec3 nq = texelFetch(gNormal, q, 0).xyz;
        float plane = abs(dot(pq.xyz - p0.xyz, n0)) / max(texelWorld * 2.0, 0.05);
        float w = max(dot(nq, n0), 0.0);
        w *= w * exp(-plane * plane);
        float aq = texelFetch(gSun, q, 0).a;
        vis += knownVis(aq) * w;
        wsum += w;
    }
    FINISH(colour + sunLight(at, sun, vis / wsum), p0.w);
}
