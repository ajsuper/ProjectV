// The tools (docs/plans/island-demo.md, D6, I1): the pickaxe, a big strike where you aim, its shards thrown
// out of the face; the chisel, which knocks out exactly the voxel you aim at as a one-voxel piece of
// rubble. Both are the engine's fracture: the pickaxe by energy (so it bites deeper in soft ground than in
// stone), the chisel by a reach smaller than a voxel (so the struck voxel alone breaks, to a shard of one).
// What a break leaves standing on nothing falls by itself (the island's StaticCollider splits): cut a
// trunk and the crown comes down.
//
// The plants (D12) are a component with no collider, so a break does not touch them: over every voxel of
// the island a break takes away, the plants standing on it (and the vines hanging under it) go too.
#ifndef ISLAND_DEMO_TOOLS_H
#define ISLAND_DEMO_TOOLS_H

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "core/application.h"
#include "core/log.h"
#include "graphics/input.h"
#include "runtime/physics.h"
#include "utils/editing.h"
#include "utils/picking.h"

#include "island.h"

namespace tools {
    using projv::Entity;
    using projv::World;
    using projv::core::ivec3;
    using projv::core::vec3;
    namespace runtime = projv::runtime;

    enum class Tool : int { Pickaxe, Chisel, Count };
    inline const char* toolName(Tool t) { return t == Tool::Pickaxe ? "Pickaxe" : "Chisel"; }

    constexpr float REACH = 6.0f;              // how far you can reach, m
    constexpr float PICKAXE_ENERGY = 35000.0f; // J: a 0.75 m bite in dirt, 0.62 m in wood, 0.52 m in stone
    constexpr float PICKAXE_PUSH = 2.5f;       // m/s out of the face
    constexpr float PICKAXE_SHARD = 0.25f;     // m: the size it breaks to
    constexpr float PICKAXE_EVERY = 0.4f;      // s between strikes, held
    constexpr float CHISEL_EVERY = 0.15f;

    struct Tools {
        Tool   selected = Tool::Pickaxe;
        float  cooldown = 0.0f;
        Entity island = projv::NullEntity, walker = projv::NullEntity;
        projv::ComponentHandle plants = projv::INVALID_COMPONENT_HANDLE;
        std::vector<ivec3> removed;   // island voxels a break took as pieces, whose plants are still to clear
        std::vector<std::pair<vec3, float>> struck;   // where strikes broke (as cheap debris, which lists no voxels): look there
        // Counts, for the self-test and the HUD.
        int strikes = 0, breaks = 0, shards = 0, fallen = 0, plantsCleared = 0;
        bool aiming = false;          // the crosshair is on something within reach
    };

    struct Aim {
        Entity entity = projv::NullEntity;
        vec3   point{0.0f}, normal{0.0f};
        float  distance = 0.0f;
    };

    inline std::optional<Aim> aim(const World& world, vec3 eye, vec3 forward, Entity walker) {
        const auto hit = runtime::castRay(world, eye, forward, REACH, 0xffffffffu, walker);
        if (!hit || hit->entity == projv::NullEntity) return std::nullopt;
        return Aim{hit->entity, hit->position, hit->normal, hit->distance};
    }

    // The centre of the voxel just inside the surface at a hit, on the island's lattice.
    inline vec3 voxelInside(const Aim& a) {
        const vec3 p = a.point - a.normal * (island::VOXEL * 0.5f);
        return (glm::floor(p / island::VOXEL) + vec3(0.5f)) * island::VOXEL;
    }

    inline void strike(World& world, Tools& t, Tool tool, const Aim& a, vec3 forward) {
        runtime::Strike s;
        if (tool == Tool::Pickaxe) {
            s.at = a.point + forward * 0.1f;   // just inside what was hit
            s.energy = PICKAXE_ENERGY;
            s.push = a.normal * PICKAXE_PUSH;
            s.shardSize = PICKAXE_SHARD;
        } else {
            s.at = voxelInside(a);
            s.reach = island::VOXEL * 0.55f;   // over half a voxel (less breaks nothing), under one: it, and only it
            s.shardSize = island::VOXEL;
            s.push = a.normal * 0.6f;          // a nudge out, so it drops at your feet rather than in the hole
        }
        s.striker = t.walker;
        runtime::fracture(world, a.entity, s);
        t.strikes++;
    }

    // ---- The plants over a break ---------------------------------------------------------------------

    inline bool plantAt(const projv::Scene& scene, const projv::SceneGrid& grid, ivec3 v) {
        const ivec3 cell = ivec3(glm::floor(vec3(v) / float(island::RES))) - grid.originCellCoord;
        if (glm::any(glm::lessThan(cell, ivec3(0))) || glm::any(glm::greaterThanEqual(cell, grid.dims))) return false;
        const int32_t c = grid.cellToChunk[size_t(cell.x + grid.dims.x * (cell.y + grid.dims.y * cell.z))];
        if (c < 0) return false;
        const projv::Chunk& chunk = scene.chunks[size_t(c)];
        if (!chunk.alive || chunk.geometryPoolIndex < 0) return false;
        const ivec3 local = v - (cell + grid.originCellCoord) * island::RES;
        uint8_t slot = 0;
        return projv::utils::queryVoxelMaterial(scene.geometryPool[size_t(chunk.geometryPoolIndex)], uint32_t(island::RES), local, slot);
    }

    // Clears plants with nothing under them: over each removed island voxel, the plant column standing
    // on it and the one hanging under it; round each strike, every plant voxel whose column neither stands
    // on the island nor hangs from it.
    inline void clearPlants(World& world, Tools& t) {
        if ((t.removed.empty() && t.struck.empty()) || t.plants == projv::INVALID_COMPONENT_HANDLE) return;
        auto& scene = world.ctx().get<projv::Scene>();
        const int32_t g = scene.components[t.plants].gridIndex, gi = scene.components[world.get<projv::VoxelComponent>(t.island).handle].gridIndex;
        if (g < 0 || gi < 0) { t.removed.clear(); t.struck.clear(); return; }
        const projv::SceneGrid& grid = scene.grids[size_t(g)];
        const projv::SceneGrid& ground = scene.grids[size_t(gi)];
        std::vector<projv::PendingVoxelOp> ops;
        for (const auto& [at, radius] : t.struck) {
            const ivec3 lo = ivec3(glm::floor((at - vec3(radius)) / island::VOXEL)), hi = ivec3(glm::floor((at + vec3(radius)) / island::VOXEL));
            for (int z = lo.z; z <= hi.z; z++)
                for (int x = lo.x; x <= hi.x; x++)
                    for (int y = lo.y; y <= hi.y; y++) {
                        const ivec3 p(x, y, z);
                        if (!plantAt(scene, grid, p)) continue;
                        ivec3 down = p, up = p;
                        while (plantAt(scene, grid, down - ivec3(0, 1, 0))) down.y--;
                        while (plantAt(scene, grid, up + ivec3(0, 1, 0))) up.y++;
                        const bool standing = plantAt(scene, ground, down - ivec3(0, 1, 0)), hanging = plantAt(scene, ground, up + ivec3(0, 1, 0));
                        if (!standing && !hanging) ops.push_back({false, p, 0});
                    }
        }
        t.struck.clear();
        std::sort(t.removed.begin(), t.removed.end(), [](ivec3 a, ivec3 b) { return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z; });
        t.removed.erase(std::unique(t.removed.begin(), t.removed.end()), t.removed.end());
        for (const ivec3 v : t.removed)
            for (int dir : {1, -1})
                for (ivec3 p = v + ivec3(0, dir, 0); plantAt(scene, grid, p); p.y += dir) ops.push_back({false, p, 0});
        t.removed.clear();
        if (ops.empty()) return;
        t.plantsCleared += int(ops.size());
        projv::utils::queueVoxelRemove(scene, t.plants, ops);
        projv::utils::updateScene(scene);
    }

    // ---- Install and each frame ----------------------------------------------------------------------

    inline void install(projv::Application& app, Entity islandEntity, projv::ComponentHandle plants, Entity walker) {
        World& world = app.world;
        Tools& t = world.ctx().emplace<Tools>();
        t.island = islandEntity;
        t.plants = plants;
        t.walker = walker;
        // Inside the physics step: only note what went (the scene edit waits for the frame).
        app.events().on<projv::BodyBroken>([&world](const projv::BodyBroken& e) {
            Tools& t = world.ctx().get<Tools>();
            if (std::getenv("ISLAND_DEBUG_BREAKS"))
                projv::core::info("island: break of {} (island {}): cause {}, {} pieces, {} shards, {} debris, {} dust, at ({:.2f},{:.2f},{:.2f})",
                                  uint32_t(e.entity), uint32_t(t.island), int(e.cause), e.pieces.size(), e.shards, e.debris.size(), e.dust.size(),
                                  e.at.x, e.at.y, e.at.z);
            if (e.entity != t.island) return;
            t.breaks++;
            t.shards += int(e.shards + e.debris.size());
            if (e.cause == projv::BreakCause::Strike || e.cause == projv::BreakCause::Impact) t.struck.push_back({e.at, 1.1f});
            t.fallen += int(e.pieces.size() - e.shards);   // what came away whole: a felled crown, a cut-off ledge
            for (const auto& piece : e.pieceVoxels) t.removed.insert(t.removed.end(), piece.begin(), piece.end());
            t.removed.insert(t.removed.end(), e.dust.begin(), e.dust.end());
        });
    }

    // `use`: the left button is held and the cursor is the game's.
    inline void update(World& world, const projv::Input& input, float dt, vec3 eye, vec3 forward, bool use) {
        Tools& t = world.ctx().get<Tools>();
        if (!input.wantCaptureKeyboard) {
            if (input.pressed(projv::Key::Num1)) t.selected = Tool::Pickaxe;
            if (input.pressed(projv::Key::Num2)) t.selected = Tool::Chisel;
        }
        if (input.scroll.y != 0.0f) t.selected = Tool((int(t.selected) + (input.scroll.y < 0.0f ? 1 : int(Tool::Count) - 1)) % int(Tool::Count));
        t.cooldown = std::max(0.0f, t.cooldown - dt);
        const std::optional<Aim> a = aim(world, eye, forward, t.walker);
        t.aiming = a.has_value();
        if (use && a && t.cooldown <= 0.0f && runtime::canBreak(world, a->entity)) {
            strike(world, t, t.selected, *a, forward);
            t.cooldown = t.selected == Tool::Pickaxe ? PICKAXE_EVERY : CHISEL_EVERY;
        }
        clearPlants(world, t);
    }
}

#endif
