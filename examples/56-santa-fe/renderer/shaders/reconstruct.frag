$input v_color0
$input v_texcoord0

// =============================================================================
// reconstruct.frag -- the Santa Fe renderer's upscaler (santa-fe-reference S6), restir-pt's pass 13 copied:
// the trace pass's half (or other) resolution frame rebuilt at window resolution voxel by voxel. Below, restir-pt's notes.
//
// restir's pass 13: examples/shared/voxelRenderer/shaders/reconstruct.frag, copied
// (a renderer folder's shaders compile from their own directory), with one change: what fills the part
// of a pixel no voxel layer claimed (see `background` below). restir's G-buffer writes the
// slots it reads (gNormal 2, gFace 5, gRot 6), and ao_apply's lit frame is its slot 0.
//
// The render-resolution frame rebuilt at OUTPUT resolution, voxel by voxel, and
// antialiased in the same stroke. Taken unchanged from the physics demos' renderer
// (the physics example's demoRenderer, since retired), which is where the turned-chunk support below was added.
//
// It is bound by its texture fetches, not its maths: a fast path that skipped the coverage work for
// pixels whose whole 3x3 window recorded one face measured SLOWER (0.44 -> 0.50 ms), because its own
// nine face reads cost more than the arithmetic they saved.
//
// The method is the one 30-renderers' reconstruction renderer argued step by step at length (its
// renderers/reconstruction/shaders/reconstruct.frag, removed from the gallery 2026-11-01 and kept in
// git history); this is that pass made to work for voxels in TURNED chunks, which a physics scene is
// full of. In short:
//
// - A G-buffer texel that saw a voxel knows that whole voxel: its face centre, edge length, normal
//   and -- new here -- the chunk's rotation (gRot) give the cube exactly. So for each output pixel,
//   the voxels its 3x3 neighbourhood of render texels saw are tested against the OUTPUT pixel's own
//   ray.
// - Each is tested for how much of the pixel it COVERS: the overlap of the pixel's footprint with each
//   face of the voxel that some texel actually recorded, summed. Coverage is the antialiasing: a pixel
//   inside a face is 1 and stays crisp; along an edge it is the true fraction, so edges come out
//   smooth at output resolution even though the source never had them. That holds at any scale, 1:1
//   included.
// - RECORDED faces only, not the cube's whole silhouette (which the gallery version sums). A face a
//   texel saw is at least partly visible; one no texel saw is usually buried -- against the next voxel
//   of the same surface. Counting buried faces is what left colour edges hard: at the boundary
//   between two coplanar voxels of different colours, each claimed the band of the pixel its buried
//   side would cover, so whichever sorted first took nearly the whole pixel and the edge came out
//   binary. With recorded faces the two tops split the pixel by area, as they should. (A face too thin
//   for any texel to see is left to the fallback filter, which is the cheaper failure.)
// - Up to three voxels, nearest first, are composited by coverage -- partitioning the pixel where
//   they are adjacent, occluding where one is a silhouette over another -- over the sky (when the
//   window saw any) or a plain bilinear of the source for whatever no voxel claimed.
// - A voxel's colour is the mean of every texel that saw it, weighted by how much of the pixel the
//   face each one recorded covers, times a bilinear tent on its distance -- smooth shading within a
//   surface, never blended across two.
//
// What the turned chunks change: the cube's axes are the rotation's columns rather than the world's,
// so the visible faces, each face's in-plane axes, and the "is this the same voxel" test are all taken
// in the voxel's own frame. With world axes a 45-degree crate's neighbouring voxels read as one, and
// its faces' footprints were measured along the wrong edges.
//
// The G-buffer is jittered (jitterParams): texel i was traced through i + 0.5 + jitter, and the colour
// weights are measured from there. The voxels themselves are where they are, whichever ray found them.
//
// Santa Fe (S22): the frame comes as four layers (shadow_soft.frag), rebuilt with one set of weights -- each
// texel's share of the pixel is worked out once and every layer is summed through it -- or, by each layer's
// setting (sfUpscale), with the spatial filter instead: the bilinear tent, held to texels at the pixel's depth.
// Inputs: shadow_soft's layers 0-3 and G-buffer 4-6. Outputs: the four layers at the window's resolution, the
// front voxel's face (centre, edge; 0 where none), and the normal and depth (-1 the sky).
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_camera.sc>      // the engine camera: pjvView*, pjvPrevView*
#include <pjv_filter.sc>     // pjvTexelTent

// shadow_soft's outputs: the layers (0-3) and the G-buffer -- the trace pass's, or a cloud box's (blocky style).
SAMPLER2D(composedHDR, 0);
SAMPLER2D(sunLayer,    1);
SAMPLER2D(reflVis,     2);
SAMPLER2D(aux,         3);
SAMPLER2D(gNormal,     4);   // xyz face normal
SAMPLER2D(gFace,       5);   // xyz face centre, w = voxel edge (0: the sky or the far field)
SAMPLER2D(gRot,        6);   // the chunk's rotation, xyzw

#include <pjv_engine.sc>   // passTargetRes, passInputRes: engine-set
// z: bypass -- point-sample the render-resolution frame, what the frame is without this pass.
// w: debug -- green where reconstructed geometry claimed the pixel, red where it fell back.
uniform vec4 sfRecon;        // z: bypass, w: debug (renderParams in restir-pt)
uniform vec4 sfUpscale;      // each layer's upscale: x the colour, y the sun, z reflections, w the sun's visibility; 0 reconstruct, 1 spatial, 2 temporal (reconstructed here)
uniform vec4 jitterParams;   // xy: where this frame's texels were traced, off their centres, in texels

#define SEARCH_RADIUS 1
#define MAX_FOOTPRINT_PIXELS 8.0

// Exactly the forward map gbuffer.frag's rays used (pjvPerspectiveRay), or the coverage test would
// be against a different ray than the one that filled the G-buffer.
vec3 rayDirection(vec2 uv, vec2 res) {
    return pjvViewRayDirection(uv, res);
}

// As pjv_scene.sc's pjvRotationFromQuat: columns are the rotated x, y and z axes.
mat3 rotationOf(vec4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    return mat3(vec3(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z), 2.0 * (x * z - w * y)),
                vec3(2.0 * (x * y - w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + w * x)),
                vec3(2.0 * (x * z + w * y), 2.0 * (y * z - w * x), 1.0 - 2.0 * (x * x + y * y)));
}

// One voxel, or two at different levels of one surface: containment, in the voxel's own frame.
bool sameVoxel(vec3 centreA, float sizeA, vec3 centreB, float sizeB, mat3 R) {
    vec3 offset = abs((centreA - centreB) * R);   // row-vector product: transpose(R) * d
    return all(lessThanEqual(offset, vec3(0.5 * max(sizeA, sizeB) + 0.25 * min(sizeA, sizeB))));
}

// The overlap of a slab of half-width `halfWidth`, centred `offset` from a footprint of `width`, as
// a fraction of the footprint.
float slabOverlap(float offset, float halfWidth, float width) {
    float lo = max(offset - halfWidth, -0.5 * width);
    float hi = min(offset + halfWidth, 0.5 * width);
    return max(hi - lo, 0.0) / width;
}

// How much of the output pixel a square face covers: the pixel's footprint on the face's plane (the
// exact Jacobian of the ray/plane solve), as a box along the face's own axes u and v, overlapped with
// the face.
float faceCoverage(vec3 centre, vec3 n, vec3 u, vec3 v, float halfSide, vec3 rayDir,
                   vec3 camRight, vec3 camUp, float theta, out float outT) {
    outT = 1e30;
    float denom = dot(rayDir, n);
    if (abs(denom) < 1e-6) return 0.0;
    float t = dot(centre - pjvViewPosition(), n) / denom;
    if (t <= 0.0) return 0.0;
    vec3 d = pjvViewPosition() + rayDir * t - centre;
    vec3 fx = t * theta * (camRight - rayDir * (dot(camRight, n) / denom));
    vec3 fy = t * theta * (camUp - rayDir * (dot(camUp, n) / denom));
    float cap = MAX_FOOTPRINT_PIXELS * t * theta;
    float wu = clamp(abs(dot(fx, u)) + abs(dot(fy, u)), 1e-6, cap);
    float wv = clamp(abs(dot(fx, v)) + abs(dot(fy, v)), 1e-6, cap);
    outT = t;
    return slabOverlap(abs(dot(d, u)), halfSide, wu) * slabOverlap(abs(dot(d, v)), halfSide, wv);
}

// What one source texel says about the voxel it saw: the voxel (centre, size, rotation), and the
// face it recorded -- which one (an id per axis and side), how much of this output pixel it covers,
// and at what depth.
bool voxelSample(vec2 uv, vec4 face, vec3 rayDir, vec3 camRight, vec3 camUp, float theta,
                 out float outT, out float outCoverage, out vec3 outCentre, out float outSize,
                 out vec4 outRotation, out int outFace) {
    outT = 1e30; outCoverage = 0.0; outCentre = vec3(0.0); outSize = 0.0; outFace = 0;
    outRotation = vec4(0.0, 0.0, 0.0, 1.0);
    float halfSide = face.w * 0.5;
    if (halfSide <= 0.0) return false;
    vec3 n = texture2D(gNormal, uv).xyz;
    outRotation = texture2D(gRot, uv);
    mat3 R = rotationOf(outRotation);
    outCentre = face.xyz - n * halfSide;
    outSize = face.w;
    // The face's axis among the voxel's own, its side, and the two axes that lie in it.
    vec3 k = n * R;
    vec3 ak = abs(k);
    int along = ak.x >= ak.y && ak.x >= ak.z ? 0 : (ak.y >= ak.z ? 1 : 2);
    float side = along == 0 ? k.x : (along == 1 ? k.y : k.z);
    outFace = along * 2 + (side > 0.0 ? 1 : 0);
    outCoverage = faceCoverage(face.xyz, n, R[(along + 1) % 3], R[(along + 2) % 3], halfSide, rayDir,
                               camRight, camUp, theta, outT);
    return outCoverage > 0.0;
}

// The layers (shadow_soft.frag): 0 the colour without the sun, 1 the sun's light, 2 reflections and the sun's
// visibility, 3 the blocker distance, depth and motion.
vec4 layer0(vec2 uv) { return texture2D(composedHDR, uv); }
vec4 layer1(vec2 uv) { return texture2D(sunLayer, uv); }
vec4 layer2(vec2 uv) { return texture2D(reflVis, uv); }
vec4 layer3(vec2 uv) { return texture2D(aux, uv); }

void main() {
    vec2 srcRes = passInputRes[0].xy;
    vec2 srcTexel = passInputRes[0].zw;
    vec3 rayDir = rayDirection(v_texcoord0, passTargetRes.xy);
    if (sfRecon.z > 0.5) {
        vec2 uv = (floor(v_texcoord0 * srcRes) + 0.5) * srcTexel;
        vec4 f = texture2D(gFace, uv);
        vec3 n = texture2D(gNormal, uv).xyz;
        float d = layer3(uv).y;
        gl_FragData[0] = layer0(uv); gl_FragData[1] = layer1(uv); gl_FragData[2] = layer2(uv); gl_FragData[3] = layer3(uv);
        gl_FragData[4] = f.w > 0.0 ? f : vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[5] = vec4(n, d);
        return;
    }

    // In texel units, relative to where the texels were traced: texel i's sample is at i + 0.5 + jitter.
    vec2 srcPos = v_texcoord0 * srcRes - jitterParams.xy;
    vec2 centreTexel = floor(srcPos);
    PjvCameraBasis basis = pjvCameraBasis(pjvViewDirection());
    vec3 camRight = basis.right, camUp = basis.up;
    float theta = (2.0 * pjvViewTanHalfFov()) / passTargetRes.y;   // one output pixel, in radians

    // ---- 1. The voxels the window saw: three layers, nearest first -- depth t, coverage a, the weight w of the
    // texels that saw it, the voxel's centre v, size s and rotation q, and which of its faces f are counted in a.
    float t1 = 1e30, a1 = 0.0, w1c = 0.0, s1 = 0.0; vec3 v1 = vec3(0.0); vec4 q1 = vec4(0, 0, 0, 1); int f1 = 0;
    float t2 = 1e30, a2 = 0.0, w2c = 0.0, s2 = 0.0; vec3 v2 = vec3(0.0); vec4 q2 = vec4(0, 0, 0, 1); int f2 = 0;
    float t3 = 1e30, a3 = 0.0, w3c = 0.0, s3 = 0.0; vec3 v3 = vec3(0.0); vec4 q3 = vec4(0, 0, 0, 1); int f3 = 0;
    float skyWeight = 0.0;
    float geometryWeight = 0.0;   // every texel that saw a voxel, whichever voxel: the window's own mix
    float tentWeight = 0.0;
    // What the first loop learnt of each tap, so the second only reads the layers (S26: it measured 1.65 ms
    // when it worked every voxel out again): the tap's coverage weight, which layer it joined (each layer keeps
    // the id it was made with as the sorting moves it: 0 none), and whether it saw a voxel at all.
    float tapCw[9];
    int tapSlot[9];
    bool tapGeo[9];
    int id1 = 0, id2 = 0, id3 = 0, nextId = 1;
    for (int j = -SEARCH_RADIUS; j <= SEARCH_RADIUS; j++)
    for (int i = -SEARCH_RADIUS; i <= SEARCH_RADIUS; i++) {
        int k = (j + 1) * 3 + (i + 1);
        tapCw[k] = 0.0; tapSlot[k] = 0; tapGeo[k] = false;
        vec2 tap = centreTexel + vec2(float(i), float(j)) + 0.5;
        vec2 uv = clamp(tap * srcTexel, srcTexel * 0.5, 1.0 - srcTexel * 0.5);
        vec4 face = texture2D(gFace, uv);   // w, the voxel's size, is 0 for the sky (and the far field)
        float tent = pjvTexelTent(tap, srcPos);
        float tw = max(tent, 1e-6);
        tentWeight += tent;
        if (face.w <= 0.0) { skyWeight += tw; continue; }
        tapGeo[k] = true;
        geometryWeight += tw;
        float t, coverage, size; vec3 centre; vec4 q;
        int faceId;
        if (!voxelSample(uv, face, rayDir, camRight, camUp, theta, t, coverage, centre, size, q, faceId)) continue;
        float cw = max(coverage * tent, 1e-6);
        tapCw[k] = cw;
        int bit = 1 << faceId;
        if (a1 > 0.0 && sameVoxel(centre, size, v1, s1, rotationOf(q1))) {
            w1c += cw; tapSlot[k] = id1;
            if ((f1 & bit) == 0) { f1 |= bit; a1 = min(a1 + coverage, 1.0); t1 = min(t1, t); }
            continue;
        }
        if (a2 > 0.0 && sameVoxel(centre, size, v2, s2, rotationOf(q2))) {
            w2c += cw; tapSlot[k] = id2;
            if ((f2 & bit) == 0) { f2 |= bit; a2 = min(a2 + coverage, 1.0); t2 = min(t2, t); }
            continue;
        }
        if (a3 > 0.0 && sameVoxel(centre, size, v3, s3, rotationOf(q3))) {
            w3c += cw; tapSlot[k] = id3;
            if ((f3 & bit) == 0) { f3 |= bit; a3 = min(a3 + coverage, 1.0); t3 = min(t3, t); }
            continue;
        }
        if (t < t3) { tapSlot[k] = nextId; }
        if (t < t1) {
            t3 = t2; a3 = a2; w3c = w2c; v3 = v2; s3 = s2; q3 = q2; f3 = f2; id3 = id2;
            t2 = t1; a2 = a1; w2c = w1c; v2 = v1; s2 = s1; q2 = q1; f2 = f1; id2 = id1;
            t1 = t; a1 = coverage; w1c = cw; v1 = centre; s1 = size; q1 = q; f1 = bit; id1 = nextId;
        } else if (t < t2) {
            t3 = t2; a3 = a2; w3c = w2c; v3 = v2; s3 = s2; q3 = q2; f3 = f2; id3 = id2;
            t2 = t; a2 = coverage; w2c = cw; v2 = centre; s2 = size; q2 = q; f2 = bit; id2 = nextId;
        } else if (t < t3) {
            t3 = t; a3 = coverage; w3c = cw; v3 = centre; s3 = size; q3 = q; f3 = bit; id3 = nextId;
        }
        nextId++;
    }

    // Layers within about a voxel of the front one share the pixel with it; further back, the front
    // one is a silhouette over them.
    float adjacency = max(max(s1, min(t1, 1e6) * theta), 1e-6);
    float part2 = 1.0 - smoothstep(0.25 * adjacency, 1.5 * adjacency, abs(t2 - t1));
    float part3 = 1.0 - smoothstep(0.25 * adjacency, 1.5 * adjacency, abs(t3 - t1));
    float w1 = a1;
    float rem1 = 1.0 - w1;
    float w2 = mix(rem1 * a2, min(a2, rem1), part2);
    float rem2 = max(0.0, rem1 - w2);
    float w3 = mix(rem2 * a3, min(a3, rem2), part3);
    float wRest = max(0.0, 1.0 - w1 - w2 - w3);
    // A layer no texel's colour reached gives its share to the background.
    float wBg = wRest + (w1c > 0.0 ? 0.0 : w1) + (w2c > 0.0 ? 0.0 : w2) + (w3c > 0.0 ? 0.0 : w3);

    // What fills the part of the pixel no layer claimed. At a silhouette it is the sky -- the layers claim
    // as much of the pixel as the window's texels saw geometry. In foliage it is not: leaves smaller than a
    // pixel, more of them than three layers, each claiming only its sliver, while nearly every texel saw a
    // leaf. So the remainder takes the geometry's mix for as much of the window's geometry as the layers left
    // unclaimed, and the sky for the rest; with no sky in the window, the bilinear of the texels.
    float bgSky = 0.0, bgGeo = 0.0, bgTent = 1.0 / max(tentWeight, 1e-6);
    bool bgMix = skyWeight > 0.0;
    if (bgMix) {
        float seen = geometryWeight / (geometryWeight + skyWeight);
        float claimed = 1.0 - wRest;
        float u = wRest > 1e-4 ? clamp((seen - claimed) / wRest, 0.0, 1.0) : 0.0;
        if (geometryWeight > 0.0) { bgSky = (1.0 - u) / skyWeight; bgGeo = u / geometryWeight; }
        else bgSky = 1.0 / skyWeight;
    }

    // ---- 2. Each texel's share of the pixel, and every layer through it: the reconstruction (the voxels' own
    // weights, the same for every layer) and the spatial filter (the bilinear tent, held to texels at the
    // pixel's depth), each layer by its setting (sfUpscale).
    float dRef = -1.0;
    vec3 nRef = vec3(0.0, 1.0, 0.0);
    bool frontNormal = false;
    float nearestTent = -1.0;
    vec4 r0 = vec4(0.0), r1 = vec4(0.0), r2 = vec4(0.0), r3 = vec4(0.0);
    vec4 s0 = vec4(0.0), sp1 = vec4(0.0), sp2 = vec4(0.0), sp3 = vec4(0.0);
    float sw = 0.0;
    float gsum = 0.0;
    // Each layer's share per unit of a tap's weight (0: the layer is empty or no texel's colour reached it).
    float k1 = a1 > 0.0 && w1c > 0.0 ? w1 / w1c : 0.0;
    float k2 = a2 > 0.0 && w2c > 0.0 ? w2 / w2c : 0.0;
    float k3 = a3 > 0.0 && w3c > 0.0 ? w3 / w3c : 0.0;
    for (int j = -SEARCH_RADIUS; j <= SEARCH_RADIUS; j++)
    for (int i = -SEARCH_RADIUS; i <= SEARCH_RADIUS; i++) {
        int k = (j + 1) * 3 + (i + 1);
        vec2 tap = centreTexel + vec2(float(i), float(j)) + 0.5;
        vec2 uv = clamp(tap * srcTexel, srcTexel * 0.5, 1.0 - srcTexel * 0.5);
        float tent = pjvTexelTent(tap, srcPos);
        float tw = max(tent, 1e-6);
        float g = bgMix ? (tapGeo[k] ? bgGeo : bgSky) * tw : tent * bgTent;
        g *= wBg;
        vec4 c0 = layer0(uv), c1 = layer1(uv), c2 = layer2(uv), c3 = layer3(uv);
        if (tent > nearestTent) {   // the depth and normal where no voxel claims the pixel: the nearest texel's
            nearestTent = tent;
            if (!frontNormal) { dRef = c3.y; nRef = texture2D(gNormal, uv).xyz; }
        }
        // Each share at most the layer's whole (a texel the first loop gave next to nothing, or counted under
        // another layer, must not divide by a sum it is not in: that blew up into bright squares).
        int slot = tapSlot[k];
        float cw = tapCw[k];
        if (slot != 0 && slot == id1 && k1 > 0.0) {
            g += min(cw * k1, w1);
            if (!frontNormal) { frontNormal = true; nRef = texture2D(gNormal, uv).xyz; }
        } else if (slot != 0 && slot == id2) g += min(cw * k2, w2);
        else if (slot != 0 && slot == id3) g += min(cw * k3, w3);
        gsum += g;
        r0 += c0 * g; r1 += c1 * g; r2 += c2 * g; r3 += c3 * g;
        if (tent > 0.0) {
            // Spatial: the tent, held to texels at about the pixel's depth (the sky with the sky).
            float d = c3.y;
            float ref = a1 > 0.0 ? t1 : -1.0;
            float wd = 1.0;
            if (ref > 0.0) wd = d < 0.0 ? 1e-3 : exp(-pow((d - ref) / (0.02 * ref + s1), 2.0));
            float ws = tent * max(wd, 1e-3);
            s0 += c0 * ws; sp1 += c1 * ws; sp2 += c2 * ws; sp3 += c3 * ws;
            sw += ws;
        }
    }
    // The shares sum to one when both loops agree on every texel; where they do not (two voxels at an edge that
    // each claim a texel), normalised: an edge reconstructs a little over, never into a hole or a flare.
    if (gsum > 1e-6) { r0 /= gsum; r1 /= gsum; r2 /= gsum; r3 /= gsum; }
    else { r0 = s0; r1 = sp1; r2 = sp2; r3 = sp3; if (sw > 0.0) { r0 /= sw; r1 /= sw; r2 /= sw; r3 /= sw; } }
    if (sw > 0.0) { s0 /= sw; sp1 /= sw; sp2 /= sw; sp3 /= sw; }
    else { s0 = r0; sp1 = r1; sp2 = r2; sp3 = r3; }
    if (sfUpscale.x > 0.5 && sfUpscale.x < 1.5) r0 = s0;
    if (sfUpscale.y > 0.5 && sfUpscale.y < 1.5) r1 = sp1;
    if (sfUpscale.z > 0.5 && sfUpscale.z < 1.5) r2.rgb = sp2.rgb;
    if (sfUpscale.w > 0.5 && sfUpscale.w < 1.5) r2.a = sp2.a;
    r3.zw = sp3.zw;   // the motion: smooth
    if (a1 > 0.0) dRef = t1;
    // Debug: green where voxels claimed the pixel, red where it fell back to filtering, dark blue for
    // the sky (nothing there to rebuild).
    if (sfRecon.w > 0.5 && sfRecon.w < 1.5)
        r0.rgb = a1 == 0.0 && skyWeight > 0.0 ? vec3(0.05, 0.05, 0.3)
                                              : mix(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), 1.0 - wRest);
    if (any(isnan(r0)) || any(isinf(r0))) r0 = vec4(0.0, 0.0, 0.0, 0.0);
    if (any(isnan(r1)) || any(isinf(r1))) r1 = vec4(0.0, 0.0, 0.0, 0.0);
    if (any(isnan(r2)) || any(isinf(r2))) r2 = vec4(0.0, 0.0, 0.0, 1.0);
    if (any(isnan(r3)) || any(isinf(r3))) r3 = vec4(0.0, dRef, 0.0, 0.0);
    gl_FragData[0] = r0;
    gl_FragData[1] = r1;
    gl_FragData[2] = r2;
    gl_FragData[3] = r3;
    // The pixel's front voxel face (centre, edge) and its normal and depth: the window-resolution G-buffer the
    // shadow filter, the AO and the resolve read.
    gl_FragData[4] = a1 > 0.0 ? vec4(v1 + nRef * (0.5 * s1), s1) : vec4(0.0, 0.0, 0.0, 0.0);
    gl_FragData[5] = vec4(nRef, dRef);
}
