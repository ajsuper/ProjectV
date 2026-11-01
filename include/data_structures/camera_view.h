#ifndef PROJV_CAMERA_VIEW_H
#define PROJV_CAMERA_VIEW_H

#include "core/math.h"

namespace projv {
    /**
     * Where a renderer looks from, and how it projects. Set on a renderer with graphics::setCamera;
     * the renderer driver uploads it, and the view that renderer last drew from as the previous frame,
     * before every pass (pjv_engine.sc's pjvCamera* uniforms, read through pjv_camera.sc).
     *
     * The basis is derived from `forward` alone, with no roll: right = forward x worldUp, where
     * worldUp is +y, or +z when forward is within 0.999 of vertical (graphics::cameraBasis).
     * Plan: docs/plans/shader-library.md, D4.
     */
    struct CameraView {
        core::vec3 position{0.0f, 0.0f, 0.0f};
        core::vec3 forward{0.0f, 0.0f, 1.0f};   ///< Need not be unit length.
        float verticalFovDegrees = 60.0f;       ///< Perspective only.
        float orthographicHeight = 0.0f;        ///< World units the image spans vertically; 0 is perspective.
        float orthographicBackoff = 0.0f;       ///< Orthographic rays start this far behind position.
        float nearPlane = 0.05f;
        float farPlane = 10000.0f;
        core::vec2 jitter{0.0f, 0.0f};          ///< Sub-pixel offset of this frame's samples, in pixels.
    };
}

#endif
