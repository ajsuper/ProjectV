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
            uint32_t                      maxDebris = 3000;
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
            };
            std::vector<Edit>             edits;
            // The physics material library: names in definition order (index = position + 1).
            struct Library {
                std::vector<std::string>        names;
                std::map<std::string, uint16_t> index;
                uint64_t                        version = 0;   // bumped by every definition
                std::set<std::string>           warned;        // unknown names, logged once each
            } library;
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
            } else {
                d.layer = fixed->layer;
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

        // A strike half applied: its shards' components are made and the edits that fill them and cut
        // them from the body are queued. What is left (finishBreak) waits for those edits: a step's
        // strikes share one updateScene.
        struct PendingBreak {
            PhysicsRuntime::Edit strike;
            struct Made { ComponentHandle component; core::vec3 centre; size_t voxels; };
            std::vector<Made> made;
            RigidBody shardRigid;
            OnUnlink onUnlink = OnUnlink::Destroy;
            bool networked = false, struckMoves = false;
            BodyState state;
            float originalMass = 0.0f, edge = 0.0f, strength = 0.0f, metres = 0.0f, reach = 0.0f;
            uint64_t total = 0, broken = 0;
        };

        // A strike, applied: see runtime::fracture. Its first half: false when nothing breaks.
        bool breakBody(World& world, PhysicsRuntime& physics, Scene& scene, const PhysicsRuntime::Edit& strike,
                       PendingBreak& out) {
            const Entity entity = strike.entity;
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
            const core::vec3 at = core::vec3(glm::inverse(toWorld) * core::vec4(strike.s.at, 1.0f)) / source.voxel;

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
            float reach = 0.0f;
            if (strike.s.reach > 0.0f) reach = strike.s.reach / metres;
            else if (strike.s.energy > 0.0f) reach = std::cbrt(3.0f * strike.s.energy / (4.0f * 3.14159265f * material.strength)) / metres;
            else return false;
            if (reach > limits.maxReachVoxels) {
                core::trace("physics: fracture of entity {}: reach {} voxels capped at {}", static_cast<uint32_t>(entity), reach,
                            limits.maxReachVoxels);
                reach = limits.maxReachVoxels;
            }
            if (reach < 0.5f) return false;   // not enough to break a voxel
            if (strike.s.minimumBreak > 0.0f && reach < strike.s.minimumBreak * material.shardSize / metres) return false;   // a knock
            const float outer = 1.5f * reach;

            std::vector<FractureVoxel> voxels = voxelsNear(scene, source, at, outer, maxCells, decoded);
            for (FractureVoxel& v : voxels) {
                v.material = materialOf(v.slot);
                v.breakable = breakable(v.material);
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
            seed.add(strike.s.at);
            seed.add(strike.s.energy);
            if (strike.s.reach > 0.0f) seed.add(strike.s.reach);
            FractureRandom random(seed.value);
            const FractureInput input{voxels, at, reach, outer, material.shardSize / metres, material.grain, material};
            std::vector<int32_t> shardOf = (*pattern)(input, random);
            if (shardOf.size() != voxels.size()) {
                core::warn("physics: fracture pattern '{}' answered for {} voxels of {}; nothing breaks", material.fracture,
                           shardOf.size(), voxels.size());
                return false;
            }
            std::vector<std::vector<size_t>> shards = fractureShards(input, shardOf, 64, limits.maxShards);
            if (shards.empty()) return false;

            // The shards: each a new top-level chunk holding its voxels, with the body's palette,
            // placed where they were.
            const std::vector<Material> palette = scene.components[owner].materialPalette;
            const core::quat rotation = utils::getComponentWorldRotation(scene, h);
            const std::string name = scene.components[h].name;
            const BodyState state = sim.bodyState(body->id);
            std::vector<PendingBreak::Made> made;
            std::vector<PendingVoxelOp> cut;
            for (const std::vector<size_t>& shard : shards) {
                core::vec3 centre;
                ComponentHandle piece = pieceOf(scene, palette, voxels, shard, toWorld, rotation, scale, source.voxel,
                                                name + " shard", true, centre);
                if (piece == INVALID_COMPONENT_HANDLE) continue;
                for (size_t i : shard) cut.push_back({false, voxels[i].position, 0});
                made.push_back({piece, centre, shard.size()});
            }
            utils::queueVoxelRemove(scene, h, cut);

            // What the shards are: a dynamic body's own RigidBody, or one from the static collider.
            PendingBreak& p = out;
            p.strike = strike;
            p.made = std::move(made);
            if (rigid) {
                p.shardRigid = *rigid;
                p.shardRigid.motion = MotionType::Dynamic;
            } else {
                p.shardRigid.friction = fixed->friction;
                p.shardRigid.restitution = fixed->restitution;
                p.shardRigid.material = fixed->material;
                p.shardRigid.breaksOnImpact = fixed->breaksOnImpact;
            }
            if (strike.s.shardsBreakOnImpact) p.shardRigid.breaksOnImpact = *strike.s.shardsBreakOnImpact;
            p.struckMoves = body->motion == MotionType::Dynamic;
            if (rigid && rigid->mass > 0.0f) {
                for (const VoxelSource::Part& part : source.parts) {
                    utils::VoxelOccupancy o;
                    const Chunk& chunk = scene.chunks[part.chunk];
                    if (chunk.geometryPoolIndex >= 0 &&
                        utils::decodeOccupancy(scene.geometryPool[size_t(chunk.geometryPoolIndex)], part.resolution, maxCells, o) ==
                            utils::DecodeResult::Ok)
                        p.total += o.solid;   // before the cut, which is only queued
                }
            }
            for (const PendingBreak::Made& m : p.made) p.broken += m.voxels;
            p.onUnlink = link->onUnlink;
            p.networked = net != nullptr;
            p.state = state;
            p.originalMass = rigid ? rigid->mass : 0.0f;
            p.edge = outer * metres;
            p.strength = material.strength;
            p.metres = metres;
            p.reach = reach;
            return true;
        }

        // A strike's second half, once its edits are applied: the shards made bodies, the event sent,
        // the striker given back what the break did not use.
        void finishBreak(World& world, PhysicsRuntime& physics, const PendingBreak& p) {
            const Strike& strike = p.strike.s;
            const Entity entity = p.strike.entity;
            const uint64_t total = std::max<uint64_t>(p.total, 1);
            BodyFractured event{entity, {}, strike.at, strike.striker, strike.energy};
            for (const PendingBreak::Made& m : p.made) {
                Entity e = world.create();
                if (!linkComponent(world, e, m.component, LinkMode::Root, p.onUnlink)) {
                    world.destroy(e);
                    continue;
                }
                RigidBody r = p.shardRigid;
                if (p.originalMass > 0.0f) r.mass = p.originalMass * float(m.voxels) / float(total);
                float falloff = std::max(0.0f, 1.0f - glm::distance(m.centre, strike.at) / std::max(p.edge, 1e-6f));
                // From the motion the strike says (an impact's: the body's before the hit, which the
                // contact has since stopped), or the motion it has.
                const core::vec3 base = strike.bodyVelocity.value_or(p.state.linearVelocity);
                core::vec3 velocity = base + glm::cross(p.state.angularVelocity, m.centre - p.state.centerOfMass) + strike.push * falloff;
                physics.carried[e] = {velocity, p.state.angularVelocity};
                if (p.networked) assignNetId(world, e);
                world.emplace<Debris>(e, Debris{physics.world->tick()});
                world.emplace<RigidBody>(e, r);
                event.shards.push_back(e);
            }
            if (p.originalMass > 0.0f && world.valid(entity) && world.all_of<RigidBody>(entity)) {
                float kept = p.originalMass * float(total - std::min(p.broken, total)) / float(total);
                world.patch<RigidBody>(entity, [kept](RigidBody& rb) { rb.mass = std::max(kept, 1e-3f); });
            }
            // What struck something that does not move carries on with the energy the break did not
            // use: breaking a voxel costs the struck material's strength times its volume.
            if (strike.returnUnusedEnergy && strike.energy > 0.0f && !p.struckMoves && world.valid(strike.striker)) {
                if (const PhysicsBody* striker = world.try_get<PhysicsBody>(strike.striker);
                    striker && striker->motion == MotionType::Dynamic) {
                    const float used = p.strength * float(p.broken) * p.metres * p.metres * p.metres;
                    const float kept = std::sqrt(std::clamp(1.0f - used / strike.energy, 0.0f, 1.0f));
                    BodyState now = physics.world->bodyState(striker->id);
                    physics.world->setVelocity(striker->id, strike.strikerVelocity * kept, now.angularVelocity);
                }
            }
            core::trace("physics: entity {} broke into {} shards ({} voxels, reach {:.1f})", static_cast<uint32_t>(entity),
                        event.shards.size(), p.broken, p.reach);
            send(world, std::move(event));
        }

        // ---- Support ----------------------------------------------------------------------------

        constexpr uint64_t MAX_SUPPORT_CELLS = 1ull << 24;   // the box support is worked out in, at most
        constexpr size_t   MAX_FALLING_PIECES = 512;

        // A static collider that splits (StaticCollider::splits), after its voxels changed: every
        // face-connected island of them with nothing to hold it up falls, as a dynamic body of its
        // own (BodySplit). What holds up: voxels of an anchor material (PhysicsMaterial::anchor), and
        // with StaticCollider::anchorBase the component's own y = 0 and below. A collider with
        // neither drops nothing (and says so, once). Applied (updateScene) before returning.
        void dropUnsupported(World& world, PhysicsRuntime& physics, Scene& scene, Entity entity) {
            const StaticCollider* fixed = world.try_get<StaticCollider>(entity);
            const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
            if (!fixed || !fixed->splits || !link) return;
            const ComponentHandle h = voxelComponentOf(world, entity);
            if (h == INVALID_COMPONENT_HANDLE) return;
            const VoxelSource source = voxelSourceOf(scene, h);
            if (source.parts.empty() || !(source.voxel > 0.0f)) return;

            // Every voxel, and a box over them to look neighbours up in.
            std::map<size_t, Decoded> decoded;
            core::ivec3 low(std::numeric_limits<int>::max()), high(std::numeric_limits<int>::min());
            for (const VoxelSource::Part& part : source.parts) {
                low = glm::min(low, part.offset);
                high = glm::max(high, part.offset + core::ivec3(int(part.resolution) - 1));
            }
            const core::ivec3 dims = high - low + core::ivec3(1);
            const uint64_t cells = uint64_t(dims.x) * uint64_t(dims.y) * uint64_t(dims.z);
            if (cells > MAX_SUPPORT_CELLS) {
                core::warn("physics: entity {} is too big ({} cells) to work out what holds it up; nothing falls",
                           static_cast<uint32_t>(entity), cells);
                return;
            }
            const core::vec3 middle = 0.5f * core::vec3(low + high + core::ivec3(1));
            const float radius = glm::length(core::vec3(dims)) * 0.5f + 1.0f;
            std::vector<FractureVoxel> voxels = voxelsNear(scene, source, middle, radius, physics.collision.maxDecodedCells, decoded);
            if (voxels.empty()) return;
            auto at = [&](core::ivec3 p) {
                p -= low;
                return (size_t(p.z) * size_t(dims.y) + size_t(p.y)) * size_t(dims.x) + size_t(p.x);
            };
            std::vector<int32_t> index(size_t(cells), -1);
            for (size_t i = 0; i < voxels.size(); i++) index[at(voxels[i].position)] = int32_t(i);
            static const core::ivec3 faces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            auto neighbours = [&](size_t i, auto visit) {
                for (core::ivec3 f : faces) {
                    core::ivec3 n = voxels[i].position + f;
                    if (glm::any(glm::lessThan(n, low)) || glm::any(glm::greaterThan(n, high))) continue;
                    if (int32_t j = index[at(n)]; j >= 0) visit(size_t(j));
                }
            };

            // What holds it up: anchor materials, and its base when asked.
            const ComponentHandle owner = paletteOwner(scene, scene.chunks[source.parts.front().chunk]);
            utils::SlotMaterials slotMaterials;
            slotMaterialsOf(physics, scene, owner, slotMaterials);
            const uint16_t own = ownMaterial(world, physics, entity);
            std::array<uint8_t, 256> anchorSlot{};
            for (size_t slot = 0; slot < 256; slot++) {
                uint16_t m = slotMaterials[slot] ? slotMaterials[slot] : own;
                anchorSlot[slot] = m != 0 && physics.world->material(m).anchor;
            }
            std::vector<uint8_t> held(voxels.size(), 0);
            std::vector<size_t> stack;
            for (size_t i = 0; i < voxels.size(); i++)
                if ((fixed->anchorBase && voxels[i].position.y <= 0) || anchorSlot[voxels[i].slot]) {
                    held[i] = 1;
                    stack.push_back(i);
                }
            if (stack.empty()) {
                // Nothing holds it up, so everything would fall: a collider set up wrong, not a collapse.
                if (physics.warnedUnanchored.insert(entity).second)
                    core::warn("physics: entity {} splits but has nothing to stand on (no anchor material, and anchorBase "
                               "off); nothing falls", static_cast<uint32_t>(entity));
                return;
            }
            while (!stack.empty()) {
                size_t i = stack.back();
                stack.pop_back();
                neighbours(i, [&](size_t j) { if (!held[j]) { held[j] = 1; stack.push_back(j); } });
            }
            // The rest, island by island, in voxel order.
            std::vector<std::vector<size_t>> islands;
            for (size_t i = 0; i < voxels.size() && islands.size() < MAX_FALLING_PIECES; i++) {
                if (held[i]) continue;
                std::vector<size_t> island;
                held[i] = 1;
                stack.push_back(i);
                while (!stack.empty()) {
                    size_t k = stack.back();
                    stack.pop_back();
                    island.push_back(k);
                    neighbours(k, [&](size_t j) { if (!held[j]) { held[j] = 1; stack.push_back(j); } });
                }
                std::sort(island.begin(), island.end());
                islands.push_back(std::move(island));
            }
            if (islands.empty()) return;

            const core::mat4 toWorld = utils::getComponentWorldMatrix(scene, h);
            const float scale = glm::length(core::vec3(toWorld[0]));
            const core::quat rotation = utils::getComponentWorldRotation(scene, h);
            const std::vector<Material> palette = scene.components[owner].materialPalette;
            const std::string name = scene.components[h].name;
            std::vector<ComponentHandle> pieces;
            std::vector<PendingVoxelOp> cut;
            for (const std::vector<size_t>& island : islands) {
                core::vec3 centre;
                ComponentHandle piece = pieceOf(scene, palette, voxels, island, toWorld, rotation, scale, source.voxel,
                                                name + " piece", false, centre);
                if (piece == INVALID_COMPONENT_HANDLE) continue;
                for (size_t i : island) cut.push_back({false, voxels[i].position, 0});
                pieces.push_back(piece);
            }
            utils::queueVoxelRemove(scene, h, cut);
            utils::updateScene(scene);

            // Each falls as what the wall was made of.
            RigidBody fall;
            fall.friction = fixed->friction;
            fall.restitution = fixed->restitution;
            fall.material = fixed->material;
            fall.breaksOnImpact = fixed->breaksOnImpact;
            const OnUnlink onUnlink = link->onUnlink;
            const bool networked = world.all_of<NetId>(entity);
            BodySplit event{entity, {}};
            for (ComponentHandle piece : pieces) {
                Entity e = world.create();
                if (!linkComponent(world, e, piece, LinkMode::Root, onUnlink)) {
                    world.destroy(e);
                    continue;
                }
                if (networked) assignNetId(world, e);
                world.emplace<RigidBody>(e, fall);
                event.pieces.push_back(e);
            }
            core::trace("physics: {} pieces of entity {} lost their support and fall", event.pieces.size(),
                        static_cast<uint32_t>(entity));
            send(world, std::move(event));
        }

        // The debris budget: past PhysicsConfig::maxDebris shards, the oldest go, their bodies at once.
        void clearDebris(World& world, PhysicsRuntime& physics) {
            if (physics.maxDebris == 0) return;
            auto debris = world.view<Debris>();
            const size_t count = debris.size();
            if (count <= physics.maxDebris) return;
            std::vector<std::tuple<uint64_t, uint32_t, Entity>> order;
            order.reserve(count);
            for (auto [entity, d] : debris.each()) {
                const NetId* id = world.try_get<NetId>(entity);
                order.push_back({d.madeAt, id ? id->value : 0u, entity});
            }
            std::sort(order.begin(), order.end());
            const size_t excess = count - physics.maxDebris;
            DebrisCleared event;
            event.entities.reserve(excess);
            for (size_t i = 0; i < excess; i++) event.entities.push_back(std::get<2>(order[i]));
            // Delivered now, not at the next pump: a handler has to see them before they go, to keep
            // one (by taking its Debris off) or put something in its place.
            if (Events* events = world.ctx().find<Events>()) events->trigger(event);
            size_t cleared = 0;
            for (Entity e : event.entities)
                if (world.valid(e) && world.all_of<Debris>(e)) {
                    world.destroy(e);
                    cleared++;
                }
            for (BodyId id : physics.doomed) physics.world->destroyBody(id);
            physics.doomed.clear();
            core::trace("physics: cleared {} shards of debris, the oldest (budget {}); {} kept", cleared, physics.maxDebris,
                        excess - cleared);
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
                // Strikes are applied in halves around one updateScene for the lot. A strike sees what
                // was edited before it: a plain edit, or a strike on the same body, flushes first.
                std::vector<PendingBreak> breaks;
                std::set<Entity> struck;
                bool queued = false;
                auto flush = [&] {
                    utils::updateScene(*scene);
                    for (const PendingBreak& b : breaks) finishBreak(world, physics, b);
                    breaks.clear();
                    struck.clear();
                    queued = false;
                };
                for (const auto& edit : edits) {
                    if (edit.strike) {
                        if (queued || struck.count(edit.entity)) flush();
                        PendingBreak b;
                        if (breakBody(world, physics, *scene, edit, b)) {
                            breaks.push_back(std::move(b));
                            struck.insert(edit.entity);
                        }
                        continue;
                    }
                    queued = true;
                    ComponentHandle target = voxelComponentOf(world, edit.entity);
                    if (target == INVALID_COMPONENT_HANDLE) continue;
                    std::vector<PendingVoxelOp> adds, removes;
                    for (const PendingVoxelOp& op : edit.ops) (op.isAdd ? adds : removes).push_back(op);
                    if (!adds.empty()) utils::queueVoxelAdd(*scene, target, adds);
                    if (!removes.empty()) utils::queueVoxelRemove(*scene, target, removes);
                }
                flush();
                clearDebris(world, physics);
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
                    } else {
                        dropUnsupported(world, physics, *scene, entity);
                    }
                    physics.rebuild.push_back(entity);
                    physics.reshaped.insert(entity);
                }
                physics.seenStamp = latestBlobContentStamp();
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
                // Dynamic bodies, and kinematic ones flying on a ConstantVelocity: the simulation moves
                // those too, so it writes their Transform. (Other kinematic bodies follow theirs.)
                bool simulated = body.motion == MotionType::Dynamic ||
                                 (body.motion == MotionType::Kinematic && world.all_of<ConstantVelocity>(entity));
                if (!simulated || !world.all_of<Transform>(entity)) continue;
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
        physics.maxDebris = config.maxDebris;
        {
            // Guards, clamped rather than refused: a bad value must not leave fracture unguarded.
            PhysicsConfig::FractureLimits want = config.fracture, &got = physics.fractureLimits;
            auto finiteOr = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
            got.searchVoxels = std::clamp(finiteOr(want.searchVoxels, 3.0f), 0.5f, 16.0f);
            got.maxReachVoxels = std::clamp(finiteOr(want.maxReachVoxels, 64.0f), 1.0f, 256.0f);
            got.maxShards = std::clamp<uint32_t>(want.maxShards, 1, 4096);
            if (got.searchVoxels != want.searchVoxels || got.maxReachVoxels != want.maxReachVoxels ||
                got.maxShards != want.maxShards)
                core::warn("physics: fracture limits out of range, clamped to search {} voxels, reach {} voxels, {} shards",
                           got.searchVoxels, got.maxReachVoxels, got.maxShards);
        }
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

    void fracture(World& world, Entity entity, const Strike& strike) {
        auto finiteVec = [](core::vec3 v) { return std::isfinite(glm::dot(v, v)); };
        if (!finiteVec(strike.at) || !finiteVec(strike.push) || !finiteVec(strike.strikerVelocity) ||
            (strike.bodyVelocity && !finiteVec(*strike.bodyVelocity)) || !std::isfinite(strike.energy) ||
            !std::isfinite(strike.reach) || !std::isfinite(strike.minimumBreak)) {
            core::warn("physics: refused a fracture with a value that is not finite");
            return;
        }
        if (strike.energy < 0.0f || strike.reach < 0.0f) {
            core::warn("physics: refused a fracture with a negative energy or reach");
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
    if (!b.material.empty()) j["material"] = b.material;
    if (b.breaksOnImpact) j["breaksOnImpact"] = true;
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
