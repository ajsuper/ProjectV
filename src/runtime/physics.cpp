#include "runtime/physics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <map>
#include <cstring>
#include <tuple>

#include <glm/gtc/quaternion.hpp>

#include "core/log.h"
#include "utils/loose_bvh.h"
#include "utils/scene_query.h"
#include "utils/picking.h"
#include "utils/editing.h"
#include "utils/voxel_islands.h"

namespace projv::runtime {
    namespace {
        // ---- The physics resource, in world.ctx() ---------------------------------------------

        // A command as queued: the public one, plus the local entity when it was issued here (an
        // entity without a NetId can still be pushed).
        struct Command {
            PhysicsCommand c;
            Entity         entity = NullEntity;
        };

        struct PhysicsRuntime {
            std::unique_ptr<PhysicsWorld> world;
            utils::CollisionParams        collision;
            bool                          destroyLeavers = true;
            uint64_t                      seenDroppedSteps = 0;
            uint64_t                      lastSlowWarningTick = 0;
            std::vector<Command>          commands;     // waiting; applied by stamp, not arrival
            uint32_t                      sequence = 0;
            uint64_t                      late = 0;
            std::vector<BodyId>           doomed;       // bodies whose entity or component went
            std::vector<Entity>           pending;      // want a body made (or remade)
            std::vector<Entity>           rebuild;      // want their body destroyed, then remade
            bool                          writingTransforms = false;
            std::set<Entity>              warnedAboutTransform;
            uint64_t                      seenStamp = 0;    // latestBlobContentStamp at the last check
            // Motion a remade body keeps (a shape rebuilt under it, a piece cut from it).
            struct Motion { core::vec3 linear, angular; };
            std::map<Entity, Motion>      carried;
            std::set<Entity>              reshaped;     // rebuilt this step because their voxels changed
            // The newest blob content stamp when each entity asked for a body: voxels stamped after
            // it were edited since, so an empty result is an outcome (BodyEmptied), not a mistake.
            std::map<Entity, uint64_t>    requestedAt;
            std::map<uint32_t, Entity>    byBody;       // body id -> entity, for queries
            std::vector<Entity>           pendingCharacters;
            std::vector<Entity>           pendingJoints;
            std::vector<CharacterId>      doomedCharacters;
            std::vector<JointId>          doomedJoints;
            struct Edit { Entity entity; std::vector<PendingVoxelOp> ops; };
            std::vector<Edit>             edits;
        };

        PhysicsRuntime& runtimeOf(World& world) { return world.ctx().get<PhysicsRuntime>(); }

        void send(World& world, auto event) {
            if (Events* events = world.ctx().find<Events>()) events->send(std::move(event));
        }

        // ---- Signals: noted here, acted on at the next tick ----------------------------------------

        void wantBody(World& world, Entity entity) {
            PhysicsRuntime& physics = runtimeOf(world);
            physics.pending.push_back(entity);
            physics.requestedAt[entity] = latestBlobContentStamp();
        }
        void wantRebuild(World& world, Entity entity) { runtimeOf(world).rebuild.push_back(entity); }

        void bodyGone(World& world, Entity entity) {
            PhysicsRuntime& physics = runtimeOf(world);
            if (const PhysicsBody* body = world.try_get<PhysicsBody>(entity)) physics.doomed.push_back(body->id);
            physics.requestedAt.erase(entity);
        }

        void wantCharacter(World& world, Entity entity) { runtimeOf(world).pendingCharacters.push_back(entity); }
        void wantJoint(World& world, Entity entity) { runtimeOf(world).pendingJoints.push_back(entity); }
        void jointGone(World& world, Entity entity) {
            if (const PhysicsJoint* j = world.try_get<PhysicsJoint>(entity)) runtimeOf(world).doomedJoints.push_back(j->id);
        }
        void characterGone(World& world, Entity entity) {
            if (const PhysicsCharacter* c = world.try_get<PhysicsCharacter>(entity)) {
                PhysicsRuntime& physics = runtimeOf(world);
                physics.doomedCharacters.push_back(c->id);
                physics.byBody.erase(physics.world->characterBody(c->id).value);
            }
        }

        void bodyComponentGone(World& world, Entity entity) {
            PhysicsRuntime& physics = runtimeOf(world);
            const PhysicsBody& body = world.get<PhysicsBody>(entity);
            physics.doomed.push_back(body.id);
            physics.byBody.erase(body.id.value);
        }

        // A dynamic body's Transform is the simulation's to write. A change from anywhere else is a
        // second writer, which is how bodies end up fighting their own presentation: report it.
        void transformChanged(World& world, Entity entity) {
            PhysicsRuntime& physics = runtimeOf(world);
            if (physics.writingTransforms) return;
            const PhysicsBody* body = world.try_get<PhysicsBody>(entity);
            if (!body || body->motion != MotionType::Dynamic) return;
            if (!physics.warnedAboutTransform.insert(entity).second) return;
            core::warn("physics: entity {}'s Transform was changed, but it is a dynamic body: the simulation "
                       "moves it, and will put it back. Use runtime::teleport to place it.",
                       static_cast<uint32_t>(entity));
        }

        // ---- Building a body from an entity ---------------------------------------------------------

        struct Box3 {
            core::vec3 min{std::numeric_limits<float>::max()}, max{std::numeric_limits<float>::lowest()};
            bool empty() const { return min.x > max.x; }
            void add(core::vec3 p) { min = glm::min(min, p); max = glm::max(max, p); }
        };

        // Every chunk under `h`, in a fixed order: the component's own chunk, a grid's cells by cell
        // index, then children in order.
        void collectChunks(const Scene& scene, ComponentHandle h, std::vector<ChunkHandle>& out) {
            const ComponentRecord& record = scene.components[h];
            if (record.kind == ComponentKind::Chunk) {
                out.push_back(record.chunkHandle);
            } else if (record.kind == ComponentKind::Grid && record.gridIndex >= 0 &&
                       size_t(record.gridIndex) < scene.grids.size()) {
                for (int32_t cell : scene.grids[size_t(record.gridIndex)].cellToChunk)
                    if (cell >= 0) out.push_back(ChunkHandle(cell));
            }
            for (ComponentHandle child : record.children)
                if (child < scene.components.size()) collectChunks(scene, child, out);
        }

        // The voxels under a component, as shape parts placed in the frame of the body at
        // (position, rotation): each chunk's shared voxel shape at its voxel size, where its header
        // puts it. Also the voxels' bounds in that frame, for fitting primitives.
        using Sources = std::vector<std::pair<ChunkHandle, uint64_t>>;

        // What a body made from `h` now would be built from: each live chunk with geometry, and its
        // blob's content stamp.
        Sources sourcesOf(const Scene& scene, ComponentHandle h) {
            std::vector<ChunkHandle> chunks;
            collectChunks(scene, h, chunks);
            Sources out;
            for (ChunkHandle c : chunks) {
                if (c >= scene.chunks.size()) continue;
                const Chunk& chunk = scene.chunks[c];
                if (!chunk.alive || chunk.geometryPoolIndex < 0 || size_t(chunk.geometryPoolIndex) >= scene.geometryPool.size())
                    continue;
                out.push_back({c, scene.geometryPool[size_t(chunk.geometryPoolIndex)].contentStamp});
            }
            return out;
        }

        std::vector<PhysicsShapePart> voxelParts(PhysicsWorld& physics, const utils::CollisionParams& params,
                                                 const Scene& scene, ComponentHandle h, core::vec3 position,
                                                 core::quat rotation, Box3& bounds,
                                                 std::vector<PhysicsVoxelPart>& voxelOut) {
            std::vector<ChunkHandle> chunks;
            collectChunks(scene, h, chunks);
            const core::quat inverse = glm::conjugate(rotation);
            std::vector<PhysicsShapePart> parts;
            for (ChunkHandle c : chunks) {
                if (c >= scene.chunks.size()) continue;
                const Chunk& chunk = scene.chunks[c];
                if (!chunk.alive || chunk.geometryPoolIndex < 0 ||
                    size_t(chunk.geometryPoolIndex) >= scene.geometryPool.size() || chunk.header.resolution == 0)
                    continue;
                const GeometryBlob& blob = scene.geometryPool[size_t(chunk.geometryPoolIndex)];
                CollisionShapeRef shape = physics.voxelShape(blob, chunk.header.resolution, params);
                if (!shape) continue;   // an empty chunk: nothing there to collide with
                float voxelSize = chunk.header.scale / float(chunk.header.resolution);
                PhysicsShapePart part;
                part.shape = PhysicsShape::fromVoxels(shape, voxelSize);
                part.position = inverse * (chunk.header.position - position);
                part.rotation = glm::normalize(inverse * chunk.header.rotation);
                core::ivec3 low, high;
                if (utils::blobContentBounds(blob, low, high)) {
                    for (int corner = 0; corner < 8; corner++) {
                        core::vec3 local((corner & 1) ? high.x + 1 : low.x, (corner & 2) ? high.y + 1 : low.y,
                                         (corner & 4) ? high.z + 1 : low.z);
                        bounds.add(part.position + part.rotation * (local * voxelSize));
                    }
                }
                voxelOut.push_back({c, shape, part.position, part.rotation, voxelSize});
                parts.push_back(std::move(part));
            }
            return parts;
        }

        struct Description {
            BodyDesc   desc;
            const char* refusal = nullptr;
            bool        noVoxels = false;   // refused because there are no voxels (left) to build from
            std::vector<PhysicsVoxelPart> voxelParts;
            Sources    sources;
        };

        Description describe(World& world, PhysicsRuntime& physics, Entity entity) {
            Description out;
            const RigidBody* rigid = world.try_get<RigidBody>(entity);
            const StaticCollider* fixed = world.try_get<StaticCollider>(entity);
            if (rigid && fixed) {
                core::warn("physics: entity {} has both a RigidBody and a StaticCollider; the RigidBody wins",
                           static_cast<uint32_t>(entity));
            }
            const Scene* scene = world.ctx().find<Scene>();
            const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
            bool linked = link && scene && utils::isComponentAlive(*scene, link->ref());

            MotionType motion = rigid ? rigid->motion : MotionType::Static;
            if (linked && link->mode == LinkMode::Part && motion == MotionType::Dynamic) {
                out.refusal = "a dynamic body needs a Root link (it is placed in the world, not inside an asset); "
                              "a part can be kinematic";
                return out;
            }

            BodyDesc& d = out.desc;
            d.motion = motion;
            if (linked) {
                d.position = utils::getComponentWorldPosition(*scene, link->handle);
                d.rotation = utils::getComponentWorldRotation(*scene, link->handle);
            } else if (const Transform* t = world.try_get<Transform>(entity)) {
                d.position = t->position;
                d.rotation = t->rotation;
            }

            Box3 bounds;
            std::vector<PhysicsShapePart> parts;
            if (linked) {
                parts = voxelParts(*physics.world, physics.collision, *scene, link->handle, d.position, d.rotation, bounds,
                                   out.voxelParts);
                out.sources = sourcesOf(*scene, link->handle);
            }

            RigidBody::Shape kind = rigid ? rigid->shape : RigidBody::Shape::Voxels;
            if (kind == RigidBody::Shape::Voxels) {
                if (!linked) {
                    out.refusal = "a voxel shape needs the entity linked to a component";
                    return out;
                }
                if (parts.empty()) {
                    out.refusal = "its component has no voxels to collide with";
                    out.noVoxels = true;
                    return out;
                }
                d.shape = parts.size() == 1 && parts[0].position == core::vec3(0.0f) &&
                                  parts[0].rotation == core::quat(1, 0, 0, 0)
                              ? parts[0].shape
                              : PhysicsShape::compound(std::move(parts));
            } else {
                out.voxelParts.clear();   // a primitive's hits are not on voxels
                // A primitive: sized by the file, or fitted to the voxels' box and centred on it.
                core::vec3 centre(0.0f), half(0.0f);
                if (!bounds.empty()) {
                    centre = 0.5f * (bounds.min + bounds.max);
                    half = 0.5f * (bounds.max - bounds.min);
                }
                PhysicsShape primitive;
                switch (kind) {
                    case RigidBody::Shape::Sphere:
                        primitive = PhysicsShape::sphere(rigid->radius > 0.0f ? rigid->radius
                                                                              : std::max({half.x, half.y, half.z}));
                        break;
                    case RigidBody::Shape::Box:
                        primitive = PhysicsShape::box(rigid->halfExtents.x > 0.0f ? rigid->halfExtents : half);
                        break;
                    case RigidBody::Shape::Capsule: {
                        float radius = rigid->radius > 0.0f ? rigid->radius : std::max(half.x, half.z);
                        float halfHeight = rigid->halfHeight > 0.0f ? rigid->halfHeight
                                                                    : std::max(half.y - radius, 0.05f * radius);
                        primitive = PhysicsShape::capsule(halfHeight, radius);
                        break;
                    }
                    case RigidBody::Shape::Voxels: break;
                }
                bool sized = (kind == RigidBody::Shape::Box) ? rigid->halfExtents.x > 0.0f
                           : (kind == RigidBody::Shape::Capsule) ? (rigid->radius > 0.0f && rigid->halfHeight > 0.0f)
                           : rigid->radius > 0.0f;
                if (bounds.empty() && !sized) {
                    out.refusal = "its primitive shape has no size, and there are no voxels to fit it to";
                    out.noVoxels = true;
                    return out;
                }
                d.shape = centre == core::vec3(0.0f) ? primitive
                                                     : PhysicsShape::compound({{primitive, centre, core::quat(1, 0, 0, 0)}});
            }

            if (rigid) {
                d.layer = rigid->layer;
                d.density = rigid->density;
                d.mass = rigid->mass;
                d.friction = rigid->friction;
                d.restitution = rigid->restitution;
                d.linearDamping = rigid->linearDamping;
                d.angularDamping = rigid->angularDamping;
                d.gravityFactor = rigid->gravityFactor;
                switch (rigid->quality) {
                    case RigidBody::Quality::Continuous: d.continuous = true; break;
                    case RigidBody::Quality::Discrete:   d.continuous = false; break;
                    case RigidBody::Quality::Auto: {
                        // Small things are the ones that slip through others in a step.
                        float smallest = bounds.empty() ? 2.0f * std::max(rigid->radius, 0.0f)
                                                        : std::min({bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y,
                                                                    bounds.max.z - bounds.min.z});
                        d.continuous = motion == MotionType::Dynamic && smallest < 1.0f;
                        break;
                    }
                }
            } else {
                d.layer = fixed->layer;
                d.friction = fixed->friction;
                d.restitution = fixed->restitution;
            }
            return out;
        }

        // ---- The FixedUpdate system ----------------------------------------------------------------

        bool wantsBody(const World& world, Entity entity) {
            return world.valid(entity) && world.any_of<RigidBody, StaticCollider>(entity);
        }

        void dropBody(World& world, PhysicsRuntime& physics, Entity entity) {
            if (const PhysicsBody* body = world.try_get<PhysicsBody>(entity)) {
                physics.world->destroyBody(body->id);
                world.remove<PhysicsBody>(entity);
            }
        }

        // The voxel chunk a body's islands are counted in: the linked component itself when it is a
        // chunk, or the one chunk inside an asset (a prefab). Anything else -- a grid, an asset of
        // several chunks -- is not split.
        ComponentHandle splittableChunk(const Scene& scene, ComponentHandle h) {
            const ComponentRecord& record = scene.components[h];
            if (record.kind == ComponentKind::Chunk) return h;
            if (record.kind != ComponentKind::Asset) return INVALID_COMPONENT_HANDLE;
            ComponentHandle found = INVALID_COMPONENT_HANDLE;
            std::vector<ComponentHandle> stack(record.children.begin(), record.children.end());
            while (!stack.empty()) {
                ComponentHandle c = stack.back();
                stack.pop_back();
                const ComponentRecord& child = scene.components[c];
                if (child.kind == ComponentKind::Grid) return INVALID_COMPONENT_HANDLE;
                if (child.kind == ComponentKind::Chunk) {
                    if (found != INVALID_COMPONENT_HANDLE) return INVALID_COMPONENT_HANDLE;
                    found = c;
                }
                stack.insert(stack.end(), child.children.begin(), child.children.end());
            }
            return found;
        }

        // A dynamic voxel body cut in pieces: each piece cut loose becomes a body of its own, moving
        // as that part of the parent was (its velocity at the piece's centre).
        void splitCutPieces(World& world, PhysicsRuntime& physics, Scene& scene, Entity parent, const BodyState& state) {
            const RigidBody* rigid = world.try_get<RigidBody>(parent);
            const VoxelComponent* link = world.try_get<VoxelComponent>(parent);
            if (!rigid || rigid->shape != RigidBody::Shape::Voxels || !link || link->mode != LinkMode::Root) return;
            ComponentHandle chunk = splittableChunk(scene, link->handle);
            if (chunk == INVALID_COMPONENT_HANDLE) return;
            utils::IslandSplit split = utils::splitVoxelIslands(scene, chunk, true);
            if (split.pieces.empty()) return;

            uint64_t total = 0;
            for (uint64_t n : split.voxels) total += n;
            RigidBody pieceRigid = *rigid;
            const OnUnlink onUnlink = link->onUnlink;
            const bool networked = world.all_of<NetId>(parent);
            BodySplit event{parent, {}};
            for (size_t i = 0; i < split.pieces.size(); i++) {
                ComponentHandle piece = split.pieces[i];
                Entity e = world.create();
                if (!linkComponent(world, e, piece, LinkMode::Root, onUnlink)) {
                    world.destroy(e);
                    continue;
                }
                RigidBody r = pieceRigid;
                if (r.mass > 0.0f) r.mass = pieceRigid.mass * float(split.voxels[i + 1]) / float(total);
                // Its motion: the parent's, at the piece's centre.
                core::vec3 centre = state.centerOfMass;
                const ComponentRecord& record = scene.components[piece];
                if (record.kind == ComponentKind::Chunk) {
                    const Chunk& c = scene.chunks[record.chunkHandle];
                    core::ivec3 low, high;
                    if (c.geometryPoolIndex >= 0 && utils::blobContentBounds(scene.geometryPool[size_t(c.geometryPoolIndex)], low, high)) {
                        float voxel = c.header.scale / float(std::max(c.header.resolution, 1u));
                        core::vec3 local = 0.5f * core::vec3(low + high + core::ivec3(1)) * voxel;
                        centre = c.header.position + c.header.rotation * local;
                    }
                }
                physics.carried[e] = {state.linearVelocity + glm::cross(state.angularVelocity, centre - state.centerOfMass),
                                      state.angularVelocity};
                if (networked) assignNetId(world, e);
                world.emplace<RigidBody>(e, r);
                event.pieces.push_back(e);
            }
            if (rigid->mass > 0.0f) {
                float kept = rigid->mass * float(split.voxels[0]) / float(total);
                world.patch<RigidBody>(parent, [kept](RigidBody& r) { r.mass = kept; });
            }
            core::trace("physics: entity {} split into {} pieces", static_cast<uint32_t>(parent), event.pieces.size() + 1);
            send(world, std::move(event));
        }

        void step(Application& app) {
            World& world = app.world;
            PhysicsRuntime& physics = runtimeOf(world);
            PhysicsWorld& sim = *physics.world;
            const float dt = app.time().fixedDelta;

            // 1. Bodies whose entity, component or RigidBody went.
            for (BodyId id : physics.doomed) sim.destroyBody(id);
            physics.doomed.clear();
            std::vector<Entity> orphans;
            for (auto [entity, body] : world.view<PhysicsBody>().each())
                if (!world.any_of<RigidBody, StaticCollider>(entity)) orphans.push_back(entity);
            for (Entity e : orphans) dropBody(world, physics, e);

            // 2a. Voxel edits issued as commands, in issue order.
            Scene* scene = world.ctx().find<Scene>();
            if (scene && !physics.edits.empty()) {
                std::vector<PhysicsRuntime::Edit> edits;
                edits.swap(physics.edits);
                for (const auto& edit : edits) {
                    ComponentHandle target = voxelComponentOf(world, edit.entity);
                    if (target == INVALID_COMPONENT_HANDLE) continue;
                    std::vector<PendingVoxelOp> adds, removes;
                    for (const PendingVoxelOp& op : edit.ops) (op.isAdd ? adds : removes).push_back(op);
                    if (!adds.empty()) utils::queueVoxelAdd(*scene, target, adds);
                    if (!removes.empty()) utils::queueVoxelRemove(*scene, target, removes);
                }
                utils::updateScene(*scene);
            }

            // 2b. Bodies whose voxels changed: split what an edit cut loose, then rebuild. Only looked
            //     for when some blob's content changed since the last step, anywhere.
            if (scene && latestBlobContentStamp() != physics.seenStamp) {
                std::vector<std::pair<Entity, uint32_t>> changed;
                for (auto [entity, body] : world.view<PhysicsBody>().each()) {
                    const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
                    if (!link || !utils::isComponentAlive(*scene, link->ref())) continue;
                    if (sourcesOf(*scene, link->handle) == body.sources) continue;
                    const NetId* id = world.try_get<NetId>(entity);
                    changed.push_back({entity, id ? id->value : 0});
                }
                std::sort(changed.begin(), changed.end(), [](const auto& a, const auto& b) {
                    return std::make_tuple(a.second == 0, a.second, a.first) < std::make_tuple(b.second == 0, b.second, b.first);
                });
                for (auto [entity, net] : changed) {
                    const PhysicsBody& body = world.get<PhysicsBody>(entity);
                    if (body.motion == MotionType::Dynamic) {
                        BodyState state = sim.bodyState(body.id);
                        physics.carried[entity] = {state.linearVelocity, state.angularVelocity};
                        splitCutPieces(world, physics, *scene, entity, state);
                    }
                    physics.rebuild.push_back(entity);
                    physics.reshaped.insert(entity);
                }
                physics.seenStamp = latestBlobContentStamp();
            }

            // 2. Rebuilds: a changed RigidBody, a new or lost link, changed voxels.
            for (Entity e : physics.rebuild) {
                if (!world.valid(e)) continue;
                dropBody(world, physics, e);
                if (wantsBody(world, e)) physics.pending.push_back(e);
            }
            physics.rebuild.clear();

            // 3. New bodies: those with NetIds first, by NetId -- the order every peer agrees on -- then
            //    the local-only rest, by entity.
            std::vector<Entity> pending;
            pending.swap(physics.pending);
            auto order = [&](Entity e) {
                const NetId* id = world.valid(e) ? world.try_get<NetId>(e) : nullptr;
                uint32_t net = id ? id->value : 0;
                return std::make_tuple(net == 0, net, e);
            };
            std::sort(pending.begin(), pending.end(), [&](Entity a, Entity b) { return order(a) < order(b); });
            pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
            for (Entity e : pending) {
                if (!wantsBody(world, e) || world.all_of<PhysicsBody>(e)) continue;
                Description described = describe(world, physics, e);
                if (auto carried = physics.carried.find(e); carried != physics.carried.end()) {
                    described.desc.linearVelocity = carried->second.linear;
                    described.desc.angularVelocity = carried->second.angular;
                    physics.carried.erase(carried);
                }
                BodyId id;
                if (!described.refusal) id = sim.createBody(described.desc);
                // Edited down to nothing -- under an existing body, or since the entity asked for one
                // -- is not a mistake to report but an outcome for the game. A component that was
                // simply never filled is still a refusal.
                bool editedSince = false;
                if (auto asked = physics.requestedAt.find(e); asked != physics.requestedAt.end())
                    for (const auto& [chunk, stamp] : described.sources) editedSince = editedSince || stamp > asked->second;
                physics.requestedAt.erase(e);
                if (!id.valid() && described.noVoxels && (physics.reshaped.count(e) || editedSince)) {
                    core::trace("physics: entity {}'s voxels are all gone; it has no body now", static_cast<uint32_t>(e));
                    send(world, BodyEmptied{e});
                    continue;
                }
                if (!id.valid()) {
                    std::string reason = described.refusal ? described.refusal : "the simulation refused it (see the log)";
                    core::warn("physics: entity {} gets no body: {}", static_cast<uint32_t>(e), reason);
                    send(world, PhysicsBodyRefused{e, reason});
                    continue;
                }
                PhysicsBody body;
                body.id = id;
                body.motion = described.desc.motion;
                body.previousPosition = body.position = described.desc.position;
                body.previousRotation = body.rotation = glm::normalize(described.desc.rotation);
                body.sources = std::move(described.sources);
                body.voxelParts = std::move(described.voxelParts);
                physics.byBody[id.value] = e;
                world.emplace<PhysicsBody>(e, body);
            }

            physics.reshaped.clear();

            // 3b. Characters: gone ones removed, new ones made, in NetId-then-entity order.
            for (CharacterId id : physics.doomedCharacters) sim.destroyCharacter(id);
            physics.doomedCharacters.clear();
            {
                std::vector<Entity> orphanCharacters;
                for (auto [entity, c] : world.view<PhysicsCharacter>().each())
                    if (!world.all_of<CharacterController>(entity)) orphanCharacters.push_back(entity);
                for (Entity entity : orphanCharacters) {
                    sim.destroyCharacter(world.get<PhysicsCharacter>(entity).id);
                    world.remove<PhysicsCharacter>(entity);
                }
                std::vector<Entity> wanted;
                wanted.swap(physics.pendingCharacters);
                std::sort(wanted.begin(), wanted.end(), [&](Entity a, Entity b) { return order(a) < order(b); });
                wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
                for (Entity e : wanted) {
                    if (!world.valid(e) || !world.all_of<CharacterController>(e) || world.all_of<PhysicsCharacter>(e)) continue;
                    const CharacterController& controller = world.get<CharacterController>(e);
                    CharacterDesc desc;
                    desc.radius = controller.radius;
                    desc.height = controller.height;
                    desc.maxSlope = controller.maxSlopeDegrees * 3.14159265f / 180.0f;
                    desc.mass = controller.mass;
                    desc.maxStrength = controller.strength;
                    desc.stepUp = controller.stepHeight;
                    desc.layer = controller.layer;
                    if (const Transform* t = world.try_get<Transform>(e)) desc.position = t->position;
                    CharacterId id = sim.createCharacter(desc);
                    if (!id.valid()) {
                        send(world, PhysicsBodyRefused{e, "the character's shape is unusable (see the log)"});
                        continue;
                    }
                    PhysicsCharacter c;
                    c.id = id;
                    c.previousPosition = c.position = desc.position;
                    physics.byBody[sim.characterBody(id).value] = e;
                    world.emplace<PhysicsCharacter>(e, c);
                }
            }

            // 3c. Joints: gone ones removed; new ones made once both their bodies exist (they wait
            //     until then), in NetId-then-entity order.
            for (JointId id : physics.doomedJoints) sim.destroyJoint(id);
            physics.doomedJoints.clear();
            {
                std::vector<Entity> orphanJoints;
                for (auto [entity, j] : world.view<PhysicsJoint>().each())
                    if (!world.all_of<Joint>(entity)) orphanJoints.push_back(entity);
                for (Entity entity : orphanJoints) {
                    sim.destroyJoint(world.get<PhysicsJoint>(entity).id);
                    world.remove<PhysicsJoint>(entity);
                }
                std::vector<Entity> wanted;
                wanted.swap(physics.pendingJoints);
                std::sort(wanted.begin(), wanted.end(), [&](Entity a, Entity b) { return order(a) < order(b); });
                wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
                for (Entity e : wanted) {
                    if (!world.valid(e) || !world.all_of<Joint>(e) || world.all_of<PhysicsJoint>(e)) continue;
                    const Joint& joint = world.get<Joint>(e);
                    auto bodyOf = [&](NetId id) -> std::optional<BodyId> {
                        if (id.value == 0) return BodyId{};
                        Entity other = entityForNetId(world, id);
                        const PhysicsBody* b = world.valid(other) ? world.try_get<PhysicsBody>(other) : nullptr;
                        if (!b) return std::nullopt;
                        return b->id;
                    };
                    std::optional<BodyId> a = bodyOf(joint.a), b = bodyOf(joint.b);
                    if (!a || !b || !a->valid()) {
                        // A body not made yet: try again next step. One that never will be is the
                        // game's to notice (its entity has no body).
                        if (joint.a.value != 0 && world.valid(entityForNetId(world, joint.a)) &&
                            (joint.b.value == 0 || world.valid(entityForNetId(world, joint.b))))
                            physics.pendingJoints.push_back(e);
                        continue;
                    }
                    JointDesc d;
                    d.type = joint.type;
                    d.a = *a;
                    d.b = *b;
                    d.anchor = joint.anchor;
                    d.anchorB = joint.anchorB;
                    d.axis = joint.axis;
                    d.limitMin = joint.limitMin;
                    d.limitMax = joint.limitMax;
                    d.springFrequency = joint.springFrequency;
                    d.springDamping = joint.springDamping;
                    d.friction = joint.friction;
                    d.motor = joint.motor;
                    d.motorMode = joint.motorMode;
                    d.motorVelocity = joint.motorVelocity;
                    d.motorTarget = joint.motorTarget;
                    d.motorLimit = joint.motorLimit;
                    d.motorFrequency = joint.motorFrequency;
                    d.breakForce = joint.breakForce;
                    d.breakTorque = joint.breakTorque;
                    d.collideConnected = joint.collideConnected;
                    JointId id = sim.createJoint(d);
                    if (!id.valid()) {
                        send(world, PhysicsBodyRefused{e, "the joint could not be made (see the log)"});
                        world.emplace<PhysicsJoint>(e, PhysicsJoint{id, true});
                        continue;
                    }
                    world.emplace<PhysicsJoint>(e, PhysicsJoint{id, false});
                }
            }

            // 4. Commands due at this step, in (tick, issuer, sequence) order. Later ones wait.
            const uint64_t thisTick = sim.tick() + 1;
            std::vector<Command> due, later;
            for (Command& c : physics.commands) (c.c.tick <= thisTick ? due : later).push_back(c);
            physics.commands.swap(later);
            std::stable_sort(due.begin(), due.end(), [](const Command& x, const Command& y) {
                return std::tie(x.c.tick, x.c.issuer, x.c.sequence) < std::tie(y.c.tick, y.c.issuer, y.c.sequence);
            });
            using Kind = PhysicsCommand::Kind;
            for (const Command& command : due) {
                const PhysicsCommand& c = command.c;
                if (c.tick < thisTick) physics.late++;
                if (c.kind == Kind::Gravity) { sim.setGravity(c.a); continue; }
                Entity target = command.entity != NullEntity ? command.entity : entityForNetId(world, c.target);
                if (c.kind == Kind::JointMotor) {
                    if (const PhysicsJoint* j = world.valid(target) ? world.try_get<PhysicsJoint>(target) : nullptr)
                        sim.setJointMotor(j->id, c.jump, c.b.x > 0.5f ? MotorMode::Position : MotorMode::Velocity, c.a.x);
                    continue;
                }
                if (c.kind == Kind::CharacterInput) {
                    if (const PhysicsCharacter* ch = world.valid(target) ? world.try_get<PhysicsCharacter>(target) : nullptr)
                        sim.setCharacterInput(ch->id, c.a, c.jump, c.b.x);
                    continue;
                }
                PhysicsBody* body = world.valid(target) ? world.try_get<PhysicsBody>(target) : nullptr;
                if (!body) continue;
                switch (c.kind) {
                    case Kind::Impulse:        sim.addImpulse(body->id, c.a); break;
                    case Kind::AngularImpulse: sim.addAngularImpulse(body->id, c.a); break;
                    case Kind::AddVelocity:    sim.addVelocity(body->id, c.a); break;
                    case Kind::SetVelocity:    sim.setVelocity(body->id, c.a, c.b); break;
                    case Kind::Teleport:
                        sim.setPose(body->id, c.a, c.q);
                        // Not interpolated from where it was: a teleport is a jump, drawn as one.
                        body->previousPosition = body->position = c.a;
                        body->previousRotation = body->rotation = glm::normalize(c.q);
                        body->presented = false;
                        break;
                    case Kind::Gravity: break;
                    case Kind::CharacterInput: break;
                    case Kind::JointMotor: break;
                }
            }

            // 5. Kinematic bodies head for their Transform (a Part's from the Scene, where the bridge
            //    last put it).
            for (auto [entity, body] : world.view<PhysicsBody>().each()) {
                if (body.motion != MotionType::Kinematic) continue;
                const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
                if (link && link->mode == LinkMode::Part) {
                    if (scene && utils::isComponentAlive(*scene, link->ref()))
                        sim.moveKinematic(body.id, utils::getComponentWorldPosition(*scene, link->handle),
                                          utils::getComponentWorldRotation(*scene, link->handle), dt);
                } else if (const Transform* t = world.try_get<Transform>(entity)) {
                    sim.moveKinematic(body.id, t->position, t->rotation, dt);
                }
            }

            // 6. The step.
            sim.step(dt);

            // 7. The poses it left.
            for (auto [entity, body] : world.view<PhysicsBody>().each()) {
                if (body.motion == MotionType::Static) continue;
                BodyState state = sim.bodyState(body.id);
                body.previousPosition = body.position;
                body.previousRotation = body.rotation;
                body.position = state.position;
                body.rotation = state.rotation;
            }

            for (auto [entity, c] : world.view<PhysicsCharacter>().each()) {
                c.previousPosition = c.position;
                c.position = sim.characterState(c.id).position;
            }

            // 7b. Joints that broke, or lost a body.
            {
                std::vector<PhysicsWorld::BrokenJoint> broken = sim.takeBrokenJoints();
                struct Gone { Entity entity; bool bodyGone; float force, torque; };
                std::vector<Gone> gone;
                for (auto [entity, j] : world.view<PhysicsJoint>().each()) {
                    if (j.broken) continue;
                    auto it = std::find_if(broken.begin(), broken.end(), [&](const auto& b) { return b.id == j.id; });
                    if (it != broken.end()) gone.push_back({entity, false, it->force, it->torque});
                    else if (!sim.isAlive(j.id)) gone.push_back({entity, true, 0.0f, 0.0f});
                }
                std::sort(gone.begin(), gone.end(), [](const Gone& x, const Gone& y) { return x.entity < y.entity; });
                for (const Gone& g : gone) {
                    world.get<PhysicsJoint>(g.entity).broken = true;
                    send(world, JointBroke{g.entity, g.bodyGone, g.force, g.torque});
                }
            }

            // 8. Bodies that left the world: reported, and by default retired.
            std::vector<BodyId> leavers = sim.takeBodiesThatLeftTheWorld();
            if (!leavers.empty()) {
                std::vector<std::pair<Entity, core::vec3>> gone;
                for (auto [entity, body] : world.view<PhysicsBody>().each())
                    if (std::find(leavers.begin(), leavers.end(), body.id) != leavers.end())
                        gone.push_back({entity, body.position});
                std::sort(gone.begin(), gone.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                for (auto [entity, position] : gone) {
                    core::trace("physics: entity {} left the world at ({}, {}, {})", static_cast<uint32_t>(entity),
                                position.x, position.y, position.z);
                    send(world, BodyLeftWorld{entity, position});
                    if (physics.destroyLeavers) world.destroy(entity);
                }
            }

            // 9. Steps dropped because frames could not keep up: said, but not every frame.
            const Time& time = app.time();
            if (time.droppedFixedSteps != physics.seenDroppedSteps) {
                physics.seenDroppedSteps = time.droppedFixedSteps;
                send(world, SimulationSlow{time.droppedFixedSteps});
                if (sim.tick() >= physics.lastSlowWarningTick + 300 || physics.lastSlowWarningTick == 0) {
                    physics.lastSlowWarningTick = sim.tick();
                    // Say where the time went: a frame hitch (a load, a window drag) drops steps too,
                    // and then physics is not what to look at.
                    core::warn("physics: frames fell behind the fixed clock and {} fixed steps were dropped so far "
                               "(the simulation runs slower than real time while that lasts); the last physics "
                               "step took {:.2f} ms", time.droppedFixedSteps, sim.stats().lastStepMilliseconds);
                }
            }
        }

        // ---- The PostUpdate system: what is drawn -----------------------------------------------------

        void present(Application& app) {
            World& world = app.world;
            PhysicsRuntime& physics = runtimeOf(world);
            const float alpha = app.time().fixedAlpha;
            struct Write { Entity entity; core::vec3 position; core::quat rotation; };
            std::vector<Write> writes;
            for (auto [entity, body] : world.view<PhysicsBody>().each()) {
                if (body.motion != MotionType::Dynamic || !world.all_of<Transform>(entity)) continue;
                core::vec3 position = glm::mix(body.previousPosition, body.position, alpha);
                core::quat rotation = glm::slerp(body.previousRotation, body.rotation, alpha);
                if (body.presented && position == body.presentedPosition && rotation == body.presentedRotation) continue;
                body.presented = true;
                body.presentedPosition = position;
                body.presentedRotation = rotation;
                writes.push_back({entity, position, rotation});
            }
            std::vector<std::pair<Entity, core::vec3>> characters;
            for (auto [entity, c] : world.view<PhysicsCharacter>().each()) {
                if (!world.all_of<Transform>(entity)) continue;
                core::vec3 position = glm::mix(c.previousPosition, c.position, alpha);
                if (c.presented && position == c.presentedPosition) continue;
                c.presented = true;
                c.presentedPosition = position;
                characters.push_back({entity, position});
            }
            physics.writingTransforms = true;
            for (auto [entity, position] : characters)
                world.patch<Transform>(entity, [&](Transform& t) { t.position = position; });
            for (const Write& w : writes) {
                world.patch<Transform>(w.entity, [&](Transform& t) {
                    t.position = w.position;
                    t.rotation = w.rotation;
                });
            }
            physics.writingTransforms = false;
        }

        // A local command: for the next step, issuer 0, in issue order.
        void queue(World& world, PhysicsCommand::Kind kind, Entity entity, core::vec3 a, core::vec3 b = core::vec3(0.0f),
                   core::quat q = core::quat(1, 0, 0, 0)) {
            PhysicsRuntime& physics = runtimeOf(world);
            Command command;
            command.c.kind = kind;
            command.c.a = a;
            command.c.b = b;
            command.c.q = q;
            command.c.tick = physics.world->tick() + 1;
            command.c.sequence = physics.sequence++;
            if (entity != NullEntity && world.valid(entity)) {
                command.entity = entity;
                if (const NetId* id = world.try_get<NetId>(entity)) command.c.target = *id;
            }
            physics.commands.push_back(command);
        }

        struct Hash {
            uint64_t value = 1469598103934665603ull;
            template <typename T> void add(const T& v) {
                const auto* p = reinterpret_cast<const uint8_t*>(&v);
                for (size_t i = 0; i < sizeof(T); i++) { value ^= p[i]; value *= 1099511628211ull; }
            }
        };

        // Bodies in the order the hash and the snapshot use: NetId order, then the rest by entity.
        std::vector<std::pair<Entity, uint32_t>> bodiesInOrder(const World& world) {
            std::vector<std::pair<Entity, uint32_t>> out;
            for (auto [entity, body] : world.view<PhysicsBody>().each()) {
                const NetId* id = world.try_get<NetId>(entity);
                out.push_back({entity, id ? id->value : 0});
            }
            std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) {
                return std::make_tuple(x.second == 0, x.second, x.first) < std::make_tuple(y.second == 0, y.second, y.first);
            });
            return out;
        }

        template <typename T> void put(std::vector<uint8_t>& out, const T& v) {
            const auto* p = reinterpret_cast<const uint8_t*>(&v);
            out.insert(out.end(), p, p + sizeof(T));
        }
        template <typename T> bool get(const std::vector<uint8_t>& in, size_t& at, T& v) {
            if (at > in.size() || in.size() - at < sizeof(T)) return false;
            std::memcpy(&v, in.data() + at, sizeof(T));
            at += sizeof(T);
            return true;
        }
        constexpr uint32_t RUNTIME_SNAPSHOT_MAGIC = 0x52564a50u;   // "PJVR"

        // ---- entities.json helpers -------------------------------------------------------------------

        double tidy(float v) { return std::round(double(v) * 1e4) / 1e4; }

        constexpr std::array<const char*, 5> LAYER_NAMES{"static", "moving", "debris", "sensor", "character"};
        const char* layerName(PhysicsLayer layer) {
            size_t i = size_t(layer);
            return i < LAYER_NAMES.size() ? LAYER_NAMES[i] : "moving";
        }
        std::optional<PhysicsLayer> layerFrom(const std::string& name) {
            for (size_t i = 0; i < LAYER_NAMES.size(); i++)
                if (name == LAYER_NAMES[i]) return PhysicsLayer(i);
            return std::nullopt;
        }
    }

    void installPhysics(Application& app, const PhysicsConfig& config) {
        World& world = app.world;
        if (world.ctx().contains<PhysicsRuntime>()) return;
        PhysicsRuntime& physics = world.ctx().emplace<PhysicsRuntime>();
        physics.world = std::make_unique<PhysicsWorld>(config.settings);
        physics.collision = config.collision;
        physics.destroyLeavers = config.destroyBodiesThatLeaveTheWorld;
        physics.seenStamp = latestBlobContentStamp();

        installNetIds(world);
        registerComponent<RigidBody>(world);
        registerComponent<StaticCollider>(world);
        registerComponent<CharacterController>(world);
        registerComponent<Joint>(world);
        world.on_construct<Joint>().connect<&wantJoint>();
        world.on_destroy<Joint>().connect<&jointGone>();
        world.on_destroy<PhysicsJoint>().connect<&jointGone>();
        world.on_construct<CharacterController>().connect<&wantCharacter>();
        world.on_destroy<CharacterController>().connect<&characterGone>();
        world.on_destroy<PhysicsCharacter>().connect<&characterGone>();

        world.on_construct<RigidBody>().connect<&wantBody>();
        world.on_construct<StaticCollider>().connect<&wantBody>();
        world.on_update<RigidBody>().connect<&wantRebuild>();
        world.on_update<StaticCollider>().connect<&wantRebuild>();
        world.on_destroy<RigidBody>().connect<&bodyGone>();
        world.on_destroy<StaticCollider>().connect<&bodyGone>();
        world.on_destroy<PhysicsBody>().connect<&bodyComponentGone>();
        world.on_construct<VoxelComponent>().connect<&wantRebuild>();
        world.on_destroy<VoxelComponent>().connect<&wantRebuild>();
        world.on_update<Transform>().connect<&transformChanged>();

        app.addSystem(Stage::FixedUpdate, "physics", step);
        app.addSystem(Stage::PostUpdate, "physics: present", present, {.before = "scene bridge"});
    }

    PhysicsWorld& physicsWorld(World& world) { return *runtimeOf(world).world; }
    const PhysicsWorld& physicsWorld(const World& world) { return *world.ctx().get<PhysicsRuntime>().world; }

    using Kind = PhysicsCommand::Kind;
    void addImpulse(World& world, Entity e, core::vec3 v) { queue(world, Kind::Impulse, e, v); }
    void addAngularImpulse(World& world, Entity e, core::vec3 v) { queue(world, Kind::AngularImpulse, e, v); }
    void addVelocity(World& world, Entity e, core::vec3 v) { queue(world, Kind::AddVelocity, e, v); }
    void setVelocity(World& world, Entity e, core::vec3 linear, core::vec3 angular) {
        queue(world, Kind::SetVelocity, e, linear, angular);
    }
    void teleport(World& world, Entity e, core::vec3 position, core::quat rotation) {
        queue(world, Kind::Teleport, e, position, core::vec3(0.0f), rotation);
    }
    void setGravity(World& world, core::vec3 gravity) { queue(world, Kind::Gravity, NullEntity, gravity); }

    void editVoxels(World& world, Entity entity, const std::vector<PendingVoxelOp>& ops) {
        runtimeOf(world).edits.push_back({entity, ops});
    }

    ComponentHandle voxelComponentOf(const World& world, Entity entity) {
        const Scene* scene = world.ctx().find<Scene>();
        const VoxelComponent* link = world.valid(entity) ? world.try_get<VoxelComponent>(entity) : nullptr;
        if (!scene || !link || !utils::isComponentAlive(*scene, link->ref())) return INVALID_COMPONENT_HANDLE;
        ComponentKind kind = scene->components[link->handle].kind;
        if (kind == ComponentKind::Grid) return link->handle;
        return splittableChunk(*scene, link->handle);
    }

    std::vector<PendingVoxelOp> voxelsInSphere(const World& world, Entity entity, core::vec3 centre, float radius) {
        std::vector<PendingVoxelOp> ops;
        const Scene* scene = world.ctx().find<Scene>();
        ComponentHandle h = voxelComponentOf(world, entity);
        if (h == INVALID_COMPONENT_HANDLE || !(radius > 0.0f)) return ops;
        const ComponentRecord& record = scene->components[h];
        // The component's voxel size in its own (unscaled) space, and its bounds in voxels.
        float voxel = 0.0f;
        core::ivec3 low(std::numeric_limits<int>::min()), high(std::numeric_limits<int>::max());
        if (record.kind == ComponentKind::Chunk) {
            const Chunk& chunk = scene->chunks[record.chunkHandle];
            voxel = chunk.nativeScale / float(std::max(chunk.header.resolution, 1u));
            low = core::ivec3(0);
            high = core::ivec3(int(chunk.header.resolution) - 1);
        } else if (record.gridIndex >= 0) {
            const SceneGrid& grid = scene->grids[size_t(record.gridIndex)];
            int cellResolution = 0;
            for (int32_t cell : grid.cellToChunk)
                if (cell >= 0) { cellResolution = int(std::max(scene->chunks[size_t(cell)].header.resolution, 1u)); break; }
            if (cellResolution > 0) {
                voxel = grid.nativeCellSize / float(cellResolution);
                // Only where the grid has cells: there is nothing to carve beyond them.
                low = grid.originCellCoord * cellResolution;
                high = (grid.originCellCoord + grid.dims) * cellResolution - core::ivec3(1);
            }
        }
        if (!(voxel > 0.0f)) return ops;
        core::mat4 toLocal = glm::inverse(utils::getComponentWorldMatrix(*scene, h));
        float scale = glm::length(core::vec3(utils::getComponentWorldMatrix(*scene, h)[0]));
        core::vec3 at = core::vec3(toLocal * core::vec4(centre, 1.0f)) / voxel;   // in voxels
        float r = radius / std::max(scale, 1e-6f) / voxel;
        core::ivec3 from = glm::max(core::ivec3(glm::floor(at - r)), low), to = glm::min(core::ivec3(glm::ceil(at + r)), high);
        for (int z = from.z; z <= to.z; z++)
            for (int y = from.y; y <= to.y; y++)
                for (int x = from.x; x <= to.x; x++) {
                    core::vec3 d = core::vec3(x, y, z) + core::vec3(0.5f) - at;
                    if (glm::dot(d, d) <= r * r) ops.push_back({false, core::ivec3(x, y, z), 0});
                }
        return ops;
    }

    void setCharacterInput(World& world, Entity entity, core::vec3 velocity, bool jump, float jumpSpeed) {
        PhysicsRuntime& physics = runtimeOf(world);
        Command command;
        command.c.kind = Kind::CharacterInput;
        command.c.a = velocity;
        command.c.b = core::vec3(jumpSpeed, 0.0f, 0.0f);
        command.c.jump = jump;
        command.c.tick = physics.world->tick() + 1;
        command.c.sequence = physics.sequence++;
        if (world.valid(entity)) {
            command.entity = entity;
            if (const NetId* id = world.try_get<NetId>(entity)) command.c.target = *id;
        }
        physics.commands.push_back(command);
    }

    Entity connectBodies(World& world, Entity a, Entity b, Joint joint) {
        joint.a = assignNetId(world, a);
        joint.b = b == NullEntity ? NetId{} : assignNetId(world, b);
        Entity e = world.create();
        assignNetId(world, e);
        world.emplace<Joint>(e, joint);
        return e;
    }

    void setJointMotor(World& world, Entity joint, bool on, float value, MotorMode mode) {
        PhysicsRuntime& physics = runtimeOf(world);
        Command command;
        command.c.kind = Kind::JointMotor;
        command.c.a = core::vec3(value, 0.0f, 0.0f);
        command.c.b = core::vec3(mode == MotorMode::Position ? 1.0f : 0.0f, 0.0f, 0.0f);
        command.c.jump = on;
        command.c.tick = physics.world->tick() + 1;
        command.c.sequence = physics.sequence++;
        if (world.valid(joint)) {
            command.entity = joint;
            if (const NetId* id = world.try_get<NetId>(joint)) command.c.target = *id;
        }
        physics.commands.push_back(command);
    }

    JointState jointState(const World& world, Entity joint) {
        const PhysicsJoint* j = world.valid(joint) ? world.try_get<PhysicsJoint>(joint) : nullptr;
        return j && !j->broken ? physicsWorld(world).jointState(j->id) : JointState{};
    }

    CharacterState characterState(const World& world, Entity entity) {
        const PhysicsCharacter* c = world.valid(entity) ? world.try_get<PhysicsCharacter>(entity) : nullptr;
        return c ? physicsWorld(world).characterState(c->id) : CharacterState{};
    }

    void submitCommand(World& world, const PhysicsCommand& command) {
        runtimeOf(world).commands.push_back(Command{command, NullEntity});
    }
    uint64_t nextPhysicsTick(const World& world) { return physicsWorld(world).tick() + 1; }
    uint64_t lateCommands(const World& world) { return world.ctx().get<PhysicsRuntime>().late; }

    uint64_t physicsStateHash(const World& world) {
        const PhysicsWorld& sim = physicsWorld(world);
        Hash h;
        h.add(sim.tick());
        for (auto [entity, net] : bodiesInOrder(world)) {
            BodyState s = sim.bodyState(world.get<PhysicsBody>(entity).id);
            h.add(net);
            if (net == 0) h.add(entity);
            h.add(s.position); h.add(s.rotation); h.add(s.linearVelocity); h.add(s.angularVelocity);
            h.add(uint8_t(s.awake));
        }
        return h.value;
    }

    std::vector<uint8_t> savePhysics(const World& world) {
        const PhysicsRuntime& physics = world.ctx().get<PhysicsRuntime>();
        std::vector<uint8_t> out;
        put(out, RUNTIME_SNAPSHOT_MAGIC);
        std::vector<uint8_t> sim = physics.world->saveState();
        put(out, uint64_t(sim.size()));
        out.insert(out.end(), sim.begin(), sim.end());
        auto bodies = bodiesInOrder(world);
        put(out, uint32_t(bodies.size()));
        for (auto [entity, net] : bodies) {
            const PhysicsBody& b = world.get<PhysicsBody>(entity);
            put(out, net);
            put(out, entity);
            put(out, b.previousPosition); put(out, b.position); put(out, b.previousRotation); put(out, b.rotation);
        }
        put(out, uint32_t(physics.commands.size()));
        for (const Command& c : physics.commands) { put(out, c.c); put(out, c.entity); }
        put(out, physics.sequence);
        return out;
    }

    bool restorePhysics(World& world, const std::vector<uint8_t>& snapshot) {
        PhysicsRuntime& physics = runtimeOf(world);
        size_t at = 0;
        uint32_t magic = 0, count = 0, commandCount = 0, sequence = 0;
        uint64_t simSize = 0;
        if (!get(snapshot, at, magic) || magic != RUNTIME_SNAPSHOT_MAGIC || !get(snapshot, at, simSize) ||
            snapshot.size() - at < simSize) {
            core::warn("physics: restorePhysics refused: not a physics snapshot");
            return false;
        }
        std::vector<uint8_t> sim(snapshot.begin() + long(at), snapshot.begin() + long(at + simSize));
        at += simSize;
        struct Saved { uint32_t net; Entity entity; PhysicsBody poses; };
        std::vector<Saved> saved;
        if (!get(snapshot, at, count)) return false;
        for (uint32_t i = 0; i < count; i++) {
            Saved s{};
            if (!get(snapshot, at, s.net) || !get(snapshot, at, s.entity) || !get(snapshot, at, s.poses.previousPosition) ||
                !get(snapshot, at, s.poses.position) || !get(snapshot, at, s.poses.previousRotation) ||
                !get(snapshot, at, s.poses.rotation)) {
                core::warn("physics: restorePhysics refused: the snapshot is truncated");
                return false;
            }
            saved.push_back(s);
        }
        std::vector<Command> commands;
        if (!get(snapshot, at, commandCount)) return false;
        for (uint32_t i = 0; i < commandCount; i++) {
            Command c;
            if (!get(snapshot, at, c.c) || !get(snapshot, at, c.entity)) return false;
            commands.push_back(c);
        }
        if (!get(snapshot, at, sequence)) return false;

        // The same bodies, matched by NetId (or by entity, for those without): checked before
        // anything is touched.
        auto bodies = bodiesInOrder(world);
        bool same = bodies.size() == saved.size();
        for (size_t i = 0; same && i < bodies.size(); i++)
            same = bodies[i].second == saved[i].net && (saved[i].net != 0 || bodies[i].first == saved[i].entity);
        if (!same) {
            core::warn("physics: restorePhysics refused: the snapshot's bodies are not this world's");
            return false;
        }
        if (!physics.world->restoreState(sim)) return false;
        for (size_t i = 0; i < bodies.size(); i++) {
            PhysicsBody& b = world.get<PhysicsBody>(bodies[i].first);
            b.previousPosition = saved[i].poses.previousPosition;
            b.position = saved[i].poses.position;
            b.previousRotation = saved[i].poses.previousRotation;
            b.rotation = saved[i].poses.rotation;
            b.presented = false;
        }
        physics.commands = std::move(commands);
        physics.sequence = sequence;
        return true;
    }

    namespace {
        // A hit on a body, traced to its entity and, for a voxel body, the voxel. The path names one
        // compound index per level: the part (when the body has more than one), then the piece (when
        // the part's shape has more than one). The point is stepped half a voxel into the surface,
        // into the body's frame by its simulation pose, and into the part's voxels.
        std::optional<QueryHit> resolve(const World& world, const std::optional<PhysicsHit>& hit) {
            if (!hit) return std::nullopt;
            const PhysicsRuntime& physics = world.ctx().get<PhysicsRuntime>();
            auto found = physics.byBody.find(hit->body.value);
            if (found == physics.byBody.end() || !world.valid(found->second)) return std::nullopt;
            QueryHit out;
            out.entity = found->second;
            out.position = hit->position;
            out.normal = hit->normal;
            out.distance = hit->distance;
            if (const NetId* id = world.try_get<NetId>(out.entity)) out.netId = *id;
            const PhysicsBody* body = world.try_get<PhysicsBody>(out.entity);
            const Scene* scene = world.ctx().find<Scene>();
            if (!body || body->voxelParts.empty() || !scene) return out;

            size_t level = 0;
            size_t partIndex = 0;
            if (body->voxelParts.size() > 1) {
                if (level >= hit->path.size()) return out;
                partIndex = hit->path[level++];
            }
            if (partIndex >= body->voxelParts.size()) return out;
            const PhysicsVoxelPart& part = body->voxelParts[partIndex];
            const utils::CollisionPieces* pieces = part.shape ? part.shape->source.get() : nullptr;
            if (!pieces || pieces->pieces.empty()) return out;
            size_t pieceIndex = 0;
            if (pieces->pieces.size() > 1) {
                if (level >= hit->path.size()) return out;
                pieceIndex = hit->path[level];
            }
            if (pieceIndex >= pieces->pieces.size()) return out;
            const utils::CollisionPiece& piece = pieces->pieces[pieceIndex];

            BodyState pose = physicsWorld(world).bodyState(body->id);
            core::vec3 inside = hit->position - hit->normal * (0.5f * part.voxelSize);
            core::vec3 local = glm::conjugate(pose.rotation) * (inside - pose.position);
            core::vec3 voxel = glm::conjugate(part.rotation) * (local - part.position) / part.voxelSize;
            out.voxel = glm::clamp(core::ivec3(glm::floor(voxel)), piece.voxelMin, piece.voxelMax);
            out.chunk = part.chunk;
            out.onVoxel = true;
            if (part.chunk < scene->chunks.size()) {
                const Chunk& chunk = scene->chunks[part.chunk];
                out.component = chunk.gridIndex >= 0 && size_t(chunk.gridIndex) < scene->grids.size()
                                    ? scene->grids[size_t(chunk.gridIndex)].componentHandle
                                    : chunk.componentHandle;
                if (chunk.geometryPoolIndex >= 0 && size_t(chunk.geometryPoolIndex) < scene->geometryPool.size())
                    utils::queryVoxelMaterial(scene->geometryPool[size_t(chunk.geometryPoolIndex)], chunk.header.resolution,
                                              out.voxel, out.materialSlot);
            }
            return out;
        }

        QueryFilter filterFor(const World& world, uint32_t layers, Entity ignore) {
            QueryFilter f;
            f.layers = layers;
            if (ignore != NullEntity && world.valid(ignore))
                if (const PhysicsBody* b = world.try_get<PhysicsBody>(ignore)) f.ignore = b->id;
            return f;
        }

        std::vector<Entity> entitiesOf(const World& world, const std::vector<BodyId>& ids) {
            const PhysicsRuntime& physics = world.ctx().get<PhysicsRuntime>();
            std::vector<std::pair<Entity, uint32_t>> found;
            for (BodyId id : ids) {
                auto it = physics.byBody.find(id.value);
                if (it == physics.byBody.end() || !world.valid(it->second)) continue;
                const NetId* net = world.try_get<NetId>(it->second);
                found.push_back({it->second, net ? net->value : 0});
            }
            std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
                return std::make_tuple(a.second == 0, a.second, a.first) < std::make_tuple(b.second == 0, b.second, b.first);
            });
            std::vector<Entity> out;
            for (auto [e, net] : found) out.push_back(e);
            return out;
        }
    }

    std::optional<QueryHit> castRay(const World& world, core::vec3 origin, core::vec3 direction, float maxDistance,
                                    uint32_t layers, Entity ignore) {
        return resolve(world, physicsWorld(world).castRay(origin, direction, maxDistance, filterFor(world, layers, ignore)));
    }

    std::optional<QueryHit> castSphere(const World& world, core::vec3 origin, float radius, core::vec3 direction,
                                       float maxDistance, uint32_t layers, Entity ignore) {
        return resolve(world, physicsWorld(world).castSphere(origin, radius, direction, maxDistance,
                                                             filterFor(world, layers, ignore)));
    }

    std::vector<Entity> overlapSphere(const World& world, core::vec3 centre, float radius, uint32_t layers, Entity ignore) {
        return entitiesOf(world, physicsWorld(world).overlapSphere(centre, radius, filterFor(world, layers, ignore)));
    }

    std::vector<Entity> overlapBox(const World& world, core::vec3 centre, core::vec3 halfExtents, core::quat rotation,
                                   uint32_t layers, Entity ignore) {
        return entitiesOf(world, physicsWorld(world).overlapBox(centre, halfExtents, rotation, filterFor(world, layers, ignore)));
    }

    BodyState bodyPose(const World& world, Entity entity) {
        if (!world.valid(entity)) return {};
        const PhysicsBody* body = world.try_get<PhysicsBody>(entity);
        if (!body) return {};
        return physicsWorld(world).bodyState(body->id);
    }

    bool hasBody(const World& world, Entity entity) {
        return world.valid(entity) && world.all_of<PhysicsBody>(entity);
    }
}

// ---- entities.json ---------------------------------------------------------------------------------


nlohmann::json projv::runtime::ComponentTraits<projv::RigidBody>::save(const projv::RigidBody& b) {
    const projv::RigidBody defaults;
    nlohmann::json j = nlohmann::json::object();
    static constexpr const char* motions[] = {"static", "kinematic", "dynamic"};
    static constexpr const char* shapes[] = {"voxels", "sphere", "box", "capsule"};
    static constexpr const char* qualities[] = {"auto", "discrete", "continuous"};
    j["motion"] = motions[size_t(b.motion)];
    if (b.shape != defaults.shape) j["shape"] = shapes[size_t(b.shape)];
    if (b.radius > 0.0f) j["radius"] = tidy(b.radius);
    if (b.halfExtents.x > 0.0f) j["halfExtents"] = {tidy(b.halfExtents.x), tidy(b.halfExtents.y), tidy(b.halfExtents.z)};
    if (b.halfHeight > 0.0f) j["halfHeight"] = tidy(b.halfHeight);
    if (b.mass > 0.0f) j["mass"] = tidy(b.mass);
    else if (b.density != defaults.density) j["density"] = tidy(b.density);
    if (b.friction != defaults.friction) j["friction"] = tidy(b.friction);
    if (b.restitution != defaults.restitution) j["restitution"] = tidy(b.restitution);
    if (b.linearDamping != defaults.linearDamping) j["linearDamping"] = tidy(b.linearDamping);
    if (b.angularDamping != defaults.angularDamping) j["angularDamping"] = tidy(b.angularDamping);
    if (b.gravityFactor != defaults.gravityFactor) j["gravityFactor"] = tidy(b.gravityFactor);
    if (b.layer != defaults.layer) j["layer"] = layerName(b.layer);
    if (b.quality != defaults.quality) j["quality"] = qualities[size_t(b.quality)];
    return j;
}

std::optional<projv::RigidBody> projv::runtime::ComponentTraits<projv::RigidBody>::load(const nlohmann::json& j, uint32_t) {
    using projv::RigidBody;
    RigidBody b;
    // An unknown word refuses the whole value, which entities.json then keeps as written: better
    // than simulating something the file did not say.
    std::string motion = j.value("motion", "dynamic");
    if (motion == "static") b.motion = projv::runtime::MotionType::Static;
    else if (motion == "kinematic") b.motion = projv::runtime::MotionType::Kinematic;
    else if (motion == "dynamic") b.motion = projv::runtime::MotionType::Dynamic;
    else return std::nullopt;
    std::string shape = j.value("shape", "voxels");
    if (shape == "voxels") b.shape = RigidBody::Shape::Voxels;
    else if (shape == "sphere") b.shape = RigidBody::Shape::Sphere;
    else if (shape == "box") b.shape = RigidBody::Shape::Box;
    else if (shape == "capsule") b.shape = RigidBody::Shape::Capsule;
    else return std::nullopt;
    std::string quality = j.value("quality", "auto");
    if (quality == "auto") b.quality = RigidBody::Quality::Auto;
    else if (quality == "discrete") b.quality = RigidBody::Quality::Discrete;
    else if (quality == "continuous") b.quality = RigidBody::Quality::Continuous;
    else return std::nullopt;
    if (j.contains("layer")) {
        auto layer = layerFrom(j.at("layer").get<std::string>());
        if (!layer) return std::nullopt;
        b.layer = *layer;
    }
    b.radius = j.value("radius", 0.0f);
    if (j.contains("halfExtents")) {
        const auto& h = j.at("halfExtents");
        b.halfExtents = {h.at(0).get<float>(), h.at(1).get<float>(), h.at(2).get<float>()};
    }
    b.halfHeight = j.value("halfHeight", 0.0f);
    b.density = j.value("density", b.density);
    b.mass = j.value("mass", 0.0f);
    b.friction = j.value("friction", b.friction);
    b.restitution = j.value("restitution", b.restitution);
    b.linearDamping = j.value("linearDamping", b.linearDamping);
    b.angularDamping = j.value("angularDamping", b.angularDamping);
    b.gravityFactor = j.value("gravityFactor", b.gravityFactor);
    return b;
}

nlohmann::json projv::runtime::ComponentTraits<projv::StaticCollider>::save(const projv::StaticCollider& c) {
    const projv::StaticCollider defaults;
    nlohmann::json j = nlohmann::json::object();
    if (c.friction != defaults.friction) j["friction"] = tidy(c.friction);
    if (c.restitution != defaults.restitution) j["restitution"] = tidy(c.restitution);
    if (c.layer != defaults.layer) j["layer"] = layerName(c.layer);
    if (c.splits) j["splits"] = true;
    return j;
}

std::optional<projv::StaticCollider> projv::runtime::ComponentTraits<projv::StaticCollider>::load(const nlohmann::json& j,
                                                                                                uint32_t) {
    projv::StaticCollider c;
    c.friction = j.value("friction", c.friction);
    c.restitution = j.value("restitution", c.restitution);
    if (j.contains("layer")) {
        auto layer = layerFrom(j.at("layer").get<std::string>());
        if (!layer) return std::nullopt;
        c.layer = *layer;
    }
    c.splits = j.value("splits", false);
    return c;
}

nlohmann::json projv::runtime::ComponentTraits<projv::CharacterController>::save(const projv::CharacterController& c) {
    const projv::CharacterController defaults;
    nlohmann::json j = nlohmann::json::object();
    if (c.radius != defaults.radius) j["radius"] = tidy(c.radius);
    if (c.height != defaults.height) j["height"] = tidy(c.height);
    if (c.maxSlopeDegrees != defaults.maxSlopeDegrees) j["maxSlopeDegrees"] = tidy(c.maxSlopeDegrees);
    if (c.mass != defaults.mass) j["mass"] = tidy(c.mass);
    if (c.strength != defaults.strength) j["strength"] = tidy(c.strength);
    if (c.stepHeight != defaults.stepHeight) j["stepHeight"] = tidy(c.stepHeight);
    if (c.layer != defaults.layer) j["layer"] = layerName(c.layer);
    return j;
}

std::optional<projv::CharacterController> projv::runtime::ComponentTraits<projv::CharacterController>::load(
    const nlohmann::json& j, uint32_t) {
    projv::CharacterController c;
    c.radius = j.value("radius", c.radius);
    c.height = j.value("height", c.height);
    c.maxSlopeDegrees = j.value("maxSlopeDegrees", c.maxSlopeDegrees);
    c.mass = j.value("mass", c.mass);
    c.strength = j.value("strength", c.strength);
    c.stepHeight = j.value("stepHeight", c.stepHeight);
    if (j.contains("layer")) {
        auto layer = layerFrom(j.at("layer").get<std::string>());
        if (!layer) return std::nullopt;
        c.layer = *layer;
    }
    return c;
}

nlohmann::json projv::runtime::ComponentTraits<projv::Joint>::save(const projv::Joint& j) {
    static constexpr const char* types[] = {"fixed", "point", "hinge", "slider", "distance"};
    auto v3 = [](projv::core::vec3 v) { return nlohmann::json{tidy(v.x), tidy(v.y), tidy(v.z)}; };
    nlohmann::json out{{"type", types[size_t(j.type)]}, {"a", j.a.value}, {"anchor", v3(j.anchor)}};
    if (j.b.value) out["b"] = j.b.value;
    if (j.type == projv::runtime::JointType::Distance) out["anchorB"] = v3(j.anchorB);
    if (j.type == projv::runtime::JointType::Hinge || j.type == projv::runtime::JointType::Slider) out["axis"] = v3(j.axis);
    if (j.limitMin != 0.0f || j.limitMax != 0.0f) out["limits"] = {tidy(j.limitMin), tidy(j.limitMax)};
    if (j.springFrequency > 0.0f) out["spring"] = {tidy(j.springFrequency), tidy(j.springDamping)};
    if (j.friction > 0.0f) out["friction"] = tidy(j.friction);
    if (j.motor || j.motorVelocity != 0.0f || j.motorLimit > 0.0f) {
        out["motor"] = {{"on", j.motor}, {"velocity", tidy(j.motorVelocity)}, {"limit", tidy(j.motorLimit)}};
        if (j.motorMode == projv::runtime::MotorMode::Position) {
            out["motor"]["mode"] = "position";
            out["motor"]["target"] = tidy(j.motorTarget);
        }
        if (j.motorFrequency > 0.0f) out["motor"]["frequency"] = tidy(j.motorFrequency);
    }
    if (j.breakForce > 0.0f) out["breakForce"] = tidy(j.breakForce);
    if (j.breakTorque > 0.0f) out["breakTorque"] = tidy(j.breakTorque);
    if (j.collideConnected) out["collideConnected"] = true;
    return out;
}

std::optional<projv::Joint> projv::runtime::ComponentTraits<projv::Joint>::load(const nlohmann::json& j, uint32_t) {
    using projv::runtime::JointType;
    projv::Joint out;
    std::string type = j.value("type", "point");
    if (type == "fixed") out.type = JointType::Fixed;
    else if (type == "point") out.type = JointType::Point;
    else if (type == "hinge") out.type = JointType::Hinge;
    else if (type == "slider") out.type = JointType::Slider;
    else if (type == "distance") out.type = JointType::Distance;
    else return std::nullopt;
    auto v3 = [](const nlohmann::json& a) { return projv::core::vec3(a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>()); };
    out.a = projv::NetId{j.at("a").get<uint32_t>()};
    if (out.a.value == 0) return std::nullopt;
    out.b = projv::NetId{j.value("b", 0u)};
    out.anchor = v3(j.at("anchor"));
    if (j.contains("anchorB")) out.anchorB = v3(j.at("anchorB"));
    if (j.contains("axis")) out.axis = v3(j.at("axis"));
    if (j.contains("limits")) { out.limitMin = j["limits"].at(0).get<float>(); out.limitMax = j["limits"].at(1).get<float>(); }
    if (j.contains("spring")) { out.springFrequency = j["spring"].at(0).get<float>(); out.springDamping = j["spring"].at(1).get<float>(); }
    out.friction = j.value("friction", 0.0f);
    if (j.contains("motor")) {
        const auto& m = j.at("motor");
        out.motor = m.value("on", false);
        out.motorVelocity = m.value("velocity", 0.0f);
        out.motorLimit = m.value("limit", 0.0f);
        std::string mode = m.value("mode", "velocity");
        if (mode == "position") out.motorMode = projv::runtime::MotorMode::Position;
        else if (mode != "velocity") return std::nullopt;
        out.motorTarget = m.value("target", 0.0f);
        out.motorFrequency = m.value("frequency", 0.0f);
    }
    out.breakForce = j.value("breakForce", 0.0f);
    out.breakTorque = j.value("breakTorque", 0.0f);
    out.collideConnected = j.value("collideConnected", false);
    return out;
}
