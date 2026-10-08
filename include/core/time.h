#ifndef PROJECTV_TIME_H
#define PROJECTV_TIME_H

#include <cstdint>

namespace projv {
    // The application's clock, as an engine resource: `app.time()`, or
    // `world.ctx().get<projv::Time>()` from anywhere that has the world.
    //
    // Two clocks run off it. The frame clock gives `delta` once per frame, for anything that should
    // move smoothly at whatever rate the display runs. The fixed clock runs FixedUpdate zero or more
    // times per frame at exactly `fixedDelta` each, for anything whose result must not depend on
    // the frame rate -- physics above all. `fixedAlpha` is how far the frame has got into the next
    // fixed step, which is what a renderer interpolates by so a 60 Hz simulation does not stutter on
    // a 144 Hz display.
    struct Time {
        // ---- Settings ----------------------------------------------------------------------
        float    scale = 1.0f;                 // 0 pauses Update's delta and FixedUpdate; unscaledDelta keeps running
        float    fixedDelta = 1.0f / 60.0f;
        // A frame longer than this is treated as this long. A debugger pause, a window drag or a
        // load hitch would otherwise arrive as one enormous step.
        float    maxDelta = 0.25f;
        // The spiral-of-death guard: a frame that owes more fixed steps than this runs this many
        // and drops the rest, reported in `droppedFixedSteps`, rather than falling further behind
        // each frame trying to catch up.
        int      maxFixedStepsPerFrame = 8;

        // ---- State, written by advance() --------------------------------------------------------
        double   elapsed = 0.0;                // scaled seconds since the first frame
        float    delta = 0.0f;                 // this frame, scaled and clamped
        float    unscaledDelta = 0.0f;         // this frame, clamped, not scaled
        float    fixedAlpha = 0.0f;            // accumulated time / fixedDelta, in [0, 1)
        uint64_t frame = 0;                    // frames advanced so far
        uint64_t fixedStep = 0;                // fixed steps run so far; the Application counts them
        uint64_t droppedFixedSteps = 0;        // fixed steps discarded by the guard, in total

        // Moves the clock to `nowSeconds` and returns how many fixed steps this frame owes. The first
        // call only sets the starting point: a frame needs two timestamps to have a length.
        //
        // The Application calls this once per frame with its clock. Tests call it with exact values.
        int advance(double nowSeconds);

    private:
        double lastSeconds = 0.0;
        double accumulator = 0.0;
        bool   started = false;
    };
}

#endif
