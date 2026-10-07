#ifndef PROJV_UTILS_SURFACE_QUADRICS_H
#define PROJV_UTILS_SURFACE_QUADRICS_H

// Surface voxels -- an implicit surface per voxel, with the voxel as its bounding box. PROTOTYPE.
//
// A voxel can carry one to three surface TERMS, combined by intersection (convex edges, corners,
// thin walls) or union (concave edges). Each term exists in two STYLES, fitted separately:
//
//   contour  a plane              f(u) = n.u + d                  the faceted voxel look
//   quadric  a quadric            f(u) = q0 x^2 + ... + q8 z + q9  the smooth, faithful look
//
// u is the offset from the voxel's centre in voxels, so u spans [-0.5, 0.5]^3 inside the box, and f
// grows outward (f <= 0 is inside). The renderer clips the combined surface to the box. Occupancy
// still says where surfaces are and what the traversal visits; the terms say exactly where in the
// box the surface lies.
//
// Two ways to read the combined f, chosen per voxel:
//
//   * one-sided (solid):  f <= 0 is solid. A ray that enters the box already inside hits the box
//                         face. With the interior of a closed mesh filled solid (mesh_voxelizer
//                         does this), a ray that slips through a crack between two voxels' surfaces
//                         lands on solid material one voxel deeper: a dent, never a hole.
//   * two-sided (sheet):  only f = 0 is surface, from either side. For open meshes (foliage, single
//                         planes), which have no inside to be solid.
//
// Storage is a sidecar beside the .data file (model.data -> model.surfaces), keyed by block grid
// coordinate and voxel. At load the entries are quantised and laid out beside the tree (see "GPU
// data" below), which is what pjv_surface.sc reads -- from inside the traversal, so a ray that
// crosses a box without touching its surface keeps stepping instead of starting over.
//
// A voxel without an entry is an ordinary box: the fallback for anything three terms cannot hold.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math.h"

namespace projv {
    struct Scene;
    struct GPUData;
}

namespace projv::utils {

constexpr int SURFACE_QUADRIC_COEFFS = 10;
constexpr int SURFACE_MAX_TERMS = 3;

// Packed voxel coordinate: x | y << 10 | z << 20, so chunks up to 1024^3.
inline uint32_t packSurfaceVoxel(int x, int y, int z) {
    return uint32_t(x) | (uint32_t(y) << 10) | (uint32_t(z) << 20);
}

// Bit 31 of the packed voxel word: this voxel is two-sided (a sheet) rather than a solid.
constexpr uint32_t SURFACE_TWO_SIDED = 0x80000000u;
constexpr uint32_t SURFACE_VOXEL_MASK = 0x3FFFFFFFu;

struct SurfaceTerm {
    float plane[4] = {0, 0, 1, 0};                  // Unit outward normal, then d.
    float quadric[SURFACE_QUADRIC_COEFFS] = {};     // |grad f| ~ 1 near the surface.
};

// How terms combine, as bits. max(f, g) is the intersection of the insides, min(f, g) the union.
//   two terms:   bit 0 = op(t0, t1)
//   three terms: bit 0 = inner op on the two terms that are not the outer one, bit 1 = outer op
//                between that and the outer term, bits 2-3 = index of the outer term.
// So three planes can make a step -- (upper tread AND riser) OR lower tread -- not just a corner.
constexpr uint8_t SURFACE_COMBINE_UNION_INNER = 1u;
constexpr uint8_t SURFACE_COMBINE_UNION_OUTER = 2u;

struct SurfaceVoxel {
    uint32_t voxel = 0;            // packSurfaceVoxel(...) | optional SURFACE_TWO_SIDED
    uint8_t  termCount = 1;
    uint8_t  combine = 0;          // See SURFACE_COMBINE_*.
    SurfaceTerm term[SURFACE_MAX_TERMS];
};

// The combined surface, in the voxel's frame.
double evaluateSurfaceVoxel(const SurfaceVoxel& v, bool planes, const core::vec3& u);
// Turns inside out: every term negated, and every op swapped (-max(a,b) = min(-a,-b)).
void flipSurfaceVoxel(SurfaceVoxel& v);

struct SurfaceBlock {
    core::ivec3 gridCoord{0};   // Same as the DataBlock's gridX/Y/Z.
    std::vector<SurfaceVoxel> voxels;
};

struct SurfaceFile {
    uint32_t resolution = 0;    // Chunk resolution the voxel coordinates are in.
    std::vector<SurfaceBlock> blocks;
};

// ---- Fitting -------------------------------------------------------------------------------

// A point on the true surface near the voxel, in the voxel's frame (offset from its centre, in
// voxels), with the outward normal there and a weight (typically area * falloff).
struct SurfaceSample {
    core::vec3 position;
    core::vec3 normal;
    float weight;
};

struct SurfaceFitOptions {
    float offset = 0.3f;          // Distance of the +/- normal constraints, voxels.
    float ridge = 1e-3f;          // Pull on the quadratic terms toward zero (prefers planes).
    float minNormalAgreement = 0.85f;  // Weighted mean cos(fit gradient, sample normal).
    float maxDistanceError = 0.12f;    // Weighted RMS of |f| / |grad f| at the samples, voxels.
    // Sign validation: on a grid through the voxel, the fit must put every point that is clearly
    // off the surface (by more than signTolerance, judged from the nearest sample and its normal)
    // on the same side the samples do. Catches a zero set displaced off an exposed face and a
    // spurious second sheet inside the box.
    bool  validateSign = true;
    float signTolerance = 0.1f;
    int   signGrid = 5;
    // A contour plane is an approximation by design; it is held to a looser test.
    float planeSignTolerance = 0.25f;
    // When the quadric fails, try a plane (no quadratic terms, so no second sheet) before giving up.
    bool  planeFallback = true;
    // When no single surface fits, split the samples by normal into up to this many groups (edges,
    // corners, thin walls) and combine one term per group.
    int   maxTerms = SURFACE_MAX_TERMS;
    float multiTermReach = 0.6f;   // Multi-term fits use samples within this of the centre (box +0.1).
    float multiTermTolerance = 0.1f;   // ...and must pass within this of 95% of them (by weight).
    float offsetGap = 0.15f;       // Samples of one normal this far apart along it are separate faces.
    // When nothing fits within tolerance, keep the best effort (the multi-term combination the most
    // samples lie on, else one plane) instead of leaving a box. A box sticks out as a whole cube; an
    // imperfect surface over a solid interior is at worst a dent. Reported as Approximate.
    bool  allowApproximate = true;
    float approximateCoverage = 0.5f;   // Minimum sample coverage for a multi-term best effort.
};

enum class SurfaceFitResult {
    Ok,
    TooFewSamples,
    Singular,
    NormalsDisagree,   // Sharp edge, or two sheets with opposing normals in one voxel.
    PoorFit,
    SignMismatch,      // The zero set strays to the wrong side somewhere in the box.
};

// What a fit settled on, for statistics.
enum class SurfaceFitKind { None, Quadric, Plane, MultiTerm, Approximate };

/**
 * Least-squares quadric through the samples: f = 0 at each sample and f = +/-offset at the sample
 * moved +/-offset along its normal, so the gradient follows the normals and f grows outward. The
 * result is normalised so |grad f| ~ 1 near the surface, making f roughly a distance in voxels.
 */
SurfaceFitResult fitSurfaceQuadric(const std::vector<SurfaceSample>& samples,
                                   const SurfaceFitOptions& options,
                                   float outCoefficients[SURFACE_QUADRIC_COEFFS],
                                   SurfaceFitKind* outKind = nullptr);

/**
 * Everything a voxel needs, both styles: one term when one surface fits, otherwise up to
 * options.maxTerms terms from the samples split by normal and combined by intersection or union.
 * The result is one-sided as fitted (the caller decides sidedness and orientation) -- except a
 * double-sided face (opposite normals on one plane), which comes back as one term already marked
 * SURFACE_TWO_SIDED in out.voxel, for the caller to keep.
 */
SurfaceFitResult fitSurfaceVoxel(const std::vector<SurfaceSample>& samples,
                                 const SurfaceFitOptions& options, SurfaceVoxel& out,
                                 SurfaceFitKind* outKind = nullptr, bool forceMultiTerm = false);

/**
 * Does the voxel's surface claim solid on a face whose neighbour is known to be empty exterior?
 * `exteriorFaces` has a bit per face: +x, -x, +y, -y, +z, -z. Nothing occupies that neighbour, so
 * the true surface cannot reach it; a fit that does extends a face past where it really ends -- a
 * single plane at an edge it does not know about -- and shows as a cut box face (a notch).
 */
bool surfaceVoxelBreachesExterior(const SurfaceVoxel& v, uint8_t exteriorFaces, float tolerance = 0.05f);

// DEBUG: prints why multi-term fits failed, accumulated over every fitSurfaceVoxel call.
void reportMultiTermFailures();

/**
 * Re-expresses a quadric given in a chunk's voxel space as a 4x4 symmetric matrix Q (f = [x 1] Q
 * [x 1]^T, row-major) in the frame of the voxel whose minimum corner is `voxel`, normalised so the
 * linear part at the centre is unit length.
 */
void quadricMatrixToVoxelFrame(const double Q[4][4], const core::ivec3& voxel,
                               float outCoefficients[SURFACE_QUADRIC_COEFFS]);

// ---- Sidecar file --------------------------------------------------------------------------

// model.data -> model.surfaces
std::string surfaceFilePathFor(const std::string& dataFilePath);

bool writeSurfaceFile(const std::string& path, const SurfaceFile& file);
bool readSurfaceFile(const std::string& path, SurfaceFile& file);

// ---- GPU data ------------------------------------------------------------------------------
//
// Addressed through the tree rather than hashed: the traversal already holds the leaf node it is
// standing in and the voxel's index among that leaf's 64 children, which is exactly how material
// bytes are found. So the surface data needs no keys and no empty slots.
//
// One RGBA32U texture holding one SET per style (contour, quadric), each laid out the same way.
// All offsets are in texels:
//
//   A  node table, at nodeBase:  one word per tree64 node (global index, as the GPU stores them),
//                                4 per texel. 0 = no surfaces in this leaf, else leaf record + 1.
//   B  leaf records, at recordBase: one texel each: (entryOffset, curvedMaskHi, curvedMaskLo, flags)
//      Masks use the tree64 child-mask convention (z-order < 32 in the first word, bit 31 - z).
//      flags bit 0: every voxel two-sided.      bit 3: FULL mask present (quadric set)
//      flags bit 4: MULTI mask present.         bit 5: TWO-SIDED mask present (mixed leaf)
//      flags bit 6: OWN SURFACE MASK -- the leaf's entries start with a 2-word mask of which voxels
//                   have a surface, and plane words are ranked by it instead of by occupancy (for
//                   leaves where the interior fill leaves many voxels without one)
//   C  entry words, at entryBase, 4 per texel. Per leaf, from entryOffset:
//        one PLANE word per OCCUPIED voxel in the leaf's own rank order (so the shader ranks with
//          the occupancy mask the march already holds); 0xFFFFFFFF for a voxel with no surface;
//          for a multi-term voxel this is its first term
//        then, each only when flagged: full mask (2), multi mask (2), two-sided mask (2)
//        quadric set: one TANGENT word per curved voxel not in the full mask, then two FULL words
//          per voxel in it, each in rank order (the first term's curvature)
//        then per multi-term voxel, in rank order, a fixed-size block:
//          contour set: (count | combine << 2, plane1, plane2)
//          quadric set: (count | combine << 2, plane1, full1a, full1b, plane2, full2a, full2b)
//          (combine is the 4-bit SURFACE_COMBINE_* field)
//          an absent third term is a 0xFFFFFFFF plane
//
// PLANE word: octahedral unit normal (2 x 10 bits) | offset (12 bits, [-4, 4] voxels).
// TANGENT word: the quadratic part restricted to the tangent plane of that normal -- h_uu, h_uv,
// h_vv in a frame built from the decoded normal -- as three 9-bit mantissas sharing a 5-bit
// exponent. FULL words: all six quadratic coefficients, 10-bit mantissas, 4-bit shared exponent.

struct SurfaceGPUOptions {
    float planeTolerance = 0.01f;    // Voxels. Curvature below this is dropped (quadric set).
    float tangentTolerance = 0.01f;  // Tangent form when within this of the fitted surface.
};

struct SurfaceGPUSet {
    uint32_t nodeBase = 0, recordBase = 0, entryBase = 0;   // Texels.
    uint32_t nodes = 0, leaves = 0, entryWords = 0;
    uint32_t entries = 0, curved = 0, fullCurvature = 0, multiTerm = 0, twoSided = 0, sentinels = 0;
    uint32_t maskedLeaves = 0;
    double   maxError = 0.0;            // Stored vs fitted surface, first terms, voxels.
    uint32_t errorOver[3] = {0, 0, 0};  // Entries whose error exceeds 0.005 / 0.02 / 0.05.
    size_t nodeTableBytes() const { return size_t(nodes) * 4; }
    size_t recordBytes() const    { return size_t(leaves) * 16; }
    size_t entryBytes() const     { return size_t(entryWords) * 4; }
    size_t totalBytes() const     { return nodeTableBytes() + recordBytes() + entryBytes(); }
};

struct SurfaceGPUData {
    std::vector<uint32_t> texels;   // 4 words per texel, row-major.
    uint32_t width = 1, height = 1;
    SurfaceGPUSet contour, quadric;
    uint32_t voxels = 0;            // Surface voxels found (either set).
};

/**
 * Builds both sets for every live blob with surfaces: from a .surfaces sidecar beside the blob's
 * .data, plus `extra`, keyed by chunk handle (for chunks built in memory). Needs the scene already
 * uploaded, because it indexes nodes by where createTexturesForScene placed them. Blobs uploaded
 * at a storage LOD are skipped (their nodes are not the file's nodes).
 */
SurfaceGPUData buildSurfaceGPUData(const Scene& scene, const GPUData& gpuData,
                                   const std::unordered_map<uint32_t, std::vector<SurfaceVoxel>>& extra = {},
                                   const SurfaceGPUOptions& options = {});

} // namespace projv::utils

#endif
