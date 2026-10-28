#ifndef PJV_MATH_SC
#define PJV_MATH_SC

// =============================================================================
// pjv_math.sc -- constants, bases and direction encodings (docs/plans/shader-library.md)
// =============================================================================
//
// Mechanisms only. Nothing here reads a uniform.

#define PJV_PI      3.14159265359
#define PJV_TWO_PI  6.28318530718

// An orthonormal basis around n (the third column), for turning a local-space sample into a world one.
// The tangent is built from +z, or from +y when n is nearly +-z. A sampler's pattern turns with the
// basis, so a renderer that builds its own keeps it.
mat3 pjvBasisAround(vec3 n) {
    vec3 tangent = abs(n.z) < 0.999 ? normalize(cross(vec3(0.0, 0.0, 1.0), n))
                                    : normalize(cross(vec3(0.0, 1.0, 0.0), n));
    return mat3(tangent, cross(n, tangent), n);
}

// Octahedral encoding of a unit vector into [-1, 1]^2 (Cigolle et al. 2014): the sphere folded onto
// the square, the lower hemisphere's corners wrapped over the upper's. A texture storing it in [0, 1]
// maps e * 0.5 + 0.5 on the way in and back on the way out.
vec2 pjvOctWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 pjvOctEncode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : pjvOctWrap(n.xy);
}

vec3 pjvOctDecode(vec2 e) {
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = pjvOctWrap(n.xy);
    return normalize(n);
}

#endif
