#include "runtime/net.h"

#include <algorithm>
#include <map>

#include "core/log.h"

namespace projv::runtime {
    namespace {
        struct NetIds {
            uint32_t next = 1;
            std::map<uint32_t, Entity> byId;
        };

        // An id arriving on an entity -- assigned, loaded from a file, or set by hand -- is indexed,
        // and the allocator moves past it so it is never handed out again.
        void added(World& world, Entity entity) {
            NetIds& ids = world.ctx().get<NetIds>();
            uint32_t value = world.get<NetId>(entity).value;
            if (value == 0) return;
            ids.next = std::max(ids.next, value + 1);
            auto [it, inserted] = ids.byId.emplace(value, entity);
            if (!inserted && it->second != entity) {
                core::error("net: NetId {} is on entity {} and entity {}; ids must be unique, and the second "
                            "will not be found by it", value, static_cast<uint32_t>(it->second),
                            static_cast<uint32_t>(entity));
            }
        }

        void removed(World& world, Entity entity) {
            NetIds& ids = world.ctx().get<NetIds>();
            auto it = ids.byId.find(world.get<NetId>(entity).value);
            if (it != ids.byId.end() && it->second == entity) ids.byId.erase(it);
        }

        void changed(World& world, Entity entity) {
            // The old value is gone by now; rebuild this entity's entry from scratch.
            NetIds& ids = world.ctx().get<NetIds>();
            for (auto it = ids.byId.begin(); it != ids.byId.end();) it = it->second == entity ? ids.byId.erase(it) : std::next(it);
            added(world, entity);
        }
    }

    void installNetIds(World& world) {
        if (world.ctx().contains<NetIds>()) return;
        world.ctx().emplace<NetIds>();
        registerComponent<NetId>(world);
        world.on_construct<NetId>().connect<&added>();
        world.on_update<NetId>().connect<&changed>();
        world.on_destroy<NetId>().connect<&removed>();
    }

    NetId assignNetId(World& world, Entity entity) {
        installNetIds(world);
        if (const NetId* existing = world.try_get<NetId>(entity); existing && existing->value != 0) return *existing;
        NetIds& ids = world.ctx().get<NetIds>();
        NetId id{ids.next};
        world.emplace_or_replace<NetId>(entity, id);
        return id;
    }

    Entity entityForNetId(const World& world, NetId id) {
        const NetIds* ids = world.ctx().find<NetIds>();
        if (!ids || id.value == 0) return NullEntity;
        auto it = ids->byId.find(id.value);
        return it == ids->byId.end() ? Entity(NullEntity) : it->second;
    }
}
