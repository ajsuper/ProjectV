#include "runtime/replication.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include <glm/gtc/quaternion.hpp>

#include "core/log.h"
#include "net/wire.h"
#include "runtime/entities.h"
#include "runtime/physics.h"
#include "runtime/scene_bridge.h"

namespace projv::runtime {
    namespace {
        enum class Message : uint8_t { Hello = 1, Welcome, Refuse, Spawn, Despawn, Snapshot, Input };

        constexpr size_t   MAX_TEXT = 1u << 16;         // a name, a reason, a prefab path
        constexpr size_t   MAX_COMPONENTS = 1u << 20;   // an entity's components, as JSON
        constexpr size_t   MAX_SNAPSHOT_BYTES = 1100;   // under a UDP packet, so ENet need not fragment
        constexpr uint32_t INPUT_REDUNDANCY = 4;        // inputs repeated in each message
        constexpr size_t   MAX_POSES = 64;              // a proxy's history

        void send(Events* events, auto event) {
            if (events) events->send(std::move(event));
        }

        std::vector<uint8_t> text(const std::string& s) { return {s.begin(), s.end()}; }
        std::string text(const std::vector<uint8_t>& b) { return {b.begin(), b.end()}; }

        void writeQuat(net::BitWriter& w, core::quat q) { net::writeRotation(w, q, 12); }

        // Where an entity is in the simulation: its character's or body's pose, else its Transform.
        std::pair<core::vec3, core::quat> poseOf(const World& world, Entity e) {
            core::quat rotation(1, 0, 0, 0);
            if (const Transform* t = world.try_get<Transform>(e)) rotation = t->rotation;
            if (world.all_of<PhysicsCharacter>(e)) return {characterState(world, e).position, rotation};
            if (hasBody(world, e)) {
                BodyState s = bodyPose(world, e);
                return {s.position, s.rotation};
            }
            if (const Transform* t = world.try_get<Transform>(e)) return {t->position, t->rotation};
            return {core::vec3(0.0f), rotation};
        }

        // ---- The server ----------------------------------------------------------------------------

        struct ServerSession {
            std::unique_ptr<net::Transport> transport;
            ServerConfig config;
            uint64_t sceneHash = 0;
            struct Client {
                ClientId    id = 0;
                bool        welcomed = false;
                Entity      player = NullEntity;
                std::string name;
                core::vec3  move{0.0f};
                bool        jump = false;
                uint64_t    lastInput = 0;
            };
            std::map<net::PeerId, Client> clients;
            std::map<Entity, NetId>       sent;          // replicated entities clients have, and their ids
            std::vector<Entity>           spawning;      // Replicated added: sent at the next "net: send"
            std::vector<NetId>            despawning;
            std::vector<net::NetEvent>    early;         // gathered while flushing in "net: send"
            uint64_t                      lastSnapshot = 0;
            uint64_t                      refused = 0;
        };

        ServerSession* serverOf(World& world) { return world.ctx().find<ServerSession>(); }

        void replicatedAdded(World& world, Entity e) {
            if (ServerSession* s = serverOf(world)) s->spawning.push_back(e);
        }
        void replicatedGone(World& world, Entity e) {
            ServerSession* s = serverOf(world);
            if (!s) return;
            if (auto it = s->sent.find(e); it != s->sent.end()) {
                s->despawning.push_back(it->second);
                s->sent.erase(it);
            }
        }

        std::vector<uint8_t> spawnMessage(World& world, Entity e, NetId id) {
            const Replicated& r = world.get<Replicated>(e);
            nlohmann::json components = entityComponents(world, e, true);
            for (const std::string& key : r.omit) components.erase(key);
            net::BitWriter w;
            w.varint(uint8_t(Message::Spawn));
            w.varint(id.value);
            w.bytes(text(r.prefab));
            auto [position, rotation] = poseOf(world, e);
            for (int i = 0; i < 3; i++) w.f32(position[i]);
            writeQuat(w, rotation);
            const Transform* t = world.try_get<Transform>(e);
            w.f32(t ? t->scale : 1.0f);
            w.bytes(text(components.dump()));
            return w.take();
        }

        void sendAll(ServerSession& s, net::Channel channel, const std::vector<uint8_t>& data) {
            for (auto& [peer, client] : s.clients)
                if (client.welcomed) s.transport->send(peer, channel, data);
        }

        void refuse(ServerSession& s, net::PeerId peer, const std::string& reason) {
            core::info("net: refused a client: {}", reason);
            net::BitWriter w;
            w.varint(uint8_t(Message::Refuse));
            w.bytes(text(reason));
            s.transport->send(peer, net::Channel::Reliable, w.data());
            s.transport->disconnect(peer);
            s.refused++;
        }

        ClientId freeClientId(const ServerSession& s) {
            std::set<ClientId> used;
            for (const auto& [peer, c] : s.clients) used.insert(c.id);
            ClientId id = 1;
            while (used.count(id)) id++;
            return id;
        }

        void serverHello(World& world, ServerSession& s, net::PeerId peer, net::BitReader& r) {
            const uint32_t protocol = uint32_t(r.varint());
            const uint64_t hash = r.bits(64);
            const std::string name = text(r.bytes(MAX_TEXT));
            if (r.failed) return refuse(s, peer, "a malformed hello");
            if (protocol != PROTOCOL_VERSION)
                return refuse(s, peer, "protocol " + std::to_string(protocol) + ", the server speaks " +
                                           std::to_string(PROTOCOL_VERSION));
            if (hash != s.sceneHash) return refuse(s, peer, "its scene files are not the server's");
            ServerSession::Client& client = s.clients[peer];
            if (client.welcomed) return;
            client.id = freeClientId(s);
            client.name = name;
            client.welcomed = true;
            if (s.config.spawnPlayer) client.player = s.config.spawnPlayer(world, client.id);
            NetId playerId;
            if (client.player != NullEntity && world.valid(client.player)) {
                if (!world.all_of<Replicated>(client.player)) world.emplace<Replicated>(client.player);
                playerId = assignNetId(world, client.player);
            }
            net::BitWriter w;
            w.varint(uint8_t(Message::Welcome));
            w.varint(client.id);
            w.varint(playerId.value);
            w.f32(world.ctx().get<Time>().fixedDelta);
            w.varint(world.ctx().get<Time>().fixedStep);
            s.transport->send(peer, net::Channel::Reliable, w.data());
            // Everything that is there already. What is spawning goes to everyone at the next send.
            std::vector<std::pair<NetId, Entity>> existing;
            for (auto [e, id] : s.sent) existing.push_back({id, e});
            std::sort(existing.begin(), existing.end(), [](auto& a, auto& b) { return a.first.value < b.first.value; });
            for (auto [id, e] : existing)
                if (world.valid(e)) s.transport->send(peer, net::Channel::Reliable, spawnMessage(world, e, id));
            core::info("net: client {} ('{}') joined", client.id, name);
            send(world.ctx().find<Events>(), ClientJoined{client.id, client.player, name});
        }

        void serverInput(ServerSession::Client& client, net::BitReader& r) {
            const uint32_t count = uint32_t(r.varint());
            if (r.failed || count > INPUT_REDUNDANCY) return;
            uint64_t newest = client.lastInput;
            for (uint32_t i = 0; i < count; i++) {
                const uint64_t sequence = r.varint();
                const core::vec3 move = net::readVector(r, 64.0f, 16);
                const bool jump = r.boolean();
                if (r.failed) return;
                if (sequence <= client.lastInput) continue;
                if (jump) client.jump = true;   // a jump in any input not seen before
                if (sequence > newest) {
                    newest = sequence;
                    client.move = move;
                }
            }
            client.lastInput = newest;
        }

        void serverReceive(Application& app) {
            World& world = app.world;
            ServerSession& s = *serverOf(world);
            std::vector<net::NetEvent> events = std::move(s.early);
            s.early.clear();
            s.transport->poll(events);
            for (net::NetEvent& e : events) {
                switch (e.kind) {
                    case net::NetEvent::Kind::Connected:
                        s.clients[e.peer] = {};
                        break;
                    case net::NetEvent::Kind::Disconnected: {
                        auto it = s.clients.find(e.peer);
                        if (it == s.clients.end()) break;
                        ServerSession::Client client = it->second;
                        s.clients.erase(it);
                        if (!client.welcomed) break;
                        if (client.player != NullEntity && world.valid(client.player)) world.destroy(client.player);
                        core::info("net: client {} left", client.id);
                        send(world.ctx().find<Events>(), ClientLeft{client.id});
                        break;
                    }
                    case net::NetEvent::Kind::Received: {
                        auto it = s.clients.find(e.peer);
                        if (it == s.clients.end()) break;
                        net::BitReader r(e.data);
                        const auto kind = Message(r.varint());
                        if (kind == Message::Hello && e.channel == net::Channel::Reliable) serverHello(world, s, e.peer, r);
                        else if (kind == Message::Input && it->second.welcomed) serverInput(it->second, r);
                        break;
                    }
                }
            }
        }

        void serverApplyInput(Application& app) {
            World& world = app.world;
            ServerSession& s = *serverOf(world);
            for (auto& [peer, client] : s.clients) {
                if (!client.welcomed || client.player == NullEntity || !world.valid(client.player)) continue;
                if (s.config.applyInput) s.config.applyInput(world, client.id, client.player, client.move, client.jump);
                else setCharacterInput(world, client.player, client.move, client.jump);
                client.jump = false;
            }
        }

        void serverSend(Application& app) {
            World& world = app.world;
            ServerSession& s = *serverOf(world);
            // Spawns, in NetId order, then despawns.
            std::vector<Entity> spawning;
            spawning.swap(s.spawning);
            std::vector<std::pair<NetId, Entity>> fresh;
            for (Entity e : spawning) {
                if (!world.valid(e) || !world.all_of<Replicated>(e) || s.sent.count(e)) continue;
                fresh.push_back({assignNetId(world, e), e});
            }
            std::sort(fresh.begin(), fresh.end(), [](auto& a, auto& b) { return a.first.value < b.first.value; });
            for (auto [id, e] : fresh) {
                s.sent[e] = id;
                sendAll(s, net::Channel::Reliable, spawnMessage(world, e, id));
            }
            for (NetId id : s.despawning) {
                net::BitWriter w;
                w.varint(uint8_t(Message::Despawn));
                w.varint(id.value);
                sendAll(s, net::Channel::Reliable, w.data());
            }
            s.despawning.clear();

            // Snapshots, at the rate asked, in as many packets as it takes.
            const Time& time = world.ctx().get<Time>();
            const uint64_t every = std::max<uint64_t>(1, uint64_t(1.0f / (time.fixedDelta * std::max(s.config.snapshotRate, 0.1f)) + 0.5f));
            if (time.fixedStep >= s.lastSnapshot + every && !s.sent.empty()) {
                s.lastSnapshot = time.fixedStep;
                std::vector<std::pair<NetId, Entity>> all;
                for (auto [e, id] : s.sent) if (world.valid(e)) all.push_back({id, e});
                std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first.value < b.first.value; });
                size_t next = 0;
                while (next < all.size()) {
                    net::BitWriter w;
                    w.varint(uint8_t(Message::Snapshot));
                    w.varint(time.fixedStep);
                    net::BitWriter body;
                    uint32_t count = 0;
                    for (; next < all.size() && body.data().size() < MAX_SNAPSHOT_BYTES - 64; next++, count++) {
                        auto [position, rotation] = poseOf(world, all[next].second);
                        body.varint(all[next].first.value);
                        for (int i = 0; i < 3; i++) body.f32(position[i]);
                        writeQuat(body, rotation);
                    }
                    w.varint(count);
                    w.bytes(body.data());
                    sendAll(s, net::Channel::Unreliable, w.data());
                }
            }
            s.transport->poll(s.early);   // flush; what arrived waits for the next receive
        }

        // ---- The client ----------------------------------------------------------------------------

        struct ClientSession {
            std::unique_ptr<net::Transport> transport;
            ClientConfig config;
            uint64_t     sceneHash = 0;
            net::PeerId  server = net::NO_PEER;
            bool         welcomed = false, gone = false;
            ClientId     you = 0;
            NetId        player;
            float        tickDelta = 1.0f / 60.0f;
            uint64_t     latestTick = 0;
            double       clock = 0.0, latestAt = 0.0;
            std::map<uint32_t, Entity> proxies;
            std::string  refusal;
            uint64_t     snapshots = 0;
            // Input.
            core::vec3   move{0.0f};
            bool         jumpPending = false;
            uint64_t     sequence = 0;
            struct Sent { uint64_t sequence; core::vec3 move; bool jump; };
            std::vector<Sent> recent;
            std::vector<net::NetEvent> early;
        };

        ClientSession* clientOf(World& world) { return world.ctx().find<ClientSession>(); }

        void clientSpawn(World& world, ClientSession& c, net::BitReader& r) {
            const uint32_t id = uint32_t(r.varint());
            const std::string prefab = text(r.bytes(MAX_TEXT));
            core::vec3 position;
            for (int i = 0; i < 3; i++) position[i] = r.f32();
            const core::quat rotation = net::readRotation(r, 12);
            const float scale = r.f32();
            const std::string components = text(r.bytes(MAX_COMPONENTS));
            if (r.failed || id == 0) return;
            if (c.proxies.count(id)) return;
            Entity e = NullEntity;
            if (!prefab.empty()) {
                std::string folder = (std::filesystem::path(c.config.sceneFolder) / prefab).string();
                if (prefab.find("..") != std::string::npos) folder.clear();   // the scene's own folders only
                if (!folder.empty()) e = instantiatePrefab(world, folder, position, rotation, std::isfinite(scale) ? scale : 1.0f);
                if (e == NullEntity) core::warn("net: could not make '{}' for entity {}", prefab, id);
            }
            if (e == NullEntity) {
                e = world.create();
                world.emplace<Transform>(e, Transform{position, rotation, std::isfinite(scale) ? scale : 1.0f});
            }
            nlohmann::json json = nlohmann::json::parse(components, nullptr, false);
            if (json.is_object()) {
                json.erase("projv.netid");
                applyEntityComponents(world, e, json, "the server");
            }
            world.emplace_or_replace<NetId>(e, NetId{id});
            world.emplace_or_replace<NetProxy>(e, NetProxy{{{c.latestTick, position, rotation}}});
            c.proxies[id] = e;
        }

        void clientSnapshot(World& world, ClientSession& c, net::BitReader& r) {
            const uint64_t tick = r.varint();
            const uint32_t count = uint32_t(r.varint());
            const std::vector<uint8_t> body = r.bytes(MAX_SNAPSHOT_BYTES * 2);
            if (r.failed || count > body.size()) return;
            c.snapshots++;
            if (tick > c.latestTick) {
                c.latestTick = tick;
                c.latestAt = c.clock;
            }
            net::BitReader b(body);
            for (uint32_t i = 0; i < count; i++) {
                const uint32_t id = uint32_t(b.varint());
                core::vec3 position;
                for (int k = 0; k < 3; k++) position[k] = b.f32();
                const core::quat rotation = net::readRotation(b, 12);
                if (b.failed) return;
                auto it = c.proxies.find(id);
                if (it == c.proxies.end() || !world.valid(it->second)) continue;
                NetProxy* proxy = world.try_get<NetProxy>(it->second);
                if (!proxy || !std::isfinite(position.x + position.y + position.z)) continue;
                if (!proxy->poses.empty() && proxy->poses.back().tick >= tick) continue;
                proxy->poses.push_back({tick, position, rotation});
                if (proxy->poses.size() > MAX_POSES) proxy->poses.erase(proxy->poses.begin());
            }
        }

        void dropProxies(World& world, ClientSession& c) {
            for (auto [id, e] : c.proxies)
                if (world.valid(e)) world.destroy(e);
            c.proxies.clear();
        }

        void clientReceive(Application& app) {
            World& world = app.world;
            ClientSession& c = *clientOf(world);
            c.clock += app.time().unscaledDelta;
            std::vector<net::NetEvent> events = std::move(c.early);
            c.early.clear();
            c.transport->poll(events);
            for (net::NetEvent& e : events) {
                if (e.peer != c.server) continue;
                if (e.kind == net::NetEvent::Kind::Connected) {
                    net::BitWriter w;
                    w.varint(uint8_t(Message::Hello));
                    w.varint(PROTOCOL_VERSION);
                    w.bits(c.sceneHash, 64);
                    w.bytes(text(c.config.name));
                    c.transport->send(c.server, net::Channel::Reliable, w.data());
                    continue;
                }
                if (e.kind == net::NetEvent::Kind::Disconnected) {
                    if (c.gone) continue;
                    c.gone = true;
                    c.welcomed = false;
                    dropProxies(world, c);
                    std::string reason = c.refusal.empty() ? "disconnected" : c.refusal;
                    core::info("net: left the server: {}", reason);
                    send(world.ctx().find<Events>(), LeftServer{reason});
                    continue;
                }
                net::BitReader r(e.data);
                switch (Message(r.varint())) {
                    case Message::Welcome: {
                        const ClientId you = ClientId(r.varint());
                        const uint32_t player = uint32_t(r.varint());
                        const float delta = r.f32();
                        const uint64_t tick = r.varint();
                        if (r.failed) break;
                        c.welcomed = true;
                        c.you = you;
                        c.player = NetId{player};
                        if (delta > 0.0f && delta < 1.0f) c.tickDelta = delta;
                        c.latestTick = tick;
                        c.latestAt = c.clock;
                        core::info("net: joined as client {}", you);
                        send(world.ctx().find<Events>(), JoinedServer{you, c.player});
                        break;
                    }
                    case Message::Refuse:
                        c.refusal = text(r.bytes(MAX_TEXT));
                        break;
                    case Message::Spawn:
                        if (c.welcomed) clientSpawn(world, c, r);
                        break;
                    case Message::Despawn: {
                        const uint32_t id = uint32_t(r.varint());
                        auto it = c.proxies.find(id);
                        if (r.failed || it == c.proxies.end()) break;
                        if (world.valid(it->second)) world.destroy(it->second);
                        c.proxies.erase(it);
                        break;
                    }
                    case Message::Snapshot:
                        if (c.welcomed) clientSnapshot(world, c, r);
                        break;
                    default:
                        break;
                }
            }
        }

        void clientSendInput(Application& app) {
            ClientSession& c = *clientOf(app.world);
            if (!c.welcomed) return;
            c.recent.push_back({++c.sequence, c.move, c.jumpPending});
            c.jumpPending = false;
            if (c.recent.size() > INPUT_REDUNDANCY) c.recent.erase(c.recent.begin());
            net::BitWriter w;
            w.varint(uint8_t(Message::Input));
            w.varint(c.recent.size());
            for (auto it = c.recent.rbegin(); it != c.recent.rend(); ++it) {
                w.varint(it->sequence);
                net::writeVector(w, it->move, 64.0f, 16);
                w.boolean(it->jump);
            }
            c.transport->send(c.server, net::Channel::Unreliable, w.data());
        }

        // Each proxy between the two poses around `interpolationDelay` behind the newest snapshot.
        void clientPresent(Application& app) {
            World& world = app.world;
            ClientSession& c = *clientOf(world);
            if (c.welcomed) {
                const double estimate = double(c.latestTick) + (c.clock - c.latestAt) / double(c.tickDelta);
                const double at = estimate - double(c.config.interpolationDelay) / double(c.tickDelta);
                for (auto [id, e] : c.proxies) {
                    if (!world.valid(e)) continue;
                    const NetProxy* proxy = world.try_get<NetProxy>(e);
                    if (!proxy || proxy->poses.empty()) continue;
                    const auto& poses = proxy->poses;
                    core::vec3 position = poses.back().position;
                    core::quat rotation = poses.back().rotation;
                    if (at <= double(poses.front().tick)) {
                        position = poses.front().position;
                        rotation = poses.front().rotation;
                    } else {
                        for (size_t i = 1; i < poses.size(); i++) {
                            if (double(poses[i].tick) < at) continue;
                            const auto& a = poses[i - 1];
                            const auto& b = poses[i];
                            const float t = float((at - double(a.tick)) / double(std::max<uint64_t>(b.tick - a.tick, 1)));
                            position = glm::mix(a.position, b.position, t);
                            rotation = glm::slerp(a.rotation, b.rotation, t);
                            break;
                        }
                    }
                    const Transform* now = world.try_get<Transform>(e);
                    if (now && now->position == position && now->rotation == rotation) continue;
                    float scale = now ? now->scale : 1.0f;
                    world.emplace_or_replace<Transform>(e, Transform{position, rotation, scale});
                }
            }
            c.transport->poll(c.early);   // flush the input
        }
    }

    // ---- Public -------------------------------------------------------------------------------------

    uint64_t sceneFilesHash(const std::string& folder) {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (auto it = std::filesystem::recursive_directory_iterator(folder, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
            if (it->is_regular_file()) files.push_back(it->path());
        std::sort(files.begin(), files.end());
        uint64_t hash = 1469598103934665603ull;
        auto add = [&hash](const char* data, size_t size) {
            for (size_t i = 0; i < size; i++) { hash ^= uint8_t(data[i]); hash *= 1099511628211ull; }
        };
        for (const auto& path : files) {
            std::string relative = std::filesystem::relative(path, folder).generic_string();
            add(relative.data(), relative.size() + 1);
            std::ifstream in(path, std::ios::binary);
            std::vector<char> buffer(1 << 16);
            while (in.read(buffer.data(), std::streamsize(buffer.size())) || in.gcount() > 0) add(buffer.data(), size_t(in.gcount()));
        }
        return hash;
    }

    bool installServer(Application& app, std::unique_ptr<net::Transport> transport, ServerConfig config) {
        World& world = app.world;
        if (world.ctx().contains<ServerSession>() || !transport) return false;
        if (!transport->listen(config.port, config.maxClients)) {
            core::error("net: the server could not listen on port {}", config.port);
            return false;
        }
        installNetIds(world);
        ServerSession& s = world.ctx().emplace<ServerSession>();
        s.transport = std::move(transport);
        s.sceneHash = sceneFilesHash(config.sceneFolder);
        s.config = std::move(config);
        world.on_construct<Replicated>().connect<&replicatedAdded>();
        world.on_destroy<Replicated>().connect<&replicatedGone>();
        for (auto e : world.view<Replicated>()) s.spawning.push_back(e);
        app.addSystem(Stage::PreUpdate, "net: receive", serverReceive);
        app.addSystem(Stage::FixedUpdate, "net: input", serverApplyInput, {.before = "physics", .after = ""});
        app.addSystem(Stage::PostUpdate, "net: send", serverSend, {.before = "", .after = "physics: present"});
        core::info("net: serving on port {}", s.config.port);
        return true;
    }

    bool installClient(Application& app, std::unique_ptr<net::Transport> transport, ClientConfig config) {
        World& world = app.world;
        if (world.ctx().contains<ClientSession>() || !transport) return false;
        installNetIds(world);
        installSceneBridge(app);
        ClientSession& c = world.ctx().emplace<ClientSession>();
        c.transport = std::move(transport);
        c.sceneHash = sceneFilesHash(config.sceneFolder);
        c.config = std::move(config);
        c.server = c.transport->connect(c.config.host, c.config.port);
        if (c.server == net::NO_PEER) {
            core::error("net: could not start connecting to {}:{}", c.config.host, c.config.port);
            return false;
        }
        app.addSystem(Stage::PreUpdate, "net: receive", clientReceive);
        app.addSystem(Stage::FixedUpdate, "net: input", clientSendInput);
        app.addSystem(Stage::PostUpdate, "net: present", clientPresent, {.before = "scene bridge", .after = ""});
        return true;
    }

    void setLocalInput(World& world, core::vec3 move, bool jump) {
        if (ClientSession* c = clientOf(world)) {
            c->move = move;
            c->jumpPending = c->jumpPending || jump;
        }
    }

    SessionStats sessionStats(const World& world) {
        SessionStats out;
        if (const ServerSession* s = world.ctx().find<ServerSession>()) {
            out.connected = true;
            for (const auto& [peer, client] : s->clients) out.clients += client.welcomed;
            out.serverTick = world.ctx().get<Time>().fixedStep;
            out.messagesRefused = s->refused;
        }
        if (const ClientSession* c = world.ctx().find<ClientSession>()) {
            out.connected = c->welcomed;
            out.you = c->you;
            out.player = c->player;
            out.serverTick = c->latestTick;
            out.snapshotsReceived = c->snapshots;
            out.refusal = c->refusal;
        }
        return out;
    }
}
