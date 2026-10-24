// Fracture (the physics plan, "Fracture and support"): a strike breaks a voxel body round a point, as
// far as its energy against the struck material's strength reaches, into shards laid out by the
// material's fracture pattern. Checked: no voxel is lost or made, shards keep their palette entries,
// the reach follows the energy, what has no strength stays, a registered pattern decides the shards,
// the engine's rules hold whatever a pattern says, and the same strike breaks the same way twice.

#include "doctest/doctest.h"

#include <cmath>
#include <limits>
#include <optional>
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
        // Breaks by a strike, and the rest (edits, support), from the one event.
        std::vector<projv::BodyBroken> fractures;
        std::vector<projv::BodyBroken> splits;
        explicit Fixture(int threads = -1, uint32_t maxPieces = 3000,
                         std::optional<runtime::ImpactFractureConfig> impactFracture = runtime::ImpactFractureConfig{},
                         runtime::PhysicsConfig::FractureLimits limits = {}, uint32_t dustVoxels = 0)
            : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            runtime::installSceneBridge(app);
            runtime::PhysicsConfig config;
            config.settings.threads = threads;
            config.maxPieces = maxPieces;
            config.fracture = limits;
            config.dustVoxels = dustVoxels;
            runtime::installPhysics(app, config);
            if (impactFracture) runtime::installImpactFracture(app, *impactFracture);
            app.events().on<projv::BodyBroken>([this](const projv::BodyBroken& e) {
                bool struck = e.cause == projv::BreakCause::Strike || e.cause == projv::BreakCause::Impact;
                (struck ? fractures : splits).push_back(e);
            });
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
        Entity wall(const std::string& material = "stone", bool breaksOnImpact = false) {
            wallComponent = utils::addComponent(scene, projv::ComponentKind::Chunk, "wall", INVALID_COMPONENT_HANDLE, 16, 0.25f);
            utils::setComponentTransform(scene, wallComponent, vec3(-4, 0, 0), quat(1, 0, 0, 0), 1.0f);
            utils::queueVoxelAdd(scene, wallComponent, fill({0, 0, 0}, {31, 23, 1}, GREY));
            utils::updateScene(scene);
            REQUIRE(utils::setMaterialPhysics(scene, wallComponent, slotOf(scene, wallComponent, GREY), material));
            Entity e = runtime::spawnComponent(app.world, wallComponent);
            projv::StaticCollider collider;
            collider.breaksOnImpact = breaksOnImpact;
            app.world.emplace<projv::StaticCollider>(e, collider);
            runtime::assignNetId(app.world, e);
            return e;
        }
        // The energy that breaks out to `radius` metres in stone.
        static float energyFor(float radius) { return 1e5f * 4.0f / 3.0f * PI * radius * radius * radius; }

        uint64_t shardVoxels(const projv::BodyBroken& f) {
            uint64_t n = 0;
            for (Entity e : f.pieces) n += countOf(scene, app.world.get<projv::VoxelComponent>(e).handle).voxels;
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
    const projv::BodyBroken& broke = f.fractures[0];
    CHECK(broke.entity == wall);
    CHECK(broke.pieces.size() >= 3);

    const uint64_t after = countOf(f.scene, f.wallComponent).voxels, inShards = f.shardVoxels(broke);
    CHECK(inShards > 0);
    CHECK(after + inShards == before);
    // About a 1.5 m ball's worth of a half-metre wall, give or take the jagged edge: 2 x pi r^2 x 0.5 m.
    const float expected = PI * 1.5f * 1.5f * 0.5f / (0.25f * 0.25f * 0.25f);
    CHECK(float(inShards) > 0.4f * expected);
    CHECK(float(inShards) < 2.5f * expected);

    for (Entity e : broke.pieces) {
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
    for (Entity e : broke.pieces)
        if (f.app.world.valid(e) && runtime::bodyPose(f.app.world, e).centerOfMass.z > 0.35f) forward++;
    CHECK(forward > int(broke.pieces.size()) / 3);
}

TEST_CASE("each voxel breaks within the reach the energy has in its own material: iron in stone, near the blow") {
    auto ironLost = [](bool byReach) {
        Fixture f;
        runtime::PhysicsMaterial iron{7800.0f, 0.5f, 0.0f};
        iron.strength = 1e6f;   // ten times stone's: the same energy reaches 0.46 as far in it
        runtime::definePhysicsMaterial(f.app.world, "iron", iron);
        Entity wall = f.wall();
        // An iron bar across the wall, y 2.75..3.25 m, its top half a metre below the strike.
        utils::queueVoxelAdd(f.scene, f.wallComponent, fill({0, 11, 0}, {31, 12, 1}, RED));
        utils::updateScene(f.scene);
        const uint8_t slot = slotOf(f.scene, f.wallComponent, RED);
        REQUIRE(utils::setMaterialPhysics(f.scene, f.wallComponent, slot, "iron"));
        f.frames(2);
        if (byReach) runtime::fractureWithin(f.app.world, wall, vec3(0, 3.6f, 0), 1.5f);
        else runtime::fracture(f.app.world, wall, vec3(0, 3.6f, 0), Fixture::energyFor(1.5f));
        f.frames(1);
        REQUIRE(f.fractures.size() == 1);
        return 32 * 2 * 2 - int(utils::countMaterialUsage(f.scene, f.wallComponent)[slot]);
    };
    // 1.5 m of stone, but only 0.7 m of iron: a few voxels of the bar under the blow go, not the
    // three metres of it the stone's reach crosses.
    const int lost = ironLost(false);
    CHECK(lost >= 1);
    CHECK(lost <= 12);
    // A strike by reach breaks whatever is breakable within it, whatever it is made of.
    CHECK(ironLost(true) > 3 * lost);
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
    CHECK(!f.fractures[0].pieces.empty());
    for (Entity e : f.fractures[0].pieces)
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
        for (Entity e : f.fractures[0].pieces)
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
    REQUIRE(!f.fractures[0].pieces.empty());
    for (Entity e : f.fractures[0].pieces) {
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
    REQUIRE(f.fractures[0].pieces.size() == 2);
    for (Entity e : f.fractures[0].pieces)
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
            {"name": "plaster", "strength": 1e4, "fracture": "crumble"},
            {"name": "bedrock", "anchor": true}]})";
    }
    REQUIRE(runtime::loadPhysicsMaterials(f.app.world, path.string()));
    std::optional<runtime::PhysicsMaterial> oak = runtime::physicsMaterial(f.app.world, "oak");
    REQUIRE(oak);
    CHECK(oak->strength == doctest::Approx(3e5f));
    CHECK(oak->fracture == "splinter");
    CHECK(oak->shardSize == doctest::Approx(0.3f));
    CHECK(oak->grain == vec3(1, 0, 0));   // normalized
    CHECK(runtime::physicsMaterial(f.app.world, "plaster")->fracture == "crumble");
    CHECK_FALSE(runtime::physicsMaterial(f.app.world, "plaster")->anchor);
    CHECK(runtime::physicsMaterial(f.app.world, "bedrock")->anchor);

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
    Entity wall = f.wall("chalk", true);
    f.frames(2);
    // A tonne at 30 m/s: 450 kJ, which breaks out to about a metre.
    Entity rock = ball(f, vec3(0, 3, -6), vec3(0, 0, 30), 1000.0f);
    for (int i = 0; i < 30 && f.fractures.empty(); i++) f.frames(1);
    REQUIRE(f.fractures.size() >= 1);
    CHECK(f.fractures[0].entity == wall);
    CHECK(f.fractures[0].striker == rock);
    CHECK(f.fractures[0].pieces.size() >= 2);
    CHECK(f.fractures[0].energy == doctest::Approx(0.5f * 1000.0f * 30.0f * 30.0f).epsilon(0.02));
    // Its pieces take after the wall: they break on impact too.
    for (Entity e : f.fractures[0].pieces) CHECK(f.app.world.get<projv::RigidBody>(e).breaksOnImpact);
    // Its pieces go the way it was going.
    vec3 shardVelocity(0.0f);
    for (Entity e : f.fractures[0].pieces) shardVelocity += runtime::bodyPose(f.app.world, e).linearVelocity;
    CHECK(shardVelocity.z / float(f.fractures[0].pieces.size()) > 3.0f);
    // The wall stopped it; the break gave back what it did not use (less what it lost to the rubble).
    CHECK(runtime::bodyPose(f.app.world, rock).linearVelocity.z > 15.0f);
    // Through the half-metre wall, and still going.
    f.frames(30);
    runtime::BodyState after = runtime::bodyPose(f.app.world, rock);
    CHECK(after.position.z > 1.0f);
    CHECK(after.linearVelocity.z > 5.0f);
}

TEST_CASE("on impact: a knock breaks nothing, nor does a body that did not ask to break") {
    {
        Fixture f;
        f.wall("stone", true);
        f.frames(2);
        // 400 kg at 3 m/s: 1.8 kJ, a reach of 0.16 m -- less than a shard.
        Entity rock = ball(f, vec3(0, 3, -2), vec3(0, 0, 3), 400.0f);
        f.frames(60);
        CHECK(f.fractures.empty());
        CHECK(runtime::bodyPose(f.app.world, rock).position.z < 0.0f);   // stopped by the wall
    }
    for (bool patched : {false, true}) {
        // Off by default; and turned off after the body was made.
        Fixture f;
        Entity wall = f.wall("stone", patched);
        if (patched) f.app.world.patch<projv::StaticCollider>(wall, [](projv::StaticCollider& c) { c.breaksOnImpact = false; });
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
        f.wall("stone", true);
        f.frames(2);
        ball(f, vec3(0.2f, 2.8f, -6), vec3(0.5f, 0.3f, 30), 400.0f);
        f.frames(90);
        return std::make_pair(f.fractures.size(), runtime::physicsStateHash(f.app.world));
    };
    auto a = run(), b = run();
    CHECK(a.first >= 1);
    CHECK(a == b);
}

TEST_CASE("breaksOnImpact is off unless asked for, and saved only when on") {
    projv::RigidBody r;
    CHECK_FALSE(r.breaksOnImpact);
    CHECK_FALSE(runtime::ComponentTraits<projv::RigidBody>::save(r).contains("breaksOnImpact"));
    CHECK_FALSE(runtime::ComponentTraits<projv::RigidBody>::load(nlohmann::json::object(), 1)->breaksOnImpact);
    r.breaksOnImpact = true;
    nlohmann::json j = runtime::ComponentTraits<projv::RigidBody>::save(r);
    CHECK(j.at("breaksOnImpact") == true);
    CHECK(runtime::ComponentTraits<projv::RigidBody>::load(j, 1)->breaksOnImpact);
    projv::StaticCollider c;
    CHECK_FALSE(c.breaksOnImpact);
    c.breaksOnImpact = true;
    CHECK(runtime::ComponentTraits<projv::StaticCollider>::load(runtime::ComponentTraits<projv::StaticCollider>::save(c), 1)->breaksOnImpact);
}

TEST_CASE("on impact: what breaks does not depend on the number of threads") {
    // Three falling balls into a chalk wall, the shards raining onto each other: impacts measured
    // from motion the solver's threads also change (gravity is applied while contacts are found).
    auto run = [](int threads) {
        Fixture f(threads);
        runtime::PhysicsMaterial chalk{300.0f, 0.6f, 0.0f};
        chalk.strength = 1e5f;
        runtime::definePhysicsMaterial(f.app.world, "chalk", chalk);
        f.wall("chalk", true);
        f.frames(2);
        ball(f, vec3(-2, 4, -6), vec3(0, 2, 28), 1000.0f, 1.0f);
        ball(f, vec3(0.5f, 3, -7), vec3(0.3f, 1, 30), 800.0f, 1.0f);
        ball(f, vec3(2.5f, 2, -5), vec3(-0.2f, 3, 26), 1200.0f, 1.0f);
        f.frames(120);
        std::vector<size_t> shards;
        for (const projv::BodyBroken& e : f.fractures) shards.push_back(e.pieces.size());
        return std::make_pair(shards, runtime::physicsStateHash(f.app.world));
    };
    auto single = run(0), four = run(4), many = run(12);
    CHECK(single.first.size() >= 3);
    CHECK(single == four);
    CHECK(single == many);
}

TEST_CASE("on impact: nothing breaks without the system, and its config decides what the shards do") {
    // A tonne at 30 m/s into a chalk wall that asked to break; what the ball is going at afterwards.
    auto throwAt = [](std::optional<runtime::ImpactFractureConfig> config, std::vector<projv::BodyBroken>* fractures) {
        Fixture f(-1, 3000, config);
        runtime::PhysicsMaterial chalk{300.0f, 0.6f, 0.0f};
        chalk.strength = 1e5f;
        runtime::definePhysicsMaterial(f.app.world, "chalk", chalk);
        f.wall("chalk", true);
        f.frames(2);
        Entity rock = ball(f, vec3(0, 3, -6), vec3(0, 0, 30), 1000.0f);
        for (int i = 0; i < 30 && f.fractures.empty(); i++) f.frames(1);
        f.frames(1);
        if (fractures) *fractures = f.fractures;
        // Shards take after the wall, which breaks on impact: so do they.
        for (const projv::BodyBroken& e : f.fractures) {
            CHECK(e.cause == projv::BreakCause::Impact);
            for (Entity shard : e.pieces)
                if (f.app.world.valid(shard)) CHECK(f.app.world.get<projv::RigidBody>(shard).breaksOnImpact);
        }
        return runtime::bodyPose(f.app.world, rock).linearVelocity.z;
    };
    std::vector<projv::BodyBroken> fractures;
    // No system: the ball stops at the wall, which stays whole.
    CHECK(throwAt(std::nullopt, &fractures) < 1.0f);
    CHECK(fractures.empty());
    // The defaults: it breaks through and carries on.
    const float through = throwAt(runtime::ImpactFractureConfig{}, &fractures);
    CHECK(fractures.size() >= 1);
    CHECK(through > 15.0f);
    // Given nothing back.
    runtime::ImpactFractureConfig keep;
    keep.returnUnusedEnergy = false;
    const float stopped = throwAt(keep, &fractures);
    CHECK(fractures.size() >= 1);
    CHECK(stopped < through - 5.0f);
}

TEST_CASE("impacts name the entities that met, after each step") {
    Fixture f(-1, 3000, std::nullopt);
    Entity wall = f.wall();
    f.frames(2);
    Entity rock = ball(f, vec3(0, 3, -3), vec3(0, 0, 20), 400.0f);
    std::vector<runtime::BodyImpact> seen;
    f.app.addSystem(projv::Stage::FixedUpdate, "test: impacts", [&seen](projv::Application& app) {
        for (const runtime::BodyImpact& hit : runtime::impacts(app.world)) seen.push_back(hit);
    }, {.after = "physics"});
    f.frames(30);
    REQUIRE(seen.size() >= 1);
    const runtime::BodyImpact& hit = seen.front();
    CHECK(((hit.a == rock && hit.b == wall) || (hit.a == wall && hit.b == rock)));
    CHECK(hit.speed > 15.0f);
    CHECK(hit.energy == doctest::Approx(0.5f * 400.0f * hit.speed * hit.speed).epsilon(0.02));
}

TEST_CASE("a strike by reach breaks out to its radius whatever the strength, and a strike can ask for a least break") {
    auto broken = [](float strength, bool byReach) {
        Fixture f;
        runtime::PhysicsMaterial rock{2400.0f, 0.7f, 0.0f};
        rock.strength = strength;
        rock.shardSize = 0.5f;
        runtime::definePhysicsMaterial(f.app.world, "rock", rock);
        Entity wall = f.wall("rock");
        f.frames(2);
        if (byReach) runtime::fractureWithin(f.app.world, wall, vec3(0, 3, 0), 1.0f);
        else runtime::fracture(f.app.world, wall, vec3(0, 3, 0), Fixture::energyFor(1.0f));
        f.frames(1);
        REQUIRE(f.fractures.size() == 1);
        return f.shardVoxels(f.fractures[0]);
    };
    // Energy for a metre in stone (1e5 J/m^3): in rock ten times stronger it breaks much less; by
    // reach, the same either way.
    CHECK(float(broken(1e6f, false)) < 0.5f * float(broken(1e5f, false)));
    const float soft = float(broken(1e5f, true)), hard = float(broken(1e6f, true));
    CHECK(hard > 0.7f * soft);
    CHECK(hard < 1.4f * soft);

    Fixture f;
    Entity wall = f.wall("stone", true);
    f.frames(2);
    // 0.2 m is less than stone's half-metre shard: asked for at least one shard, nothing breaks.
    runtime::Strike knock;
    knock.at = vec3(0, 3, 0);
    knock.energy = Fixture::energyFor(0.2f);
    knock.minimumBreak = 1.0f;
    runtime::fracture(f.app.world, wall, knock);
    f.frames(2);
    CHECK(f.fractures.empty());
    // A static collider's shards take its breaksOnImpact; a handler of the event, which comes before
    // their bodies are made, can say otherwise.
    runtime::fracture(f.app.world, wall, vec3(-2, 3, 0), Fixture::energyFor(0.8f));
    f.frames(1);
    REQUIRE(f.fractures.size() == 1);
    for (Entity e : f.fractures[0].pieces) CHECK(f.app.world.get<projv::RigidBody>(e).breaksOnImpact);
    bool hadBodies = false;
    f.app.events().on<projv::BodyBroken>([&](const projv::BodyBroken& e) {
        for (Entity piece : e.pieces) {
            hadBodies = hadBodies || runtime::hasBody(f.app.world, piece);
            f.app.world.patch<projv::RigidBody>(piece, [](projv::RigidBody& r) { r.breaksOnImpact = false; });
        }
    });
    runtime::fracture(f.app.world, wall, vec3(2, 3, 0), Fixture::energyFor(0.8f));
    f.frames(1);
    REQUIRE(f.fractures.size() == 2);
    CHECK_FALSE(hadBodies);
    for (Entity e : f.fractures[1].pieces) {
        CHECK_FALSE(f.app.world.get<projv::RigidBody>(e).breaksOnImpact);
        CHECK(runtime::hasBody(f.app.world, e));
    }
}

TEST_CASE("fracture's guards are the game's to set, and stay guards") {
    auto strike = [](runtime::PhysicsConfig::FractureLimits limits, vec3 at, float radius) {
        Fixture f(-1, 3000, runtime::ImpactFractureConfig{}, limits);
        Entity wall = f.wall();
        f.frames(2);
        runtime::fracture(f.app.world, wall, at, Fixture::energyFor(radius));
        f.frames(1);
        return f.fractures.empty() ? std::make_pair(size_t(0), uint64_t(0))
                                   : std::make_pair(f.fractures[0].pieces.size(), f.shardVoxels(f.fractures[0]));
    };
    const runtime::PhysicsConfig::FractureLimits defaults;
    const auto plain = strike(defaults, vec3(0, 3, 0), 1.5f);
    REQUIRE(plain.first > 2);
    // At most two shards: the rest of what would break stays.
    runtime::PhysicsConfig::FractureLimits few;
    few.maxShards = 2;
    const auto capped = strike(few, vec3(0, 3, 0), 1.5f);
    CHECK(capped.first == 2);
    CHECK(capped.second < plain.second);
    // A reach of at most two voxels, half a metre.
    runtime::PhysicsConfig::FractureLimits near;
    near.maxReachVoxels = 2.0f;
    const auto small = strike(near, vec3(0, 3, 0), 1.5f);
    CHECK(small.second > 0);
    CHECK(small.second * 4 < plain.second);
    // A metre off the wall (four voxels) misses at three, and strikes at ten -- the reach still
    // measured from where the strike is.
    CHECK(strike(defaults, vec3(0, 3, -1), 1.5f).first == 0);
    runtime::PhysicsConfig::FractureLimits wide;
    wide.searchVoxels = 10.0f;
    CHECK(strike(wide, vec3(0, 3, -1), 1.5f).first > 0);
    // Out of range is clamped, never off: no shards at all is one.
    runtime::PhysicsConfig::FractureLimits none;
    none.maxShards = 0;
    none.maxReachVoxels = std::numeric_limits<float>::infinity();
    CHECK(strike(none, vec3(0, 3, 0), 1.5f).first == 1);
}

TEST_CASE("the piece budget clears the oldest pieces first, and only pieces") {
    Fixture f(-1, 20);
    Entity wall = f.wall();
    f.frames(2);
    // A small break, within the budget.
    runtime::fracture(f.app.world, wall, vec3(-2, 3, 0), Fixture::energyFor(0.6f));
    f.frames(1);
    REQUIRE(f.fractures.size() == 1);
    const std::vector<Entity> first = f.fractures[0].pieces;
    REQUIRE(first.size() >= 2);
    REQUIRE(first.size() <= 20);
    for (Entity e : first) CHECK(f.app.world.all_of<projv::Piece>(e));
    // A shard kept for good: no longer a piece.
    f.app.world.remove<projv::Piece>(first.front());
    f.frames(5);
    // A big one, past it: the newest twenty stay, every older shard goes but the one kept.
    runtime::fracture(f.app.world, wall, vec3(2, 3, 0), Fixture::energyFor(1.5f));
    f.frames(1);
    REQUIRE(f.fractures.size() == 2);
    const std::vector<Entity>& second = f.fractures[1].pieces;
    REQUIRE(second.size() > 20);
    CHECK(f.app.world.view<projv::Piece>().size() == 20);
    CHECK(f.app.world.valid(first.front()));
    for (size_t i = 1; i < first.size(); i++) CHECK_FALSE(f.app.world.valid(first[i]));
    CHECK(f.app.world.valid(wall));
    // Their bodies went with them, at once.
    CHECK(runtime::physicsWorld(f.app.world).stats().bodies == f.app.world.view<projv::PhysicsBody>().size());
}

namespace {
    // Removal ops for a slab of the wall, x and y in voxels, through its whole thickness.
    std::vector<projv::PendingVoxelOp> slot(ivec3 low, ivec3 high) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) ops.push_back({false, ivec3(x, y, z), 0});
        return ops;
    }
}

TEST_CASE("the piece budget names what it clears first, and a handler can keep one") {
    Fixture f(-1, 20);
    Entity wall = f.wall();
    f.frames(2);
    runtime::fracture(f.app.world, wall, vec3(-2, 3, 0), Fixture::energyFor(0.6f));
    f.frames(1);
    REQUIRE(f.fractures.size() == 1);
    const std::vector<Entity> first = f.fractures[0].pieces;
    REQUIRE(first.size() >= 2);
    // Whole when named, and the oldest first; the handler keeps the last it is told of.
    std::vector<Entity> named;
    bool alive = true;
    f.app.events().on<projv::PiecesCleared>([&](const projv::PiecesCleared& e) {
        for (Entity x : e.entities) {
            // (The newest may not have bodies yet: the budget is kept in the step that made them.)
            alive = alive && f.app.world.valid(x) && f.app.world.all_of<projv::Piece>(x);
            named.push_back(x);
        }
        if (!e.entities.empty()) f.app.world.remove<projv::Piece>(e.entities.back());
    });
    f.frames(5);
    runtime::fracture(f.app.world, wall, vec3(2, 3, 0), Fixture::energyFor(1.5f));
    f.frames(1);
    REQUIRE(f.fractures.size() == 2);
    REQUIRE(named.size() >= first.size());
    CHECK(alive);
    for (size_t i = 0; i < first.size(); i++) CHECK(named[i] == first[i]);
    const Entity kept = named.back();
    CHECK(f.app.world.valid(kept));
    CHECK_FALSE(f.app.world.all_of<projv::Piece>(kept));
    f.frames(1);
    CHECK(runtime::hasBody(f.app.world, kept));
    for (size_t i = 0; i + 1 < named.size(); i++) CHECK_FALSE(f.app.world.valid(named[i]));
}

TEST_CASE("support: what a cut leaves standing on nothing falls, made of what it was") {
    Fixture f;
    Entity wall = f.wall();
    f.app.world.patch<projv::StaticCollider>(wall, [](projv::StaticCollider& c) {
        c.splits = true;
        c.anchorBase = true;
    });
    f.frames(2);
    // A hole that cuts nothing loose: nothing falls.
    runtime::editVoxels(f.app.world, wall, slot({10, 4, 0}, {20, 8, 1}));
    f.frames(2);
    CHECK(f.splits.empty());
    // A slot right across, a metre up: the 5 m above it is held by nothing.
    runtime::editVoxels(f.app.world, wall, slot({0, 4, 0}, {31, 4, 1}));
    f.frames(1);
    REQUIRE(f.splits.size() == 1);
    CHECK(f.splits[0].entity == wall);
    REQUIRE(f.splits[0].pieces.size() == 1);
    Entity top = f.splits[0].pieces[0];
    Count fallen = countOf(f.scene, f.app.world.get<projv::VoxelComponent>(top).handle);
    CHECK(fallen.voxels == uint64_t(32 * 19 * 2 - 11 * 4 * 2));   // y 5..23, less the hole's part above the slot
    CHECK(fallen.materials == std::set<std::string>{"stone"});
    CHECK(countOf(f.scene, f.wallComponent).voxels == uint64_t(32 * 4 * 2));   // y 0..3 stays
    CHECK(f.app.world.get<projv::RigidBody>(top).motion == runtime::MotionType::Dynamic);
    CHECK(f.app.world.all_of<projv::NetId>(top));
    const float before = runtime::bodyPose(f.app.world, top).centerOfMass.y;
    // It drops the slot's height, a quarter-metre, onto what stands.
    f.frames(30);
    CHECK(runtime::bodyPose(f.app.world, top).centerOfMass.y == doctest::Approx(before - 0.25f).epsilon(0.02));
}

TEST_CASE("support: cut away the whole base and all of it falls") {
    Fixture f;
    Entity wall = f.wall();
    f.app.world.patch<projv::StaticCollider>(wall, [](projv::StaticCollider& c) {
        c.splits = true;
        c.anchorBase = true;
    });
    f.frames(2);
    // The bottom course, y = 0, right across: nothing is left at the base to stand on.
    runtime::editVoxels(f.app.world, wall, slot({0, 0, 0}, {31, 0, 1}));
    f.frames(1);
    REQUIRE(f.splits.size() == 1);
    REQUIRE(f.splits[0].pieces.size() == 1);
    Entity rest = f.splits[0].pieces[0];
    CHECK(countOf(f.scene, f.app.world.get<projv::VoxelComponent>(rest).handle).voxels == uint64_t(32 * 23 * 2));
    CHECK(countOf(f.scene, f.wallComponent).voxels == 0);
    const float before = runtime::bodyPose(f.app.world, rest).centerOfMass.y;
    f.frames(30);
    CHECK(runtime::bodyPose(f.app.world, rest).centerOfMass.y < before - 0.1f);   // it dropped
}

TEST_CASE("support: a collider too big to look over whole finds what fell from the change outward") {
    // A level: a pedestal filling the lowest row of blocks (y 0..17, the ground), a tower on it, and one
    // voxel far off that makes its box ~500M cells -- past what the whole-body pass takes.
    Fixture f;
    ComponentHandle level = utils::addComponent(f.scene, projv::ComponentKind::Chunk, "level", INVALID_COMPONENT_HANDLE, 16, 0.25f);
    utils::queueVoxelAdd(f.scene, level, fill({0, 0, 0}, {11, 17, 11}, GREY));
    utils::queueVoxelAdd(f.scene, level, fill({4, 18, 4}, {7, 33, 7}, GREY));
    utils::queueVoxelAdd(f.scene, level, fill({5000, 0, 5000}, {5000, 0, 5000}, GREY));
    utils::updateScene(f.scene);
    REQUIRE(utils::setMaterialPhysics(f.scene, level, slotOf(f.scene, level, GREY), "stone"));
    Entity e = runtime::spawnComponent(f.app.world, level);
    projv::StaticCollider collider;
    collider.splits = true;
    collider.anchorBase = true;
    f.app.world.emplace<projv::StaticCollider>(e, collider);
    runtime::assignNetId(f.app.world, e);
    f.frames(2);
    // A hole in the ground cuts nothing loose.
    runtime::editVoxels(f.app.world, e, slot({0, 10, 0}, {11, 10, 2}));
    f.frames(2);
    CHECK(f.splits.empty());
    // The tower's bottom course: the 15 courses above it stand on nothing, and fall.
    runtime::editVoxels(f.app.world, e, slot({4, 18, 4}, {7, 18, 7}));
    f.frames(1);
    REQUIRE(f.splits.size() == 1);
    REQUIRE(f.splits[0].pieces.size() == 1);
    Entity tower = f.splits[0].pieces[0];
    CHECK(countOf(f.scene, f.app.world.get<projv::VoxelComponent>(tower).handle).voxels == uint64_t(4 * 15 * 4));
    const float before = runtime::bodyPose(f.app.world, tower).centerOfMass.y;
    f.frames(30);
    CHECK(runtime::bodyPose(f.app.world, tower).centerOfMass.y < before - 0.1f);
}

TEST_CASE("support: a collider that does not split keeps standing on nothing") {
    Fixture f;
    Entity wall = f.wall();
    f.frames(2);
    runtime::editVoxels(f.app.world, wall, slot({0, 4, 0}, {31, 4, 1}));
    f.frames(2);
    CHECK(f.splits.empty());
    CHECK(countOf(f.scene, f.wallComponent).voxels == uint64_t(32 * 23 * 2));
}

TEST_CASE("support: anchors are data -- an anchor material holds up what it is part of, and the base only when asked") {
    Fixture f;
    runtime::PhysicsMaterial bedrock{2700.0f, 0.8f, 0.0f};
    bedrock.anchor = true;
    runtime::definePhysicsMaterial(f.app.world, "bedrock", bedrock);
    // A band of bedrock through the wall, y 11..12; the rest stone. It splits, its base no anchor.
    Entity wall = f.wall();
    utils::queueVoxelAdd(f.scene, f.wallComponent, fill({0, 11, 0}, {31, 12, 1}, RED));
    utils::updateScene(f.scene);
    REQUIRE(utils::setMaterialPhysics(f.scene, f.wallComponent, slotOf(f.scene, f.wallComponent, RED), "bedrock"));
    f.app.world.patch<projv::StaticCollider>(wall, [](projv::StaticCollider& c) { c.splits = true; });
    f.frames(2);
    // A slot right across a metre up: the top hangs from the bedrock; the foot, under it, falls.
    runtime::editVoxels(f.app.world, wall, slot({0, 4, 0}, {31, 4, 1}));
    f.frames(1);
    REQUIRE(f.splits.size() == 1);
    REQUIRE(f.splits[0].pieces.size() == 1);
    Count fallen = countOf(f.scene, f.app.world.get<projv::VoxelComponent>(f.splits[0].pieces[0]).handle);
    CHECK(fallen.voxels == uint64_t(32 * 4 * 2));   // y 0..3
    CHECK(countOf(f.scene, f.wallComponent).voxels == uint64_t(32 * 19 * 2));   // y 5..23 stands

    // Neither an anchor nor its base: it says so, and nothing falls.
    Fixture g;
    Entity bare = g.wall();
    g.app.world.patch<projv::StaticCollider>(bare, [](projv::StaticCollider& c) { c.splits = true; });
    g.frames(2);
    runtime::editVoxels(g.app.world, bare, slot({0, 4, 0}, {31, 4, 1}));
    g.frames(2);
    CHECK(g.splits.empty());
    CHECK(countOf(g.scene, g.wallComponent).voxels == uint64_t(32 * 23 * 2));
}

TEST_CASE("support: a strike that breaks a pillar through brings the top down") {
    Fixture f;
    // A 1 m pillar 6 m high, of stone that splits.
    ComponentHandle h = utils::addComponent(f.scene, projv::ComponentKind::Chunk, "pillar", INVALID_COMPONENT_HANDLE, 16, 0.25f);
    utils::setComponentTransform(f.scene, h, vec3(-0.5f, 0, -0.5f), quat(1, 0, 0, 0), 1.0f);
    utils::queueVoxelAdd(f.scene, h, fill({0, 0, 0}, {3, 23, 3}, GREY));
    utils::updateScene(f.scene);
    REQUIRE(utils::setMaterialPhysics(f.scene, h, slotOf(f.scene, h, GREY), "stone"));
    Entity pillar = runtime::spawnComponent(f.app.world, h);
    projv::StaticCollider c;
    c.splits = true;
    c.anchorBase = true;
    f.app.world.emplace<projv::StaticCollider>(pillar, c);
    f.frames(2);
    runtime::fracture(f.app.world, pillar, vec3(0, 2.5f, -0.5f), Fixture::energyFor(1.2f));
    f.frames(2);
    // One pass: the shards and the top they leave unsupported come away together, in one event.
    REQUIRE(f.fractures.size() == 1);
    CHECK(f.splits.empty());
    CHECK(f.fractures[0].entity == pillar);
    // The top fell; the foot stands.
    float highest = 0.0f;
    for (Entity e : f.fractures[0].pieces) highest = std::max(highest, runtime::bodyPose(f.app.world, e).centerOfMass.y);
    CHECK(highest > 3.0f);
    CHECK(countOf(f.scene, h).voxels > 0);
}

namespace {
    // A free-floating stone bar, 4 m x 0.5 m x 0.5 m of quarter-metre voxels, at (0, 50, 0), moving
    // at `velocity` with no gravity.
    Entity bar(Fixture& f, vec3 velocity) {
        ComponentHandle h = utils::addComponent(f.scene, projv::ComponentKind::Chunk, "bar", INVALID_COMPONENT_HANDLE, 16, 0.25f);
        utils::setComponentTransform(f.scene, h, vec3(0, 50, 0), quat(1, 0, 0, 0), 1.0f);
        utils::queueVoxelAdd(f.scene, h, fill({0, 0, 0}, {15, 1, 1}, GREY));
        utils::updateScene(f.scene);
        REQUIRE(utils::setMaterialPhysics(f.scene, h, slotOf(f.scene, h, GREY), "stone"));
        Entity e = runtime::spawnComponent(f.app.world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        projv::RigidBody rigid;
        rigid.gravityFactor = 0.0f;
        rigid.linearDamping = 0.0f;
        rigid.mass = 40.0f;
        f.app.world.emplace<projv::RigidBody>(e, rigid);
        f.frames(1);
        runtime::setVelocity(f.app.world, e, velocity);
        f.frames(2);
        return e;
    }
}

TEST_CASE("one pass: a moving body struck through comes apart in one event, shards and the end the cut left") {
    Fixture f;
    Entity e = bar(f, vec3(0, 0, 2));
    const ComponentHandle h = f.app.world.get<projv::VoxelComponent>(e).handle;
    // A strike a metre from the end, wide enough to cut right through: the short end loses its
    // connection to the rest, and comes away with the shards.
    runtime::fractureWithin(f.app.world, e, vec3(1.0f, 50.25f, 0.25f), 0.6f);
    f.frames(1);
    REQUIRE(f.fractures.size() == 1);
    CHECK(f.splits.empty());
    const projv::BodyBroken& broke = f.fractures[0];
    CHECK(broke.cause == projv::BreakCause::Strike);
    uint64_t inPieces = 0, largest = 0;
    float mass = 0.0f;
    const runtime::PhysicsWorld& sim = runtime::physicsWorld(f.app.world);
    for (Entity piece : broke.pieces) {
        REQUIRE(runtime::hasBody(f.app.world, piece));
        CHECK(f.app.world.all_of<projv::Piece>(piece));
        uint64_t n = countOf(f.scene, f.app.world.get<projv::VoxelComponent>(piece).handle).voxels;
        inPieces += n;
        largest = std::max(largest, n);
        mass += sim.bodyMass(f.app.world.get<projv::PhysicsBody>(piece).id);
        CHECK(runtime::bodyPose(f.app.world, piece).linearVelocity.z == doctest::Approx(2.0f).epsilon(0.05));
    }
    const uint64_t kept = countOf(f.scene, h).voxels;
    CHECK(kept + inPieces == 16 * 2 * 2);
    CHECK(largest >= 4);   // the end, x 0..1 at least, is one piece
    CHECK(kept > largest);  // the body kept the long side
    mass += sim.bodyMass(f.app.world.get<projv::PhysicsBody>(e).id);
    CHECK(mass == doctest::Approx(40.0f).epsilon(0.02));   // shared out by voxel count
}

TEST_CASE("pieces take after what they came from: a moving body's shards break on impact if it did") {
    Fixture f;
    Entity e = bar(f, vec3(0));
    f.app.world.patch<projv::RigidBody>(e, [](projv::RigidBody& r) { r.breaksOnImpact = true; r.linearDamping = 0.3f; });
    f.frames(2);
    runtime::fracture(f.app.world, e, vec3(2.0f, 50.25f, 0.25f), Fixture::energyFor(0.5f));
    f.frames(1);
    REQUIRE(f.fractures.size() == 1);
    REQUIRE(!f.fractures[0].pieces.empty());
    for (Entity piece : f.fractures[0].pieces) {
        CHECK(f.app.world.get<projv::RigidBody>(piece).breaksOnImpact);
        CHECK(f.app.world.get<projv::RigidBody>(piece).linearDamping == 0.3f);
    }
}

TEST_CASE("dust: an island of PhysicsConfig::dustVoxels or fewer is removed, not made a body") {
    auto strike = [](uint32_t dust) {
        Fixture f(-1, 3000, runtime::ImpactFractureConfig{}, {}, dust);
        Entity wall = f.wall();
        f.frames(2);
        runtime::fracture(f.app.world, wall, vec3(0, 3, 0), Fixture::energyFor(1.2f));
        f.frames(1);
        REQUIRE(f.fractures.size() == 1);
        uint64_t smallest = std::numeric_limits<uint64_t>::max(), inPieces = 0;
        for (Entity e : f.fractures[0].pieces) {
            uint64_t n = countOf(f.scene, f.app.world.get<projv::VoxelComponent>(e).handle).voxels;
            smallest = std::min(smallest, n);
            inPieces += n;
        }
        const uint64_t left = countOf(f.scene, f.wallComponent).voxels;
        return std::make_tuple(f.fractures[0].pieces.size(), smallest, left + inPieces);
    };
    auto [allPieces, allSmallest, allVoxels] = strike(0);
    CHECK(allVoxels == 32 * 24 * 2);   // none lost
    REQUIRE(allSmallest <= 4);         // a strike makes small shards at its middle...
    auto [fewer, dustSmallest, dustVoxels] = strike(4);
    CHECK(dustSmallest > 4);           // ...which, as dust, are gone
    CHECK(fewer < allPieces);
    CHECK(dustVoxels < allVoxels);
}

TEST_CASE("a strike can break finer than its material: Strike::shardSize") {
    auto pieces = [](float shardSize) {
        Fixture f;
        Entity wall = f.wall();
        f.frames(2);
        runtime::Strike strike;
        strike.at = vec3(0, 3, 0);
        strike.energy = Fixture::energyFor(1.2f);
        strike.shardSize = shardSize;
        runtime::fracture(f.app.world, wall, strike);
        f.frames(1);
        REQUIRE(f.fractures.size() == 1);
        return f.fractures[0].pieces.size();
    };
    CHECK(pieces(0.25f) > pieces(0.0f) * 3 / 2);   // stone's is 0.5 m
}

TEST_CASE("the least impact break: a piece that fits in it is not broken by a hit, whatever the energy") {
    Fixture f;
    // Two voxels across (0.5 m): stone's shard size, so a cube of the least break by default.
    ComponentHandle h = utils::addComponent(f.scene, projv::ComponentKind::Chunk, "pebble", INVALID_COMPONENT_HANDLE, 16, 0.25f);
    utils::setComponentTransform(f.scene, h, vec3(0, 50, 0), quat(1, 0, 0, 0), 1.0f);
    utils::queueVoxelAdd(f.scene, h, fill({0, 0, 0}, {1, 1, 1}, GREY));
    utils::updateScene(f.scene);
    REQUIRE(utils::setMaterialPhysics(f.scene, h, slotOf(f.scene, h, GREY), "stone"));
    Entity pebble = runtime::spawnComponent(f.app.world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    projv::RigidBody rigid;
    rigid.gravityFactor = 0.0f;
    f.app.world.emplace<projv::RigidBody>(pebble, rigid);
    f.frames(2);
    runtime::Strike hit;
    hit.at = vec3(0.25f, 50.25f, 0.25f);
    hit.energy = 1e9f;
    hit.cause = projv::BreakCause::Impact;
    runtime::fracture(f.app.world, pebble, hit);
    f.frames(1);
    CHECK(f.fractures.empty());
    // The same blow, not an impact: it breaks.
    hit.cause = projv::BreakCause::Strike;
    runtime::fracture(f.app.world, pebble, hit);
    f.frames(1);
    CHECK(f.fractures.size() == 1);
}

TEST_CASE("shards are made flush, not overlapping: without gravity, thrown out of the wall, they keep their speed") {
    // Cut from one solid, the shards start touching each other and the wall on every side. If they
    // started overlapping, the contacts would push them apart and friction would grind them to a stop
    // (a hole's shards held by friction under gravity is the weight of the stack, not this).
    Fixture f;
    runtime::setGravity(f.app.world, vec3(0));
    Entity wall = f.wall();
    f.frames(2);
    std::vector<Entity> shards;
    f.app.events().on<projv::BodyBroken>([&](const projv::BodyBroken& e) {
        for (size_t i = 0; i < e.shards; i++) {
            shards.push_back(e.pieces[i]);
            runtime::setVelocity(f.app.world, e.pieces[i], vec3(0, 0, -6), vec3(0));   // out of the face
        }
    });
    runtime::fracture(f.app.world, wall, vec3(0, 3, 0), Fixture::energyFor(1.2f));
    f.frames(4);
    REQUIRE(shards.size() > 10);
    for (Entity e : shards)
        if (runtime::hasBody(f.app.world, e)) CHECK(runtime::bodyPose(f.app.world, e).linearVelocity.z < -5.9f);
}

TEST_CASE("impacts: a body's own minImpactSpeed overrides the world's, higher or lower; of two, the lower") {
    auto hits = [](float a, float b, float drop) {
        runtime::PhysicsWorld sim;
        runtime::BodyDesc floor{.shape = runtime::PhysicsShape::box({20, 1, 20}), .motion = runtime::MotionType::Static,
                                .layer = runtime::PhysicsLayer::Static, .position = {0, -1, 0}};
        floor.minImpactSpeed = b;
        sim.createBody(floor);
        runtime::BodyDesc box{.shape = runtime::PhysicsShape::box({0.5f, 0.5f, 0.5f}), .position = {0, 0.5f + drop, 0}};
        box.minImpactSpeed = a;
        sim.createBody(box);
        int n = 0;
        for (int i = 0; i < 90; i++) { sim.step(1.0f / 60.0f); n += int(sim.impacts().size()); }
        return n;
    };
    // From 0.3 m it lands at about 2.4 m/s; the world's threshold is 1 m/s.
    CHECK(hits(-1, -1, 0.3f) == 1);
    CHECK(hits(5, -1, 0.3f) == 0);    // a boulder that does not count light knocks
    CHECK(hits(-1, 5, 0.3f) == 0);    // the same, set on the floor
    CHECK(hits(5, 0.5f, 0.3f) == 1);  // two set: the lower
    // From 4 cm, about 0.6 m/s as the contact begins (2 cm out, Jolt's speculative distance): under
    // the world's, over a glass cup's.
    CHECK(hits(-1, -1, 0.04f) == 0);
    CHECK(hits(0.2f, -1, 0.04f) == 1);
}

TEST_CASE("islands: at most maxIslands come away in one pass, and the rest at the next change") {
    runtime::PhysicsConfig::FractureLimits limits;
    limits.maxIslands = 3;
    Fixture f(-1, 3000, runtime::ImpactFractureConfig{}, limits);
    Entity wall = f.wall();
    f.app.world.patch<projv::StaticCollider>(wall, [](projv::StaticCollider& c) {
        c.splits = true;
        c.anchorBase = true;
    });
    f.frames(2);
    // A slot right across, a metre up, and seven up through the top: eight columns, held by nothing.
    std::vector<projv::PendingVoxelOp> cuts = slot({0, 4, 0}, {31, 4, 1});
    for (int x = 3; x < 31; x += 4) {
        std::vector<projv::PendingVoxelOp> up = slot({x, 5, 0}, {x, 23, 1});
        cuts.insert(cuts.end(), up.begin(), up.end());
    }
    runtime::editVoxels(f.app.world, wall, cuts);
    f.frames(1);
    REQUIRE(f.splits.size() == 1);
    CHECK(f.splits[0].pieces.size() == 3);
    // Any later change to the wall looks again: three more, then the last two.
    runtime::editVoxels(f.app.world, wall, slot({0, 0, 0}, {0, 0, 0}));
    f.frames(1);
    REQUIRE(f.splits.size() == 2);
    CHECK(f.splits[1].pieces.size() == 3);
    runtime::editVoxels(f.app.world, wall, slot({31, 0, 0}, {31, 0, 0}));
    f.frames(1);
    REQUIRE(f.splits.size() == 3);
    CHECK(f.splits[2].pieces.size() == 2);
}

TEST_CASE("minImpactSpeed saves on RigidBody and StaticCollider only when set") {
    using projv::runtime::ComponentTraits;
    projv::RigidBody cup;
    cup.minImpactSpeed = 0.3f;
    auto rigid = ComponentTraits<projv::RigidBody>::load(ComponentTraits<projv::RigidBody>::save(cup), 1);
    REQUIRE(rigid);
    CHECK(rigid->minImpactSpeed == doctest::Approx(0.3f));
    CHECK_FALSE(ComponentTraits<projv::RigidBody>::save(projv::RigidBody{}).contains("minImpactSpeed"));
    projv::StaticCollider floor;
    floor.minImpactSpeed = 0.0f;   // every contact
    nlohmann::json j = ComponentTraits<projv::StaticCollider>::save(floor);
    CHECK(j["minImpactSpeed"] == 0.0);
    auto fixed = ComponentTraits<projv::StaticCollider>::load(j, 1);
    REQUIRE(fixed);
    CHECK(fixed->minImpactSpeed == 0.0f);
    CHECK(ComponentTraits<projv::StaticCollider>::load(nlohmann::json::object(), 1)->minImpactSpeed < 0.0f);
}

TEST_CASE("a split a server made, applied on a client, makes the same pieces with the same NetIds") {
    // Two worlds from the same start: the server breaks its wall; the client is told what came away.
    Fixture server, client;
    Entity serverWall = server.wall();
    Entity clientWall = client.wall();
    REQUIRE(server.app.world.get<projv::NetId>(serverWall) == client.app.world.get<projv::NetId>(clientWall));
    server.frames(2);
    client.frames(2);

    runtime::fracture(server.app.world, serverWall, vec3(0, 3, 0), Fixture::energyFor(1.2f), vec3(0, 0, 5));
    server.frames(1);
    REQUIRE(server.fractures.size() == 1);
    const projv::BodyBroken& broke = server.fractures[0];
    runtime::VoxelSplit split = runtime::voxelSplitOf(server.app.world, broke);
    REQUIRE(split.pieces.size() == broke.pieces.size());
    REQUIRE(split.pieces.size() > 3);

    runtime::applyVoxelSplit(client.app.world, clientWall, split);
    client.frames(1);
    REQUIRE(client.splits.size() == 1);
    CHECK(client.splits[0].cause == projv::BreakCause::Replicated);
    CHECK(client.splits[0].shards == broke.shards);
    // The wall that is left, voxel for voxel the same count; every piece there, by NetId, as big.
    CHECK(countOf(client.scene, client.wallComponent).voxels == countOf(server.scene, server.wallComponent).voxels);
    for (const runtime::VoxelSplit::Piece& piece : split.pieces) {
        REQUIRE(piece.netId.value != 0);
        Entity e = runtime::entityForNetId(client.app.world, piece.netId);
        REQUIRE(e != projv::NullEntity);
        CHECK(countOf(client.scene, client.app.world.get<projv::VoxelComponent>(e).handle).voxels == piece.voxels.size());
        CHECK(client.app.world.all_of<projv::Piece>(e));
    }
    // And they move as the server's did when made.
    client.frames(1);
    server.frames(1);
    for (size_t i = 0; i < split.pieces.size(); i++) {
        Entity mine = runtime::entityForNetId(client.app.world, split.pieces[i].netId);
        CHECK(runtime::hasBody(client.app.world, mine));
        CHECK(glm::distance(runtime::bodyPose(client.app.world, mine).position,
                            runtime::bodyPose(server.app.world, broke.pieces[i]).position) < 0.05f);
    }
}

TEST_CASE("a split naming voxels the client does not have skips them and cuts the rest") {
    Fixture f;
    Entity wall = f.wall();
    f.frames(2);
    runtime::VoxelSplit split;
    runtime::VoxelSplit::Piece piece;
    for (int x = 0; x < 4; x++) piece.voxels.push_back(ivec3(x, 23, 0));   // the top corner of the face
    piece.voxels.push_back(ivec3(100, 100, 100));                           // not in the wall
    piece.netId = projv::NetId{900};
    split.pieces.push_back(piece);
    split.dust.push_back(ivec3(10, 23, 1));
    runtime::applyVoxelSplit(f.app.world, wall, split);
    f.frames(1);
    CHECK(countOf(f.scene, f.wallComponent).voxels == 32 * 24 * 2 - 5);
    Entity e = runtime::entityForNetId(f.app.world, projv::NetId{900});
    REQUIRE(e != projv::NullEntity);
    CHECK(countOf(f.scene, f.app.world.get<projv::VoxelComponent>(e).handle).voxels == 4);
}
