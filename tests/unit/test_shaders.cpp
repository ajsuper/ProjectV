// The shader library's mechanisms that run without a GPU: the embedded engine shaders' table, and the
// capture tolerance its milestones are checked by (docs/plans/shader-library.md, D3 and D5).

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "graphics/engine_shaders.h"
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
