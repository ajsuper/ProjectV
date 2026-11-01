#ifndef PROJV_GRAPHICS_CAMERA_H
#define PROJV_GRAPHICS_CAMERA_H

#include <array>

#include "core/math.h"
#include "data_structures/camera_view.h"
#include "data_structures/constructedRenderer.h"

// The engine's camera model, on the CPU: the same rays and projections pjv_camera.sc makes on the
// GPU, so a program can pick, project a gizmo or place a cursor against exactly what is drawn.
// Plan: docs/plans/shader-library.md, D4.
namespace projv::graphics {
    struct CameraBasis {
        core::vec3 forward;
        core::vec3 right;
        core::vec3 up;
    };

    /** The camera's orthonormal basis from its forward vector (see CameraView). */
    CameraBasis cameraBasis(core::vec3 forward);

    /**
     * The ray through a pixel. uv is in [0, 1] with a top-left origin, resolution the image's in
     * pixels. Perspective rays start at the camera; orthographic ones on the plane behind it.
     */
    core::vec3 cameraRayOrigin(const CameraView& camera, core::vec2 uv, core::vec2 resolution);
    core::vec3 cameraRayDirection(const CameraView& camera, core::vec2 uv, core::vec2 resolution);

    /**
     * Where a world point lands in the image (uv, top-left origin): the inverse of the ray.
     * @param inFront Set to whether the point is in front of the camera.
     */
    core::vec2 cameraWorldToUV(const CameraView& camera, core::vec3 point, core::vec2 resolution, bool* inFront = nullptr);

    /**
     * Sets the view this renderer draws from next. The view it drew from last becomes the previous
     * frame's, for reprojection; until it has drawn once, the previous frame is this one.
     */
    void setCamera(ConstructedRenderer& renderer, const CameraView& camera);

    /**
     * The values of pjv_engine.sc's camera uniforms, in declaration order: pjvCameraPos,
     * pjvCameraDir, pjvCameraLens, pjvCameraJitter, pjvPrevCameraPos, pjvPrevCameraDir,
     * pjvPrevCameraLens. The driver uploads these; exposed for tests.
     */
    std::array<core::vec4, 7> cameraUniformValues(const CameraView& camera, const CameraView& previous);
}

#endif
