#include "runtime/loading.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_map>

#include "core/log.h"
#include "runtime/physics.h"
#include "runtime/scene_bridge.h"
#include "utils/scene_query.h"

namespace projv::runtime {
    namespace {
        // An entity that is not in the registry, and what makes it again.
        struct Unloaded {
            std::optional<Authored> authored;
            ComponentRef    link;            // empty: unlinked
            LinkMode        mode = LinkMode::Root;
            OnUnlink        onUnlink = OnUnlink::Keep;
            NetId           netId;
            LoadBounds      bounds;          // unlinked ones are judged by it
            core::vec3      position{0.0f};
            nlohmann::json  components;
            utils::Presence presence = utils::Presence::Absent;
        };

        struct WorldLoading {
            std::unique_ptr<utils::SceneLoader> loader;
            std::vector<utils::Viewer>          programViewers;
            std::vector<Unloaded>               unloaded;
        };

        WorldLoading* stateOf(World& world) { return world.ctx().find<WorldLoading>(); }
        const WorldLoading* stateOf(const World& world) { return world.ctx().find<WorldLoading>(); }

        // Every component with blocks (and every ancestor of one), with the most any loaded block
        // under it is present as. A component missing from the map has no blocks under it.
        std::unordered_map<ComponentHandle, utils::Presence> componentPresence(const Scene& scene,
                                                                               const utils::SceneLoader& loader) {
            std::unordered_map<ComponentHandle, utils::Presence> out;
            const utils::SceneBlocks& blocks = loader.blocks();
            for (utils::BlockId id = 0; id < blocks.size(); id++) {
                ComponentHandle h = blocks.source(id).component;
                if (!utils::isComponentAlive(scene, h)) continue;
                utils::Presence p = blocks.loaded(id) ? loader.presence(id) : utils::Presence::Absent;
                if (blocks.loaded(id) && p == utils::Presence::Absent) p = utils::Presence::Kept;   // still going
                for (; h != INVALID_COMPONENT_HANDLE && h < scene.components.size(); h = scene.components[h].parent) {
                    auto [it, inserted] = out.emplace(h, p);
                    if (!inserted) {
                        if (it->second >= p) break;   // an ancestor already has as much
                        it->second = p;
                    }
                }
            }
            return out;
        }

        core::vec3 positionOf(const World& world, const Scene& scene, Entity e) {
            if (const VoxelComponent* link = world.try_get<VoxelComponent>(e)) {
                ComponentHandle h = utils::resolve(scene, link->ref());
                if (h != INVALID_COMPONENT_HANDLE) return utils::getComponentWorldPosition(scene, h);
            }
            if (const Transform* t = world.try_get<Transform>(e)) return t->position;
            return core::vec3(0.0f);
        }

        utils::Presence judgeBounds(const utils::SceneLoader& loader, std::span<const utils::Viewer> viewers,
                                    const utils::LoadPolicy& policy, core::vec3 position, const LoadBounds& bounds,
                                    Entity e, utils::Presence current) {
            utils::LoadCandidate c;
            c.id = std::numeric_limits<utils::BlockId>::max();
            c.bounds = {position - bounds.halfExtents, position + bounds.halfExtents};
            c.current = current;
            c.entity = static_cast<uint32_t>(e);
            if (policy.hint.radius > 0.0f) {
                bool near = false;
                for (const utils::Viewer& v : viewers) near = near || utils::distanceToBox(c.bounds, v.position) <= policy.hint.radius;
                if (!near) return utils::Presence::Absent;
            }
            (void)loader;
            return policy.rule(c, viewers);
        }

        struct Policy {
            utils::LoadPolicy policy = utils::loadEverything();
        };

        void unload(World& world, WorldLoading& state, const Scene& scene, Entity e) {
            Unloaded u;
            if (const Authored* a = world.try_get<Authored>(e)) u.authored = *a;
            if (const NetId* id = world.try_get<NetId>(e)) u.netId = *id;
            if (const LoadBounds* b = world.try_get<LoadBounds>(e)) u.bounds = *b;
            u.position = positionOf(world, scene, e);
            VoxelComponent* link = world.try_get<VoxelComponent>(e);
            if (link) {
                u.link = link->ref();
                u.mode = link->mode;
                u.onUnlink = link->onUnlink;
            }
            // A moving body keeps its motion: physics gives FrozenMotion back when it is Active.
            if (hasBody(world, e) && !world.all_of<FrozenMotion>(e)) {
                BodyState state = bodyPose(world, e);
                if (const PhysicsBody* body = world.try_get<PhysicsBody>(e); body && body->motion == MotionType::Dynamic)
                    world.emplace<FrozenMotion>(e, FrozenMotion{state.linearVelocity, state.angularVelocity});
            }
            u.components = entityComponents(world, e, link == nullptr);

            if (Events* events = world.ctx().find<Events>()) events->trigger(EntityUnloading{e, u.netId});
            // Its component stays: the entity is going, the voxels are only unloaded.
            if (world.valid(e) && (link = world.try_get<VoxelComponent>(e))) link->onUnlink = OnUnlink::Keep;
            if (world.valid(e)) world.destroy(e);
            state.unloaded.push_back(std::move(u));
        }

        Entity reload(World& world, const Scene& scene, Unloaded& u) {
            Entity e = world.create();
            if (u.authored) world.emplace<Authored>(e, *u.authored);
            ComponentHandle h = utils::resolve(scene, u.link);
            if (h != INVALID_COMPONENT_HANDLE && !linkComponent(world, e, h, u.mode, u.onUnlink)) {
                core::warn("world loading: an entity could not be linked again to '{}' and comes back unlinked",
                           scene.components[h].name);
            }
            applyEntityComponents(world, e, u.components, "an unloaded entity");
            if (u.netId.value != 0 && !world.all_of<NetId>(e)) world.emplace<NetId>(e, u.netId);
            world.emplace_or_replace<LoadState>(e, LoadState{u.presence});
            if (Events* events = world.ctx().find<Events>()) events->send(EntityReloaded{e, u.netId});
            return e;
        }
    }

    bool openWorld(Application& app, const std::string& folder, const WorldLoadingConfig& config) {
        World& world = app.world;
        installSceneBridge(app);
        registerComponent<LoadViewer>(world);
        registerComponent<LoadBounds>(world);
        Scene& scene = world.ctx().contains<Scene>() ? world.ctx().get<Scene>() : world.ctx().emplace<Scene>();
        bool installed = world.ctx().contains<WorldLoading>();
        WorldLoading& state = installed ? world.ctx().get<WorldLoading>() : world.ctx().emplace<WorldLoading>();
        state.loader = std::make_unique<utils::SceneLoader>(config.loader);
        state.unloaded.clear();
        bool ok = state.loader->open(scene, folder);
        state.loader->setRule(config.policy);
        if (!world.ctx().contains<Policy>()) world.ctx().emplace<Policy>();
        world.ctx().get<Policy>().policy = config.policy;
        if (!installed) {
            app.addSystem(Stage::PreUpdate, "world loading", [](Application& a) { updateWorldLoading(a.world); });
        }
        if (!ok) core::error("openWorld: {} did not open", folder);
        return ok;
    }

    utils::SceneLoader& worldLoader(World& world) { return *stateOf(world)->loader; }
    bool hasWorldLoading(const World& world) {
        const WorldLoading* state = stateOf(world);
        return state && state->loader;
    }

    void setLoadRule(World& world, utils::LoadPolicy policy) {
        WorldLoading* state = stateOf(world);
        if (!state) return;
        if (!policy.rule) policy = utils::loadEverything();
        world.ctx().get<Policy>().policy = policy;
        state->loader->setRule(std::move(policy));
    }

    void setViewers(World& world, std::vector<utils::Viewer> viewers) {
        if (WorldLoading* state = stateOf(world)) state->programViewers = std::move(viewers);
    }

    size_t unloadedEntityCount(const World& world) {
        const WorldLoading* state = stateOf(world);
        return state ? state->unloaded.size() : 0;
    }

    void updateWorldLoading(World& world) {
        WorldLoading* state = stateOf(world);
        Scene* scene = world.ctx().find<Scene>();
        if (!state || !state->loader || !scene) return;
        const utils::LoadPolicy& policy = world.ctx().get<Policy>().policy;

        // 1. Viewers: the program's, then every LoadViewer entity's, in entity order.
        std::vector<utils::Viewer> viewers = state->programViewers;
        {
            std::vector<std::pair<Entity, uint32_t>> found;
            for (auto [e, v] : world.view<LoadViewer>().each()) found.push_back({e, v.id});
            std::sort(found.begin(), found.end());
            for (auto [e, id] : found) viewers.push_back({positionOf(world, *scene, e), id});
        }
        state->loader->setViewers(viewers);

        // 2. Blocks.
        state->loader->update(*scene);

        // 3. Entities out: those taking part whose blocks have gone, or whose bounds the rule drops.
        auto presence = componentPresence(*scene, *state->loader);
        std::vector<std::pair<Entity, utils::Presence>> judged;
        auto judge = [&](Entity e) -> std::optional<utils::Presence> {
            if (world.all_of<LoadViewer>(e)) return std::nullopt;
            const VoxelComponent* link = world.try_get<VoxelComponent>(e);
            if (link) {
                ComponentHandle h = utils::resolve(*scene, link->ref());
                if (h == INVALID_COMPONENT_HANDLE) return std::nullopt;
                if (!world.all_of<Authored>(e) && !world.all_of<LoadBounds>(e)) return std::nullopt;
                auto it = presence.find(h);
                if (it == presence.end()) return std::nullopt;   // no blocks under it
                return it->second;
            }
            const LoadBounds* bounds = world.try_get<LoadBounds>(e);
            if (!bounds) return std::nullopt;
            const LoadState* current = world.try_get<LoadState>(e);
            return judgeBounds(*state->loader, viewers, policy, positionOf(world, *scene, e), *bounds, e,
                               current ? current->presence : utils::Presence::Active);
        };
        std::vector<Entity> candidates;
        for (auto e : world.view<Authored>()) candidates.push_back(e);
        for (auto e : world.view<LoadBounds>()) if (!world.all_of<Authored>(e)) candidates.push_back(e);
        std::sort(candidates.begin(), candidates.end());
        std::vector<Entity> going;
        for (Entity e : candidates) {
            std::optional<utils::Presence> p = judge(e);
            if (!p) continue;
            if (*p == utils::Presence::Absent) {
                going.push_back(e);
            } else {
                const LoadState* current = world.try_get<LoadState>(e);
                if (!current || current->presence != *p) world.emplace_or_replace<LoadState>(e, LoadState{*p});
            }
        }
        for (Entity e : going) {
            if (world.valid(e)) unload(world, *state, *scene, e);
        }

        // 4. Entities back, in the order they went.
        std::vector<Unloaded> still;
        std::vector<Unloaded> back;
        for (Unloaded& u : state->unloaded) {
            if (u.link.handle != INVALID_COMPONENT_HANDLE) {
                ComponentHandle h = utils::resolve(*scene, u.link);
                if (h == INVALID_COMPONENT_HANDLE) continue;   // its component was deleted: it is gone
                auto it = presence.find(h);
                u.presence = it == presence.end() ? utils::Presence::Active : it->second;
            } else {
                u.presence = judgeBounds(*state->loader, viewers, policy, u.position, u.bounds, NullEntity, u.presence);
            }
            (u.presence == utils::Presence::Absent ? still : back).push_back(std::move(u));
        }
        state->unloaded = std::move(still);
        for (Unloaded& u : back) reload(world, *scene, u);
    }

    void settleWorld(World& world) {
        WorldLoading* state = stateOf(world);
        Scene* scene = world.ctx().find<Scene>();
        if (!state || !scene) return;
        // Twice: the second pass sees the entities the first brought back (a viewer among them).
        for (int pass = 0; pass < 2; pass++) {
            updateWorldLoading(world);
            state->loader->settle(*scene);
            updateWorldLoading(world);
        }
    }
}
