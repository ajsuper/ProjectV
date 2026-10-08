// Time: the frame clock and the fixed-step accumulator (core/time.h).

#include "doctest/doctest.h"

#include "core/time.h"

TEST_CASE("the first advance only sets the starting point") {
    projv::Time time;
    CHECK(time.advance(100.0) == 0);
    CHECK(time.delta == 0.0f);
    CHECK(time.frame == 1);
    CHECK(time.elapsed == 0.0);
}

TEST_CASE("a fixed step runs once per fixedDelta of accumulated time") {
    projv::Time time;
    time.fixedDelta = 0.1f;
    time.advance(0.0);

    CHECK(time.advance(0.05) == 0);      // half a step owed
    CHECK(time.fixedAlpha == doctest::Approx(0.5));
    CHECK(time.advance(0.10) == 1);      // the half carries over
    CHECK(time.fixedAlpha == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(time.advance(0.35) == 2);      // 0.25 more: two steps, half left
    CHECK(time.fixedAlpha == doctest::Approx(0.5));
    CHECK(time.elapsed == doctest::Approx(0.35));
}

TEST_CASE("frames of exactly fixedDelta run exactly one step each") {
    // The case floating-point drift breaks: 1/60 accumulated sixty times must not come out a hair
    // short and run no step on some frame and two on the next.
    projv::Time time;
    time.fixedDelta = 1.0f / 60.0f;
    double now = 0.0;
    time.advance(now);
    int total = 0;
    for (int frame = 0; frame < 600; frame++) {
        now += 1.0 / 60.0;
        int steps = time.advance(now);
        CHECK(steps == 1);
        total += steps;
    }
    CHECK(total == 600);
}

TEST_CASE("a long frame is clamped to maxDelta") {
    projv::Time time;
    time.maxDelta = 0.25f;
    time.fixedDelta = 0.05f;
    time.advance(0.0);
    int steps = time.advance(30.0);      // a 30-second debugger pause
    CHECK(time.unscaledDelta == doctest::Approx(0.25));
    CHECK(time.delta == doctest::Approx(0.25));
    CHECK(steps == 5);
}

TEST_CASE("steps beyond maxFixedStepsPerFrame are dropped and counted") {
    projv::Time time;
    time.maxDelta = 10.0f;
    time.fixedDelta = 0.01f;
    time.maxFixedStepsPerFrame = 8;
    time.advance(0.0);
    CHECK(time.advance(0.2) == 8);       // owed 20
    CHECK(time.droppedFixedSteps == 12);
    // Dropped means gone: the next short frame does not try to catch up.
    CHECK(time.advance(0.205) == 0);
}

TEST_CASE("scale 0 pauses delta and the fixed clock, not the unscaled clock") {
    projv::Time time;
    time.fixedDelta = 0.1f;
    time.advance(0.0);
    time.scale = 0.0f;
    CHECK(time.advance(1.0) == 0);
    CHECK(time.delta == 0.0f);
    CHECK(time.unscaledDelta == doctest::Approx(0.25));   // clamped, but still running
    CHECK(time.elapsed == 0.0);

    time.scale = 2.0f;
    CHECK(time.advance(1.1) == 2);       // 0.1 real seconds at double speed
    CHECK(time.delta == doctest::Approx(0.2));
}

TEST_CASE("a clock that goes backwards is a zero-length frame") {
    projv::Time time;
    time.advance(5.0);
    CHECK(time.advance(4.0) == 0);
    CHECK(time.delta == 0.0f);
    CHECK(time.advance(4.5) >= 0);
    CHECK(time.delta == doctest::Approx(0.25));           // measured from 4.0, clamped
}
