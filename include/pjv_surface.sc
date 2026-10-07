// =============================================================================
// pjv_surface.sc -- voxels as bounding boxes for implicit surfaces. PROTOTYPE.
//
// Include this INSTEAD of pjv_utils_DDA.sc: it switches on the traversal's
// surface hook and then includes the traversal itself. Reads the data built by
// projv::utils::buildSurfaceGPUData (see include/utils/surface_quadrics.h for
// the layout and the frame), bound as `surfaceData` on the stage named by
// PJV_SURFACE_DATA_STAGE (default 0), plus the uniforms
//
//   pjvSurfaceInfo  = (nodeBase, recordBase, entryBase, mode)  of the set in use
//   pjvSurfaceInfo2 = (log2(texture width), 0, 0, 0)
//
// The texture holds two sets: contour (planes) and quadric. The caller points
// pjvSurfaceInfo at the one its mode reads.
//
// Every solid voxel the march lands on is judged inside the march itself:
//
//   no entry   -> the voxel is a box, exactly as the plain traversal draws it
//   entry      -> one to three terms combined by intersection or union, clipped
//                 to the box; a ray that crosses the box without entering the
//                 surface steps on to the next voxel
//
//   PJV_SURFACE_BASELINE  never looks anything up: the plain traversal.
//   PJV_SURFACE_VOXELS    boxes, with the lookup (the same data, as voxels).
//   PJV_SURFACE_CONTOURS  the contour set: planes, the faceted voxel look.
//   PJV_SURFACE_QUADRICS  the quadric set: the smooth, faithful look.
// =============================================================================

#ifndef PJV_SURFACE_SC
#define PJV_SURFACE_SC

#define PJV_SURFACE_HOOK 1
#include <pjv_utils_DDA.sc>

#ifndef PJV_SURFACE_DATA_STAGE
#define PJV_SURFACE_DATA_STAGE 0
#endif

USAMPLER2D(surfaceData, PJV_SURFACE_DATA_STAGE);
uniform vec4 pjvSurfaceInfo;
uniform vec4 pjvSurfaceInfo2;

#define PJV_SURFACE_BASELINE 0
#define PJV_SURFACE_VOXELS   1
#define PJV_SURFACE_CONTOURS 2
#define PJV_SURFACE_QUADRICS 3

// Root search slack past each face, in voxels (single-term sheets). Neighbouring
// fits are independent and do not meet exactly; letting each reach a sliver past
// its box closes the cracks between them.
#ifndef PJV_SURFACE_OVERLAP
#define PJV_SURFACE_OVERLAP 0.03
#endif

// A ray entering a one-sided voxel already inside its surface hits the box face. Within this many
// voxels of the surface that is a crack between neighbouring fits, shaded with the surface's normal
// so it does not read as a notch; deeper, it is a genuine cut and keeps the face's normal.
#ifndef PJV_SURFACE_CRACK_DEPTH
#define PJV_SURFACE_CRACK_DEPTH 0.3
#endif

struct PjvQuadric {
    vec4 a;   // q0..q3: xx yy zz xy
    vec4 b;   // q4..q7: xz yz x y
    vec2 c;   // q8, q9: z 1
};

struct PjvSurfaceVoxel {
    bool found;
    bool twoSided;
    int  count;       // 1..3 terms
    uint combine;     // SURFACE_COMBINE_* bits: inner op, outer op, outer term (surface_quadrics.h)
    PjvQuadric t0;
    PjvQuadric t1;
    PjvQuadric t2;
};

// ---- Quadric arithmetic -------------------------------------------------------------------------

float pjvQuadricEval(PjvQuadric q, vec3 u) {
    return q.a.x * u.x * u.x + q.a.y * u.y * u.y + q.a.z * u.z * u.z
         + q.a.w * u.x * u.y + q.b.x * u.x * u.z + q.b.y * u.y * u.z
         + q.b.z * u.x + q.b.w * u.y + q.c.x * u.z + q.c.y;
}

vec3 pjvQuadricGrad(PjvQuadric q, vec3 u) {
    return vec3(2.0 * q.a.x * u.x + q.a.w * u.y + q.b.x * u.z + q.b.z,
                2.0 * q.a.y * u.y + q.a.w * u.x + q.b.y * u.z + q.b.w,
                2.0 * q.a.z * u.z + q.b.x * u.x + q.b.y * u.y + q.c.x);
}

// The quadratic form alone, on a direction.
float pjvQuadricForm(PjvQuadric q, vec3 d) {
    return q.a.x * d.x * d.x + q.a.y * d.y * d.y + q.a.z * d.z * d.z
         + q.a.w * d.x * d.y + q.b.x * d.x * d.z + q.b.y * d.y * d.z;
}

PjvQuadric pjvZeroQuadric() {
    PjvQuadric q;
    q.a = vec4(0.0, 0.0, 0.0, 0.0);
    q.b = vec4(0.0, 0.0, 0.0, 0.0);
    q.c = vec2(0.0, 0.0);
    return q;
}

// ---- Fetch and decode ---------------------------------------------------------------------------

uvec4 pjvSurfaceTexel(uint index) {
    uint shift = uint(pjvSurfaceInfo2.x + 0.5);
    uint width = 1u << shift;
    return texelFetch(surfaceData, ivec2(int(index & (width - 1u)), int(index >> shift)), 0);
}

uint pjvSurfaceWord(uint word) {
    uvec4 t = pjvSurfaceTexel(word >> 2u);
    uint lane = word & 3u;
    return lane == 0u ? t.x : (lane == 1u ? t.y : (lane == 2u ? t.z : t.w));
}

// Mirrors octDecode in surface_quadrics.cpp.
vec3 pjvOctDecode(vec2 f) {
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

float pjvSignedBits(uint bits) {   // 10-bit two's complement
    return float(int(bits) - (bits >= 512u ? 1024 : 0));
}

float pjvSignedBits9(uint bits) {  // 9-bit two's complement
    return float(int(bits) - (bits >= 256u ? 512 : 0));
}

// A PLANE word: unit normal and offset, as the linear part and constant of a quadric.
PjvQuadric pjvDecodePlane(uint plane) {
    PjvQuadric q = pjvZeroQuadric();
    vec2 oct = vec2(float(plane & 1023u), float((plane >> 10u) & 1023u)) / 1023.0 * 2.0 - 1.0;
    vec3 n = pjvOctDecode(oct);
    q.b.z = n.x;
    q.b.w = n.y;
    q.c = vec2(n.z, float(plane >> 20u) / 4095.0 * 8.0 - 4.0);
    return q;
}

// Two FULL-CURVATURE words: the six quadratic coefficients.
void pjvDecodeFull(uint w0, uint w1, inout PjvQuadric q) {
    float scale = exp2(float(w0 & 15u) - 6.0) / 511.0;
    q.a = vec4(pjvSignedBits((w0 >> 4u) & 1023u),
               pjvSignedBits((w0 >> 14u) & 1023u),
               pjvSignedBits(((w0 >> 24u) & 255u) | ((w1 & 3u) << 8u)),
               pjvSignedBits((w1 >> 2u) & 1023u)) * scale;
    q.b.x = pjvSignedBits((w1 >> 12u) & 1023u) * scale;
    q.b.y = pjvSignedBits((w1 >> 22u) & 1023u) * scale;
}

// One TANGENT-CURVATURE word, about the plane's own normal. Mirrors tangentFrame and
// encodeTangentCurvature in surface_quadrics.cpp.
void pjvDecodeTangent(uint w, inout PjvQuadric q) {
    vec3 n = vec3(q.b.z, q.b.w, q.c.x);
    vec3 a = abs(n.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 t1 = normalize(cross(n, a));
    vec3 t2 = cross(n, t1);
    float scale = exp2(float(w & 31u) - 16.0) / 255.0;
    float huu = pjvSignedBits9((w >> 5u) & 511u) * scale;
    float huv = pjvSignedBits9((w >> 14u) & 511u) * scale;
    float hvv = pjvSignedBits9((w >> 23u) & 511u) * scale;
    // A = huu t1t1 + huv (t1t2 + t2t1) + hvv t2t2, as the six coefficients.
    vec3 dg = huu * t1 * t1 + 2.0 * huv * t1 * t2 + hvv * t2 * t2;
    float axy = huu * t1.x * t1.y + huv * (t1.x * t2.y + t2.x * t1.y) + hvv * t2.x * t2.y;
    float axz = huu * t1.x * t1.z + huv * (t1.x * t2.z + t2.x * t1.z) + hvv * t2.x * t2.z;
    float ayz = huu * t1.y * t1.z + huv * (t1.y * t2.z + t2.y * t1.z) + hvv * t2.y * t2.z;
    q.a = vec4(dg.x, dg.y, dg.z, 2.0 * axy);
    q.b.x = 2.0 * axz;
    q.b.y = 2.0 * ayz;
}

PjvSurfaceVoxel pjvSurfaceFetch(uint leafNode, uint leafMask1, uint leafMask2, uint z, bool quadricSet) {
    PjvSurfaceVoxel v;
    v.found = false;
    v.twoSided = false;
    v.count = 1;
    v.combine = 0u;
    v.t0 = pjvZeroQuadric();
    v.t1 = pjvZeroQuadric();
    v.t2 = pjvZeroQuadric();

    uint record = pjvSurfaceWord(uint(pjvSurfaceInfo.x + 0.5) * 4u + leafNode);
    if (record == 0u) return v;
    // (entryOffset, curvedMaskHi, curvedMaskLo, flags)
    uvec4 r = pjvSurfaceTexel(uint(pjvSurfaceInfo.y + 0.5) + record - 1u);
    uint entryWord = uint(pjvSurfaceInfo.z + 0.5) * 4u + r.x;

    // Plane words are ranked by the leaf's occupancy, or by its own surface mask when it has one.
    uint rankMask1 = leafMask1, rankMask2 = leafMask2;
    if ((r.w & 64u) != 0u) {
        rankMask1 = pjvSurfaceWord(entryWord);
        rankMask2 = pjvSurfaceWord(entryWord + 1u);
        entryWord += 2u;
        if (!checkZOrderInValidMasks(rankMask1, rankMask2, z)) return v;   // No surface: a box.
    }
    uint plane = pjvSurfaceWord(entryWord + calculateSiblingsBeforeThisZOrder(4, rankMask1, rankMask2, z));
    if (plane == 0xFFFFFFFFu) return v;   // This voxel has no surface: a box.
    v.found = true;
    v.t0 = pjvDecodePlane(plane);

    // Optional masks after the leaf's plane words, in a fixed order.
    uint after = entryWord + countbits(rankMask1) + countbits(rankMask2);
    uint full1 = 0u, full2 = 0u, multi1 = 0u, multi2 = 0u, two1 = 0u, two2 = 0u;
    if ((r.w & 8u) != 0u)  { full1 = pjvSurfaceWord(after);  full2 = pjvSurfaceWord(after + 1u);  after += 2u; }
    if ((r.w & 16u) != 0u) { multi1 = pjvSurfaceWord(after); multi2 = pjvSurfaceWord(after + 1u); after += 2u; }
    if ((r.w & 32u) != 0u) { two1 = pjvSurfaceWord(after);   two2 = pjvSurfaceWord(after + 1u);   after += 2u; }
    v.twoSided = (r.w & 1u) != 0u || checkZOrderInValidMasks(two1, two2, z);

    // First term's curvature (quadric set): tangent words, then full ones.
    uint curvedCount = countbits(r.y) + countbits(r.z);
    uint fullCount = countbits(full1) + countbits(full2);
    if (quadricSet && checkZOrderInValidMasks(r.y, r.z, z)) {
        uint curvedRank = calculateSiblingsBeforeThisZOrder(4, r.y, r.z, z);
        uint fullRank = calculateSiblingsBeforeThisZOrder(4, full1, full2, z);
        if (checkZOrderInValidMasks(full1, full2, z)) {
            uint at = after + (curvedCount - fullCount) + 2u * fullRank;
            pjvDecodeFull(pjvSurfaceWord(at), pjvSurfaceWord(at + 1u), v.t0);
        } else {
            pjvDecodeTangent(pjvSurfaceWord(after + curvedRank - fullRank), v.t0);
        }
    }

    // Further terms: a fixed-size block per multi-term voxel, after the curvature words.
    if (checkZOrderInValidMasks(multi1, multi2, z)) {
        uint blockSize = quadricSet ? 7u : 3u;
        uint at = after + (quadricSet ? (curvedCount - fullCount) + 2u * fullCount : 0u)
                + blockSize * calculateSiblingsBeforeThisZOrder(4, multi1, multi2, z);
        uint head = pjvSurfaceWord(at);
        v.count = int(head & 3u);
        v.combine = (head >> 2u) & 15u;
        uint stride = quadricSet ? 3u : 1u;
        v.t1 = pjvDecodePlane(pjvSurfaceWord(at + 1u));
        if (quadricSet) pjvDecodeFull(pjvSurfaceWord(at + 2u), pjvSurfaceWord(at + 3u), v.t1);
        if (v.count > 2) {
            v.t2 = pjvDecodePlane(pjvSurfaceWord(at + 1u + stride));
            if (quadricSet) pjvDecodeFull(pjvSurfaceWord(at + 2u + stride), pjvSurfaceWord(at + 3u + stride), v.t2);
        }
    }
    return v;
}

// ---- Ray tests ----------------------------------------------------------------------------------

// First root of a s^2 + b s + c in [lo, hi], or a value below lo when there is none.
float pjvFirstRoot(float a, float b, float c, float lo, float hi) {
    float none = lo - 1.0;
    float scale = abs(b) * max(abs(hi), 1.0) + abs(c);
    if (abs(a) * max(hi * hi, 1.0) <= 1e-6 * scale) {
        if (abs(b) < 1e-20) return none;
        float s = -c / b;
        return (s >= lo && s <= hi) ? s : none;
    }
    float disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return none;
    float sq = sqrt(disc);
    float qq = -0.5 * (b + (b >= 0.0 ? sq : -sq));
    float r1 = qq / a;
    float r2 = abs(qq) > 1e-30 ? c / qq : r1;
    float near = min(r1, r2);
    float far  = max(r1, r2);
    if (near >= lo && near <= hi) return near;
    if (far  >= lo && far  <= hi) return far;
    return none;
}

// Both roots of a s^2 + b s + c (or the one of a line), as (r1, r2); 1e30 where there is none.
vec2 pjvRoots(float a, float b, float c) {
    float scale = abs(b) + abs(c);
    if (abs(a) <= 1e-6 * max(scale, 1e-20)) {
        return vec2(abs(b) > 1e-20 ? -c / b : 1e30, 1e30);
    }
    float disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return vec2(1e30, 1e30);
    float sq = sqrt(disc);
    float qq = -0.5 * (b + (b >= 0.0 ? sq : -sq));
    return vec2(qq / a, abs(qq) > 1e-30 ? c / qq : qq / a);
}

// The combined surface: max of two values is the intersection of their insides, min the union.
// Two terms: op(t0, t1). Three: outerOp(innerOp(the other two), outer term). Also reports which
// term decides the value at u, whose gradient is the surface normal there.
float pjvCombinedWhich(PjvSurfaceVoxel v, vec3 u, out int which) {
    float f0 = pjvQuadricEval(v.t0, u);
    float f1 = pjvQuadricEval(v.t1, u);
    bool innerUnion = (v.combine & 1u) != 0u;
    if (v.count < 3) {
        bool pickFirst = innerUnion ? f0 <= f1 : f0 >= f1;
        which = pickFirst ? 0 : 1;
        return pickFirst ? f0 : f1;
    }
    float f2 = pjvQuadricEval(v.t2, u);
    int outer = int((v.combine >> 2u) & 3u);
    int ia = outer == 0 ? 1 : 0;
    int ib = outer == 2 ? 1 : 2;
    float fa = ia == 0 ? f0 : f1;
    float fb = ib == 1 ? f1 : f2;
    float fo = outer == 0 ? f0 : (outer == 1 ? f1 : f2);
    bool pickA = innerUnion ? fa <= fb : fa >= fb;
    float inner = pickA ? fa : fb;
    int innerWhich = pickA ? ia : ib;
    bool outerUnion = (v.combine & 2u) != 0u;
    bool pickInner = outerUnion ? inner <= fo : inner >= fo;
    which = pickInner ? innerWhich : outer;
    return pickInner ? inner : fo;
}

float pjvCombined(PjvSurfaceVoxel v, vec3 u) {
    int which;
    return pjvCombinedWhich(v, u, which);
}

vec3 pjvActiveGrad(PjvSurfaceVoxel v, vec3 u) {
    int which;
    pjvCombinedWhich(v, u, which);
    return which == 0 ? pjvQuadricGrad(v.t0, u) : (which == 1 ? pjvQuadricGrad(v.t1, u) : pjvQuadricGrad(v.t2, u));
}

// ---- The hook -----------------------------------------------------------------------------------
//
// Per-query state, reset by pjvSurfaceIntersect. The side of the surface the ray was on as it left
// the last voxel it stepped past, and where: neighbouring fits are independent, so they can leave a
// crack at a shared face that no root search finds -- but the ray still changes sides across it,
// and that is detectable. (Two-sided sheets only; a one-sided solid's cracks land on solid voxels.)
static bool  pjvSurfaceHaveLast = false;
static float pjvSurfaceLastExitT = -1.0;
static float pjvSurfaceLastSide = 0.0;
// What the last stopping verdict was, for the caller's shading and debug views. Holds for the hit
// a single-chunk query returns; with several chunks it is whichever chunk decided last.
static bool  pjvSurfaceHitFitted = false;
static int   pjvSurfaceHitPath = 0;

int pjvSurfaceLeafVerdict(Ray localRay, uint leafNode, uint leafMask1, uint leafMask2, uint voxelZ,
                          ivec3 voxel, float exitT, inout float rayT, inout vec3 normal) {
    int mode = int(pjvSurfaceInfo.w + 0.5);
    pjvSurfaceHitFitted = false;
    pjvSurfaceHitPath = 0;
    if (mode == PJV_SURFACE_BASELINE) return PJV_PEEL_STOP_HERE;

    PjvSurfaceVoxel v = pjvSurfaceFetch(leafNode, leafMask1, leafMask2, voxelZ, mode == PJV_SURFACE_QUADRICS);
    if (!v.found || mode == PJV_SURFACE_VOXELS) {
        pjvSurfaceHaveLast = false;
        return PJV_PEEL_STOP_HERE;   // A plain box.
    }

    float t0 = rayT;
    float L = max(exitT - t0, 0.0);
    vec3 dir = localRay.direction;
    vec3 u0 = localRay.origin + dir * t0 - (vec3(voxel) + 0.5);
    bool hasEntryFace = dot(normal, normal) > 0.5;

    // ---- Several terms: a one-sided solid, the intersection or union of the terms' insides ----
    // The first point of [0, L] inside the combined solid is either the entry itself or a root of
    // one of the terms; test those few candidates in order.
    if (v.count > 1) {
        pjvSurfaceHaveLast = false;
        if (pjvCombined(v, u0) <= 0.0) {
            // Entered already inside: a cut by the box face. When the surface is close by, this is
            // a crack between neighbouring fits rather than a real cut, and the surface's normal
            // hides it; a deep cut (a primitive clipped by its voxels) keeps the face's.
            vec3 g = pjvActiveGrad(v, u0);
            if (-pjvCombined(v, u0) < PJV_SURFACE_CRACK_DEPTH * max(length(g), 1e-20)) {
                normal = normalize(g);
                pjvSurfaceHitFitted = true;
            }
            pjvSurfaceHitPath = 7;
            return PJV_PEEL_STOP_HERE;
        }
        vec2 r0 = pjvRoots(pjvQuadricForm(v.t0, dir), dot(pjvQuadricGrad(v.t0, u0), dir), pjvQuadricEval(v.t0, u0));
        vec2 r1 = pjvRoots(pjvQuadricForm(v.t1, dir), dot(pjvQuadricGrad(v.t1, u0), dir), pjvQuadricEval(v.t1, u0));
        vec2 r2 = v.count > 2
                ? pjvRoots(pjvQuadricForm(v.t2, dir), dot(pjvQuadricGrad(v.t2, u0), dir), pjvQuadricEval(v.t2, u0))
                : vec2(1e30, 1e30);
        float best = 1e30;
        float cand[6];
        cand[0] = r0.x; cand[1] = r0.y; cand[2] = r1.x; cand[3] = r1.y; cand[4] = r2.x; cand[5] = r2.y;
        for (int i = 0; i < 6; ++i) {
            float s = cand[i];
            if (s < 0.0 || s > L || s >= best) continue;
            // Just past the crossing, is the ray inside the combined solid?
            if (pjvCombined(v, u0 + dir * min(s + 1e-3, L)) <= 0.0) best = s;
        }
        if (best <= L) {
            rayT = t0 + best;
            normal = normalize(pjvActiveGrad(v, u0 + dir * min(best + 1e-3, L)));
            pjvSurfaceHitFitted = true;
            pjvSurfaceHitPath = 8;
            return PJV_PEEL_STOP_HERE;
        }
        return PJV_PEEL_CONTINUE;
    }

    // ---- One term ----
    PjvQuadric q = v.t0;
    float c = pjvQuadricEval(q, u0);
    float b = dot(pjvQuadricGrad(q, u0), dir);
    float a = pjvQuadricForm(q, dir);

    bool continuing = pjvSurfaceHaveLast && abs(t0 - pjvSurfaceLastExitT) < 0.01;
    bool crossedInCrack = v.twoSided && continuing && (c > 0.0 ? 1.0 : -1.0) != pjvSurfaceLastSide;

    if (crossedInCrack) {
        normal = normalize(pjvQuadricGrad(q, u0));
        pjvSurfaceHitFitted = true;
        pjvSurfaceHitPath = 5;
        return PJV_PEEL_STOP_HERE;
    }
    if (!v.twoSided && c <= 0.0) {
        // Entered already inside the solid: the box face is the surface (a cut), unless the
        // surface runs along the face -- a rounding miss from the previous voxel, which wants the
        // curved normal rather than the face's.
        vec3 g = pjvQuadricGrad(q, u0);
        if (-c < PJV_SURFACE_CRACK_DEPTH * max(length(g), 1e-20)) {
            normal = normalize(g);
            pjvSurfaceHitFitted = true;
        }
        pjvSurfaceHaveLast = false;
        return PJV_PEEL_STOP_HERE;
    }

    // The overlap reaches back across the entry face, but never behind the ray's own origin: a
    // shadow ray leaving a surface would find that surface again.
    float lo = max(-PJV_SURFACE_OVERLAP, -t0);
    float s = pjvFirstRoot(a, b, c, lo, L + PJV_SURFACE_OVERLAP);
    // Behind the entry face, only a crossing INTO the surface is this voxel's to claim; one coming
    // out is the far side of something the ray never entered.
    if (s >= lo && s < 0.0 && dot(pjvQuadricGrad(q, u0 + dir * s), dir) > 0.0) {
        s = pjvFirstRoot(a, b, c, 0.0, L + PJV_SURFACE_OVERLAP);
        if (s < 0.0) s = lo - 1.0;
    }
    if (s >= lo) {
        s = clamp(s, 0.0, L);
        rayT = t0 + s;
        normal = normalize(pjvQuadricGrad(q, u0 + dir * s));
        pjvSurfaceHitFitted = true;
        pjvSurfaceHitPath = (c > 0.0 ? 1 : 3) + (continuing ? 1 : 0);
        return PJV_PEEL_STOP_HERE;
    }
    if (v.twoSided && c <= 0.0 && !continuing && hasEntryFace) {
        // Came in from empty space already on the inside, and never crosses out: the surface was
        // crossed at (or a hair before) this face, where the fit pokes out of its box into empty
        // space no fit covers. An open sheet seen from behind still crosses inside the box, so it
        // takes the root above; a ray that started inside this voxel has no entry face.
        normal = normalize(pjvQuadricGrad(q, u0));
        pjvSurfaceHitFitted = true;
        pjvSurfaceHitPath = 6;
        return PJV_PEEL_STOP_HERE;
    }

    pjvSurfaceHaveLast = true;
    pjvSurfaceLastExitT = exitT;
    pjvSurfaceLastSide = pjvQuadricEval(q, u0 + dir * L) > 0.0 ? 1.0 : -1.0;
    return PJV_PEEL_CONTINUE;
}

// ---- The query ----------------------------------------------------------------------------------

struct PjvSurfaceHit {
    bool  hit;
    float t;           // World units.
    vec3  normal;      // World space, not necessarily facing the ray.
    vec3  albedo;
    float voxelSize;   // World size of the voxel hit, for offsets.
    bool  fitted;      // The hit came from a surface rather than a plain box face.
    int   path;        // DEBUG: which branch produced the hit.
};

// One scene query: the surfaces are resolved inside the march (the mode is pjvSurfaceInfo.w), so
// a ray that passes through a box without touching its surface costs a DDA step, not a new march.
PjvSurfaceHit pjvSurfaceIntersect(Ray ray, RayQuery rayQuery) {
    PjvSurfaceHit result;
    result.hit = false;
    result.t = -1.0;
    result.normal = vec3(0.0, 1.0, 0.0);
    result.albedo = vec3(0.0, 0.0, 0.0);
    result.voxelSize = 1.0;
    result.fitted = false;
    result.path = 0;

    pjvSurfaceHaveLast = false;
    pjvSurfaceHitFitted = false;
    pjvSurfaceHitPath = 0;

    SceneIntersectData h = raySceneIntersectFrom(ray, rayQuery, 0.0);
    if (h.foundBox.size < 0.0 || h.rayT < 0.0) return result;

    chunkHeader hd = headers(int(h.headerIndex));
    result.hit = true;
    result.t = h.rayT;
    result.normal = dot(h.normal, h.normal) > 0.5 ? normalize(h.normal) : -ray.direction;
    result.albedo = fetchVoxelMaterialFromHit(h).albedo;
    result.voxelSize = hd.scale / float(hd.resolution);
    result.fitted = pjvSurfaceHitFitted;
    result.path = pjvSurfaceHitPath;
    return result;
}

#endif // PJV_SURFACE_SC
