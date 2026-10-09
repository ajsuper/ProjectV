#ifndef PROJECTV_RUNTIME_NET_H
#define PROJECTV_RUNTIME_NET_H

#include <cstdint>
#include <optional>

#include "nlohmann/json.hpp"
#include "core/world.h"
#include "runtime/entities.h"

// Network identity: the one name for an entity that means the same thing on every machine.
//
// Entity, ComponentHandle, ComponentRef and physics body ids are all local -- reused, reordered,
// different between two runs that did the same thing in a different order -- and never leave the
// process. A NetId is assigned once, by whoever is the authority (the server, or a single-player
// game), and is what anything sent over a network, written to a replay, or compared between peers
// names an entity by. Authored entities carry theirs in entities.json; spawned ones are given one.
//
// Physics orders by it: bodies made in the same tick are made in NetId order, so two peers making
// the same bodies get the same simulation whatever order their registries happened to fill in.
namespace projv {
    struct NetId {
        uint32_t value = 0;   // 0 is no id
        bool operator==(const NetId&) const = default;
    };
}

namespace projv::runtime {
    // Registers NetId with entities.json and starts the allocator. Idempotent. installPhysics calls it.
    void installNetIds(World& world);

    // Gives `entity` the next unused NetId (keeping one it already has) and returns it. Authority
    // only: a client is told its ids, it does not make them.
    NetId assignNetId(World& world, Entity entity);

    // The entity with that id, or NullEntity.
    Entity entityForNetId(const World& world, NetId id);
}

template<> struct projv::runtime::ComponentTraits<projv::NetId> {
    static constexpr const char* key = "projv.netid";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const projv::NetId& id) { return nlohmann::json{{"id", id.value}}; }
    static std::optional<projv::NetId> load(const nlohmann::json& json, uint32_t) {
        uint32_t value = json.at("id").get<uint32_t>();
        if (value == 0) return std::nullopt;
        return projv::NetId{value};
    }
};

#endif
