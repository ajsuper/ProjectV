#include "graphics/camera.h"

#include <cmath>

namespace projv::graphics {
    CameraBasis cameraBasis(core::vec3 forward) {
        // Line for line pjv_camera.sc's pjvCameraBasis, and so rayStartDirection's and
        // utils::rayDirectionThroughImage's.
        CameraBasis basis;
        basis.forward = glm::normalize(forward);
        core::vec3 worldUp = std::abs(basis.forward.y) > 0.999f ? core::vec3(0.0f, 0.0f, 1.0f)
                                                                : core::vec3(0.0f, 1.0f, 0.0f);
        basis.right = glm::normalize(glm::cross(basis.forward, worldUp));
        basis.up = glm::normalize(glm::cross(basis.right, basis.forward));
        return basis;
    }

    static core::vec2 toNdc(core::vec2 uv) {
        return core::vec2(uv.x, 1.0f - uv.y) * 2.0f - 1.0f;
    }

    core::vec3 cameraRayOrigin(const CameraView& camera, core::vec2 uv, core::vec2 resolution) {
        if (camera.orthographicHeight <= 0.0f) return camera.position;
        CameraBasis basis = cameraBasis(camera.forward);
        core::vec2 ndc = toNdc(uv);
        float aspectRatio = resolution.y > 0.0f ? resolution.x / resolution.y : 1.0f;
        float halfHeight = camera.orthographicHeight * 0.5f;
        return camera.position - basis.forward * camera.orthographicBackoff +
               basis.right * (ndc.x * halfHeight * aspectRatio) + basis.up * (ndc.y * halfHeight);
    }

    core::vec3 cameraRayDirection(const CameraView& camera, core::vec2 uv, core::vec2 resolution) {
        CameraBasis basis = cameraBasis(camera.forward);
        if (camera.orthographicHeight > 0.0f) return basis.forward;
        core::vec2 ndc = toNdc(uv);
        float aspectRatio = resolution.y > 0.0f ? resolution.x / resolution.y : 1.0f;
        float scale = std::tan(glm::radians(camera.verticalFovDegrees * 0.5f));
        return glm::normalize(basis.right * (ndc.x * scale * aspectRatio) + basis.up * (ndc.y * scale) +
                              basis.forward);
    }

    core::vec2 cameraWorldToUV(const CameraView& camera, core::vec3 point, core::vec2 resolution, bool* inFront) {
        CameraBasis basis = cameraBasis(camera.forward);
        float aspectRatio = resolution.y > 0.0f ? resolution.x / resolution.y : 1.0f;
        core::vec3 d = point - camera.position;
        float z = glm::dot(d, basis.forward);
        core::vec2 ndc;
        if (camera.orthographicHeight > 0.0f) {
            float halfHeight = camera.orthographicHeight * 0.5f;
            if (inFront) *inFront = z + camera.orthographicBackoff > 0.0f;
            ndc = core::vec2(glm::dot(d, basis.right) / (halfHeight * aspectRatio), glm::dot(d, basis.up) / halfHeight);
        } else {
            float scale = std::tan(glm::radians(camera.verticalFovDegrees * 0.5f));
            if (inFront) *inFront = z > 1e-4f;
            ndc = core::vec2(glm::dot(d, basis.right) / (z * scale * aspectRatio), glm::dot(d, basis.up) / (z * scale));
        }
        return core::vec2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
    }

    void setCamera(ConstructedRenderer& renderer, const CameraView& camera) {
        renderer.camera = camera;
    }

    std::array<core::vec4, 7> cameraUniformValues(const CameraView& camera, const CameraView& previous) {
        auto pos = [](const CameraView& c) { return core::vec4(c.position, c.verticalFovDegrees); };
        auto dir = [](const CameraView& c) { return core::vec4(c.forward, c.orthographicHeight); };
        auto lens = [](const CameraView& c) { return core::vec4(c.orthographicBackoff, c.nearPlane, c.farPlane, 0.0f); };
        return {pos(camera), dir(camera), lens(camera),
                core::vec4(camera.jitter, previous.jitter),
                pos(previous), dir(previous), lens(previous)};
    }
}
