// Building (docs/plans/island-demo.md, D8, I3): with a material in hand (hotbar slots 3-9), the left button
// places a 0.5 m block of it -- 4 x 4 x 4 voxels at 0.125 m -- against the face you aim at, on a 0.5 m grid
// so blocks line up and stack. It goes into the island itself (runtime::editVoxels, the material's own
// palette slots), so a placed block is ground like any other: walked on, broken, gathered again. It costs
// 64 voxels of the material; it fills only the empty voxels of its cell (no ground is replaced), never one
// inside you, and clears the plants where it lands.
#ifndef ISLAND_DEMO_BUILD_H
#define ISLAND_DEMO_BUILD_H

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "core/application.h"
#include "graphics/input.h"
#include "runtime/physics.h"
#include "utils/editing.h"

#include "gather.h"
#include "island.h"
#include "tools.h"

namespace build {
    using projv::Entity;
    using projv::World;
    using projv::core::ivec3;
    using projv::core::vec3;
    namespace runtime = projv::runtime;

    constexpr int   BLOCK = 4;                       // voxels on a side
    constexpr float BLOCK_SIZE = BLOCK * island::VOXEL;
    constexpr float PLACE_EVERY = 0.25f;             // s between blocks, held
    constexpr float REACH = 6.0f;

    // The material in a hotbar slot (2..8), or none.
    inline const char* materialOf(int slot) {
        const int i = slot - int(tools::Tool::Count);
        return i >= 0 && i < int(std::size(gather::MATERIALS)) ? gather::MATERIALS[i] : nullptr;
    }

    // The island's palette slots a material's block is made of, as the generator shades them.
    inline std::vector<uint8_t> shadesOf(const std::string& material) {
        using namespace island;
        if (material == "grass") return {Grass0, Grass1, Grass2};
        if (material == "dirt") return {Dirt0, Dirt1};
        if (material == "sand") return {Sand0, Sand1};
        if (material == "stone") return {Stone0, Stone1, Stone2, Stone3, Stone4};
        if (material == "wood") return {Wood, Wood, Wood, Bark};
        if (material == "leaves") return {Leaves0, Leaves1, Leaves2};
        if (material == "mushroom") return {CapRed, CapRed, CapSpot};
        return {Dirt0};
    }
    // The slot for voxel `v` of a block of `material`: a scatter of its shades, as ground has.
    inline uint8_t shadeAt(const std::string& material, ivec3 v) {
        const std::vector<uint8_t> shades = shadesOf(material);
        return shades[island::hash(v.x, v.y, v.z, 7777u) % shades.size()];
    }

    struct Build {
        float cooldown = 0.0f;
        int   placed = 0;            // blocks, this session
        std::string refused;         // why the last try placed nothing (the HUD may say)
    };

    // Whether the island (or the plants: `grid`) has a voxel at v. tools::plantAt reads any grid.
    inline const projv::SceneGrid* gridOf(World& world, projv::ComponentHandle h) {
        auto& scene = world.ctx().get<projv::Scene>();
        const int32_t g = h < scene.components.size() ? scene.components[h].gridIndex : -1;
        return g < 0 ? nullptr : &scene.grids[size_t(g)];
    }

    // Where a block would go for a hit: the 0.5 m cell just outside the face, as island voxel coordinates
    // of its lowest corner.
    inline ivec3 cellFor(vec3 point, vec3 normal) {
        const vec3 outside = point + normal * (island::VOXEL * 0.5f);
        return ivec3(glm::floor(outside / BLOCK_SIZE)) * BLOCK;
    }

    // Tries to place a block of `material` in the cell at `corner`. True if it did (the edit is issued; it
    // lands at the next physics step).
    inline bool place(World& world, Build& b, const std::string& material, ivec3 corner) {
        tools::Tools& t = world.ctx().get<tools::Tools>();
        gather::Inventory& inventory = world.ctx().get<gather::Inventory>();
        if (inventory.voxels[material] < gather::VOXELS_PER_BLOCK - 1e-6) { b.refused = "not enough " + material; return false; }
        // Not inside you: the walker's capsule, as a box.
        const vec3 lo = vec3(corner) * island::VOXEL, hi = lo + vec3(BLOCK_SIZE);
        if (const projv::Transform* w = world.try_get<projv::Transform>(t.walker)) {
            const vec3 feet = w->position;
            const vec3 plo = feet - vec3(0.32f, 0.0f, 0.32f), phi = feet + vec3(0.32f, 1.85f, 0.32f);
            if (glm::all(glm::lessThan(lo, phi)) && glm::all(glm::lessThan(plo, hi))) { b.refused = "you are in the way"; return false; }
        }
        auto& scene = world.ctx().get<projv::Scene>();
        const projv::SceneGrid* ground = gridOf(world, world.get<projv::VoxelComponent>(t.island).handle);
        const projv::SceneGrid* plants = t.plants != projv::INVALID_COMPONENT_HANDLE ? gridOf(world, t.plants) : nullptr;
        if (!ground) return false;
        std::vector<projv::PendingVoxelOp> ops, clear;
        for (int z = 0; z < BLOCK; z++)
            for (int y = 0; y < BLOCK; y++)
                for (int x = 0; x < BLOCK; x++) {
                    const ivec3 v = corner + ivec3(x, y, z);
                    if (v.y < 0 || v.y >= island::COLUMN) continue;
                    if (plants && tools::plantAt(scene, *plants, v)) clear.push_back({false, v, 0});
                    if (tools::plantAt(scene, *ground, v)) continue;   // ground already: left as it is
                    projv::PendingVoxelOp op{true, v, 0};
                    op.slot = shadeAt(material, v);
                    ops.push_back(op);
                }
        if (ops.empty()) { b.refused = "something is already there"; return false; }
        runtime::editVoxels(world, t.island, ops);
        if (!clear.empty()) {
            projv::utils::queueVoxelRemove(scene, t.plants, clear);
            projv::utils::updateScene(scene);
        }
        inventory.voxels[material] -= gather::VOXELS_PER_BLOCK;   // a block's price, however much of its cell was empty
        b.placed++;
        b.refused.clear();
        t.bursts.push_back({vec3(lo + hi) * 0.5f, vec3(0, 1, 0), shadeAt(material, corner), 10, 1.2f});   // a puff as it lands
        return true;
    }

    inline void install(World& world) { world.ctx().emplace<Build>(); }

    // Each frame: with a material in hand and the button held, a block where you aim.
    inline void update(World& world, float dt, vec3 eye, vec3 forward, bool use) {
        Build& b = world.ctx().get<Build>();
        tools::Tools& t = world.ctx().get<tools::Tools>();
        b.cooldown = std::max(0.0f, b.cooldown - dt);
        const char* material = materialOf(t.slot);
        if (!material || !use || b.cooldown > 0.0f) return;
        b.cooldown = PLACE_EVERY;
        const auto hit = runtime::castRay(world, eye, forward, REACH, 0xffffffffu, t.walker);
        // Only against the island: a block must touch the ground it joins (never on a falling piece).
        if (!hit || hit->entity != t.island) { b.refused = "nothing to build on"; return; }
        if (place(world, b, material, cellFor(hit->position, hit->normal))) {
            t.swing = 0.0f;   // the hand's jab (fx.h)
            t.swingTool = tools::Tool::Chisel;
            t.landed = true;
        }
    }
}

#endif
