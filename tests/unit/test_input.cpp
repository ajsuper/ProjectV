// Input: per-frame key and button edges (graphics/input.h). No window: the platform system is what
// calls setKey, and here the test does.

#include "doctest/doctest.h"

#include "graphics/input.h"

TEST_CASE("pressed and released are true for exactly the frame the change is seen") {
    projv::Input input;
    CHECK_FALSE(input.down(projv::Key::W));

    input.setKey(projv::Key::W, true);
    CHECK(input.down(projv::Key::W));
    CHECK(input.pressed(projv::Key::W));
    CHECK_FALSE(input.released(projv::Key::W));

    input.setKey(projv::Key::W, true);           // held
    CHECK(input.down(projv::Key::W));
    CHECK_FALSE(input.pressed(projv::Key::W));

    input.setKey(projv::Key::W, false);
    CHECK_FALSE(input.down(projv::Key::W));
    CHECK(input.released(projv::Key::W));

    input.setKey(projv::Key::W, false);
    CHECK_FALSE(input.released(projv::Key::W));
}

TEST_CASE("keys and mouse buttons are independent") {
    projv::Input input;
    input.setKey(projv::Key::Space, true);
    input.setMouse(projv::MouseButton::Right, true);
    CHECK(input.down(projv::Key::Space));
    CHECK_FALSE(input.down(projv::Key::Enter));
    CHECK(input.pressed(projv::MouseButton::Right));
    CHECK_FALSE(input.down(projv::MouseButton::Left));
}
