// The Scene bridge (runtime/scene_bridge.h): entities linked to voxel components.
//
// CPU only. A real Application runs real frames -- the bridge is a PostUpdate system -- with Asset
// components standing in for geometry wherever geometry does not matter, and a Chunk where it does
// (its header position is what a transform write must reach).

#include "doctest/doctest.h"

#include <optional>
#include <string>
#include <vector>

#include "core/application.h"
#include "runtime/scene_bridge.h"
#include "utils/attachments.h"
#include "utils/scene_query.h"

namespace {
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::vec3;
    namespace utils = projv::utils;
    namespace runtime = projv::runtime;

    struct SpawnKind { std::string kind; };
    struct Gravity { float y = 0.0f; };
}

template<> struct projv::utils::AttachmentTraits<SpawnKind> {
    static constexpr const char* key = "test.spawn";
    static constexpr uint32_t version = 1;
    static constexpr projv::OnDuplicate onDuplicate = projv::OnDuplicate::Copy;
    static nlohmann::json save(const SpawnKind& s) { return nlohmann::json{{"kind", s.kind}}; }
    static std::optional<SpawnKind> load(const nlohmann::json& j, uint32_t) { return SpawnKind{j.at("kind").get<std::string>()}; }
};
template<> struct projv::utils::AttachmentTraits<Gravity> {
    static constexpr const char* key = "test.gravity";
    static constexpr uint32_t version = 1;
    static constexpr projv::OnDuplicate onDuplicate = projv::OnDuplicate::Copy;
    static nlohmann::json save(const Gravity& g) { return nlohmann::json{{"y", g.y}}; }
    static std::optional<Gravity> load(const nlohmann::json& j, uint32_t) { return Gravity{j.at("y").get<float>()}; }
};

namespace {
    // An Application with a Scene and the bridge, and Startup already run.
    struct Fixture {
        projv::Application app;
        projv::Scene& scene;
        std::vector<projv::ComponentDestroyed> destroyed;
        Fixture() : scene(app.world.ctx().emplace<projv::Scene>()) {
            runtime::installSceneBridge(app);
            app.events().on<projv::ComponentDestroyed>([this](const projv::ComponentDestroyed& e) { destroyed.push_back(e); });
            app.runStartup();
        }
        ComponentHandle asset(const char* name, ComponentHandle parent = INVALID_COMPONENT_HANDLE) {
            return utils::addComponent(scene, projv::ComponentKind::Asset, name, parent, 4, 1.0f);
        }
        ComponentHandle chunk(const char* name, ComponentHandle parent = INVALID_COMPONENT_HANDLE) {
            return utils::addComponent(scene, projv::ComponentKind::Chunk, name, parent, 4, 1.0f);
        }
        void frame() { app.runFrame(); }
    };

    void moveTo(projv::World& world, Entity e, vec3 position) {
        world.patch<projv::Transform>(e, [position](projv::Transform& t) { t.position = position; });
    }
}

TEST_CASE("linking seeds the entity's Transform from the component, and moves nothing") {
    Fixture f;
    ComponentHandle house = f.asset("House");
    utils::setComponentTransform(f.scene, house, vec3(3, 4, 5), projv::core::quat(1, 0, 0, 0), 2.0f);

    Entity e = f.app.world.create();
    REQUIRE(runtime::linkComponent(f.app.world, e, house));
    const auto& t = f.app.world.get<projv::Transform>(e);
    CHECK(t.position == vec3(3, 4, 5));
    CHECK(t.scale == 2.0f);
    CHECK_FALSE(f.app.world.all_of<projv::TransformDirty>(e));   // nothing to write back
    CHECK(runtime::entityFor(f.app.world, house) == e);
}

TEST_CASE("links are refused for a non-root in Root mode, a dead component, and a second link") {
    Fixture f;
    ComponentHandle house = f.asset("House");
    ComponentHandle door = f.asset("Door", house);
    ComponentHandle shed = f.asset("Shed");
    utils::deleteComponent(f.scene, shed);

    projv::World& w = f.app.world;
    CHECK_FALSE(runtime::linkComponent(w, w.create(), door));                         // Root, has a parent
    CHECK(runtime::linkComponent(w, w.create(), door, projv::LinkMode::Part));        // fine as a Part
    CHECK_FALSE(runtime::linkComponent(w, w.create(), shed));                         // deleted
    CHECK_FALSE(runtime::linkComponent(w, w.create(), 777));                          // never existed
    Entity first = w.create();
    CHECK(runtime::linkComponent(w, first, house));
    CHECK_FALSE(runtime::linkComponent(w, w.create(), house));                        // already linked
    CHECK(runtime::linkComponent(w, first, house));                                   // relinking itself is fine
}

TEST_CASE("moving a Root-linked entity moves the component and everything under it") {
    Fixture f;
    ComponentHandle house = f.asset("House");
    ComponentHandle wall = f.chunk("Wall", house);
    utils::setComponentTransform(f.scene, wall, vec3(1, 0, 0), projv::core::quat(1, 0, 0, 0), 1.0f);

    Entity e = f.app.world.create();
    REQUIRE(runtime::linkComponent(f.app.world, e, house));
    moveTo(f.app.world, e, vec3(10, 0, 0));
    f.frame();

    CHECK(f.scene.components[house].localPosition == vec3(10, 0, 0));
    CHECK(utils::getComponentWorldPosition(f.scene, wall) == vec3(11, 0, 0));
    // The chunk header is what the renderer reads: the subtree was rebaked.
    CHECK(f.scene.chunks[f.scene.components[wall].chunkHandle].header.position.x == doctest::Approx(11.0f));
    CHECK(f.app.world.get<projv::WorldTransform>(e).matrix[3][0] == doctest::Approx(10.0f));
}

TEST_CASE("a Part link writes in the part's Scene-parent space") {
    Fixture f;
    ComponentHandle house = f.asset("House");
    utils::setComponentTransform(f.scene, house, vec3(100, 0, 0), projv::core::quat(1, 0, 0, 0), 1.0f);
    ComponentHandle door = f.chunk("Door", house);

    Entity e = f.app.world.create();
    REQUIRE(runtime::linkComponent(f.app.world, e, door, projv::LinkMode::Part));
    moveTo(f.app.world, e, vec3(0, 2, 0));
    f.frame();
    CHECK(f.scene.components[door].localPosition == vec3(0, 2, 0));
    CHECK(utils::getComponentWorldPosition(f.scene, door) == vec3(100, 2, 0));
}

TEST_CASE("an untouched entity is not written back") {
    Fixture f;
    ComponentHandle house = f.asset("House");
    Entity e = f.app.world.create();
    REQUIRE(runtime::linkComponent(f.app.world, e, house));
    // Change the Scene behind the bridge's back. Had the sync written every linked entity, it would
    // put the entity's (stale) transform over this.
    utils::setComponentTransform(f.scene, house, vec3(7, 7, 7), projv::core::quat(1, 0, 0, 0), 1.0f);
    f.frame();
    CHECK(f.scene.components[house].localPosition == vec3(7, 7, 7));
}

TEST_CASE("reseedTransforms reads the Scene back into its entities") {
    Fixture f;
    ComponentHandle house = f.asset("House");
    Entity e = f.app.world.create();
    REQUIRE(runtime::linkComponent(f.app.world, e, house));
    std::vector<projv::ComponentTransformChanged> changed;
    f.app.events().on<projv::ComponentTransformChanged>([&changed](const auto& c) { changed.push_back(c); });

    utils::setComponentTransform(f.scene, house, vec3(7, 7, 7), projv::core::quat(1, 0, 0, 0), 1.0f);
    runtime::reseedTransforms(f.app.world);
    CHECK(f.app.world.get<projv::Transform>(e).position == vec3(7, 7, 7));
    runtime::reseedTransforms(f.app.world);   // unchanged now: no second event
    f.frame();
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].entity == e);
}

TEST_CASE("unlinking keeps the component by default, and deletes it with OnUnlink::Destroy") {
    Fixture f;
    projv::World& w = f.app.world;
    ComponentHandle kept = f.asset("Kept");
    ComponentHandle doomed = f.asset("Doomed");
    ComponentHandle child = f.asset("Child", doomed);

    Entity a = w.create();
    Entity b = w.create();
    REQUIRE(runtime::linkComponent(w, a, kept));
    REQUIRE(runtime::linkComponent(w, b, doomed, projv::LinkMode::Root, projv::OnUnlink::Destroy));

    runtime::unlinkComponent(w, a);
    CHECK(utils::isComponentAlive(f.scene, kept));
    w.destroy(b);                                    // destroying the entity unlinks it too
    CHECK_FALSE(utils::isComponentAlive(f.scene, doomed));
    CHECK_FALSE(utils::isComponentAlive(f.scene, child));
}

TEST_CASE("a linked component deleted elsewhere unlinks its entity and says so") {
    Fixture f;
    projv::World& w = f.app.world;
    ComponentHandle house = f.asset("House");
    Entity e = w.create();
    REQUIRE(runtime::linkComponent(w, e, house));

    utils::deleteComponent(f.scene, house);          // e.g. the editor, or gameplay without the bridge
    f.frame();                                       // sync notices; the pump delivers
    CHECK_FALSE(w.all_of<projv::VoxelComponent>(e));
    CHECK(w.valid(e));                               // the entity is the game's to destroy, or not
    REQUIRE(f.destroyed.size() == 1);
    CHECK(f.destroyed[0].entity == e);
    CHECK(f.destroyed[0].component == house);
}

TEST_CASE("spawn handlers run for their own attachment type, and unknown keys are left alone") {
    Fixture f;
    projv::World& w = f.app.world;
    struct Spawner { std::string kind; };
    int gravityCalls = 0;
    runtime::registerSpawnHandler<SpawnKind>(w, [](projv::World& world, Entity e, const SpawnKind& s) {
        world.emplace<Spawner>(e, s.kind);
    });
    runtime::registerSpawnHandler<Gravity>(w, [&gravityCalls](projv::World&, Entity, const Gravity&) { gravityCalls++; });

    ComponentHandle door = f.asset("Door");
    ComponentHandle rock = f.asset("Rock");
    utils::setAttachment(f.scene, door, SpawnKind{"door"});
    utils::attachRaw(f.scene, rock, projv::AttachmentScope::Component, {{"someone.else", R"({"v":1})"}});

    Entity doorEntity = runtime::spawnComponent(w, door);
    Entity rockEntity = runtime::spawnComponent(w, rock);
    REQUIRE(doorEntity != projv::NullEntity);
    REQUIRE(rockEntity != projv::NullEntity);
    REQUIRE(w.all_of<Spawner>(doorEntity));
    CHECK(w.get<Spawner>(doorEntity).kind == "door");
    CHECK_FALSE(w.all_of<Spawner>(rockEntity));
    CHECK(gravityCalls == 0);
    CHECK(utils::hasAttachment(f.scene, rock, "someone.else"));   // still there, still saved
}

TEST_CASE("spawnFromCompose spawns every live root and runs document handlers on the document") {
    Fixture f;
    projv::World& w = f.app.world;
    float gravitySeen = 0.0f;
    Entity gravityEntity = projv::NullEntity;
    runtime::registerDocumentSpawnHandler<Gravity>(w, [&](projv::World&, Entity e, const Gravity& g) {
        gravitySeen = g.y;
        gravityEntity = e;
    });

    ComponentHandle a = f.asset("A");
    ComponentHandle b = f.asset("B");
    f.asset("Inside", a);                     // not a root
    ComponentHandle gone = f.asset("Gone");
    utils::deleteComponent(f.scene, gone);
    utils::setAttachment(f.scene, INVALID_COMPONENT_HANDLE, Gravity{-9.8f}, projv::AttachmentScope::Document);

    runtime::SpawnedDocument spawned = runtime::spawnFromCompose(w);
    REQUIRE(spawned.document != projv::NullEntity);
    CHECK(w.all_of<projv::SceneDocument>(spawned.document));
    CHECK(gravitySeen == doctest::Approx(-9.8f));
    CHECK(gravityEntity == spawned.document);
    REQUIRE(spawned.roots.size() == 2);
    CHECK(w.get<projv::VoxelComponent>(spawned.roots[0]).handle == a);
    CHECK(w.get<projv::VoxelComponent>(spawned.roots[1]).handle == b);
}

TEST_CASE("an Asset root's own folder block reaches document handlers when it is spawned") {
    Fixture f;
    projv::World& w = f.app.world;
    std::vector<Entity> seenOn;
    runtime::registerDocumentSpawnHandler<Gravity>(w, [&seenOn](projv::World&, Entity e, const Gravity&) { seenOn.push_back(e); });
    ComponentHandle level = f.asset("Level");
    utils::setAttachment(f.scene, level, Gravity{-1.0f}, projv::AttachmentScope::Document);
    Entity e = runtime::spawnComponent(w, level);
    CHECK(seenOn == std::vector<Entity>{e});
}

TEST_CASE("EntityLinked is sent for every link") {
    Fixture f;
    std::vector<projv::EntityLinked> linked;
    f.app.events().on<projv::EntityLinked>([&linked](const projv::EntityLinked& l) { linked.push_back(l); });
    ComponentHandle house = f.asset("House");
    Entity e = runtime::spawnComponent(f.app.world, house);
    f.frame();
    REQUIRE(linked.size() == 1);
    CHECK(linked[0].entity == e);
    CHECK(linked[0].component == house);
}

TEST_CASE("a component spawned with OnUnlink::Destroy goes when its entity does") {
    Fixture f;
    ComponentHandle prefab = f.asset("Prefab");
    ComponentHandle part = f.asset("Part", prefab);
    Entity e = runtime::spawnComponent(f.app.world, prefab, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    REQUIRE(e != projv::NullEntity);
    f.app.world.destroy(e);
    CHECK_FALSE(utils::isComponentAlive(f.scene, prefab));
    CHECK_FALSE(utils::isComponentAlive(f.scene, part));
}
