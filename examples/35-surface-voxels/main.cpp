// ProjectV Surface Voxels -- prototype
//
// Voxels as bounding boxes for implicit surfaces. A voxel may carry a quadric in its own frame, and
// the renderer clips that quadric to the voxel's box: occupancy says WHERE the surface is and what
// the traversal visits, the quadric says exactly where inside the box it lies. Voxels without one
// are ordinary boxes. See include/utils/surface_quadrics.h and include/pjv_surface.sc.
//
// Two sources of quadrics:
//
//   * no arguments    a built-in scene of analytic primitives (sphere, cylinder, cone, ...), each
//                     one quadric, re-expressed per voxel. Exact, and one-sided (solid).
//   * --scene <dir>   any compose scene. Quadrics come from a `.surfaces` sidecar beside each
//                     `.data`, which `mesh_voxelizer --surfaces` writes by fitting the original
//                     triangles. A scene without sidecars renders as plain voxels in every mode.
//
// Controls:
//   1 / 2 / 3 -- voxels / contours / quadrics
//   4         -- toggle sun shadows
//   5         -- tint voxels that have no quadric (where the fit fell back to a box)
//   6         -- paint leaks red: a fitted surface seen from behind, i.e. a ray through a crack
//   0         -- baseline: the plain voxel traversal with no surface lookup (for comparison)
//   W/S/A/D   -- move,  R/F -- up / down,  Mouse -- look,  Scroll -- movement speed
//   Esc       -- release the cursor (left-click re-captures); close the window to quit
//
// Usage:  ./surface_voxels [--scene <dir>] [--res 16|64|256] [--mode 0|1|2|3] [--no-shadows]
//
// Measurement, both of which fly a fixed orbit around the scene and exit when done:
//   SURFACE_BENCH=<frames>    per mode 0..3, two interleaved rounds: mean GPU and wall frame time
//   SURFACE_CAPTURE=<dir>     writes mode<N>.ppm per mode from the app's own frame (no desktop
//                             screenshot), and mode<N>_leaks.ppm with leaks painted red

#include <array>
#include <chrono>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/ecs.h"
#include "core/log.h"
#include "core/math.h"
#include "core/paths.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "utils/compose_io.h"
#include "utils/material.h"
#include "utils/surface_quadrics.h"
#include "utils/voxel_management.h"

namespace {

constexpr float DEMO_WORLD_SIZE = 32.0f;  // World units across the demo chunk, at any resolution.

std::string g_scenePath;
int  g_resolution   = 64;
int  g_startMode    = 3;
bool g_startShadows = true;
double g_scrollAccum = 0.0;

void scrollCallback(GLFWwindow*, double, double yoffset) { g_scrollAccum += yoffset; }

// -----------------------------------------------------------------------------------------
// The built-in scene: analytic quadrics
// -----------------------------------------------------------------------------------------
//
// f(x) = [x 1] Q [x 1]^T, solid where f <= 0, Q symmetric. Built in a primitive's local frame and
// carried into voxel space by Q' = T^T Q T, where T maps a voxel-space point into the local frame.

struct Mat4d {
    double m[4][4] = {};
};

Mat4d multiply(const Mat4d& a, const Mat4d& b) {
    Mat4d r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

Mat4d transpose(const Mat4d& a) {
    Mat4d r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[j][i];
    return r;
}

using Rotation = std::array<std::array<double, 3>, 3>;

// Euler angles (radians), applied X then Y then Z. Local -> world.
Rotation rotationXYZ(double rx, double ry, double rz) {
    const double cx = std::cos(rx), sx = std::sin(rx);
    const double cy = std::cos(ry), sy = std::sin(ry);
    const double cz = std::cos(rz), sz = std::sin(rz);
    const double X[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
    const double Y[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    const double Z[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
    double YX[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) YX[i][j] += Y[i][k] * X[k][j];
    Rotation R{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) R[i][j] += Z[i][k] * YX[k][j];
    return R;
}

const Rotation kIdentity = rotationXYZ(0, 0, 0);

// Places a local-frame quadric at `centre` (unit space: the chunk as [0,1]^3, so the scene reads the
// same at every resolution) with rotation R, expressed in voxel space.
Mat4d placeQuadric(const Mat4d& local, const std::array<double, 3>& centre, const Rotation& R) {
    // T: voxel-space point -> local frame.  local = R^T (x / res - centre)
    const double s = 1.0 / g_resolution;
    Mat4d T;
    for (int i = 0; i < 3; ++i) {
        double offset = 0.0;
        for (int j = 0; j < 3; ++j) {
            T.m[i][j] = R[j][i] * s;
            offset -= R[j][i] * centre[j];
        }
        T.m[i][3] = offset;
    }
    T.m[3][3] = 1.0;
    return multiply(transpose(T), multiply(local, T));
}

struct Primitive {
    const char* name;
    std::array<uint8_t, 3> colour;
    Mat4d quadric;                   // In voxel space.
    std::array<double, 3> boundsMin; // Unit space. The quadric is clipped to this box, which is
    std::array<double, 3> boundsMax; // how an infinite cylinder becomes a finite one.
};

constexpr double GROUND = 1.0 / 16.0;  // Ground top, in unit space: a whole voxel at every res.

std::vector<Primitive> buildPrimitives() {
    std::vector<Primitive> prims;

    // Ground: the plane y = GROUND. A plane is a quadric with only linear terms.
    {
        Mat4d L;
        L.m[1][3] = L.m[3][1] = 0.5;  // f = y
        prims.push_back({"ground", {150, 150, 140},
                         placeQuadric(L, {0.0, GROUND, 0.0}, kIdentity),
                         {0.0, 0.0, 0.0}, {1.0, GROUND, 1.0}});
    }
    // Sphere, resting on the ground.
    {
        const double r = 0.16;
        Mat4d L;
        L.m[0][0] = L.m[1][1] = L.m[2][2] = 1.0;
        L.m[3][3] = -r * r;
        prims.push_back({"sphere", {215, 70, 60},
                         placeQuadric(L, {0.25, GROUND + r, 0.25}, kIdentity),
                         {0.25 - r, GROUND, 0.25 - r}, {0.25 + r, GROUND + 2 * r, 0.25 + r}});
    }
    // Upright cylinder. The quadric is infinite; the bounds cut its ends, and because they fall on
    // voxel boundaries the caps come out flat and exact.
    {
        const double r = 0.11;
        Mat4d L;
        L.m[0][0] = L.m[2][2] = 1.0;
        L.m[3][3] = -r * r;
        prims.push_back({"cylinder", {70, 180, 95},
                         placeQuadric(L, {0.72, 0.0, 0.25}, kIdentity),
                         {0.72 - r, GROUND, 0.25 - r}, {0.72 + r, 0.45, 0.25 + r}});
    }
    // Cone, apex up. A cone is a double cone; the bounds keep the lower nappe.
    {
        const double apexY = 0.5, baseR = 0.15;
        const double k = baseR / (apexY - GROUND);
        Mat4d L;
        L.m[0][0] = L.m[2][2] = 1.0;
        L.m[1][1] = -k * k;
        prims.push_back({"cone", {70, 120, 220},
                         placeQuadric(L, {0.25, apexY, 0.72}, kIdentity),
                         {0.25 - baseR, GROUND, 0.72 - baseR}, {0.25 + baseR, apexY, 0.72 + baseR}});
    }
    // Hyperboloid of one sheet -- a solid spool, or a cooling tower.
    {
        const double a = 0.07, c = 0.09, cy = 0.26;
        Mat4d L;
        L.m[0][0] = L.m[2][2] = 1.0 / (a * a);
        L.m[1][1] = -1.0 / (c * c);
        L.m[3][3] = -1.0;
        const double reach = 0.17;
        prims.push_back({"spool", {235, 150, 50},
                         placeQuadric(L, {0.72, cy, 0.72}, kIdentity),
                         {0.72 - reach, GROUND, 0.72 - reach}, {0.72 + reach, 0.46, 0.72 + reach}});
    }
    // Tilted cylinder. Its ends are cut by an axis-aligned box, so they come out as stair-stepped
    // voxel faces: one quadric per voxel cannot make a slanted cap.
    {
        const double r = 0.05;
        Mat4d L;
        L.m[0][0] = L.m[2][2] = 1.0;
        L.m[3][3] = -r * r;
        prims.push_back({"tilted cylinder", {200, 200, 210},
                         placeQuadric(L, {0.5, 0.25, 0.5}, rotationXYZ(0.0, 0.0, -0.45)),
                         {0.36, GROUND, 0.44}, {0.64, 0.42, 0.56}});
    }
    // Ellipsoid, rotated on two axes so its quadric has cross terms.
    {
        const double ax = 0.22, ay = 0.07, az = 0.11;
        Mat4d L;
        L.m[0][0] = 1.0 / (ax * ax);
        L.m[1][1] = 1.0 / (ay * ay);
        L.m[2][2] = 1.0 / (az * az);
        L.m[3][3] = -1.0;
        prims.push_back({"ellipsoid", {165, 90, 200},
                         placeQuadric(L, {0.5, 0.72, 0.5}, rotationXYZ(0.0, 0.52, 0.26)),
                         {0.26, 0.55, 0.26}, {0.74, 0.89, 0.74}});
    }
    return prims;
}

// Bounds on f over the voxel at (x, y, z), from its centre c:
//   f(c + e) = f(c) + g.e + e^T A e,  |e| <= sqrt(3)/2 = rho,  ||A|| <= its Frobenius norm.
struct VoxelRange { double lo, hi; };

VoxelRange quadricRangeOverVoxel(const Mat4d& Q, int x, int y, int z) {
    const double h[4] = {x + 0.5, y + 0.5, z + 0.5, 1.0};
    double f = 0.0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) f += h[i] * Q.m[i][j] * h[j];
    double gradSq = 0.0, frob = 0.0;
    for (int i = 0; i < 3; ++i) {
        double g = 0.0;
        for (int j = 0; j < 4; ++j) g += 2.0 * Q.m[i][j] * h[j];
        gradSq += g * g;
        for (int j = 0; j < 3; ++j) frob += Q.m[i][j] * Q.m[i][j];
    }
    const double rho = 0.8660254;
    const double spread = std::sqrt(gradSq) * rho + std::sqrt(frob) * rho * rho;
    return {f - spread, f + spread};
}

// Builds the demo chunk and its per-voxel quadrics. A voxel is solid when the primitive may reach
// into it (conservative: the shader rejects the boxes it misses), and carries a quadric unless the
// primitive certainly fills it -- an interior voxel is just a box.
projv::Scene buildDemoScene(std::vector<projv::utils::SurfaceVoxel>& quadrics) {
    const std::vector<Primitive> prims = buildPrimitives();
    projv::Scene scene;
    const int res = g_resolution;

    projv::ChunkHeader header;
    header.chunkID    = 1;
    header.position   = projv::core::vec3(0.0f);
    header.scale      = DEMO_WORLD_SIZE;
    header.voxelScale = DEMO_WORLD_SIZE / res;
    header.resolution = res;
    header.rotation   = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);

    projv::Chunk chunk;
    chunk.header          = header;
    chunk.requestedLOD    = 0;
    chunk.alive           = true;
    chunk.componentHandle = 0;

    scene.components.push_back(projv::ComponentRecord{
        projv::ComponentKind::Chunk, 0, -1, "internal/surface_voxels", false, {}, -1});
    projv::ComponentRecord& component = scene.components[0];

    std::vector<uint8_t> slots;
    for (const Primitive& p : prims) {
        slots.push_back(projv::utils::internMaterial(scene, component, p.name,
                            projv::packColor({p.colour[0], p.colour[1], p.colour[2]})));
    }

    auto brickMap = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(res));
    std::vector<uint8_t> claimed(size_t(res) * res * res, 0);
    size_t solid = 0;

    // First primitive to claim a voxel owns it. The scene is laid out so primitives do not share
    // voxels; where two would, one voxel can only name one surface.
    for (size_t i = 0; i < prims.size(); ++i) {
        const Primitive& p = prims[i];
        int lo[3], hi[3];
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::max(0, int(std::floor(p.boundsMin[a] * res + 1e-9)));
            hi[a] = std::min(res, int(std::ceil(p.boundsMax[a] * res - 1e-9)));
        }
        for (int z = lo[2]; z < hi[2]; ++z)
            for (int y = lo[1]; y < hi[1]; ++y)
                for (int x = lo[0]; x < hi[0]; ++x) {
                    uint8_t& owner = claimed[(size_t(z) * res + y) * res + x];
                    if (owner) continue;
                    const VoxelRange range = quadricRangeOverVoxel(p.quadric, x, y, z);
                    if (range.lo > 0.0) continue;
                    owner = 1;
                    ++solid;
                    projv::utils::brickMapSetVoxel(*brickMap, x, y, z, slots[i]);
                    if (range.hi >= 0.0) {
                        // One term, one-sided. The contour is the primitive's tangent plane at
                        // the voxel centre -- the quadric's linear part, already unit length.
                        projv::utils::SurfaceVoxel entry;
                        entry.voxel = projv::utils::packSurfaceVoxel(x, y, z);
                        float* q = entry.term[0].quadric;
                        projv::utils::quadricMatrixToVoxelFrame(p.quadric.m, {x, y, z}, q);
                        entry.term[0].plane[0] = q[6];
                        entry.term[0].plane[1] = q[7];
                        entry.term[0].plane[2] = q[8];
                        entry.term[0].plane[3] = q[9];
                        quadrics.push_back(entry);
                    }
                }
    }

    projv::utils::updateChunkFromBrickMap(chunk, *brickMap);
    std::vector<uint8_t> bakedMaterialIDs;
    projv::utils::bakeMaterialsFromBrickMap(chunk.geometryData, bakedMaterialIDs, *brickMap);

    const int32_t blobIndex = projv::internChunkGeometry(scene, chunk, std::move(brickMap));
    if (blobIndex >= 0 && static_cast<size_t>(blobIndex) < scene.geometryPool.size()) {
        scene.geometryPool[blobIndex].materialIDs = std::move(bakedMaterialIDs);
    }

    scene.chunks.push_back(std::move(chunk));
    scene.looseChunks.push_back(0);
    scene.looseChunkCount = 1;

    projv::core::info("Built a {}^3 demo chunk: {} primitive(s), {} solid voxel(s), {} with a quadric.",
                      res, prims.size(), solid, quadrics.size());
    return scene;
}

// -----------------------------------------------------------------------------------------
// Camera and input
// -----------------------------------------------------------------------------------------

struct Camera {
    projv::core::vec3 position{-12.0f, 20.0f, -12.0f};
    float yaw   = 0.785f;
    float pitch = -0.38f;
    float speed = 0.2f;
};

struct ViewState {
    int  mode    = 3;
    bool shadows = true;
    bool tintFallback = false;
    bool paintLeaks = false;
    int  debugView = 0;          // Set by SURFACE_CAPTURE only: 1 leaks.
    projv::core::vec3 orbitCentre{16.0f, 8.0f, 16.0f};
    float orbitDistance = 40.0f;
    projv::core::vec4 contourSet{0.0f};   // (nodeBase, recordBase, entryBase, -) per style
    projv::core::vec4 quadricSet{0.0f};
    float dataWidthLog2 = 0.0f;
};

// Frames the camera on every live chunk's bounding box, from a raised three-quarter angle.
void frameScene(const projv::Scene& scene, Camera& camera, ViewState& viewState) {
    using namespace projv::core;
    bool found = false;
    vec3 lo(0.0f), hi(0.0f);
    for (const projv::Chunk& chunk : scene.chunks) {
        if (!chunk.alive || chunk.header.scale <= 0.0f) continue;
        const vec3 cMin = chunk.header.position;
        const vec3 cMax = chunk.header.position + vec3(chunk.header.scale);
        lo = found ? min(lo, cMin) : cMin;
        hi = found ? max(hi, cMax) : cMax;
        found = true;
    }
    if (!found) return;
    const vec3 centre = (lo + hi) * 0.5f;
    const float radius = std::max(length(hi - lo) * 0.5f, 1.0f);
    const float distance = radius / std::tan(0.5236f) * 1.1f;
    camera.yaw = 0.785f;
    camera.pitch = -0.35f;
    const vec3 view{std::cos(camera.pitch) * std::cos(camera.yaw), std::sin(camera.pitch),
                    std::cos(camera.pitch) * std::sin(camera.yaw)};
    camera.position = centre - view * distance;
    camera.speed = radius * 0.004f;
    viewState.orbitCentre = centre;
    viewState.orbitDistance = distance;
}

bool   g_cursorCaptured = true;
double g_lastMouseX = 0.0, g_lastMouseY = 0.0;
bool   g_mouseTracking = false;

void updateCamera(Camera& camera, GLFWwindow* window) {
    if (g_cursorCaptured && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        g_cursorCaptured = false;
    } else if (!g_cursorCaptured && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        g_cursorCaptured = true;
    }

    double mouseX, mouseY;
    glfwGetCursorPos(window, &mouseX, &mouseY);
    if (!g_mouseTracking || !g_cursorCaptured) {
        g_lastMouseX = mouseX;
        g_lastMouseY = mouseY;
        g_mouseTracking = true;
    }
    if (g_cursorCaptured) {
        const float sensitivity = 0.0025f;
        camera.yaw   += static_cast<float>(mouseX - g_lastMouseX) * sensitivity;
        camera.pitch -= static_cast<float>(mouseY - g_lastMouseY) * sensitivity;
        camera.pitch = std::fmax(-1.55f, std::fmin(1.55f, camera.pitch));
    }
    g_lastMouseX = mouseX;
    g_lastMouseY = mouseY;

    if (g_scrollAccum != 0.0) {
        camera.speed *= float(std::pow(1.25, g_scrollAccum));
        g_scrollAccum = 0.0;
    }

    const projv::core::vec3 forward{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
    const projv::core::vec3 right{std::cos(camera.yaw - 1.5708f), 0.0f, std::sin(camera.yaw - 1.5708f)};
    const float speed = camera.speed;

    if (glfwGetKey(window, GLFW_KEY_W)) camera.position += forward * speed;
    if (glfwGetKey(window, GLFW_KEY_S)) camera.position -= forward * speed;
    if (glfwGetKey(window, GLFW_KEY_D)) camera.position += right * speed;
    if (glfwGetKey(window, GLFW_KEY_A)) camera.position -= right * speed;
    if (glfwGetKey(window, GLFW_KEY_R)) camera.position[1] += speed;
    if (glfwGetKey(window, GLFW_KEY_F)) camera.position[1] -= speed;
}

void updateTitle(GLFWwindow* window, const ViewState& view) {
    static const char* names[] = {"0: Baseline", "1: Voxels", "2: Contours", "3: Quadrics"};
    const std::string source = g_scenePath.empty()
        ? std::to_string(g_resolution) + "^3 demo"
        : std::filesystem::path(g_scenePath).filename().string();
    const std::string title = std::string("ProjectV Surface Voxels -- ") + names[view.mode] +
                              "  (" + source + ", shadows " + (view.shadows ? "on" : "off") +
                              (view.tintFallback ? ", boxes tinted" : "") +
                              (view.paintLeaks ? ", leaks red" : "") + ")";
    glfwSetWindowTitle(window, title.c_str());
}

// Rising-edge toggle for a key.
bool pressedOnce(GLFWwindow* window, int key, bool& held) {
    const bool down = glfwGetKey(window, key) == GLFW_PRESS;
    const bool fired = down && !held;
    held = down;
    return fired;
}

void updateViewKeys(ViewState& view, GLFWwindow* window) {
    static bool shadowHeld = false, tintHeld = false, leakHeld = false;
    bool changed = false;
    for (int mode = 0; mode <= 3; ++mode) {
        if (glfwGetKey(window, GLFW_KEY_0 + mode) == GLFW_PRESS && view.mode != mode) {
            view.mode = mode;
            changed = true;
        }
    }
    if (pressedOnce(window, GLFW_KEY_4, shadowHeld)) { view.shadows = !view.shadows; changed = true; }
    if (pressedOnce(window, GLFW_KEY_5, tintHeld)) { view.tintFallback = !view.tintFallback; changed = true; }
    if (pressedOnce(window, GLFW_KEY_6, leakHeld)) { view.paintLeaks = !view.paintLeaks; changed = true; }
    if (changed) updateTitle(window, view);
}

// -----------------------------------------------------------------------------------------
// Measurement: SURFACE_BENCH and SURFACE_CAPTURE
// -----------------------------------------------------------------------------------------
//
// Both fly the same orbit -- around the framing's centre, at the framing's distance, slightly
// above -- so a mode's numbers and pictures are of the same views as every other mode's.

struct Measurement {
    int benchFrames = 0;            // Frames measured per segment; 0 = off.
    int warmup = 30;                // Frames skipped at the start of each segment.
    std::vector<int> segments;      // Mode per segment, in order.
    size_t segment = 0;
    int segmentFrame = 0;
    double gpuMs = 0.0, wallMs = 0.0;
    std::chrono::high_resolution_clock::time_point last;
    double totalGpu[4] = {}, totalWall[4] = {};
    int totalCount[4] = {};

    std::string captureDir;         // Empty = off.
    std::vector<std::pair<int, int>> captures;    // (mode, debug: 0 none, 1 leaks)
    size_t capture = 0;
    int captureFrame = 0;
    bgfx::TextureHandle readback = BGFX_INVALID_HANDLE;
    std::vector<uint8_t> pixels;
    uint32_t readWidth = 0, readHeight = 0;

    bool active() const { return benchFrames > 0 || !captureDir.empty(); }
};
Measurement g_measure;

// SURFACE_CAPTURE_LOOK="x,y,z,distance,yaw,pitch" (world units, radians) overrides the capture
// view, for close-ups.
bool captureLook(Camera& camera) {
    const char* value = std::getenv("SURFACE_CAPTURE_LOOK");
    if (!value) return false;
    float x, y, z, distance, yaw, pitch;
    if (std::sscanf(value, "%f,%f,%f,%f,%f,%f", &x, &y, &z, &distance, &yaw, &pitch) != 6) return false;
    camera.yaw = yaw;
    camera.pitch = pitch;
    const projv::core::vec3 dir{std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
    camera.position = projv::core::vec3(x, y, z) - dir * distance;
    return true;
}

void orbitCamera(Camera& camera, const ViewState& view, float angle) {
    const float pitch = -0.35f;
    camera.yaw = angle;
    camera.pitch = pitch;
    const projv::core::vec3 dir{std::cos(pitch) * std::cos(angle), std::sin(pitch),
                                std::cos(pitch) * std::sin(angle)};
    camera.position = view.orbitCentre - dir * view.orbitDistance;
}

bool writePPM(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "P6\n" << w << " " << h << "\n255\n";
    for (size_t i = 0; i < size_t(w) * h; ++i) out.write(reinterpret_cast<const char*>(&rgba[i * 4]), 3);
    return bool(out);
}

// Advances the measurement by one frame: sets the mode and camera for it, records the last one.
// Returns true when everything asked for is done and the app should close.
bool stepMeasurement(ViewState& view, Camera& camera, projv::graphics::RenderInstance& renderInstance) {
    using clock = std::chrono::high_resolution_clock;
    Measurement& m = g_measure;
    const projv::core::vec2 res = renderInstance.getWindowResolution();

    if (m.benchFrames > 0 && m.segment < m.segments.size()) {
        const int mode = m.segments[m.segment];
        const int total = m.warmup + m.benchFrames;
        if (m.segmentFrame > m.warmup) {
            const auto now = clock::now();
            m.wallMs += std::chrono::duration<double, std::milli>(now - m.last).count();
            const bgfx::Stats* stats = bgfx::getStats();
            if (stats && stats->gpuTimerFreq > 0) {
                m.gpuMs += double(stats->gpuTimeEnd - stats->gpuTimeBegin) * 1000.0 / double(stats->gpuTimerFreq);
            }
        }
        m.last = clock::now();
        if (m.segmentFrame == total) {
            const int n = m.benchFrames;
            projv::core::info("BENCH mode={} round={} frames={} resolution={}x{} gpu={:.3f}ms wall={:.3f}ms",
                mode, m.segment * 2 / m.segments.size() + 1, n, int(res.x), int(res.y), m.gpuMs / n, m.wallMs / n);
            m.totalGpu[mode] += m.gpuMs / n;
            m.totalWall[mode] += m.wallMs / n;
            m.totalCount[mode]++;
            m.gpuMs = m.wallMs = 0.0;
            m.segmentFrame = 0;
            if (++m.segment == m.segments.size()) {
                static const char* names[] = {"baseline", "voxels", "contours", "quadrics"};
                for (int i = 0; i < 4; ++i) {
                    if (!m.totalCount[i]) continue;
                    projv::core::info("BENCH summary {:9s} gpu={:7.3f}ms ({:.2f}x baseline) wall={:7.3f}ms",
                        names[i], m.totalGpu[i] / m.totalCount[i],
                        m.totalCount[0] ? (m.totalGpu[i] / m.totalCount[i]) / (m.totalGpu[0] / m.totalCount[0]) : 0.0,
                        m.totalWall[i] / m.totalCount[i]);
                }
            }
        }
        if (m.segment < m.segments.size()) {
            view.mode = m.segments[m.segment];
            // Warmup frames hold the first view; measured frames sweep one full orbit.
            const int step = std::max(0, m.segmentFrame - m.warmup);
            orbitCamera(camera, view, 0.785f + 6.2831853f * float(step) / float(m.benchFrames));
            m.segmentFrame++;
            return false;
        }
    }

    if (!m.captureDir.empty() && m.capture < m.captures.size()) {
        auto [mode, debug] = m.captures[m.capture];
        view.mode = mode;
        view.debugView = debug;
        view.tintFallback = std::getenv("SURFACE_CAPTURE_TINT") != nullptr;
        if (!captureLook(camera)) orbitCamera(camera, view, 0.785f);
        constexpr int settle = 10, readAt = 10, writeAt = 16;
        if (m.captureFrame == readAt) {
            auto renderer = renderInstance.getActiveRenderer();
            bgfx::TextureHandle frame = renderer->resources.textures.textureHandles.at(2);
            const uint32_t w = uint32_t(res.x), h = uint32_t(res.y);
            if (!bgfx::isValid(m.readback) || w != m.readWidth || h != m.readHeight) {
                if (bgfx::isValid(m.readback)) bgfx::destroy(m.readback);
                m.readback = bgfx::createTexture2D(uint16_t(w), uint16_t(h), false, 1, bgfx::TextureFormat::RGBA8,
                                                   BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
                m.readWidth = w;
                m.readHeight = h;
                m.pixels.assign(size_t(w) * h * 4, 0);
            }
            bgfx::blit(200, m.readback, 0, 0, frame, 0, 0, uint16_t(w), uint16_t(h));
            bgfx::readTexture(m.readback, m.pixels.data());
        }
        if (m.captureFrame == writeAt) {
            static const char* suffix[] = {"", "_leaks"};
            const std::string path = m.captureDir + "/mode" + std::to_string(mode) + suffix[debug] + ".ppm";
            if (writePPM(path, m.pixels, m.readWidth, m.readHeight)) {
                projv::core::info("CAPTURE {} ({}x{})", path, m.readWidth, m.readHeight);
            }
            m.captureFrame = 0;
            m.capture++;
            return false;
        }
        (void)settle;
        m.captureFrame++;
        return false;
    }
    return true;
}

// Bytes the voxels themselves cost on the GPU (tree64 + material bytes, once per geometry blob),
// against the surface data that rides beside them.
void reportMemory(const projv::Scene& scene, const projv::utils::SurfaceGPUData& surfaces) {
    size_t geometryBytes = 0, materialBytes = 0;
    for (const projv::GeometryBlob& blob : scene.geometryPool) {
        if (blob.refCount == 0) continue;
        geometryBytes += blob.geometry.size() * sizeof(uint32_t);
        materialBytes += blob.materialIDs.size();
    }
    const double mb = 1.0 / (1024.0 * 1024.0);
    const size_t voxelBytes = geometryBytes + materialBytes;
    projv::core::info("MEMORY voxels: tree64 {:.3f} MB + materials {:.3f} MB = {:.3f} MB",
        geometryBytes * mb, materialBytes * mb, voxelBytes * mb);
    for (int s = 0; s < 2; ++s) {
        const projv::utils::SurfaceGPUSet& set = s == 0 ? surfaces.contour : surfaces.quadric;
        projv::core::info("MEMORY {} style: {:.3f} MB ({:.2f} bytes per surface voxel, {:.1f}x the voxel data)",
            s == 0 ? "contour" : "quadric", set.totalBytes() * mb,
            set.entries ? double(set.totalBytes()) / set.entries : 0.0,
            voxelBytes ? double(set.totalBytes()) / double(voxelBytes) : 0.0);
    }
}

// -----------------------------------------------------------------------------------------
// Application stages
// -----------------------------------------------------------------------------------------

void startup(projv::Application& app) {
    auto& renderInstance =
        projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(1280, 720, "ProjectV Surface Voxels");
    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetScrollCallback(renderInstance.window, scrollCallback);

    auto& scene   = projv::core::createGlobalResource<projv::Scene>(app.world);
    auto& gpuData = projv::core::createGlobalResource<projv::GPUData>(app.world);
    auto& view    = projv::core::createGlobalResource<ViewState>(app.world);
    auto& camera  = projv::core::createGlobalResource<Camera>(app.world);
    view.mode = g_startMode;
    view.shadows = g_startShadows;

    // The table is keyed by chunk handle, which is also the chunk's GPU header index.
    std::unordered_map<uint32_t, std::vector<projv::utils::SurfaceVoxel>> demoQuadrics;
    if (g_scenePath.empty()) {
        scene = buildDemoScene(demoQuadrics[0]);
        if (g_measure.active()) frameScene(scene, camera, view);
    } else {
        scene = projv::utils::loadComposeFromDisk(g_scenePath);
        frameScene(scene, camera, view);
    }
    // The surface data indexes tree nodes by where the upload put them, so the scene goes first.
    gpuData = projv::graphics::createTexturesForScene(scene);
    // Storage knobs, for measuring the memory/quality trade-off without rebuilding.
    projv::utils::SurfaceGPUOptions surfaceOptions;
    if (const char* v = std::getenv("SURFACE_PLANE_TOL")) surfaceOptions.planeTolerance = float(std::atof(v));
    if (const char* v = std::getenv("SURFACE_TANGENT_TOL")) surfaceOptions.tangentTolerance = float(std::atof(v));
    projv::utils::SurfaceGPUData surfaces =
        projv::utils::buildSurfaceGPUData(scene, gpuData, demoQuadrics, surfaceOptions);
    if (!g_scenePath.empty() && surfaces.voxels == 0) {
        projv::core::warn("No surface quadrics found beside this scene's .data files -- every mode "
                          "will draw plain voxels. Voxelize the model with `mesh_voxelizer --surfaces`.");
    }
    reportMemory(scene, surfaces);
    view.contourSet = {float(surfaces.contour.nodeBase), float(surfaces.contour.recordBase),
                       float(surfaces.contour.entryBase), 0.0f};
    view.quadricSet = {float(surfaces.quadric.nodeBase), float(surfaces.quadric.recordBase),
                       float(surfaces.quadric.entryBase), 0.0f};
    view.dataWidthLog2 = float(std::log2(double(surfaces.width)));

    const std::filesystem::path assets = projv::core::executableDirectory();
    const std::string rendererDirectory = (assets / "surfaceRenderer").string() + "/";
    projv::RendererSpecification specification =
        projv::graphics::loadRendererSpecification(rendererDirectory);
    // The data's size is only known now, so the texture declared in resources.json is resized
    // before the renderer is constructed rather than fixed in the file.
    for (projv::Texture& texture : specification.resources.textures) {
        if (texture.textureID == 1) {
            texture.resolutionX = int(surfaces.width);
            texture.resolutionY = int(surfaces.height);
        }
    }
    renderInstance.addRendererSpecification(1, specification);

    bgfx::ShaderHandle vertexShader = projv::graphics::loadShader(
        (assets / "surfaceRenderer/surfaceShaders/vs_quad.bin").string());
    renderInstance.setActiveRenderer(projv::graphics::constructRendererSpecification(
        renderInstance.getRendererSpecification(1), vertexShader));
    projv::graphics::setTextureToData(renderInstance.getActiveRenderer(), 1,
        reinterpret_cast<unsigned char*>(surfaces.texels.data()), surfaces.width, surfaces.height);

    updateTitle(renderInstance.window, view);
    if (g_measure.benchFrames > 0) bgfx::setDebug(BGFX_DEBUG_PROFILER);
}

void update(projv::Application& app) {
    auto& renderInstance =
        projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    if (renderInstance.shouldClose) app.closeAppFlag = true;
}

void render(projv::Application& app) {
    auto& renderInstance =
        projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    auto& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    auto& camera  = projv::core::getGlobalResource<Camera>(app.world);
    auto& view    = projv::core::getGlobalResource<ViewState>(app.world);

    if (g_measure.active()) {
        if (stepMeasurement(view, camera, renderInstance)) app.closeAppFlag = true;
    } else {
        updateCamera(camera, renderInstance.window);
        updateViewKeys(view, renderInstance.window);
    }

    // Not const: setUniformToValue dispatches on the deduced type, and const types are unknown to it.
    projv::core::vec3 direction{
        std::cos(camera.pitch) * std::cos(camera.yaw),
        std::sin(camera.pitch),
        std::cos(camera.pitch) * std::sin(camera.yaw)
    };
    projv::core::vec2 resolution = renderInstance.getWindowResolution();
    projv::core::vec4 surfaceMode{float(view.mode), view.shadows ? 1.0f : 0.0f,
                                  view.tintFallback ? 1.0f : 0.0f,
                                  view.debugView ? float(view.debugView) : (view.paintLeaks ? 1.0f : 0.0f)};
    // Contours and quadrics are separate sets in one texture; point the shader at the mode's.
    projv::core::vec4 surfaceInfo = view.mode == 3 ? view.quadricSet : view.contourSet;
    surfaceInfo.w = float(view.mode);
    projv::core::vec4 surfaceInfo2{view.dataWidthLog2, 0.0f, 0.0f, 0.0f};

    auto renderer = renderInstance.getActiveRenderer();
    projv::graphics::setUniformToValue(renderer, "cameraPos", camera.position);
    projv::graphics::setUniformToValue(renderer, "cameraDir", direction);
    projv::graphics::setUniformToValue(renderer, "windowRes", resolution);
    projv::graphics::setUniformToValue(renderer, "surfaceMode", surfaceMode);
    projv::graphics::setUniformToValue(renderer, "pjvSurfaceInfo", surfaceInfo);
    projv::graphics::setUniformToValue(renderer, "pjvSurfaceInfo2", surfaceInfo2);

    projv::graphics::renderConstructedRenderer(renderInstance, renderer, &gpuData);
}

void shutdown(projv::Application& app) {
    auto& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    if (bgfx::isValid(g_measure.readback)) bgfx::destroy(g_measure.readback);
    projv::graphics::destroyGPUData(gpuData);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            g_scenePath = argv[++i];
        } else if (std::strcmp(argv[i], "--res") == 0 && i + 1 < argc) {
            const int res = std::atoi(argv[++i]);
            if (res == 16 || res == 64 || res == 256) {
                g_resolution = res;
            } else {
                projv::core::error("--res must be 16, 64 or 256 (a power of four); using {}.",
                                   g_resolution);
            }
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const int mode = std::atoi(argv[++i]);
            if (mode >= 0 && mode <= 3) g_startMode = mode;
        } else if (std::strcmp(argv[i], "--no-shadows") == 0) {
            g_startShadows = false;
        }
    }

    if (const char* value = std::getenv("SURFACE_BENCH")) {
        g_measure.benchFrames = std::max(0, std::atoi(value));
        // Interleaved, so drift (clocks, thermals) lands on every mode alike.
        std::string modes = std::getenv("SURFACE_BENCH_MODES") ? std::getenv("SURFACE_BENCH_MODES") : "0123";
        for (int round = 0; round < 2; ++round)
            for (char c : modes) if (c >= '0' && c <= '3') g_measure.segments.push_back(c - '0');
    }
    if (const char* value = std::getenv("SURFACE_CAPTURE")) {
        g_measure.captureDir = value;
        std::filesystem::create_directories(g_measure.captureDir);
        for (int mode = 0; mode <= 3; ++mode) g_measure.captures.push_back({mode, 0});
        for (int mode = 2; mode <= 3; ++mode) g_measure.captures.push_back({mode, 1});
    }

    projv::Application app = projv::core::createApp();
    projv::core::assignSystemStage(app, projv::SystemStage::Startup,  startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update,   update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render,   render);
    projv::core::assignSystemStage(app, projv::SystemStage::Shutdown, shutdown);
    projv::core::runApplication(app);
    return 0;
}
