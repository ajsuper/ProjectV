#include "runtime/physics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <cstring>
#include <tuple>

#include <glm/gtc/quaternion.hpp>

#include "core/log.h"
#include "utils/loose_bvh.h"
#include "utils/scene_query.h"

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
        };

        PhysicsRuntime& runtimeOf(World& world) { return world.ctx().get<PhysicsRuntime>(); }

        void send(World& world, auto event) {
            if (Events* events = world.ctx().find<Events>()) events->send(std::move(event));
        }

        // ---- Signals: noted here, acted on at the next tick ----------------------------------------

        void wantBody(World& world, Entity entity) { runtimeOf(world).pending.push_back(entity); }
        void wantRebuild(World& world, Entity entity) { runtimeOf(world).rebuild.push_back(entity); }

        void bodyGone(World& world, Entity entity) {
            if (const PhysicsBody* body = world.try_get<PhysicsBody>(entity)) runtimeOf(world).doomed.push_back(body->id);
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
        std::vector<PhysicsShapePart> voxelParts(PhysicsWorld& physics, const utils::CollisionParams& params,
                                                 const Scene& scene, ComponentHandle h, core::vec3 position,
                                                 core::quat rotation, Box3& bounds) {
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
                parts.push_back(std::move(part));
            }
            return parts;
        }

        struct Description {
            BodyDesc   desc;
            const char* refusal = nullptr;
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
            if (linked) parts = voxelParts(*physics.world, physics.collision, *scene, link->handle, d.position,
                                           d.rotation, bounds);

            RigidBody::Shape kind = rigid ? rigid->shape : RigidBody::Shape::Voxels;
            if (kind == RigidBody::Shape::Voxels) {
                if (!linked) {
                    out.refusal = "a voxel shape needs the entity linked to a component";
                    return out;
                }
                if (parts.empty()) {
                    out.refusal = "its component has no voxels to collide with";
                    return out;
                }
                d.shape = parts.size() == 1 && parts[0].position == core::vec3(0.0f) &&
                                  parts[0].rotation == core::quat(1, 0, 0, 0)
                              ? parts[0].shape
                              : PhysicsShape::compound(std::move(parts));
            } else {
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

            // 2. Rebuilds: a changed RigidBody, a new or lost link.
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
                BodyId id;
                if (!described.refusal) id = sim.createBody(described.desc);
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
                world.emplace<PhysicsBody>(e, body);
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
                }
            }

            // 5. Kinematic bodies head for their Transform (a Part's from the Scene, where the bridge
            //    last put it).
            const Scene* scene = world.ctx().find<Scene>();
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
            physics.writingTransforms = true;
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

        installNetIds(world);
        registerComponent<RigidBody>(world);
        registerComponent<StaticCollider>(world);

        world.on_construct<RigidBody>().connect<&wantBody>();
        world.on_construct<StaticCollider>().connect<&wantBody>();
        world.on_update<RigidBody>().connect<&wantRebuild>();
        world.on_update<StaticCollider>().connect<&wantRebuild>();
        world.on_destroy<RigidBody>().connect<&bodyGone>();
        world.on_destroy<StaticCollider>().connect<&bodyGone>();
        world.on_destroy<PhysicsBody>().connect<&bodyGone>();
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
