#ifndef PROJECTV_WORLD_H
#define PROJECTV_WORLD_H

#include <entt/entity/registry.hpp>

// The names engine code uses for EnTT's types. Not a wrapper: World *is* an entt::registry, and
// views, signals, ctx() and snapshots are used directly, as EnTT names and documents them. What this
// buys is one place to adjust if EnTT moves a type between versions, so an upgrade touches this
// header rather than every signature that mentions a registry.
//
// EnTT is pinned (external/entt, v4.0.0) and upgraded deliberately, in a commit of its own with the
// unit tests run -- the same way bgfx is pinned.
//
// Global resources live in the registry's context:
//
//     auto& scene = world.ctx().emplace<projv::Scene>();
//     auto& scene = world.ctx().get<projv::Scene>();
//     auto* scene = world.ctx().find<projv::Scene>();     // nullptr when absent
namespace projv {
    using World  = entt::registry;
    using Entity = entt::entity;
    inline constexpr entt::null_t NullEntity{};
}

#endif
