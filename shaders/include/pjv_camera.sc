#ifndef PJV_CAMERA_SC
#define PJV_CAMERA_SC

// =============================================================================
// pjv_camera.sc -- the engine's camera model (docs/plans/shader-library.md, D4)
// =============================================================================
//
// Two layers. The functions taking a camera's position and direction are the projection itself, for
// any camera a renderer has (a light's, a probe's). The pjvView* ones apply it to the camera the
// engine uploads (graphics::setCamera) and to the one its renderer last drew from (pjvPrevView*), so a
// renderer declares no camera uniforms and defines no field of view.
//
// The same arithmetic as graphics/camera.h on the CPU, so a pick or a gizmo lands on what is drawn.
// uv is [0, 1] with a top-left origin; res the image's size in pixels.

#include <pjv_engine.sc>

struct PjvCameraBasis {
    vec3 forward;
    vec3 right;
    vec3 up;
};

// No roll: right is forward x worldUp, worldUp +y, or +z within 0.999 of vertical.
PjvCameraBasis pjvCameraBasis(vec3 direction) {
    PjvCameraBasis b;
    b.forward = normalize(direction);
    vec3 worldUp = abs(b.forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    b.right = normalize(cross(b.forward, worldUp));
    b.up = normalize(cross(b.right, b.forward));
    return b;
}

// --- Any camera ---------------------------------------------------------------
//
// A perspective projection is given by tanHalfFov, tan(vertical field of view / 2): pjvTanHalfFov for a
// field of view in degrees, or pjvViewTanHalfFov for the engine camera's, which the CPU computes in
// double precision -- the value a constant field of view folds to at compile time.

float pjvTanHalfFov(float verticalFovDegrees) {
    return tan(radians(verticalFovDegrees * 0.5));
}

// A perspective ray's direction through uv.
vec3 pjvPerspectiveRay(vec2 uv, vec2 res, vec3 direction, float tanHalfFov) {
    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float aspectRatio = res.x / res.y;
    float scale = tanHalfFov;
    PjvCameraBasis b = pjvCameraBasis(direction);
    return normalize(b.right * (ndc.x * scale * aspectRatio) + b.up * (ndc.y * scale) + b.forward);
}

// An orthographic ray's origin: on the plane `backoff` behind position, `height` world units tall.
// Its direction is the camera's forward.
vec3 pjvOrthographicOrigin(vec2 uv, vec2 res, vec3 position, vec3 direction, float height, float backoff) {
    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float aspectRatio = res.x / res.y;
    float halfHeight = height * 0.5;
    PjvCameraBasis b = pjvCameraBasis(direction);
    return position - b.forward * backoff + b.right * (ndc.x * halfHeight * aspectRatio) + b.up * (ndc.y * halfHeight);
}

// Where a world point lands (uv), under perspective: the inverse of pjvPerspectiveRay.
vec2 pjvPerspectiveUV(vec3 P, vec2 res, vec3 position, vec3 direction, float tanHalfFov, out bool inFront) {
    PjvCameraBasis b = pjvCameraBasis(direction);
    vec3 d = P - position;
    float z = dot(d, b.forward);
    inFront = z > 1e-4;
    float scale = tanHalfFov;
    vec2 ndc = vec2(dot(d, b.right) / (z * scale * (res.x / res.y)), dot(d, b.up) / (z * scale));
    return vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
}

// Where a world point lands (uv), under an orthographic projection: the inverse of
// pjvOrthographicOrigin. In front when it is beyond the plane the rays start on.
vec2 pjvOrthographicUV(vec3 P, vec2 res, vec3 position, vec3 direction, float height, float backoff, out bool inFront) {
    PjvCameraBasis b = pjvCameraBasis(direction);
    vec3 toPoint = P - (position - b.forward * backoff);
    inFront = dot(toPoint, b.forward) > 0.0;
    float halfHeight = max(height, 1e-6) * 0.5;
    vec2 ndc = vec2(dot(toPoint, b.right) / (halfHeight * (res.x / res.y)), dot(toPoint, b.up) / halfHeight);
    return vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
}

// --- The engine's camera ------------------------------------------------------

vec3  pjvViewPosition()  { return pjvCameraPos.xyz; }
vec3  pjvViewDirection() { return pjvCameraDir.xyz; }   // as set, not normalised
float pjvViewFov()       { return pjvCameraPos.w; }   // degrees
float pjvViewTanHalfFov() { return pjvCameraLens.w; }
bool  pjvViewIsOrthographic() { return pjvCameraDir.w > 0.0; }
float pjvViewOrthographicHeight() { return pjvCameraDir.w; }

vec3  pjvPrevViewPosition()  { return pjvPrevCameraPos.xyz; }
vec3  pjvPrevViewDirection() { return pjvPrevCameraDir.xyz; }
float pjvPrevViewFov()       { return pjvPrevCameraPos.w; }
float pjvPrevViewTanHalfFov() { return pjvPrevCameraLens.w; }

// The engine camera's ray through uv, perspective or orthographic.
vec3 pjvViewRayOrigin(vec2 uv, vec2 res) {
    if (pjvCameraDir.w <= 0.0) return pjvCameraPos.xyz;
    return pjvOrthographicOrigin(uv, res, pjvCameraPos.xyz, pjvCameraDir.xyz, pjvCameraDir.w, pjvCameraLens.x);
}

vec3 pjvViewRayDirection(vec2 uv, vec2 res) {
    if (pjvCameraDir.w > 0.0) return normalize(pjvCameraDir.xyz);
    return pjvPerspectiveRay(uv, res, pjvCameraDir.xyz, pjvCameraLens.w);
}

// Where a world point was in the previous frame's image, under the projection that frame had: what a
// temporal pass reprojects by.
vec2 pjvPrevViewUV(vec3 P, vec2 res, out bool inFront) {
    if (pjvPrevCameraDir.w > 0.0) {
        return pjvOrthographicUV(P, res, pjvPrevCameraPos.xyz, pjvPrevCameraDir.xyz, pjvPrevCameraDir.w,
                                 pjvPrevCameraLens.x, inFront);
    }
    return pjvPerspectiveUV(P, res, pjvPrevCameraPos.xyz, pjvPrevCameraDir.xyz, pjvPrevCameraLens.w, inFront);
}

#endif
