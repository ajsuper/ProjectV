#ifndef PROJECTV_SCENE_BRIDGE_H
#define PROJECTV_SCENE_BRIDGE_H

#include <functional>
#include <vector>

#include "core/application.h"
#include "core/math.h"
#include "core/world.h"
#include "data_structures/scene.h"
#include "utils/attachments.h"

// The Scene bridge: how entities (the registry -- what the running game is doing) relate to voxel
// components (the Scene -- what the assets are made of).
//
// **Entities link to components; they never absorb them.** The Scene keeps its own hierarchy --
// parts, nested assets, placements inside an asset -- exactly as compose.json describes it, and an
// entity points at one component in it. Moving the entity moves the component and everything under
// it; nothing about the asset's structure is copied into the registry. (Why not one tree: see
// Appendix A of the runtime plan.)
//
//     app.world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(folder));
//     projv::runtime::installSceneBridge(app);
//     auto spawned = projv::runtime::spawnFromCompose(app.world);      // one entity per root
//     app.world.patch<projv::Transform>(spawned.roots[0], [](auto& t) { t.position.y += 1.0f; });
//
// Each frame, in PostUpdate, the bridge writes every *changed* Transform into its component
// (utils::setComponentTransform, which rebakes the subtree's chunk headers), notices components
// deleted out from under their links, and then the event pump delivers what it sent.
//
// **Change Transform through the registry** -- patch, replace or emplace_or_replace -- so the change
// is seen. Assigning through a reference from get<Transform> is invisible to the bridge, by EnTT's
// design: signals are what make "only the changed ones" cheap.
//
// Requires a projv::Scene in world.ctx() whenever links are made or the bridge runs. Main thread
// only: reading attachments may decode them in place.
namespace projv {
    // Local transform. For a Root link it is the root component's transform, which -- the root having
    // no Scene parent -- is its world transform. For a Part link it is measured in the linked
    // component's Scene-parent space.
    struct Transform {
        core::vec3 position{0.0f};
        core::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        float      scale = 1.0f;     // uniform, like the components it drives
    };

    // Derived from Transform by the bridge, for anything that wants a matrix.
    struct WorldTransform {
        core::mat4 matrix{1.0f};
    };

    enum class LinkMode {
        Root,   // a component with no Scene parent; the entity's transform is its world transform
        Part    // a component inside an asset (a door in a house); transform in Scene-parent space
    };

    enum class OnUnlink {
        Keep,     // the component stays in the Scene when the link goes (the default)
        Destroy   // the component is deleted with the link -- and so when its entity is destroyed
    };

    // The link. Made through runtime::linkComponent, which validates it; emplacing it directly skips
    // the validation.
    struct VoxelComponent {
        ComponentHandle handle = INVALID_COMPONENT_HANDLE;
        LinkMode        mode = LinkMode::Root;
        OnUnlink        onUnlink = OnUnlink::Keep;
    };

    // A Transform changed since the last sync. Set by the bridge's signals, cleared by its sync.
    struct TransformDirty {};

    // Tags the entity spawnFromCompose makes for the opened folder itself, which document-scope
    // spawn handlers act on.
    struct SceneDocument {};

    // ---- Events the bridge sends --------------------------------------------------------------

    struct EntityLinked              { Entity entity; ComponentHandle component; };
    // The linked component was deleted. The link is already gone when this arrives; the entity is
    // not. Whether it should go too is the game's decision.
    struct ComponentDestroyed        { Entity entity; ComponentHandle component; };
    // reseedTransforms read a linked component's transform back into its entity.
    struct ComponentTransformChanged { Entity entity; ComponentHandle component; };
}

namespace projv::runtime {
    // Connects the bridge's signals to the registry and adds its PostUpdate system. Call once,
    // before linking anything. Idempotent.
    void installSceneBridge(Application& app);

    // Links `entity` to `component` and seeds its Transform from the component. Returns false, and
    // logs why, if the component is not alive, a Root link names a component that has a Scene
    // parent, or another entity already links it (two writers of one transform would fight).
    bool linkComponent(World& world, Entity entity, ComponentHandle component,
                       LinkMode mode = LinkMode::Root, OnUnlink onUnlink = OnUnlink::Keep);

    // Removes the link, applying its OnUnlink. Destroying the entity does the same.
    void unlinkComponent(World& world, Entity entity);

    // The entity linked to `component`, or NullEntity.
    Entity entityFor(const World& world, ComponentHandle component);

    // Reads every linked component's transform back into its entity, and sends
    // ComponentTransformChanged for each that differed. For when the Scene was changed by something
    // other than an entity -- an editor tool, a load into the same Scene.
    void reseedTransforms(World& world);

    // ---- Spawning: attachments become ECS components --------------------------------------------
    //
    // An attachment is what an asset says (saved in compose.json, see utils/attachments.h); an ECS
    // component is what the running game is doing. A module registers, per attachment type, what
    // spawning a component that carries it means:
    //
    //     projv::runtime::registerSpawnHandler<Spawner>(world, [](projv::World& w, projv::Entity e,
    //                                                            const Spawner& s) {
    //         w.emplace<SpawnPoint>(e, s.kind);
    //     });
    //
    // Handlers read attachments and never write them. Keys nothing registered for are ignored here
    // and still saved with the Scene.

    template<typename T>
    void registerSpawnHandler(World& world, std::function<void(World&, Entity, const T&)> handler);

    // For folder-level (document-scope) attachments: a level's settings, a spawn table. Run for the
    // opened folder by spawnFromCompose, and for an Asset root's own folder when it is spawned.
    template<typename T>
    void registerDocumentSpawnHandler(World& world, std::function<void(World&, Entity, const T&)> handler);

    // Creates an entity, links it (see linkComponent) and runs the spawn handlers for the
    // component's attachments. NullEntity if the link is refused.
    Entity spawnComponent(World& world, ComponentHandle component, LinkMode mode = LinkMode::Root);

    struct SpawnedDocument {
        Entity              document = NullEntity;   // tagged SceneDocument; document handlers ran on it
        std::vector<Entity> roots;                   // one per live root component, Root-linked
    };
    // Spawns the whole Scene in world.ctx(): an entity for the opened folder, then one per root.
    SpawnedDocument spawnFromCompose(World& world);
}

// ---- Template definitions -------------------------------------------------------------------

namespace projv::runtime {
    namespace detail {
        using RawSpawnHandler = std::function<void(World&, Entity, const Scene&, ComponentHandle)>;
        void addSpawnHandler(World& world, AttachmentScope scope, RawSpawnHandler handler);
    }

    template<typename T>
    void registerSpawnHandler(World& world, std::function<void(World&, Entity, const T&)> handler) {
        detail::addSpawnHandler(world, AttachmentScope::Component,
            [handler = std::move(handler)](World& w, Entity e, const Scene& scene, ComponentHandle h) {
                if (const T* value = utils::getAttachment<T>(scene, h)) handler(w, e, *value);
            });
    }

    template<typename T>
    void registerDocumentSpawnHandler(World& world, std::function<void(World&, Entity, const T&)> handler) {
        detail::addSpawnHandler(world, AttachmentScope::Document,
            [handler = std::move(handler)](World& w, Entity e, const Scene& scene, ComponentHandle h) {
                if (const T* value = utils::getAttachment<T>(scene, h, AttachmentScope::Document)) {
                    handler(w, e, *value);
                }
            });
    }
}

#endif
