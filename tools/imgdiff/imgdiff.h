#ifndef PROJV_TOOLS_IMGDIFF_H
#define PROJV_TOOLS_IMGDIFF_H

// The capture comparison the shader library's milestones are checked by (docs/plans/shader-library.md,
// D5). Header-only so the unit tests check the measure itself; tools/imgdiff/main.cpp is the CLI.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace projv::imgdiff {
    // A capture passes when, over its colour channels (alpha is ignored: a screenshot's is not the image):
    struct Limits {
        double meanAbsolute = 0.001;   // mean |a - b| as a fraction of 255: 0.1%
        int    stepTolerance = 2;      // a channel differing by more than this many 8-bit steps...
        double overFraction = 0.001;   // ...counts its pixel, and at most 0.1% of pixels may count
    };

    struct Result {
        double meanAbsolute = 0.0;     // fraction of 255
        double overFraction = 0.0;     // fraction of pixels with a channel over stepTolerance
        int    largest = 0;            // largest single channel difference, in steps; reported only
        bool   pass = false;
    };

    // a and b: `pixels` pixels of `channels` bytes each (3 or 4; the first three are compared).
    inline Result compare(std::span<const uint8_t> a, std::span<const uint8_t> b, size_t pixels,
                          int channels, const Limits& limits = {}) {
        Result result;
        if (pixels == 0 || a.size() < pixels * channels || b.size() < pixels * channels) {
            return result;
        }
        const int compared = std::min(channels, 3);
        uint64_t sum = 0;
        size_t over = 0;
        for (size_t p = 0; p < pixels; ++p) {
            bool pixelOver = false;
            for (int c = 0; c < compared; ++c) {
                const int d = std::abs(int(a[p * channels + c]) - int(b[p * channels + c]));
                sum += uint64_t(d);
                result.largest = std::max(result.largest, d);
                pixelOver |= d > limits.stepTolerance;
            }
            over += pixelOver ? 1 : 0;
        }
        result.meanAbsolute = double(sum) / (double(pixels) * compared * 255.0);
        result.overFraction = double(over) / double(pixels);
        result.pass = result.meanAbsolute <= limits.meanAbsolute && result.overFraction <= limits.overFraction;
        return result;
    }
}

#endif
