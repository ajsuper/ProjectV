// Queries (the physics plan, "Queries"): rays, sphere casts and overlaps, and hits traced back to
// the exact voxel -- checked against utils::pickVoxel, the editor's exact CPU picker, on the same
// scene.

#include "doctest/doctest.h"

#include <cmath>
#include <random>
#include <vector>

#include "core/application.h"
#include "runtime/physics.h"
#include "utils/editing.h"
#include "utils/picking.h"
#include "utils/scene_query.h"

namespace {
    using namespace projv::runtime;
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;
    namespace utils = projv::utils;
    constexpr double DT = 1.0 / 60.0;

    BodyId staticBox(PhysicsWorld& physics, vec3 at, vec3 half, PhysicsLayer layer = PhysicsLayer::Static) {
        BodyDesc d;
        d.shape = PhysicsShape::box(half);
        d.motion = MotionType::Static;
        d.layer = layer;
        d.position = at;
        return physics.createBody(d);
    }
}

TEST_CASE("a ray finds the nearest body, its distance and its face's normal") {
    PhysicsWorld physics;
    BodyId nearby = staticBox(physics, vec3(0, 0, 5), vec3(1));
    BodyId farther = staticBox(physics, vec3(0, 0, 12), vec3(1));
    auto hit = physics.castRay(vec3(0, 0, 0), vec3(0, 0, 1), 100.0f);
    REQUIRE(hit);
    CHECK(hit->body == nearby);
    CHECK(hit->distance == doctest::Approx(4.0f).epsilon(1e-3));
    CHECK(hit->normal.z == doctest::Approx(-1.0f).epsilon(1e-3));
    CHECK(hit->position.z == doctest::Approx(4.0f).epsilon(1e-3));

    CHECK_FALSE(physics.castRay(vec3(0, 0, 0), vec3(0, 0, 1), 3.5f));    // too short
    CHECK_FALSE(physics.castRay(vec3(0, 0, 0), vec3(0, 1, 0), 100.0f));  // misses
    QueryFilter skip;
    skip.ignore = nearby;
    CHECK(physics.castRay(vec3(0, 0, 0), vec3(0, 0, 1), 100.0f, skip)->body == farther);
    QueryFilter onlyMoving;
    onlyMoving.layers = QueryFilter::only(PhysicsLayer::Moving);
    CHECK_FALSE(physics.castRay(vec3(0, 0, 0), vec3(0, 0, 1), 100.0f, onlyMoving));
    CHECK_FALSE(physics.castRay(vec3(0, 0, 0), vec3(0, 0, 0), 100.0f));   // no direction, no hit
}

TEST_CASE("a swept sphere stops short of the surface by its radius; overlaps list who is there") {
    PhysicsWorld physics;
    BodyId wall = staticBox(physics, vec3(0, 0, 5), vec3(3, 3, 1));
    auto hit = physics.castSphere(vec3(0, 0, 0), 0.5f, vec3(0, 0, 1), 100.0f);
    REQUIRE(hit);
    CHECK(hit->body == wall);
    CHECK(hit->distance == doctest::Approx(3.5f).epsilon(0.01));
    CHECK(hit->normal.z == doctest::Approx(-1.0f).epsilon(0.01));

    BodyId a = staticBox(physics, vec3(10, 0, 0), vec3(0.5f), PhysicsLayer::Moving);
    BodyId b = staticBox(physics, vec3(11.5f, 0, 0), vec3(0.5f), PhysicsLayer::Moving);
    std::vector<BodyId> both = physics.overlapSphere(vec3(10.75f, 0, 0), 1.0f);
    REQUIRE(both.size() == 2);
    CHECK(both[0].value < both[1].value);
    CHECK((both[0] == a || both[1] == a));
    CHECK((both[0] == b || both[1] == b));
    CHECK(physics.overlapBox(vec3(10, 0, 0), vec3(0.2f), quat(1, 0, 0, 0)) == std::vector<BodyId>{a});
    CHECK(physics.overlapSphere(vec3(50, 50, 50), 1.0f).empty());
}

namespace {
    struct Fixture {
        projv::Application app;
        projv::Scene& scene;
        double now = 0.0;
        Fixture() : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            installSceneBridge(app);
            installPhysics(app);
            app.runStartup();
            app.runFrame();
        }
        void frames(int n) { for (int i = 0; i < n; i++) { now += DT; app.runFrame(); } }
        ComponentHandle chunk(const std::vector<projv::PendingVoxelOp>& ops, vec3 at, quat rotation, float voxel,
                              ComponentHandle parent = INVALID_COMPONENT_HANDLE, uint32_t resolution = 16) {
            ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "c", parent, resolution, voxel);
            utils::setComponentTransform(scene, h, at, rotation, 1.0f);
            utils::queueVoxelAdd(scene, h, ops);
            utils::updateScene(scene);
            return h;
        }
        Entity solid(ComponentHandle h) {
            Entity e = spawnComponent(app.world, h);
            app.world.emplace<projv::StaticCollider>(e);
            return e;
        }
    };

    // A random clumpy shape in a 16^3 chunk, in a few colours.
    std::vector<projv::PendingVoxelOp> clumps(std::mt19937& random) {
        std::uniform_int_distribution<int> cell(0, 12), size(1, 4), colour(0, 3);
        const uint32_t colours[] = {0x3FF00000u, 0x000FFC00u, 0x000003FFu, 0x3FFFFFFFu};
        std::vector<projv::PendingVoxelOp> ops;
        for (int k = 0; k < 5; k++) {
            ivec3 low(cell(random), cell(random), cell(random));
            ivec3 high = glm::min(low + ivec3(size(random), size(random), size(random)), ivec3(15));
            uint32_t c = colours[colour(random)];
            for (int z = low.z; z <= high.z; z++)
                for (int y = low.y; y <= high.y; y++)
                    for (int x = low.x; x <= high.x; x++) ops.push_back({true, ivec3(x, y, z), c});
        }
        return ops;
    }

    // Rays at a scene from all around: castRay and pickVoxel must agree on the voxel struck, its
    // material slot, and (away from edges, where box corners are rounded) the face's normal.
    void agreeWithPicker(Fixture& f, vec3 centre, float spread, int rays, unsigned seed) {
        std::mt19937 random(seed);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        int hits = 0, normals = 0;
        for (int i = 0; i < rays; i++) {
            vec3 direction = glm::normalize(vec3(unit(random), unit(random), unit(random)) + vec3(0.001f));
            vec3 origin = centre - direction * 30.0f + vec3(unit(random), unit(random), unit(random)) * spread;
            utils::VoxelPick pick = utils::pickVoxel(f.scene, origin, direction, 100.0f);
            auto hit = castRay(f.app.world, origin, direction, 100.0f);
            REQUIRE(pick.hit == hit.has_value());
            if (!pick.hit) continue;
            CAPTURE(i);
            REQUIRE(hit->onVoxel);
            CHECK(hit->chunk == pick.chunk);
            CHECK(hit->voxel == pick.voxelCoord);
            CHECK(hit->materialSlot == pick.materialSlot);
            CHECK(hit->distance == doctest::Approx(pick.distance).epsilon(0.02));
            const projv::Chunk& chunk = f.scene.chunks[pick.chunk];
            vec3 face = glm::mat3_cast(chunk.header.rotation) * vec3(pick.faceNormal);
            if (glm::dot(hit->normal, face) > 0.999f) normals++;
            hits++;
        }
        CHECK(hits > rays / 8);   // enough agreement to mean something
        CHECK(normals >= hits * 9 / 10);   // the rest graze a rounded box edge
    }
}

TEST_CASE("a ray hit names the same voxel and material the exact picker does: one chunk") {
    for (unsigned seed : {1u, 2u, 3u}) {
        Fixture f;
        std::mt19937 random(seed);
        f.solid(f.chunk(clumps(random), vec3(3, -2, 1), glm::angleAxis(0.6f, glm::normalize(vec3(1, 2, 0.5f))), 0.25f));
        f.frames(2);
        agreeWithPicker(f, vec3(3, -2, 1) + vec3(2), 1.5f, 300, seed);
    }
}

TEST_CASE("a ray hit names the same voxel: a prefab's chunk, offset inside its asset") {
    Fixture f;
    std::mt19937 random(9);
    ComponentHandle node = utils::addComponent(f.scene, projv::ComponentKind::Asset, "prefab", INVALID_COMPONENT_HANDLE, 4, 1.0f);
    f.chunk(clumps(random), vec3(-2), quat(1, 0, 0, 0), 0.25f, node);
    f.chunk(clumps(random), vec3(1, 0, 0), glm::angleAxis(0.4f, vec3(0, 1, 0)), 0.25f, node);
    utils::setComponentTransform(f.scene, node, vec3(10, 4, -3), glm::angleAxis(1.1f, glm::normalize(vec3(0, 1, 1))), 1.0f);
    f.solid(node);
    f.frames(2);
    agreeWithPicker(f, vec3(10, 4, -3), 2.0f, 400, 9);
}

TEST_CASE("a ray hit names the same voxel: a grid") {
    Fixture f;
    std::vector<projv::PendingVoxelOp> ground;
    for (int z = 0; z < 40; z++)
        for (int x = 0; x < 40; x++)
            for (int y = 0; y <= (x * 7 + z * 3) % 5; y++) ground.push_back({true, ivec3(x, y, z), y % 2 ? 0x3FF00000u : 0x000FFC00u});
    ComponentHandle h = f.chunk(ground, vec3(-10, 0, -10), quat(1, 0, 0, 0), 0.5f);
    REQUIRE(f.scene.components[h].kind == projv::ComponentKind::Grid);
    f.solid(h);
    f.frames(2);
    agreeWithPicker(f, vec3(0, 1, 0), 6.0f, 400, 4);
}

TEST_CASE("a moving body is hit where the simulation has it, and overlaps come in NetId order") {
    Fixture f;
    std::vector<Entity> balls;
    for (int i = 0; i < 4; i++) {
        Entity e = f.app.world.create();
        f.app.world.emplace<projv::Transform>(e, projv::Transform{vec3(float(i) * 1.1f, 20, 0)});
        f.app.world.emplace<projv::NetId>(e, projv::NetId{uint32_t(50 - i)});   // reverse of creation order
        projv::RigidBody rigid;
        rigid.shape = projv::RigidBody::Shape::Sphere;
        rigid.radius = 0.5f;
        rigid.gravityFactor = 0.0f;
        f.app.world.emplace<projv::RigidBody>(e, rigid);
        balls.push_back(e);
    }
    f.frames(2);
    std::vector<Entity> found = overlapSphere(f.app.world, vec3(1.65f, 20, 0), 3.0f);
    REQUIRE(found.size() == 4);
    for (size_t i = 0; i < 4; i++) CHECK(found[i] == balls[3 - i]);   // NetIds 47, 48, 49, 50

    setVelocity(f.app.world, balls[0], vec3(0, 0, 4));
    f.frames(30);
    BodyState pose = bodyPose(f.app.world, balls[0]);
    auto hit = castRay(f.app.world, pose.position + vec3(0, 5, 0), vec3(0, -1, 0), 10.0f);
    REQUIRE(hit);
    CHECK(hit->entity == balls[0]);
    CHECK(hit->netId.value == 50);
    CHECK(hit->distance == doctest::Approx(4.5f).epsilon(0.01));
    CHECK_FALSE(hit->onVoxel);   // a primitive
    CHECK_FALSE(castRay(f.app.world, pose.position + vec3(0, 5, 0), vec3(0, -1, 0), 10.0f, 0xffffffffu, balls[0]));
}
