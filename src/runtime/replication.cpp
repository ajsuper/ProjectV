#include "runtime/replication.h"

#include <algorithm>
#include <climits>
#include <limits>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>

#include <glm/gtc/quaternion.hpp>

#include "core/log.h"
#include "net/wire.h"
#include "runtime/entities.h"
#include "runtime/physics.h"
#include "runtime/physics/physics_world.h"
#include "runtime/scene_bridge.h"
#include "utils/scene_query.h"

namespace projv::runtime {
    namespace {
        enum class Message : uint8_t { Hello = 1, Welcome, Refuse, Spawn, Despawn, Snapshot, Input, Split, Edit, Check };

        constexpr size_t   MAX_TEXT = 1u << 16;         // a name, a reason, a prefab path
        constexpr size_t   MAX_COMPONENTS = 1u << 20;   // an entity's components, as JSON
        constexpr size_t   MAX_SNAPSHOT_BYTES = 1100;   // under a UDP packet, so ENet need not fragment
        constexpr uint32_t INPUT_REDUNDANCY = 4;        // inputs repeated in each message
        constexpr size_t   MAX_POSES = 64;              // a proxy's history
        constexpr uint64_t MAX_SPLIT_CELLS = 1u << 24;  // the box a split's labels cover
        constexpr uint32_t MAX_SPLIT_PIECES = 4096;
        constexpr size_t   MAX_EDIT_OPS = 1u << 20;

        // ---- World changes on the wire ----

        std::vector<uint8_t> splitMessage(NetId target, const VoxelSplit& split) {
            core::ivec3 low(std::numeric_limits<int>::max()), high(std::numeric_limits<int>::min());
            auto widen = [&](core::ivec3 p) { low = glm::min(low, p); high = glm::max(high, p); };
            for (const auto& piece : split.pieces) for (core::ivec3 v : piece.voxels) widen(v);
            for (core::ivec3 v : split.dust) widen(v);
            net::BitWriter w;
            w.varint(uint8_t(Message::Split));
            w.varint(target.value);
            w.varint(split.pieces.size());
            for (const auto& piece : split.pieces) {
                w.varint(piece.netId.value);
                w.boolean(piece.shard);
                net::writeVector(w, piece.linear, 128.0f, 16);
                net::writeVector(w, piece.angular, 128.0f, 16);
            }
            if (low.x > high.x) {
                w.boolean(false);
                return w.take();
            }
            w.boolean(true);
            const core::ivec3 dims = high - low + core::ivec3(1);
            for (int i = 0; i < 3; i++) w.signedVarint(low[i]);
            for (int i = 0; i < 3; i++) w.varint(uint64_t(dims[i]));
            // A label per cell of the box, x fastest: 0 stays, k the k-th piece, pieces + 1 dust.
            std::vector<uint32_t> labels(size_t(dims.x) * size_t(dims.y) * size_t(dims.z), 0);
            auto cell = [&](core::ivec3 v) {
                v -= low;
                return (size_t(v.z) * size_t(dims.y) + size_t(v.y)) * size_t(dims.x) + size_t(v.x);
            };
            for (size_t k = 0; k < split.pieces.size(); k++)
                for (core::ivec3 v : split.pieces[k].voxels) labels[cell(v)] = uint32_t(k + 1);
            for (core::ivec3 v : split.dust) labels[cell(v)] = uint32_t(split.pieces.size() + 1);
            net::writeLabels(w, labels);
            return w.take();
        }

        bool readSplit(net::BitReader& r, NetId& target, VoxelSplit& split) {
            target = NetId{uint32_t(r.varint())};
            const uint64_t pieces = r.varint();
            if (r.failed || pieces > MAX_SPLIT_PIECES) return false;
            split.pieces.resize(size_t(pieces));
            for (auto& piece : split.pieces) {
                piece.netId = NetId{uint32_t(r.varint())};
                piece.shard = r.boolean();
                piece.linear = net::readVector(r, 128.0f, 16);
                piece.angular = net::readVector(r, 128.0f, 16);
            }
            if (!r.boolean()) return !r.failed;
            core::ivec3 low, dims;
            for (int i = 0; i < 3; i++) low[i] = int(r.signedVarint());
            for (int i = 0; i < 3; i++) dims[i] = int(std::min<uint64_t>(r.varint(), MAX_SPLIT_CELLS));
            if (r.failed || dims.x <= 0 || dims.y <= 0 || dims.z <= 0 ||
                uint64_t(dims.x) * uint64_t(dims.y) * uint64_t(dims.z) > MAX_SPLIT_CELLS) return false;
            const size_t cells = size_t(dims.x) * size_t(dims.y) * size_t(dims.z);
            const std::vector<uint32_t> labels = net::readLabels(r, cells, uint32_t(pieces + 1));
            if (r.failed || labels.size() != cells) return false;
            for (size_t i = 0; i < cells; i++) {
                if (!labels[i]) continue;
                core::ivec3 v = low + core::ivec3(int(i % size_t(dims.x)), int((i / size_t(dims.x)) % size_t(dims.y)),
                                                  int(i / (size_t(dims.x) * size_t(dims.y))));
                if (labels[i] <= pieces) split.pieces[labels[i] - 1].voxels.push_back(v);
                else split.dust.push_back(v);
            }
            return true;
        }

        std::vector<uint8_t> editMessage(NetId target, const std::vector<PendingVoxelOp>& ops) {
            net::BitWriter w;
            w.varint(uint8_t(Message::Edit));
            w.varint(target.value);
            w.varint(ops.size());
            for (const PendingVoxelOp& op : ops) {
                w.boolean(op.isAdd);
                for (int i = 0; i < 3; i++) w.signedVarint(op.position[i]);
                w.bits(op.packedColor, 32);
                w.bits(op.slot, 8);
            }
            return w.take();
        }

        bool readEdit(net::BitReader& r, NetId& target, std::vector<PendingVoxelOp>& ops) {
            target = NetId{uint32_t(r.varint())};
            const uint64_t count = r.varint();
            if (r.failed || count > MAX_EDIT_OPS || count > r.bitsLeft()) return false;
            ops.resize(size_t(count));
            for (PendingVoxelOp& op : ops) {
                op.isAdd = r.boolean();
                for (int i = 0; i < 3; i++) op.position[i] = int(std::clamp<int64_t>(r.signedVarint(), INT32_MIN / 2, INT32_MAX / 2));
                op.packedColor = uint32_t(r.bits(32));
                op.slot = uint8_t(r.bits(8));
            }
            return !r.failed;
        }

        uint64_t contentHashOf(World& world, Entity e) {
            const Scene* scene = world.ctx().find<Scene>();
            ComponentHandle h = scene ? voxelComponentOf(world, e) : INVALID_COMPONENT_HANDLE;
            return h == INVALID_COMPONENT_HANDLE ? 0 : utils::voxelContentHash(*scene, h);
        }

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
                // Inputs not yet applied, by sequence; one is applied a step, in order.
                std::map<uint64_t, std::pair<core::vec3, bool>> inputs;
                uint64_t    applied = 0;           // the last input applied
                bool        started = false;       // a couple of inputs have come: apply from here on
                bool        predicts = false;      // walks its own player: not sent its pose, only acks
                std::map<uint32_t, float> priority;   // by NetId: grows until sent
                std::set<uint32_t>        toldAsleep; // bodies it was last told are asleep
                uint64_t    snapshotBytes = 0;
            };
            std::map<net::PeerId, Client> clients;
            std::map<Entity, NetId>       sent;          // replicated entities clients have, and their ids
            std::vector<Entity>           spawning;      // Replicated added: sent at the next "net: send"
            std::vector<NetId>            despawning;
            std::vector<net::NetEvent>    early;         // gathered while flushing in "net: send"
            uint64_t                      lastSnapshot = 0;
            uint64_t                      refused = 0;
            // World changes since the start, in order: sent to everyone as they happen, and to a
            // client that joins later after its spawns, so it ends where everyone is.
            std::vector<std::vector<uint8_t>> history;
            std::set<Entity>                  fromSplits;    // pieces clients make themselves
            std::vector<Entity>               changed;       // voxels changed this step: checked
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
                if (s->fromSplits.erase(e)) {
                    // A late joiner makes the piece from the history: it must hear it go, too.
                    net::BitWriter w;
                    w.varint(uint8_t(Message::Despawn));
                    w.varint(it->second.value);
                    s->history.push_back(w.take());
                }
                s->sent.erase(it);
            }
        }

        // `owner`: for the client whose player it is, which predicts it, and so is sent all of it.
        std::vector<uint8_t> spawnMessage(World& world, Entity e, NetId id, bool owner = false) {
            const Replicated& r = world.get<Replicated>(e);
            nlohmann::json components = entityComponents(world, e, true);
            if (!owner)
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
            const bool predicts = r.boolean();
            if (r.failed) return refuse(s, peer, "a malformed hello");
            if (protocol != PROTOCOL_VERSION)
                return refuse(s, peer, "protocol " + std::to_string(protocol) + ", the server speaks " +
                                           std::to_string(PROTOCOL_VERSION));
            if (hash != s.sceneHash) return refuse(s, peer, "its scene files are not the server's");
            ServerSession::Client& client = s.clients[peer];
            if (client.welcomed) return;
            client.id = freeClientId(s);
            client.name = name;
            client.predicts = predicts;
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
            for (auto [e, id] : s.sent)
                if (!s.fromSplits.count(e)) existing.push_back({id, e});
            std::sort(existing.begin(), existing.end(), [](auto& a, auto& b) { return a.first.value < b.first.value; });
            for (auto [id, e] : existing)
                if (world.valid(e)) s.transport->send(peer, net::Channel::Reliable, spawnMessage(world, e, id, e == client.player));
            for (const std::vector<uint8_t>& change : s.history) s.transport->send(peer, net::Channel::Reliable, change);
            core::info("net: client {} ('{}') joined", client.id, name);
            send(world.ctx().find<Events>(), ClientJoined{client.id, client.player, name});
        }

        void serverInput(ServerSession::Client& client, net::BitReader& r) {
            const uint32_t count = uint32_t(r.varint());
            if (r.failed || count > INPUT_REDUNDANCY) return;
            for (uint32_t i = 0; i < count; i++) {
                const uint64_t sequence = r.varint();
                const core::vec3 move = net::readVector(r, 64.0f, 16);
                const bool jump = r.boolean();
                if (r.failed) return;
                if (sequence <= client.applied) continue;
                client.inputs.try_emplace(sequence, move, jump);
                client.lastInput = std::max(client.lastInput, sequence);
            }
            // A client far ahead -- or lying -- is held to a second's worth.
            while (client.inputs.size() > 60) client.inputs.erase(client.inputs.begin());
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
                // One input a step, in order: what the client predicted with, step for step. A lost
                // one (gone past its redundancy) is skipped; with none waiting, the last is repeated.
                if (!client.started) client.started = client.inputs.size() >= 2;
                client.jump = false;
                if (client.started && !client.inputs.empty()) {
                    auto next = client.inputs.begin();
                    client.applied = next->first;
                    client.move = next->second.first;
                    client.jump = next->second.second;
                    client.inputs.erase(next);
                }
                if (s.config.applyInput) s.config.applyInput(world, client.id, client.player, client.move, client.jump);
                else setCharacterInput(world, client.player, client.move, client.jump);
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
                for (auto& [peer, client] : s.clients)
                    if (client.welcomed) s.transport->send(peer, net::Channel::Reliable, spawnMessage(world, e, id, e == client.player));
            }
            for (NetId id : s.despawning) {
                net::BitWriter w;
                w.varint(uint8_t(Message::Despawn));
                w.varint(id.value);
                sendAll(s, net::Channel::Reliable, w.data());
            }
            s.despawning.clear();

            // What each changed body's voxels are now, after this step's changes: clients compare.
            if (!s.changed.empty()) {
                std::vector<std::pair<NetId, uint64_t>> hashes;
                std::sort(s.changed.begin(), s.changed.end());
                s.changed.erase(std::unique(s.changed.begin(), s.changed.end()), s.changed.end());
                for (Entity e : s.changed) {
                    const NetId* id = world.valid(e) ? world.try_get<NetId>(e) : nullptr;
                    if (id) hashes.push_back({*id, contentHashOf(world, e)});
                }
                s.changed.clear();
                net::BitWriter w;
                w.varint(uint8_t(Message::Check));
                w.varint(hashes.size());
                for (auto [id, hash] : hashes) {
                    w.varint(id.value);
                    w.bits(hash, 64);
                }
                sendAll(s, net::Channel::Reliable, w.data());
            }

            // Snapshots, at the rate asked: for each client, what matters most first, within its budget.
            const Time& time = world.ctx().get<Time>();
            const uint64_t every = std::max<uint64_t>(1, uint64_t(1.0f / (time.fixedDelta * std::max(s.config.snapshotRate, 0.1f)) + 0.5f));
            if (time.fixedStep >= s.lastSnapshot + every) {
                s.lastSnapshot = time.fixedStep;
                // Everything a snapshot can carry, this once: what was sent (players, pieces) and every
                // moving body with a NetId (authored ones every client has from its files).
                std::map<uint32_t, Entity> candidates;
                for (auto [e, id] : s.sent) if (world.valid(e)) candidates[id.value] = e;
                for (auto [e, body, id] : world.view<PhysicsBody, NetId>().each())
                    if (body.motion == MotionType::Dynamic) candidates[id.value] = e;
                struct Entry { uint32_t id; float weight; bool asleep; std::vector<uint8_t> bits; size_t bitCount; };
                std::vector<Entry> entries;
                for (auto [id, e] : candidates) {
                    auto [position, rotation] = poseOf(world, e);
                    net::BitWriter w;
                    w.varint(id);
                    const PhysicsBody* body = world.try_get<PhysicsBody>(e);
                    // A moving body: every client simulates it too, and is sent its velocities to correct it.
                    const bool moving = body && body->motion == MotionType::Dynamic;
                    BodyState state = moving ? bodyPose(world, e) : BodyState{};
                    w.boolean(moving);
                    w.boolean(moving && !state.awake);
                    net::writePosition(w, position, core::vec3(0.0f), 0.001f);
                    net::writeRotation(w, rotation, 10);
                    float weight = 10.0f;   // a player, or anything posed by snapshots alone
                    if (moving) {
                        net::writeVector(w, state.linearVelocity, 64.0f, 12);
                        net::writeVector(w, state.angularVelocity, 32.0f, 12);
                        weight = state.awake ? 1.0f + 0.5f * glm::length(state.linearVelocity) : 5.0f;
                    }
                    entries.push_back({id, weight, moving && !state.awake, w.data(), w.bitCount()});
                }
                const size_t budget = std::max<size_t>(64, size_t(float(s.config.bytesPerSecond) / std::max(s.config.snapshotRate, 0.1f)));
                for (auto& [peer, client] : s.clients) {
                    if (!client.welcomed) continue;
                    // Gather priority; a body asleep and told so already gathers none.
                    std::vector<std::pair<float, const Entry*>> order;
                    const NetId* own = client.predicts && world.valid(client.player) ? world.try_get<NetId>(client.player) : nullptr;
                    for (const Entry& entry : entries) {
                        if (entry.asleep && client.toldAsleep.count(entry.id)) continue;
                        if (own && own->value == entry.id) continue;
                        float& p = client.priority[entry.id];
                        p += entry.weight;
                        order.push_back({p, &entry});
                    }
                    std::stable_sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
                    size_t spent = 0;
                    size_t next = 0;
                    // The owner's ack, in the first packet: the last input applied, where its player is
                    // after it, and how many inputs wait (what keeps the client's clock in step).
                    bool ack = client.player != NullEntity && world.valid(client.player) && world.all_of<PhysicsCharacter>(client.player);
                    while ((next < order.size() || ack) && spent < budget) {
                        net::BitWriter packet;
                        packet.varint(uint8_t(Message::Snapshot));
                        packet.varint(time.fixedStep);
                        packet.boolean(ack);
                        if (ack) {
                            const CharacterState state = characterState(world, client.player);
                            packet.varint(client.applied);
                            net::writePosition(packet, state.position, core::vec3(0.0f), 0.0001f);
                            net::writeVector(packet, state.velocity, 64.0f, 20);
                            packet.varint(client.inputs.size());
                            ack = false;
                        }
                        net::BitWriter body;
                        uint32_t count = 0;
                        for (; next < order.size(); next++) {
                            const Entry& entry = *order[next].second;
                            if (body.data().size() + entry.bits.size() > MAX_SNAPSHOT_BYTES - 32) break;
                            if (spent + body.data().size() + entry.bits.size() > budget) break;
                            for (size_t bit = 0; bit < entry.bitCount; bit++)
                                body.bits((entry.bits[bit / 8] >> (bit % 8)) & 1u, 1);
                            count++;
                            client.priority[entry.id] = 0.0f;
                            if (entry.asleep) client.toldAsleep.insert(entry.id);
                            else client.toldAsleep.erase(entry.id);
                        }
                        if (count == 0 && next < order.size()) break;
                        packet.varint(count);
                        packet.bytes(body.data());
                        spent += packet.data().size();
                        s.transport->send(peer, net::Channel::Unreliable, packet.data());
                    }
                    client.snapshotBytes += spent;
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
            // World changes, applied in order at fixed steps: one waits until what it names exists.
            struct Change {
                Message kind;
                NetId   target;
                VoxelSplit split;
                std::vector<PendingVoxelOp> ops;
                std::vector<std::pair<NetId, uint64_t>> hashes;   // Check
                int     waited = 0;
            };
            std::vector<Change> changes;
            uint64_t applied = 0, mismatches = 0;
            std::map<uint32_t, uint64_t> corrected;   // by NetId: the tick of the last correction
            uint64_t corrections = 0;
            // Prediction: the own player, walked here; what each input left it as; the newest ack.
            Entity   predicted = NullEntity;
            struct Predicted { uint64_t sequence; core::vec3 move; bool jump; std::vector<uint8_t> state; core::vec3 position, velocity; };
            std::vector<Predicted> history;
            struct Ack { uint64_t sequence; core::vec3 position, velocity; uint64_t waiting; };
            std::optional<Ack> ack;
            uint64_t reconciliations = 0;
            float    largestCorrection = 0.0f;
            float    clockScale = 1.0f;
        };

        ClientSession* clientOf(World& world) { return world.ctx().find<ClientSession>(); }
        bool world_valid_predicted(World& world, const ClientSession& c) {
            return c.predicted != NullEntity && world.valid(c.predicted) && world.all_of<CharacterController>(c.predicted);
        }

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
            // Its own player, with physics here to walk it: predicted. Without physics, a proxy.
            if (id == c.player.value && world.all_of<CharacterController>(e)) {
                if (hasPhysics(world)) {
                    c.predicted = e;
                    c.proxies[id] = e;
                    return;
                }
                world.remove<CharacterController>(e);
            }
            // A moving body is simulated here and corrected by snapshots; anything else is posed by them.
            const RigidBody* rigid = world.try_get<RigidBody>(e);
            if (!(rigid && rigid->motion == MotionType::Dynamic))
                world.emplace_or_replace<NetProxy>(e, NetProxy{{{c.latestTick, position, rotation}}});
            c.proxies[id] = e;
        }

        void clientSnapshot(World& world, ClientSession& c, net::BitReader& r) {
            const uint64_t tick = r.varint();
            if (r.boolean()) {
                ClientSession::Ack ack;
                ack.sequence = r.varint();
                ack.position = net::readPosition(r, core::vec3(0.0f), 0.0001f);
                ack.velocity = net::readVector(r, 64.0f, 20);
                ack.waiting = r.varint();
                if (r.failed) return;
                if (!c.ack || ack.sequence > c.ack->sequence) c.ack = ack;
            }
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
                const bool moving = b.boolean();
                const bool asleep = b.boolean();
                const core::vec3 position = net::readPosition(b, core::vec3(0.0f), 0.001f);
                const core::quat rotation = net::readRotation(b, 10);
                core::vec3 linear(0.0f), angular(0.0f);
                if (moving) {
                    linear = net::readVector(b, 64.0f, 12);
                    angular = net::readVector(b, 32.0f, 12);
                }
                if (b.failed) return;
                if (!std::isfinite(position.x + position.y + position.z)) continue;
                Entity e = entityForNetId(world, NetId{id});
                if (e == NullEntity) continue;
                if (NetProxy* proxy = world.try_get<NetProxy>(e)) {
                    if (!proxy->poses.empty() && proxy->poses.back().tick >= tick) continue;
                    proxy->poses.push_back({tick, position, rotation});
                    if (proxy->poses.size() > MAX_POSES) proxy->poses.erase(proxy->poses.begin());
                    continue;
                }
                // A body this client simulates: corrected to the server's, newest only.
                if (!moving || !hasBody(world, e)) continue;
                uint64_t& last = c.corrected[id];
                if (tick <= last) continue;
                last = tick;
                setBodyState(world, e, position, rotation, linear, angular, asleep);
                c.corrections++;
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
                    w.boolean(hasPhysics(world));   // it predicts its own player: the ack is enough of it
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
                        if (r.failed) break;
                        // In order with the world's changes: a piece goes after the split that made it.
                        ClientSession::Change change{Message::Despawn, NetId{id}};
                        c.changes.push_back(std::move(change));
                        break;
                    }
                    case Message::Split: {
                        ClientSession::Change change{Message::Split};
                        if (readSplit(r, change.target, change.split)) c.changes.push_back(std::move(change));
                        else core::warn("net: a malformed split from the server was dropped");
                        break;
                    }
                    case Message::Edit: {
                        ClientSession::Change change{Message::Edit};
                        if (readEdit(r, change.target, change.ops)) c.changes.push_back(std::move(change));
                        else core::warn("net: a malformed edit from the server was dropped");
                        break;
                    }
                    case Message::Check: {
                        ClientSession::Change change{Message::Check};
                        const uint64_t count = r.varint();
                        for (uint64_t i = 0; i < count && !r.failed && i < 4096; i++) {
                            uint32_t id = uint32_t(r.varint());
                            change.hashes.push_back({NetId{id}, r.bits(64)});
                        }
                        if (!r.failed) c.changes.push_back(std::move(change));
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

        // The server's world changes, in order, before the physics step that applies them. A change
        // naming what does not exist yet -- a piece an earlier split makes this step -- waits; a check
        // waits until what came before it has been applied.
        void clientApplyWorld(Application& app) {
            World& world = app.world;
            ClientSession& c = *clientOf(world);
            bool dispatched = false;
            size_t done = 0;
            for (; done < c.changes.size(); done++) {
                ClientSession::Change& change = c.changes[done];
                if (change.kind == Message::Check) {
                    if (dispatched) break;   // the physics applies those at this step; check at the next
                    for (auto [id, hash] : change.hashes) {
                        Entity e = entityForNetId(world, id);
                        if (e == NullEntity) continue;
                        if (contentHashOf(world, e) != hash) {
                            c.mismatches++;
                            core::warn("net: entity {}'s voxels are not the server's after its changes", id.value);
                        }
                    }
                    continue;
                }
                Entity e = entityForNetId(world, change.target);
                if (change.kind == Message::Despawn) {
                    if (e == NullEntity && change.waited++ < 240) break;
                    if (e != NullEntity) world.destroy(e);
                    c.proxies.erase(change.target.value);
                    continue;
                }
                const bool ready = e != NullEntity && hasBody(world, e);
                if (!ready) {
                    if (change.waited++ < 240) break;
                    core::warn("net: a change to entity {} was dropped: it is not here", change.target.value);
                    continue;
                }
                if (change.kind == Message::Split) applyVoxelSplit(world, e, std::move(change.split));
                else applyVoxelEdit(world, e, std::move(change.ops));
                c.applied++;
                dispatched = true;
            }
            c.changes.erase(c.changes.begin(), c.changes.begin() + long(done));
        }

        // The newest ack against what was predicted for that input: if they differ, back to the
        // server's, and every input since run again, the character alone.
        void reconcile(World& world, ClientSession& c, float dt) {
            if (!c.ack) return;
            const ClientSession::Ack ack = *c.ack;
            c.ack.reset();
            // Keep the server's queue about two inputs long: run a little faster or slower.
            c.clockScale = std::clamp(1.0f + 0.02f * (2.0f - float(ack.waiting)), 0.95f, 1.05f);
            const PhysicsCharacter* character = world.valid(c.predicted) ? world.try_get<PhysicsCharacter>(c.predicted) : nullptr;
            if (!character) return;
            auto at = std::find_if(c.history.begin(), c.history.end(), [&](const auto& p) { return p.sequence == ack.sequence; });
            if (at == c.history.end()) return;
            const float off = glm::distance(at->position, ack.position);
            const bool differs = off > 0.001f || glm::distance(at->velocity, ack.velocity) > 0.01f;
            if (differs) {
                PhysicsWorld& sim = physicsWorld(world);
                sim.restoreCharacter(character->id, at->state);
                sim.setCharacterPosition(character->id, ack.position);
                sim.setCharacterVelocity(character->id, ack.velocity);
                for (auto later = at + 1; later != c.history.end(); ++later) {
                    sim.setCharacterInput(character->id, later->move, later->jump);
                    sim.stepCharacterAlone(character->id, dt);
                    later->state = sim.saveCharacter(character->id);
                    later->position = sim.characterState(character->id).position;
                    later->velocity = sim.characterState(character->id).velocity;
                }
                PhysicsCharacter& shown = world.get<PhysicsCharacter>(c.predicted);
                const core::vec3 now = sim.characterState(character->id).position;
                shown.previousPosition += now - shown.position;
                shown.position = now;
                c.reconciliations++;
                c.largestCorrection = std::max(c.largestCorrection, off);
            }
            c.history.erase(c.history.begin(), at + 1);
        }

        void clientSendInput(Application& app) {
            ClientSession& c = *clientOf(app.world);
            if (!c.welcomed) return;
            reconcile(app.world, c, app.time().fixedDelta);
            app.time().scale = c.clockScale;
            if (world_valid_predicted(app.world, c)) setCharacterInput(app.world, c.predicted, c.move, c.jumpPending);
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

        // After the step: what this step's input left the predicted player as.
        void clientRecordPrediction(Application& app) {
            World& world = app.world;
            ClientSession& c = *clientOf(world);
            if (!c.welcomed || c.recent.empty()) return;
            const PhysicsCharacter* character = world.valid(c.predicted) ? world.try_get<PhysicsCharacter>(c.predicted) : nullptr;
            if (!character) return;
            const PhysicsWorld& sim = physicsWorld(world);
            const auto& input = c.recent.back();
            const CharacterState state = sim.characterState(character->id);
            c.history.push_back({input.sequence, input.move, input.jump, sim.saveCharacter(character->id), state.position, state.velocity});
            if (c.history.size() > 240) c.history.erase(c.history.begin());
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
        // World changes, as the physics makes them: in order, inside its step.
        app.events().on<BodyBroken>([&world](const BodyBroken& event) {
            ServerSession* s = serverOf(world);
            const NetId* id = world.valid(event.entity) ? world.try_get<NetId>(event.entity) : nullptr;
            if (!s || !id) return;
            VoxelSplit split = voxelSplitOf(world, event);
            std::vector<uint8_t> message = splitMessage(*id, split);
            sendAll(*s, net::Channel::Reliable, message);
            s->history.push_back(std::move(message));
            s->changed.push_back(event.entity);
            // The pieces are made on every client by the split, with these ids: on the clients
            // already, so not spawned, only posed -- and despawned when they go.
            for (Entity piece : event.pieces) {
                if (!world.valid(piece) || !world.all_of<NetId>(piece)) continue;
                s->sent[piece] = world.get<NetId>(piece);
                s->fromSplits.insert(piece);
                world.emplace_or_replace<Replicated>(piece);
            }
        });
        app.events().on<VoxelsEdited>([&world](const VoxelsEdited& event) {
            ServerSession* s = serverOf(world);
            const NetId* id = world.valid(event.entity) ? world.try_get<NetId>(event.entity) : nullptr;
            if (!s || !id) return;
            std::vector<uint8_t> message = editMessage(*id, event.ops);
            sendAll(*s, net::Channel::Reliable, message);
            s->history.push_back(std::move(message));
            s->changed.push_back(event.entity);
        });
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
        app.addSystem(Stage::FixedUpdate, "net: input", clientSendInput, {.before = "physics", .after = ""});
        app.addSystem(Stage::FixedUpdate, "net: predicted", clientRecordPrediction, {.before = "", .after = "physics"});
        app.addSystem(Stage::FixedUpdate, "net: world", clientApplyWorld, {.before = "physics", .after = ""});
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
            for (const auto& [peer, client] : s->clients) out.snapshotBytes = std::max(out.snapshotBytes, client.snapshotBytes);
        }
        if (const ClientSession* c = world.ctx().find<ClientSession>()) {
            out.connected = c->welcomed;
            out.you = c->you;
            out.player = c->player;
            out.serverTick = c->latestTick;
            out.snapshotsReceived = c->snapshots;
            out.refusal = c->refusal;
            out.worldChanges = c->applied;
            out.worldMismatches = c->mismatches;
            out.corrections = c->corrections;
            out.reconciliations = c->reconciliations;
            out.largestCorrection = c->largestCorrection;
            out.predicting = c->predicted != NullEntity;
        }
        return out;
    }
}
