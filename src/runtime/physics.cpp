#include "runtime/physics.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <map>
#include <cstring>
#include <fstream>
#include <tuple>

#include <glm/gtc/quaternion.hpp>

#include "core/log.h"
#include "runtime/loading.h"
#include "utils/loose_bvh.h"
#include "utils/scene_query.h"
#include "utils/picking.h"
#include "utils/editing.h"
#include "utils/voxel_math.h"

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
            uint32_t                      maxPieces = 3000;
            uint32_t                      dustVoxels = 0;
            PhysicsConfig::FractureLimits fractureLimits;
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
            uint64_t                      seenResidency = 0;   // blockResidencyCounter at the last check
            // Rebuilt because blocks came or went (world loading): with no voxels left loaded, such a
            // body waits, without complaint, for them to come back (waitingForBlocks).
            std::set<Entity>              streamed;
            std::set<Entity>              waitingForBlocks;
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
            // A voxel edit, or a strike (runtime::fracture): one queue, so they apply in the order issued.
            struct Edit {
                Entity entity;
                std::vector<PendingVoxelOp> ops;
                bool       strike = false;
                Strike     s;
                std::shared_ptr<VoxelSplit> split;   // applyVoxelSplit
                bool       replicated = false;       // applyVoxelEdit
                int        waited = 0;               // steps a split has waited for its body
            };
            std::vector<Edit>             edits;
            // A strike applied but not yet cut: which voxels go together, waiting for the island pass
            // (splitIslands) to cut them and make them bodies, with the body's other islands.
            struct Break {
                Strike s;
                std::vector<std::vector<FractureVoxel>> shards;   // in the struck component's voxels
                float edge = 0.0f, strength = 0.0f, metres = 0.0f;
            };
            std::map<Entity, Break>       breaking;
            // The physics material library: names in definition order (index = position + 1).
            struct Library {
                std::vector<std::string>        names;
                std::map<std::string, uint16_t> index;
                uint64_t                        version = 0;   // bumped by every definition
                std::set<std::string>           warned;        // unknown names, logged once each
            } library;
            // Collision group names (CollisionGroups, definePhysicsGroup): bit i is names[i].
            struct Groups {
                std::vector<std::string>        names;
                std::map<std::string, uint32_t> bit;
                uint64_t                        version = 0;   // bumped by every new name
                uint64_t                        seen = 0;      // the version at the last regroup
                std::set<std::string>           warned;        // unknown names, logged once each
            } groups;
            std::vector<Entity>           regroup;      // CollisionGroups added, changed or removed
            std::vector<Entity>           rereport;     // ContactReports added or removed
            // Contacts as entities see them (ContactBegan): each body pair the simulation reports
            // touching, and the entity pair it stands for; each entity pair, with how many body pairs
            // hold it (a body rebuilt under a contact is one ending and one beginning in a step) and
            // whether it was reported when it began, so its end is too.
            using OrderKey = std::tuple<bool, uint32_t, Entity>;
            struct EntityPair {
                int      bodies = 0;
                bool     reported = false;
                bool     sensor = false;
                OrderKey orderA, orderB;   // as they were when it began: an ended entity may be gone
            };
            std::map<uint64_t, std::pair<Entity, Entity>>         contactBodies;
            std::map<std::pair<Entity, Entity>, EntityPair>       contactEntities;
            std::vector<BodyContact>                              contacts;   // the last step's
            uint64_t                      seenPalettes = 0;  // palette versions and library at the last check
            std::set<std::string>         warnedPatterns;    // fracture patterns named but not registered
            std::set<Entity>              warnedUnanchored;  // colliders that split with nothing to stand on
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
        void wantRegroup(World& world, Entity entity) { runtimeOf(world).regroup.push_back(entity); }
        void wantReport(World& world, Entity entity) { runtimeOf(world).rereport.push_back(entity); }

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
                if (record.chunkHandle < scene.chunks.size()) out.push_back(record.chunkHandle);
            } else if (record.kind == ComponentKind::Grid && record.gridIndex >= 0 &&
                       size_t(record.gridIndex) < scene.grids.size()) {
                for (int32_t cell : scene.grids[size_t(record.gridIndex)].cellToChunk)
                    if (cell >= 0) out.push_back(ChunkHandle(cell));
            }
            for (ComponentHandle child : record.children)
                if (child < scene.components.size()) collectChunks(scene, child, out);
        }

        // ---- Materials ---------------------------------------------------------------------------

        struct Fnv {
            uint64_t value = 1469598103934665603ull;
            template <typename T> void add(const T& v) {
                const auto* p = reinterpret_cast<const uint8_t*>(&v);
                for (size_t i = 0; i < sizeof(T); i++) { value ^= p[i]; value *= 1099511628211ull; }
            }
        };

        // A library index by name: 0 for none, and for a name the library does not have (logged once).
        uint16_t materialNamed(PhysicsRuntime& physics, const std::string& name) {
            if (name.empty()) return 0;
            auto found = physics.library.index.find(name);
            if (found != physics.library.index.end()) return found->second;
            if (physics.library.warned.insert(name).second)
                core::warn("physics: no physics material '{}' in the library; using the body's own values", name);
            return 0;
        }

        // The component whose palette a chunk's voxels index: a grid's for its cells, the chunk's own
        // otherwise.
        ComponentHandle paletteOwner(const Scene& scene, const Chunk& chunk) {
            if (chunk.gridIndex >= 0 && size_t(chunk.gridIndex) < scene.grids.size())
                return scene.grids[size_t(chunk.gridIndex)].componentHandle;
            return chunk.componentHandle;
        }

        // Each palette slot's physics material. False -- and no map -- when no entry names one, so
        // the voxels collide as they always have.
        bool slotMaterialsOf(PhysicsRuntime& physics, const Scene& scene, ComponentHandle owner, utils::SlotMaterials& out) {
            if (owner >= scene.components.size()) return false;
            const std::vector<Material>& palette = scene.components[owner].materialPalette;
            out.fill(0);
            bool any = false;
            for (size_t slot = 0; slot < palette.size() && slot < out.size(); slot++) {
                out[slot] = materialNamed(physics, palette[slot].physics);
                any = any || out[slot] != 0;
            }
            return any;
        }

        // Group names as bits; a name not defined is left out, and said once.
        uint32_t groupBits(PhysicsRuntime& physics, const std::vector<std::string>& names) {
            uint32_t out = 0;
            for (const std::string& name : names) {
                auto found = physics.groups.bit.find(name);
                if (found != physics.groups.bit.end()) out |= found->second;
                else if (physics.groups.warned.insert(name).second)
                    core::warn("physics: collision group '{}' is not defined (definePhysicsGroup); left out", name);
            }
            return out;
        }

        // The filter an entity's CollisionGroups says (none: the default, in no group).
        CollisionFilter filterOf(const World& world, PhysicsRuntime& physics, Entity entity) {
            CollisionFilter f;
            const CollisionGroups* named = world.try_get<CollisionGroups>(entity);
            if (!named) return f;
            f.groups = groupBits(physics, named->groups);
            f.ignores = groupBits(physics, named->ignores);
            f.family = named->family;
            return f;
        }

        // A kinematic body static geometry stops (RigidBody::blockedByStatic), other than a Part.
        bool blockedKinematic(const World& world, Entity entity) {
            const RigidBody* rigid = world.try_get<RigidBody>(entity);
            if (!rigid || rigid->motion != MotionType::Kinematic || rigid->blockedByStatic == KinematicBlocking::None) return false;
            const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
            return !(link && link->mode == LinkMode::Part);
        }

        bool isSensor(const World& world, Entity entity) {
            const RigidBody* rigid = world.try_get<RigidBody>(entity);
            const StaticCollider* fixed = world.try_get<StaticCollider>(entity);
            return (rigid && rigid->layer == PhysicsLayer::Sensor) || (!rigid && fixed && fixed->layer == PhysicsLayer::Sensor);
        }

        // Whether `self` wants its contact with `other` reported: it asked (ContactReports), or is a
        // sensor, and `other` is in a group it named in `only` (or it named none).
        bool reportsContactWith(const World& world, PhysicsRuntime& physics, Entity self, Entity other) {
            if (!world.valid(self)) return false;
            const ContactReports* asked = world.try_get<ContactReports>(self);
            if (!asked) return isSensor(world, self);
            if (asked->only.empty()) return true;
            return world.valid(other) && (filterOf(world, physics, other).groups & groupBits(physics, asked->only)) != 0;
        }

        // The material a body's own component names (RigidBody::material, StaticCollider::material).
        uint16_t ownMaterial(World& world, PhysicsRuntime& physics, Entity entity) {
            if (const RigidBody* rigid = world.try_get<RigidBody>(entity)) return materialNamed(physics, rigid->material);
            if (const StaticCollider* fixed = world.try_get<StaticCollider>(entity)) return materialNamed(physics, fixed->material);
            return 0;
        }

        // What a body's materials are, as one number: 0 when it uses none; otherwise its own, every
        // palette under its voxels (when it is made of them) and the library's version.
        uint64_t materialKeyOf(PhysicsRuntime& physics, const Scene* scene, ComponentHandle h, bool voxels, uint16_t own) {
            Fnv key;
            bool any = own != 0;
            key.add(own);
            if (voxels && scene) {
                std::vector<ChunkHandle> chunks;
                collectChunks(*scene, h, chunks);
                ComponentHandle last = INVALID_COMPONENT_HANDLE;
                utils::SlotMaterials slots;
                for (ChunkHandle c : chunks) {
                    if (c >= scene->chunks.size() || !scene->chunks[c].alive) continue;
                    ComponentHandle owner = paletteOwner(*scene, scene->chunks[c]);
                    if (owner == last) continue;   // a grid's cells: one palette
                    last = owner;
                    if (!slotMaterialsOf(physics, *scene, owner, slots)) continue;
                    any = true;
                    key.add(owner);
                    key.add(slots);
                }
            }
            if (!any) return 0;
            key.add(physics.library.version);
            return key.value | 1u;
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

        // Voxels whose palette entry names a physics material are built with it; the rest weigh
        // `otherDensity`.
        std::vector<PhysicsShapePart> voxelParts(PhysicsRuntime& runtime, const Scene& scene, ComponentHandle h,
                                                 core::vec3 position, core::quat rotation, float otherDensity,
                                                 Box3& bounds, std::vector<PhysicsVoxelPart>& voxelOut) {
            PhysicsWorld& physics = *runtime.world;
            const utils::CollisionParams& params = runtime.collision;
            std::vector<ChunkHandle> chunks;
            collectChunks(scene, h, chunks);
            const core::quat inverse = glm::conjugate(rotation);
            std::vector<PhysicsShapePart> parts;
            ComponentHandle lastOwner = INVALID_COMPONENT_HANDLE;
            utils::SlotMaterials slots;
            bool hasSlots = false;
            for (ChunkHandle c : chunks) {
                if (c >= scene.chunks.size()) continue;
                const Chunk& chunk = scene.chunks[c];
                if (!chunk.alive || chunk.geometryPoolIndex < 0 ||
                    size_t(chunk.geometryPoolIndex) >= scene.geometryPool.size() || chunk.header.resolution == 0)
                    continue;
                const GeometryBlob& blob = scene.geometryPool[size_t(chunk.geometryPoolIndex)];
                ComponentHandle owner = paletteOwner(scene, chunk);
                if (owner != lastOwner || lastOwner == INVALID_COMPONENT_HANDLE) {
                    lastOwner = owner;
                    hasSlots = slotMaterialsOf(runtime, scene, owner, slots);
                }
                CollisionShapeRef shape = physics.voxelShape(blob, chunk.header.resolution, params,
                                                             hasSlots ? &slots : nullptr, otherDensity);
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
            uint64_t   materialKey = 0;
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

            // The body's own values: its material's when it names one, its fields' otherwise.
            const uint16_t own = ownMaterial(world, physics, entity);
            const PhysicsMaterial ownValues = physics.world->material(own);
            d.density = own ? ownValues.density : rigid ? rigid->density : d.density;
            d.friction = own ? ownValues.friction : rigid ? rigid->friction : fixed->friction;
            d.restitution = own ? ownValues.restitution : rigid ? rigid->restitution : fixed->restitution;

            RigidBody::Shape kind = rigid ? rigid->shape : RigidBody::Shape::Voxels;
            Box3 bounds;
            std::vector<PhysicsShapePart> parts;
            if (linked) {
                parts = voxelParts(physics, *scene, link->handle, d.position, d.rotation, d.density, bounds, out.voxelParts);
                out.sources = sourcesOf(*scene, link->handle);
            }
            out.materialKey = materialKeyOf(physics, scene, linked ? link->handle : INVALID_COMPONENT_HANDLE,
                                            linked && kind == RigidBody::Shape::Voxels, own);

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
                d.mass = rigid->mass;
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
                d.senses = rigid->senses;
                // A Part's pose is the Scene's: nothing may stop it short of where that puts it.
                if (!(link && link->mode == LinkMode::Part)) d.blockedByStatic = rigid->blockedByStatic;
            } else {
                d.layer = fixed->layer;
                d.senses = fixed->senses;
            }
            d.minImpactSpeed = rigid ? rigid->minImpactSpeed : fixed->minImpactSpeed;
            d.filter = filterOf(world, physics, entity);
            d.reportContacts = world.all_of<ContactReports>(entity);
            return out;
        }

        // ---- The FixedUpdate system ----------------------------------------------------------------

        // Whether the entity's linked component, or anything under it, has blocks not loaded.
        bool unloadedUnder(const World& world, Entity entity) {
            const Scene* scene = world.ctx().find<Scene>();
            const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
            if (!scene || !link || !utils::isComponentAlive(*scene, link->ref())) return false;
            std::vector<ComponentHandle> stack{link->handle};
            while (!stack.empty()) {
                ComponentHandle h = stack.back();
                stack.pop_back();
                if (utils::hasUnloadedBlocks(*scene, h)) return true;
                for (ComponentHandle child : scene->components[h].children) stack.push_back(child);
            }
            return false;
        }

        bool wantsBody(const World& world, Entity entity) {
            return world.valid(entity) && world.any_of<RigidBody, StaticCollider>(entity);
        }

        void dropBody(World& world, PhysicsRuntime& physics, Entity entity) {
            if (const PhysicsBody* body = world.try_get<PhysicsBody>(entity)) {
                physics.world->destroyBody(body->id);
                world.remove<PhysicsBody>(entity);
            }
        }

        // The voxel chunk a body is edited and broken through: the linked component itself when it is
        // a chunk, or the one chunk inside an asset (a prefab). An asset of several chunks has none
        // (voxelComponentOf takes grids as they are).
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

        // ---- Fracture -------------------------------------------------------------------------

        // The chunks under a voxel component (a chunk, or a grid's cells), each with where its voxels
        // start in the component's voxel coordinates, and the component's voxel size.
        struct VoxelSource {
            struct Part { size_t chunk; core::ivec3 offset; uint32_t resolution; };
            std::vector<Part> parts;
            float voxel = 0.0f;
        };
        VoxelSource voxelSourceOf(const Scene& scene, ComponentHandle h) {
            VoxelSource source;
            const ComponentRecord& record = scene.components[h];
            if (record.kind == ComponentKind::Chunk) {
                if (record.chunkHandle >= scene.chunks.size()) return source;
                const Chunk& chunk = scene.chunks[record.chunkHandle];
                uint32_t resolution = std::max(chunk.header.resolution, 1u);
                source.voxel = chunk.nativeScale / float(resolution);
                source.parts.push_back({size_t(record.chunkHandle), core::ivec3(0), resolution});
            } else if (record.gridIndex >= 0 && size_t(record.gridIndex) < scene.grids.size()) {
                const SceneGrid& grid = scene.grids[size_t(record.gridIndex)];
                for (size_t lin = 0; lin < grid.cellToChunk.size(); lin++) {
                    int32_t c = grid.cellToChunk[lin];
                    if (c < 0 || size_t(c) >= scene.chunks.size() || !scene.chunks[size_t(c)].alive) continue;
                    uint32_t resolution = std::max(scene.chunks[size_t(c)].header.resolution, 1u);
                    int x = int(lin % size_t(grid.dims.x)), y = int((lin / size_t(grid.dims.x)) % size_t(grid.dims.y));
                    int z = int(lin / (size_t(grid.dims.x) * size_t(grid.dims.y)));
                    core::ivec3 cell = grid.originCellCoord + core::ivec3(x, y, z);
                    source.parts.push_back({size_t(c), cell * int(resolution), resolution});
                    source.voxel = grid.nativeCellSize / float(resolution);
                }
            }
            return source;
        }

        // The voxels of `source` whose centres lie within `radius` of `at` (both in voxels), with their
        // palette slots, in z, y, x order. Chunks are decoded once and kept in `decoded`.
        struct Decoded { utils::VoxelOccupancy occupancy; std::vector<uint8_t> slots; bool ok = false; };
        std::vector<FractureVoxel> voxelsNear(const Scene& scene, const VoxelSource& source, core::vec3 at, float radius,
                                              uint64_t maxCells, std::map<size_t, Decoded>& decoded) {
            std::vector<FractureVoxel> out;
            const core::ivec3 low(glm::floor(at - radius)), high(glm::ceil(at + radius));
            for (const VoxelSource::Part& part : source.parts) {
                core::ivec3 partHigh = part.offset + core::ivec3(int(part.resolution) - 1);
                if (glm::any(glm::lessThan(partHigh, low)) || glm::any(glm::greaterThan(part.offset, high))) continue;
                auto [it, fresh] = decoded.try_emplace(part.chunk);
                Decoded& d = it->second;
                if (fresh) {
                    const Chunk& chunk = scene.chunks[part.chunk];
                    if (chunk.geometryPoolIndex >= 0 && size_t(chunk.geometryPoolIndex) < scene.geometryPool.size())
                        d.ok = utils::decodeVoxelSlots(scene.geometryPool[size_t(chunk.geometryPoolIndex)], part.resolution,
                                                       maxCells, d.occupancy, d.slots) == utils::DecodeResult::Ok;
                }
                if (!d.ok) continue;
                const utils::VoxelOccupancy& o = d.occupancy;
                core::ivec3 from = glm::max(low - part.offset - o.origin, core::ivec3(0));
                core::ivec3 to = glm::min(high - part.offset - o.origin, o.dims - core::ivec3(1));
                for (int z = from.z; z <= to.z; z++)
                    for (int y = from.y; y <= to.y; y++)
                        for (int x = from.x; x <= to.x; x++) {
                            size_t i = o.index({x, y, z});
                            if (!o.cells[i]) continue;
                            core::ivec3 p = part.offset + o.origin + core::ivec3(x, y, z);
                            if (glm::distance(core::vec3(p) + core::vec3(0.5f), at) > radius) continue;
                            FractureVoxel v;
                            v.position = p;
                            v.slot = d.slots[i];
                            out.push_back(v);
                        }
            }
            std::sort(out.begin(), out.end(), [](const FractureVoxel& a, const FractureVoxel& b) {
                return std::make_tuple(a.position.z, a.position.y, a.position.x) < std::make_tuple(b.position.z, b.position.y, b.position.x);
            });
            return out;
        }


        // A new top-level component holding `members` of `voxels` with their palette entries (the
        // body's palette, copied), placed where they were: its corner at the lowest of them, through
        // `toWorld`. One chunk of the least resolution that holds them when `oneChunk` (shards, which
        // fit by construction); otherwise a 16-voxel chunk that becomes a grid as it outgrows it.
        // Its edits are queued, not applied. `centre`: the voxels' middle, in the world.
        ComponentHandle pieceOf(Scene& scene, const std::vector<Material>& palette, const std::vector<FractureVoxel>& voxels,
                                const std::vector<size_t>& members, const core::mat4& toWorld, core::quat rotation, float scale,
                                float voxel, const std::string& name, bool oneChunk, core::vec3& centre) {
            core::ivec3 lo(std::numeric_limits<int>::max()), hi(std::numeric_limits<int>::min());
            core::vec3 sum(0.0f);
            for (size_t i : members) {
                lo = glm::min(lo, voxels[i].position);
                hi = glm::max(hi, voxels[i].position);
                sum += core::vec3(voxels[i].position) + 0.5f;
            }
            const core::ivec3 size = hi - lo + core::ivec3(1);
            const int extent = std::max(size.x, std::max(size.y, size.z));
            uint32_t resolution = oneChunk ? 4 : 16;
            while (oneChunk && int(resolution) < extent) resolution *= 4;
            ComponentHandle piece = utils::addComponent(scene, ComponentKind::Chunk, name, INVALID_COMPONENT_HANDLE, resolution, voxel);
            if (piece == INVALID_COMPONENT_HANDLE) return piece;
            scene.components[piece].materialPalette = palette;
            scene.components[piece].paletteVersion++;
            core::vec3 corner = core::vec3(toWorld * core::vec4(core::vec3(lo) * voxel, 1.0f));
            utils::setComponentTransform(scene, piece, corner, rotation, scale);
            std::vector<PendingVoxelOp> adds;
            adds.reserve(members.size());
            for (size_t i : members) {
                const FractureVoxel& v = voxels[i];
                uint32_t colour = v.slot < palette.size() ? palette[v.slot].packedColor : 0u;
                adds.push_back({true, v.position - lo, colour, v.slot});
            }
            utils::queueVoxelAdd(scene, piece, adds);
            centre = core::vec3(toWorld * core::vec4(sum / float(members.size()) * voxel, 1.0f));
            return piece;
        }

        // Whether anything in a voxel body can break: its own material, or one its palette names, has
        // a strength. What keeps every landing on a floor from looking at the floor's voxels.
        bool canBreak(World& world, PhysicsRuntime& physics, const Scene& scene, Entity entity) {
            const PhysicsWorld& sim = *physics.world;
            if (uint16_t own = ownMaterial(world, physics, entity); own && sim.material(own).strength > 0.0f) return true;
            ComponentHandle h = voxelComponentOf(world, entity);
            if (h == INVALID_COMPONENT_HANDLE) return false;
            const ComponentRecord& record = scene.components[h];
            ComponentHandle owner = record.kind == ComponentKind::Chunk && record.chunkHandle < scene.chunks.size()
                                        ? paletteOwner(scene, scene.chunks[record.chunkHandle])
                                        : h;
            if (owner >= scene.components.size()) return false;
            for (const Material& entry : scene.components[owner].materialPalette)
                if (!entry.physics.empty()) {
                    auto found = physics.library.index.find(entry.physics);
                    if (found != physics.library.index.end() && sim.material(found->second).strength > 0.0f) return true;
                }
            return false;
        }

        // A strike, applied: see runtime::fracture. It cuts nothing itself. It works out which voxels
        // go together -- the struck material's pattern, held to the engine's rules -- and leaves them
        // for the island pass (splitIslands), which cuts them away with whatever else comes loose.
        // False when nothing breaks.
        bool breakBody(World& world, PhysicsRuntime& physics, Scene& scene, Entity entity, const Strike& strike,
                       PhysicsRuntime::Break& out) {
            if (!world.valid(entity)) return false;
            const PhysicsBody* body = world.try_get<PhysicsBody>(entity);
            const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
            const RigidBody* rigid = world.try_get<RigidBody>(entity);
            const StaticCollider* fixed = world.try_get<StaticCollider>(entity);
            if (!body || !link || (!rigid && !fixed) || (rigid && rigid->shape != RigidBody::Shape::Voxels)) {
                core::trace("physics: fracture of entity {} skipped: not a voxel body", static_cast<uint32_t>(entity));
                return false;
            }
            const ComponentHandle h = voxelComponentOf(world, entity);
            if (h == INVALID_COMPONENT_HANDLE) {
                core::trace("physics: fracture of entity {} skipped: not a chunk, a grid or a one-chunk prefab",
                            static_cast<uint32_t>(entity));
                return false;
            }
            const VoxelSource source = voxelSourceOf(scene, h);
            if (source.parts.empty() || !(source.voxel > 0.0f)) return false;
            const core::mat4 toWorld = utils::getComponentWorldMatrix(scene, h);
            const float scale = glm::length(core::vec3(toWorld[0]));
            const float metres = source.voxel * scale;   // one voxel, in the world
            if (!(metres > 0.0f)) return false;
            const core::vec3 at = core::vec3(glm::inverse(toWorld) * core::vec4(strike.at, 1.0f)) / source.voxel;

            // Materials: each palette slot's, the body's own where a slot names none.
            const ComponentHandle owner = paletteOwner(scene, scene.chunks[source.parts.front().chunk]);
            utils::SlotMaterials slotMaterials;
            slotMaterialsOf(physics, scene, owner, slotMaterials);
            const uint16_t own = ownMaterial(world, physics, entity);
            const PhysicsWorld& sim = *physics.world;
            auto materialOf = [&](uint8_t slot) { return slotMaterials[slot] ? slotMaterials[slot] : own; };
            auto breakable = [&](uint16_t m) { return m != 0 && sim.material(m).strength > 0.0f; };

            // What was struck: the voxel nearest the point.
            std::map<size_t, Decoded> decoded;
            const uint64_t maxCells = physics.collision.maxDecodedCells;
            const PhysicsConfig::FractureLimits& limits = physics.fractureLimits;
            std::vector<FractureVoxel> near = voxelsNear(scene, source, at, limits.searchVoxels, maxCells, decoded);
            if (near.empty()) {
                core::trace("physics: fracture of entity {}: no voxel within {} of the strike", static_cast<uint32_t>(entity),
                            limits.searchVoxels);
                return false;
            }
            const FractureVoxel* struck = &near.front();
            for (const FractureVoxel& v : near)
                if (glm::distance(core::vec3(v.position) + 0.5f, at) < glm::distance(core::vec3(struck->position) + 0.5f, at))
                    struck = &v;
            const uint16_t struckMaterial = materialOf(struck->slot);
            if (!breakable(struckMaterial)) {
                core::trace("physics: fracture of entity {}: what was struck does not break", static_cast<uint32_t>(entity));
                return false;
            }
            const PhysicsMaterial material = sim.material(struckMaterial);
            const float shardMetres = strike.shardSize > 0.0f ? strike.shardSize : material.shardSize;
            float reach = 0.0f;
            if (strike.reach > 0.0f) reach = strike.reach / metres;
            else if (strike.energy > 0.0f) reach = std::cbrt(3.0f * strike.energy / (4.0f * 3.14159265f * material.strength)) / metres;
            else return false;
            if (reach > limits.maxReachVoxels) {
                core::trace("physics: fracture of entity {}: reach {} voxels capped at {}", static_cast<uint32_t>(entity), reach,
                            limits.maxReachVoxels);
                reach = limits.maxReachVoxels;
            }
            if (reach < 0.5f) return false;   // not enough to break a voxel
            if (strike.minimumBreak > 0.0f && reach < strike.minimumBreak * shardMetres / metres) return false;   // a knock
            if (strike.cause == BreakCause::Impact) {
                // The least an impact breaks; and a body that fits in a cube of it is too small for a
                // hit to break at all -- what keeps pieces that break on impact from cascading.
                const float least = limits.minImpactBreak * shardMetres / metres;
                if (reach < least) return false;
                core::ivec3 lo(std::numeric_limits<int>::max()), hi(std::numeric_limits<int>::min());
                for (const VoxelSource::Part& part : source.parts) {
                    const Chunk& chunk = scene.chunks[part.chunk];
                    core::ivec3 l, u;
                    if (chunk.geometryPoolIndex >= 0 && utils::blobContentBounds(scene.geometryPool[size_t(chunk.geometryPoolIndex)], l, u)) {
                        lo = glm::min(lo, part.offset + l);
                        hi = glm::max(hi, part.offset + u);
                    }
                }
                if (lo.x <= hi.x && glm::all(glm::lessThanEqual(core::vec3(hi - lo + core::ivec3(1)), core::vec3(least)))) return false;
            }
            const float outer = 1.5f * reach;

            // Each voxel breaks only within the reach the energy has in its own material: steel in
            // concrete snaps near the blow, where the concrete round it goes much further. (A strike
            // by reach breaks everything breakable in it, whatever it is made of.)
            std::map<uint16_t, float> ownReach;
            auto reachIn = [&](uint16_t m) {
                auto [it, fresh] = ownReach.try_emplace(m, reach);
                if (fresh && strike.reach <= 0.0f)
                    it->second = std::min(std::cbrt(3.0f * strike.energy / (4.0f * 3.14159265f * sim.material(m).strength)) / metres,
                                          limits.maxReachVoxels);
                return it->second;
            };
            std::vector<FractureVoxel> voxels = voxelsNear(scene, source, at, outer, maxCells, decoded);
            for (FractureVoxel& v : voxels) {
                v.material = materialOf(v.slot);
                v.breakable = breakable(v.material) &&
                              glm::distance(core::vec3(v.position) + 0.5f, at) <= reachIn(v.material);
            }
            const FracturePattern* pattern = fracturePattern(world, material.fracture);
            if (!pattern) {
                if (physics.warnedPatterns.insert(material.fracture).second)
                    core::warn("physics: no fracture pattern '{}' is registered; breaking as \"shatter\"", material.fracture);
                pattern = fracturePattern(world, "shatter");
                if (!pattern) return false;
            }
            const NetId* net = world.try_get<NetId>(entity);
            Fnv seed;
            seed.add(net ? net->value : 0u);
            seed.add(sim.tick());
            seed.add(strike.at);
            seed.add(strike.energy);
            if (strike.reach > 0.0f) seed.add(strike.reach);
            if (strike.shardSize > 0.0f) seed.add(strike.shardSize);
            FractureRandom random(seed.value);
            const FractureInput input{voxels, at, reach, outer, shardMetres / metres, material.grain, material};
            std::vector<int32_t> shardOf = (*pattern)(input, random);
            if (shardOf.size() != voxels.size()) {
                core::warn("physics: fracture pattern '{}' answered for {} voxels of {}; nothing breaks", material.fracture,
                           shardOf.size(), voxels.size());
                return false;
            }
            std::vector<std::vector<size_t>> shards = fractureShards(input, shardOf, 64, limits.maxShards);
            if (shards.empty()) return false;

            out.s = strike;
            out.shards.clear();
            out.shards.reserve(shards.size());
            for (const std::vector<size_t>& shard : shards) {
                std::vector<FractureVoxel> members;
                members.reserve(shard.size());
                for (size_t i : shard) members.push_back(voxels[i]);
                out.shards.push_back(std::move(members));
            }
            out.edge = outer * metres;
            out.strength = material.strength;
            out.metres = metres;
            core::trace("physics: entity {} struck: {} shards, reach {:.1f} voxels", static_cast<uint32_t>(entity), shards.size(),
                        reach);
            return true;
        }

        // ---- Islands ----------------------------------------------------------------------------

        constexpr uint64_t MAX_ISLAND_CELLS = 1ull << 24;   // the box islands are worked out in, at most

        // Every voxel under a source, counted.
        uint64_t voxelCount(const Scene& scene, const VoxelSource& source, uint64_t maxCells) {
            uint64_t total = 0;
            for (const VoxelSource::Part& part : source.parts) {
                utils::VoxelOccupancy o;
                const Chunk& chunk = scene.chunks[part.chunk];
                if (chunk.geometryPoolIndex >= 0 &&
                    utils::decodeOccupancy(scene.geometryPool[size_t(chunk.geometryPoolIndex)], part.resolution, maxCells, o) ==
                        utils::DecodeResult::Ok)
                    total += o.solid;
            }
            return total;
        }

        // An island to cut away (cutIslands).
        struct Cut {
            std::vector<size_t>                    members;   // indices into the voxels
            bool                                   shard = false;
            std::optional<NetId>                   netId;     // given (a server's), or assigned as usual
            std::optional<PhysicsRuntime::Motion>  motion;    // given, or the body's at its centre
        };

        // Cuts `cuts` -- each a list of indices into `voxels`, the body's voxels in its component's
        // voxel space -- out of the body, each a component and body of its own (its NetId and motion
        // when given, the body's otherwise), and `dustOnly` with nothing made of it. Sends BodyBroken.
        // The second half of the island pass (splitIslands), and all of applyVoxelSplit.
        void cutIslands(World& world, PhysicsRuntime& physics, Scene& scene, Entity entity, ComponentHandle h,
                        const VoxelSource& source, const std::vector<FractureVoxel>& voxels, std::vector<Cut> cuts,
                        uint64_t total, const BodyState& state, const PhysicsRuntime::Break* strike, BreakCause cause,
                        const std::vector<size_t>& dustOnly = {}) {
            const RigidBody* rigid = world.try_get<RigidBody>(entity);
            const StaticCollider* fixed = rigid ? nullptr : world.try_get<StaticCollider>(entity);
            const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
            const PhysicsBody* body = world.try_get<PhysicsBody>(entity);
            if (!link || !body || (!rigid && !fixed) || source.parts.empty()) return;
            const ComponentHandle owner = paletteOwner(scene, scene.chunks[source.parts.front().chunk]);
            const bool moves = body->motion == MotionType::Dynamic;
            const float parentMass = rigid ? rigid->mass : 0.0f;
            RigidBody pieceRigid;
            if (rigid) {
                pieceRigid = *rigid;
                pieceRigid.motion = MotionType::Dynamic;
            } else {
                pieceRigid.friction = fixed->friction;
                pieceRigid.restitution = fixed->restitution;
                pieceRigid.material = fixed->material;
                pieceRigid.breaksOnImpact = fixed->breaksOnImpact;
                pieceRigid.minImpactSpeed = fixed->minImpactSpeed;
            }
            const OnUnlink onUnlink = link->onUnlink;
            const bool networked = world.all_of<NetId>(entity);


            // Cut every island out; each but the dust becomes a component of its own.
            const core::mat4 toWorld = utils::getComponentWorldMatrix(scene, h);
            const float scale = glm::length(core::vec3(toWorld[0]));
            const core::quat rotation = utils::getComponentWorldRotation(scene, h);
            const std::vector<Material> palette = scene.components[owner].materialPalette;
            const std::string name = scene.components[h].name;
            struct Made { ComponentHandle component; core::vec3 centre; size_t voxels; bool shard; const Cut* cut; };
            std::vector<Made> made;
            std::vector<PendingVoxelOp> cut;
            uint64_t gone = 0, broken = 0, dust = 0;
            std::vector<core::ivec3> dustVoxels;
            for (const Cut& island : cuts) {
                for (size_t i : island.members) cut.push_back({false, voxels[i].position, 0});
                gone += island.members.size();
                if (island.shard) broken += island.members.size();
                // Dust: what the island pass judged too small to be a body. Pieces sent by a server
                // (a NetId given) are made whatever their size: it made them.
                if (!island.netId && island.members.size() <= physics.dustVoxels) {
                    dust++;
                    for (size_t i : island.members) dustVoxels.push_back(voxels[i].position);
                    continue;
                }
                if (island.members.empty()) continue;
                core::vec3 centre;
                ComponentHandle piece = pieceOf(scene, palette, voxels, island.members, toWorld, rotation, scale, source.voxel,
                                                name + (island.shard ? " shard" : " piece"), island.shard, centre);
                if (piece == INVALID_COMPONENT_HANDLE) continue;
                made.push_back({piece, centre, island.members.size(), island.shard, &island});
            }
            for (size_t i : dustOnly) {
                cut.push_back({false, voxels[i].position, 0});
                dustVoxels.push_back(voxels[i].position);
                gone++;
            }
            utils::queueVoxelRemove(scene, h, cut);
            utils::updateScene(scene);

            BodyBroken event;
            event.entity = entity;
            event.cause = cause;
            event.dust = std::move(dustVoxels);
            if (strike) {
                event.at = strike->s.at;
                event.striker = strike->s.striker;
                event.energy = strike->s.energy;
            }
            const uint64_t tick = physics.world->tick();
            const float share = total > 0 ? 1.0f / float(total) : 0.0f;
            const CollisionGroups* parentGroups = world.try_get<CollisionGroups>(entity);
            const std::optional<CollisionGroups> groups = parentGroups ? std::optional(*parentGroups) : std::nullopt;
            for (const Made& m : made) {
                Entity e = world.create();
                if (!linkComponent(world, e, m.component, LinkMode::Root, onUnlink)) {
                    world.destroy(e);
                    continue;
                }
                RigidBody r = pieceRigid;
                if (parentMass > 0.0f) r.mass = std::max(parentMass * float(m.voxels) * share, 1e-3f);
                // Its motion: the body's at its centre -- a shard's from the motion the strike says (an
                // impact's: the body's before the hit, which the contact has since stopped), plus the push.
                core::vec3 velocity = state.linearVelocity;
                if (m.shard && strike && strike->s.bodyVelocity) velocity = *strike->s.bodyVelocity;
                velocity += glm::cross(state.angularVelocity, m.centre - state.centerOfMass);
                if (m.shard && strike) {
                    float falloff = std::max(0.0f, 1.0f - glm::distance(m.centre, strike->s.at) / std::max(strike->edge, 1e-6f));
                    velocity += strike->s.push * falloff;
                }
                PhysicsRuntime::Motion motion{velocity, state.angularVelocity};
                if (m.cut->motion) motion = *m.cut->motion;
                physics.carried[e] = motion;
                if (m.cut->netId) {
                    if (entityForNetId(world, *m.cut->netId) != NullEntity)
                        core::warn("physics: a piece's NetId {} is taken; it is given another", m.cut->netId->value);
                    else
                        world.emplace<NetId>(e, *m.cut->netId);
                }
                if (networked && !world.all_of<NetId>(e)) assignNetId(world, e);
                world.emplace<Piece>(e, Piece{tick});
                world.emplace<RigidBody>(e, r);
                if (groups) world.emplace<CollisionGroups>(e, *groups);
                event.pieces.push_back(e);
                event.pieceMotion.push_back({motion.linear, motion.angular});
                std::vector<core::ivec3>& placed = event.pieceVoxels.emplace_back();
                for (size_t i : m.cut->members) placed.push_back(voxels[i].position);
                if (m.shard) event.shards++;
            }
            if (parentMass > 0.0f) {
                float kept = parentMass * float(total - std::min(gone, total)) * share;
                world.patch<RigidBody>(entity, [kept](RigidBody& rb) { rb.mass = std::max(kept, 1e-3f); });
            }
            // What struck something that does not move carries on with the energy the break did not
            // use: breaking a voxel costs the struck material's strength times its volume.
            if (strike && strike->s.returnUnusedEnergy && strike->s.energy > 0.0f && !moves && world.valid(strike->s.striker)) {
                if (const PhysicsBody* striker = world.try_get<PhysicsBody>(strike->s.striker);
                    striker && striker->motion == MotionType::Dynamic) {
                    const float used = strike->strength * float(broken) * strike->metres * strike->metres * strike->metres;
                    const float kept = std::sqrt(std::clamp(1.0f - used / strike->s.energy, 0.0f, 1.0f));
                    BodyState now = physics.world->bodyState(striker->id);
                    physics.world->setVelocity(striker->id, strike->s.strikerVelocity * kept, now.angularVelocity);
                }
            }
            core::trace("physics: {} pieces came away from entity {} ({} voxels, {} of them dust islands)", event.pieces.size(),
                        static_cast<uint32_t>(entity), gone, dust);
            // Delivered now: a handler shapes the pieces before their bodies are made.
            if (Events* events = world.ctx().find<Events>()) events->trigger(event);
        }

        // applyVoxelSplit, at the step: the pieces cut as given, then the body rebuilt with no island
        // pass of its own (its sources are taken as they now are).
        void applySplit(World& world, PhysicsRuntime& physics, Scene& scene, Entity entity, const VoxelSplit& split) {
            const ComponentHandle h = voxelComponentOf(world, entity);
            if (h == INVALID_COMPONENT_HANDLE) return;
            const VoxelSource source = voxelSourceOf(scene, h);
            if (source.parts.empty() || !(source.voxel > 0.0f)) return;
            core::ivec3 low(std::numeric_limits<int>::max()), high(std::numeric_limits<int>::min());
            auto widen = [&](core::ivec3 p) { low = glm::min(low, p); high = glm::max(high, p); };
            for (const VoxelSplit::Piece& piece : split.pieces) for (core::ivec3 p : piece.voxels) widen(p);
            for (core::ivec3 p : split.dust) widen(p);
            if (low.x > high.x) return;
            std::map<size_t, Decoded> decoded;
            const core::vec3 middle = 0.5f * core::vec3(low + high + core::ivec3(1));
            const float radius = glm::length(core::vec3(high - low + core::ivec3(1))) * 0.5f + 1.0f;
            const std::vector<FractureVoxel> voxels = voxelsNear(scene, source, middle, radius, physics.collision.maxDecodedCells, decoded);
            std::map<std::tuple<int, int, int>, size_t> at;
            for (size_t i = 0; i < voxels.size(); i++) at[{voxels[i].position.x, voxels[i].position.y, voxels[i].position.z}] = i;
            size_t missing = 0;
            auto find = [&](core::ivec3 p, std::vector<size_t>& out) {
                auto it = at.find({p.x, p.y, p.z});
                if (it == at.end()) missing++;
                else out.push_back(it->second);
            };
            std::vector<Cut> cuts;
            for (const VoxelSplit::Piece& piece : split.pieces) {
                Cut cut;
                for (core::ivec3 p : piece.voxels) find(p, cut.members);
                std::sort(cut.members.begin(), cut.members.end());
                cut.shard = piece.shard;
                cut.netId = piece.netId.value ? std::optional(piece.netId) : std::nullopt;
                cut.motion = PhysicsRuntime::Motion{piece.linear, piece.angular};
                cuts.push_back(std::move(cut));
            }
            std::vector<size_t> dust;
            for (core::ivec3 p : split.dust) find(p, dust);
            if (missing)
                core::warn("physics: a split of entity {} named {} voxels it does not have; they are skipped",
                           static_cast<uint32_t>(entity), missing);

            PhysicsBody& body = world.get<PhysicsBody>(entity);
            BodyState state = physics.world->bodyState(body.id);
            if (body.motion == MotionType::Dynamic && !physics.carried.count(entity))
                physics.carried[entity] = {state.linearVelocity, state.angularVelocity};
            const RigidBody* rigid = world.try_get<RigidBody>(entity);
            const uint64_t total = rigid && rigid->mass > 0.0f ? voxelCount(scene, source, physics.collision.maxDecodedCells) : 0;
            cutIslands(world, physics, scene, entity, h, source, voxels, std::move(cuts), total, state, nullptr,
                       BreakCause::Replicated, dust);
            // Rebuilt for its new shape, and no island pass: the server's split is the whole change.
            if (world.valid(entity) && world.all_of<PhysicsBody>(entity)) {
                world.get<PhysicsBody>(entity).sources = sourcesOf(scene, h);
                physics.rebuild.push_back(entity);
                physics.reshaped.insert(entity);
            }
        }

        // The one pass, run after a voxel body's voxels changed for whatever reason -- an edit, a strike
        // (`strike`, its shards not yet cut), support lost: its face-connected islands, across all its
        // chunks (a grid's cells too), each stay or come away as a body of their own (BodyBroken). A
        // strike's shards are islands of their own (voxels of two shards do not join), and always come
        // away. Of the rest:
        // - a moving body keeps its largest island, the first in voxel order of equals;
        // - a static collider that splits (StaticCollider::splits) keeps the islands held up: voxels of
        //   an anchor material (PhysicsMaterial::anchor), and with StaticCollider::anchorBase the
        //   component's own y = 0 and below. A collider with nothing that could hold it drops nothing
        //   (and says so, once);
        // - anything else (a static collider that does not split, a kinematic body) keeps it all.
        // Every piece takes after the body (see BodyBroken), is a Piece, and moves as that part of the
        // body was (`state`, from before the change) -- a shard plus the strike's push, fading to
        // nothing at the edge of the break. An island of PhysicsConfig::dustVoxels or fewer is dust:
        // cut, and nothing made of it. Applied (updateScene) before returning.
        void splitIslands(World& world, PhysicsRuntime& physics, Scene& scene, Entity entity, const BodyState& state,
                          const PhysicsRuntime::Break* strike) {
            const RigidBody* rigid = world.try_get<RigidBody>(entity);
            const StaticCollider* fixed = rigid ? nullptr : world.try_get<StaticCollider>(entity);
            const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
            const PhysicsBody* body = world.try_get<PhysicsBody>(entity);
            if (!link || !body || (!rigid && !fixed)) return;
            if (rigid && rigid->shape != RigidBody::Shape::Voxels) return;
            enum class Rest { Kept, Largest, Anchored } rest = Rest::Kept;
            if (rigid && body->motion == MotionType::Dynamic && link->mode == LinkMode::Root) rest = Rest::Largest;
            else if (fixed && fixed->splits) rest = Rest::Anchored;
            if (rest == Rest::Kept && !strike) return;
            const ComponentHandle h = voxelComponentOf(world, entity);
            if (h == INVALID_COMPONENT_HANDLE) return;
            // Part of it not loaded (world loading): what is not here cannot be weighed, so a moving
            // body keeps all it has, and a static one counts what is not loaded as holding it up.
            const bool partlyLoaded = utils::hasUnloadedBlocks(scene, h);
            if (partlyLoaded && rest == Rest::Largest) rest = Rest::Kept;
            if (rest == Rest::Kept && !strike) return;
            const VoxelSource source = voxelSourceOf(scene, h);
            if (source.parts.empty() || !(source.voxel > 0.0f)) return;
            const ComponentHandle owner = paletteOwner(scene, scene.chunks[source.parts.front().chunk]);
            const bool anchorBase = fixed && fixed->anchorBase;
            const float parentMass = rigid ? rigid->mass : 0.0f;

            // The voxels, each with its group: 0 the rest, k + 1 the strike's shard k. When the rest
            // stays whatever happens, or the body is too big to look over whole, only the shards are
            // read -- each already one face-connected island.
            std::vector<FractureVoxel> voxels;
            std::vector<uint32_t> group;
            struct Island { std::vector<size_t> members; uint32_t group; };
            std::vector<Island> islands;
            uint64_t total = 0;   // voxels in the body before the cut
            bool whole = rest != Rest::Kept;
            core::ivec3 low(std::numeric_limits<int>::max()), high(std::numeric_limits<int>::min());
            for (const VoxelSource::Part& part : source.parts) {
                low = glm::min(low, part.offset);
                high = glm::max(high, part.offset + core::ivec3(int(part.resolution) - 1));
            }
            const core::ivec3 dims = high - low + core::ivec3(1);
            const uint64_t cells = uint64_t(dims.x) * uint64_t(dims.y) * uint64_t(dims.z);
            if (whole && cells > MAX_ISLAND_CELLS) {
                if (!strike) {
                    core::warn("physics: entity {} is too big ({} cells) to find its islands; nothing comes away",
                               static_cast<uint32_t>(entity), cells);
                    return;
                }
                core::trace("physics: entity {} is too big ({} cells) to find its islands; only the strike's shards come away",
                            static_cast<uint32_t>(entity), cells);
                whole = false;
            }
            if (!whole) {
                for (size_t k = 0; k < strike->shards.size(); k++) {
                    Island island{{}, uint32_t(k + 1)};
                    for (const FractureVoxel& v : strike->shards[k]) {
                        island.members.push_back(voxels.size());
                        voxels.push_back(v);
                        group.push_back(uint32_t(k + 1));
                    }
                    islands.push_back(std::move(island));
                }
                if (parentMass > 0.0f) total = voxelCount(scene, source, physics.collision.maxDecodedCells);
            } else {
                std::map<size_t, Decoded> decoded;
                const core::vec3 middle = 0.5f * core::vec3(low + high + core::ivec3(1));
                const float radius = glm::length(core::vec3(dims)) * 0.5f + 1.0f;
                voxels = voxelsNear(scene, source, middle, radius, physics.collision.maxDecodedCells, decoded);
                if (voxels.empty()) return;
                total = voxels.size();
                group.assign(voxels.size(), 0);
                auto at = [&](core::ivec3 p) {
                    p -= low;
                    return (size_t(p.z) * size_t(dims.y) + size_t(p.y)) * size_t(dims.x) + size_t(p.x);
                };
                auto inside = [&](core::ivec3 p) { return !glm::any(glm::lessThan(p, low)) && !glm::any(glm::greaterThan(p, high)); };
                std::vector<int32_t> index(size_t(cells), -1);
                for (size_t i = 0; i < voxels.size(); i++) index[at(voxels[i].position)] = int32_t(i);
                if (strike)
                    for (size_t k = 0; k < strike->shards.size(); k++)
                        for (const FractureVoxel& v : strike->shards[k])
                            if (inside(v.position))
                                if (int32_t j = index[at(v.position)]; j >= 0) group[size_t(j)] = uint32_t(k + 1);
                static const core::ivec3 faces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                std::vector<uint8_t> held(voxels.size(), 0);
                std::vector<size_t> stack;
                auto flood = [&] {
                    std::vector<size_t> reached;
                    while (!stack.empty()) {
                        size_t k = stack.back();
                        stack.pop_back();
                        reached.push_back(k);
                        for (core::ivec3 f : faces) {
                            core::ivec3 n = voxels[k].position + f;
                            if (!inside(n)) continue;
                            int32_t j = index[at(n)];
                            if (j < 0 || held[size_t(j)] || group[size_t(j)] != group[k]) continue;
                            held[size_t(j)] = 1;
                            stack.push_back(size_t(j));
                        }
                    }
                    return reached;
                };

                // A static collider: what holds it up, anchor materials and its base when asked.
                if (rest == Rest::Anchored) {
                    utils::SlotMaterials slotMaterials;
                    slotMaterialsOf(physics, scene, owner, slotMaterials);
                    const uint16_t own = ownMaterial(world, physics, entity);
                    std::array<uint8_t, 256> anchorSlot{};
                    bool anchorRule = anchorBase;   // whether anything could hold it up: a rule, not a voxel
                    const size_t entries = std::min<size_t>(scene.components[owner].materialPalette.size(), 256);
                    for (size_t slot = 0; slot < 256; slot++) {
                        uint16_t m = slotMaterials[slot] ? slotMaterials[slot] : own;
                        anchorSlot[slot] = m != 0 && physics.world->material(m).anchor;
                        if (slot < entries && anchorSlot[slot]) anchorRule = true;
                    }
                    for (size_t i = 0; i < voxels.size(); i++)
                        if (group[i] == 0 && ((anchorBase && voxels[i].position.y <= 0) || anchorSlot[voxels[i].slot])) {
                            held[i] = 1;
                            stack.push_back(i);
                        }
                    // A face against a cell that is not loaded: whatever is there may hold it, so it is held.
                    if (partlyLoaded && scene.components[h].kind == ComponentKind::Grid) {
                        const SceneGrid& grid = scene.grids[size_t(scene.components[h].gridIndex)];
                        std::set<std::tuple<int, int, int>> unloaded;
                        for (size_t lin = 0; lin < grid.cellToChunk.size(); lin++) {
                            if (grid.cellToChunk[lin] != CELL_NOT_LOADED) continue;
                            int x = int(lin % size_t(grid.dims.x)), y = int((lin / size_t(grid.dims.x)) % size_t(grid.dims.y));
                            int z = int(lin / (size_t(grid.dims.x) * size_t(grid.dims.y)));
                            core::ivec3 cell = grid.originCellCoord + core::ivec3(x, y, z);
                            unloaded.insert({cell.x, cell.y, cell.z});
                        }
                        const int res = int(source.parts.front().resolution);
                        auto cellOf = [res](core::ivec3 p) {
                            return core::ivec3(utils::floorDiv(p.x, res), utils::floorDiv(p.y, res), utils::floorDiv(p.z, res));
                        };
                        for (size_t i = 0; i < voxels.size() && !unloaded.empty(); i++) {
                            if (group[i] != 0 || held[i]) continue;
                            const core::ivec3 own = cellOf(voxels[i].position);
                            for (core::ivec3 f : faces) {
                                core::ivec3 n = cellOf(voxels[i].position + f);
                                if (n == own || !unloaded.count({n.x, n.y, n.z})) continue;
                                held[i] = 1;
                                stack.push_back(i);
                                break;
                            }
                        }
                        anchorRule = anchorRule || !unloaded.empty();
                    }
                    if (stack.empty() && !anchorRule) {
                        // Nothing could ever hold it up, so everything would fall at its first edit: a
                        // collider set up wrong, not a collapse. (One with a rule whose anchors are all cut
                        // away falls.) A strike's shards still come away.
                        if (physics.warnedUnanchored.insert(entity).second)
                            core::warn("physics: entity {} splits but has nothing to stand on (no anchor material, and "
                                       "anchorBase off); nothing falls", static_cast<uint32_t>(entity));
                        for (size_t i = 0; i < voxels.size(); i++) held[i] = group[i] == 0;
                    }
                    flood();
                }

                // The islands not held, in voxel order. A moving body keeps the largest of the rest.
                for (size_t i = 0; i < voxels.size(); i++) {
                    if (held[i]) continue;
                    held[i] = 1;
                    stack.push_back(i);
                    std::vector<size_t> members = flood();
                    std::sort(members.begin(), members.end());
                    islands.push_back({std::move(members), group[i]});
                }
                if (rest == Rest::Largest) {
                    std::optional<size_t> largest;
                    for (size_t i = 0; i < islands.size(); i++)
                        if (islands[i].group == 0 && (!largest || islands[i].members.size() > islands[*largest].members.size()))
                            largest = i;
                    if (largest) islands.erase(islands.begin() + std::ptrdiff_t(*largest));
                }
                size_t rests = 0;
                std::vector<Island> capped;
                for (Island& island : islands)
                    if (island.group != 0 || rests++ < physics.fractureLimits.maxIslands) capped.push_back(std::move(island));
                if (rests > physics.fractureLimits.maxIslands)
                    core::trace("physics: entity {} came apart in {} islands; the first {} come away", static_cast<uint32_t>(entity),
                                rests, physics.fractureLimits.maxIslands);
                islands.swap(capped);
            }
            if (islands.empty()) return;
            // A strike's shards first, then the rest's islands, each in voxel order.
            std::stable_partition(islands.begin(), islands.end(), [](const Island& i) { return i.group != 0; });
            std::vector<Cut> cuts;
            for (Island& island : islands) cuts.push_back({std::move(island.members), island.group != 0, {}, {}});
            cutIslands(world, physics, scene, entity, h, source, voxels, std::move(cuts), total, state, strike,
                       strike ? strike->s.cause : fixed ? BreakCause::Support : BreakCause::Edit);
        }

        // The piece budget: past PhysicsConfig::maxPieces pieces, the oldest go, their bodies at once.
        void clearPieces(World& world, PhysicsRuntime& physics) {
            if (physics.maxPieces == 0) return;
            auto pieces = world.view<Piece>();
            const size_t count = pieces.size();
            if (count <= physics.maxPieces) return;
            std::vector<std::tuple<uint64_t, uint32_t, Entity>> order;
            order.reserve(count);
            for (auto [entity, p] : pieces.each()) {
                const NetId* id = world.try_get<NetId>(entity);
                order.push_back({p.madeAt, id ? id->value : 0u, entity});
            }
            std::sort(order.begin(), order.end());
            const size_t excess = count - physics.maxPieces;
            PiecesCleared event;
            event.entities.reserve(excess);
            for (size_t i = 0; i < excess; i++) event.entities.push_back(std::get<2>(order[i]));
            // Delivered now, not at the next pump: a handler has to see them before they go, to keep
            // one (by taking its Piece off) or put something in its place.
            if (Events* events = world.ctx().find<Events>()) events->trigger(event);
            size_t cleared = 0;
            for (Entity e : event.entities)
                if (world.valid(e) && world.all_of<Piece>(e)) {
                    world.destroy(e);
                    cleared++;
                }
            for (BodyId id : physics.doomed) physics.world->destroyBody(id);
            physics.doomed.clear();
            core::trace("physics: cleared {} pieces, the oldest (budget {}); {} kept", cleared, physics.maxPieces, excess - cleared);
        }

        // NetId order, then entity order for the rest: what every peer agrees on.
        PhysicsRuntime::OrderKey orderKey(const World& world, Entity e) {
            const NetId* id = world.valid(e) ? world.try_get<NetId>(e) : nullptr;
            const uint32_t net = id ? id->value : 0;
            return {net == 0, net, e};
        }

        // The simulation's contact changes, as entities: body pairs become entity pairs (a body
        // rebuilt under a contact ends one pair and begins another in the same step, which nets out),
        // and an entity pair begins when its first body pair does and ends with its last. Reported
        // when one of the two asked (reportsContactWith); its end is reported when its begin was.
        void gatherContacts(World& world, PhysicsRuntime& physics) {
            physics.contacts.clear();
            const std::vector<ContactChange>& changes = physics.world->contactChanges();
            if (changes.empty()) return;
            using Pair = std::pair<Entity, Entity>;
            struct Delta { int bodies = 0; bool sensor = false; bool began = false; core::vec3 point{0.0f}, normal{0.0f}; };
            std::map<Pair, Delta> deltas;
            for (const ContactChange& c : changes) {
                const uint64_t key = (uint64_t(c.a.value) << 32) | c.b.value;
                if (c.began) {
                    auto ea = physics.byBody.find(c.a.value), eb = physics.byBody.find(c.b.value);
                    if (ea == physics.byBody.end() || eb == physics.byBody.end()) continue;   // not an entity's
                    Entity a = ea->second, b = eb->second;
                    if (a == b || !world.valid(a) || !world.valid(b)) continue;
                    core::vec3 normal = c.normal;
                    if (orderKey(world, b) < orderKey(world, a)) { std::swap(a, b); normal = -normal; }
                    physics.contactBodies[key] = {a, b};
                    Delta& d = deltas[{a, b}];
                    d.bodies++;
                    d.sensor = d.sensor || c.sensor;
                    if (!d.began) { d.began = true; d.point = c.point; d.normal = normal; }
                } else {
                    auto it = physics.contactBodies.find(key);
                    if (it == physics.contactBodies.end()) continue;
                    deltas[it->second].bodies--;
                    physics.contactBodies.erase(it);
                }
            }
            std::vector<std::pair<std::pair<PhysicsRuntime::OrderKey, PhysicsRuntime::OrderKey>, BodyContact>> out;
            for (const auto& [pair, d] : deltas) {
                auto it = physics.contactEntities.find(pair);
                const int before = it == physics.contactEntities.end() ? 0 : it->second.bodies;
                const int after = before + d.bodies;
                if (before <= 0 && after > 0) {
                    PhysicsRuntime::EntityPair e;
                    e.bodies = after;
                    e.sensor = d.sensor;
                    e.reported = reportsContactWith(world, physics, pair.first, pair.second) ||
                                 reportsContactWith(world, physics, pair.second, pair.first);
                    e.orderA = orderKey(world, pair.first);
                    e.orderB = orderKey(world, pair.second);
                    physics.contactEntities[pair] = e;
                    if (e.reported) out.push_back({{e.orderA, e.orderB}, {pair.first, pair.second, true, e.sensor, d.point, d.normal}});
                } else if (before > 0 && after <= 0) {
                    const PhysicsRuntime::EntityPair& e = it->second;
                    if (e.reported) out.push_back({{e.orderA, e.orderB}, {pair.first, pair.second, false, e.sensor}});
                    physics.contactEntities.erase(it);
                } else if (it != physics.contactEntities.end()) {
                    it->second.bodies = after;
                }
            }
            std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            for (const auto& [key, c] : out) {
                physics.contacts.push_back(c);
                if (c.began) send(world, ContactBegan{c.a, c.b, c.point, c.normal, c.sensor});
                else send(world, ContactEnded{c.a, c.b, c.sensor});
            }
        }

        // After a restore: who touches whom, as the restored simulation has it, with nothing sent.
        void rebuildContacts(World& world, PhysicsRuntime& physics) {
            physics.contactBodies.clear();
            physics.contactEntities.clear();
            physics.contacts.clear();
            for (const ContactChange& c : physics.world->contactPairs()) {
                auto ea = physics.byBody.find(c.a.value), eb = physics.byBody.find(c.b.value);
                if (ea == physics.byBody.end() || eb == physics.byBody.end()) continue;
                Entity a = ea->second, b = eb->second;
                if (a == b || !world.valid(a) || !world.valid(b)) continue;
                if (orderKey(world, b) < orderKey(world, a)) std::swap(a, b);
                physics.contactBodies[(uint64_t(c.a.value) << 32) | c.b.value] = {a, b};
                PhysicsRuntime::EntityPair& e = physics.contactEntities[{a, b}];
                if (e.bodies++ == 0) {
                    e.sensor = c.sensor;
                    e.reported = reportsContactWith(world, physics, a, b) || reportsContactWith(world, physics, b, a);
                    e.orderA = orderKey(world, a);
                    e.orderB = orderKey(world, b);
                }
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

            // 2a. Voxel edits issued as commands, in issue order. A strike only works out which voxels
            //     go together; the island pass below cuts them. A strike sees what was edited before it
            //     (a plain edit is applied first), and a second strike on a body struck already this step
            //     sees the first one's cut (that body's pass runs first).
            Scene* scene = world.ctx().find<Scene>();
            auto broke = [&](Entity entity, const PhysicsRuntime::Break* strike) {
                const PhysicsBody& body = world.get<PhysicsBody>(entity);
                BodyState state = sim.bodyState(body.id);
                if (body.motion == MotionType::Dynamic && !physics.carried.count(entity))
                    physics.carried[entity] = {state.linearVelocity, state.angularVelocity};
                splitIslands(world, physics, *scene, entity, state, strike);
                physics.rebuild.push_back(entity);
                physics.reshaped.insert(entity);
            };
            if (scene && !physics.edits.empty()) {
                std::vector<PhysicsRuntime::Edit> edits;
                edits.swap(physics.edits);
                bool queued = false;
                std::vector<PhysicsRuntime::Edit> deferred;
                for (auto& edit : edits) {
                    if (edit.split) {
                        if (queued) utils::updateScene(*scene);
                        queued = false;
                        if (world.valid(edit.entity) && world.all_of<PhysicsBody>(edit.entity)) {
                            applySplit(world, physics, *scene, edit.entity, *edit.split);
                        } else if (world.valid(edit.entity) && edit.waited++ < 120) {
                            deferred.push_back(std::move(edit));   // its body is made at this step's end
                        } else {
                            core::warn("physics: a split of entity {} was dropped: it has no body", static_cast<uint32_t>(edit.entity));
                        }
                        continue;
                    }
                    if (edit.replicated) {
                        if (queued) utils::updateScene(*scene);
                        queued = false;
                        ComponentHandle target = voxelComponentOf(world, edit.entity);
                        if (target == INVALID_COMPONENT_HANDLE) continue;
                        std::vector<PendingVoxelOp> adds, removes;
                        for (const PendingVoxelOp& op : edit.ops) (op.isAdd ? adds : removes).push_back(op);
                        if (!adds.empty()) utils::queueVoxelAdd(*scene, target, adds);
                        if (!removes.empty()) utils::queueVoxelRemove(*scene, target, removes);
                        utils::updateScene(*scene);
                        // Rebuilt for its new shape, and no island pass: the server says what came loose.
                        if (PhysicsBody* body = world.try_get<PhysicsBody>(edit.entity)) {
                            const VoxelComponent& link = world.get<VoxelComponent>(edit.entity);
                            if (body->motion == MotionType::Dynamic && !physics.carried.count(edit.entity)) {
                                BodyState state = sim.bodyState(body->id);
                                physics.carried[edit.entity] = {state.linearVelocity, state.angularVelocity};
                            }
                            body->sources = sourcesOf(*scene, link.handle);
                            physics.rebuild.push_back(edit.entity);
                            physics.reshaped.insert(edit.entity);
                        }
                        continue;
                    }
                    if (edit.strike) {
                        if (queued) utils::updateScene(*scene);
                        queued = false;
                        if (auto earlier = physics.breaking.find(edit.entity); earlier != physics.breaking.end()) {
                            PhysicsRuntime::Break first = std::move(earlier->second);
                            physics.breaking.erase(earlier);
                            if (world.valid(edit.entity) && world.all_of<PhysicsBody>(edit.entity)) broke(edit.entity, &first);
                        }
                        PhysicsRuntime::Break b;
                        if (breakBody(world, physics, *scene, edit.entity, edit.s, b)) physics.breaking[edit.entity] = std::move(b);
                        continue;
                    }
                    queued = true;
                    ComponentHandle target = voxelComponentOf(world, edit.entity);
                    if (target == INVALID_COMPONENT_HANDLE) continue;
                    std::vector<PendingVoxelOp> adds, removes;
                    for (const PendingVoxelOp& op : edit.ops) (op.isAdd ? adds : removes).push_back(op);
                    if (!adds.empty()) utils::queueVoxelAdd(*scene, target, adds);
                    if (!removes.empty()) utils::queueVoxelRemove(*scene, target, removes);
                    // At once, so it is heard in order with the breaks that follow from it.
                    if (Events* events = world.ctx().find<Events>()) events->trigger(VoxelsEdited{edit.entity, edit.ops});
                }
                if (queued) utils::updateScene(*scene);
                physics.edits.insert(physics.edits.begin(), std::make_move_iterator(deferred.begin()),
                                     std::make_move_iterator(deferred.end()));
            }

            // 2b. Bodies whose voxels changed or that were struck: the island pass (what came away
            //     becomes bodies), then a rebuild. Only looked for when some blob's content changed
            //     since the last step, anywhere, or something was struck. In NetId-then-entity order.
            if (scene && (latestBlobContentStamp() != physics.seenStamp || blockResidencyCounter != physics.seenResidency ||
                          !physics.breaking.empty())) {
                // Waiting for their blocks: back in line when some have loaded.
                for (auto it = physics.waitingForBlocks.begin(); it != physics.waitingForBlocks.end();) {
                    Entity e = *it;
                    const VoxelComponent* link = world.valid(e) ? world.try_get<VoxelComponent>(e) : nullptr;
                    if (!link || !wantsBody(world, e) || !utils::isComponentAlive(*scene, link->ref())) {
                        it = physics.waitingForBlocks.erase(it);
                        continue;
                    }
                    if (!sourcesOf(*scene, link->handle).empty()) {
                        physics.pending.push_back(e);
                        physics.streamed.insert(e);
                        it = physics.waitingForBlocks.erase(it);
                        continue;
                    }
                    ++it;
                }
                std::vector<std::pair<Entity, uint32_t>> changed;
                std::set<Entity> loadedOnly;
                for (auto [entity, body] : world.view<PhysicsBody>().each()) {
                    const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
                    if (!link || !utils::isComponentAlive(*scene, link->ref())) continue;
                    const bool struck = physics.breaking.count(entity) != 0;
                    Sources now = sourcesOf(*scene, link->handle);
                    if (!struck && now == body.sources) continue;
                    // Chunks only came or went (world loading), none changed: a rebuild, not an edit.
                    if (!struck) {
                        std::map<ChunkHandle, uint64_t> before(body.sources.begin(), body.sources.end());
                        bool edited = false;
                        for (const auto& [chunk, stamp] : now)
                            if (auto it = before.find(chunk); it != before.end() && it->second != stamp) edited = true;
                        if (!edited) loadedOnly.insert(entity);
                    }
                    const NetId* id = world.try_get<NetId>(entity);
                    changed.push_back({entity, id ? id->value : 0});
                }
                std::sort(changed.begin(), changed.end(), [](const auto& a, const auto& b) {
                    return std::make_tuple(a.second == 0, a.second, a.first) < std::make_tuple(b.second == 0, b.second, b.first);
                });
                std::map<Entity, PhysicsRuntime::Break> breaking;
                breaking.swap(physics.breaking);
                for (auto [entity, net] : changed) {
                    if (!world.valid(entity) || !world.all_of<PhysicsBody>(entity)) continue;   // a handler destroyed it
                    if (loadedOnly.count(entity)) {
                        const PhysicsBody& body = world.get<PhysicsBody>(entity);
                        if (body.motion == MotionType::Dynamic && !physics.carried.count(entity)) {
                            BodyState state = sim.bodyState(body.id);
                            physics.carried[entity] = {state.linearVelocity, state.angularVelocity};
                        }
                        physics.rebuild.push_back(entity);
                        physics.streamed.insert(entity);
                        continue;
                    }
                    auto strike = breaking.find(entity);
                    broke(entity, strike != breaking.end() ? &strike->second : nullptr);
                }
                physics.seenStamp = latestBlobContentStamp();
                physics.seenResidency = blockResidencyCounter;
                clearPieces(world, physics);
            }

            // 2c. Bodies whose materials changed: a palette entry named another, the library redefined
            //     one. Only looked for when some palette or the library changed since the last step.
            if (scene) {
                Fnv palettes;
                palettes.add(physics.library.version);
                for (const ComponentRecord& record : scene->components) palettes.add(record.paletteVersion);
                if (palettes.value != physics.seenPalettes) {
                    physics.seenPalettes = palettes.value;
                    std::vector<std::pair<Entity, uint32_t>> changed;
                    for (auto [entity, body] : world.view<PhysicsBody>().each()) {
                        const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
                        bool linked = link && utils::isComponentAlive(*scene, link->ref());
                        const RigidBody* rigid = world.try_get<RigidBody>(entity);
                        bool voxels = linked && (!rigid || rigid->shape == RigidBody::Shape::Voxels);
                        uint64_t key = materialKeyOf(physics, scene, linked ? link->handle : INVALID_COMPONENT_HANDLE,
                                                     voxels, ownMaterial(world, physics, entity));
                        if (key == body.materialKey) continue;
                        const NetId* id = world.try_get<NetId>(entity);
                        changed.push_back({entity, id ? id->value : 0});
                    }
                    std::sort(changed.begin(), changed.end(), [](const auto& a, const auto& b) {
                        return std::make_tuple(a.second == 0, a.second, a.first) < std::make_tuple(b.second == 0, b.second, b.first);
                    });
                    for (auto [entity, net] : changed) {
                        const PhysicsBody& body = world.get<PhysicsBody>(entity);
                        if (body.motion == MotionType::Dynamic && !physics.carried.count(entity)) {
                            BodyState state = sim.bodyState(body.id);
                            physics.carried[entity] = {state.linearVelocity, state.angularVelocity};
                        }
                        physics.rebuild.push_back(entity);
                    }
                }
            }

            // 2. Rebuilds: a changed RigidBody, a new or lost link, changed voxels. A moving body keeps
            //    its motion through it: changing its friction must not stop it in mid-air. (Voxel and
            //    material changes noted theirs already, before splitting moved the pieces.)
            for (Entity e : physics.rebuild) {
                if (!world.valid(e)) continue;
                if (const PhysicsBody* body = world.try_get<PhysicsBody>(e);
                    body && body->motion == MotionType::Dynamic && !physics.carried.count(e)) {
                    BodyState state = sim.bodyState(body->id);
                    physics.carried[e] = {state.linearVelocity, state.angularVelocity};
                }
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
                if (!id.valid() && described.noVoxels && (physics.streamed.count(e) || unloadedUnder(world, e))) {
                    core::trace("physics: entity {}'s voxels are not loaded; its body waits for them", static_cast<uint32_t>(e));
                    physics.waitingForBlocks.insert(e);
                    continue;
                }
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
                body.materialKey = described.materialKey;
                physics.byBody[id.value] = e;
                world.emplace<PhysicsBody>(e, body);
            }

            physics.reshaped.clear();
            physics.streamed.clear();

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
                    desc.filter = filterOf(world, physics, e);
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
                    if (world.all_of<ContactReports>(e)) sim.setReportContacts(sim.characterBody(id), true);
                    world.emplace<PhysicsCharacter>(e, c);
                }
            }

            // 3b'. Collision groups changed, or a name defined that some named: each body's or
            //      character's filter set again, in NetId-then-entity order, where it changed.
            {
                std::vector<Entity> regroup;
                regroup.swap(physics.regroup);
                if (physics.groups.version != physics.groups.seen) {
                    physics.groups.seen = physics.groups.version;
                    for (Entity e : world.view<CollisionGroups>()) regroup.push_back(e);
                }
                std::sort(regroup.begin(), regroup.end(), [&](Entity a, Entity b) { return order(a) < order(b); });
                regroup.erase(std::unique(regroup.begin(), regroup.end()), regroup.end());
                for (Entity e : regroup) {
                    if (!world.valid(e)) continue;
                    const CollisionFilter f = filterOf(world, physics, e);
                    if (const PhysicsBody* body = world.try_get<PhysicsBody>(e); body && sim.filter(body->id) != f)
                        sim.setFilter(body->id, f);
                    if (const PhysicsCharacter* c = world.try_get<PhysicsCharacter>(e);
                        c && sim.filter(sim.characterBody(c->id)) != f)
                        sim.setCharacterFilter(c->id, f);
                }
            }

            // 3b''. ContactReports added or removed: the body or character reports from this step.
            {
                std::vector<Entity> rereport;
                rereport.swap(physics.rereport);
                std::sort(rereport.begin(), rereport.end(), [&](Entity a, Entity b) { return order(a) < order(b); });
                rereport.erase(std::unique(rereport.begin(), rereport.end()), rereport.end());
                for (Entity e : rereport) {
                    if (!world.valid(e)) continue;
                    const bool want = world.all_of<ContactReports>(e);
                    if (const PhysicsBody* body = world.try_get<PhysicsBody>(e)) sim.setReportContacts(body->id, want);
                    if (const PhysicsCharacter* c = world.try_get<PhysicsCharacter>(e))
                        sim.setReportContacts(sim.characterBody(c->id), want);
                }
            }

            // 3b'''. World loading: a dynamic body whose entity is Kept, or held (holdBody), is frozen -- solid and still --
            //       its motion kept in FrozenMotion; Active (or not taking part) thaws it and gives the
            //       motion back. In NetId-then-entity order.
            {
                std::vector<Entity> moving;
                for (auto [e, body] : world.view<PhysicsBody>().each())
                    if (body.motion == MotionType::Dynamic) moving.push_back(e);
                std::sort(moving.begin(), moving.end(), [&](Entity a, Entity b) { return order(a) < order(b); });
                for (Entity e : moving) {
                    const BodyId id = world.get<PhysicsBody>(e).id;
                    const LoadState* load = world.try_get<LoadState>(e);
                    const bool want = (load && load->presence == utils::Presence::Kept) || world.all_of<PhysicsHold>(e);
                    const bool is = sim.frozen(id);
                    if (want && !is) {
                        if (!world.all_of<FrozenMotion>(e)) {
                            BodyState state = sim.bodyState(id);
                            world.emplace<FrozenMotion>(e, FrozenMotion{state.linearVelocity, state.angularVelocity});
                        }
                        sim.setFrozen(id, true);
                    } else if (!want) {
                        if (is) sim.setFrozen(id, false);
                        if (const FrozenMotion* motion = world.try_get<FrozenMotion>(e)) {
                            sim.setVelocity(id, motion->linear, motion->angular);
                            world.remove<FrozenMotion>(e);
                        }
                    }
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
                    case Kind::SetState: {
                        // Drawn from where it was drawn, easing to the corrected pose.
                        const core::vec3 shownAt = body->presented ? body->presentedPosition : body->position;
                        const core::quat shownAs = body->presented ? body->presentedRotation : body->rotation;
                        const core::quat to = glm::normalize(c.q);
                        sim.setBodyState(body->id, c.a, to, c.b, c.c, c.jump);
                        body->previousPosition = body->position = c.a;
                        body->previousRotation = body->rotation = to;
                        body->easePosition = shownAt - c.a;
                        body->easeRotation = glm::normalize(shownAs * glm::conjugate(to));
                        break;
                    }
                    case Kind::Gravity: break;
                    case Kind::CharacterInput: break;
                    case Kind::JointMotor: break;
                }
            }

            // 5. Kinematic bodies head for their Transform (a Part's from the Scene, where the bridge
            //    last put it) -- or, with a ConstantVelocity, on from where they are.
            for (auto [entity, body] : world.view<PhysicsBody>().each()) {
                if (body.motion != MotionType::Kinematic) continue;
                const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
                const ConstantVelocity* flying = world.try_get<ConstantVelocity>(entity);
                if (flying && !(link && link->mode == LinkMode::Part)) {
                    // From the simulation's pose, not the Transform: that is interpolated for
                    // drawing, a step behind.
                    BodyState at = sim.bodyState(body.id);
                    core::quat rotation = at.rotation;
                    float turn = glm::length(flying->spin) * dt;
                    if (turn > 0.0f) rotation = glm::normalize(glm::angleAxis(turn, glm::normalize(flying->spin)) * rotation);
                    sim.moveKinematic(body.id, at.position + flying->velocity * dt, rotation, dt);
                } else if (link && link->mode == LinkMode::Part) {
                    if (scene && utils::isComponentAlive(*scene, link->ref()))
                        sim.moveKinematic(body.id, utils::getComponentWorldPosition(*scene, link->handle),
                                          utils::getComponentWorldRotation(*scene, link->handle), dt);
                } else if (const Transform* t = world.try_get<Transform>(entity)) {
                    // A blocked body's Transform is physics' to write (present): left as written, it
                    // says nothing, and the body holds where the simulation has it.
                    const bool unmoved = body.presented && t->position == body.presentedPosition &&
                                         t->rotation == body.presentedRotation && blockedKinematic(world, entity);
                    if (unmoved) sim.moveKinematic(body.id, body.position, body.rotation, dt);
                    else sim.moveKinematic(body.id, t->position, t->rotation, dt);
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

            // 7a. Contacts begun and ended, as entities: queued (ContactBegan, ContactEnded) and kept
            //     for runtime::contacts.
            gatherContacts(world, physics);

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
                // Dynamic bodies, and kinematic ones flying on a ConstantVelocity: the simulation moves
                // those too, so it writes their Transform. (Other kinematic bodies follow theirs.)
                bool simulated = body.motion == MotionType::Dynamic ||
                                 (body.motion == MotionType::Kinematic &&
                                  (world.all_of<ConstantVelocity>(entity) || blockedKinematic(world, entity)));
                if (!simulated || !world.all_of<Transform>(entity)) continue;
                core::vec3 position = glm::mix(body.previousPosition, body.position, alpha);
                core::quat rotation = glm::slerp(body.previousRotation, body.rotation, alpha);
                // A correction's offset, fading with a tenth of a second's time constant.
                if (body.easePosition != core::vec3(0.0f) || body.easeRotation != core::quat(1, 0, 0, 0)) {
                    const float keep = std::exp(-app.time().delta / 0.1f);
                    body.easePosition *= keep;
                    body.easeRotation = glm::slerp(core::quat(1, 0, 0, 0), body.easeRotation, keep);
                    if (glm::length(body.easePosition) < 1e-4f && std::abs(body.easeRotation.w) > 1.0f - 1e-8f) {
                        body.easePosition = core::vec3(0.0f);
                        body.easeRotation = core::quat(1, 0, 0, 0);
                    }
                    position += body.easePosition;
                    rotation = glm::normalize(body.easeRotation * rotation);
                }
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

        constexpr std::array<const char*, 4> LAYER_NAMES{"static", "moving", "sensor", "character"};
        const char* layerName(PhysicsLayer layer) {
            size_t i = size_t(layer);
            return i < LAYER_NAMES.size() ? LAYER_NAMES[i] : "moving";
        }
        std::optional<PhysicsLayer> layerFrom(const std::string& name) {
            for (size_t i = 0; i < LAYER_NAMES.size(); i++)
                if (name == LAYER_NAMES[i]) return PhysicsLayer(i);
            return std::nullopt;
        }
        // Layer bits as names, and back (an unknown name, or not a list of names: nullopt).
        nlohmann::json layerNames(uint32_t bits) {
            nlohmann::json out = nlohmann::json::array();
            for (size_t i = 0; i < LAYER_NAMES.size(); i++)
                if (bits & layerBit(PhysicsLayer(i))) out.push_back(LAYER_NAMES[i]);
            return out;
        }
        std::optional<uint32_t> layersFrom(const nlohmann::json& list) {
            if (!list.is_array()) return std::nullopt;
            uint32_t bits = 0;
            for (const nlohmann::json& name : list) {
                if (!name.is_string()) return std::nullopt;
                auto layer = layerFrom(name.get<std::string>());
                if (!layer) return std::nullopt;
                bits |= layerBit(*layer);
            }
            return bits;
        }
    }

    void installPhysics(Application& app, const PhysicsConfig& config) {
        World& world = app.world;
        if (world.ctx().contains<PhysicsRuntime>()) return;
        PhysicsRuntime& physics = world.ctx().emplace<PhysicsRuntime>();
        physics.world = std::make_unique<PhysicsWorld>(config.settings);
        physics.collision = config.collision;
        physics.destroyLeavers = config.destroyBodiesThatLeaveTheWorld;
        physics.maxPieces = config.maxPieces;
        physics.dustVoxels = config.dustVoxels;
        {
            // Guards, clamped rather than refused: a bad value must not leave fracture unguarded.
            PhysicsConfig::FractureLimits want = config.fracture, &got = physics.fractureLimits;
            auto finiteOr = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
            got.searchVoxels = std::clamp(finiteOr(want.searchVoxels, 3.0f), 0.5f, 16.0f);
            got.maxReachVoxels = std::clamp(finiteOr(want.maxReachVoxels, 64.0f), 1.0f, 256.0f);
            got.maxShards = std::clamp<uint32_t>(want.maxShards, 1, 4096);
            got.maxIslands = std::clamp<uint32_t>(want.maxIslands, 1, 4096);
            got.minImpactBreak = std::clamp(finiteOr(want.minImpactBreak, 1.0f), 0.5f, 16.0f);
            if (got.searchVoxels != want.searchVoxels || got.maxReachVoxels != want.maxReachVoxels ||
                got.maxShards != want.maxShards || got.maxIslands != want.maxIslands || got.minImpactBreak != want.minImpactBreak)
                core::warn("physics: fracture limits out of range, clamped to search {} voxels, reach {} voxels, {} shards, "
                           "{} islands, least impact break {} shard sizes",
                           got.searchVoxels, got.maxReachVoxels, got.maxShards, got.maxIslands, got.minImpactBreak);
        }
        physics.seenResidency = blockResidencyCounter;
        physics.seenStamp = latestBlobContentStamp();

        installNetIds(world);
        registerFracturePattern(world, "shatter", fracture_patterns::shatter);
        registerFracturePattern(world, "splinter", fracture_patterns::splinter);
        registerFracturePattern(world, "crumble", fracture_patterns::crumble);
        registerFracturePattern(world, "seams", fracture_patterns::seams);
        registerComponent<RigidBody>(world);
        registerComponent<StaticCollider>(world);
        registerComponent<CharacterController>(world);
        registerComponent<Joint>(world);
        registerComponent<CollisionGroups>(world);
        registerComponent<ContactReports>(world);
        registerComponent<FrozenMotion>(world);
        world.on_construct<ContactReports>().connect<&wantReport>();
        world.on_destroy<ContactReports>().connect<&wantReport>();
        world.on_construct<CollisionGroups>().connect<&wantRegroup>();
        world.on_update<CollisionGroups>().connect<&wantRegroup>();
        world.on_destroy<CollisionGroups>().connect<&wantRegroup>();
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

    bool hasPhysics(const World& world) { return world.ctx().contains<PhysicsRuntime>(); }

    PhysicsWorld& physicsWorld(World& world) { return *runtimeOf(world).world; }
    const PhysicsWorld& physicsWorld(const World& world) { return *world.ctx().get<PhysicsRuntime>().world; }

    using Kind = PhysicsCommand::Kind;
    void addImpulse(World& world, Entity e, core::vec3 v) { queue(world, Kind::Impulse, e, v); }
    void addAngularImpulse(World& world, Entity e, core::vec3 v) { queue(world, Kind::AngularImpulse, e, v); }
    void addVelocity(World& world, Entity e, core::vec3 v) { queue(world, Kind::AddVelocity, e, v); }
    void setVelocity(World& world, Entity e, core::vec3 linear, core::vec3 angular) {
        queue(world, Kind::SetVelocity, e, linear, angular);
    }
    void holdBody(World& world, Entity entity, bool held) {
        if (!world.valid(entity)) return;
        if (held) world.emplace_or_replace<PhysicsHold>(entity);
        else world.remove<PhysicsHold>(entity);
    }

    bool isHeld(const World& world, Entity entity) { return world.valid(entity) && world.all_of<PhysicsHold>(entity); }

    void setBodyState(World& world, Entity e, core::vec3 position, core::quat rotation, core::vec3 linear, core::vec3 angular,
                      bool asleep) {
        if (!std::isfinite(glm::dot(position, position) + glm::dot(linear, linear) + glm::dot(angular, angular))) return;
        queue(world, Kind::SetState, e, position, linear, rotation);
        PhysicsCommand& c = runtimeOf(world).commands.back().c;
        c.c = angular;
        c.jump = asleep;
    }

    void teleport(World& world, Entity e, core::vec3 position, core::quat rotation) {
        queue(world, Kind::Teleport, e, position, core::vec3(0.0f), rotation);
    }
    void setGravity(World& world, core::vec3 gravity) { queue(world, Kind::Gravity, NullEntity, gravity); }

    void editVoxels(World& world, Entity entity, const std::vector<PendingVoxelOp>& ops) {
        runtimeOf(world).edits.push_back({entity, ops});
    }

    void acceptVoxelChanges(World& world, Entity entity) {
        if (!world.ctx().contains<PhysicsRuntime>() || !world.valid(entity)) return;
        PhysicsRuntime& physics = runtimeOf(world);
        PhysicsBody* body = world.try_get<PhysicsBody>(entity);
        const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
        const Scene* scene = world.ctx().find<Scene>();
        if (!body || !link || !scene || !utils::isComponentAlive(*scene, link->ref())) return;
        if (body->motion == MotionType::Dynamic && !physics.carried.count(entity)) {
            BodyState state = physics.world->bodyState(body->id);
            physics.carried[entity] = {state.linearVelocity, state.angularVelocity};
        }
        body->sources = sourcesOf(*scene, link->handle);
        physics.rebuild.push_back(entity);
        physics.reshaped.insert(entity);
    }

    void applyVoxelEdit(World& world, Entity entity, std::vector<PendingVoxelOp> ops) {
        if (!world.ctx().contains<PhysicsRuntime>() || !world.valid(entity)) return;
        PhysicsRuntime::Edit edit;
        edit.entity = entity;
        edit.ops = std::move(ops);
        edit.replicated = true;
        runtimeOf(world).edits.push_back(std::move(edit));
    }

    void fracture(World& world, Entity entity, const Strike& strike) {
        auto finiteVec = [](core::vec3 v) { return std::isfinite(glm::dot(v, v)); };
        if (!finiteVec(strike.at) || !finiteVec(strike.push) || !finiteVec(strike.strikerVelocity) ||
            (strike.bodyVelocity && !finiteVec(*strike.bodyVelocity)) || !std::isfinite(strike.energy) ||
            !std::isfinite(strike.reach) || !std::isfinite(strike.minimumBreak) || !std::isfinite(strike.shardSize)) {
            core::warn("physics: refused a fracture with a value that is not finite");
            return;
        }
        if (strike.energy < 0.0f || strike.reach < 0.0f || strike.shardSize < 0.0f) {
            core::warn("physics: refused a fracture with a negative energy, reach or shard size");
            return;
        }
        PhysicsRuntime::Edit edit{entity, {}};
        edit.strike = true;
        edit.s = strike;
        runtimeOf(world).edits.push_back(std::move(edit));
    }

    void fracture(World& world, Entity entity, core::vec3 at, float energy, core::vec3 push) {
        Strike strike;
        strike.at = at;
        strike.energy = energy;
        strike.push = push;
        fracture(world, entity, strike);
    }

    void fractureWithin(World& world, Entity entity, core::vec3 at, float radius, core::vec3 push) {
        if (!(radius > 0.0f)) {
            core::warn("physics: refused a fracture within a radius that is not positive");
            return;
        }
        Strike strike;
        strike.at = at;
        strike.reach = radius;
        strike.push = push;
        fracture(world, entity, strike);
    }

    VoxelSplit voxelSplitOf(const World& world, const BodyBroken& event) {
        VoxelSplit split;
        for (size_t i = 0; i < event.pieces.size() && i < event.pieceVoxels.size(); i++) {
            VoxelSplit::Piece piece;
            piece.voxels = event.pieceVoxels[i];
            if (world.valid(event.pieces[i]))
                if (const NetId* id = world.try_get<NetId>(event.pieces[i])) piece.netId = *id;
            piece.shard = i < event.shards;
            if (i < event.pieceMotion.size()) std::tie(piece.linear, piece.angular) = event.pieceMotion[i];
            split.pieces.push_back(std::move(piece));
        }
        split.dust = event.dust;
        return split;
    }

    void applyVoxelSplit(World& world, Entity entity, VoxelSplit split) {
        if (!world.ctx().contains<PhysicsRuntime>() || !world.valid(entity)) return;
        PhysicsRuntime::Edit edit;
        edit.entity = entity;
        edit.split = std::make_shared<VoxelSplit>(std::move(split));
        runtimeOf(world).edits.push_back(std::move(edit));
    }

    bool canBreak(World& world, Entity entity) {
        const Scene* scene = world.ctx().find<Scene>();
        return scene && world.valid(entity) && world.ctx().contains<PhysicsRuntime>() &&
               canBreak(world, runtimeOf(world), *scene, entity);
    }

    std::vector<BodyImpact> impacts(const World& world) {
        std::vector<BodyImpact> out;
        const PhysicsRuntime* physics = world.ctx().find<PhysicsRuntime>();
        if (!physics) return out;
        auto entityOf = [&](BodyId id) {
            auto it = physics->byBody.find(id.value);
            return it != physics->byBody.end() && world.valid(it->second) ? it->second : NullEntity;
        };
        for (const Impact& hit : physics->world->impacts()) {
            BodyImpact e{entityOf(hit.a), entityOf(hit.b), hit.point, hit.normal, hit.speed, hit.energy, hit.velocityA, hit.velocityB};
            if (e.a != NullEntity && e.b != NullEntity) out.push_back(e);
        }
        return out;
    }

    std::vector<BodyContact> contacts(const World& world) {
        const PhysicsRuntime* physics = world.ctx().find<PhysicsRuntime>();
        return physics ? physics->contacts : std::vector<BodyContact>{};
    }

    std::vector<Entity> touching(const World& world, Entity entity) {
        std::vector<std::pair<PhysicsRuntime::OrderKey, Entity>> found;
        if (const PhysicsRuntime* physics = world.ctx().find<PhysicsRuntime>())
            for (const auto& [pair, e] : physics->contactEntities) {
                if (!e.reported) continue;
                if (pair.first == entity) found.push_back({e.orderB, pair.second});
                else if (pair.second == entity) found.push_back({e.orderA, pair.first});
            }
        std::sort(found.begin(), found.end());
        std::vector<Entity> out;
        for (const auto& [key, e] : found) out.push_back(e);
        return out;
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
            if (record.chunkHandle >= scene->chunks.size()) return ops;
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

    uint16_t definePhysicsMaterial(World& world, const std::string& name, const PhysicsMaterial& material) {
        PhysicsRuntime& physics = runtimeOf(world);
        if (name.empty() || name.size() > 64) {
            core::warn("physics: refused a physics material named '{}': names are 1 to 64 characters", name);
            return 0;
        }
        auto found = physics.library.index.find(name);
        uint16_t index = found != physics.library.index.end() ? found->second : uint16_t(physics.library.names.size() + 1);
        if (index >= MAX_PHYSICS_MATERIALS) {
            core::warn("physics: refused physics material '{}': the library is full ({})", name, MAX_PHYSICS_MATERIALS - 1);
            return 0;
        }
        if (!physics.world->setMaterial(index, material)) return 0;
        if (material.strength > 0.0f && !fracturePattern(world, material.fracture))
            core::warn("physics: physics material '{}' breaks as '{}', which no fracture pattern is registered as (yet)", name,
                       material.fracture);
        if (found == physics.library.index.end()) {
            physics.library.names.push_back(name);
            physics.library.index[name] = index;
            physics.library.warned.erase(name);
        }
        physics.library.version++;
        return index;
    }

    uint32_t definePhysicsGroup(World& world, const std::string& name) {
        PhysicsRuntime& physics = runtimeOf(world);
        if (name.empty() || name.size() > 64) {
            core::warn("physics: refused a collision group named '{}': names are 1 to 64 characters", name);
            return 0;
        }
        if (auto found = physics.groups.bit.find(name); found != physics.groups.bit.end()) return found->second;
        if (physics.groups.names.size() >= MAX_COLLISION_GROUPS) {
            core::warn("physics: refused collision group '{}': all {} are taken", name, MAX_COLLISION_GROUPS);
            return 0;
        }
        const uint32_t bit = 1u << physics.groups.names.size();
        physics.groups.names.push_back(name);
        physics.groups.bit[name] = bit;
        physics.groups.version++;
        physics.groups.warned.erase(name);
        return bit;
    }

    uint32_t physicsGroup(const World& world, const std::string& name) {
        const PhysicsRuntime& physics = world.ctx().get<PhysicsRuntime>();
        auto found = physics.groups.bit.find(name);
        return found != physics.groups.bit.end() ? found->second : 0;
    }

    uint16_t physicsMaterialIndex(const World& world, const std::string& name) {
        const PhysicsRuntime& physics = world.ctx().get<PhysicsRuntime>();
        auto found = physics.library.index.find(name);
        return found != physics.library.index.end() ? found->second : 0;
    }

    std::optional<PhysicsMaterial> physicsMaterial(const World& world, const std::string& name) {
        uint16_t index = physicsMaterialIndex(world, name);
        if (!index) return std::nullopt;
        return physicsWorld(world).material(index);
    }

    bool loadPhysicsMaterials(World& world, const std::string& path) {
        std::ifstream file(path);
        if (!file) {
            core::warn("physics: cannot open physics material library '{}'", path);
            return false;
        }
        nlohmann::json json = nlohmann::json::parse(file, nullptr, false);
        if (json.is_discarded() || !json.is_object() || !json.contains("materials") || !json["materials"].is_array()) {
            core::warn("physics: '{}' is not a physics material library (an object with a \"materials\" array)", path);
            return false;
        }
        if (json.value("v", 1) != 1) {
            core::warn("physics: '{}' is library version {}; this build reads version 1", path, json.value("v", 1));
            return false;
        }
        for (const nlohmann::json& entry : json["materials"]) {
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string()) {
                core::warn("physics: '{}' has a material with no name; the rest are not loaded", path);
                return false;
            }
            PhysicsMaterial m;
            try {
                m.density = entry.value("density", m.density);
                m.friction = entry.value("friction", m.friction);
                m.restitution = entry.value("restitution", m.restitution);
                m.strength = entry.value("strength", m.strength);
                m.fracture = entry.value("fracture", m.fracture);
                m.shardSize = entry.value("shardSize", m.shardSize);
                m.anchor = entry.value("anchor", m.anchor);
                if (entry.contains("grain")) {
                    const nlohmann::json& g = entry["grain"];
                    if (!g.is_array() || g.size() != 3) throw nlohmann::json::type_error::create(302, "grain is three numbers", &g);
                    m.grain = core::vec3(g[0].get<float>(), g[1].get<float>(), g[2].get<float>());
                }
            } catch (const nlohmann::json::exception&) {
                core::warn("physics: '{}': material '{}' has a value of the wrong type; the rest are not loaded", path,
                           entry["name"].get<std::string>());
                return false;
            }
            if (!definePhysicsMaterial(world, entry["name"].get<std::string>(), m)) return false;
        }
        return true;
    }

    uint64_t physicsStateHash(const World& world) {
        const PhysicsWorld& sim = physicsWorld(world);
        Hash h;
        h.add(sim.tick());
        // The library: peers with a different stone are a mismatch, found at once.
        const PhysicsRuntime& physics = world.ctx().get<PhysicsRuntime>();
        for (size_t i = 0; i < physics.library.names.size(); i++) {
            PhysicsMaterial m = sim.material(uint16_t(i + 1));
            for (char c : physics.library.names[i]) h.add(c);
            h.add(m.density); h.add(m.friction); h.add(m.restitution);
            h.add(m.strength); h.add(m.shardSize); h.add(m.grain);
            h.add(uint8_t(m.anchor));
            for (char c : m.fracture) h.add(c);
        }
        // The group names, in bit order: peers must agree on what each bit means.
        for (const std::string& name : physics.groups.names) {
            for (char c : name) h.add(c);
            h.add(uint8_t(0));
        }
        for (auto [entity, net] : bodiesInOrder(world)) {
            const BodyId id = world.get<PhysicsBody>(entity).id;
            BodyState s = sim.bodyState(id);
            h.add(net);
            if (net == 0) h.add(entity);
            h.add(s.position); h.add(s.rotation); h.add(s.linearVelocity); h.add(s.angularVelocity);
            h.add(uint8_t(s.awake));
            const CollisionFilter f = sim.filter(id);
            h.add(f.groups); h.add(f.ignores); h.add(f.family);
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
        rebuildContacts(world, physics);
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

        QueryFilter filterFor(const World& world, uint32_t layers, Entity ignore, uint32_t skipGroups) {
            QueryFilter f;
            f.layers = layers;
            f.skipGroups = skipGroups;
            // The entity's body, or a character's inner body: a ray from a player's eyes starts inside it.
            if (ignore != NullEntity && world.valid(ignore)) {
                if (const PhysicsBody* b = world.try_get<PhysicsBody>(ignore)) f.ignore = b->id;
                else if (const PhysicsCharacter* c = world.try_get<PhysicsCharacter>(ignore))
                    f.ignore = physicsWorld(world).characterBody(c->id);
            }
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
                                    uint32_t layers, Entity ignore, uint32_t skipGroups) {
        return resolve(world, physicsWorld(world).castRay(origin, direction, maxDistance, filterFor(world, layers, ignore, skipGroups)));
    }

    std::optional<QueryHit> castSphere(const World& world, core::vec3 origin, float radius, core::vec3 direction,
                                       float maxDistance, uint32_t layers, Entity ignore, uint32_t skipGroups) {
        return resolve(world, physicsWorld(world).castSphere(origin, radius, direction, maxDistance,
                                                             filterFor(world, layers, ignore, skipGroups)));
    }

    std::vector<Entity> overlapSphere(const World& world, core::vec3 centre, float radius, uint32_t layers, Entity ignore, uint32_t skipGroups) {
        return entitiesOf(world, physicsWorld(world).overlapSphere(centre, radius, filterFor(world, layers, ignore, skipGroups)));
    }

    std::vector<Entity> overlapBox(const World& world, core::vec3 centre, core::vec3 halfExtents, core::quat rotation,
                                   uint32_t layers, Entity ignore, uint32_t skipGroups) {
        return entitiesOf(world, physicsWorld(world).overlapBox(centre, halfExtents, rotation, filterFor(world, layers, ignore, skipGroups)));
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
    if (!b.material.empty()) j["material"] = b.material;
    if (b.breaksOnImpact) j["breaksOnImpact"] = true;
    if (b.senses != defaults.senses) j["senses"] = layerNames(b.senses);
    if (b.blockedByStatic == projv::runtime::KinematicBlocking::Stop) j["blockedByStatic"] = "stop";
    if (b.blockedByStatic == projv::runtime::KinematicBlocking::Slide) j["blockedByStatic"] = "slide";
    if (b.minImpactSpeed >= 0.0f) j["minImpactSpeed"] = tidy(b.minImpactSpeed);
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
    b.material = j.value("material", std::string());
    b.breaksOnImpact = j.value("breaksOnImpact", false);
    if (j.contains("senses")) {
        auto senses = layersFrom(j.at("senses"));
        if (!senses) return std::nullopt;
        b.senses = *senses;
    }
    std::string blocked = j.value("blockedByStatic", "none");
    if (blocked == "stop") b.blockedByStatic = projv::runtime::KinematicBlocking::Stop;
    else if (blocked == "slide") b.blockedByStatic = projv::runtime::KinematicBlocking::Slide;
    else if (blocked != "none") return std::nullopt;
    b.minImpactSpeed = j.value("minImpactSpeed", -1.0f);
    return b;
}

nlohmann::json projv::runtime::ComponentTraits<projv::StaticCollider>::save(const projv::StaticCollider& c) {
    const projv::StaticCollider defaults;
    nlohmann::json j = nlohmann::json::object();
    if (c.friction != defaults.friction) j["friction"] = tidy(c.friction);
    if (c.restitution != defaults.restitution) j["restitution"] = tidy(c.restitution);
    if (c.layer != defaults.layer) j["layer"] = layerName(c.layer);
    if (c.splits) j["splits"] = true;
    if (c.anchorBase) j["anchorBase"] = true;
    if (!c.material.empty()) j["material"] = c.material;
    if (c.breaksOnImpact) j["breaksOnImpact"] = true;
    if (c.senses != defaults.senses) j["senses"] = layerNames(c.senses);
    if (c.minImpactSpeed >= 0.0f) j["minImpactSpeed"] = tidy(c.minImpactSpeed);
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
    c.anchorBase = j.value("anchorBase", false);
    c.material = j.value("material", std::string());
    c.breaksOnImpact = j.value("breaksOnImpact", false);
    if (j.contains("senses")) {
        auto senses = layersFrom(j.at("senses"));
        if (!senses) return std::nullopt;
        c.senses = *senses;
    }
    c.minImpactSpeed = j.value("minImpactSpeed", -1.0f);
    return c;
}

nlohmann::json projv::runtime::ComponentTraits<projv::ContactReports>::save(const projv::ContactReports& r) {
    nlohmann::json j = nlohmann::json::object();
    if (!r.only.empty()) j["only"] = r.only;
    return j;
}

std::optional<projv::ContactReports> projv::runtime::ComponentTraits<projv::ContactReports>::load(const nlohmann::json& j,
                                                                                                uint32_t) {
    projv::ContactReports r;
    if (!j.contains("only")) return r;
    const nlohmann::json& list = j.at("only");
    if (!list.is_array()) return std::nullopt;
    for (const nlohmann::json& name : list) {
        if (!name.is_string()) return std::nullopt;
        r.only.push_back(name.get<std::string>());
    }
    return r;
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

nlohmann::json projv::runtime::ComponentTraits<projv::CollisionGroups>::save(const projv::CollisionGroups& g) {
    nlohmann::json j = nlohmann::json::object();
    if (!g.groups.empty()) j["groups"] = g.groups;
    if (!g.ignores.empty()) j["ignores"] = g.ignores;
    if (g.family) j["family"] = g.family;
    return j;
}

std::optional<projv::CollisionGroups> projv::runtime::ComponentTraits<projv::CollisionGroups>::load(const nlohmann::json& j,
                                                                                                  uint32_t) {
    projv::CollisionGroups g;
    auto names = [&](const char* key, std::vector<std::string>& out) {
        if (!j.contains(key)) return true;
        const nlohmann::json& list = j.at(key);
        if (!list.is_array()) return false;
        for (const nlohmann::json& name : list) {
            if (!name.is_string()) return false;
            out.push_back(name.get<std::string>());
        }
        return true;
    };
    if (!names("groups", g.groups) || !names("ignores", g.ignores)) return std::nullopt;
    g.family = j.value("family", 0u);
    return g;
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
