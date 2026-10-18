#ifndef PROJECTV_RUNTIME_LOADING_H
#define PROJECTV_RUNTIME_LOADING_H

#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "core/application.h"
#include "core/world.h"
#include "runtime/entities.h"
#include "runtime/net.h"
#include "utils/scene_loading.h"

// World loading for a running program: utils::SceneLoader's blocks, plus the entities that go with
// them. The plan: docs/plans/world-loading.md.
//
//     runtime::openWorld(app, "scenes/Island", {.policy = utils::loadNear(64, 192)});
//     runtime::spawnEntities(app.world);                       // as with any scene
//     app.world.emplace<LoadViewer>(player);                   // or runtime::setViewers each frame
//
// Each frame, in PreUpdate, "world loading" asks the rule, loads and unloads blocks, then brings
// entities in and out with them:
//   - an entity linked to a component is present while any block of that component's subtree is
//     loaded (a component with no blocks at all: always present);
//   - an unlinked entity with LoadBounds is judged by the rule, as a box around its position;
//   - only entities from a file (Authored) or with LoadBounds take part: one made at runtime with
//     neither, and every LoadViewer, stays whatever the rule says.
// An absent entity is not in the registry: its components are saved (entityComponents) and it is
// destroyed -- its link let go without deleting the component -- and made again from them when it
// comes back, with its NetId, as a new Entity. EntityUnloading is triggered before it goes;
// EntityReloaded is sent when it is back. Present entities carry LoadState, the rule's answer.
//
// Main thread only. Flush the Scene to the GPU as usual (graphics::flushSceneUpdates): loaded and
// freed blocks are chunks made and freed.
namespace projv {
    // An entity the world is seen from: blocks and entities load around it (a player, a camera rig).
    // Never unloaded itself.
    struct LoadViewer {
        uint32_t id = 0;   // passed to the rule as Viewer::id
    };

    // How big an unlinked entity is, for the rule: a box this far each way from its position.
    struct LoadBounds {
        core::vec3 halfExtents{0.5f};
    };

    // What the rule says for a present entity that takes part. Kept and Active both mean present;
    // physics gives them their meaning (world-loading.md, L3).
    struct LoadState {
        utils::Presence presence = utils::Presence::Active;
    };

    // Triggered (delivered at once) just before an entity is unloaded: read it now.
    struct EntityUnloading { Entity entity; NetId netId; };
    // Sent when an unloaded entity has been made again. `entity` is new.
    struct EntityReloaded { Entity entity; NetId netId; };
}

namespace projv::runtime {
    struct WorldLoadingConfig {
        utils::LoaderConfig loader;
        utils::LoadPolicy   policy = utils::loadEverything();
    };

    // Opens the compose folder lazily into the Scene in world.ctx() (made, or replaced), installs the
    // Scene bridge, registers LoadViewer and LoadBounds, and adds "world loading" to PreUpdate.
    // False if the folder did not open. Spawn the folder's entities afterwards, as usual.
    bool openWorld(Application& app, const std::string& folder, const WorldLoadingConfig& config = {});

    utils::SceneLoader& worldLoader(World& world);
    bool hasWorldLoading(const World& world);
    void setLoadRule(World& world, utils::LoadPolicy policy);
    // The program's own viewers (a free camera), besides every LoadViewer entity. Kept until replaced.
    void setViewers(World& world, std::vector<utils::Viewer> viewers);

    // One frame of it: what the "world loading" system runs.
    void updateWorldLoading(World& world);
    // Until every wanted block is loaded and every entity is where the rule puts it: a loading
    // screen, a test.
    void settleWorld(World& world);

    // How many entities are unloaded, waiting to come back.
    size_t unloadedEntityCount(const World& world);
}

template<> struct projv::runtime::ComponentTraits<projv::LoadViewer> {
    static constexpr const char* key = "projv.loadviewer";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::LoadViewer& v) { return nlohmann::json{{"id", v.id}}; }
    static std::optional<projv::LoadViewer> load(const nlohmann::json& json, uint32_t) {
        return projv::LoadViewer{json.value("id", 0u)};
    }
};

template<> struct projv::runtime::ComponentTraits<projv::LoadBounds> {
    static constexpr const char* key = "projv.loadbounds";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::LoadBounds& b) {
        return nlohmann::json{{"halfExtents", {b.halfExtents.x, b.halfExtents.y, b.halfExtents.z}}};
    }
    static std::optional<projv::LoadBounds> load(const nlohmann::json& json, uint32_t) {
        projv::LoadBounds b;
        if (json.contains("halfExtents")) {
            const auto& h = json.at("halfExtents");
            b.halfExtents = projv::core::vec3(h.at(0).get<float>(), h.at(1).get<float>(), h.at(2).get<float>());
        }
        return b;
    }
};

#endif
