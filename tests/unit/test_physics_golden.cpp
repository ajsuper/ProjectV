// Determinism across builds (the physics plan, R3 and D6): one session, recorded once and committed
// (data/physics_golden.rec), must replay bit for bit on every build that runs these tests -- debug
// or release, another compiler, another CPU or OS. Within one build the replay tests elsewhere show
// the simulation is a function of its inputs; this is what shows the function is the same
// everywhere, which a server and its clients rest on.
//
// The session covers what makes and destroys bodies, not only what moves them: a stack, a wall that
// breaks on impact, a boulder through it, a strike, the pieces breaking again, a hinge, a character.
//
// The recording's format is PhysicsWorld's log: a change to it (LOG_VERSION) or to what the
// simulation does -- a Jolt upgrade, a new rule in the step -- makes this fail on purpose. Look at
// why; when the change is meant, regenerate the file on one machine and commit it:
//
//     PROJV_WRITE_GOLDEN=1 build/bin/projv_unit_tests -tc="golden*"

#include "doctest/doctest.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

#include "core/application.h"
#include "runtime/physics.h"
#include "runtime/physics/physics_world.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/scene_query.h"

namespace {
    using namespace projv;
    using core::ivec3;
    using core::quat;
    using core::vec3;
    constexpr double DT = 1.0 / 60.0;
    constexpr uint32_t GREY = 0x2AAAAAAAu, RED = 0x3FF00000u;

    std::filesystem::path goldenPath() { return std::filesystem::path(PROJV_TEST_DATA_DIR) / "physics_golden.rec"; }

    ComponentHandle block(Scene& scene, const char* name, vec3 corner, ivec3 size, float voxel, uint32_t colour) {
        ComponentHandle h = utils::addComponent(scene, ComponentKind::Chunk, name, INVALID_COMPONENT_HANDLE, 16, voxel);
        utils::setComponentTransform(scene, h, corner, quat(1, 0, 0, 0), 1.0f);
        std::vector<PendingVoxelOp> ops;
        for (int z = 0; z < size.z; z++)
            for (int y = 0; y < size.y; y++)
                for (int x = 0; x < size.x; x++) ops.push_back({true, ivec3(x, y, z), colour});
        utils::queueVoxelAdd(scene, h, ops);
        utils::updateScene(scene);
        return h;
    }

    // The session, scripted by step: the same on every build up to the floats Jolt computes.
    runtime::PhysicsLog record() {
        Application app;
        Scene& scene = app.world.ctx().emplace<Scene>();
        double now = 0.0;
        app.clock = [&now] { return now; };
        runtime::installSceneBridge(app);
        runtime::PhysicsConfig config;
        config.settings.record = true;
        runtime::installPhysics(app, config);
        runtime::installImpactFracture(app);
        app.runStartup();
        app.runFrame();
        World& world = app.world;
        // What happened, checked at the end, so the session stays the one this file says it is.
        int impacts = 0, strikes = 0, pieces = 0;
        app.events().on<BodyBroken>([&](const BodyBroken& e) {
            impacts += e.cause == BreakCause::Impact;
            strikes += e.cause == BreakCause::Strike;
            pieces += int(e.pieces.size());
        });
        auto frames = [&](int n) { for (int i = 0; i < n; i++) { now += DT; app.runFrame(); } };

        runtime::PhysicsMaterial stone{2400.0f, 0.7f, 0.0f};
        stone.strength = 2e4f;
        stone.shardSize = 0.5f;
        runtime::definePhysicsMaterial(world, "stone", stone);

        // A floor, and a stone wall that splits and breaks on impact, standing on it.
        ComponentHandle floor = block(scene, "floor", vec3(-10, -1, -10), ivec3(40, 2, 40), 0.5f, GREY);
        Entity floorEntity = runtime::spawnComponent(world, floor);
        world.emplace<StaticCollider>(floorEntity);
        ComponentHandle wall = block(scene, "wall", vec3(-2, 0, 2), ivec3(16, 12, 2), 0.25f, RED);
        for (size_t s = 0; s < scene.components[wall].materialPalette.size(); s++) utils::setMaterialPhysics(scene, wall, uint8_t(s), "stone");
        Entity wallEntity = runtime::spawnComponent(world, wall);
        StaticCollider collider;
        collider.splits = true;
        collider.anchorBase = true;
        collider.breaksOnImpact = true;
        world.emplace<StaticCollider>(wallEntity, collider);
        runtime::assignNetId(world, wallEntity);

        // A stack of five crates beside it.
        for (int i = 0; i < 5; i++) {
            ComponentHandle crate = block(scene, "crate", vec3(4.0f, 0.02f + float(i) * 0.52f, -1.0f), ivec3(2), 0.25f, GREY);
            Entity e = runtime::spawnComponent(world, crate, LinkMode::Root, OnUnlink::Destroy);
            world.emplace<RigidBody>(e);
            runtime::assignNetId(world, e);
        }
        // A plank on a hinge to the world, its motor turning it.
        ComponentHandle plank = block(scene, "plank", vec3(-6, 1, -1), ivec3(8, 1, 2), 0.25f, GREY);
        Entity plankEntity = runtime::spawnComponent(world, plank, LinkMode::Root, OnUnlink::Destroy);
        world.emplace<RigidBody>(plankEntity);
        NetId plankId = runtime::assignNetId(world, plankEntity);
        Entity hinge = world.create();
        Joint joint;
        joint.type = runtime::JointType::Hinge;
        joint.a = plankId;
        joint.anchor = vec3(-6, 1.125f, -0.75f);
        joint.axis = vec3(0, 0, 1);
        joint.motor = true;
        joint.motorVelocity = 1.5f;
        joint.motorLimit = 2000.0f;
        world.emplace<Joint>(hinge, joint);
        // A character, walking.
        Entity walker = world.create();
        world.emplace<Transform>(walker, Transform{vec3(0, 0.1f, -4)});
        world.emplace<NetId>(walker, NetId{900});
        world.emplace<CharacterController>(walker);
        frames(30);
        runtime::setCharacterInput(world, walker, vec3(1.5f, 0, 0));
        const float startX = runtime::characterState(world, walker).position.x;

        // A heavy ball through the wall: it breaks on impact, the pieces fall, and break again.
        ComponentHandle ball = block(scene, "ball", vec3(-0.4f, 1.0f, -4.0f), ivec3(4), 0.2f, GREY);
        Entity ballEntity = runtime::spawnComponent(world, ball, LinkMode::Root, OnUnlink::Destroy);
        RigidBody rigid;
        rigid.shape = RigidBody::Shape::Sphere;
        rigid.radius = 0.4f;
        rigid.mass = 800.0f;
        rigid.quality = RigidBody::Quality::Continuous;
        world.emplace<RigidBody>(ballEntity, rigid);
        runtime::assignNetId(world, ballEntity);
        frames(1);
        runtime::setVelocity(world, ballEntity, vec3(0, 1, 18), vec3(0));
        frames(60);
        // A strike near the top: what it leaves hanging falls (support).
        runtime::fracture(world, wallEntity, vec3(2.0f, 2.5f, 2.0f), 4e4f, vec3(0, 0, 2));
        // A crate knocked over.
        frames(20);
        runtime::addImpulse(world, ballEntity, vec3(0, 0, -2000));
        frames(120);
        CHECK(impacts >= 1);   // the ball broke the wall
        CHECK(strikes == 1);
        CHECK(pieces > 10);
        CHECK(runtime::characterState(world, walker).position.x > startX + 3.0f);   // it walked
        CHECK(runtime::jointState(world, hinge).alive);
        return runtime::physicsWorld(world).recording();
    }
}

TEST_CASE("golden: a session recorded on one build replays bit for bit on this one") {
    if (std::getenv("PROJV_WRITE_GOLDEN")) {
        const runtime::PhysicsLog recording = record();
        std::filesystem::create_directories(goldenPath().parent_path());
        REQUIRE(recording.saveToFile(goldenPath().string()));
        MESSAGE("wrote " << goldenPath().string() << ", " << recording.bytes.size() << " bytes, " << recording.steps << " steps");
        CHECK(runtime::PhysicsWorld::replay(recording).matched);   // and it replays here, at least
        return;
    }
    runtime::PhysicsLog recording;
    REQUIRE_MESSAGE(runtime::PhysicsLog::loadFromFile(goldenPath().string(), recording),
                    "no golden recording at " << goldenPath().string() << ": see the top of this file");
    const runtime::ReplayResult result = runtime::PhysicsWorld::replay(recording);
    CAPTURE(result.problem);
    CAPTURE(result.firstDivergentStep);
    CHECK(result.matched);
    CHECK(result.stepsReplayed > 200);
}

TEST_CASE("golden: the session it was recorded from is still the same session") {
    // The script, run again here, must make the same recording byte for byte: if it does not, what
    // the engine does with the same calls has changed (or this file has), and the golden file is
    // out of date even if it still replays.
    if (std::getenv("PROJV_WRITE_GOLDEN")) return;
    runtime::PhysicsLog golden;
    REQUIRE(runtime::PhysicsLog::loadFromFile(goldenPath().string(), golden));
    const runtime::PhysicsLog now = record();
    CHECK(now.bytes.size() == golden.bytes.size());
    CHECK(now.bytes == golden.bytes);
}
