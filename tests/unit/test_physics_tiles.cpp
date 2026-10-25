// Static voxels colliding as their tile tables (CollisionRepresentation::Tiles; the voxel collision
// shape plan, Phase 2). The rest of the suite runs on them with PROJV_COLLISION=tiles; these are what
// the representation changes: no content-box fallback, no seams on a floor, nothing through a thin
// wall, and contacts named by coarser cells when a body has too many chunks for voxel ids.

#include "doctest/doctest.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <vector>

#include "core/application.h"
#include "runtime/physics.h"
#include "utils/collision_geometry.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/scene_query.h"
#include "utils/tile_table.h"

namespace {
    using namespace projv::runtime;
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;
    using projv::utils::CollisionRepresentation;
    namespace utils = projv::utils;
    constexpr double DT = 1.0 / 60.0;

    std::vector<projv::PendingVoxelOp> fill(ivec3 low, ivec3 high, uint32_t colour = 0x3FFFFFFFu) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) ops.push_back({true, ivec3(x, y, z), colour});
        return ops;
    }

    struct Fixture {
        projv::Application app;
        projv::Scene& scene;
        double now = 0.0;
        explicit Fixture(CollisionRepresentation representation, uint32_t pieceBudget = 1024)
            : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            installSceneBridge(app);
            PhysicsConfig config;
            config.collision.representation = representation;
            config.collision.pieceBudget = pieceBudget;
            installPhysics(app, config);
            app.runStartup();
            app.runFrame();
        }
        void frames(int n) { for (int i = 0; i < n; i++) { now += DT; app.runFrame(); } }
        Entity solid(const std::vector<projv::PendingVoxelOp>& ops, vec3 at, float voxel, uint32_t resolution = 16) {
            ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "c", INVALID_COMPONENT_HANDLE, resolution, voxel);
            utils::setComponentTransform(scene, h, at, quat(1, 0, 0, 0), 1.0f);
            utils::queueVoxelAdd(scene, h, ops);
            utils::updateScene(scene);
            Entity e = spawnComponent(app.world, h);
            app.world.emplace<projv::StaticCollider>(e);
            return e;
        }
        Entity ball(vec3 at, float radius, projv::RigidBody rigid = {}) {
            Entity e = app.world.create();
            app.world.emplace<projv::Transform>(e, projv::Transform{at});
            rigid.shape = projv::RigidBody::Shape::Sphere;
            rigid.radius = radius;
            app.world.emplace<projv::RigidBody>(e, rigid);
            return e;
        }
        Entity box(vec3 at, vec3 half, projv::RigidBody rigid = {}) {
            Entity e = app.world.create();
            app.world.emplace<projv::Transform>(e, projv::Transform{at});
            rigid.shape = projv::RigidBody::Shape::Box;
            rigid.halfExtents = half;
            app.world.emplace<projv::RigidBody>(e, rigid);
            return e;
        }
        BodyState pose(Entity e) { return bodyPose(app.world, e); }
    };
}

TEST_CASE("tiles: a chunk too detailed for its box budget collides as its voxels, not its content box") {
    // A 2 m slab of 0.125 m voxels with a checker of holes in its top and a shaft down its middle: more
    // boxes than a budget of 64, so as boxes it is one solid box -- and a ball dropped down the shaft
    // lands on its lid. As tiles the shaft is there.
    std::vector<projv::PendingVoxelOp> ops;
    for (int z = 0; z < 16; z++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                if (x >= 6 && x <= 9 && z >= 6 && z <= 9) continue;              // the shaft, 0.5 m across
                if (y == 15 && (x + z) % 2) continue;                            // the checker
                ops.push_back({true, ivec3(x, y, z), 0x3FFFFFFFu});
            }
    for (CollisionRepresentation r : {CollisionRepresentation::Boxes, CollisionRepresentation::Tiles}) {
        CAPTURE(int(r));
        Fixture f(r, 64);
        f.solid(ops, vec3(-1, -2, -1), 0.125f);
        Entity ball = f.ball(vec3(0, 1, 0), 0.15f);
        f.frames(120);
        const float y = f.pose(ball).position.y;
        if (r == CollisionRepresentation::Boxes) CHECK(y > -0.1f);   // on the content box's top, at y = 0
        else CHECK(y < -2.0f);                                       // down the shaft and out of the bottom
    }
}

TEST_CASE("tiles: a box slides across a voxel floor of many chunks without catching on a seam") {
    // A 16 m floor of 0.25 m voxels in 16^3 chunks -- 4 x 4 chunks, a seam every 4 m -- and a box sliding
    // across it without friction: a catch at any seam, between voxels or chunks, takes speed or bumps it.
    for (CollisionRepresentation r : {CollisionRepresentation::Boxes, CollisionRepresentation::Tiles}) {
        CAPTURE(int(r));
        Fixture f(r);
        f.solid(fill({0, 0, 0}, {63, 1, 63}), vec3(-8, -0.5f, -8), 0.25f);
        projv::RigidBody rigid;
        rigid.friction = 0.0f;
        rigid.linearDamping = 0.0f;
        rigid.angularDamping = 0.0f;
        Entity crate = f.box(vec3(-7, 0.3f, 0.1f), vec3(0.3f), rigid);
        f.frames(30);
        setVelocity(f.app.world, crate, vec3(2, 0, 0));
        float slowest = 2.0f, highest = 0.0f;
        for (int i = 0; i < 6 * 60; i++) {
            f.frames(1);
            const BodyState s = f.pose(crate);
            slowest = std::min(slowest, s.linearVelocity.x);
            highest = std::max(highest, std::abs(s.linearVelocity.y));
        }
        CHECK(f.pose(crate).position.x > 4.5f);     // across three seams
        CHECK(slowest > 1.98f);                     // no speed lost to them
        CHECK(highest < 0.05f);                     // and no bump
    }
}

TEST_CASE("tiles: fast small balls do not pass through a wall one voxel thick") {
    Fixture f(CollisionRepresentation::Tiles);
    f.solid(fill({0, 0, 0}, {15, 15, 0}), vec3(-2, -2, 5), 0.25f);   // 4 x 4 m, 0.25 m thick, at z = 5
    std::vector<Entity> balls;
    projv::RigidBody rigid;
    rigid.gravityFactor = 0.0f;
    rigid.quality = projv::RigidBody::Quality::Continuous;
    for (int i = 0; i < 20; i++) balls.push_back(f.ball(vec3(-1.5f + 0.15f * float(i), 0.1f * float(i % 7), 0), 0.05f, rigid));
    f.frames(1);
    for (Entity b : balls) setVelocity(f.app.world, b, vec3(0, 0, 200));
    f.frames(30);
    int through = 0;
    for (Entity b : balls) through += f.pose(b).position.z > 5.0f;
    CHECK(through == 0);
}

TEST_CASE("tiles: a shape given fewer id bits names its contacts by coarser cells, and still collides") {
    PhysicsWorld physics;
    utils::TileTable table;
    table.resolution = 64;
    std::vector<utils::TileEdit> edits;
    for (int z = 0; z < 64; z++)
        for (int x = 0; x < 64; x++)
            for (int y = 0; y <= 2 + (x / 3 + z / 5) % 3; y++) edits.push_back({ivec3(x, y, z), true, uint8_t((x + y) % 2)});
    utils::applyTileEdits(table, edits);
    auto shared = std::make_shared<const utils::TileTable>(std::move(table));
    for (uint32_t bits : {32u, 12u}) {   // 18 bits name every voxel of 64^3; 12 name 4^3 bricks
        CAPTURE(bits);
        CollisionShapeRef shape = physics.shapeFromTiles(shared, nullptr, 1000.0f, bits);
        REQUIRE(shape);
        BodyDesc floor;
        floor.shape = PhysicsShape::fromVoxels(shape, 0.25f);
        floor.motion = MotionType::Static;
        floor.layer = PhysicsLayer::Static;
        floor.position = vec3(float(bits) * 100.0f, 0, 0);
        BodyId id = physics.createBody(floor);
        REQUIRE(id.valid());
        auto hit = physics.castRay(floor.position + vec3(5.1f, 10, 5.1f), vec3(0, -1, 0), 20.0f);
        REQUIRE(hit);
        REQUIRE(hit->path.size() == 2);   // the cell, and its level
        CHECK(hit->path[1] == (bits == 32u ? 0u : 2u));
        // Boxes dropped on the stepped ground come to rest on it, whatever names their contacts.
        std::vector<BodyId> crates;
        for (int i = 0; i < 6; i++) {
            BodyDesc d;
            d.shape = PhysicsShape::box(vec3(0.2f));
            d.position = floor.position + vec3(2.0f + 2.0f * float(i), 3, 8);
            crates.push_back(physics.createBody(d));
        }
        for (int i = 0; i < 240; i++) physics.step(float(DT));
        for (BodyId c : crates) {
            const BodyState s = physics.bodyState(c);
            CHECK(s.position.y > 0.5f);   // on the ground (at least 3 voxels up), not through it
            CHECK(s.position.y < 1.5f);
            CHECK(glm::length(s.linearVelocity) < 0.05f);
        }
    }
    // A moving body of tile voxels is refused, not asserted on.
    BodyDesc moving;
    moving.shape = PhysicsShape::fromVoxels(physics.shapeFromTiles(shared), 0.25f);
    CHECK_FALSE(physics.createBody(moving).valid());
}

TEST_CASE("tiles bench: a pile of voxel pieces on a voxel floor, boxes against tiles" * doctest::skip()) {
    // Run with: build/bin/projv_unit_tests -tc="tiles bench*" --no-skip
    // 400 pieces of four shapes dropped in a 6 m square onto a voxel floor; ms a step and how they settle.
    // Measured 2026-10-24: boxes 1.04-1.07 ms a step, tiles 1.13 (4.6 before a moving shape's bounds
    // were its voxels' rather than its blocks').
    for (CollisionRepresentation r : {CollisionRepresentation::Boxes, CollisionRepresentation::Tiles}) {
        Fixture f(r);
        f.solid(fill({0, 0, 0}, {127, 1, 127}), vec3(-16, -0.5f, -16), 0.25f);
        std::vector<std::vector<projv::PendingVoxelOp>> shapes = {fill({0, 0, 0}, {2, 1, 1}), fill({0, 0, 0}, {3, 0, 3}),
                                                                  fill({0, 0, 0}, {1, 3, 1}), fill({0, 0, 0}, {2, 2, 2})};
        for (auto op : fill({0, 0, 0}, {0, 2, 0})) shapes[0].push_back({true, op.position + ivec3(3, 0, 0), op.packedColor});   // an L
        std::vector<Entity> pieces;
        for (int i = 0; i < 400; i++) {
            ComponentHandle h = utils::addComponent(f.scene, projv::ComponentKind::Chunk, "p", INVALID_COMPONENT_HANDLE, 16, 0.25f);
            utils::setComponentTransform(f.scene, h, vec3(-3.0f + float(i % 10) * 0.6f, 0.5f + float(i / 100) * 0.8f, -3.0f + float((i / 10) % 10) * 0.6f),
                                         glm::angleAxis(float(i) * 0.7f, glm::normalize(vec3(1, 2, 3))), 1.0f);
            utils::queueVoxelAdd(f.scene, h, shapes[size_t(i) % shapes.size()]);
            utils::updateScene(f.scene);
            Entity e = spawnComponent(f.app.world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
            f.app.world.emplace<projv::RigidBody>(e);
            pieces.push_back(e);
        }
        f.frames(2);
        double ms = 0.0;
        int steps = 0;
        for (int i = 0; i < 6 * 60; i++) {
            f.frames(1);
            ms += physicsWorld(f.app.world).stats().lastStepMilliseconds;
            steps++;
        }
        int asleep = 0, under = 0;
        for (Entity e : pieces) {
            const BodyState s = f.pose(e);
            asleep += !s.awake;
            under += s.centerOfMass.y < 0.0f;
        }
        MESSAGE((r == CollisionRepresentation::Tiles ? "tiles" : "boxes") << ": " << ms / steps << " ms a step over " << steps
                << "; " << asleep << " of 400 asleep, " << under << " under the floor");
        CHECK(under == 0);
    }
}

TEST_CASE("tiles diagnosis: a big hull falls on a floor one voxel thick" * doctest::skip()) {
    // Run with: build/bin/projv_unit_tests -tc="tiles diagnosis*" --no-skip
    spdlog::set_level(spdlog::level::trace);
    for (CollisionRepresentation r : {CollisionRepresentation::Boxes, CollisionRepresentation::Tiles})
    for (float height : {10.0f, 4.0f})
    for (float floorSpeed : {12.0f, 3.0f}) {
        projv::Application app;
        projv::Scene& scene = app.world.ctx().emplace<projv::Scene>();
        double now = 0.0;
        app.clock = [&now] { return now; };
        installSceneBridge(app);
        PhysicsConfig config;
        config.collision.representation = r;
        installPhysics(app, config);
        installImpactFracture(app);
        int breaks = 0, impactCount = 0;
        float fastestImpact = 0.0f, mostEnergy = 0.0f;
        app.events().on<projv::BodyBroken>([&](const projv::BodyBroken&) { breaks++; });
        app.runStartup();
        app.runFrame();
        PhysicsMaterial concrete{2400.0f, 0.8f, 0.0f};
        concrete.strength = 4e5f;
        concrete.shardSize = 0.3f;
        definePhysicsMaterial(app.world, "concrete", concrete);
        PhysicsMaterial wood{600.0f, 0.6f, 0.0f};
        wood.strength = 1e5f;
        wood.shardSize = 0.3f;
        definePhysicsMaterial(app.world, "wood", wood);
        const uint32_t GREY = 0x2AAAAAAAu, BROWN = 0x1F0A0000u;
        ComponentHandle floor = utils::addComponent(scene, projv::ComponentKind::Chunk, "floor", INVALID_COMPONENT_HANDLE, 64, 0.1f);
        utils::setComponentTransform(scene, floor, vec3(-10, -0.1f, -10), quat(1, 0, 0, 0), 1.0f);
        utils::queueVoxelAdd(scene, floor, fill({0, 0, 0}, {199, 0, 199}, GREY));
        utils::updateScene(scene);
        utils::setMaterialPhysics(scene, floor, utils::findMaterialByColor(scene.components[floor], GREY), "concrete");
        Entity floorEntity = spawnComponent(app.world, floor);
        projv::StaticCollider fixed;
        fixed.breaksOnImpact = true;
        fixed.minImpactSpeed = floorSpeed;   // Voxel Mayhem's map world: 12 m/s
        app.world.emplace<projv::StaticCollider>(floorEntity, fixed);
        // A hull: a 6 x 1.5 x 2 m box of 10 cm voxels, its walls and bottom one voxel thick, open at the top.
        std::vector<projv::PendingVoxelOp> hull;
        for (int z = 0; z < 20; z++)
            for (int y = 0; y < 15; y++)
                for (int x = 0; x < 60; x++)
                    if (y == 0 || x == 0 || x == 59 || z == 0 || z == 19) hull.push_back({true, ivec3(x, y, z), BROWN});
        ComponentHandle boat = utils::addComponent(scene, projv::ComponentKind::Chunk, "boat", INVALID_COMPONENT_HANDLE, 64, 0.1f);
        utils::setComponentTransform(scene, boat, vec3(-3, height, -1), glm::angleAxis(0.15f, vec3(0, 0, 1)), 1.0f);
        utils::queueVoxelAdd(scene, boat, hull);
        utils::updateScene(scene);
        utils::setMaterialPhysics(scene, boat, utils::findMaterialByColor(scene.components[boat], BROWN), "wood");
        Entity boatEntity = spawnComponent(app.world, boat, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        projv::RigidBody rigid;
        rigid.breaksOnImpact = true;
        app.world.emplace<projv::RigidBody>(boatEntity, rigid);
        float lowest = 1e9f;
        for (int i = 0; i < 4 * 60; i++) {
            now += DT;
            app.runFrame();
            for (const BodyImpact& hit : impacts(app.world)) {
                impactCount++;
                fastestImpact = std::max(fastestImpact, hit.speed);
                mostEnergy = std::max(mostEnergy, hit.energy);
            }
            if (app.world.valid(boatEntity) && app.world.all_of<projv::PhysicsBody>(boatEntity))
                lowest = std::min(lowest, bodyPose(app.world, boatEntity).position.y);
        }
        const BodyState end = app.world.valid(boatEntity) ? bodyPose(app.world, boatEntity) : BodyState{};
        MESSAGE((r == CollisionRepresentation::Tiles ? "tiles" : "boxes") << " from " << height << " m, floor's least " << floorSpeed << " m/s: impacts " << impactCount << " (fastest " << fastestImpact
                << " m/s, most " << mostEnergy << " J), breaks " << breaks << "; the hull's corner lowest y " << lowest << ", ends at y "
                << end.position.y << ", awake " << end.awake << ", speed " << glm::length(end.linearVelocity));
    }
}
