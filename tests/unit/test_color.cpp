// The colour convention's storage half (docs/plans/color-convention.md): a palette colour is sRGB, 8 bits
// widened to 10. packColor must reach both ends of the 10-bit range and unpackColor must undo it exactly,
// or a colour drifts each time a tool reads and rewrites a palette.

#include "doctest/doctest.h"

#include <cstdlib>

#include "data_structures/voxel.h"

TEST_CASE("packColor widens 0..255 to the whole 0..1023 range") {
    CHECK(projv::packColor({0, 0, 0}) == 0u);
    CHECK(projv::packColor({255, 255, 255}) == ((1023u << 20) | (1023u << 10) | 1023u));
    CHECK(projv::packColor({255, 0, 0}) == (1023u << 20));
    CHECK(projv::packColor({0, 255, 0}) == (1023u << 10));
    CHECK(projv::packColor({0, 0, 255}) == 1023u);
}

TEST_CASE("unpackColor inverts packColor for every byte") {
    for (int v = 0; v < 256; ++v) {
        const auto b = static_cast<uint8_t>(v);
        const projv::Color c = projv::unpackColor(projv::packColor({b, b, b}));
        CHECK(c.r == b);
        CHECK(c.g == b);
        CHECK(c.b == b);
    }
}

// Palettes on disk from before the fix hold byte x 4 (white = 1020). Those still read back within one
// step of the byte they were written from; the GPU reads the 10-bit value itself, so nothing shifts there.
TEST_CASE("unpackColor reads palettes written the old way (byte x 4) to within one step") {
    for (int v = 0; v < 256; ++v) {
        const uint32_t old = uint32_t(v) * 4u;
        const projv::Color c = projv::unpackColor((old << 20) | (old << 10) | old);
        CHECK(std::abs(int(c.r) - v) <= 1);
    }
}
