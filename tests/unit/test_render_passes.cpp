// A renderer's passes turned on and off by name, and its render scale (graphics/perform_renderer.h):
// what a program's quality presets are made of. CPU only: no pass is drawn.
#include <doctest/doctest.h>

#include "graphics/perform_renderer.h"

using projv::BGFXDependencyGraph;
using projv::ConstructedRenderer;
namespace graphics = projv::graphics;

TEST_CASE("passes are turned off and on by name") {
    ConstructedRenderer renderer;
    for (const char* name : {"gbuffer", "dof", "bloom_down1", "bloom_down2"}) {
        BGFXDependencyGraph pass;
        pass.name = name;
        renderer.dependencyGraph.push_back(pass);
    }
    CHECK(graphics::setPassEnabled(renderer, "dof", false) == 1);
    CHECK_FALSE(renderer.dependencyGraph[1].enabled);
    CHECK(renderer.dependencyGraph[0].enabled);
    CHECK(renderer.dependencyGraph[2].enabled);
    CHECK(graphics::setPassEnabled(renderer, "bloom", false) == 0);   // whole names only
    CHECK(graphics::setPassEnabled(renderer, "dof", true) == 1);
    CHECK(renderer.dependencyGraph[1].enabled);
}

TEST_CASE("a render scale sizes the targets, never below one pixel") {
    CHECK(graphics::renderTargetSize({1600, 900}, 1.0f) == projv::core::ivec2(1600, 900));
    CHECK(graphics::renderTargetSize({1600, 900}, 0.5f) == projv::core::ivec2(800, 450));
    CHECK(graphics::renderTargetSize({1601, 901}, 0.5f) == projv::core::ivec2(801, 451));   // rounded, not cut
    CHECK(graphics::renderTargetSize({3, 1}, 0.1f) == projv::core::ivec2(1, 1));
    CHECK(graphics::renderTargetSize({1600, 900}, 0.0f) == projv::core::ivec2(1600, 900));  // unset: whole
}
