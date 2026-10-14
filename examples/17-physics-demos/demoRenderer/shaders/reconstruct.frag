$input v_color0
$input v_texcoord0

// =============================================================================
// reconstruct.frag -- pass 4: the render-resolution frame rebuilt at OUTPUT resolution, voxel by
// voxel, and antialiased in the same stroke.
//
// The method is 30-renderers' reconstruction renderer (renderers/reconstruction/shaders/
// reconstruct.frag, where every step is argued at length); this is that pass made to work for
// voxels in TURNED chunks, which a physics scene is full of. In short:
//
// - A G-buffer texel that saw a voxel knows that whole voxel: its face centre, edge length, normal
//   and -- new here -- the chunk's rotation (gRot) give the cube exactly. So for each output pixel,
//   the voxels its 3x3 neighbourhood of render texels saw are tested against the OUTPUT pixel's own
//   ray.
// - Each is tested for how much of the pixel its silhouette COVERS (the overlap of the pixel's
//   footprint on each visible face with that face, summed over the at most three faces a cube shows).
//   Coverage is the antialiasing: a pixel inside a face is 1 and stays crisp; along an edge it is the
//   true fraction, so edges come out smooth at output resolution even though the source never had
//   them. That holds at any scale, 1:1 included.
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
// Inputs: FBO 3 (the lit frame, slot 0), FBO 1 (the G-buffer, slots 1..6).
// Output (FBO 4): the frame at output resolution.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(composedHDR, 0);
SAMPLER2D(gPos,        1);   // xyz world hit, w = distance (w < 0: sky)
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

// The cube's whole silhouette: the faces turned toward the ray, one per axis, are disjoint, so
// their coverages add.
float voxelCoverage(vec3 centre, float halfSide, mat3 R, vec3 rayDir, vec3 camRight, vec3 camUp,
                    float theta, out float outT) {
    outT = 1e30;
    float total = 0.0;
    for (int axis = 0; axis < 3; axis++) {
        vec3 a = R[axis], u = R[(axis + 1) % 3], v = R[(axis + 2) % 3];
        vec3 n = dot(rayDir, a) > 0.0 ? -a : a;
        float t;
        float c = faceCoverage(centre + n * halfSide, n, u, v, halfSide, rayDir, camRight, camUp, theta, t);
        if (c > 0.0) { total += c; outT = min(outT, t); }
    }
    return min(total, 1.0);
}

// What one source texel says about the voxel it saw.
bool voxelSample(vec2 uv, vec3 rayDir, vec3 camRight, vec3 camUp, float theta,
                 out float outT, out float outCoverage, out vec3 outCentre, out float outRecorded,
                 out float outSize, out vec4 outRotation) {
    outT = 1e30; outCoverage = 0.0; outCentre = vec3(0.0); outRecorded = 0.0; outSize = 0.0;
    outRotation = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 face = texture2D(gFace, uv);
    float halfSide = face.w * 0.5;
    if (halfSide <= 0.0) return false;
    vec3 n = texture2D(gNormal, uv).xyz;
    outRotation = texture2D(gRot, uv);
    mat3 R = rotationOf(outRotation);
    outCentre = face.xyz - n * halfSide;
    outSize = face.w;
    // The recorded face's own axes: the two of the voxel's axes that lie in it.
    vec3 k = abs(n * R);
    int along = k.x >= k.y && k.x >= k.z ? 0 : (k.y >= k.z ? 1 : 2);
    float tRecorded;
    outRecorded = faceCoverage(face.xyz, n, R[(along + 1) % 3], R[(along + 2) % 3], halfSide, rayDir,
                               camRight, camUp, theta, tRecorded);
    outCoverage = voxelCoverage(outCentre, halfSide, R, rayDir, camRight, camUp, theta, outT);
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

    vec2 centreTexel = floor(v_texcoord0 * srcRes);
    vec2 srcPos = v_texcoord0 * srcRes;
    vec3 rayDir = rayDirection(v_texcoord0, passTargetRes.xy);
    vec3 forward = normalize(cameraDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 camRight = normalize(cross(forward, worldUp));
    vec3 camUp = normalize(cross(camRight, forward));
    float theta = (2.0 * tan(radians(FOV * 0.5))) / passTargetRes.y;   // one output pixel, in radians

    // Three layers, nearest first: depth t, coverage a, colour sum c and its weight w, the voxel's
    // centre v, size s and rotation q.
    float t1 = 1e30, a1 = 0.0, w1c = 0.0, s1 = 0.0; vec3 c1 = vec3(0.0), v1 = vec3(0.0); vec4 q1 = vec4(0, 0, 0, 1);
    float t2 = 1e30, a2 = 0.0, w2c = 0.0, s2 = 0.0; vec3 c2 = vec3(0.0), v2 = vec3(0.0); vec4 q2 = vec4(0, 0, 0, 1);
    float t3 = 1e30, a3 = 0.0, w3c = 0.0, s3 = 0.0; vec3 c3 = vec3(0.0), v3 = vec3(0.0); vec4 q3 = vec4(0, 0, 0, 1);
    vec3 skySum = vec3(0.0);
    float skyWeight = 0.0;

    for (int j = -SEARCH_RADIUS; j <= SEARCH_RADIUS; j++)
    for (int i = -SEARCH_RADIUS; i <= SEARCH_RADIUS; i++) {
        vec2 tap = centreTexel + vec2(float(i), float(j)) + 0.5;
        vec2 uv = clamp(tap * srcTexel, srcTexel * 0.5, 1.0 - srcTexel * 0.5);
        if (texture2D(gPos, uv).w < 0.0) {
            float w = max(texelTent(tap, srcPos), 1e-6);
            skySum += texture2D(composedHDR, uv).rgb * w;
            skyWeight += w;
            continue;
        }
        float t, coverage, recorded, size; vec3 centre; vec4 q;
        if (!voxelSample(uv, rayDir, camRight, camUp, theta, t, coverage, centre, recorded, size, q)) continue;
        vec3 colour = texture2D(composedHDR, uv).rgb;
        float cw = max(max(recorded, 1e-4) * texelTent(tap, srcPos), 1e-6);

        // A voxel already held takes this texel's colour, and its better geometry if this has it.
        if (a1 > 0.0 && sameVoxel(centre, size, v1, s1, rotationOf(q1))) {
            c1 += colour * cw; w1c += cw;
            if (coverage > a1) { a1 = coverage; t1 = t; v1 = centre; s1 = size; q1 = q; }
            continue;
        }
        if (a2 > 0.0 && sameVoxel(centre, size, v2, s2, rotationOf(q2))) {
            c2 += colour * cw; w2c += cw;
            if (coverage > a2) { a2 = coverage; t2 = t; v2 = centre; s2 = size; q2 = q; }
            continue;
        }
        if (a3 > 0.0 && sameVoxel(centre, size, v3, s3, rotationOf(q3))) {
            c3 += colour * cw; w3c += cw;
            if (coverage > a3) { a3 = coverage; t3 = t; v3 = centre; s3 = size; q3 = q; }
            continue;
        }
        if (t < t1) {
            t3 = t2; a3 = a2; c3 = c2; w3c = w2c; v3 = v2; s3 = s2; q3 = q2;
            t2 = t1; a2 = a1; c2 = c1; w2c = w1c; v2 = v1; s2 = s1; q2 = q1;
            t1 = t; a1 = coverage; c1 = colour * cw; w1c = cw; v1 = centre; s1 = size; q1 = q;
        } else if (t < t2) {
            t3 = t2; a3 = a2; c3 = c2; w3c = w2c; v3 = v2; s3 = s2; q3 = q2;
            t2 = t; a2 = coverage; c2 = colour * cw; w2c = cw; v2 = centre; s2 = size; q2 = q;
        } else if (t < t3) {
            t3 = t; a3 = coverage; c3 = colour * cw; w3c = cw; v3 = centre; s3 = size; q3 = q;
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

    vec3 background = skyWeight > 0.0 ? skySum / skyWeight : texture2D(composedHDR, v_texcoord0).rgb;
    vec3 col1 = w1c > 0.0 ? c1 / w1c : background;
    vec3 col2 = w2c > 0.0 ? c2 / w2c : background;
    vec3 col3 = w3c > 0.0 ? c3 / w3c : background;
    vec3 colour = w1 * col1 + w2 * col2 + w3 * col3 + wRest * background;

    // Debug: green where voxels claimed the pixel, red where it fell back to filtering, dark blue for
    // the sky (nothing there to rebuild).
    if (renderParams.w > 0.5)
        colour = a1 == 0.0 && skyWeight > 0.0 ? vec3(0.05, 0.05, 0.3)
                                             : mix(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), 1.0 - wRest);
    gl_FragColor = vec4(colour, 1.0);
}
