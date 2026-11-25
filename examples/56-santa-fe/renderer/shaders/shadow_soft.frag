$input v_color0
$input v_texcoord0

// =============================================================================
// shadow_soft.frag -- the frame's layers, at the trace pass's resolution (santa-fe-reference S22; S11's blur is
// gone: the shadows are filtered at the window's resolution, shadow_atrous.sc).
//
// The frame is no longer composited here: each part goes out apart, so the reconstruction can upscale each
// by its own setting and resolve.frag composites them at the window's resolution. Only the clouds are put on
// here (each layer through what they let through, their light in the colour), before the reconstruction:
//   0  the colour without the sun (the sky light, the bounce, the air), a: the sky light's share (the AO's)
//   1  the sun's light, unshadowed (the far field's low shadow in it), a: the leaf glow's extra visibility --
//      the light the leaves round this one found, past its own (sfLeafSpread)
//   2  reflect.frag's light, a: the sun's visibility (hard: its own shadow ray)
//   3  x the shadow's blocker distance, y the depth, zw the motion (the resolve's reprojection)
//   4-6 the G-buffer the reconstruction reads (normal, face, rotation): the trace pass's, or a cloud box's
//      where the blocky style's clouds are in front (then 0 is the whole composite, a -1)
//
// Inputs: the trace pass (colour 0, gPos 1, gNormal 2 -- w the blocker distance --, gFace 3, gRot 4, gMotion 5,
// gSun 6: rgb the sun's light, a its visibility; gGloss 7), the clouds (clouds 8, cloudDepth 9), reflect.frag's
// light 10.
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
SAMPLER2D(clouds, 8);       // clouds.frag: rgb their light, a what they let through (7 is gGloss: reflect.frag's)
SAMPLER2D(cloudDepth, 9);   // x their front
SAMPLER2D(reflection, 10);  // reflect.frag: the glints' and the sheen's reflected light, weighted (S21)

// The clouds over a texel, where they are in front of what it sees (S22: here again, before the reconstruction --
// composited per window pixel after the history, a silhouette's pixel flipped between cloud and none as the
// jitter moved it on and off the voxel, and flickered).
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

uniform vec4 sunDir;
uniform vec4 mtnSun;     // rgb the sun's light
uniform vec4 sfLeafSpread;   // x: how far the leaf glow's light spreads through a crown (m), y: how much, z: most texels
// gSun.a: the visibility, + 2 on leaves.
float knownVis(float a) { return a >= 1.5 ? a - 2.0 : a; }
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
    vec3 refl = texelFetch(reflection, at, 0).rgb;
    vec4 sun = texelFetch(gSun, at, 0);
    vec4 p0 = texelFetch(gPos, at, 0);
    vec4 nrm = texelFetch(gNormal, at, 0);
    vec4 face = texelFetch(gFace, at, 0);
    vec4 rot = texelFetch(gRot, at, 0);
    vec4 mot = texelFetch(gMotion, at, 0);
    // The far field's share of the shadow (clouds.frag, SF2_FAR_SHADOW_LOW): on the sun's light (the leaf glow
    // rides in it: a crown in a ridge's shadow does not glow).
    if ((uint(sfFlags.y + 0.5) & 256u) != 0u) sun.rgb *= farVisAt(at);
    float vis = knownVis(sun.a);
    // The leaf glow's light through a crown: what the leaves round this one found, past this texel's own.
    vec3 lit = sunLight(at, sun, vis);
    float sunLum = dot(sun.rgb, vec3(0.2126, 0.7152, 0.0722));
    float extra = sunLum > 1e-6 ? max(dot(lit, vec3(0.2126, 0.7152, 0.0722)) / sunLum - vis, 0.0) : 0.0;
    vec4 L0 = vec4(colour, mot.z);
    vec4 L1 = vec4(sun.rgb, extra);
    vec4 L2 = vec4(refl, vis);
    vec4 L3 = vec4(nrm.w, p0.w, mot.xy);
    // The clouds in front of what this texel sees: every layer through what they let through, their light in
    // the colour (its sky-light share kept to the part that was the sky light's: the AO does not darken clouds).
    {
        vec4 cl = cloudsAt(v_texcoord0);
        float front = texture2D(cloudDepth, v_texcoord0).x;
        if (!(p0.w >= 0.0 && p0.w < front) && cl.a < 1.0) {
            vec3 under = colour * cl.a;
            vec3 c = under + cl.rgb;
            float lumC = dot(c, vec3(0.2126, 0.7152, 0.0722));
            L0 = vec4(c, lumC > 1e-6 ? L0.a * dot(under, vec3(0.2126, 0.7152, 0.0722)) / lumC : 0.0);
            L1.rgb *= cl.a;
            L2.rgb *= cl.a;
        }
    }
    // The blocky style's clouds (SF2_CLOUD_BLOCKY): a box in front is composited here, whole, and marked (a -1):
    // the composite takes it as it is, and the reconstruction gets the box's face.
    vec4 cl;
    vec3 bn;
    vec4 bface;
    if ((uint(sfFlags.y + 0.5) & 1u) != 0u && cloudBox(v_texcoord0, p0.w, cl, bn, bface)) {
        vec3 c = colour + refl + sun.rgb * min(vis + extra, 1.0);
        L0 = vec4(c * cl.a + cl.rgb, -1.0);
        L1 = vec4(0.0, 0.0, 0.0, 0.0);
        L2 = vec4(0.0, 0.0, 0.0, 1.0);
        nrm = vec4(bn, 0.0);
        face = bface;
        rot = vec4(0.0, 0.0, 0.0, 1.0);
    }
    gl_FragData[0] = L0;
    gl_FragData[1] = L1;
    gl_FragData[2] = L2;
    gl_FragData[3] = L3;
    gl_FragData[4] = nrm;
    gl_FragData[5] = face;
    gl_FragData[6] = rot;
}
