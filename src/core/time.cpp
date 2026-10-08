#include "core/time.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"

namespace projv {
    int Time::advance(double nowSeconds) {
        if (!started) {
            started = true;
            lastSeconds = nowSeconds;
            delta = unscaledDelta = 0.0f;
            fixedAlpha = 0.0f;
            frame++;
            return 0;
        }

        // A clock that goes backwards (it should not, but a test or a replaced clock can) is a frame
        // of zero length rather than a negative one.
        double raw = std::max(0.0, nowSeconds - lastSeconds);
        lastSeconds = nowSeconds;

        // The arithmetic is double throughout and only the published fields are float: a frame
        // length that went through float on its way into the accumulator would drift against the
        // real clock over a long session.
        double clamped = std::min(raw, static_cast<double>(maxDelta));
        double scaled = clamped * static_cast<double>(std::max(0.0f, scale));
        unscaledDelta = static_cast<float>(clamped);
        delta = static_cast<float>(scaled);
        elapsed += scaled;
        frame++;

        if (fixedDelta <= 0.0f) {
            fixedAlpha = 0.0f;
            return 0;
        }

        accumulator += scaled;
        double step = static_cast<double>(fixedDelta);
        // Within a hundred-thousandth of a step counts as reaching it. fixedDelta is a float, so
        // 0.05f is a little more than 0.05 and a quarter-second frame is a little *less* than five
        // of them; without the tolerance it would floor to four, and a frame of exactly fixedDelta
        // would sometimes run no step and the next one two. The overshoot -- at most a
        // hundred-thousandth of a step -- is forgiven rather than carried as a negative remainder.
        int owed = static_cast<int>(std::floor(accumulator / step + 1e-5));
        int steps = std::min(owed, std::max(0, maxFixedStepsPerFrame));
        accumulator -= static_cast<double>(owed) * step;
        if (accumulator < 0.0) accumulator = 0.0;

        if (owed > steps) {
            uint64_t dropped = static_cast<uint64_t>(owed - steps);
            droppedFixedSteps += dropped;
            core::perf("Time: frame owed {} fixed steps, ran {} and dropped {}", owed, steps, dropped);
        }

        fixedAlpha = static_cast<float>(accumulator / step);
        return steps;
    }
}
