// A server and clients in one process (runtime/replication.h, networking.md N2): each its own
// Application, over net::LoopbackNetwork, all on a clock the test owns. No window, no sockets.

#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/application.h"
#include "net/transport.h"
#include "runtime/entities.h"
#include "runtime/net.h"
#include "runtime/physics.h"
#include "runtime/replication.h"
#include "runtime/scene_bridge.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
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

    // An arena: a 32 m floor (its top at y = 0) with a static collider, and avatar/, a prefab.
    void writeArena(const std::filesystem::path& folder) {
        {
            projv::Scene scene;
            ComponentHandle floor = utils::addComponent(scene, projv::ComponentKind::Chunk, "floor", INVALID_COMPONENT_HANDLE, 16, 0.5f);
            utils::queueVoxelAdd(scene, floor, box({0, 0, 0}, {63, 1, 63}));
            utils::updateScene(scene);
            utils::setComponentTransform(scene, floor, vec3(-16, -1, -16), projv::core::quat(1, 0, 0, 0), 1.0f);
            REQUIRE(utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder.string()));
            uint32_t id = utils::loadComposeFromDisk(folder.string()).components[0].localId;
            std::ofstream(folder / "entities.json") << R"({ "version": 1, "entities": [ { "name": "floor", "link": )" << id
                                                    << R"(, "components": { "physics.static": { "v": 1 } } } ] })";
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

    std::unique_ptr<Machine> server(net::LoopbackNetwork& network, const std::filesystem::path& folder) {
        auto m = std::make_unique<Machine>();
        m->loadScene(folder);
        runtime::installPhysics(m->app);
        runtime::spawnEntities(m->app.world);
        runtime::ServerConfig config;
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

    size_t proxies(Machine& m) { return m.app.world.view<projv::NetProxy>().size(); }
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
