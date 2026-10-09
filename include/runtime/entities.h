#ifndef PROJECTV_ENTITIES_H
#define PROJECTV_ENTITIES_H

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "runtime/scene_bridge.h"

// Authored entities: gameplay data saved as the ECS components themselves.
//
// A folder holds its voxels in compose.json and its entities in entities.json beside it:
//
//     { "version": 1,
//       "entities": [
//         { "name": "Spawner", "link": 3, "components": { "sandbox.spawner": { "v": 1, "interval": 1.1 } } },
//         { "name": "Rules",               "components": { "sandbox.rules":   { "v": 1, "gravity": -28 } } },
//         { "link": "document",             "components": { "sandbox.body":    { "v": 1, "radius": 1.5 } } }
//       ] }
//
// - "link" names a component of *this folder* by its id (ComponentRecord::localId, the entry's "id"
//   in compose.json), or "document" for the node the folder becomes when it is loaded -- what a
//   prefab's own data links to. No link: the entity stands alone (a light, a spawn point, rules).
//   Links are local, so a folder loaded twice resolves each copy against its own node.
// - "components" are ECS components, written by their own ComponentTraits. What a file says is
//   what runs: there is no second, authored-only type and no translation step.
// - A key no program in this process registered is kept (UnknownComponents) and written back by
//   saveEntities, so a tool that does not know a game's components cannot erase them.
//
// Attachments (utils/attachments.h) remain for data *about the voxel structure*, written by tools
// -- the scene editor's boolean ops. Behaviour lives here.
//
// Linking needs the Scene bridge (runtime::installSceneBridge). Main thread only.
namespace projv::runtime {
    // Specialised by the program that owns T:
    //
    //     template<> struct projv::runtime::ComponentTraits<Spin> {
    //         static constexpr const char* key = "example.spin";
    //         static constexpr uint32_t version = 1;
    //         static nlohmann::json save(const Spin&);
    //         static std::optional<Spin> load(const nlohmann::json&, uint32_t fileVersion);
    //     };
    //
    // `save` returns an object; "v" is added on the way out. `load` sees the whole value, "v"
    // included; nullopt (or a nlohmann exception) refuses it, and the value is kept as unknown.
    // Setup a component needs from its entity -- a starting angle from the Transform, say -- belongs
    // in an on_construct signal, which runs after the link has seeded the Transform.
    template<typename T>
    struct ComponentTraits;

    // Makes T loadable from and savable to entities.json. Call before spawning.
    template<typename T>
    void registerComponent(World& world);

    // Where an entity came from: every entity spawned from an entities.json carries one, and
    // saveEntities writes a folder's entities from it.
    struct Authored {
        enum class Link { None, Document, Component };
        ComponentHandle document = INVALID_COMPONENT_HANDLE;   // the folder's node; INVALID = the top-level folder
        std::string name;
        Link link = Link::None;                                // as the file wrote it
        uint32_t linkId = 0;
        uint32_t order = 0;                                    // position in the file
    };

    // Components a file named that nothing registered, as JSON text by key.
    struct UnknownComponents {
        std::map<std::string, std::string> byKey;
    };

    struct SpawnedEntities {
        std::vector<Entity> entities;   // in spawn order
    };

    // Spawns every entities.json of the Scene in world.ctx(): the top-level folder's, and every
    // Asset node's that stands for a folder. Inner folders first, so an outer file that names the
    // same component adds to -- and overrides -- what the inner one said.
    SpawnedEntities spawnEntities(World& world);

    // The same, for `node` and the folders inside it: what a freshly grafted folder needs.
    SpawnedEntities spawnEntitiesUnder(World& world, ComponentHandle node);

    // Grafts a prefab folder into the Scene (utils::instantiateComposeInto) and spawns its
    // entities. Returns the entity linked to the new node -- the one its "document" entry made, or
    // a bare linked one if it has none. With OnUnlink::Destroy (the default), destroying that entity
    // deletes the prefab's voxels.
    Entity instantiatePrefab(World& world, const std::string& folder, core::vec3 position,
                             core::quat rotation = core::quat(1.0f, 0.0f, 0.0f, 0.0f), float scale = 1.0f,
                             OnUnlink onUnlink = OnUnlink::Destroy);

    // Writes `folder`/entities.json for the folder whose node is `document` (INVALID: the top-level
    // folder). Each entity belongs to exactly one file: one spawned from a file (it has an Authored)
    // belongs to that file; one made in code belongs to the file its linked component is an *entry*
    // of -- its parent's folder. Give an entity an Authored to place it elsewhere, such as a prefab's
    // own "document" entity.
    // Registered components are saved through their traits, unknown ones as they were read. A linked
    // entity's Transform is not written -- it lives in compose.json -- and an unlinked one's is.
    bool saveEntities(const World& world, ComponentHandle document, const std::string& folder);
}

// ---- Transform is registered by the engine itself --------------------------------------------

template<> struct projv::runtime::ComponentTraits<projv::Transform> {
    static constexpr const char* key = "projv.transform";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::Transform& t);
    static std::optional<projv::Transform> load(const nlohmann::json& json, uint32_t fileVersion);
};

// ---- Template definitions ---------------------------------------------------------------------

namespace projv::runtime {
    namespace detail {
        struct ComponentCodec {
            // Emplaces (or replaces) the component on the entity; false if the value was refused.
            std::function<bool(World&, Entity, const nlohmann::json&)> load;
            // The component as JSON, "v" included, or nullopt if the entity has none.
            std::function<std::optional<nlohmann::json>(const World&, Entity)> save;
        };
        void addCodec(World& world, const std::string& key, ComponentCodec codec);
    }

    template<typename T>
    void registerComponent(World& world) {
        using Traits = ComponentTraits<T>;
        detail::ComponentCodec codec;
        codec.load = [](World& w, Entity e, const nlohmann::json& json) {
            uint32_t version = 1;
            if (json.is_object() && json.contains("v") && json["v"].is_number_unsigned()) {
                version = json["v"].template get<uint32_t>();
            }
            std::optional<T> value;
            try { value = Traits::load(json, version); } catch (const nlohmann::json::exception&) { value.reset(); }
            if (!value) return false;
            w.emplace_or_replace<T>(e, std::move(*value));
            return true;
        };
        codec.save = [](const World& w, Entity e) -> std::optional<nlohmann::json> {
            const T* value = w.try_get<T>(e);
            if (!value) return std::nullopt;
            nlohmann::json json = Traits::save(*value);
            json["v"] = Traits::version;
            return json;
        };
        detail::addCodec(world, Traits::key, std::move(codec));
    }
}

#endif
