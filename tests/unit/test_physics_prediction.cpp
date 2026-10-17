// What networking asks of the simulation (networking.md, N0): a server's correction of a body as one
// change, a frozen body moved as a proxy, and a character saved, restored and re-run alone.

#include "doctest/doctest.h"

#include <vector>

#include "runtime/physics/physics_world.h"

using namespace projv::runtime;
using projv::core::quat;
using projv::core::vec3;

namespace {
    constexpr float DT = 1.0f / 60.0f;

    PhysicsSettings recorded() {
        PhysicsSettings s;
        s.record = true;
        return s;
    }
    BodyId floorIn(PhysicsWorld& physics) {
        BodyDesc floor;
        floor.shape = PhysicsShape::box({50.0f, 1.0f, 50.0f});
        floor.motion = MotionType::Static;
        floor.position = vec3(0.0f, -1.0f, 0.0f);
        return physics.createBody(floor);
    }
    BodyId boxIn(PhysicsWorld& physics, vec3 at) {
        BodyDesc box;
        box.shape = PhysicsShape::box({0.5f, 0.5f, 0.5f});
        box.motion = MotionType::Dynamic;
        box.position = at;
        return physics.createBody(box);
    }
}

TEST_CASE("prediction: setBodyState puts a body where and how fast it is told, as one logged change") {
    PhysicsWorld physics(recorded());
    floorIn(physics);
    BodyId box = boxIn(physics, vec3(0, 0.5f, 0));
    for (int i = 0; i < 60; i++) physics.step(DT);
    REQUIRE_FALSE(physics.bodyState(box).awake);

    const quat turned = glm::angleAxis(0.3f, vec3(0, 1, 0));
    physics.setBodyState(box, vec3(2, 3, 1), turned, vec3(1, 0, 0), vec3(0, 2, 0));
    BodyState now = physics.bodyState(box);
    CHECK(now.awake);
    CHECK(glm::distance(now.position, vec3(2, 3, 1)) < 1e-5f);
    CHECK(std::abs(glm::dot(now.rotation, turned)) > 1.0f - 1e-6f);
    CHECK(now.linearVelocity == vec3(1, 0, 0));
    CHECK(now.angularVelocity == vec3(0, 2, 0));

    // A frozen body takes the pose and stays still.
    physics.setFrozen(box, true);
    physics.setBodyState(box, vec3(4, 3, 1), turned, vec3(5, 0, 0), vec3(0));
    for (int i = 0; i < 10; i++) physics.step(DT);
    CHECK(glm::distance(physics.bodyState(box).position, vec3(4, 3, 1)) < 1e-5f);
    physics.setFrozen(box, false);
    for (int i = 0; i < 30; i++) physics.step(DT);
    CHECK(PhysicsWorld::replay(physics.recording()).matched);
}

TEST_CASE("prediction: a frozen body follows the poses it is given, and what it meets is pushed") {
    PhysicsWorld physics;
    floorIn(physics);
    BodyId proxy = boxIn(physics, vec3(-3, 0.5f, 0));
    BodyId other = boxIn(physics, vec3(0, 0.5f, 0));
    for (int i = 0; i < 30; i++) physics.step(DT);
    physics.setFrozen(proxy, true);
    // A server says it moves 4 m along x over a second.
    for (int i = 1; i <= 60; i++) {
        physics.moveKinematic(proxy, vec3(-3.0f + 4.0f * float(i) / 60.0f, 0.5f, 0), quat(1, 0, 0, 0), DT);
        physics.step(DT);
    }
    CHECK(physics.bodyState(proxy).position.x == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(physics.bodyState(other).position.x > 1.5f);   // shoved ahead of it
}

TEST_CASE("prediction: a character restored and re-run alone ends where it did, and pushes nothing again") {
    PhysicsWorld physics(recorded());
    floorIn(physics);
    BodyDesc light;
    light.shape = PhysicsShape::box({0.5f, 0.5f, 0.5f});
    light.motion = MotionType::Dynamic;
    light.position = vec3(3.0f, 0.5f, 0.0f);
    light.mass = 10.0f;   // light enough for a character to shove
    BodyId crate = physics.createBody(light);
    CharacterDesc desc;
    desc.position = vec3(0, 0, 0);
    CharacterId walker = physics.createCharacter(desc);
    for (int i = 0; i < 30; i++) physics.step(DT);

    // The inputs of ticks A+1 .. A+90: walking into the crate, then a jump.
    std::vector<vec3> inputs;
    for (int i = 0; i < 90; i++) inputs.push_back(vec3(2.0f, 0, i < 70 ? 0.0f : 0.5f));
    const std::vector<uint8_t> atA = physics.saveCharacter(walker);
    REQUIRE_FALSE(atA.empty());
    for (size_t i = 0; i < inputs.size(); i++) {
        physics.setCharacterInput(walker, inputs[i], i == 75);
        physics.step(DT);
    }
    const vec3 ended = physics.characterState(walker).position;
    const vec3 crateAfter = physics.bodyState(crate).position;
    CHECK(crateAfter.x > 3.1f);   // it was pushed

    // A correction: back to A, and the same inputs again, the character alone. Nothing steps, so an
    // impulse it gave would show in the crate's velocity.
    const BodyState crateBefore = physics.bodyState(crate);
    REQUIRE(physics.restoreCharacter(walker, atA));
    CHECK(physics.characterState(walker).position.x < 0.1f);
    for (size_t i = 0; i < inputs.size(); i++) {
        physics.setCharacterInput(walker, inputs[i], i == 75);
        physics.stepCharacterAlone(walker, DT);
    }
    const vec3 again = physics.characterState(walker).position;
    // Against a crate already moved, so near -- not bit-equal -- where it ended.
    CHECK(glm::distance(again, ended) < 0.25f);
    CHECK(physics.bodyState(crate).position == crateAfter);
    CHECK(physics.bodyState(crate).linearVelocity == crateBefore.linearVelocity);   // not pushed a second time
    CHECK(physics.bodyState(crate).angularVelocity == crateBefore.angularVelocity);

    // Restore refuses another character's state.
    CharacterDesc second;
    second.position = vec3(-5, 0, 0);
    CharacterId other = physics.createCharacter(second);
    CHECK_FALSE(physics.restoreCharacter(other, atA));
    for (int i = 0; i < 5; i++) physics.step(DT);
    CHECK(PhysicsWorld::replay(physics.recording()).matched);
}

TEST_CASE("prediction: on open ground the re-run is exact") {
    PhysicsWorld physics;
    floorIn(physics);
    CharacterDesc desc;
    CharacterId walker = physics.createCharacter(desc);
    for (int i = 0; i < 30; i++) physics.step(DT);
    const std::vector<uint8_t> atA = physics.saveCharacter(walker);
    for (int i = 0; i < 50; i++) {
        physics.setCharacterInput(walker, vec3(1.5f, 0, -0.5f), i == 10);
        physics.step(DT);
    }
    const vec3 ended = physics.characterState(walker).position;
    REQUIRE(physics.restoreCharacter(walker, atA));
    for (int i = 0; i < 50; i++) {
        physics.setCharacterInput(walker, vec3(1.5f, 0, -0.5f), i == 10);
        physics.stepCharacterAlone(walker, DT);
    }
    CHECK(physics.characterState(walker).position == ended);
}
