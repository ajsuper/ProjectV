// Fracture (the physics plan, "Fracture and support"): a strike breaks a voxel body round a point, as
// far as its energy against the struck material's strength reaches, into shards laid out by the
// material's fracture pattern. Checked: no voxel is lost or made, shards keep their palette entries,
// the reach follows the energy, what has no strength stays, a registered pattern decides the shards,
// the engine's rules hold whatever a pattern says, and the same strike breaks the same way twice.

#include "doctest/doctest.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

#include "core/application.h"
#include "runtime/fracture.h"
#include "runtime/physics.h"
#include "utils/collision_geometry.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/scene_query.h"

namespace {
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;
    namespace utils = projv::utils;
    namespace runtime = projv::runtime;
    constexpr double DT = 1.0 / 60.0;
    constexpr uint32_t GREY = 0x20080200u, RED = 0x3FF00000u;
    constexpr float PI = 3.14159265f;

    std::vector<projv::PendingVoxelOp> fill(ivec3 low, ivec3 high, uint32_t colour) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) ops.push_back({true, ivec3(x, y, z), colour});
        return ops;
    }

    uint8_t slotOf(const projv::Scene& scene, ComponentHandle h, uint32_t colour) {
        const auto& palette = scene.components[h].materialPalette;
        for (size_t i = 0; i < palette.size(); i++)
            if (palette[i].packedColor == colour) return uint8_t(i);
        FAIL("no slot of that colour");
        return 0;
    }

    // Every voxel under a component (a chunk or a grid's cells), with the physics material its
    // palette entry names.
    struct Count {
        uint64_t voxels = 0;
        std::set<std::string> materials;
    };
    Count countOf(const projv::Scene& scene, ComponentHandle h) {
        Count count;
        const projv::ComponentRecord& record = scene.components[h];
        std::vector<size_t> chunks;
        if (record.kind == projv::ComponentKind::Chunk) chunks.push_back(record.chunkHandle);
        else
            for (int32_t c : scene.grids[size_t(record.gridIndex)].cellToChunk)
                if (c >= 0) chunks.push_back(size_t(c));
        for (size_t c : chunks) {
            const projv::Chunk& chunk = scene.chunks[c];
            if (chunk.geometryPoolIndex < 0) continue;
            utils::VoxelOccupancy o;
            std::vector<uint8_t> slots;
            if (utils::decodeVoxelSlots(scene.geometryPool[size_t(chunk.geometryPoolIndex)], chunk.header.resolution, 1u << 24, o,
                                        slots) != utils::DecodeResult::Ok)
                continue;
            count.voxels += o.solid;
            for (size_t i = 0; i < o.cells.size(); i++)
                if (o.cells[i]) count.materials.insert(record.materialPalette[slots[i]].physics);
        }
        return count;
    }

    struct Fixture {
        projv::Application app;
        projv::Scene& scene;
        double now = 0.0;
        std::vector<projv::BodyFractured> fractures;
        explicit Fixture(int threads = -1) : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            runtime::installSceneBridge(app);
            runtime::PhysicsConfig config;
            config.settings.threads = threads;
            runtime::installPhysics(app, config);
            app.events().on<projv::BodyFractured>([this](const projv::BodyFractured& e) { fractures.push_back(e); });
            app.runStartup();
            app.runFrame();
            runtime::PhysicsMaterial stone{2400.0f, 0.7f, 0.0f};
            stone.strength = 1e5f;
            stone.shardSize = 0.5f;
            runtime::definePhysicsMaterial(app.world, "stone", stone);
            runtime::definePhysicsMaterial(app.world, "steel", {7800.0f, 0.5f, 0.0f});   // no strength: never breaks
        }
        void frames(int n) { for (int i = 0; i < n; i++) { now += DT; app.runFrame(); } }

        // An 8 x 6 m wall of quarter-metre voxels, half a metre thick, standing on y = 0 with its
        // face at z = 0: a grid (it is bigger than a chunk), stone unless told otherwise.
        ComponentHandle wallComponent = INVALID_COMPONENT_HANDLE;
        Entity wall(const std::string& material = "stone") {
            wallComponent = utils::addComponent(scene, projv::ComponentKind::Chunk, "wall", INVALID_COMPONENT_HANDLE, 16, 0.25f);
            utils::setComponentTransform(scene, wallComponent, vec3(-4, 0, 0), quat(1, 0, 0, 0), 1.0f);
            utils::queueVoxelAdd(scene, wallComponent, fill({0, 0, 0}, {31, 23, 1}, GREY));
            utils::updateScene(scene);
            REQUIRE(utils::setMaterialPhysics(scene, wallComponent, slotOf(scene, wallComponent, GREY), material));
            Entity e = runtime::spawnComponent(app.world, wallComponent);
            app.world.emplace<projv::StaticCollider>(e);
            runtime::assignNetId(app.world, e);
            return e;
        }
        // The energy that breaks out to `radius` metres in stone.
        static float energyFor(float radius) { return 1e5f * 4.0f / 3.0f * PI * radius * radius * radius; }

        uint64_t shardVoxels(const projv::BodyFractured& f) {
            uint64_t n = 0;
            for (Entity e : f.shards) n += countOf(scene, app.world.get<projv::VoxelComponent>(e).handle).voxels;
            return n;
        }
    };

    std::vector<runtime::FractureVoxel> block(ivec3 size) {
        std::vector<runtime::FractureVoxel> voxels;
        for (int z = 0; z < size.z; z++)
            for (int y = 0; y < size.y; y++)
                for (int x = 0; x < size.x; x++) {
                    runtime::FractureVoxel v;
                    v.position = ivec3(x, y, z);
                    v.slot = uint8_t((x / 4 + y / 2) % 2);   // bricks of two shades
                    v.material = 1;
                    v.breakable = true;
                    voxels.push_back(v);
                }
        return voxels;
    }
}

TEST_CASE("a strike breaks a static wall into dynamic shards, losing no voxel and keeping what they are made of") {
    Fixture f;
    Entity wall = f.wall();
    f.frames(2);
    const uint64_t before = countOf(f.scene, f.wallComponent).voxels;
    REQUIRE(before == 32 * 24 * 2);

    runtime::fracture(f.app.world, wall, vec3(0, 3, 0), Fixture::energyFor(1.5f), vec3(0, 0, 6));
    f.frames(1);
    REQUIRE(f.fractures.size() == 1);
    const projv::BodyFractured& broke = f.fractures[0];
    CHECK(broke.entity == wall);
    CHECK(broke.shards.size() >= 3);

    const uint64_t after = countOf(f.scene, f.wallComponent).voxels, inShards = f.shardVoxels(broke);
    CHECK(inShards > 0);
    CHECK(after + inShards == before);
    // About a 1.5 m ball's worth of a half-metre wall, give or take the jagged edge: 2 x pi r^2 x 0.5 m.
    const float expected = PI * 1.5f * 1.5f * 0.5f / (0.25f * 0.25f * 0.25f);
    CHECK(float(inShards) > 0.4f * expected);
    CHECK(float(inShards) < 2.5f * expected);

    for (Entity e : broke.shards) {
        REQUIRE(f.app.world.valid(e));
        CHECK(f.app.world.get<projv::RigidBody>(e).motion == runtime::MotionType::Dynamic);
        CHECK(f.app.world.all_of<projv::PhysicsBody>(e));
        CHECK(f.app.world.all_of<projv::NetId>(e));   // the wall had one
        Count c = countOf(f.scene, f.app.world.get<projv::VoxelComponent>(e).handle);
        CHECK(c.materials == std::set<std::string>{"stone"});
    }
    // Thrown the strike's way: out of the wall (which is z 0 to 0.5), the push fading to the edge.
    f.frames(20);
    int forward = 0;
    for (Entity e : broke.shards)
        if (f.app.world.valid(e) && runtime::bodyPose(f.app.world, e).centerOfMass.z > 0.35f) forward++;
    CHECK(forward > int(broke.shards.size()) / 3);
}

TEST_CASE("the reach follows the energy, and what has no strength does not break") {
    uint64_t small = 0, large = 0;
    {
        Fixture f;
        Entity wall = f.wall();
        f.frames(2);
        runtime::fracture(f.app.world, wall, vec3(0, 3, 0), Fixture::energyFor(1.0f));
        f.frames(1);
        REQUIRE(f.fractures.size() == 1);
        small = f.shardVoxels(f.fractures[0]);
    }
    {
        Fixture f;
        Entity wall = f.wall();
        f.frames(2);
        runtime::fracture(f.app.world, wall, vec3(0, 3, 0), Fixture::energyFor(2.0f));
        f.frames(1);
        REQUIRE(f.fractures.size() == 1);
        large = f.shardVoxels(f.fractures[0]);
    }
    // A thin wall: twice the radius is about four times the face.
    CHECK(float(large) > 2.5f * float(small));
    CHECK(float(large) < 6.0f * float(small));

    Fixture f;
    Entity wall = f.wall("steel");
    f.frames(2);
    runtime::fracture(f.app.world, wall, vec3(0, 3, 0), Fixture::energyFor(2.0f));
    f.frames(2);
    CHECK(f.fractures.empty());
    CHECK(countOf(f.scene, f.wallComponent).voxels == 32 * 24 * 2);

    // Nor does a strike that misses by more than three voxels.
    Fixture g;
    Entity far = g.wall();
    g.frames(2);
    runtime::fracture(g.app.world, far, vec3(0, 3, -2), Fixture::energyFor(2.0f));
    g.frames(2);
    CHECK(g.fractures.empty());
}

TEST_CASE("only what breaks comes away: steel in a stone wall stays") {
    Fixture f;
    Entity wall = f.wall();
    // A steel bar across the wall, half a metre below the strike: in its reach, but not what was struck.
    utils::queueVoxelAdd(f.scene, f.wallComponent, fill({0, 11, 0}, {31, 12, 1}, RED));
    utils::updateScene(f.scene);
    REQUIRE(utils::setMaterialPhysics(f.scene, f.wallComponent, slotOf(f.scene, f.wallComponent, RED), "steel"));
    f.frames(2);
    runtime::fracture(f.app.world, wall, vec3(0, 3.6f, 0), Fixture::energyFor(1.5f));
    f.frames(1);
    REQUIRE(f.fractures.size() == 1);
    CHECK(!f.fractures[0].shards.empty());
    for (Entity e : f.fractures[0].shards)
        CHECK(countOf(f.scene, f.app.world.get<projv::VoxelComponent>(e).handle).materials == std::set<std::string>{"stone"});
    CHECK(countOf(f.scene, f.wallComponent).voxels >= 32 * 2 * 2);   // the bar, all of it
    CHECK(countOf(f.scene, f.wallComponent).materials.count("steel") == 1);

    // Struck on the steel itself, nothing breaks.
    runtime::fracture(f.app.world, wall, vec3(-3, 2.9f, 0), Fixture::energyFor(1.5f));
    f.frames(1);
    CHECK(f.fractures.size() == 1);
}

TEST_CASE("the same strike breaks the same way") {
    auto run = [](std::vector<size_t>& sizes) {
        Fixture f;
        Entity wall = f.wall();
        f.frames(2);
        runtime::fracture(f.app.world, wall, vec3(0.3f, 2.7f, 0), Fixture::energyFor(1.5f), vec3(0, 2, 5));
        f.frames(1);
        REQUIRE(f.fractures.size() == 1);
        for (Entity e : f.fractures[0].shards)
            sizes.push_back(countOf(f.scene, f.app.world.get<projv::VoxelComponent>(e).handle).voxels);
        f.frames(60);
        return runtime::physicsStateHash(f.app.world);
    };
    std::vector<size_t> a, b;
    uint64_t ha = run(a), hb = run(b);
    CHECK(a == b);
    CHECK(ha == hb);
}

TEST_CASE("a dynamic body breaks into shards that move as it did") {
    Fixture f;
    ComponentHandle h = utils::addComponent(f.scene, projv::ComponentKind::Chunk, "slab", INVALID_COMPONENT_HANDLE, 16, 0.25f);
    utils::setComponentTransform(f.scene, h, vec3(0, 50, 0), quat(1, 0, 0, 0), 1.0f);
    utils::queueVoxelAdd(f.scene, h, fill({0, 0, 0}, {15, 15, 1}, GREY));
    utils::updateScene(f.scene);
    REQUIRE(utils::setMaterialPhysics(f.scene, h, slotOf(f.scene, h, GREY), "stone"));
    Entity slab = runtime::spawnComponent(f.app.world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    projv::RigidBody rigid;
    rigid.gravityFactor = 0.0f;
    rigid.linearDamping = 0.0f;
    f.app.world.emplace<projv::RigidBody>(slab, rigid);
    f.frames(1);
    runtime::setVelocity(f.app.world, slab, vec3(3, 0, 0));
    f.frames(2);
    runtime::fracture(f.app.world, slab, runtime::bodyPose(f.app.world, slab).centerOfMass, Fixture::energyFor(0.8f));
    f.frames(1);
    REQUIRE(f.fractures.size() == 1);
    REQUIRE(!f.fractures[0].shards.empty());
    for (Entity e : f.fractures[0].shards) {
        CHECK(f.app.world.get<projv::RigidBody>(e).gravityFactor == 0.0f);   // the body's own RigidBody
        CHECK(runtime::bodyPose(f.app.world, e).linearVelocity.x == doctest::Approx(3.0f).epsilon(0.05));
    }
}

TEST_CASE("a registered pattern decides the shards, and the engine's rules hold whatever it says") {
    Fixture f;
    // Left of the strike one shard, right of it another, everything in reach -- even voxels the
    // pattern has no business breaking.
    REQUIRE(runtime::registerFracturePattern(f.app.world, "halves", [](const runtime::FractureInput& in, runtime::FractureRandom&) {
        std::vector<int32_t> out(in.voxels.size(), -1);
        for (size_t i = 0; i < in.voxels.size(); i++)
            if (std::abs(float(in.voxels[i].position.y) + 0.5f - in.at.y) < 2.0f &&
                std::abs(float(in.voxels[i].position.x) + 0.5f - in.at.x) < in.reach)
                out[i] = float(in.voxels[i].position.x) + 0.5f < in.at.x ? 0 : 1;
        return out;
    }));
    runtime::PhysicsMaterial shale{2400.0f, 0.7f, 0.0f};
    shale.strength = 1e5f;
    shale.fracture = "halves";
    runtime::definePhysicsMaterial(f.app.world, "shale", shale);
    Entity wall = f.wall("shale");
    f.frames(2);
    runtime::fracture(f.app.world, wall, vec3(0, 3, 0), Fixture::energyFor(1.5f));
    f.frames(1);
    REQUIRE(f.fractures.size() == 1);
    REQUIRE(f.fractures[0].shards.size() == 2);
    for (Entity e : f.fractures[0].shards)
        CHECK(countOf(f.scene, f.app.world.get<projv::VoxelComponent>(e).handle).voxels == 6 * 4 * 2);   // 1.5 m = 6 voxels a side

    // A pattern that answers for the wrong number of voxels breaks nothing.
    REQUIRE(runtime::registerFracturePattern(f.app.world, "halves", [](const runtime::FractureInput&, runtime::FractureRandom&) {
        return std::vector<int32_t>(3, 0);
    }));
    runtime::fracture(f.app.world, wall, vec3(-2, 3, 0), Fixture::energyFor(1.5f));
    f.frames(1);
    CHECK(f.fractures.size() == 1);

    CHECK_FALSE(runtime::registerFracturePattern(f.app.world, "", runtime::fracture_patterns::shatter));
    CHECK_FALSE(runtime::registerFracturePattern(f.app.world, "nothing", nullptr));
}

TEST_CASE("fractureShards cuts disconnected shards apart and big ones in two") {
    std::vector<runtime::FractureVoxel> voxels = block({100, 3, 1});
    runtime::PhysicsMaterial m;
    runtime::FractureInput in{voxels, vec3(50, 1, 0), 100.0f, 150.0f, 4.0f, vec3(0, 1, 0), m};
    std::vector<int32_t> shardOf(voxels.size(), 0);
    // A gap at x = 10 in shard 0; a voxel that does not break.
    for (size_t i = 0; i < voxels.size(); i++)
        if (voxels[i].position.x == 10) shardOf[i] = -1;
    voxels[5].breakable = false;
    std::vector<std::vector<size_t>> shards = runtime::fractureShards(in, shardOf, 64);
    // 0..9 | 11..99, which is 89 across: halved.
    REQUIRE(shards.size() == 3);
    size_t total = 0;
    std::set<size_t> seen;
    for (const auto& s : shards) {
        total += s.size();
        for (size_t i : s) {
            CHECK(seen.insert(i).second);
            CHECK(i != 5);
        }
    }
    CHECK(total == voxels.size() - 3 - 1);
    CHECK(shards[0].front() < shards[1].front());
}

TEST_CASE("the built-in patterns break within their reach, and the same strike the same way") {
    std::vector<runtime::FractureVoxel> voxels = block({24, 24, 24});
    runtime::PhysicsMaterial m;
    runtime::FractureInput in{voxels, vec3(12, 12, 12), 6.0f, 9.0f, 3.0f, vec3(0, 1, 0), m};
    using Pattern = std::vector<int32_t> (*)(const runtime::FractureInput&, runtime::FractureRandom&);
    const std::pair<const char*, Pattern> patterns[] = {{"shatter", runtime::fracture_patterns::shatter},
                                                         {"splinter", runtime::fracture_patterns::splinter},
                                                         {"crumble", runtime::fracture_patterns::crumble},
                                                         {"seams", runtime::fracture_patterns::seams}};
    for (auto [name, pattern] : patterns) {
        CAPTURE(name);
        runtime::FractureRandom r1(42), r2(42);
        std::vector<int32_t> a = pattern(in, r1), b = pattern(in, r2);
        REQUIRE(a.size() == voxels.size());
        CHECK(a == b);
        size_t broken = 0, inReach = 0;
        float farthest = 0.0f;
        std::set<int32_t> shards;
        for (size_t i = 0; i < a.size(); i++) {
            float d = glm::distance(vec3(voxels[i].position) + 0.5f, in.at);
            if (d <= in.reach) inReach++;
            if (a[i] < 0) continue;
            broken++;
            shards.insert(a[i]);
            farthest = std::max(farthest, d);
        }
        CHECK(shards.size() >= 2);
        if (std::string(name) == "crumble") CHECK(farthest <= in.reach);
        else if (std::string(name) == "seams") CHECK(farthest <= in.reach + 12.0f);   // a brick the strike reaches comes whole
        else CHECK(farthest <= in.outer);
        CHECK(float(broken) > 0.5f * float(inReach));
    }
    // Splinters run along the grain: their shards are longer in y than across it.
    runtime::FractureRandom r(7);
    std::vector<std::vector<size_t>> shards = runtime::fractureShards(in, runtime::fracture_patterns::splinter(in, r));
    float along = 0.0f, across = 0.0f;
    for (const auto& s : shards) {
        if (s.size() < 8) continue;
        ivec3 lo(1 << 20), hi(-(1 << 20));
        for (size_t i : s) { lo = glm::min(lo, voxels[i].position); hi = glm::max(hi, voxels[i].position); }
        along += float(hi.y - lo.y + 1);
        across += 0.5f * float(hi.x - lo.x + 1 + hi.z - lo.z + 1);
    }
    CHECK(along > 1.5f * across);
}

TEST_CASE("a library file carries strength and how a material breaks") {
    Fixture f;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "projv_fracture_library.json";
    {
        std::ofstream out(path);
        out << R"({"v": 1, "materials": [
            {"name": "oak", "density": 750, "strength": 3e5, "fracture": "splinter", "shardSize": 0.3, "grain": [2, 0, 0]},
            {"name": "plaster", "strength": 1e4, "fracture": "crumble"}]})";
    }
    REQUIRE(runtime::loadPhysicsMaterials(f.app.world, path.string()));
    std::optional<runtime::PhysicsMaterial> oak = runtime::physicsMaterial(f.app.world, "oak");
    REQUIRE(oak);
    CHECK(oak->strength == doctest::Approx(3e5f));
    CHECK(oak->fracture == "splinter");
    CHECK(oak->shardSize == doctest::Approx(0.3f));
    CHECK(oak->grain == vec3(1, 0, 0));   // normalized
    CHECK(runtime::physicsMaterial(f.app.world, "plaster")->fracture == "crumble");

    runtime::PhysicsMaterial bad;
    bad.strength = -1.0f;
    CHECK(runtime::definePhysicsMaterial(f.app.world, "bad", bad) == 0);
    bad = {};
    bad.grain = vec3(0);
    CHECK(runtime::definePhysicsMaterial(f.app.world, "bad", bad) == 0);
    bad = {};
    bad.shardSize = 0.0f;
    CHECK(runtime::definePhysicsMaterial(f.app.world, "bad", bad) == 0);

    // The library's breaking is in the state hash: peers whose stone breaks differently differ.
    uint64_t before = runtime::physicsStateHash(f.app.world);
    oak->strength = 4e5f;
    runtime::definePhysicsMaterial(f.app.world, "oak", *oak);
    CHECK(runtime::physicsStateHash(f.app.world) != before);
    std::filesystem::remove(path);
}

namespace {
    // A 0.8 m ball of `mass`, its centre at `at`, flying at `velocity`.
    Entity ball(Fixture& f, vec3 at, vec3 velocity, float mass, float gravity = 0.0f) {
        ComponentHandle h = utils::addComponent(f.scene, projv::ComponentKind::Chunk, "ball", INVALID_COMPONENT_HANDLE, 16, 0.2f);
        utils::setComponentTransform(f.scene, h, at - vec3(0.4f), quat(1, 0, 0, 0), 1.0f);
        utils::queueVoxelAdd(f.scene, h, fill({0, 0, 0}, {3, 3, 3}, RED));
        utils::updateScene(f.scene);
        Entity e = runtime::spawnComponent(f.app.world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        projv::RigidBody r;
        r.shape = projv::RigidBody::Shape::Sphere;
        r.radius = 0.4f;
        r.mass = mass;
        r.gravityFactor = gravity;
        r.linearDamping = 0.0f;
        r.quality = projv::RigidBody::Quality::Continuous;
        f.app.world.emplace<projv::RigidBody>(e, r);
        runtime::assignNetId(f.app.world, e);
        f.frames(1);
        runtime::setVelocity(f.app.world, e, velocity);
        return e;
    }
}

TEST_CASE("impacts: one per pair of bodies, the fastest, in body order, with what the collision can take out") {
    runtime::PhysicsWorld sim;
    sim.createBody({.shape = runtime::PhysicsShape::box({20, 1, 20}), .motion = runtime::MotionType::Static,
                    .layer = runtime::PhysicsLayer::Static, .position = {0, -1, 0}});
    runtime::BodyId slow = sim.createBody({.shape = runtime::PhysicsShape::box({0.5f, 0.5f, 0.5f}), .position = {-3, 0.52f, 0}});
    runtime::BodyId fast = sim.createBody({.shape = runtime::PhysicsShape::box({0.5f, 0.5f, 0.5f}), .position = {3, 3, 0}});
    sim.setVelocity(fast, {0, -10, 0}, {0, 0, 0});
    std::vector<runtime::Impact> seen;
    for (int i = 0; i < 60; i++) {
        sim.step(1.0f / 60.0f);
        for (const runtime::Impact& hit : sim.impacts()) {
            CHECK(hit.a.value < hit.b.value);
            seen.push_back(hit);
        }
        for (size_t k = 1; k < sim.impacts().size(); k++)
            CHECK(std::make_pair(sim.impacts()[k - 1].a.value, sim.impacts()[k - 1].b.value) <
                  std::make_pair(sim.impacts()[k].a.value, sim.impacts()[k].b.value));
    }
    // The fast box landed hard, once; the slow one set down from 2 cm, under the threshold.
    int fastHits = 0;
    for (const runtime::Impact& hit : seen) {
        CHECK(!(hit.a == slow || hit.b == slow));
        if (hit.a == fast || hit.b == fast) {
            fastHits++;
            float mass = sim.bodyMass(fast);
            CHECK(hit.speed > 10.0f);
            CHECK(hit.energy == doctest::Approx(0.5f * mass * hit.speed * hit.speed).epsilon(0.01));
            CHECK(std::abs(hit.point.y) < 0.2f);
        }
    }
    CHECK(fastHits == 1);
}

TEST_CASE("on impact: what is struck hard enough breaks, and what struck it carries on through") {
    Fixture f;
    // Light stone, so the rubble in the ball's way weighs less than the ball.
    runtime::PhysicsMaterial chalk{300.0f, 0.6f, 0.0f};
    chalk.strength = 1e5f;
    runtime::definePhysicsMaterial(f.app.world, "chalk", chalk);
    Entity wall = f.wall("chalk");
    f.frames(2);
    // A tonne at 30 m/s: 450 kJ, which breaks out to about a metre.
    Entity rock = ball(f, vec3(0, 3, -6), vec3(0, 0, 30), 1000.0f);
    for (int i = 0; i < 30 && f.fractures.empty(); i++) f.frames(1);
    REQUIRE(f.fractures.size() >= 1);
    CHECK(f.fractures[0].entity == wall);
    CHECK(f.fractures[0].striker == rock);
    CHECK(f.fractures[0].shards.size() >= 2);
    CHECK(f.fractures[0].energy == doctest::Approx(0.5f * 1000.0f * 30.0f * 30.0f).epsilon(0.02));
    for (Entity e : f.fractures[0].shards) CHECK_FALSE(f.app.world.get<projv::RigidBody>(e).breaksOnImpact);
    // Its pieces go the way it was going.
    vec3 shardVelocity(0.0f);
    for (Entity e : f.fractures[0].shards) shardVelocity += runtime::bodyPose(f.app.world, e).linearVelocity;
    CHECK(shardVelocity.z / float(f.fractures[0].shards.size()) > 3.0f);
    // The wall stopped it; the break gave back what it did not use (less what it lost to the rubble).
    CHECK(runtime::bodyPose(f.app.world, rock).linearVelocity.z > 15.0f);
    // Through the half-metre wall, and still going.
    f.frames(30);
    runtime::BodyState after = runtime::bodyPose(f.app.world, rock);
    CHECK(after.position.z > 1.0f);
    CHECK(after.linearVelocity.z > 5.0f);
}

TEST_CASE("on impact: a knock breaks nothing, nor does a body that is told not to break") {
    {
        Fixture f;
        f.wall();
        f.frames(2);
        // 400 kg at 3 m/s: 1.8 kJ, a reach of 0.16 m -- less than a shard.
        Entity rock = ball(f, vec3(0, 3, -2), vec3(0, 0, 3), 400.0f);
        f.frames(60);
        CHECK(f.fractures.empty());
        CHECK(runtime::bodyPose(f.app.world, rock).position.z < 0.0f);   // stopped by the wall
    }
    {
        Fixture f;
        Entity wall = f.wall();
        f.app.world.patch<projv::StaticCollider>(wall, [](projv::StaticCollider& c) { c.breaksOnImpact = false; });
        f.frames(2);
        ball(f, vec3(0, 3, -6), vec3(0, 0, 30), 400.0f);
        f.frames(30);
        CHECK(f.fractures.empty());
        CHECK(countOf(f.scene, f.wallComponent).voxels == 32 * 24 * 2);
    }
}

TEST_CASE("on impact: the same throw breaks the same wall the same way") {
    auto run = [] {
        Fixture f;
        f.wall();
        f.frames(2);
        ball(f, vec3(0.2f, 2.8f, -6), vec3(0.5f, 0.3f, 30), 400.0f);
        f.frames(90);
        return std::make_pair(f.fractures.size(), runtime::physicsStateHash(f.app.world));
    };
    auto a = run(), b = run();
    CHECK(a.first >= 1);
    CHECK(a == b);
}

TEST_CASE("breaksOnImpact is saved, and only when it is off") {
    projv::RigidBody r;
    CHECK_FALSE(runtime::ComponentTraits<projv::RigidBody>::save(r).contains("breaksOnImpact"));
    r.breaksOnImpact = false;
    nlohmann::json j = runtime::ComponentTraits<projv::RigidBody>::save(r);
    CHECK(j.at("breaksOnImpact") == false);
    CHECK_FALSE(runtime::ComponentTraits<projv::RigidBody>::load(j, 1)->breaksOnImpact);
    projv::StaticCollider c;
    c.breaksOnImpact = false;
    CHECK_FALSE(runtime::ComponentTraits<projv::StaticCollider>::load(runtime::ComponentTraits<projv::StaticCollider>::save(c), 1)->breaksOnImpact);
}

TEST_CASE("on impact: what breaks does not depend on the number of threads") {
    // Three falling balls into a chalk wall, the shards raining onto each other: impacts measured
    // from motion the solver's threads also change (gravity is applied while contacts are found).
    auto run = [](int threads) {
        Fixture f(threads);
        runtime::PhysicsMaterial chalk{300.0f, 0.6f, 0.0f};
        chalk.strength = 1e5f;
        runtime::definePhysicsMaterial(f.app.world, "chalk", chalk);
        f.wall("chalk");
        f.frames(2);
        ball(f, vec3(-2, 4, -6), vec3(0, 2, 28), 1000.0f, 1.0f);
        ball(f, vec3(0.5f, 3, -7), vec3(0.3f, 1, 30), 800.0f, 1.0f);
        ball(f, vec3(2.5f, 2, -5), vec3(-0.2f, 3, 26), 1200.0f, 1.0f);
        f.frames(120);
        std::vector<size_t> shards;
        for (const projv::BodyFractured& e : f.fractures) shards.push_back(e.shards.size());
        return std::make_pair(shards, runtime::physicsStateHash(f.app.world));
    };
    auto single = run(0), four = run(4), many = run(12);
    CHECK(single.first.size() >= 3);
    CHECK(single == four);
    CHECK(single == many);
}
