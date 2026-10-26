// Physics materials (the physics plan, "Physics materials"): a project library of named materials,
// named by palette entries and bodies. Looks and physics are separate: palette entries that differ
// only in colour but name one material merge into the same boxes, and entries alike in colour that
// name different materials stay apart. Checked against what the materials should do -- slide
// distances against v^2 / (2 mu g), bounce heights against e^2 h, a mixed body's mass and centre of
// mass against its parts.

#include "doctest/doctest.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/application.h"
#include "runtime/physics.h"
#include "utils/collision_geometry.h"
#include "utils/compose_io.h"
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
    constexpr uint32_t RED = 0x3FF00000u, GREEN = 0x000FFC00u, BLUE = 0x000003FFu, GREY = 0x20080200u;

    void fill(std::vector<projv::PendingVoxelOp>& ops, ivec3 low, ivec3 high, uint32_t colour) {
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) ops.push_back({true, ivec3(x, y, z), colour});
    }

    ComponentHandle chunkOf(projv::Scene& scene, const std::vector<projv::PendingVoxelOp>& ops, vec3 at, float voxel) {
        ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "c", INVALID_COMPONENT_HANDLE, 16, voxel);
        utils::setComponentTransform(scene, h, at, quat(1, 0, 0, 0), 1.0f);
        utils::queueVoxelAdd(scene, h, ops);
        utils::updateScene(scene);
        return h;
    }

    // The first slot of that colour, whatever it is made of.
    uint8_t slotOf(const projv::Scene& scene, ComponentHandle h, uint32_t colour) {
        const auto& palette = scene.components[h].materialPalette;
        for (size_t i = 0; i < palette.size(); i++)
            if (palette[i].packedColor == colour) return uint8_t(i);
        FAIL("no slot of that colour");
        return 0;
    }

    struct Fixture {
        projv::Application app;
        projv::Scene& scene;
        double now = 0.0;
        explicit Fixture(bool record = false) : scene(app.world.ctx().emplace<projv::Scene>()) {
            app.clock = [this] { return now; };
            runtime::installSceneBridge(app);
            runtime::PhysicsConfig config;
            config.settings.record = record;
            runtime::installPhysics(app, config);
            app.runStartup();
            app.runFrame();
            runtime::definePhysicsMaterial(app.world, "ice", {917.0f, 0.05f, 0.0f});
            runtime::definePhysicsMaterial(app.world, "stone", {2000.0f, 0.5f, 0.0f});
            runtime::definePhysicsMaterial(app.world, "wood", {500.0f, 0.6f, 0.0f});
            runtime::definePhysicsMaterial(app.world, "rubber", {1100.0f, 1.0f, 0.8f});
            runtime::definePhysicsMaterial(app.world, "clay", {1800.0f, 0.9f, 0.0f});
        }
        void frames(int n) { for (int i = 0; i < n; i++) { now += DT; app.runFrame(); } }

        // A 20 m floor of half-metre voxels, top at y = 0: its z < 0 half one palette entry, its
        // z >= 0 half another -- the same grey both, so they look alike -- naming `near` and `far`.
        ComponentHandle floorComponent = INVALID_COMPONENT_HANDLE;
        Entity floor(const std::string& nearby, const std::string& farther) {
            std::vector<projv::PendingVoxelOp> ops;
            fill(ops, {0, 0, 0}, {39, 1, 19}, GREY);
            fill(ops, {0, 0, 20}, {39, 1, 39}, GREEN);
            floorComponent = chunkOf(scene, ops, vec3(-10, -1, -10), 0.5f);
            uint8_t a = slotOf(scene, floorComponent, GREY), b = slotOf(scene, floorComponent, GREEN);
            REQUIRE(utils::setMaterialColor(scene, floorComponent, b, GREY));   // alike in look...
            REQUIRE(utils::setMaterialPhysics(scene, floorComponent, a, nearby)); // ...not in what they are made of
            REQUIRE(utils::setMaterialPhysics(scene, floorComponent, b, farther));
            Entity e = runtime::spawnComponent(app.world, floorComponent);
            app.world.emplace<projv::StaticCollider>(e);
            return e;
        }
        // A 1 m cube of quarter-metre voxels, its corner at `at`.
        Entity block(vec3 at, projv::RigidBody rigid = {}) {
            std::vector<projv::PendingVoxelOp> ops;
            fill(ops, {0, 0, 0}, {3, 3, 3}, RED);
            Entity e = runtime::spawnComponent(app.world, chunkOf(scene, ops, at, 0.25f), projv::LinkMode::Root,
                                               projv::OnUnlink::Destroy);
            app.world.emplace<projv::RigidBody>(e, rigid);
            return e;
        }
        runtime::BodyState pose(Entity e) { return runtime::bodyPose(app.world, e); }
        const projv::PhysicsBody& body(Entity e) { return app.world.get<projv::PhysicsBody>(e); }
    };
}

TEST_CASE("pieces stop at material boundaries, not at palette slots") {
    projv::Scene scene;
    // A 12 x 4 x 4 bar in three colour stripes.
    std::vector<projv::PendingVoxelOp> ops;
    fill(ops, {0, 0, 0}, {3, 3, 3}, RED);
    fill(ops, {4, 0, 0}, {7, 3, 3}, GREEN);
    fill(ops, {8, 0, 0}, {11, 3, 3}, BLUE);
    ComponentHandle h = chunkOf(scene, ops, vec3(0), 0.25f);
    const projv::GeometryBlob& blob = scene.geometryPool[size_t(scene.chunks[scene.components[h].chunkHandle].geometryPoolIndex)];
    uint8_t red = slotOf(scene, h, RED), green = slotOf(scene, h, GREEN), blue = slotOf(scene, h, BLUE);

    utils::CollisionPieces plain = utils::buildCollisionPieces(blob, 16);
    REQUIRE(plain.pieces.size() == 1);
    CHECK(plain.pieces[0].material == 0);

    // Three looks, one stone: still one box.
    utils::SlotMaterials one{};
    one[red] = one[green] = one[blue] = 7;
    utils::CollisionPieces stone = utils::buildCollisionPieces(blob, 16, {}, &one);
    REQUIRE(stone.pieces.size() == 1);
    CHECK(stone.pieces[0].material == 7);
    CHECK(stone.pieces[0].voxelMax == ivec3(11, 3, 3));

    // The middle stripe wood: three boxes, each of one material, covering what each material has.
    utils::SlotMaterials two = one;
    two[green] = 9;
    utils::CollisionPieces mixed = utils::buildCollisionPieces(blob, 16, {}, &two);
    REQUIRE(mixed.pieces.size() == 3);
    uint64_t stoneVoxels = 0, woodVoxels = 0;
    for (const utils::CollisionPiece& p : mixed.pieces) {
        ivec3 n = p.voxelMax - p.voxelMin + ivec3(1);
        (p.material == 9 ? woodVoxels : stoneVoxels) += uint64_t(n.x * n.y * n.z);
        CHECK((p.material == 7 || p.material == 9));
        if (p.material == 9) CHECK((p.voxelMin.x == 4 && p.voxelMax.x == 7));
    }
    CHECK(stoneVoxels == 128);
    CHECK(woodVoxels == 64);
}

TEST_CASE("a palette entry is the one asked for only if its physics material matches too") {
    projv::Scene scene;
    ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "c", INVALID_COMPONENT_HANDLE, 16, 0.25f);
    projv::ComponentRecord& c = scene.components[h];
    uint8_t plain = utils::internMaterial(scene, c, "", RED);
    uint8_t wood = utils::internMaterial(scene, c, "", RED, "wood");
    uint8_t stone = utils::internMaterial(scene, c, "", RED, "stone");
    CHECK(plain != wood);
    CHECK(wood != stone);
    CHECK(utils::internMaterial(scene, c, "", RED, "wood") == wood);
    CHECK(utils::internMaterial(scene, c, "", RED) == plain);
    CHECK(utils::findMaterialByColor(c, RED, "stone") == stone);
    CHECK(c.materialPalette[wood].packedColor == c.materialPalette[stone].packedColor);
}

TEST_CASE("the library: indices in definition order, redefinition in place, bad values refused") {
    Fixture f;
    CHECK(runtime::physicsMaterialIndex(f.app.world, "ice") == 1);
    CHECK(runtime::physicsMaterialIndex(f.app.world, "clay") == 5);
    CHECK(runtime::physicsMaterialIndex(f.app.world, "glass") == 0);
    uint64_t before = runtime::physicsStateHash(f.app.world);
    CHECK(runtime::definePhysicsMaterial(f.app.world, "ice", {917.0f, 0.02f, 0.0f}) == 1);
    CHECK(runtime::physicsMaterial(f.app.world, "ice")->friction == doctest::Approx(0.02f));
    CHECK(runtime::physicsStateHash(f.app.world) != before);   // the library is part of the state
    CHECK(runtime::definePhysicsMaterial(f.app.world, "foam", {-1.0f, 0.5f, 0.0f}) == 0);
    CHECK(runtime::definePhysicsMaterial(f.app.world, "foam", {50.0f, NAN, 0.0f}) == 0);
    CHECK(runtime::definePhysicsMaterial(f.app.world, "", {}) == 0);
    CHECK(runtime::physicsMaterialIndex(f.app.world, "foam") == 0);
    CHECK(runtime::definePhysicsMaterial(f.app.world, "spring", {500.0f, 0.5f, 3.0f}) == 6);
    CHECK(runtime::physicsMaterial(f.app.world, "spring")->restitution == 1.0f);   // clamped

    auto path = std::filesystem::temp_directory_path() / "projv_physics_materials_test.json";
    {
        std::ofstream out(path);
        out << R"({ "v": 1, "materials": [ { "name": "steel", "density": 7800, "friction": 0.4 },
                                           { "name": "ice", "friction": 0.03 } ] })";
    }
    REQUIRE(runtime::loadPhysicsMaterials(f.app.world, path.string()));
    CHECK(runtime::physicsMaterialIndex(f.app.world, "steel") == 7);
    CHECK(runtime::physicsMaterial(f.app.world, "steel")->density == 7800.0f);
    CHECK(runtime::physicsMaterial(f.app.world, "ice")->friction == doctest::Approx(0.03f));
    {
        std::ofstream out(path);
        out << R"({ "v": 1, "materials": [ { "density": 5 } ] })";
    }
    CHECK_FALSE(runtime::loadPhysicsMaterials(f.app.world, path.string()));
    CHECK_FALSE(runtime::loadPhysicsMaterials(f.app.world, (path.string() + ".missing")));
    std::filesystem::remove(path);
}

TEST_CASE("a body of two materials weighs what each half does, and balances where they do") {
    Fixture f;
    // A 2 m bar of half-metre-square section: x 0..1 m stone, 1..2 m wood. Two colours.
    std::vector<projv::PendingVoxelOp> ops;
    fill(ops, {0, 0, 0}, {3, 1, 1}, RED);
    fill(ops, {4, 0, 0}, {7, 1, 1}, GREEN);
    ComponentHandle h = chunkOf(f.scene, ops, vec3(0, 5, 0), 0.25f);
    REQUIRE(utils::setMaterialPhysics(f.scene, h, slotOf(f.scene, h, RED), "stone"));
    REQUIRE(utils::setMaterialPhysics(f.scene, h, slotOf(f.scene, h, GREEN), "wood"));
    Entity bar = runtime::spawnComponent(f.app.world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    projv::RigidBody rigid;
    rigid.gravityFactor = 0.0f;
    f.app.world.emplace<projv::RigidBody>(bar, rigid);
    f.frames(2);
    // Each half is 0.25 m^3: 500 kg of stone, 125 kg of wood.
    CHECK(runtime::physicsWorld(f.app.world).bodyMass(f.body(bar).id) == doctest::Approx(625.0f).epsilon(1e-3));
    runtime::BodyState s = f.pose(bar);
    CHECK(s.centerOfMass.x - s.position.x == doctest::Approx((500.0f * 0.5f + 125.0f * 1.5f) / 625.0f).epsilon(1e-3));
    CHECK(s.centerOfMass.y - s.position.y == doctest::Approx(0.25f).epsilon(1e-3));
    REQUIRE(f.body(bar).voxelParts.size() == 1);
    if (!f.body(bar).voxelParts[0].shape->tiles) CHECK(f.body(bar).voxelParts[0].shape->pieces == 2);   // boxes' count (tiles: none)

    // The body's own material weighs the voxels whose entry names none: wood all through, now.
    REQUIRE(utils::setMaterialPhysics(f.scene, h, slotOf(f.scene, h, RED), ""));
    f.app.world.patch<projv::RigidBody>(bar, [](projv::RigidBody& r) { r.material = "wood"; });
    f.frames(2);
    CHECK(runtime::physicsWorld(f.app.world).bodyMass(f.body(bar).id) == doctest::Approx(250.0f).epsilon(1e-3));
}

TEST_CASE("three colours of one material collide as one; renaming an entry's material rebuilds the body") {
    Fixture f;
    f.floor("stone", "stone");
    std::vector<projv::PendingVoxelOp> ops;
    fill(ops, {0, 0, 0}, {3, 3, 3}, RED);
    fill(ops, {4, 0, 0}, {7, 3, 3}, GREEN);
    fill(ops, {8, 0, 0}, {11, 3, 3}, BLUE);
    ComponentHandle h = chunkOf(f.scene, ops, vec3(-1.5f, 0.01f, -0.5f), 0.25f);
    for (uint32_t colour : {RED, GREEN, BLUE})
        REQUIRE(utils::setMaterialPhysics(f.scene, h, slotOf(f.scene, h, colour), "stone"));
    Entity e = runtime::spawnComponent(f.app.world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    f.app.world.emplace<projv::RigidBody>(e);
    f.frames(2);
    REQUIRE(f.app.world.all_of<projv::PhysicsBody>(e));
    if (!f.body(e).voxelParts[0].shape->tiles) CHECK(f.body(e).voxelParts[0].shape->pieces == 1);   // boxes' count (tiles: none)
    float stoneMass = runtime::physicsWorld(f.app.world).bodyMass(f.body(e).id);
    CHECK(stoneMass == doctest::Approx(2000.0f * 3.0f).epsilon(1e-3));   // 3 m^3
    runtime::BodyId first = f.body(e).id;

    REQUIRE(utils::setMaterialPhysics(f.scene, h, slotOf(f.scene, h, GREEN), "wood"));
    f.frames(2);
    CHECK_FALSE(f.body(e).id == first);   // rebuilt
    if (!f.body(e).voxelParts[0].shape->tiles) CHECK(f.body(e).voxelParts[0].shape->pieces == 3);   // boxes' count (tiles: none)
    CHECK(runtime::physicsWorld(f.app.world).bodyMass(f.body(e).id) ==
          doctest::Approx(2000.0f * 2.0f + 500.0f * 1.0f).epsilon(1e-3));
    // Redefining a material it uses rebuilds it too, with the new density.
    runtime::definePhysicsMaterial(f.app.world, "wood", {700.0f, 0.6f, 0.0f});
    f.frames(2);
    CHECK(runtime::physicsWorld(f.app.world).bodyMass(f.body(e).id) ==
          doctest::Approx(2000.0f * 2.0f + 700.0f * 1.0f).epsilon(1e-3));
}

TEST_CASE("a block slides as far as the floor's material lets it: v^2 / (2 mu g)") {
    Fixture f;
    f.floor("ice", "stone");   // alike in look; z < 0 ice, z >= 0 stone
    projv::RigidBody rigid;
    rigid.friction = 1.0f;     // the floor decides: combined friction is sqrt(1 * floor's)
    rigid.linearDamping = 0.0f;
    Entity onIce = f.block(vec3(-8, 0.0f, -4.5f), rigid);
    Entity onStone = f.block(vec3(-8, 0.0f, 3.5f), rigid);
    f.frames(30);
    float iceStart = f.pose(onIce).position.x, stoneStart = f.pose(onStone).position.x;
    const float v = 5.0f, g = 9.81f;
    runtime::setVelocity(f.app.world, onIce, vec3(v, 0, 0));
    runtime::setVelocity(f.app.world, onStone, vec3(v, 0, 0));
    f.frames(240);
    float ice = f.pose(onIce).position.x - iceStart, stone = f.pose(onStone).position.x - stoneStart;
    CAPTURE(ice);
    CAPTURE(stone);
    CHECK(ice == doctest::Approx(v * v / (2.0f * std::sqrt(0.05f) * g)).epsilon(0.1));
    CHECK(stone == doctest::Approx(v * v / (2.0f * std::sqrt(0.5f) * g)).epsilon(0.1));
    CHECK(f.pose(onIce).position.y == doctest::Approx(0.0f).epsilon(0.02));   // slid, did not tumble
}

TEST_CASE("a block bounces as high as the floor's material lets it: e^2 h; a primitive takes its own material") {
    Fixture f;
    f.floor("rubber", "clay");
    Entity bouncy = f.block(vec3(-4, 2, -4.5f));
    Entity dead = f.block(vec3(-4, 2, 3.5f));
    projv::RigidBody ball;
    ball.shape = projv::RigidBody::Shape::Sphere;
    ball.material = "rubber";   // on clay: the ball's rubber is the bouncy side
    Entity rubberBall = f.block(vec3(4, 2, 3.5f), ball);
    // How high each rises after it first lands: the lowest it gets, then the highest after that.
    const std::vector<Entity> watched{bouncy, dead, rubberBall};
    std::vector<float> lowest(3, 1e9f), rise(3, 0.0f);
    std::vector<bool> landed(3, false);
    for (int i = 0; i < 150; i++) {
        f.frames(1);
        for (size_t k = 0; k < 3; k++) {
            float y = f.pose(watched[k]).position.y;
            if (!landed[k]) { lowest[k] = std::min(lowest[k], y); landed[k] = y > lowest[k] + 0.005f; }
            else rise[k] = std::max(rise[k], y - lowest[k]);
        }
    }
    CAPTURE(rise[0]);
    CAPTURE(rise[1]);
    CAPTURE(rise[2]);
    // From 2 m with e = 0.8, e^2 h = 1.28 m; a cube landing a little off flat loses some of it.
    CHECK(rise[0] > 0.9f);
    CHECK(rise[0] < 1.4f);
    CHECK(rise[1] < 0.05f);
    CHECK(rise[2] > 0.9f);
    CHECK(f.body(rubberBall).materialKey != 0);
}

TEST_CASE("a session with materials replays the same, and the material survives entities.json") {
    {
        Fixture f(true);
        f.floor("ice", "rubber");
        Entity a = f.block(vec3(-8, 0.5f, -4.5f));
        f.block(vec3(-4, 2, 3.5f));
        f.frames(10);
        runtime::setVelocity(f.app.world, a, vec3(4, 0, 0));
        f.frames(60);
        runtime::ReplayResult result = runtime::PhysicsWorld::replay(runtime::physicsWorld(f.app.world).recording());
        CAPTURE(result.problem);
        CHECK(result.matched);
    }
    projv::RigidBody rigid;
    rigid.material = "stone";
    nlohmann::json j = runtime::ComponentTraits<projv::RigidBody>::save(rigid);
    CHECK(j["material"] == "stone");
    CHECK(runtime::ComponentTraits<projv::RigidBody>::load(j, 1)->material == "stone");
    projv::StaticCollider fixed;
    fixed.material = "ice";
    CHECK(runtime::ComponentTraits<projv::StaticCollider>::load(runtime::ComponentTraits<projv::StaticCollider>::save(fixed), 1)
              ->material == "ice");
}

TEST_CASE("a palette entry's physics material is saved in compose.json, and only when it names one") {
    auto folder = std::filesystem::temp_directory_path() / "projv_unit_palette_physics";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    {
        std::ofstream out(folder / "compose.json");
        out << R"({ "version": 1, "name": "Palette", "components": [
            { "type": "asset", "source": "Wall", "palette": [
                { "name": "granite", "color": [500, 500, 500], "physics": "stone" },
                { "name": "mossy",   "color": [300, 600, 300], "physics": "stone" },
                { "name": "paint",   "color": [900, 100, 100] } ] } ] })";
    }
    projv::ComposeDoc doc = utils::parseComposeJson((folder / "compose.json").string());
    REQUIRE(doc.components.size() == 1);
    REQUIRE(doc.components[0].palette.size() == 3);
    CHECK(doc.components[0].palette[0].physics == "stone");
    CHECK(doc.components[0].palette[1].physics == "stone");
    CHECK(doc.components[0].palette[2].physics.empty());

    REQUIRE(utils::writeComposeJson((folder / "again.json").string(), doc));
    std::ifstream in(folder / "again.json");
    nlohmann::json written = nlohmann::json::parse(in);
    const auto& palette = written["components"][0]["palette"];
    CHECK(palette[0]["physics"] == "stone");
    CHECK_FALSE(palette[2].contains("physics"));
    projv::ComposeDoc again = utils::parseComposeJson((folder / "again.json").string());
    CHECK(again.components[0].palette[1].physics == "stone");
    std::filesystem::remove_all(folder);
}
