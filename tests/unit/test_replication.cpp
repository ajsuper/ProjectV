// A server and clients in one process (runtime/replication.h, networking.md N2): each its own
// Application, over net::LoopbackNetwork, all on a clock the test owns. No window, no sockets.

#include "doctest/doctest.h"

#include <filesystem>
#include <map>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/application.h"
#include "net/transport.h"
#include "runtime/entities.h"
#include "runtime/loading.h"
#include "runtime/net.h"
#include "runtime/physics.h"
#include "runtime/replication.h"
#include "runtime/scene_bridge.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/scene_query.h"

namespace {
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::INVALID_COMPONENT_HANDLE;
    using projv::core::ivec3;
    using projv::core::vec3;
    namespace utils = projv::utils;
    namespace runtime = projv::runtime;
    namespace net = projv::net;

    constexpr double DT = 1.0 / 60.0;

    struct TempDir {
        std::filesystem::path path;
        explicit TempDir(const std::string& name) {
            path = std::filesystem::temp_directory_path() / ("projv_unit_" + name);
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
        }
        ~TempDir() { std::error_code e; std::filesystem::remove_all(path, e); }
    };

    std::vector<projv::PendingVoxelOp> box(ivec3 low, ivec3 high) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) ops.push_back({true, ivec3(x, y, z), 0x3FFFFFFFu});
        return ops;
    }

    // An arena: a 32 m floor (its top at y = 0) with a static collider; a stone wall, 8 x 6 m, with
    // NetId 50, that breaks; and avatar/, a prefab.
    void writeArena(const std::filesystem::path& folder, int pyramid = 0) {
        {
            projv::Scene scene;
            ComponentHandle floor = utils::addComponent(scene, projv::ComponentKind::Chunk, "floor", INVALID_COMPONENT_HANDLE, 16, 0.5f);
            utils::queueVoxelAdd(scene, floor, box({0, 0, 0}, {63, 1, 63}));
            ComponentHandle wall = utils::addComponent(scene, projv::ComponentKind::Chunk, "wall", INVALID_COMPONENT_HANDLE, 16, 0.25f);
            utils::queueVoxelAdd(scene, wall, box({0, 0, 0}, {31, 23, 1}));
            utils::updateScene(scene);
            REQUIRE(utils::setMaterialPhysics(scene, wall, 0, "stone"));
            utils::setComponentTransform(scene, floor, vec3(-16, -1, -16), projv::core::quat(1, 0, 0, 0), 1.0f);
            utils::setComponentTransform(scene, wall, vec3(-4, 0, 10), projv::core::quat(1, 0, 0, 0), 1.0f);
            // A pyramid of 1 m crates, `pyramid` wide at the base, from x = -8 at z = -8: away from where
            // players appear.
            for (int row = 0, n = 0; row < pyramid; row++)
                for (int i = 0; i < pyramid - row; i++, n++) {
                    ComponentHandle crate = utils::addComponent(scene, projv::ComponentKind::Chunk, ("crate" + std::to_string(n)).c_str(),
                                                                INVALID_COMPONENT_HANDLE, 16, 0.25f);
                    utils::queueVoxelAdd(scene, crate, box({0, 0, 0}, {3, 3, 3}));
                    utils::updateScene(scene);
                    utils::setComponentTransform(scene, crate, vec3(-8.0f + float(i) * 1.02f + float(row) * 0.51f, float(row) * 1.0f, -8.5f),
                                                 projv::core::quat(1, 0, 0, 0), 1.0f);
                }
            REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder.string()));
            projv::Scene saved = utils::loadComposeFromDisk(folder.string());
            std::ofstream out(folder / "entities.json");
            out << R"({ "version": 1, "entities": [ { "name": "floor", "link": )" << saved.components[0].localId
                << R"(, "components": { "physics.static": { "v": 1 } } }, { "name": "wall", "link": )" << saved.components[1].localId
                << R"(, "components": { "physics.static": { "v": 1 }, "projv.netid": { "v": 1, "id": 50 } } })";
            for (size_t i = 2; i < saved.components.size(); i++)
                out << R"(, { "name": ")" << saved.components[i].name << R"(", "link": )" << saved.components[i].localId
                    << R"(, "components": { "physics.rigidbody": { "v": 1 }, "projv.netid": { "v": 1, "id": )" << (100 + i) << " } } }";
            out << "] }";
        }
        projv::Scene avatar;
        ComponentHandle body = utils::addComponent(avatar, projv::ComponentKind::Chunk, "body", INVALID_COMPONENT_HANDLE, 16, 0.25f);
        utils::queueVoxelAdd(avatar, body, box({0, 0, 0}, {1, 6, 1}));
        utils::updateScene(avatar);
        REQUIRE(utils::saveComposeToDisk(avatar, INVALID_COMPONENT_HANDLE, (folder / "avatar").string()));
    }

    struct Machine {
        projv::Application app;
        double now = 0.0;
        std::vector<runtime::JoinedServer> joined;
        std::vector<runtime::LeftServer> left;
        std::vector<runtime::ClientJoined> clientsJoined;
        std::vector<runtime::ClientLeft> clientsLeft;
        Machine() {
            app.clock = [this] { return now; };
            app.events().on<runtime::JoinedServer>([this](const auto& e) { joined.push_back(e); });
            app.events().on<runtime::LeftServer>([this](const auto& e) { left.push_back(e); });
            app.events().on<runtime::ClientJoined>([this](const auto& e) { clientsJoined.push_back(e); });
            app.events().on<runtime::ClientLeft>([this](const auto& e) { clientsLeft.push_back(e); });
        }
        void loadScene(const std::filesystem::path& folder) {
            app.world.ctx().emplace<projv::Scene>(utils::loadComposeFromDisk(folder.string()));
            runtime::installSceneBridge(app);
        }
        void frame() {
            now += DT;
            app.runFrame();
        }
    };

    // The game's physics: the same material library on every machine.
    void installWorld(projv::Application& app, uint32_t maxPieces = 3000) {
        runtime::installNetIds(app.world);
        runtime::PhysicsConfig config;
        config.maxPieces = maxPieces;
        runtime::installPhysics(app, config);
        runtime::PhysicsMaterial stone{2400.0f, 0.7f, 0.0f};
        stone.strength = 1e5f;
        stone.shardSize = 0.5f;
        runtime::definePhysicsMaterial(app.world, "stone", stone);
        runtime::spawnEntities(app.world);
    }

    std::unique_ptr<Machine> server(net::LoopbackNetwork& network, const std::filesystem::path& folder, uint32_t maxPieces = 3000,
                                    uint32_t bytesPerSecond = 64000) {
        auto m = std::make_unique<Machine>();
        m->loadScene(folder);
        installWorld(m->app, maxPieces);
        runtime::ServerConfig config;
        config.bytesPerSecond = bytesPerSecond;
        config.sceneFolder = folder.string();
        const std::string avatar = (folder / "avatar").string();
        config.spawnPlayer = [avatar](projv::World& world, runtime::ClientId id) {
            Entity e = runtime::instantiatePrefab(world, avatar, vec3(float(id) * 3.0f - 6.0f, 0.0f, 0.0f));
            world.emplace<projv::CharacterController>(e);
            world.emplace<projv::Replicated>(e, projv::Replicated{"avatar", {"physics.character"}});
            return e;
        };
        REQUIRE(runtime::installServer(m->app, network.endpoint(), config));
        m->app.runStartup();
        m->app.runFrame();
        return m;
    }

    std::unique_ptr<Machine> client(net::LoopbackNetwork& network, const std::filesystem::path& folder, const std::string& name) {
        auto m = std::make_unique<Machine>();
        m->loadScene(folder);
        installWorld(m->app);
        runtime::ClientConfig config;
        config.port = 27015;
        config.sceneFolder = folder.string();
        config.name = name;
        REQUIRE(runtime::installClient(m->app, network.endpoint(), config));
        m->app.runStartup();
        m->app.runFrame();
        return m;
    }

    // Every machine a frame, the network a frame.
    void run(net::LoopbackNetwork& network, std::vector<Machine*> machines, double seconds) {
        for (double t = 0; t < seconds; t += DT) {
            network.advance(DT);
            for (Machine* m : machines)
                if (m) m->frame();
        }
    }

    // The players a client shows: everyone else's proxies, and its own (predicted, not a proxy).
    size_t proxies(Machine& m) {
        return m.app.world.view<projv::NetProxy>().size() + (runtime::sessionStats(m.app.world).predicting ? 1 : 0);
    }
    Entity proxyFor(Machine& m, projv::NetId id) { return runtime::entityForNetId(m.app.world, id); }
}

TEST_CASE("replication: two clients join, each sees both players, and walking moves them on both") {
    TempDir dir("replication_join");
    writeArena(dir.path);
    net::LoopbackNetwork network;
    network.setConditions({0.03});
    auto s = server(network, dir.path);
    auto a = client(network, dir.path, "ann");
    auto b = client(network, dir.path, "bo");
    run(network, {s.get(), a.get(), b.get()}, 1.0);

    REQUIRE(a->joined.size() == 1);
    REQUIRE(b->joined.size() == 1);
    CHECK(a->joined[0].you != b->joined[0].you);
    CHECK(s->clientsJoined.size() == 2);
    CHECK(runtime::sessionStats(s->app.world).clients == 2);
    CHECK(proxies(*a) == 2);
    CHECK(proxies(*b) == 2);
    const projv::NetId annId = a->joined[0].player;
    Entity annOnServer = runtime::entityForNetId(s->app.world, annId);
    REQUIRE(annOnServer != projv::NullEntity);
    CHECK(s->app.world.all_of<projv::PhysicsCharacter>(annOnServer));
    // The client's copy is the prefab, without the character: the server walks it.
    Entity annOnB = proxyFor(*b, annId);
    REQUIRE(annOnB != projv::NullEntity);
    CHECK(b->app.world.all_of<projv::VoxelComponent>(annOnB));
    CHECK_FALSE(b->app.world.all_of<projv::CharacterController>(annOnB));

    // Ann walks along +z (Bo stands 3 m along x) for two seconds.
    const float startZ = runtime::characterState(s->app.world, annOnServer).position.z;
    runtime::setLocalInput(a->app.world, vec3(0, 0, 3.0f));
    run(network, {s.get(), a.get(), b.get()}, 2.0);
    runtime::setLocalInput(a->app.world, vec3(0.0f));
    const float walked = runtime::characterState(s->app.world, annOnServer).position.z - startZ;
    CHECK(walked > 5.0f);
    CHECK(walked < 6.5f);
    // Both clients show her about where the server has her: a tenth of a second and a trip behind.
    for (Machine* m : {a.get(), b.get()}) {
        const vec3 shown = m->app.world.get<projv::Transform>(proxyFor(*m, annId)).position;
        const vec3 truth = runtime::characterState(s->app.world, annOnServer).position;
        CHECK(glm::distance(shown, truth) < 0.6f);
    }
    // And once she stops, exactly there.
    run(network, {s.get(), a.get(), b.get()}, 0.5);
    const vec3 truth = runtime::characterState(s->app.world, annOnServer).position;
    CHECK(glm::distance(b->app.world.get<projv::Transform>(proxyFor(*b, annId)).position, truth) < 0.01f);
    // Her voxels went with her entity.
    ComponentHandle shown = b->app.world.get<projv::VoxelComponent>(proxyFor(*b, annId)).handle;
    CHECK(glm::distance(utils::getComponentWorldPosition(b->app.world.ctx().get<projv::Scene>(), shown), truth) < 0.01f);
}

TEST_CASE("replication: a client that leaves is despawned everywhere; one joining late gets everyone") {
    TempDir dir("replication_leave");
    writeArena(dir.path);
    net::LoopbackNetwork network;
    network.setConditions({0.02});
    auto s = server(network, dir.path);
    auto a = client(network, dir.path, "ann");
    auto b = client(network, dir.path, "bo");
    run(network, {s.get(), a.get(), b.get()}, 1.0);
    REQUIRE(proxies(*a) == 2);

    const projv::NetId boId = b->joined[0].player;
    b.reset();   // gone without a word
    run(network, {s.get(), a.get()}, 0.5);
    CHECK(s->clientsLeft.size() == 1);
    CHECK(runtime::entityForNetId(s->app.world, boId) == projv::NullEntity);
    CHECK(proxies(*a) == 1);
    CHECK(proxyFor(*a, boId) == projv::NullEntity);

    auto c = client(network, dir.path, "cy");
    run(network, {s.get(), a.get(), c.get()}, 1.0);
    CHECK(proxies(*c) == 2);
    CHECK(proxies(*a) == 2);
    CHECK(proxyFor(*c, a->joined[0].player) != projv::NullEntity);
}

TEST_CASE("replication: a client with other scene files is refused, and told why") {
    TempDir dir("replication_refuse");
    TempDir other("replication_refuse_other");
    writeArena(dir.path);
    writeArena(other.path);
    std::ofstream(other.path / "extra.txt") << "not the server's";
    net::LoopbackNetwork network;
    network.setConditions({0.02});
    auto s = server(network, dir.path);
    auto a = client(network, other.path, "ann");
    run(network, {s.get(), a.get()}, 1.0);
    CHECK(a->joined.empty());
    REQUIRE(a->left.size() == 1);
    CHECK(a->left[0].reason.find("scene") != std::string::npos);
    CHECK(s->clientsJoined.empty());
    CHECK(runtime::sessionStats(s->app.world).messagesRefused == 1);
}

TEST_CASE("replication: at 150 ms and 5% loss a walk still arrives, and the client's copy follows it") {
    TempDir dir("replication_lossy");
    writeArena(dir.path);
    net::LoopbackNetwork network(9);
    network.setConditions({0.075, 0.01, 0.05, 0.0, 0.0});
    auto s = server(network, dir.path);
    auto a = client(network, dir.path, "ann");
    auto b = client(network, dir.path, "bo");
    run(network, {s.get(), a.get(), b.get()}, 1.5);
    REQUIRE(a->joined.size() == 1);
    REQUIRE(proxies(*b) == 2);
    const projv::NetId annId = a->joined[0].player;
    Entity annOnServer = runtime::entityForNetId(s->app.world, annId);
    runtime::setLocalInput(a->app.world, vec3(0, 0, 2.0f));
    run(network, {s.get(), a.get(), b.get()}, 3.0);
    runtime::setLocalInput(a->app.world, vec3(0.0f));
    run(network, {s.get(), a.get(), b.get()}, 1.0);
    const vec3 truth = runtime::characterState(s->app.world, annOnServer).position;
    CHECK(truth.z > 5.0f);
    CHECK(glm::distance(b->app.world.get<projv::Transform>(proxyFor(*b, annId)).position, truth) < 0.01f);
    CHECK(runtime::sessionStats(b->app.world).snapshotsReceived > 50);
}

// ---- World changes (N3) -------------------------------------------------------------------------

namespace {
    uint64_t voxelHash(Machine& m, projv::NetId id) {
        Entity e = runtime::entityForNetId(m.app.world, id);
        if (e == projv::NullEntity) return 0;
        return utils::voxelContentHash(m.app.world.ctx().get<projv::Scene>(), runtime::voxelComponentOf(m.app.world, e));
    }
    // The pieces a machine has: NetId -> voxels.
    std::map<uint32_t, uint64_t> piecesOf(Machine& m) {
        std::map<uint32_t, uint64_t> out;
        const auto& scene = m.app.world.ctx().get<projv::Scene>();
        for (auto [e, piece, id] : m.app.world.view<projv::Piece, projv::NetId>().each())
            out[id.value] = utils::voxelContentHash(scene, runtime::voxelComponentOf(m.app.world, e));
        return out;
    }
    constexpr float PI = 3.14159265f;
    float energyFor(float radius) { return 1e5f * 4.0f / 3.0f * PI * radius * radius * radius; }
}

TEST_CASE("replication: a wall broken on the server breaks the same on every client, through 10% loss") {
    TempDir dir("replication_break");
    writeArena(dir.path);
    net::LoopbackNetwork network(4);
    network.setConditions({0.05, 0.01, 0.1});
    auto s = server(network, dir.path);
    auto a = client(network, dir.path, "ann");
    auto b = client(network, dir.path, "bo");
    run(network, {s.get(), a.get(), b.get()}, 1.0);
    const projv::NetId wall{50};
    REQUIRE(voxelHash(*s, wall) != 0);
    REQUIRE(voxelHash(*a, wall) == voxelHash(*s, wall));

    runtime::fracture(s->app.world, runtime::entityForNetId(s->app.world, wall), vec3(0, 3, 10), energyFor(1.2f), vec3(0, 0, 4));
    run(network, {s.get(), a.get(), b.get()}, 1.5);
    const auto serverPieces = piecesOf(*s);
    REQUIRE(serverPieces.size() > 3);
    for (Machine* m : {a.get(), b.get()}) {
        CHECK(voxelHash(*m, wall) == voxelHash(*s, wall));
        CHECK(piecesOf(*m) == serverPieces);   // the same pieces, by NetId, of the same voxels
        CHECK(runtime::sessionStats(m->app.world).worldChanges == 1);
        CHECK(runtime::sessionStats(m->app.world).worldMismatches == 0);
    }

    // An edit to what is left goes the same way.
    std::vector<projv::PendingVoxelOp> notch;
    for (int x = 28; x <= 31; x++) notch.push_back({false, ivec3(x, 23, 0), 0});
    runtime::editVoxels(s->app.world, runtime::entityForNetId(s->app.world, wall), notch);
    run(network, {s.get(), a.get(), b.get()}, 1.0);
    for (Machine* m : {a.get(), b.get()}) {
        CHECK(voxelHash(*m, wall) == voxelHash(*s, wall));
        CHECK(runtime::sessionStats(m->app.world).worldMismatches == 0);
    }
}

TEST_CASE("replication: a client joining after the break gets the rubble, less what was cleared") {
    TempDir dir("replication_late");
    writeArena(dir.path);
    net::LoopbackNetwork network(5);
    network.setConditions({0.04});
    auto s = server(network, dir.path);
    auto a = client(network, dir.path, "ann");
    run(network, {s.get(), a.get()}, 1.0);
    runtime::fracture(s->app.world, runtime::entityForNetId(s->app.world, projv::NetId{50}), vec3(0, 3, 10), energyFor(1.0f), vec3(0, 0, 3));
    run(network, {s.get(), a.get()}, 1.0);
    // The server clears a piece (as its debris budget would).
    auto pieces = piecesOf(*s);
    REQUIRE(pieces.size() > 2);
    s->app.world.destroy(runtime::entityForNetId(s->app.world, projv::NetId{pieces.begin()->first}));
    run(network, {s.get(), a.get()}, 0.5);
    CHECK(piecesOf(*a) == piecesOf(*s));

    auto late = client(network, dir.path, "cy");
    run(network, {s.get(), a.get(), late.get()}, 1.5);
    CHECK(voxelHash(*late, projv::NetId{50}) == voxelHash(*s, projv::NetId{50}));
    CHECK(piecesOf(*late) == piecesOf(*s));
    CHECK(runtime::sessionStats(late->app.world).worldMismatches == 0);
    // And the pieces are where the server has them, posed by snapshots like anything else -- not yet:
    // a client's own bodies are corrected by snapshots in N4. Here they only exist, made alike.
}

TEST_CASE("replication: debris the server's budget clears goes on every client") {
    TempDir dir("replication_debris");
    writeArena(dir.path);
    net::LoopbackNetwork network(6);
    network.setConditions({0.03});
    auto s = server(network, dir.path, 4);
    auto a = client(network, dir.path, "ann");
    run(network, {s.get(), a.get()}, 1.0);
    runtime::fracture(s->app.world, runtime::entityForNetId(s->app.world, projv::NetId{50}), vec3(0, 3, 10), energyFor(1.2f), vec3(0, 0, 4));
    run(network, {s.get(), a.get()}, 1.5);
    CHECK(piecesOf(*s).size() <= 4);
    CHECK_FALSE(piecesOf(*s).empty());
    CHECK(piecesOf(*a) == piecesOf(*s));
}

TEST_CASE("replication: a client whose wall is not the server's is told so by the check") {
    TempDir dir("replication_mismatch");
    writeArena(dir.path);
    net::LoopbackNetwork network(7);
    network.setConditions({0.03});
    auto s = server(network, dir.path);
    auto a = client(network, dir.path, "ann");
    run(network, {s.get(), a.get()}, 1.0);
    // Ann's copy loses a corner the server never lost: a desync.
    Entity mine = runtime::entityForNetId(a->app.world, projv::NetId{50});
    utils::queueVoxelRemove(a->app.world.ctx().get<projv::Scene>(), runtime::voxelComponentOf(a->app.world, mine),
                            {{false, ivec3(0, 0, 0), 0}});
    utils::updateScene(a->app.world.ctx().get<projv::Scene>());
    runtime::fracture(s->app.world, runtime::entityForNetId(s->app.world, projv::NetId{50}), vec3(0, 3, 10), energyFor(1.0f), vec3(0));
    run(network, {s.get(), a.get()}, 1.0);
    CHECK(runtime::sessionStats(a->app.world).worldMismatches == 1);
}

// ---- Bodies (N4) -------------------------------------------------------------------------------------

namespace {
    // Every crate's pose on a machine, by NetId.
    std::map<uint32_t, projv::runtime::BodyState> crates(Machine& m) {
        std::map<uint32_t, projv::runtime::BodyState> out;
        for (auto [e, body, id] : m.app.world.view<projv::PhysicsBody, projv::NetId>().each())
            if (body.motion == projv::runtime::MotionType::Dynamic && id.value >= 100) out[id.value] = runtime::bodyPose(m.app.world, e);
        return out;
    }
}

TEST_CASE("replication: a pyramid knocked over on the server settles where it did on every client, through 150 ms and 5% loss") {
    TempDir dir("replication_pyramid");
    writeArena(dir.path, 4);   // ten crates
    net::LoopbackNetwork network(8);
    network.setConditions({0.075, 0.01, 0.05});
    auto s = server(network, dir.path);
    auto a = client(network, dir.path, "ann");
    auto b = client(network, dir.path, "bo");
    run(network, {s.get(), a.get(), b.get()}, 2.0);
    REQUIRE(crates(*s).size() == 10);

    // Only the server knows: the bottom left crate is hit hard, away from the players.
    Entity hit = runtime::entityForNetId(s->app.world, projv::NetId{102});
    REQUIRE(hit != projv::NullEntity);
    runtime::setVelocity(s->app.world, hit, vec3(0, 2, -9), vec3(4, 0, 0));
    run(network, {s.get(), a.get(), b.get()}, 8.0);

    const auto truth = crates(*s);
    for (auto [id, state] : truth) CHECK_FALSE(state.awake);   // all settled
    for (Machine* m : {a.get(), b.get()}) {
        const auto mine = crates(*m);
        REQUIRE(mine.size() == truth.size());
        for (auto [id, state] : truth) {
            INFO("crate " << id);
            CHECK(glm::distance(mine.at(id).position, state.position) < 0.01f);
            CHECK(std::abs(glm::dot(mine.at(id).rotation, state.rotation)) > 0.9999f);
        }
        CHECK(runtime::sessionStats(m->app.world).corrections > 50);
    }
    // And the pyramid really fell: the hit crate is metres from where it stood.
    CHECK(glm::distance(truth.at(102).position, vec3(-8.0f + 0.5f, 0.5f, -8.0f)) > 1.0f);
}

TEST_CASE("replication: snapshots stay within the budget, and what is asleep stops costing") {
    TempDir dir("replication_budget");
    writeArena(dir.path, 6);   // twenty-one crates
    net::LoopbackNetwork network(10);
    network.setConditions({0.03});
    const uint32_t budget = 6000;   // bytes a second: far less than everything every snapshot
    auto s = server(network, dir.path, 3000, budget);
    auto a = client(network, dir.path, "ann");
    run(network, {s.get(), a.get()}, 1.0);
    runtime::setVelocity(s->app.world, runtime::entityForNetId(s->app.world, projv::NetId{102}), vec3(0, 3, -10), vec3(0));
    const uint64_t before = runtime::sessionStats(s->app.world).snapshotBytes;
    run(network, {s.get(), a.get()}, 6.0);
    const uint64_t during = runtime::sessionStats(s->app.world).snapshotBytes - before;
    CHECK(double(during) / 6.0 <= budget * 1.05);
    // Settled, it all comes to the same, and then costs next to nothing.
    run(network, {s.get(), a.get()}, 6.0);
    const auto truth = crates(*s);
    const auto mine = crates(*a);
    for (auto [id, state] : truth) CHECK(glm::distance(mine.at(id).position, state.position) < 0.01f);
    int awake = 0;
    for (auto [id, state] : truth) awake += state.awake;
    INFO("awake on the server: " << awake);
    const uint64_t settled = runtime::sessionStats(s->app.world).snapshotBytes;
    CHECK(awake == 0);
    run(network, {s.get(), a.get()}, 2.0);
    // Two seconds of Ann's ack (her own player, which she predicts), about 20 bytes at 30 Hz, and
    // nothing for the crates.
    CHECK(runtime::sessionStats(s->app.world).snapshotBytes - settled < 1400);
}

// ---- Prediction (N5) ----------------------------------------------------------------------------------

TEST_CASE("prediction: your own player moves at once, and at 150 ms and 5% loss is never visibly put right") {
    TempDir dir("replication_predict");
    writeArena(dir.path);
    net::LoopbackNetwork network(12);
    network.setConditions({0.075, 0.01, 0.05});
    auto s = server(network, dir.path);
    auto a = client(network, dir.path, "ann");
    run(network, {s.get(), a.get()}, 1.5);
    REQUIRE(runtime::sessionStats(a->app.world).predicting);
    const projv::NetId annId = a->joined[0].player;
    Entity mine = runtime::entityForNetId(a->app.world, annId);
    Entity theirs = runtime::entityForNetId(s->app.world, annId);
    REQUIRE(mine != projv::NullEntity);

    // At once: two frames after the key, she has moved here -- not a round trip later.
    const vec3 before = a->app.world.get<projv::Transform>(mine).position;
    runtime::setLocalInput(a->app.world, vec3(0, 0, 3.0f));
    run(network, {s.get(), a.get()}, 2.0 * DT);
    CHECK(glm::distance(a->app.world.get<projv::Transform>(mine).position, before) > 0.03f);

    // A walk with turns and a jump.
    run(network, {s.get(), a.get()}, 1.5);
    runtime::setLocalInput(a->app.world, vec3(2.5f, 0, 0), true);
    run(network, {s.get(), a.get()}, 1.0);
    runtime::setLocalInput(a->app.world, vec3(-1.0f, 0, -2.0f));
    run(network, {s.get(), a.get()}, 1.5);
    runtime::setLocalInput(a->app.world, vec3(0.0f));
    run(network, {s.get(), a.get()}, 1.0);

    const runtime::SessionStats stats = runtime::sessionStats(a->app.world);
    CHECK(stats.largestCorrection < 0.01f);   // nothing anyone would see
    const vec3 server = runtime::characterState(s->app.world, theirs).position;
    const vec3 predicted = runtime::characterState(a->app.world, mine).position;
    CHECK(glm::distance(server, predicted) < 0.001f);
    // Where the inputs take her: 1.0 m along x, 4.6 - 3.0 m along z.
    CHECK(server.x - before.x == doctest::Approx(1.0f).epsilon(0.05));
    CHECK(server.z - before.z == doctest::Approx(1.6f).epsilon(0.05));
}

TEST_CASE("prediction: when the server moves your player, you are put right in a round trip") {
    TempDir dir("replication_correct");
    writeArena(dir.path);
    net::LoopbackNetwork network(13);
    network.setConditions({0.05});
    auto s = server(network, dir.path);
    auto a = client(network, dir.path, "ann");
    run(network, {s.get(), a.get()}, 1.5);
    const projv::NetId annId = a->joined[0].player;
    Entity theirs = runtime::entityForNetId(s->app.world, annId);
    Entity mine = runtime::entityForNetId(a->app.world, annId);
    // The server's game moves her 3 m (a respawn, a push): her client did not predict that.
    runtime::physicsWorld(s->app.world).setCharacterPosition(s->app.world.get<projv::PhysicsCharacter>(theirs).id,
                                                             runtime::characterState(s->app.world, theirs).position + vec3(3, 0, 0));
    run(network, {s.get(), a.get()}, 0.3);
    const runtime::SessionStats stats = runtime::sessionStats(a->app.world);
    CHECK(stats.reconciliations >= 1);
    CHECK(stats.largestCorrection > 2.9f);
    CHECK(glm::distance(runtime::characterState(a->app.world, mine).position,
                        runtime::characterState(s->app.world, theirs).position) < 0.001f);
}

// ---- Per-client loading (N6) -----------------------------------------------------------------------

namespace {
    // A floor 320 m square of 16 m cells (400 blocks), its top at y = 0, and a crate (NetId 300) by
    // where the second player appears, at x = +100. Players appear at x = -100 and +100.
    void writeField(const std::filesystem::path& folder) {
        projv::Scene scene;
        ComponentHandle floor = utils::addComponent(scene, projv::ComponentKind::Chunk, "floor", INVALID_COMPONENT_HANDLE, 16, 1.0f);
        utils::queueVoxelAdd(scene, floor, box({0, 0, 0}, {319, 0, 319}));
        ComponentHandle crate = utils::addComponent(scene, projv::ComponentKind::Chunk, "crate", INVALID_COMPONENT_HANDLE, 16, 0.25f);
        utils::queueVoxelAdd(scene, crate, box({0, 0, 0}, {3, 3, 3}));
        utils::updateScene(scene);
        utils::setComponentTransform(scene, floor, vec3(-160, -1, -160), projv::core::quat(1, 0, 0, 0), 1.0f);
        utils::setComponentTransform(scene, crate, vec3(95, 0, 4), projv::core::quat(1, 0, 0, 0), 1.0f);
        REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder.string()));
        projv::Scene saved = utils::loadComposeFromDisk(folder.string());
        std::ofstream(folder / "entities.json")
            << R"({ "version": 1, "entities": [ { "name": "floor", "link": )" << saved.components[0].localId
            << R"(, "components": { "physics.static": { "v": 1 } } }, { "name": "crate", "link": )" << saved.components[1].localId
            << R"(, "components": { "physics.rigidbody": { "v": 1 }, "projv.netid": { "v": 1, "id": 300 } } } ] })";
        projv::Scene avatar;
        ComponentHandle body = utils::addComponent(avatar, projv::ComponentKind::Chunk, "body", INVALID_COMPONENT_HANDLE, 16, 0.25f);
        utils::queueVoxelAdd(avatar, body, box({0, 0, 0}, {1, 6, 1}));
        utils::updateScene(avatar);
        REQUIRE(utils::saveComposeToDisk(avatar, INVALID_COMPONENT_HANDLE, (folder / "avatar").string()));
    }

    std::unique_ptr<Machine> fieldServer(net::LoopbackNetwork& network, const std::filesystem::path& folder) {
        auto m = std::make_unique<Machine>();
        m->loadScene(folder);
        installWorld(m->app);
        runtime::ServerConfig config;
        config.sceneFolder = folder.string();
        config.loadRule = utils::loadNear(24.0f, 48.0f);
        const std::string avatar = (folder / "avatar").string();
        config.spawnPlayer = [avatar](projv::World& world, runtime::ClientId id) {
            Entity e = runtime::instantiatePrefab(world, avatar, vec3(id == 1 ? -100.0f : 100.0f, 0.0f, 0.0f));
            world.emplace<projv::CharacterController>(e);
            world.emplace<projv::Replicated>(e, projv::Replicated{"avatar", {"physics.character"}});
            return e;
        };
        REQUIRE(runtime::installServer(m->app, network.endpoint(), config));
        m->app.runStartup();
        m->app.runFrame();
        return m;
    }

    // A client that opens the world lazily: it has what the server says, and no more.
    std::unique_ptr<Machine> lazyClient(net::LoopbackNetwork& network, const std::filesystem::path& folder, const std::string& name) {
        auto m = std::make_unique<Machine>();
        runtime::WorldLoadingConfig loading;
        loading.loader.threaded = false;
        loading.loader.maxLoadsPerUpdate = 64;
        REQUIRE(runtime::openWorld(m->app, folder.string(), loading));
        installWorld(m->app);
        runtime::ClientConfig config;
        config.sceneFolder = folder.string();
        config.name = name;
        REQUIRE(runtime::installClient(m->app, network.endpoint(), config));
        m->app.runStartup();
        m->app.runFrame();
        return m;
    }

    vec3 serverPlayer(Machine& s, projv::NetId id) {
        return runtime::characterState(s.app.world, runtime::entityForNetId(s.app.world, id)).position;
    }
}

TEST_CASE("per-client loading: two players 200 m apart each have only their own part of the world") {
    TempDir dir("replication_field");
    writeField(dir.path);
    net::LoopbackNetwork network(14);
    network.setConditions({0.04});
    auto s = fieldServer(network, dir.path);
    auto a = lazyClient(network, dir.path, "ann");
    run(network, {s.get(), a.get()}, 0.5);   // Ann is client 1, at x = -100
    auto b = lazyClient(network, dir.path, "bo");
    run(network, {s.get(), a.get(), b.get()}, 2.0);
    REQUIRE(a->joined.size() == 1);
    REQUIRE(b->joined.size() == 1);
    const projv::NetId annId = a->joined[0].player, boId = b->joined[0].player;

    for (auto [m, me] : {std::pair{a.get(), annId}, std::pair{b.get(), boId}}) {
        const auto& loader = runtime::worldLoader(m->app.world);
        const auto& scene = m->app.world.ctx().get<projv::Scene>();
        const vec3 at = serverPlayer(*s, me);
        CHECK(loader.stats().loaded > 0);
        CHECK(loader.stats().loaded < 60);   // of 400: about the 48 m around them
        for (utils::BlockId id = 0; id < loader.blocks().size(); id++)
            if (loader.blocks().loaded(id)) CHECK(utils::distanceToBox(loader.blocks().bounds(scene, id), at) < 60.0f);
    }
    // Neither has the other; only Bo has the crate by him, simulated.
    CHECK(runtime::entityForNetId(a->app.world, boId) == projv::NullEntity);
    CHECK(runtime::entityForNetId(b->app.world, annId) == projv::NullEntity);
    CHECK(runtime::entityForNetId(a->app.world, projv::NetId{300}) == projv::NullEntity);
    Entity crate = runtime::entityForNetId(b->app.world, projv::NetId{300});
    REQUIRE(crate != projv::NullEntity);
    CHECK(runtime::hasBody(b->app.world, crate));
    // Both stand on what they have loaded.
    CHECK(runtime::characterState(a->app.world, runtime::entityForNetId(a->app.world, annId)).position.y > -0.05f);

    // They walk toward each other. Ann's world loads ahead of her; Bo appears to her where he is.
    runtime::setLocalInput(a->app.world, vec3(8, 0, 0));
    runtime::setLocalInput(b->app.world, vec3(-8, 0, 0));
    float lowest = 1e9f, firstSight = -1.0f;
    for (int frame = 0; frame < 60 * 14; frame++) {
        run(network, {s.get(), a.get(), b.get()}, DT);
        Entity me = runtime::entityForNetId(a->app.world, annId);
        lowest = std::min(lowest, runtime::characterState(a->app.world, me).position.y);
        Entity him = runtime::entityForNetId(a->app.world, boId);
        if (him != projv::NullEntity && firstSight < 0.0f) {
            run(network, {s.get(), a.get(), b.get()}, DT);   // its first pose drawn
            firstSight = glm::distance(a->app.world.get<projv::Transform>(him).position, serverPlayer(*s, boId));
        }
        if (glm::distance(serverPlayer(*s, annId), serverPlayer(*s, boId)) < 6.0f) break;
    }
    runtime::setLocalInput(a->app.world, vec3(0.0f));
    runtime::setLocalInput(b->app.world, vec3(0.0f));
    run(network, {s.get(), a.get(), b.get()}, 1.0);
    CHECK(glm::distance(serverPlayer(*s, annId), serverPlayer(*s, boId)) < 8.0f);   // they met
    CHECK(lowest > -0.05f);                    // never fell through what had not loaded
    REQUIRE(firstSight >= 0.0f);
    CHECK(firstSight < 1.5f);                  // he appeared where he was, not somewhere else and slid in
    // Bo walked 95 m away from his crate: his client has let it go (and Ann's never had it).
    CHECK(runtime::entityForNetId(b->app.world, projv::NetId{300}) == projv::NullEntity);
    CHECK(runtime::entityForNetId(a->app.world, projv::NetId{300}) == projv::NullEntity);
    CHECK(runtime::entityForNetId(a->app.world, boId) != projv::NullEntity);   // and each other, now near
    CHECK(runtime::sessionStats(a->app.world).largestCorrection < 0.05f);
}

// ---- Scale (N7) -------------------------------------------------------------------------------------

TEST_CASE("scale: forty clients in one process each get everyone, within their budget, and stats say so") {
    TempDir dir("replication_scale");
    writeArena(dir.path);
    net::LoopbackNetwork network(15);
    network.setConditions({0.02});
    auto s = std::make_unique<Machine>();
    s->loadScene(dir.path);
    installWorld(s->app);
    runtime::ServerConfig config;
    config.sceneFolder = dir.path.string();
    config.maxClients = 64;
    config.bytesPerSecond = 20000;
    const std::string avatar = (dir.path / "avatar").string();
    config.spawnPlayer = [avatar](projv::World& world, runtime::ClientId id) {
        const float a = float(id) * 2.39996f, r = 3.0f + 1.3f * std::sqrt(float(id));
        Entity e = runtime::instantiatePrefab(world, avatar, vec3(std::cos(a) * r, 0.0f, std::sin(a) * r));
        world.emplace<projv::CharacterController>(e);
        world.emplace<projv::Replicated>(e, projv::Replicated{"avatar", {"physics.character"}});
        return e;
    };
    REQUIRE(runtime::installServer(s->app, network.endpoint(), config));
    s->app.runStartup();
    s->app.runFrame();

    // Bots: no physics, a scene each, walking about.
    std::vector<std::unique_ptr<Machine>> bots;
    for (int i = 0; i < 40; i++) {
        auto m = std::make_unique<Machine>();
        m->loadScene(dir.path);
        runtime::ClientConfig c;
        c.sceneFolder = dir.path.string();
        c.name = "bot" + std::to_string(i);
        REQUIRE(runtime::installClient(m->app, network.endpoint(), c));
        m->app.runStartup();
        m->app.runFrame();
        bots.push_back(std::move(m));
    }
    std::vector<Machine*> all{s.get()};
    for (auto& b : bots) all.push_back(b.get());
    run(network, all, 1.0);
    for (size_t i = 0; i < bots.size(); i++)
        runtime::setLocalInput(bots[i]->app.world, vec3(std::cos(float(i)), 0, std::sin(float(i))) * 2.0f);
    const auto before = runtime::clientStats(s->app.world);
    run(network, all, 3.0);
    const auto after = runtime::clientStats(s->app.world);

    REQUIRE(after.size() == 40);
    for (size_t i = 0; i < after.size(); i++) {
        CHECK(after[i].entities == 40);   // every player, its own too
        const double perSecond = double(after[i].snapshotBytes - before[i].snapshotBytes) / 3.0;
        CHECK(perSecond <= 20000.0 * 1.05);
        CHECK(perSecond > 5000.0);          // and the budget is used: forty moving players
        CHECK(after[i].bytesSent > after[i].snapshotBytes);
    }
    for (auto& b : bots) CHECK(proxies(*b) == 40);
}

TEST_CASE("replication: a client's game messages reach the server's game, in order, from that client") {
    TempDir dir("replication_messages");
    writeArena(dir.path);
    net::LoopbackNetwork network(16);
    network.setConditions({0.03, 0.01, 0.1});
    auto s = std::make_unique<Machine>();
    s->loadScene(dir.path);
    installWorld(s->app);
    runtime::ServerConfig config;
    config.sceneFolder = dir.path.string();
    std::vector<std::pair<runtime::ClientId, std::vector<uint8_t>>> heard;
    config.onMessage = [&](projv::World&, runtime::ClientId from, Entity, std::span<const uint8_t> message) {
        heard.push_back({from, std::vector<uint8_t>(message.begin(), message.end())});
    };
    REQUIRE(runtime::installServer(s->app, network.endpoint(), config));
    s->app.runStartup();
    s->app.runFrame();
    auto a = client(network, dir.path, "ann");
    const std::vector<uint8_t> early{9};
    runtime::sendToServer(a->app.world, early);   // before the welcome: dropped
    run(network, {s.get(), a.get()}, 1.0);
    for (uint8_t i = 0; i < 20; i++) runtime::sendToServer(a->app.world, std::vector<uint8_t>{i, uint8_t(i * 2)});
    run(network, {s.get(), a.get()}, 1.5);
    REQUIRE(heard.size() == 20);   // reliable: every one, through 10% loss
    for (uint8_t i = 0; i < 20; i++) {
        CHECK(heard[i].first == a->joined[0].you);
        CHECK(heard[i].second == std::vector<uint8_t>{i, uint8_t(i * 2)});
    }
}

// ---- Corrections that settle (after playing the demo) ---------------------------------------------

TEST_CASE("corrections: once the server's pile is at rest, clients stop being corrected, and nothing local moves it") {
    TempDir dir("replication_rest");
    writeArena(dir.path, 4);
    net::LoopbackNetwork network(17);
    network.setConditions({0.075, 0.01, 0.05});
    auto s = server(network, dir.path);
    auto a = client(network, dir.path, "ann");
    run(network, {s.get(), a.get()}, 2.0);
    runtime::setVelocity(s->app.world, runtime::entityForNetId(s->app.world, projv::NetId{102}), vec3(0, 2, -9), vec3(4, 0, 0));
    run(network, {s.get(), a.get()}, 8.0);
    for (auto [id, state] : crates(*s)) REQUIRE_FALSE(state.awake);

    const uint64_t settled = runtime::sessionStats(a->app.world).corrections;
    run(network, {s.get(), a.get()}, 3.0);
    CHECK(runtime::sessionStats(a->app.world).corrections == settled);   // no more: it is at rest
    const auto truth = crates(*s);
    // Something on Ann's side pokes a crate the server has at rest: it does not move here.
    Entity poked = runtime::entityForNetId(a->app.world, projv::NetId{104});
    runtime::addVelocity(a->app.world, poked, vec3(0, 6, 3));
    run(network, {s.get(), a.get()}, 2.0);
    for (auto [id, state] : crates(*a)) CHECK(glm::distance(state.position, truth.at(id).position) < 0.01f);
}

TEST_CASE("corrections: a body moving steadily is not corrected every snapshot") {
    TempDir dir("replication_glide");
    writeArena(dir.path, 1);   // one crate, NetId 102
    net::LoopbackNetwork network(18);
    network.setConditions({0.075});
    auto s = server(network, dir.path);
    auto a = client(network, dir.path, "ann");
    run(network, {s.get(), a.get()}, 2.0);
    // Slid along the floor on the server only; the client learns of it from snapshots.
    runtime::setVelocity(s->app.world, runtime::entityForNetId(s->app.world, projv::NetId{102}), vec3(0, 0, -4));
    run(network, {s.get(), a.get()}, 0.5);
    const uint64_t before = runtime::sessionStats(a->app.world).corrections;
    run(network, {s.get(), a.get()}, 1.0);
    // Thirty snapshots a second came; with each snapshot brought up to now, few needed acting on.
    CHECK(runtime::sessionStats(a->app.world).corrections - before < 10);
}
