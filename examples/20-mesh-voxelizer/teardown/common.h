#ifndef PROJECTV_TEARDOWN_COMMON_H
#define PROJECTV_TEARDOWN_COMMON_H
// Shared types: transforms, logging, and the few conventions every other file relies on.
//
// Units. Teardown positions are metres; sizes and voxel coordinates are voxels of 0.1 m. Everything
// in this converter keeps that split: a Transform is metres, a shape's voxels are integers in its
// own frame, and `voxelSize` (0.1 times a vox `scale`) joins the two.
//
// Axes. Teardown is right-handed and Y-up, and so is ProjectV, so world coordinates pass through
// unchanged. MagicaVoxel is Z-up; vox_file.h converts it on the way in, so no other file sees a
// MagicaVoxel axis.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace teardown {

using glm::ivec3;
using glm::vec3;
using glm::dvec3;
using glm::quat;
using glm::mat3;

constexpr float TD_VOXEL = 0.1f; // metres per Teardown voxel

struct Transform {
    vec3 pos{0.0f};
    quat rot{1.0f, 0.0f, 0.0f, 0.0f};

    vec3 apply(const vec3& p) const { return pos + rot * p; }
    Transform operator*(const Transform& child) const {
        return Transform{pos + rot * child.pos, glm::normalize(rot * child.rot)};
    }
    Transform inverse() const {
        quat inv = glm::conjugate(rot);
        return Transform{inv * -pos, inv};
    }
};

// Teardown's QuatEuler: degrees, the "NASA standard aeroplane" order -- heading about Y, then
// attitude about Z, then bank about X. Every `rot` attribute in a level file means this.
inline quat quatEuler(float xDeg, float yDeg, float zDeg) {
    const float h = glm::radians(yDeg) * 0.5f, a = glm::radians(zDeg) * 0.5f, b = glm::radians(xDeg) * 0.5f;
    const float c1 = std::cos(h), s1 = std::sin(h);
    const float c2 = std::cos(a), s2 = std::sin(a);
    const float c3 = std::cos(b), s3 = std::sin(b);
    quat q;
    q.w = c1 * c2 * c3 - s1 * s2 * s3;
    q.x = s1 * s2 * c3 + c1 * c2 * s3;
    q.y = s1 * c2 * c3 + c1 * s2 * s3;
    q.z = c1 * s2 * c3 - s1 * c2 * s3;
    return glm::normalize(q);
}

// ---- Logging --------------------------------------------------------------------------------
// Progress goes to stdout directly rather than through core::info, so a release build
// (PROJV_LOG_MINIMAL, which compiles info out) still reports what it converted.

extern int g_verbosity; // 0 quiet, 1 normal, 2 verbose

template <typename... Args>
void logInfo(const char* fmt, Args&&... args) {
    if (g_verbosity < 1) return;
    std::fprintf(stdout, fmt, std::forward<Args>(args)...);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
template <typename... Args>
void logVerbose(const char* fmt, Args&&... args) {
    if (g_verbosity < 2) return;
    std::fprintf(stdout, fmt, std::forward<Args>(args)...);
    std::fputc('\n', stdout);
}
template <typename... Args>
void logWarn(const char* fmt, Args&&... args) {
    std::fprintf(stderr, "warning: ");
    std::fprintf(stderr, fmt, std::forward<Args>(args)...);
    std::fputc('\n', stderr);
}

inline int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
inline int floorMod(int a, int b) { int m = a % b; return m < 0 ? m + b : m; }

} // namespace teardown

#endif // PROJECTV_TEARDOWN_COMMON_H
