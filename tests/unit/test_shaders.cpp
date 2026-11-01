// The shader library's mechanisms that run without a GPU: the embedded engine shaders' table, the
// capture tolerance its milestones are checked by, and the CPU camera model (docs/plans/shader-library.md,
// D3, D5 and D4).

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "graphics/camera.h"
#include "graphics/engine_shaders.h"
#include "utils/picking.h"
#include "../../tools/imgdiff/imgdiff.h"

TEST_CASE("engine shaders: vs_quad is embedded for the platform's renderer") {
#if defined(__APPLE__)
    const bgfx::RendererType::Enum type = bgfx::RendererType::Metal;
#else
    const bgfx::RendererType::Enum type = bgfx::RendererType::Vulkan;
#endif
    std::span<const uint8_t> bytes = projv::graphics::engineShader("vs_quad", type);
    REQUIRE(bytes.size() > 16);
    // A bgfx shader binary starts with its magic: 'V' 'S' 'H' and a version.
    CHECK(std::memcmp(bytes.data(), "VSH", 3) == 0);

    CHECK(projv::graphics::engineShader("no_such_shader", type).empty());
    CHECK(projv::graphics::engineShader("vs_quad", bgfx::RendererType::Noop).empty());
}

TEST_CASE("imgdiff: the D5 limits") {
    const size_t pixels = 10000;
    std::vector<uint8_t> a(pixels * 4, 128), b = a;

    SUBCASE("identical") {
        projv::imgdiff::Result r = projv::imgdiff::compare(a, b, pixels, 4);
        CHECK(r.pass);
        CHECK(r.largest == 0);
    }
    SUBCASE("alpha is ignored") {
        for (size_t p = 0; p < pixels; ++p) b[p * 4 + 3] = 0;
        CHECK(projv::imgdiff::compare(a, b, pixels, 4).pass);
    }
    SUBCASE("one step on one channel of every pixel is a shift, and fails the mean") {
        for (size_t p = 0; p < pixels; ++p) b[p * 4] += 1;   // mean 1/(3*255) = 0.13%
        projv::imgdiff::Result r = projv::imgdiff::compare(a, b, pixels, 4);
        CHECK(r.overFraction == 0.0);
        CHECK(r.meanAbsolute == doctest::Approx(1.0 / (3.0 * 255.0)));
        CHECK_FALSE(r.pass);   // 0.13% > 0.1%: a whole-image shift is a change, however small
    }
    SUBCASE("a few flipped pixels pass, and the largest is reported") {
        for (size_t p = 0; p < 10; ++p) b[p * 4 + 1] = 255;   // 0.1% of pixels
        projv::imgdiff::Result r = projv::imgdiff::compare(a, b, pixels, 4);
        CHECK(r.pass);
        CHECK(r.largest == 127);
    }
    SUBCASE("more than one pixel in a thousand off by more than two steps fails") {
        for (size_t p = 0; p < 11; ++p) b[p * 4 + 2] = 131;
        CHECK_FALSE(projv::imgdiff::compare(a, b, pixels, 4).pass);
    }
    SUBCASE("mismatched sizes do not pass") {
        CHECK_FALSE(projv::imgdiff::compare(a, std::vector<uint8_t>(4), pixels, 4).pass);
    }
}

// --- The camera model (S2) ----------------------------------------------------------------------

namespace {
    projv::CameraView testView(projv::core::vec3 forward) {
        projv::CameraView view;
        view.position = projv::core::vec3(3.0f, -2.0f, 7.5f);
        view.forward = forward;
        return view;
    }
}

TEST_CASE("camera: the ray matches the picking ray the engine has always used") {
    const projv::core::vec2 resolution(1280.0f, 720.0f);
    for (projv::core::vec3 forward : {projv::core::vec3(0.3f, -0.2f, 1.0f), projv::core::vec3(-1.0f, 0.0f, 0.0f),
                                      projv::core::vec3(0.0f, 1.0f, 0.0f), projv::core::vec3(0.001f, -1.0f, 0.0005f)}) {
        projv::CameraView view = testView(forward);
        for (projv::core::vec2 uv : {projv::core::vec2(0.5f), projv::core::vec2(0.0f), projv::core::vec2(0.9f, 0.13f)}) {
            projv::core::vec3 a = projv::graphics::cameraRayDirection(view, uv, resolution);
            projv::core::vec3 b = projv::utils::rayDirectionThroughImage(uv, resolution, forward, 60.0f);
            CHECK(glm::length(a - b) < 1e-6f);
            CHECK(projv::graphics::cameraRayOrigin(view, uv, resolution) == view.position);
        }
    }
}

TEST_CASE("camera: world to uv inverts the ray, perspective and orthographic") {
    const projv::core::vec2 resolution(800.0f, 600.0f);
    for (float orthoHeight : {0.0f, 12.0f}) {
        projv::CameraView view = testView(projv::core::vec3(0.4f, -0.3f, 0.8f));
        view.verticalFovDegrees = 45.0f;
        view.orthographicHeight = orthoHeight;
        view.orthographicBackoff = 5.0f;
        for (projv::core::vec2 uv : {projv::core::vec2(0.5f), projv::core::vec2(0.1f, 0.85f), projv::core::vec2(0.97f, 0.02f)}) {
            projv::core::vec3 point = projv::graphics::cameraRayOrigin(view, uv, resolution) +
                                      projv::graphics::cameraRayDirection(view, uv, resolution) * 20.0f;
            bool inFront = false;
            projv::core::vec2 back = projv::graphics::cameraWorldToUV(view, point, resolution, &inFront);
            CHECK(inFront);
            CHECK(back.x == doctest::Approx(uv.x).epsilon(1e-4));
            CHECK(back.y == doctest::Approx(uv.y).epsilon(1e-4));
        }
        bool inFront = true;
        projv::graphics::cameraWorldToUV(view, view.position - glm::normalize(view.forward) * 50.0f, resolution, &inFront);
        CHECK_FALSE(inFront);
    }
}

TEST_CASE("camera: the uniforms are laid out as pjv_engine.sc declares them") {
    projv::CameraView now = testView(projv::core::vec3(0.0f, 0.0f, 2.0f));
    now.verticalFovDegrees = 50.0f;
    now.orthographicHeight = 4.0f;
    now.orthographicBackoff = 1.5f;
    now.nearPlane = 0.1f;
    now.farPlane = 900.0f;
    now.jitter = projv::core::vec2(0.25f, -0.5f);
    projv::CameraView before = testView(projv::core::vec3(1.0f, 0.0f, 0.0f));
    before.jitter = projv::core::vec2(0.125f, 0.375f);

    auto u = projv::graphics::cameraUniformValues(now, before);
    CHECK(u[0] == projv::core::vec4(now.position, 50.0f));           // pjvCameraPos
    CHECK(u[1] == projv::core::vec4(0.0f, 0.0f, 2.0f, 4.0f));        // pjvCameraDir, as set
    CHECK(u[2] == projv::core::vec4(1.5f, 0.1f, 900.0f, 0.0f));      // pjvCameraLens
    CHECK(u[3] == projv::core::vec4(0.25f, -0.5f, 0.125f, 0.375f));  // pjvCameraJitter
    CHECK(u[4] == projv::core::vec4(before.position, 60.0f));        // pjvPrevCameraPos
    CHECK(u[5] == projv::core::vec4(1.0f, 0.0f, 0.0f, 0.0f));        // pjvPrevCameraDir
    CHECK(u[6] == projv::core::vec4(0.0f, 0.05f, 10000.0f, 0.0f));   // pjvPrevCameraLens
}
