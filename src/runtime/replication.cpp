#include "utils/blob_voxels.h"
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
#include "runtime/loading.h"
#include "runtime/physics.h"
#include "runtime/physics/physics_world.h"
#include "runtime/scene_bridge.h"
#include "utils/editing.h"
#include "utils/scene_loading.h"
#include "utils/scene_query.h"

namespace projv::runtime {
    namespace {
        enum class Message : uint8_t { Hello = 1, Welcome, Refuse, Spawn, Despawn, Snapshot, Input, Split, Edit, Check,
                                       BlockPresence, BlockContent, Game };

        constexpr size_t   MAX_TEXT = 1u << 16;         // a name, a reason, a prefab path
        constexpr size_t   MAX_COMPONENTS = 1u << 20;   // an entity's components, as JSON
        constexpr size_t   MAX_SNAPSHOT_BYTES = 1100;   // under a UDP packet, so ENet need not fragment
        constexpr uint32_t INPUT_REDUNDANCY = 4;        // inputs repeated in each message
        constexpr size_t   MAX_POSES = 64;              // a proxy's history
        constexpr uint64_t MAX_SPLIT_CELLS = 1u << 24;  // the box a split's labels cover
        constexpr uint32_t MAX_SPLIT_PIECES = 4096;
        constexpr size_t   MAX_EDIT_OPS = 1u << 20;

        std::vector<uint8_t> text(const std::string& s) { return {s.begin(), s.end()}; }
        std::string text(const std::vector<uint8_t>& b) { return {b.begin(), b.end()}; }

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

        // The chunk a block is in an eagerly loaded scene (the same component handles as a lazy open
        // of the same files), or INVALID_CHUNK_HANDLE.
        ChunkHandle chunkOfBlock(const Scene& scene, const utils::SceneBlockSource& b) {
            if (!utils::isComponentAlive(scene, b.component)) return INVALID_CHUNK_HANDLE;
            const ComponentRecord& rec = scene.components[b.component];
            if (rec.kind == ComponentKind::Chunk) return rec.chunkHandle < scene.chunks.size() ? rec.chunkHandle : INVALID_CHUNK_HANDLE;
            if (rec.kind != ComponentKind::Grid || rec.gridIndex < 0 || size_t(rec.gridIndex) >= scene.grids.size()) return INVALID_CHUNK_HANDLE;
            const SceneGrid& grid = scene.grids[size_t(rec.gridIndex)];
            core::ivec3 local = core::ivec3(b.entry.gridX, b.entry.gridY, b.entry.gridZ) - grid.originCellCoord;
            if (glm::any(glm::lessThan(local, core::ivec3(0))) || glm::any(glm::greaterThanEqual(local, grid.dims))) return INVALID_CHUNK_HANDLE;
            int32_t c = grid.cellToChunk[size_t(local.x + grid.dims.x * (local.y + grid.dims.y * local.z))];
            return c >= 0 ? ChunkHandle(c) : INVALID_CHUNK_HANDLE;
        }

        // The entity whose voxels a component is: linked to it or to one of its parents.
        Entity entityOverComponent(const World& world, const Scene& scene, ComponentHandle h) {
            for (; h != INVALID_COMPONENT_HANDLE && h < scene.components.size(); h = scene.components[h].parent)
                if (Entity e = entityFor(world, h); e != NullEntity) return e;
            return NullEntity;
        }

        void writePalette(net::BitWriter& w, const std::vector<Material>& palette) {
            w.varint(palette.size());
            for (const Material& m : palette) {
                w.bits(m.packedColor, 32); w.bits(m.packedEmission, 32); w.bits(m.packedSurface, 32); w.bits(m.packedExtra, 32);
                w.bytes(text(m.name));
                w.bytes(text(m.physics));
            }
        }
        std::vector<Material> readPalette(net::BitReader& r) {
            std::vector<Material> palette(std::min<uint64_t>(r.varint(), MAX_PALETTE_ENTRIES));
            for (Material& m : palette) {
                m.packedColor = uint32_t(r.bits(32)); m.packedEmission = uint32_t(r.bits(32));
                m.packedSurface = uint32_t(r.bits(32)); m.packedExtra = uint32_t(r.bits(32));
                m.name = text(r.bytes(256));
                m.physics = text(r.bytes(256));
            }
            return palette;
        }

        uint64_t contentHashOf(World& world, Entity e) {
            const Scene* scene = world.ctx().find<Scene>();
            ComponentHandle h = scene ? voxelComponentOf(world, e) : INVALID_COMPONENT_HANDLE;
            return h == INVALID_COMPONENT_HANDLE ? 0 : utils::voxelContentHash(*scene, h);
        }

        void send(Events* events, auto event) {
            if (events) events->send(std::move(event));
        }


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
                std::map<uint32_t, int>   toldAsleep; // bodies sent asleep, and how many times (three: one may be lost)
                uint64_t    snapshotBytes = 0;
                // What it has (N6): each block's presence as told, the content stamp of each block it
                // was sent the voxels of, and the replicated entities it has, by NetId, with what the
                // rule last said of each.
                std::vector<utils::Presence>       blocks;
                std::map<uint32_t, uint64_t>       contentSent;
                std::map<uint32_t, utils::Presence> has;
                bool        blocksDue = true;
            };
            std::map<net::PeerId, Client> clients;
            std::map<Entity, NetId>       sent;          // replicated entities clients have, and their ids
            std::vector<Entity>           spawning;      // Replicated added: sent at the next "net: send"
            std::vector<NetId>            despawning;
            std::vector<net::NetEvent>    early;         // gathered while flushing in "net: send"
            uint64_t                      lastSnapshot = 0;
            uint64_t                      refused = 0;
            std::set<Entity>              fromSplits;    // pieces: sent with their voxels to who missed the split
            std::vector<Entity>           changed;       // voxels changed this step: checked
            // The scene's blocks, numbered as every client numbers them (the files' order), their
            // bounds, and their content stamp at the start: a block whose stamp moved is sent whole.
            std::vector<utils::SceneBlockSource> blocks;
            std::vector<utils::WorldBox>         blockBounds;
            std::vector<uint64_t>                blockStamps;
            std::map<ComponentHandle, std::vector<uint32_t>> blocksUnder;   // every block under a component
            utils::LoadPolicy                    policy = utils::loadEverything();
            std::map<ClientId, utils::LoadPolicy> policies;
            uint64_t                             frames = 0;
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
                s->fromSplits.erase(e);
                s->sent.erase(it);
            }
        }

        // `owner`: for the client whose player it is, which predicts it, and so is sent all of it.
        // `voxels`: a piece, for a client that did not see the split that made it -- its voxels go too.
        std::vector<uint8_t> spawnMessage(World& world, Entity e, NetId id, bool owner = false, bool voxels = false) {
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
            const Scene* scene = world.ctx().find<Scene>();
            const ComponentHandle h = voxels && scene ? voxelComponentOf(world, e) : INVALID_COMPONENT_HANDLE;
            w.boolean(h != INVALID_COMPONENT_HANDLE);
            if (h != INVALID_COMPONENT_HANDLE) {
                const ComponentRecord& rec = scene->components[h];
                const Chunk* first = nullptr;
                if (rec.kind == ComponentKind::Chunk && rec.chunkHandle < scene->chunks.size()) first = &scene->chunks[rec.chunkHandle];
                else if (rec.kind == ComponentKind::Grid)
                    for (int32_t c : scene->grids[size_t(rec.gridIndex)].cellToChunk)
                        if (c >= 0) { first = &scene->chunks[size_t(c)]; break; }
                w.f32(first ? first->nativeScale / float(std::max(first->header.resolution, 1u)) : 0.25f);
                writePalette(w, rec.materialPalette);
                const auto all = utils::componentVoxels(*scene, h);
                w.varint(all.size());
                for (const auto& v : all) {
                    for (int i = 0; i < 3; i++) w.signedVarint(v.position[i]);
                    w.bits(v.slot, 8);
                }
            }
            return w.take();
        }

        // ---- Who has what (N6) ----

        core::vec3 viewerOf(World& world, const ServerSession::Client& client) {
            if (client.player == NullEntity || !world.valid(client.player)) return core::vec3(0.0f);
            return poseOf(world, client.player).first;
        }

        // An entity's presence for a client: a replicated one by the rule at where it is (its own
        // player always); an authored one by its blocks, as the client's world loading has it.
        bool relevant(World& world, ServerSession& s, ServerSession::Client& client, Entity e) {
            if (!world.valid(e)) return false;
            const NetId* id = world.try_get<NetId>(e);
            if (world.all_of<Replicated>(e)) return id && client.has.count(id->value);
            const VoxelComponent* link = world.try_get<VoxelComponent>(e);
            if (!link) return true;
            auto under = s.blocksUnder.find(link->handle);
            if (under == s.blocksUnder.end() || client.blocks.empty()) return true;
            for (uint32_t b : under->second)
                if (client.blocks[b] != utils::Presence::Absent) return true;
            return false;
        }

        const utils::LoadPolicy& policyOf(const ServerSession& s, const ServerSession::Client& client) {
            auto it = s.policies.find(client.id);
            return it != s.policies.end() ? it->second : s.policy;
        }

        utils::Presence judge(const utils::LoadPolicy& policy, const utils::LoadCandidate& c, std::span<const utils::Viewer> viewers) {
            if (policy.hint.radius > 0.0f) {
                bool near = false;
                for (const utils::Viewer& v : viewers) near = near || utils::distanceToBox(c.bounds, v.position) <= policy.hint.radius;
                if (!near) return utils::Presence::Absent;
            }
            return policy.rule(c, viewers);
        }

        // The rule for each client: its blocks (every few frames, and at once when it joins), sending
        // the voxels of any the server has changed first; then its entities, spawned and despawned.
        void updateRelevance(World& world, ServerSession& s, net::PeerId peer, ServerSession::Client& client) {
            const utils::LoadPolicy& policy = policyOf(s, client);
            const utils::Viewer viewer{viewerOf(world, client), client.id};
            const std::span<const utils::Viewer> viewers(&viewer, client.player != NullEntity ? 1 : 0);
            const Scene& scene = world.ctx().get<Scene>();
            if (client.blocksDue || s.frames % 10 == 0) {
                client.blocksDue = false;
                if (client.blocks.size() != s.blocks.size()) client.blocks.assign(s.blocks.size(), utils::Presence::Absent);
                std::vector<std::pair<uint32_t, utils::Presence>> changes;
                for (uint32_t b = 0; b < s.blocks.size(); b++) {
                    utils::LoadCandidate c;
                    c.id = b;
                    c.bounds = s.blockBounds[b];
                    c.component = s.blocks[b].component;
                    c.cell = core::ivec3(s.blocks[b].entry.gridX, s.blocks[b].entry.gridY, s.blocks[b].entry.gridZ);
                    c.current = client.blocks[b];
                    const utils::Presence p = judge(policy, c, viewers);
                    if (p == client.blocks[b]) continue;
                    if (p != utils::Presence::Absent) {
                        // Changed here since the files: its voxels as they are now, before it loads.
                        const ChunkHandle ch = chunkOfBlock(scene, s.blocks[b]);
                        const int32_t pool = ch != INVALID_CHUNK_HANDLE ? scene.chunks[ch].geometryPoolIndex : -1;
                        const uint64_t stamp = pool >= 0 ? scene.geometryPool[size_t(pool)].contentStamp : 0;
                        if (pool >= 0 && stamp != s.blockStamps[b] && client.contentSent[b] != stamp) {
                            const GeometryBlob& blob = scene.geometryPool[size_t(pool)];
                            net::BitWriter w;
                            w.varint(uint8_t(Message::BlockContent));
                            w.varint(b);
                            // On the wire as its tile table: the words and the material bytes. The peer knows
                            // the block's resolution, which says where the leaves start.
                            const utils::TileTable& tiles = utils::blobTiles(blob);
                            w.varint(tiles.texels.size());
                            for (uint32_t word : tiles.texels) w.bits(word, 32);
                            w.bytes(tiles.materials);
                            s.transport->send(peer, net::Channel::Reliable, w.data());
                            client.contentSent[b] = stamp;
                        }
                    }
                    client.blocks[b] = p;
                    changes.push_back({b, p});
                }
                for (size_t at = 0; at < changes.size(); at += 4096) {
                    net::BitWriter w;
                    w.varint(uint8_t(Message::BlockPresence));
                    const size_t n = std::min<size_t>(4096, changes.size() - at);
                    w.varint(n);
                    for (size_t i = at; i < at + n; i++) {
                        w.varint(changes[i].first);
                        w.bits(uint8_t(changes[i].second), 2);
                    }
                    s.transport->send(peer, net::Channel::Reliable, w.data());
                }
            }
            // Replicated entities, in NetId order.
            std::vector<std::pair<NetId, Entity>> all;
            for (auto [e, id] : s.sent) if (world.valid(e)) all.push_back({id, e});
            std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first.value < b.first.value; });
            for (auto [id, e] : all) {
                auto known = client.has.find(id.value);
                utils::Presence p = utils::Presence::Active;
                if (e != client.player) {
                    const core::vec3 at = poseOf(world, e).first;
                    utils::LoadCandidate c;
                    c.id = std::numeric_limits<uint32_t>::max();
                    c.bounds = {at - core::vec3(0.5f), at + core::vec3(0.5f)};
                    c.current = known != client.has.end() ? known->second : utils::Presence::Absent;
                    c.entity = static_cast<uint32_t>(e);
                    p = judge(policy, c, viewers);
                }
                if (p != utils::Presence::Absent && known == client.has.end()) {
                    s.transport->send(peer, net::Channel::Reliable,
                                      spawnMessage(world, e, id, e == client.player, s.fromSplits.count(e) != 0));
                    client.has[id.value] = p;
                } else if (p == utils::Presence::Absent && known != client.has.end()) {
                    net::BitWriter w;
                    w.varint(uint8_t(Message::Despawn));
                    w.varint(id.value);
                    s.transport->send(peer, net::Channel::Reliable, w.data());
                    client.has.erase(known);
                } else if (known != client.has.end()) {
                    known->second = p;
                }
            }
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
            // Its blocks and entities follow at the next send, as the rule has them.
            client.blocksDue = true;
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
                        else if (kind == Message::Game && it->second.welcomed && s.config.onMessage) {
                            const std::vector<uint8_t> body = r.bytes(MAX_TEXT);
                            if (!r.failed) s.config.onMessage(world, it->second.id, it->second.player, body);
                        }
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
            for (auto [id, e] : fresh) s.sent[e] = id;
            // Gone: to each client that had it.
            for (NetId id : s.despawning) {
                net::BitWriter w;
                w.varint(uint8_t(Message::Despawn));
                w.varint(id.value);
                for (auto& [peer, client] : s.clients)
                    if (client.welcomed && client.has.erase(id.value)) s.transport->send(peer, net::Channel::Reliable, w.data());
            }
            s.despawning.clear();
            // Who has what now.
            s.frames++;
            for (auto& [peer, client] : s.clients)
                if (client.welcomed) updateRelevance(world, s, peer, client);

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
                for (auto& [peer, client] : s.clients) {
                    if (!client.welcomed) continue;
                    std::vector<std::pair<NetId, uint64_t>> mine;
                    for (auto [id, hash] : hashes)
                        if (relevant(world, s, client, entityForNetId(world, id))) mine.push_back({id, hash});
                    if (mine.empty()) continue;
                    net::BitWriter w;
                    w.varint(uint8_t(Message::Check));
                    w.varint(mine.size());
                    for (auto [id, hash] : mine) {
                        w.varint(id.value);
                        w.bits(hash, 64);
                    }
                    s.transport->send(peer, net::Channel::Reliable, w.data());
                }
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
                    w.align();   // whole bytes: copied into every client's packet at once
                    entries.push_back({id, weight, moving && !state.awake, w.data(), w.bitCount()});
                }
                const size_t budget = std::max<size_t>(64, size_t(float(s.config.bytesPerSecond) / std::max(s.config.snapshotRate, 0.1f)));
                for (auto& [peer, client] : s.clients) {
                    if (!client.welcomed) continue;
                    // Gather priority; a body asleep and told so already gathers none.
                    std::vector<std::pair<float, const Entry*>> order;
                    const NetId* own = client.predicts && world.valid(client.player) ? world.try_get<NetId>(client.player) : nullptr;
                    for (const Entry& entry : entries) {
                        if (auto told = client.toldAsleep.find(entry.id); entry.asleep && told != client.toldAsleep.end() && told->second >= 3)
                            continue;
                        if (own && own->value == entry.id) continue;
                        if (!relevant(world, s, client, candidates[entry.id])) continue;
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
                            body.raw(entry.bits);
                            count++;
                            client.priority[entry.id] = 0.0f;
                            if (entry.asleep) client.toldAsleep[entry.id]++;
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
                uint32_t block = 0;                                // BlockContent
                utils::TileTable tiles;                            // its voxels, as they came off the wire
                int     waited = 0;
            };
            std::vector<Change> changes;
            uint64_t applied = 0, mismatches = 0;
            // Prediction: the own player, walked here; what each input left it as; the newest ack.
            Entity   predicted = NullEntity;
            struct Predicted { uint64_t sequence; core::vec3 move; bool jump; std::vector<uint8_t> state; core::vec3 position, velocity; };
            std::vector<Predicted> history;
            struct Ack { uint64_t sequence; core::vec3 position, velocity; uint64_t waiting; };
            std::optional<Ack> ack;
            uint64_t reconciliations = 0;
            float    largestCorrection = 0.0f;
            float    clockScale = 1.0f;
            // What the server says of each block (N6), read by the world-loading rule when there is
            // one; and the blocks as numbered, for a scene loaded whole.
            std::shared_ptr<std::vector<utils::Presence>> presence = std::make_shared<std::vector<utils::Presence>>();
            std::vector<utils::SceneBlockSource> blocks;
            bool lazy = false;
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
            // A piece's voxels, for one this client did not see made.
            struct Voxels { float voxel = 0.25f; std::vector<Material> palette; std::vector<PendingVoxelOp> ops; };
            std::optional<Voxels> voxels;
            if (r.boolean()) {
                voxels.emplace();
                voxels->voxel = r.f32();
                voxels->palette = readPalette(r);
                const uint64_t count = r.varint();
                if (r.failed || count > MAX_EDIT_OPS || count > r.bitsLeft()) return;
                voxels->ops.resize(size_t(count));
                for (PendingVoxelOp& op : voxels->ops) {
                    op.isAdd = true;
                    for (int i = 0; i < 3; i++) op.position[i] = int(std::clamp<int64_t>(r.signedVarint(), -(1 << 20), 1 << 20));
                    op.slot = uint8_t(r.bits(8));
                    op.packedColor = op.slot < voxels->palette.size() ? voxels->palette[op.slot].packedColor : 0u;
                }
            }
            if (r.failed || id == 0) return;
            if (c.proxies.count(id) || entityForNetId(world, NetId{id}) != NullEntity) return;
            Entity e = NullEntity;
            if (voxels && voxels->voxel > 0.0f && std::isfinite(voxels->voxel) && !voxels->ops.empty()) {
                Scene& scene = world.ctx().get<Scene>();
                ComponentHandle piece = utils::addComponent(scene, ComponentKind::Chunk, "piece", INVALID_COMPONENT_HANDLE, 16, voxels->voxel);
                if (piece != INVALID_COMPONENT_HANDLE) {
                    scene.components[piece].materialPalette = voxels->palette;
                    scene.components[piece].paletteVersion++;
                    utils::queueVoxelAdd(scene, piece, voxels->ops);
                    utils::updateScene(scene);
                    utils::setComponentTransform(scene, piece, position, rotation, std::isfinite(scale) ? scale : 1.0f);
                    e = spawnComponent(world, piece, LinkMode::Root, OnUnlink::Destroy);
                    if (e != NullEntity) world.emplace_or_replace<Piece>(e);
                }
            } else if (!prefab.empty()) {
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
            // Everything else the server sends is the server's to move: a proxy, shown where it says.
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
                (void)b.boolean();   // asleep: a proxy simply stays at its last pose
                const core::vec3 position = net::readPosition(b, core::vec3(0.0f), 0.001f);
                const core::quat rotation = net::readRotation(b, 10);
                core::vec3 linear(0.0f), angular(0.0f);
                if (moving) {
                    linear = net::readVector(b, 64.0f, 12);
                    angular = net::readVector(b, 32.0f, 12);
                }
                b.align();
                if (b.failed) return;
                if (!std::isfinite(position.x + position.y + position.z)) continue;
                Entity e = entityForNetId(world, NetId{id});
                if (e == NullEntity || e == c.predicted) continue;
                // Anything the server sends is the server's to move: shown where it says.
                NetProxy& proxy = world.get_or_emplace<NetProxy>(e);
                if (!proxy.poses.empty() && proxy.poses.back().tick >= tick) continue;
                proxy.poses.push_back({tick, position, rotation, linear, angular});
                if (proxy.poses.size() > MAX_POSES) proxy.poses.erase(proxy.poses.begin());
            }
        }

        // Where a proxy is at server tick `at`: between the two snapshots around it; before the first,
        // the first; past the newest, carried on by its velocity for at most a quarter second.
        std::pair<core::vec3, core::quat> proxyPoseAt(const NetProxy& proxy, double at, float tickDelta) {
            const auto& poses = proxy.poses;
            if (at <= double(poses.front().tick)) return {poses.front().position, poses.front().rotation};
            for (size_t i = 1; i < poses.size(); i++) {
                if (double(poses[i].tick) < at) continue;
                const auto& a = poses[i - 1];
                const auto& b = poses[i];
                const float t = float((at - double(a.tick)) / double(std::max<uint64_t>(b.tick - a.tick, 1)));
                return {glm::mix(a.position, b.position, t), glm::slerp(a.rotation, b.rotation, t)};
            }
            const auto& last = poses.back();
            const float ahead = std::min(float(at - double(last.tick)) * tickDelta, 0.25f);
            core::quat rotation = last.rotation;
            if (const float turn = glm::length(last.angular) * ahead; turn > 1e-6f)
                rotation = glm::normalize(glm::angleAxis(turn, glm::normalize(last.angular)) * rotation);
            return {last.position + last.linear * ahead, rotation};
        }

        // The server tick shown now: the newest snapshot's, moved on by the time since, less the delay.
        double shownTick(const ClientSession& c) {
            const double estimate = double(c.latestTick) + (c.clock - c.latestAt) / double(c.tickDelta);
            return estimate - double(c.config.interpolationDelay) / double(c.tickDelta);
        }

        // Each fixed step, before the physics: every moving body this client does not own is held and
        // driven to where the server had it -- the client's own character meets it there, solid. A
        // body that has just appeared (a piece a split made, a crate from the files) is held where it is
        // until its snapshots come.
        void clientDrive(Application& app) {
            World& world = app.world;
            ClientSession& c = *clientOf(world);
            if (!c.welcomed) return;
            for (auto [e, body, id] : world.view<PhysicsBody, NetId>().each()) {
                if (body.motion != MotionType::Dynamic || e == c.predicted || world.all_of<NetProxy>(e)) continue;
                const BodyState now = bodyPose(world, e);
                world.emplace<NetProxy>(e, NetProxy{{{c.latestTick, now.position, now.rotation}}});
            }
            const double at = shownTick(c);
            for (auto [e, proxy, body] : world.view<NetProxy, PhysicsBody>().each()) {
                if (proxy.poses.empty()) continue;
                auto [position, rotation] = proxyPoseAt(proxy, at, c.tickDelta);
                driveBody(world, e, position, rotation);
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
                    case Message::BlockPresence: {
                        const uint64_t count = r.varint();
                        for (uint64_t i = 0; i < count && !r.failed && i < 4096; i++) {
                            const uint32_t block = uint32_t(r.varint());
                            const auto p = utils::Presence(std::min<uint64_t>(r.bits(2), 2));
                            if (r.failed || block >= (1u << 24)) break;
                            if (c.presence->size() <= block) c.presence->resize(size_t(block) + 1, utils::Presence::Absent);
                            (*c.presence)[block] = p;
                        }
                        // Asked again at once, under the table the server keeps.
                        if (c.lazy)
                            worldLoader(world).setRule(utils::LoadPolicy{
                                [table = c.presence](const utils::LoadCandidate& candidate, std::span<const utils::Viewer>) {
                                    if (candidate.entity != 0xFFFFFFFFu) return utils::Presence::Active;   // entities: the server sends them
                                    return candidate.id < table->size() ? (*table)[candidate.id] : utils::Presence::Absent;
                                }, {}});
                        break;
                    }
                    case Message::BlockContent: {
                        ClientSession::Change change{Message::BlockContent};
                        change.block = uint32_t(r.varint());
                        const uint64_t words = r.varint();
                        if (r.failed || words > r.bitsLeft() / 32) break;
                        change.tiles.texels.resize(size_t(words));
                        for (uint32_t& word : change.tiles.texels) word = uint32_t(r.bits(32));
                        change.tiles.materials = r.bytes(size_t(1) << 26);
                        if (!r.failed) c.changes.push_back(std::move(change));
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
                    case Message::Game: {
                        const std::vector<uint8_t> body = r.bytes(MAX_TEXT);
                        if (c.welcomed && !r.failed && c.config.onMessage) c.config.onMessage(world, body);
                        break;
                    }
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
                if (change.kind == Message::BlockContent) {
                    // A block as the server has it now: replaces the files' voxels, here or when it
                    // loads, and is taken by the physics without an island pass of its own.
                    Scene& scene = world.ctx().get<Scene>();
                    ComponentHandle component = INVALID_COMPONENT_HANDLE;
                    // The table as it came off the wire, at the block's own resolution -- checked before
                    // anything reads it, and dropped if its offsets leave it.
                    auto received = [&](uint32_t resolution) {
                        utils::TileTable t = std::move(change.tiles);
                        t.resolution = resolution;
                        t.leafStart = t.texels.empty() ? 0u : uint32_t(utils::tileTableLayout(t).leafStart);   // from its header; checked below
                        if (!utils::tileTableWellFormed(t)) {
                            core::warn("replication: block {}'s content is malformed; it is dropped", change.block);
                            return std::optional<utils::TileTable>();
                        }
                        utils::tileTableRecount(t);
                        return std::optional<utils::TileTable>(std::move(t));
                    };
                    if (c.lazy && change.block < worldLoader(world).blocks().size()) {
                        utils::SceneBlocks& blocks = worldLoader(world).blocks();
                        component = blocks.source(change.block).component;
                        if (auto t = received(blocks.source(change.block).resolution)) blocks.replaceContent(scene, change.block, std::move(*t));
                        else component = INVALID_COMPONENT_HANDLE;
                    } else if (!c.lazy && change.block < c.blocks.size()) {
                        component = c.blocks[change.block].component;
                        const ChunkHandle ch = chunkOfBlock(scene, c.blocks[change.block]);
                        std::optional<utils::TileTable> t;
                        if (ch != INVALID_CHUNK_HANDLE) t = received(scene.chunks[ch].header.resolution);
                        if (t) utils::replaceChunkTiles(scene, ch, std::move(*t));
                        else component = INVALID_COMPONENT_HANDLE;
                    }
                    if (component != INVALID_COMPONENT_HANDLE)
                        if (Entity owner = entityOverComponent(world, scene, component); owner != NullEntity) acceptVoxelChanges(world, owner);
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
                const double at = shownTick(c);
                // Proxies without a body (players, here): posed. Those with one are driven at fixed
                // steps, and the physics draws them.
                for (auto [e, proxy] : world.view<NetProxy>(entt::exclude<PhysicsBody>).each()) {
                    if (proxy.poses.empty()) continue;
                    auto [position, rotation] = proxyPoseAt(proxy, at, c.tickDelta);
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
        if (s.config.loadRule.rule) s.policy = s.config.loadRule;
        // The blocks as a client opening the folder lazily numbers them, and where they are.
        {
            Scene scratch;
            utils::SceneBlocks index;
            if (index.open(scratch, s.config.sceneFolder)) {
                const Scene* scene = world.ctx().find<Scene>();
                for (utils::BlockId b = 0; b < index.size(); b++) {
                    s.blocks.push_back(index.source(b));
                    s.blockBounds.push_back(index.bounds(scratch, b));
                    const ChunkHandle ch = scene ? chunkOfBlock(*scene, index.source(b)) : INVALID_CHUNK_HANDLE;
                    const int32_t pool = ch != INVALID_CHUNK_HANDLE ? scene->chunks[ch].geometryPoolIndex : -1;
                    s.blockStamps.push_back(pool >= 0 ? scene->geometryPool[size_t(pool)].contentStamp : 0);
                    for (ComponentHandle h = index.source(b).component; h != INVALID_COMPONENT_HANDLE && h < scratch.components.size();
                         h = scratch.components[h].parent)
                        s.blocksUnder[h].push_back(b);
                }
            }
        }
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
            s->changed.push_back(event.entity);
            // To who has what broke: they make the pieces themselves, with these ids, so they have
            // them already. Anyone else is sent a piece, voxels and all, when it comes near them.
            std::vector<ServerSession::Client*> told;
            for (auto& [peer, client] : s->clients)
                if (client.welcomed && relevant(world, *s, client, event.entity)) {
                    s->transport->send(peer, net::Channel::Reliable, message);
                    told.push_back(&client);
                }
            for (Entity piece : event.pieces) {
                if (!world.valid(piece) || !world.all_of<NetId>(piece)) continue;
                const NetId pieceId = world.get<NetId>(piece);
                s->sent[piece] = pieceId;
                s->fromSplits.insert(piece);
                world.emplace_or_replace<Replicated>(piece);
                for (ServerSession::Client* client : told) client->has[pieceId.value] = utils::Presence::Active;
            }
        });
        app.events().on<VoxelsEdited>([&world](const VoxelsEdited& event) {
            ServerSession* s = serverOf(world);
            const NetId* id = world.valid(event.entity) ? world.try_get<NetId>(event.entity) : nullptr;
            if (!s || !id) return;
            std::vector<uint8_t> message = editMessage(*id, event.ops);
            for (auto& [peer, client] : s->clients)
                if (client.welcomed && relevant(world, *s, client, event.entity)) s->transport->send(peer, net::Channel::Reliable, message);
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
        // A world opened lazily loads what the server says (nothing until it does); a scene loaded
        // whole has everything, and only needs the blocks' numbering for their contents.
        c.lazy = hasWorldLoading(world);
        if (c.lazy) {
            worldLoader(world).setRule(utils::LoadPolicy{
                [](const utils::LoadCandidate& candidate, std::span<const utils::Viewer>) {
                    return candidate.entity != 0xFFFFFFFFu ? utils::Presence::Active : utils::Presence::Absent;
                }, {}});
        } else {
            Scene scratch;
            utils::SceneBlocks index;
            if (index.open(scratch, c.config.sceneFolder))
                for (utils::BlockId b = 0; b < index.size(); b++) c.blocks.push_back(index.source(b));
        }
        c.server = c.transport->connect(c.config.host, c.config.port);
        if (c.server == net::NO_PEER) {
            core::error("net: could not start connecting to {}:{}", c.config.host, c.config.port);
            return false;
        }
        app.addSystem(Stage::PreUpdate, "net: receive", clientReceive);
        app.addSystem(Stage::FixedUpdate, "net: input", clientSendInput, {.before = "physics", .after = ""});
        app.addSystem(Stage::FixedUpdate, "net: predicted", clientRecordPrediction, {.before = "", .after = "physics"});
        app.addSystem(Stage::FixedUpdate, "net: world", clientApplyWorld, {.before = "physics", .after = ""});
        app.addSystem(Stage::FixedUpdate, "net: drive", clientDrive, {.before = "physics", .after = "net: world"});
        app.addSystem(Stage::PostUpdate, "net: present", clientPresent, {.before = "scene bridge", .after = ""});
        return true;
    }

    void setClientLoadRule(World& world, utils::LoadPolicy policy) {
        if (ServerSession* s = serverOf(world)) {
            s->policy = policy.rule ? std::move(policy) : utils::loadEverything();
            for (auto& [peer, client] : s->clients) client.blocksDue = true;
        }
    }

    void setClientLoadRule(World& world, ClientId client, utils::LoadPolicy policy) {
        if (ServerSession* s = serverOf(world)) {
            if (policy.rule) s->policies[client] = std::move(policy);
            else s->policies.erase(client);
            for (auto& [peer, c] : s->clients)
                if (c.id == client) c.blocksDue = true;
        }
    }

    void sendToServer(World& world, std::span<const uint8_t> message, bool reliable) {
        ClientSession* c = clientOf(world);
        if (!c || !c->welcomed || message.size() > MAX_TEXT) return;
        net::BitWriter w;
        w.varint(uint8_t(Message::Game));
        w.bytes(message);
        c->transport->send(c->server, reliable ? net::Channel::Reliable : net::Channel::Unreliable, w.data());
    }

    void sendToClient(World& world, ClientId client, std::span<const uint8_t> message, bool reliable) {
        ServerSession* s = serverOf(world);
        if (!s || message.size() > MAX_TEXT) return;
        net::BitWriter w;
        w.varint(uint8_t(Message::Game));
        w.bytes(message);
        for (const auto& [peer, c] : s->clients)
            if (c.welcomed && c.id == client)
                s->transport->send(peer, reliable ? net::Channel::Reliable : net::Channel::Unreliable, w.data());
    }

    void sendToClients(World& world, std::span<const uint8_t> message, bool reliable) {
        ServerSession* s = serverOf(world);
        if (!s || message.size() > MAX_TEXT) return;
        net::BitWriter w;
        w.varint(uint8_t(Message::Game));
        w.bytes(message);
        for (const auto& [peer, c] : s->clients)
            if (c.welcomed) s->transport->send(peer, reliable ? net::Channel::Reliable : net::Channel::Unreliable, w.data());
    }

    void setLocalInput(World& world, core::vec3 move, bool jump) {
        if (ClientSession* c = clientOf(world)) {
            c->move = move;
            c->jumpPending = c->jumpPending || jump;
        }
    }

    std::vector<ClientStats> clientStats(const World& world) {
        std::vector<ClientStats> out;
        const ServerSession* s = world.ctx().find<ServerSession>();
        if (!s) return out;
        for (const auto& [peer, client] : s->clients) {
            if (!client.welcomed) continue;
            ClientStats c;
            c.id = client.id;
            c.name = client.name;
            const net::LinkStats link = s->transport->stats(peer);
            c.roundTripMs = link.roundTripMs;
            c.loss = link.loss;
            c.bytesSent = link.bytesSent;
            c.bytesReceived = link.bytesReceived;
            c.snapshotBytes = client.snapshotBytes;
            c.inputsWaiting = client.inputs.size();
            c.entities = client.has.size();
            for (utils::Presence p : client.blocks) c.blocks += p != utils::Presence::Absent;
            out.push_back(std::move(c));
        }
        std::sort(out.begin(), out.end(), [](const ClientStats& a, const ClientStats& b) { return a.id < b.id; });
        return out;
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
            out.reconciliations = c->reconciliations;
            out.largestCorrection = c->largestCorrection;
            out.predicting = c->predicted != NullEntity;
        }
        return out;
    }
}
