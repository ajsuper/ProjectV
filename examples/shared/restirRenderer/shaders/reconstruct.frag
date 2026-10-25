$input v_color0
$input v_texcoord0

// =============================================================================
// reconstruct.frag -- restir's pass 13: examples/shared/voxelRenderer/shaders/reconstruct.frag, copied
// unchanged (a renderer folder's shaders compile from their own directory). restir's G-buffer writes the
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
// The method is 30-renderers' reconstruction renderer (renderers/reconstruction/shaders/
// reconstruct.frag, where every step is argued at length); this is that pass made to work for
// voxels in TURNED chunks, which a physics scene is full of. In short:
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
// Inputs: FBO 3 (the lit frame, slot 0), FBO 1 (the G-buffer, slots 1..6).
// Input slot 0 here is shade's output, the lit frame.
// Output (FBO 4): the frame at output resolution, for taa.frag to accumulate.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(composedHDR, 0);
SAMPLER2D(gNormal,     2);   // xyz face normal
SAMPLER2D(gFace,       5);   // xyz face centre, w = voxel edge
SAMPLER2D(gRot,        6);   // the chunk's rotation, xyzw

uniform vec4 passTargetRes;
uniform vec4 passInputRes[8];
uniform vec4 cameraPos;
uniform vec4 cameraDir;
// z: bypass -- point-sample the render-resolution frame, what the frame is without this pass.
// w: debug -- green where reconstructed geometry claimed the pixel, red where it fell back.
uniform vec4 renderParams;
uniform vec4 jitterParams;   // xy: where this frame's texels were traced, off their centres, in texels

#define FOV 60.0
#define SEARCH_RADIUS 1
#define MAX_FOOTPRINT_PIXELS 8.0

// Exactly the forward map gbuffer.frag's rays used (rayStartDirection), or the coverage test would
// be against a different ray than the one that filled the G-buffer.
vec3 rayDirection(vec2 uv, vec2 res) {
    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float scale = tan(radians(FOV * 0.5));
    vec3 forward = normalize(cameraDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(forward, worldUp));
    vec3 up = normalize(cross(right, forward));
    return normalize(right * (ndc.x * scale * res.x / res.y) + up * (ndc.y * scale) + forward);
}

// As pjv_utils_DDA.sc's rotationFromQuat: columns are the rotated x, y and z axes.
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
    float t = dot(centre - cameraPos.xyz, n) / denom;
    if (t <= 0.0) return 0.0;
    vec3 d = cameraPos.xyz + rayDir * t - centre;
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

float texelTent(vec2 tapCentre, vec2 srcPos) {
    vec2 d = abs(tapCentre - srcPos);
    return max(0.0, 1.0 - d.x) * max(0.0, 1.0 - d.y);
}

void main() {
    vec2 srcRes = passInputRes[0].xy;
    vec2 srcTexel = passInputRes[0].zw;
    if (renderParams.z > 0.5) {
        gl_FragColor = vec4(texture2D(composedHDR, (floor(v_texcoord0 * srcRes) + 0.5) * srcTexel).rgb, 1.0);
        return;
    }

    // In texel units, relative to where the texels were traced: texel i's sample is at i + 0.5 + jitter.
    vec2 srcPos = v_texcoord0 * srcRes - jitterParams.xy;
    vec2 centreTexel = floor(srcPos);
    vec3 rayDir = rayDirection(v_texcoord0, passTargetRes.xy);
    vec3 forward = normalize(cameraDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 camRight = normalize(cross(forward, worldUp));
    vec3 camUp = normalize(cross(camRight, forward));
    float theta = (2.0 * tan(radians(FOV * 0.5))) / passTargetRes.y;   // one output pixel, in radians

    // Three layers, nearest first: depth t, coverage a, colour sum c and its weight w, the voxel's
    // centre v, size s and rotation q, and which of its faces f (a bit each) are counted in a.
    float t1 = 1e30, a1 = 0.0, w1c = 0.0, s1 = 0.0; vec3 c1 = vec3(0.0), v1 = vec3(0.0); vec4 q1 = vec4(0, 0, 0, 1); int f1 = 0;
    float t2 = 1e30, a2 = 0.0, w2c = 0.0, s2 = 0.0; vec3 c2 = vec3(0.0), v2 = vec3(0.0); vec4 q2 = vec4(0, 0, 0, 1); int f2 = 0;
    float t3 = 1e30, a3 = 0.0, w3c = 0.0, s3 = 0.0; vec3 c3 = vec3(0.0), v3 = vec3(0.0); vec4 q3 = vec4(0, 0, 0, 1); int f3 = 0;
    vec3 skySum = vec3(0.0);
    float skyWeight = 0.0;

    for (int j = -SEARCH_RADIUS; j <= SEARCH_RADIUS; j++)
    for (int i = -SEARCH_RADIUS; i <= SEARCH_RADIUS; i++) {
        vec2 tap = centreTexel + vec2(float(i), float(j)) + 0.5;
        vec2 uv = clamp(tap * srcTexel, srcTexel * 0.5, 1.0 - srcTexel * 0.5);
        // gFace's w, the voxel's size, is 0 for the sky: one fetch tells both.
        vec4 face = texture2D(gFace, uv);
        if (face.w <= 0.0) {
            float w = max(texelTent(tap, srcPos), 1e-6);
            skySum += texture2D(composedHDR, uv).rgb * w;
            skyWeight += w;
            continue;
        }
        float t, coverage, size; vec3 centre; vec4 q;
        int faceId;
        if (!voxelSample(uv, face, rayDir, camRight, camUp, theta, t, coverage, centre, size, q, faceId)) continue;
        vec3 colour = texture2D(composedHDR, uv).rgb;
        float cw = max(coverage * texelTent(tap, srcPos), 1e-6);
        int bit = 1 << faceId;

        // A voxel already held takes this texel's colour, and this face's coverage if it is one not
        // counted yet (several texels record the same face; it counts once).
        if (a1 > 0.0 && sameVoxel(centre, size, v1, s1, rotationOf(q1))) {
            c1 += colour * cw; w1c += cw;
            if ((f1 & bit) == 0) { f1 |= bit; a1 = min(a1 + coverage, 1.0); t1 = min(t1, t); }
            continue;
        }
        if (a2 > 0.0 && sameVoxel(centre, size, v2, s2, rotationOf(q2))) {
            c2 += colour * cw; w2c += cw;
            if ((f2 & bit) == 0) { f2 |= bit; a2 = min(a2 + coverage, 1.0); t2 = min(t2, t); }
            continue;
        }
        if (a3 > 0.0 && sameVoxel(centre, size, v3, s3, rotationOf(q3))) {
            c3 += colour * cw; w3c += cw;
            if ((f3 & bit) == 0) { f3 |= bit; a3 = min(a3 + coverage, 1.0); t3 = min(t3, t); }
            continue;
        }
        if (t < t1) {
            t3 = t2; a3 = a2; c3 = c2; w3c = w2c; v3 = v2; s3 = s2; q3 = q2; f3 = f2;
            t2 = t1; a2 = a1; c2 = c1; w2c = w1c; v2 = v1; s2 = s1; q2 = q1; f2 = f1;
            t1 = t; a1 = coverage; c1 = colour * cw; w1c = cw; v1 = centre; s1 = size; q1 = q; f1 = bit;
        } else if (t < t2) {
            t3 = t2; a3 = a2; c3 = c2; w3c = w2c; v3 = v2; s3 = s2; q3 = q2; f3 = f2;
            t2 = t; a2 = coverage; c2 = colour * cw; w2c = cw; v2 = centre; s2 = size; q2 = q; f2 = bit;
        } else if (t < t3) {
            t3 = t; a3 = coverage; c3 = colour * cw; w3c = cw; v3 = centre; s3 = size; q3 = q; f3 = bit;
        }
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

    vec3 background = skyWeight > 0.0 ? skySum / skyWeight : texture2D(composedHDR, srcPos * srcTexel).rgb;
    vec3 col1 = w1c > 0.0 ? c1 / w1c : background;
    vec3 col2 = w2c > 0.0 ? c2 / w2c : background;
    vec3 col3 = w3c > 0.0 ? c3 / w3c : background;
    vec3 colour = w1 * col1 + w2 * col2 + w3 * col3 + wRest * background;

    // Debug: green where voxels claimed the pixel, red where it fell back to filtering, dark blue for
    // the sky (nothing there to rebuild).
    if (renderParams.w > 0.5 && renderParams.w < 1.5)
        colour = a1 == 0.0 && skyWeight > 0.0 ? vec3(0.05, 0.05, 0.3)
                                             : mix(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), 1.0 - wRest);
    gl_FragColor = vec4(colour, 1.0);
}
