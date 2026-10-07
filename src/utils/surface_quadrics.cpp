#include "utils/surface_quadrics.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <atomic>
#include <map>

#include "core/log.h"
#include "data_structures/gpuData.h"
#include "data_structures/scene.h"

namespace projv::utils {

namespace {

// ---- Small dense algebra -------------------------------------------------------------------

// Monomials in the coefficient order of the header.
inline void monomials(const core::vec3& u, double out[SURFACE_QUADRIC_COEFFS]) {
    const double x = u.x, y = u.y, z = u.z;
    out[0] = x * x; out[1] = y * y; out[2] = z * z;
    out[3] = x * y; out[4] = x * z; out[5] = y * z;
    out[6] = x;     out[7] = y;     out[8] = z;
    out[9] = 1.0;
}

inline double evaluateQuadric(const double q[SURFACE_QUADRIC_COEFFS], double x, double y, double z) {
    return q[0] * x * x + q[1] * y * y + q[2] * z * z + q[3] * x * y + q[4] * x * z + q[5] * y * z +
           q[6] * x + q[7] * y + q[8] * z + q[9];
}

inline void quadricGradient(const double q[SURFACE_QUADRIC_COEFFS], double x, double y, double z, double g[3]) {
    g[0] = 2.0 * q[0] * x + q[3] * y + q[4] * z + q[6];
    g[1] = 2.0 * q[1] * y + q[3] * x + q[5] * z + q[7];
    g[2] = 2.0 * q[2] * z + q[4] * x + q[5] * y + q[8];
}

inline double evaluate(const double q[SURFACE_QUADRIC_COEFFS], const core::vec3& u) {
    return evaluateQuadric(q, u.x, u.y, u.z);
}

inline void gradient(const double q[SURFACE_QUADRIC_COEFFS], const core::vec3& u, double g[3]) {
    quadricGradient(q, u.x, u.y, u.z, g);
}

// Gaussian elimination with partial pivoting on a small dense system. Returns false if singular.
bool solve(double A[SURFACE_QUADRIC_COEFFS][SURFACE_QUADRIC_COEFFS], double b[SURFACE_QUADRIC_COEFFS],
           double x[SURFACE_QUADRIC_COEFFS]) {
    constexpr int n = SURFACE_QUADRIC_COEFFS;
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int row = col + 1; row < n; ++row)
            if (std::abs(A[row][col]) > std::abs(A[pivot][col])) pivot = row;
        if (std::abs(A[pivot][col]) < 1e-14) return false;
        if (pivot != col) {
            for (int k = 0; k < n; ++k) std::swap(A[col][k], A[pivot][k]);
            std::swap(b[col], b[pivot]);
        }
        for (int row = col + 1; row < n; ++row) {
            const double factor = A[row][col] / A[col][col];
            for (int k = col; k < n; ++k) A[row][k] -= factor * A[col][k];
            b[row] -= factor * b[col];
        }
    }
    for (int row = n - 1; row >= 0; --row) {
        double sum = b[row];
        for (int k = row + 1; k < n; ++k) sum -= A[row][k] * x[k];
        x[row] = sum / A[row][row];
    }
    return true;
}

// ---- Fitting -------------------------------------------------------------------------------

// Which side of the sampled surface `u` is on, from the nearest sample and its normal: +1 outside,
// -1 inside, 0 when it is too close to call (within `tolerance` of the surface).
int sampledSide(const std::vector<SurfaceSample>& samples, const core::vec3& u, float tolerance) {
    float best = 1e30f;
    const SurfaceSample* nearest = nullptr;
    for (const SurfaceSample& s : samples) {
        const core::vec3 d = u - s.position;
        const float distSq = d.x * d.x + d.y * d.y + d.z * d.z;
        if (distSq < best) { best = distSq; nearest = &s; }
    }
    if (!nearest) return 0;
    const core::vec3 d = u - nearest->position;
    const float side = d.x * nearest->normal.x + d.y * nearest->normal.y + d.z * nearest->normal.z;
    if (std::abs(side) < tolerance) return 0;
    return side > 0.0f ? 1 : -1;
}

// Does `f` put every clearly-off-surface grid point in the voxel on the side the samples do?
bool signsAgree(const std::vector<SurfaceSample>& samples, const SurfaceFitOptions& options, float tolerance,
                const std::function<double(const core::vec3&)>& f) {
    if (!options.validateSign) return true;
    const int g = std::max(2, options.signGrid);
    for (int z = 0; z < g; ++z)
        for (int y = 0; y < g; ++y)
            for (int x = 0; x < g; ++x) {
                const core::vec3 u(-0.5f + float(x) / float(g - 1), -0.5f + float(y) / float(g - 1),
                                   -0.5f + float(z) / float(g - 1));
                const int truth = sampledSide(samples, u, tolerance);
                if (truth == 0) continue;
                if ((f(u) > 0.0 ? 1 : -1) != truth) return false;
            }
    return true;
}

SurfaceFitResult fitOnce(const std::vector<SurfaceSample>& samples, const SurfaceFitOptions& options,
                         float ridge, float outCoefficients[SURFACE_QUADRIC_COEFFS]) {
    constexpr int n = SURFACE_QUADRIC_COEFFS;
    if (samples.size() < 3) return SurfaceFitResult::TooFewSamples;

    double A[n][n] = {};
    double b[n] = {};
    double totalWeight = 0.0;

    auto addRow = [&](const core::vec3& u, double target, double weight) {
        double m[n];
        monomials(u, m);
        for (int i = 0; i < n; ++i) {
            b[i] += weight * m[i] * target;
            for (int j = 0; j < n; ++j) A[i][j] += weight * m[i] * m[j];
        }
    };

    const float e = options.offset;
    for (const SurfaceSample& s : samples) {
        if (s.weight <= 0.0f) continue;
        addRow(s.position, 0.0, s.weight);
        addRow(s.position + s.normal * e,  e, s.weight);
        addRow(s.position - s.normal * e, -e, s.weight);
        totalWeight += s.weight;
    }
    if (totalWeight <= 0.0) return SurfaceFitResult::TooFewSamples;

    // The ridge keeps the quadratic terms at zero wherever the samples do not pin them down -- a
    // flat patch says nothing about curvature, and an unconstrained term is free to put a second
    // sheet in the box. A whisker on the rest only conditions the solve.
    for (int i = 0; i < 6; ++i) A[i][i] += ridge * totalWeight;
    for (int i = 6; i < n; ++i) A[i][i] += 1e-9 * totalWeight;

    double q[n];
    if (!solve(A, b, q)) return SurfaceFitResult::Singular;

    // Quality: does the gradient follow the normals, and does the zero set pass through the samples?
    double agreement = 0.0, gradientSum = 0.0, distanceSq = 0.0;
    for (const SurfaceSample& s : samples) {
        if (s.weight <= 0.0f) continue;
        double g[3];
        gradient(q, s.position, g);
        const double gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (gl < 1e-12) continue;
        agreement += s.weight * (g[0] * s.normal.x + g[1] * s.normal.y + g[2] * s.normal.z) / gl;
        gradientSum += s.weight * gl;
        const double d = evaluate(q, s.position) / gl;
        distanceSq += s.weight * d * d;
    }
    agreement /= totalWeight;
    if (gradientSum <= 1e-12 * totalWeight) return SurfaceFitResult::Singular;
    if (agreement < options.minNormalAgreement) return SurfaceFitResult::NormalsDisagree;
    if (std::sqrt(distanceSq / totalWeight) > options.maxDistanceError) return SurfaceFitResult::PoorFit;

    // Normalise so |grad f| ~ 1 at the surface: f then reads as a distance in voxels.
    const double scale = totalWeight / gradientSum;
    for (int i = 0; i < n; ++i) outCoefficients[i] = float(q[i] * scale);
    return SurfaceFitResult::Ok;
}

// A quadric's tangent plane at the voxel centre, as a unit plane. False with no usable gradient.
bool planeFromQuadric(const float q[SURFACE_QUADRIC_COEFFS], float plane[4]) {
    const double l = std::sqrt(double(q[6]) * q[6] + double(q[7]) * q[7] + double(q[8]) * q[8]);
    if (l < 1e-6) return false;
    plane[0] = float(q[6] / l); plane[1] = float(q[7] / l); plane[2] = float(q[8] / l); plane[3] = float(q[9] / l);
    return true;
}

double evaluatePlane(const float p[4], const core::vec3& u) {
    return double(p[0]) * u.x + double(p[1]) * u.y + double(p[2]) * u.z + p[3];
}

// The least-squares plane (a heavy ridge pins the quadratic terms to ~0), unit normal.
bool fitPlaneOnce(const std::vector<SurfaceSample>& samples, const SurfaceFitOptions& options, float plane[4]) {
    float q[SURFACE_QUADRIC_COEFFS];
    SurfaceFitOptions loose = options;
    loose.maxDistanceError = 1e9f;          // A plane is allowed to be approximate; sign tests judge it.
    loose.minNormalAgreement = 0.5f;
    if (fitOnce(samples, loose, 1e6f, q) != SurfaceFitResult::Ok) return false;
    return planeFromQuadric(q, plane);
}

void planeToQuadric(const float plane[4], float q[SURFACE_QUADRIC_COEFFS]) {
    for (int i = 0; i < 6; ++i) q[i] = 0.0f;
    q[6] = plane[0]; q[7] = plane[1]; q[8] = plane[2]; q[9] = plane[3];
}

} // namespace

SurfaceFitResult fitSurfaceQuadric(const std::vector<SurfaceSample>& samples,
                                   const SurfaceFitOptions& options,
                                   float outCoefficients[SURFACE_QUADRIC_COEFFS],
                                   SurfaceFitKind* outKind) {
    if (outKind) *outKind = SurfaceFitKind::None;

    auto agrees = [&](const float q[SURFACE_QUADRIC_COEFFS]) {
        double qd[SURFACE_QUADRIC_COEFFS];
        for (int i = 0; i < SURFACE_QUADRIC_COEFFS; ++i) qd[i] = q[i];
        return signsAgree(samples, options, options.signTolerance,
                          [&](const core::vec3& u) { return evaluate(qd, u); });
    };

    SurfaceFitResult result = fitOnce(samples, options, options.ridge, outCoefficients);
    if (result == SurfaceFitResult::Ok) {
        if (agrees(outCoefficients)) {
            if (outKind) *outKind = SurfaceFitKind::Quadric;
            return result;
        }
        result = SurfaceFitResult::SignMismatch;
    }
    if (!options.planeFallback || result == SurfaceFitResult::TooFewSamples) return result;

    // Retries, cheapest picture of the surface last. A fit that went wrong usually did so because
    // the neighbourhood held a second feature -- the other side of a thin part, a nearby fold -- so
    // first try again on the samples close to the voxel only, as a quadric and then as a plane.
    std::vector<SurfaceSample> inner;
    for (const SurfaceSample& sample : samples) {
        const core::vec3& u = sample.position;
        if (std::max({std::abs(u.x), std::abs(u.y), std::abs(u.z)}) <= 0.6f) inner.push_back(sample);
    }
    float candidate[SURFACE_QUADRIC_COEFFS];
    auto attempt = [&](const std::vector<SurfaceSample>& set, float ridge, SurfaceFitKind kind) {
        if (set.size() < 3 || fitOnce(set, options, ridge, candidate) != SurfaceFitResult::Ok) return false;
        if (kind == SurfaceFitKind::Plane) for (int i = 0; i < 6; ++i) candidate[i] = 0.0f;
        // Validated against every sample, not just the subset it was fitted to.
        if (!agrees(candidate)) return false;
        std::memcpy(outCoefficients, candidate, sizeof(candidate));
        if (outKind) *outKind = kind;
        return true;
    };
    if (attempt(inner, options.ridge, SurfaceFitKind::Quadric)) return SurfaceFitResult::Ok;
    if (attempt(inner, 1e6f, SurfaceFitKind::Plane))            return SurfaceFitResult::Ok;
    if (attempt(samples, 1e6f, SurfaceFitKind::Plane))          return SurfaceFitResult::Ok;
    return result;
}

// DEBUG: why multi-term fits fail, per k. [k-2][reason]: 0 small group, 1 plane fit, 2 planes' sign.
std::atomic<uint32_t> g_multiFail[2][3];
std::atomic<uint32_t> g_multiOk[2];
std::atomic<uint32_t> g_tooManyPlanes;

void reportMultiTermFailures() {
    for (int k = 0; k < 2; ++k)
        core::info("  multi-term, {} planes: ok {}, small group {}, plane fit {}, samples not on it {}", k + 2,
                   g_multiOk[k].load(), g_multiFail[k][0].load(), g_multiFail[k][1].load(), g_multiFail[k][2].load());
    core::info("  multi-term: more than {} planes needed {} time(s)", SURFACE_MAX_TERMS, g_tooManyPlanes.load());
}

bool surfaceVoxelBreachesExterior(const SurfaceVoxel& v, uint8_t exteriorFaces, float tolerance) {
    for (int face = 0; face < 6; ++face) {
        if (!(exteriorFaces & (1u << face))) continue;
        const int axis = face / 2;
        const float side = (face & 1) ? -0.5f : 0.5f;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                core::vec3 u(0.0f);
                u[axis] = side;
                u[(axis + 1) % 3] = -0.45f + 0.45f * i;
                u[(axis + 2) % 3] = -0.45f + 0.45f * j;
                if (evaluateSurfaceVoxel(v, true, u) < -tolerance) return true;
                if (evaluateSurfaceVoxel(v, false, u) < -tolerance) return true;
            }
    }
    return false;
}

SurfaceFitResult fitSurfaceVoxel(const std::vector<SurfaceSample>& samples,
                                 const SurfaceFitOptions& options, SurfaceVoxel& out,
                                 SurfaceFitKind* outKind, bool forceMultiTerm) {
    if (outKind) *outKind = SurfaceFitKind::None;
    out.termCount = 1;
    out.combine = 0;

    // ---- One surface ----
    SurfaceFitKind kind = SurfaceFitKind::None;
    const SurfaceFitResult single = forceMultiTerm ? SurfaceFitResult::NormalsDisagree
                                                   : fitSurfaceQuadric(samples, options, out.term[0].quadric, &kind);
    if (single == SurfaceFitResult::Ok) {
        // The contour plane is fitted on its own rather than read off the quadric: the quadric's
        // tangent plane at the centre is the right plane for a curve only at the centre.
        float plane[4];
        const bool planeOk = fitPlaneOnce(samples, options, plane) &&
            signsAgree(samples, options, options.planeSignTolerance,
                       [&](const core::vec3& u) { return evaluatePlane(plane, u); });
        if (planeOk) std::memcpy(out.term[0].plane, plane, sizeof(plane));
        else if (!planeFromQuadric(out.term[0].quadric, out.term[0].plane)) return SurfaceFitResult::Singular;
        if (outKind) *outKind = kind;
        return SurfaceFitResult::Ok;
    }
    if (single == SurfaceFitResult::TooFewSamples || options.maxTerms < 2) return single;

    // ---- Several surfaces: an edge, a corner, a thin wall ----
    // Features are local: only samples close to the box, so a neighbouring feature half a voxel
    // away does not join a group (or the validation).
    std::vector<SurfaceSample> nearby;
    for (const SurfaceSample& sample : samples) {
        const core::vec3& u = sample.position;
        if (std::max({std::abs(u.x), std::abs(u.y), std::abs(u.z)}) <= options.multiTermReach) nearby.push_back(sample);
    }
    if (nearby.size() < 6) return single;
    const std::vector<SurfaceSample>& local = nearby;
    // Split the samples by normal (k-means on the sphere, seeded far apart), one term per group,
    // and combine: a group lying behind another's plane is a convex feature (intersect), in front
    // of it a concave one (union).
    // Groups by normal (k-means on the sphere, seeded far apart), each split again wherever its
    // samples sit on parallel planes at different offsets (a step, a groove, a seam: one normal,
    // two faces). One plane per group, two or three in all, and every way of combining them is
    // tried; the one the samples lie on wins.
    const double totalWeight = [&] { double w = 0.0; for (const SurfaceSample& s : local) w += s.weight; return w; }();
    SurfaceVoxel bestEffort;
    double bestEffortCoverage = -1.0;
    for (int k = 1; k <= std::min(options.maxTerms, SURFACE_MAX_TERMS); ++k) {
        std::vector<core::vec3> centres;
        {
            size_t first = 0;
            for (size_t i = 1; i < local.size(); ++i) if (local[i].weight > local[first].weight) first = i;
            centres.push_back(local[first].normal);
            while (int(centres.size()) < k) {
                size_t far = 0;
                float worst = 2.0f;
                for (size_t i = 0; i < local.size(); ++i) {
                    float best = -2.0f;
                    for (const core::vec3& c : centres) best = std::max(best, dot(local[i].normal, c));
                    if (best < worst) { worst = best; far = i; }
                }
                centres.push_back(local[far].normal);
            }
        }
        std::vector<int> group(local.size(), 0);
        for (int iteration = 0; iteration < 8; ++iteration) {
            for (size_t i = 0; i < local.size(); ++i) {
                float best = -2.0f;
                for (int c = 0; c < k; ++c) {
                    const float d = dot(local[i].normal, centres[c]);
                    if (d > best) { best = d; group[i] = c; }
                }
            }
            for (int c = 0; c < k; ++c) {
                core::vec3 sum(0.0f);
                for (size_t i = 0; i < local.size(); ++i) if (group[i] == c) sum += local[i].normal * local[i].weight;
                if (length(sum) > 1e-9f) centres[c] = normalize(sum);
            }
        }

        // Split each normal group at offset gaps.
        std::vector<std::vector<SurfaceSample>> parts;
        for (int c = 0; c < k; ++c) {
            std::vector<std::pair<float, const SurfaceSample*>> byOffset;
            for (size_t i = 0; i < local.size(); ++i)
                if (group[i] == c) byOffset.push_back({dot(centres[c], local[i].position), &local[i]});
            if (byOffset.empty()) continue;
            std::sort(byOffset.begin(), byOffset.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            parts.emplace_back();
            for (size_t i = 0; i < byOffset.size(); ++i) {
                if (i > 0 && byOffset[i].first - byOffset[i - 1].first > options.offsetGap) parts.emplace_back();
                parts.back().push_back(*byOffset[i].second);
            }
        }
        const int terms = int(parts.size());
        if (terms > SURFACE_MAX_TERMS) { g_tooManyPlanes++; continue; }
        if (terms < 2) continue;

        SurfaceVoxel candidate;
        candidate.termCount = uint8_t(terms);
        bool usable = true;
        for (int c = 0; c < terms && usable; ++c) {
            if (parts[c].size() < 3) { usable = false; break; }
            SurfaceTerm& term = candidate.term[c];
            if (!fitPlaneOnce(parts[c], options, term.plane)) {
                // A sliver the solver cannot pin down: the group's mean normal through its centroid.
                core::vec3 n(0.0f), centroid(0.0f);
                float w = 0.0f;
                for (const SurfaceSample& sm : parts[c]) { n += sm.normal * sm.weight; centroid += sm.position * sm.weight; w += sm.weight; }
                if (w <= 0.0f || length(n) < 1e-9f) { usable = false; break; }
                n = normalize(n);
                centroid /= w;
                term.plane[0] = n.x; term.plane[1] = n.y; term.plane[2] = n.z; term.plane[3] = -dot(n, centroid);
                planeToQuadric(term.plane, term.quadric);
                continue;
            }
            if (fitOnce(parts[c], options, options.ridge, term.quadric) != SurfaceFitResult::Ok) {
                planeToQuadric(term.plane, term.quadric);
            }
        }
        if (!usable) { g_multiFail[terms - 2][1]++; continue; }

        // A double-sided face: two opposite normals on one plane. Zero thickness, so neither a slab
        // nor a gap -- a single two-sided sheet.
        if (terms == 2 && dot(core::vec3(candidate.term[0].plane[0], candidate.term[0].plane[1], candidate.term[0].plane[2]),
                              core::vec3(candidate.term[1].plane[0], candidate.term[1].plane[1], candidate.term[1].plane[2])) < -0.95f &&
            std::abs(candidate.term[0].plane[3] + candidate.term[1].plane[3]) < 0.05f) {
            out.termCount = 1;
            out.term[0] = candidate.term[0];
            planeToQuadric(out.term[0].plane, out.term[0].quadric);
            out.voxel |= SURFACE_TWO_SIDED;
            if (outKind) *outKind = SurfaceFitKind::MultiTerm;
            g_multiOk[0]++;
            return SurfaceFitResult::Ok;
        }

        // Validation by the samples themselves rather than by nearest-sample sides, which are wrong
        // right at an edge. The combined surface must pass through the samples: a wrong combination
        // cuts a group off or buries it.
        auto coverage = [&](bool planes) {
            double on = 0.0;
            for (const SurfaceSample& sample : local)
                if (std::abs(evaluateSurfaceVoxel(candidate, planes, sample.position)) <= options.multiTermTolerance)
                    on += sample.weight;
            return totalWeight > 0.0 ? on / totalWeight : 0.0;
        };
        const int combinations = terms == 2 ? 2 : 12;
        double best = -1.0;
        uint8_t bestCombine = 0;
        for (int i = 0; i < combinations; ++i) {
            candidate.combine = terms == 2 ? uint8_t(i) : uint8_t((i & 3) | ((i >> 2) << 2));
            const double c = coverage(true);
            if (c > best) { best = c; bestCombine = candidate.combine; }
        }
        candidate.combine = bestCombine;
        if (best < 0.95) {
            g_multiFail[terms - 2][2]++;
            if (best > bestEffortCoverage) {
                bestEffortCoverage = best;
                bestEffort = candidate;
                if (coverage(false) < best) {
                    for (int c = 0; c < terms; ++c) planeToQuadric(candidate.term[c].plane, bestEffort.term[c].quadric);
                }
            }
            continue;
        }
        g_multiOk[terms - 2]++;
        if (coverage(false) < 0.95) {
            for (int c = 0; c < terms; ++c) planeToQuadric(candidate.term[c].plane, candidate.term[c].quadric);
        }
        candidate.voxel = out.voxel;
        out = candidate;
        if (outKind) *outKind = SurfaceFitKind::MultiTerm;
        return SurfaceFitResult::Ok;
    }
    if (options.allowApproximate) {
        if (bestEffortCoverage >= options.approximateCoverage) {
            bestEffort.voxel = out.voxel;
            out = bestEffort;
            if (outKind) *outKind = SurfaceFitKind::Approximate;
            return SurfaceFitResult::Ok;
        }
        float plane[4];
        if (fitPlaneOnce(samples, options, plane)) {
            out.termCount = 1;
            out.combine = 0;
            std::memcpy(out.term[0].plane, plane, sizeof(plane));
            planeToQuadric(plane, out.term[0].quadric);
            if (outKind) *outKind = SurfaceFitKind::Approximate;
            return SurfaceFitResult::Ok;
        }
    }
    return single;
}

double evaluateSurfaceVoxel(const SurfaceVoxel& v, bool planes, const core::vec3& u) {
    double f[SURFACE_MAX_TERMS];
    for (int t = 0; t < v.termCount; ++t) {
        if (planes) {
            f[t] = evaluatePlane(v.term[t].plane, u);
        } else {
            double q[SURFACE_QUADRIC_COEFFS];
            for (int i = 0; i < SURFACE_QUADRIC_COEFFS; ++i) q[i] = v.term[t].quadric[i];
            f[t] = evaluate(q, u);
        }
    }
    auto op = [](bool unite, double a, double b) { return unite ? std::min(a, b) : std::max(a, b); };
    if (v.termCount == 1) return f[0];
    if (v.termCount == 2) return op(v.combine & SURFACE_COMBINE_UNION_INNER, f[0], f[1]);
    const int outer = (v.combine >> 2) & 3;
    const int a = outer == 0 ? 1 : 0, b = outer == 2 ? 1 : 2;
    return op(v.combine & SURFACE_COMBINE_UNION_OUTER,
              op(v.combine & SURFACE_COMBINE_UNION_INNER, f[a], f[b]), f[outer]);
}

void flipSurfaceVoxel(SurfaceVoxel& v) {
    for (int t = 0; t < v.termCount; ++t) {
        for (float& c : v.term[t].plane) c = -c;
        for (float& c : v.term[t].quadric) c = -c;
    }
    v.combine ^= SURFACE_COMBINE_UNION_INNER | SURFACE_COMBINE_UNION_OUTER;
}

void quadricMatrixToVoxelFrame(const double Q[4][4], const core::ivec3& voxel,
                               float out[SURFACE_QUADRIC_COEFFS]) {
    // f(c + u) with c the voxel centre. The quadratic part is unchanged; the linear part picks up
    // 2 A c, and the constant becomes f(c).
    const double c[3] = {voxel.x + 0.5, voxel.y + 0.5, voxel.z + 0.5};
    double linear[3];
    for (int i = 0; i < 3; ++i) {
        linear[i] = 2.0 * Q[i][3];
        for (int j = 0; j < 3; ++j) linear[i] += 2.0 * Q[i][j] * c[j];
    }
    const double h[4] = {c[0], c[1], c[2], 1.0};
    double constant = 0.0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) constant += h[i] * Q[i][j] * h[j];

    double q[SURFACE_QUADRIC_COEFFS] = {
        Q[0][0], Q[1][1], Q[2][2],
        2.0 * Q[0][1], 2.0 * Q[0][2], 2.0 * Q[1][2],
        linear[0], linear[1], linear[2], constant};

    const double gl = std::sqrt(linear[0] * linear[0] + linear[1] * linear[1] + linear[2] * linear[2]);
    const double scale = gl > 1e-20 ? 1.0 / gl : 1.0;
    for (int i = 0; i < SURFACE_QUADRIC_COEFFS; ++i) out[i] = float(q[i] * scale);
}

// ---- Sidecar file --------------------------------------------------------------------------
//
// Little-endian, no compression:
//   "PVSQ" u32 version=2  u32 resolution  u32 blockCount
//   per block: i32 gx gy gz  u32 count
//     per voxel: u32 voxel, u8 termCount, u8 combine, u16 0, then termCount x (f32 plane[4], f32 q[10])

namespace {
constexpr char     kMagic[4] = {'P', 'V', 'S', 'Q'};
constexpr uint32_t kVersion  = 2;
}

std::string surfaceFilePathFor(const std::string& dataFilePath) {
    std::filesystem::path path(dataFilePath);
    path.replace_extension(".surfaces");
    return path.string();
}

bool writeSurfaceFile(const std::string& path, const SurfaceFile& file) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        core::error("writeSurfaceFile: cannot open '{}'", path);
        return false;
    }
    auto put = [&](const void* data, size_t bytes) { out.write(static_cast<const char*>(data), bytes); };
    put(kMagic, 4);
    put(&kVersion, 4);
    put(&file.resolution, 4);
    const uint32_t blockCount = uint32_t(file.blocks.size());
    put(&blockCount, 4);
    for (const SurfaceBlock& block : file.blocks) {
        const int32_t coord[3] = {block.gridCoord.x, block.gridCoord.y, block.gridCoord.z};
        put(coord, sizeof(coord));
        const uint32_t count = uint32_t(block.voxels.size());
        put(&count, 4);
        for (const SurfaceVoxel& v : block.voxels) {
            const uint8_t header[4] = {v.termCount, v.combine, 0, 0};
            put(&v.voxel, 4);
            put(header, 4);
            for (int t = 0; t < v.termCount; ++t) {
                put(v.term[t].plane, sizeof(v.term[t].plane));
                put(v.term[t].quadric, sizeof(v.term[t].quadric));
            }
        }
    }
    return bool(out);
}

bool readSurfaceFile(const std::string& path, SurfaceFile& file) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    auto get = [&](void* data, size_t bytes) { return bool(in.read(static_cast<char*>(data), bytes)); };
    char magic[4];
    uint32_t version = 0, blockCount = 0;
    if (!get(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !get(&version, 4) || version != kVersion) {
        core::error("readSurfaceFile: '{}' is not a version {} surface file -- re-run mesh_voxelizer --surfaces",
                    path, kVersion);
        return false;
    }
    if (!get(&file.resolution, 4) || !get(&blockCount, 4)) return false;
    file.blocks.resize(blockCount);
    for (SurfaceBlock& block : file.blocks) {
        int32_t coord[3];
        uint32_t count = 0;
        if (!get(coord, sizeof(coord)) || !get(&count, 4)) return false;
        block.gridCoord = core::ivec3(coord[0], coord[1], coord[2]);
        block.voxels.resize(count);
        for (SurfaceVoxel& v : block.voxels) {
            uint8_t header[4];
            if (!get(&v.voxel, 4) || !get(header, 4)) return false;
            v.termCount = std::clamp<uint8_t>(header[0], 1, SURFACE_MAX_TERMS);
            v.combine = header[1];
            for (int t = 0; t < v.termCount; ++t) {
                if (!get(v.term[t].plane, sizeof(v.term[t].plane)) ||
                    !get(v.term[t].quadric, sizeof(v.term[t].quadric))) return false;
            }
        }
    }
    return true;
}

// ---- GPU data ------------------------------------------------------------------------------

namespace {

// Child z-order within a node -> offset in child units (two bits per axis, interleaved XYZXYZ).
inline core::ivec3 childOffset(uint32_t z) {
    return core::ivec3(int((z & 1u) | (((z >> 3) & 1u) << 1)),
                       int(((z >> 1) & 1u) | (((z >> 4) & 1u) << 1)),
                       int(((z >> 2) & 1u) | (((z >> 5) & 1u) << 1)));
}

inline bool childSet(uint32_t mask1, uint32_t mask2, uint32_t z) {
    const uint32_t word = z < 32u ? mask1 : mask2;
    return (word & (0x80000000u >> (z & 31u))) != 0u;
}

inline void setChild(uint32_t& mask1, uint32_t& mask2, uint32_t z) {
    (z < 32u ? mask1 : mask2) |= 0x80000000u >> (z & 31u);
}

uint32_t quantise(double value, double lo, double hi, int bits) {
    const double maxCode = double((1u << bits) - 1u);
    const double t = std::clamp((value - lo) / (hi - lo), 0.0, 1.0);
    return uint32_t(std::lround(t * maxCode));
}

double dequantise(uint32_t code, double lo, double hi, int bits) {
    return lo + (hi - lo) * double(code) / double((1u << bits) - 1u);
}

// Octahedral mapping of a unit vector to [-1, 1]^2, and back. Mirrored in pjv_surface.sc.
void octEncode(const double n[3], double& ox, double& oy) {
    const double l1 = std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]);
    double x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0.0) {
        const double sx = x >= 0.0 ? 1.0 : -1.0, sy = y >= 0.0 ? 1.0 : -1.0;
        const double nx = (1.0 - std::abs(y)) * sx, ny = (1.0 - std::abs(x)) * sy;
        x = nx; y = ny;
    }
    ox = x; oy = y;
}

void octDecode(double ox, double oy, double n[3]) {
    n[0] = ox; n[1] = oy; n[2] = 1.0 - std::abs(ox) - std::abs(oy);
    const double t = std::max(-n[2], 0.0);
    n[0] += n[0] >= 0.0 ? -t : t;
    n[1] += n[1] >= 0.0 ? -t : t;
    const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int i = 0; i < 3; ++i) n[i] /= l;
}

constexpr int    OCT_BITS = 10;
constexpr int    OFFSET_BITS = 12;
constexpr double OFFSET_RANGE = 4.0;
constexpr int    CURV_BITS = 10;
constexpr int    CURV_EXP_BIAS = 6;    // value = mantissa / 511 * 2^(exponent - bias)
constexpr int    TCURV_BITS = 9;
constexpr int    TCURV_EXP_BIAS = 16;  // value = mantissa / 255 * 2^(exponent - bias), exponent 5 bits
// Plane words can never be this: the offset code is capped one below its maximum.
constexpr uint32_t BOX_SENTINEL = 0xFFFFFFFFu;

// A unit plane (n, d) as one word; `decoded` gets what the shader will see (linear part + constant).
uint32_t encodePlane(const double n[3], double d, double decoded[SURFACE_QUADRIC_COEFFS]) {
    double ox, oy;
    octEncode(n, ox, oy);
    const uint32_t cx = quantise(ox, -1.0, 1.0, OCT_BITS);
    const uint32_t cy = quantise(oy, -1.0, 1.0, OCT_BITS);
    const uint32_t cd = std::min(quantise(d, -OFFSET_RANGE, OFFSET_RANGE, OFFSET_BITS),
                                 (1u << OFFSET_BITS) - 2u);   // keeps BOX_SENTINEL unreachable
    double nd[3];
    octDecode(dequantise(cx, -1.0, 1.0, OCT_BITS), dequantise(cy, -1.0, 1.0, OCT_BITS), nd);
    for (int i = 0; i < SURFACE_QUADRIC_COEFFS; ++i) decoded[i] = 0.0;
    decoded[6] = nd[0]; decoded[7] = nd[1]; decoded[8] = nd[2];
    decoded[9] = dequantise(cd, -OFFSET_RANGE, OFFSET_RANGE, OFFSET_BITS);
    return cx | (cy << OCT_BITS) | (cd << (2 * OCT_BITS));
}

// The six quadratic coefficients as two words; fills decoded[0..5].
void encodeFullCurvature(const double q[SURFACE_QUADRIC_COEFFS], uint32_t words[2], double decoded[SURFACE_QUADRIC_COEFFS]) {
    double maxAbs = 1e-12;
    for (int i = 0; i < 6; ++i) maxAbs = std::max(maxAbs, std::abs(q[i]));
    const int exponent = std::clamp(int(std::ceil(std::log2(maxAbs))) + CURV_EXP_BIAS, 0, 15);
    const double scale = std::ldexp(1.0, exponent - CURV_EXP_BIAS);
    const int maxMantissa = (1 << (CURV_BITS - 1)) - 1;
    uint32_t m[6];
    for (int i = 0; i < 6; ++i) {
        const int v = std::clamp(int(std::lround(q[i] / scale * maxMantissa)), -maxMantissa, maxMantissa);
        m[i] = uint32_t(v) & ((1u << CURV_BITS) - 1u);
        decoded[i] = double(v) / maxMantissa * scale;
    }
    words[0] = uint32_t(exponent) | (m[0] << 4) | (m[1] << 14) | ((m[2] & 0xFFu) << 24);
    words[1] = ((m[2] >> 8) & 3u) | (m[3] << 2) | (m[4] << 12) | (m[5] << 22);
}

// A frame in the tangent plane of unit normal n. Mirrored in pjv_surface.sc, and built from the
// DECODED normal on both sides so they agree exactly.
void tangentFrame(const double n[3], double t1[3], double t2[3]) {
    const double a[3] = {std::abs(n[0]) < 0.9 ? 1.0 : 0.0, std::abs(n[0]) < 0.9 ? 0.0 : 1.0, 0.0};
    t1[0] = n[1] * a[2] - n[2] * a[1];
    t1[1] = n[2] * a[0] - n[0] * a[2];
    t1[2] = n[0] * a[1] - n[1] * a[0];
    const double l = std::sqrt(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]);
    for (int i = 0; i < 3; ++i) t1[i] /= l;
    t2[0] = n[1] * t1[2] - n[2] * t1[1];
    t2[1] = n[2] * t1[0] - n[0] * t1[2];
    t2[2] = n[0] * t1[1] - n[1] * t1[0];
}

// The quadratic part restricted to the tangent plane: three numbers, one word. `decoded` must
// already hold the decoded plane; its quadratic terms are filled from the word.
uint32_t encodeTangentCurvature(const double q[SURFACE_QUADRIC_COEFFS], double decoded[SURFACE_QUADRIC_COEFFS]) {
    const double A[3][3] = {{q[0], q[3] * 0.5, q[4] * 0.5},
                            {q[3] * 0.5, q[1], q[5] * 0.5},
                            {q[4] * 0.5, q[5] * 0.5, q[2]}};
    const double n[3] = {decoded[6], decoded[7], decoded[8]};
    double t1[3], t2[3];
    tangentFrame(n, t1, t2);
    auto form = [&](const double* x, const double* y) {
        double r = 0.0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r += x[i] * A[i][j] * y[j];
        return r;
    };
    const double h[3] = {form(t1, t1), form(t1, t2), form(t2, t2)};
    const double maxAbs = std::max({std::abs(h[0]), std::abs(h[1]), std::abs(h[2]), 1e-12});
    const int exponent = std::clamp(int(std::ceil(std::log2(maxAbs))) + TCURV_EXP_BIAS, 0, 31);
    const double scale = std::ldexp(1.0, exponent - TCURV_EXP_BIAS);
    const int maxMantissa = (1 << (TCURV_BITS - 1)) - 1;
    uint32_t word = uint32_t(exponent);
    double hd[3];
    for (int i = 0; i < 3; ++i) {
        const int v = std::clamp(int(std::lround(h[i] / scale * maxMantissa)), -maxMantissa, maxMantissa);
        word |= (uint32_t(v) & ((1u << TCURV_BITS) - 1u)) << (5 + TCURV_BITS * i);
        hd[i] = double(v) / maxMantissa * scale;
    }
    double Ad[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            Ad[i][j] = hd[0] * t1[i] * t1[j] + hd[1] * (t1[i] * t2[j] + t2[i] * t1[j]) + hd[2] * t2[i] * t2[j];
    decoded[0] = Ad[0][0]; decoded[1] = Ad[1][1]; decoded[2] = Ad[2][2];
    decoded[3] = 2.0 * Ad[0][1]; decoded[4] = 2.0 * Ad[0][2]; decoded[5] = 2.0 * Ad[1][2];
    return word;
}

// A quadric term in plane-first form: normalised by its linear part at the centre. False when it
// has none (the voxel is then left a box).
bool normaliseQuadric(const float in[SURFACE_QUADRIC_COEFFS], double q[SURFACE_QUADRIC_COEFFS]) {
    const double lin = std::sqrt(double(in[6]) * in[6] + double(in[7]) * in[7] + double(in[8]) * in[8]);
    if (lin < 1e-4) return false;
    for (int i = 0; i < SURFACE_QUADRIC_COEFFS; ++i) q[i] = in[i] / lin;
    return true;
}

// Points on a quadric's zero set inside the voxel's box: grid points pulled onto it by Newton steps.
void sampleVoxelSurface(const double q[SURFACE_QUADRIC_COEFFS], std::vector<core::vec3>& out) {
    for (int k = 0; k < 64; ++k) {
        double p[3] = {-0.375 + 0.25 * (k & 3), -0.375 + 0.25 * ((k >> 2) & 3), -0.375 + 0.25 * ((k >> 4) & 3)};
        double g[3];
        for (int it = 0; it < 4; ++it) {
            const double f = evaluateQuadric(q, p[0], p[1], p[2]);
            quadricGradient(q, p[0], p[1], p[2], g);
            const double gg = g[0] * g[0] + g[1] * g[1] + g[2] * g[2];
            if (gg < 1e-12) break;
            for (int i = 0; i < 3; ++i) p[i] -= f * g[i] / gg;
        }
        if (std::max({std::abs(p[0]), std::abs(p[1]), std::abs(p[2])}) > 0.52) continue;
        quadricGradient(q, p[0], p[1], p[2], g);
        const double gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (gl < 1e-6 || std::abs(evaluateQuadric(q, p[0], p[1], p[2])) > 1e-3 * gl) continue;
        out.push_back(core::vec3(float(p[0]), float(p[1]), float(p[2])));
    }
}

// Distance from the fitted surface to a stored one, at points on the fitted surface.
double surfaceError(const double fitted[SURFACE_QUADRIC_COEFFS], const double stored[SURFACE_QUADRIC_COEFFS]) {
    std::vector<core::vec3> points;
    sampleVoxelSurface(fitted, points);
    double worst = 0.0;
    for (const core::vec3& p : points) {
        double g[3];
        quadricGradient(stored, p.x, p.y, p.z, g);
        const double gl = std::max(std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]), 1e-9);
        worst = std::max(worst, std::abs(evaluateQuadric(stored, p.x, p.y, p.z)) / gl);
    }
    return worst;
}

struct Leaf {
    uint32_t node;                              // Local node index in the blob.
    uint32_t mask1, mask2;                      // The leaf's own occupancy.
    std::vector<std::pair<uint32_t, const SurfaceVoxel*>> voxels;   // (z-order, its surface)
};

struct BlobLeaves {
    uint32_t geomTexelOffset;
    std::vector<Leaf> leaves;
};

// A term in plane-first form for one style. False when the quadric has no usable linear part.
bool termCoefficients(const SurfaceTerm& term, bool contour, double q[SURFACE_QUADRIC_COEFFS]) {
    if (contour) {
        for (int i = 0; i < 6; ++i) q[i] = 0.0;
        for (int i = 0; i < 4; ++i) q[6 + i] = term.plane[i];
        return true;
    }
    return normaliseQuadric(term.quadric, q);
}

// One style's set: node table, records, entry words. Records' entry offsets are relative to the
// set's own entry region.
void buildSet(bool contour, const std::vector<BlobLeaves>& blobs, uint32_t totalNodes,
              const SurfaceGPUOptions& options, SurfaceGPUSet& set,
              std::vector<uint32_t>& nodeTable, std::vector<uint32_t>& records, std::vector<uint32_t>& words) {
    nodeTable.assign(totalNodes, 0u);
    set.nodes = totalNodes;

    for (const BlobLeaves& blob : blobs) {
        for (const Leaf& leaf : blob.leaves) {
            const uint32_t entryOffset = uint32_t(words.size());
            std::map<uint32_t, const SurfaceVoxel*> byZ(leaf.voxels.begin(), leaf.voxels.end());
            uint32_t curv1 = 0, curv2 = 0, full1 = 0, full2 = 0, multi1 = 0, multi2 = 0, two1 = 0, two2 = 0;
            uint32_t twoCount = 0, surfaceCount = 0;
            std::vector<uint32_t> tangentWords, fullWords, multiWords;

            // Plane words are indexed by the leaf's occupancy -- unless enough of its voxels have no
            // surface (interior fill shares leaves with the shell) that a mask of its own is cheaper
            // than a sentinel each. Decided per leaf: two words of mask against one word per sentinel.
            uint32_t surf1 = 0, surf2 = 0, sentinelsHere = 0;
            for (uint32_t z = 0; z < 64; ++z) {
                if (!childSet(leaf.mask1, leaf.mask2, z)) continue;
                auto found = byZ.find(z);
                double probe[SURFACE_QUADRIC_COEFFS];
                if (found != byZ.end() && termCoefficients(found->second->term[0], contour, probe)) setChild(surf1, surf2, z);
                else sentinelsHere++;
            }
            const bool ownMask = sentinelsHere > 2;
            if (ownMask) { words.push_back(surf1); words.push_back(surf2); }

            for (uint32_t z = 0; z < 64; ++z) {
                if (!childSet(leaf.mask1, leaf.mask2, z)) continue;
                auto found = byZ.find(z);
                double first[SURFACE_QUADRIC_COEFFS], decoded[SURFACE_QUADRIC_COEFFS];
                if (found == byZ.end() || !termCoefficients(found->second->term[0], contour, first)) {
                    if (!ownMask) { words.push_back(BOX_SENTINEL); set.sentinels++; }
                    continue;
                }
                const SurfaceVoxel& v = *found->second;

                // The first term's plane word (and, for quadrics, its curvature).
                const double n[3] = {first[6], first[7], first[8]};
                words.push_back(encodePlane(n, first[9], decoded));
                surfaceCount++;
                set.entries++;

                double entryError;
                double reach = 0.0;
                for (int i = 0; i < 6; ++i) reach += 0.25 * std::abs(first[i]);
                if (!contour && reach > options.planeTolerance) {
                    setChild(curv1, curv2, z);
                    set.curved++;
                    double tangentDecoded[SURFACE_QUADRIC_COEFFS];
                    std::copy(decoded, decoded + SURFACE_QUADRIC_COEFFS, tangentDecoded);
                    const uint32_t tangentWord = encodeTangentCurvature(first, tangentDecoded);
                    const double tangentError = surfaceError(first, tangentDecoded);
                    if (tangentError <= options.tangentTolerance) {
                        tangentWords.push_back(tangentWord);
                        entryError = tangentError;
                    } else {
                        uint32_t fw[2];
                        encodeFullCurvature(first, fw, decoded);
                        setChild(full1, full2, z);
                        fullWords.push_back(fw[0]);
                        fullWords.push_back(fw[1]);
                        set.fullCurvature++;
                        entryError = surfaceError(first, decoded);
                    }
                } else {
                    entryError = surfaceError(first, decoded);
                }
                set.maxError = std::max(set.maxError, entryError);
                if (entryError > 0.005) set.errorOver[0]++;
                if (entryError > 0.02)  set.errorOver[1]++;
                if (entryError > 0.05)  set.errorOver[2]++;

                if (v.voxel & SURFACE_TWO_SIDED) { setChild(two1, two2, z); twoCount++; set.twoSided++; }

                // Further terms: a fixed-size block per multi-term voxel.
                if (v.termCount > 1) {
                    setChild(multi1, multi2, z);
                    set.multiTerm++;
                    multiWords.push_back(uint32_t(v.termCount) | (uint32_t(v.combine) << 2));
                    for (int t = 1; t < 3; ++t) {
                        double q[SURFACE_QUADRIC_COEFFS], dq[SURFACE_QUADRIC_COEFFS];
                        const bool present = t < v.termCount && termCoefficients(v.term[t], contour, q);
                        if (!present) {
                            multiWords.push_back(BOX_SENTINEL);
                            if (!contour) { multiWords.push_back(0u); multiWords.push_back(0u); }
                            continue;
                        }
                        const double tn[3] = {q[6], q[7], q[8]};
                        multiWords.push_back(encodePlane(tn, q[9], dq));
                        if (!contour) {
                            uint32_t fw[2];
                            encodeFullCurvature(q, fw, dq);
                            multiWords.push_back(fw[0]);
                            multiWords.push_back(fw[1]);
                        }
                    }
                }
            }
            if (surfaceCount == 0) {
                words.resize(entryOffset);   // Nothing but sentinels: leave the leaf plain.
                continue;
            }
            uint32_t flags = 0;
            if (ownMask) { flags |= 64u; set.maskedLeaves++; }
            if (twoCount == surfaceCount) flags |= 1u;
            if (full1 | full2)   { flags |= 8u;  words.push_back(full1);  words.push_back(full2); }
            if (multi1 | multi2) { flags |= 16u; words.push_back(multi1); words.push_back(multi2); }
            if (twoCount != 0 && twoCount != surfaceCount) { flags |= 32u; words.push_back(two1); words.push_back(two2); }
            words.insert(words.end(), tangentWords.begin(), tangentWords.end());
            words.insert(words.end(), fullWords.begin(), fullWords.end());
            words.insert(words.end(), multiWords.begin(), multiWords.end());
            nodeTable[blob.geomTexelOffset + leaf.node] = uint32_t(records.size() / 4) + 1u;
            records.insert(records.end(), {entryOffset, curv1, curv2, flags});
            set.leaves++;
        }
    }
    set.entryWords = uint32_t(words.size());
}

} // namespace

SurfaceGPUData buildSurfaceGPUData(const Scene& scene, const GPUData& gpuData,
                                   const std::unordered_map<uint32_t, std::vector<SurfaceVoxel>>& extra,
                                   const SurfaceGPUOptions& options) {
    SurfaceGPUData out;

    // Which surfaces each blob carries. A blob shared by several chunks is filled once.
    std::map<std::string, SurfaceFile> files;
    std::unordered_map<int32_t, std::vector<const SurfaceVoxel*>> byBlob;
    std::unordered_map<int32_t, bool> blobDone;
    std::unordered_map<int32_t, uint32_t> blobResolution;
    for (size_t handle = 0; handle < scene.chunks.size(); ++handle) {
        const Chunk& chunk = scene.chunks[handle];
        const int32_t b = chunk.geometryPoolIndex;
        if (!chunk.alive || b < 0 || size_t(b) >= scene.geometryPool.size() || blobDone[b]) continue;
        blobDone[b] = true;
        blobResolution[b] = chunk.header.resolution;
        auto& list = byBlob[b];
        const GeometryBlob& blob = scene.geometryPool[b];
        if (!blob.sourceDataPath.empty()) {
            auto it = files.find(blob.sourceDataPath);
            if (it == files.end()) {
                SurfaceFile file;
                const std::string sidecar = surfaceFilePathFor(blob.sourceDataPath);
                if (readSurfaceFile(sidecar, file)) core::info("Surface voxels: read '{}'", sidecar);
                it = files.emplace(blob.sourceDataPath, std::move(file)).first;
            }
            for (const SurfaceBlock& block : it->second.blocks) {
                if (block.gridCoord != blob.sourceBlockCoord) continue;
                for (const SurfaceVoxel& v : block.voxels) list.push_back(&v);
            }
        }
        auto extraIt = extra.find(uint32_t(handle));
        if (extraIt != extra.end()) for (const SurfaceVoxel& v : extraIt->second) list.push_back(&v);
    }

    // The node tables must cover every node index the GPU can produce.
    uint32_t totalNodes = 0;
    for (const GPUBlobRange& r : gpuData.blobRanges) {
        if (!r.uploaded) continue;
        totalNodes = std::max({totalNodes, r.geomTexelOffset + r.geomTexelAllocated,
                               r.envTexelOffset + r.envTexelAllocated});
    }

    // Walk each blob's tree to its leaves, pairing surface voxels with (leaf, z-order).
    std::vector<BlobLeaves> blobs;
    for (auto& [b, list] : byBlob) {
        if (list.empty() || size_t(b) >= gpuData.blobRanges.size()) continue;
        const GPUBlobRange& range = gpuData.blobRanges[b];
        const GeometryBlob& blob = scene.geometryPool[b];
        if (!range.uploaded || range.uploadedLOD != 0) {
            core::warn("Surface voxels: blob {} is uploaded at storage LOD {}; its surfaces are skipped.",
                       b, range.uploadedLOD);
            continue;
        }
        const std::vector<uint32_t>& g = blob.geometry;
        const uint32_t resolution = blobResolution[b];
        if (g.size() < 3 || resolution < 4) continue;

        std::unordered_map<uint32_t, const SurfaceVoxel*> byVoxel;
        for (const SurfaceVoxel* v : list) byVoxel.emplace(v->voxel & SURFACE_VOXEL_MASK, v);
        out.voxels += uint32_t(byVoxel.size());

        BlobLeaves leaves{range.geomTexelOffset, {}};
        struct Item { uint32_t node; core::ivec3 origin; uint32_t size; };
        std::vector<Item> stack{{0u, core::ivec3(0), resolution}};
        while (!stack.empty()) {
            const Item it = stack.back();
            stack.pop_back();
            const uint32_t m1 = g[it.node * 3], m2 = g[it.node * 3 + 1], d3 = g[it.node * 3 + 2];
            const uint32_t childSize = it.size / 4;
            if (d3 & 1u) {
                Leaf leaf{it.node, m1, m2, {}};
                for (uint32_t z = 0; z < 64; ++z) {
                    if (!childSet(m1, m2, z)) continue;
                    const core::ivec3 v = it.origin + childOffset(z);
                    auto found = byVoxel.find(packSurfaceVoxel(v.x, v.y, v.z));
                    if (found != byVoxel.end()) leaf.voxels.push_back({z, found->second});
                }
                if (!leaf.voxels.empty()) leaves.leaves.push_back(std::move(leaf));
                continue;
            }
            uint32_t rank = 0;
            for (uint32_t z = 0; z < 64; ++z) {
                if (!childSet(m1, m2, z)) continue;
                stack.push_back({it.node + (d3 >> 1) + rank, it.origin + childOffset(z) * int(childSize), childSize});
                ++rank;
            }
        }
        blobs.push_back(std::move(leaves));
    }

    // Both sets, then one texture: [contour nodes][records][entries][quadric nodes][records][entries].
    std::vector<uint32_t> nodesC, recordsC, wordsC, nodesQ, recordsQ, wordsQ;
    buildSet(true, blobs, totalNodes, options, out.contour, nodesC, recordsC, wordsC);
    buildSet(false, blobs, totalNodes, options, out.quadric, nodesQ, recordsQ, wordsQ);

    auto texelsOf = [](size_t words) { return uint32_t((words + 3) / 4); };
    uint32_t cursor = 0;
    out.contour.nodeBase = cursor;   cursor += texelsOf(nodesC.size());
    out.contour.recordBase = cursor; cursor += texelsOf(recordsC.size());
    out.contour.entryBase = cursor;  cursor += texelsOf(wordsC.size());
    out.quadric.nodeBase = cursor;   cursor += texelsOf(nodesQ.size());
    out.quadric.recordBase = cursor; cursor += texelsOf(recordsQ.size());
    out.quadric.entryBase = cursor;  cursor += texelsOf(wordsQ.size());

    const uint64_t totalTexels = std::max<uint64_t>(1, cursor);
    out.width = 1;
    while (out.width < 4096 && uint64_t(out.width) * out.width < totalTexels) out.width <<= 1;
    out.height = uint32_t((totalTexels + out.width - 1) / out.width);
    out.texels.assign(size_t(out.width) * out.height * 4, 0u);
    auto place = [&](const std::vector<uint32_t>& src, uint32_t texel) {
        std::copy(src.begin(), src.end(), out.texels.begin() + size_t(texel) * 4);
    };
    place(nodesC, out.contour.nodeBase);
    place(recordsC, out.contour.recordBase);
    place(wordsC, out.contour.entryBase);
    place(nodesQ, out.quadric.nodeBase);
    place(recordsQ, out.quadric.recordBase);
    place(wordsQ, out.quadric.entryBase);

    const double mb = 1.0 / (1024.0 * 1024.0);
    for (int s = 0; s < 2; ++s) {
        const SurfaceGPUSet& set = s == 0 ? out.contour : out.quadric;
        core::info("Surface voxels [{}]: {} entries ({} multi-term, {} two-sided, {} curved, {} full) in {} leaves, "
                   "{} box sentinels; {:.3f} MB = nodes {:.3f} + records {:.3f} + entries {:.3f}; "
                   "stored error max {:.4f}, >0.005/0.02/0.05: {}/{}/{}",
                   s == 0 ? "contour" : "quadric", set.entries, set.multiTerm, set.twoSided, set.curved,
                   set.fullCurvature, set.leaves, set.sentinels, set.totalBytes() * mb,
                   set.nodeTableBytes() * mb, set.recordBytes() * mb, set.entryBytes() * mb,
                   set.maxError, set.errorOver[0], set.errorOver[1], set.errorOver[2]);
    }
    return out;
}

} // namespace projv::utils
