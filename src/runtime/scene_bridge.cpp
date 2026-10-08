#include "runtime/scene_bridge.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/log.h"
#include "utils/scene_query.h"

namespace projv::runtime {
    namespace {
        // The bridge's own state, in world.ctx().
        struct BridgeState {
            uint64_t seenDeletions = 0;
            std::vector<detail::RawSpawnHandler> componentHandlers;
            std::vector<detail::RawSpawnHandler> documentHandlers;
        };

        Scene* sceneOf(World& world) {
            return world.ctx().find<Scene>();
        }

        void send(World& world, auto event) {
            if (Events* events = world.ctx().find<Events>()) events->send(event);
        }

        Transform transformOf(const ComponentRecord& record) {
            return Transform{record.localPosition, record.localRotation, record.localScale};
        }

        core::mat4 matrixOf(const Transform& t) {
            return glm::translate(core::mat4(1.0f), t.position) * glm::mat4_cast(t.rotation) *
                   glm::scale(core::mat4(1.0f), core::vec3(t.scale));
        }

        // ---- Signals ----

        void markDirty(World& world, Entity entity) {
            world.emplace_or_replace<TransformDirty>(entity);
        }

        // A new link takes its Transform from the component, so linking moves nothing. The dirty mark
        // that emplacing the Transform raises is cleared again: the values came *from* the Scene, and
        // writing them back would be a rebake for no change.
        void onLinked(World& world, Entity entity) {
            Scene* scene = sceneOf(world);
            const VoxelComponent& link = world.get<VoxelComponent>(entity);
            if (!scene || !utils::isComponentAlive(*scene, link.handle)) {
                core::error("scene bridge: entity {} links component {}, which is not alive - use "
                            "runtime::linkComponent, which refuses it",
                            static_cast<uint32_t>(entity), link.handle);
                return;
            }
            const ComponentRecord& record = scene->components[link.handle];
            Transform seeded = transformOf(record);
            world.emplace_or_replace<Transform>(entity, seeded);
            world.emplace_or_replace<WorldTransform>(entity, matrixOf(seeded));
            world.remove<TransformDirty>(entity);
        }

        // Unlinking applies the link's own policy. A component already deleted -- which is how the
        // bridge's deletion scan unlinks -- has nothing left to destroy.
        void onUnlinked(World& world, Entity entity) {
            const VoxelComponent& link = world.get<VoxelComponent>(entity);
            if (link.onUnlink != OnUnlink::Destroy) return;
            Scene* scene = sceneOf(world);
            if (scene && utils::isComponentAlive(*scene, link.handle)) {
                utils::deleteComponent(*scene, link.handle);
            }
        }

        // ---- The PostUpdate system ----

        void sync(Application& app) {
            World& world = app.world;
            Scene* scene = sceneOf(world);
            BridgeState& state = world.ctx().get<BridgeState>();

            // 1. Links whose component was deleted under them. Only looked for when something was
            //    deleted since the last frame, which is what Scene::deletions is for.
            if (scene && scene->deletions != state.seenDeletions) {
                state.seenDeletions = scene->deletions;
                std::vector<std::pair<Entity, ComponentHandle>> dead;
                for (auto [entity, link] : world.view<VoxelComponent>().each()) {
                    if (!utils::isComponentAlive(*scene, link.handle)) dead.push_back({entity, link.handle});
                }
                for (auto [entity, handle] : dead) {
                    world.remove<VoxelComponent>(entity);
                    send(world, ComponentDestroyed{entity, handle});
                }
            }

            // 2. Changed transforms into the Scene. Entity parenting does not exist yet, so a
            //    Transform is its own world transform (Root) or Scene-parent-local (Part), and both
            //    are written the same way: as the component's local transform.
            auto dirty = world.view<TransformDirty, Transform>();
            for (auto [entity, transform] : dirty.each()) {
                world.emplace_or_replace<WorldTransform>(entity, matrixOf(transform));
                const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
                if (!link || !scene || !utils::isComponentAlive(*scene, link->handle)) continue;
                utils::setComponentTransform(*scene, link->handle, transform.position,
                                             transform.rotation, transform.scale);
            }
            world.clear<TransformDirty>();
        }
    }

    void installSceneBridge(Application& app) {
        World& world = app.world;
        if (world.ctx().contains<BridgeState>()) return;
        world.ctx().emplace<BridgeState>();
        if (Scene* scene = sceneOf(world)) world.ctx().get<BridgeState>().seenDeletions = scene->deletions;

        world.on_construct<VoxelComponent>().connect<&onLinked>();
        world.on_destroy<VoxelComponent>().connect<&onUnlinked>();
        world.on_construct<Transform>().connect<&markDirty>();
        world.on_update<Transform>().connect<&markDirty>();

        app.addSystem(Stage::PostUpdate, "scene bridge", sync);
    }

    Entity entityFor(const World& world, ComponentHandle component) {
        for (auto [entity, link] : world.view<VoxelComponent>().each()) {
            if (link.handle == component) return entity;
        }
        return NullEntity;
    }

    bool linkComponent(World& world, Entity entity, ComponentHandle component, LinkMode mode,
                       OnUnlink onUnlink) {
        Scene* scene = sceneOf(world);
        if (!scene) {
            core::error("linkComponent: there is no projv::Scene in world.ctx()");
            return false;
        }
        if (!world.ctx().contains<BridgeState>()) {
            core::error("linkComponent: the scene bridge is not installed (runtime::installSceneBridge)");
            return false;
        }
        if (!utils::isComponentAlive(*scene, component)) {
            core::error("linkComponent: component {} is not alive", component);
            return false;
        }
        if (mode == LinkMode::Root && scene->components[component].parent != INVALID_COMPONENT_HANDLE) {
            core::error("linkComponent: '{}' is inside an asset; a Root link needs a root component - "
                        "link it as a Part", scene->components[component].name);
            return false;
        }
        Entity existing = entityFor(world, component);
        if (existing != NullEntity && existing != entity) {
            core::error("linkComponent: '{}' is already linked to entity {}", scene->components[component].name,
                        static_cast<uint32_t>(existing));
            return false;
        }
        if (world.all_of<VoxelComponent>(entity)) world.remove<VoxelComponent>(entity);
        world.emplace<VoxelComponent>(entity, VoxelComponent{component, mode, onUnlink});
        send(world, EntityLinked{entity, component});
        return true;
    }

    void unlinkComponent(World& world, Entity entity) {
        world.remove<VoxelComponent>(entity);
    }

    void reseedTransforms(World& world) {
        Scene* scene = sceneOf(world);
        if (!scene) return;
        std::vector<std::pair<Entity, Transform>> changed;
        for (auto [entity, link] : world.view<VoxelComponent>().each()) {
            if (!utils::isComponentAlive(*scene, link.handle)) continue;
            Transform fromScene = transformOf(scene->components[link.handle]);
            const Transform* current = world.try_get<Transform>(entity);
            bool same = current && current->position == fromScene.position &&
                        current->rotation == fromScene.rotation && current->scale == fromScene.scale;
            if (!same) changed.push_back({entity, fromScene});
        }
        for (auto [entity, transform] : changed) {
            world.emplace_or_replace<Transform>(entity, transform);
            world.emplace_or_replace<WorldTransform>(entity, matrixOf(transform));
            // Read from the Scene, so not written back to it.
            world.remove<TransformDirty>(entity);
            send(world, ComponentTransformChanged{entity, world.get<VoxelComponent>(entity).handle});
        }
    }

    namespace detail {
        void addSpawnHandler(World& world, AttachmentScope scope, RawSpawnHandler handler) {
            BridgeState* state = world.ctx().find<BridgeState>();
            if (!state) {
                core::error("registerSpawnHandler: the scene bridge is not installed");
                return;
            }
            (scope == AttachmentScope::Document ? state->documentHandlers : state->componentHandlers)
                .push_back(std::move(handler));
        }
    }

    namespace {
        void runHandlers(World& world, const std::vector<detail::RawSpawnHandler>& handlers, Entity entity,
                         const Scene& scene, ComponentHandle handle) {
            // A copy: a handler may register another.
            std::vector<detail::RawSpawnHandler> snapshot = handlers;
            for (const detail::RawSpawnHandler& handler : snapshot) handler(world, entity, scene, handle);
        }
    }

    Entity spawnComponent(World& world, ComponentHandle component, LinkMode mode, OnUnlink onUnlink) {
        Entity entity = world.create();
        if (!linkComponent(world, entity, component, mode, onUnlink)) {
            world.destroy(entity);
            return NullEntity;
        }
        Scene& scene = *sceneOf(world);
        BridgeState& state = world.ctx().get<BridgeState>();
        runHandlers(world, state.componentHandlers, entity, scene, component);
        // An Asset stands for a folder, and the folder's own block is attached to it.
        if (scene.components[component].kind == ComponentKind::Asset) {
            runHandlers(world, state.documentHandlers, entity, scene, component);
        }
        return entity;
    }

    SpawnedDocument spawnFromCompose(World& world) {
        SpawnedDocument spawned;
        Scene* scene = sceneOf(world);
        if (!scene || !world.ctx().contains<BridgeState>()) {
            core::error("spawnFromCompose: needs a projv::Scene in world.ctx() and the scene bridge installed");
            return spawned;
        }

        spawned.document = world.create();
        world.emplace<SceneDocument>(spawned.document);
        runHandlers(world, world.ctx().get<BridgeState>().documentHandlers, spawned.document, *scene,
                    INVALID_COMPONENT_HANDLE);

        // Handles first: spawning can add components (a handler may), and those are not roots of
        // the document being spawned.
        std::vector<ComponentHandle> roots;
        for (ComponentHandle h = 0; h < scene->components.size(); h++) {
            if (scene->components[h].parent == INVALID_COMPONENT_HANDLE && utils::isComponentAlive(*scene, h)) {
                roots.push_back(h);
            }
        }
        for (ComponentHandle h : roots) {
            Entity entity = spawnComponent(world, h, LinkMode::Root);
            if (entity != NullEntity) spawned.roots.push_back(entity);
        }
        return spawned;
    }
}
