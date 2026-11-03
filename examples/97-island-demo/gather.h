// Gathering (docs/plans/island-demo.md, D7, D8, I2): rubble is whatever a break threw off -- cheap debris
// (projv::Debris, in no query, so found by a pass over them) and small pieces that are bodies (found with
// overlapSphere). E toggles collecting: while it is on, rubble that has landed near where you look, or
// near you, comes in by itself (while off, looking at rubble says what is there). Each piece taken has its
// voxels counted by their palette entry's physics material, times the piece's Rubble::weight (a pickaxe
// keeps only a share of its shards, tools.h), into the inventory, and is pulled to you before it goes.
#ifndef ISLAND_DEMO_GATHER_H
#define ISLAND_DEMO_GATHER_H

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/application.h"
#include "graphics/input.h"
#include "runtime/physics.h"
#include "runtime/scene_bridge.h"
#include "utils/scene_query.h"

#include "tools.h"

namespace gather {
    using projv::Entity;
    using projv::World;
    using projv::core::ivec3;
    using projv::core::vec3;
    namespace runtime = projv::runtime;

    constexpr float REACH = 5.0f;            // how far away rubble can be looked at, m
    constexpr float GATHER_RADIUS = 1.5f;    // what E takes, round where you look, m
    constexpr int   MAX_RUBBLE_VOXELS = 4096;   // a body piece bigger than this is not rubble (0.5 m^3 at 0.125 m)
    constexpr float VOXELS_PER_BLOCK = 64.0f;   // a placed block is 0.5 m: 4 x 4 x 4 voxels (D8)
    constexpr float PULL_SECONDS = 0.3f;
    constexpr float MAGNET = 2.5f;           // with collecting on: rubble this near you comes too, m
    constexpr float SETTLE = 0.35f;          // with collecting on: rubble younger than this, s, is left to fly

    // The materials the inventory holds, in hotbar order (D4's physics materials that break).
    inline const char* const MATERIALS[] = {"grass", "dirt", "sand", "stone", "wood", "leaves", "mushroom"};

    struct Inventory {
        std::map<std::string, double> voxels;   // by material
        double blocks(const std::string& material) const {
            auto it = voxels.find(material);
            return it == voxels.end() ? 0.0 : it->second / VOXELS_PER_BLOCK;
        }
    };

    // A piece on its way to you: gone when the pull is over.
    struct Pull { Entity entity; float time = 0.0f; bool debris = true; };

    struct Gather {
        std::vector<Entity> near;           // what E would take now
        std::map<std::string, double> offer;   // and what it is worth, voxels by material
        std::string prompt;
        bool collect = false;               // E toggles: rubble near you and where you look is taken as it lands
        std::vector<Pull> pulling;
        double gathered = 0.0;              // voxels, all told (the self-test's)
    };

    inline const tools::RubbleCount& countOf(World& world, Entity e) {
        using tools::RubbleCount;
        if (const RubbleCount* c = world.try_get<RubbleCount>(e)) return *c;
        const auto& link = world.get<projv::VoxelComponent>(e);
        return world.emplace<RubbleCount>(e, tools::countVoxels(world.ctx().get<projv::Scene>(), link.handle));
    }

    inline vec3 debrisCentre(const projv::Debris& d) { return d.position + d.rotation * d.centre; }

    inline std::string blocksText(double voxels) {
        char text[32];
        std::snprintf(text, sizeof text, voxels / VOXELS_PER_BLOCK < 10.0 ? "%.1f" : "%.0f", voxels / VOXELS_PER_BLOCK);
        return text;
    }

    // What lies round `at`: the rubble E would take.
    // `radius`: how far round `at`; `youngest`: rubble made after this (Tools::time) is left alone.
    inline void lookAround(World& world, Gather& g, vec3 at, Entity island, Entity walker, float radius = GATHER_RADIUS, float youngest = 1e30f) {
        auto take = [&](Entity e) {
            if (world.all_of<Pull>(e)) return;   // already on its way
            if (std::find(g.near.begin(), g.near.end(), e) != g.near.end()) return;
            if (const tools::Rubble* r = world.try_get<tools::Rubble>(e); r && r->born > youngest) return;
            const tools::RubbleCount& c = countOf(world, e);
            if (c.total == 0 || c.total > MAX_RUBBLE_VOXELS) return;
            const float weight = world.all_of<tools::Rubble>(e) ? world.get<tools::Rubble>(e).weight : 1.0f;
            for (const auto& [material, n] : c.voxels) g.offer[material] += double(n) * weight;
            g.near.push_back(e);
        };
        for (auto [e, d] : world.view<projv::Debris>().each())
            if (world.all_of<projv::VoxelComponent>(e) && glm::distance(debrisCentre(d), at) < radius) take(e);
        for (Entity e : runtime::overlapSphere(world, at, radius, 0xffffffffu, walker))
            if (e != island && world.all_of<projv::Piece>(e) && world.all_of<projv::VoxelComponent>(e)) take(e);
    }

    // Takes what lookAround found: into the inventory, and the pieces pulled toward `eye`.
    inline void take(World& world, Gather& g, Inventory& inventory) {
        for (const auto& [material, n] : g.offer) {
            inventory.voxels[material] += n;
            g.gathered += n;
        }
        for (Entity e : g.near) {
            const bool debris = world.all_of<projv::Debris>(e);
            world.emplace_or_replace<Pull>(e, Pull{e, 0.0f, debris});
            g.pulling.push_back(Pull{e, 0.0f, debris});
        }
        g.near.clear();
        g.offer.clear();
    }

    // The pull: each piece flies at you for a moment, then is gone.
    inline void pull(World& world, Gather& g, vec3 eye, float dt) {
        auto& scene = world.ctx().get<projv::Scene>();
        for (Pull& p : g.pulling) {
            p.time += dt;
            if (!world.valid(p.entity)) continue;
            if (p.time < PULL_SECONDS) {
                if (projv::Debris* d = world.try_get<projv::Debris>(p.entity)) {
                    d->velocity = (eye - vec3(0.0f, 0.5f, 0.0f) - debrisCentre(*d)) / std::max(PULL_SECONDS - p.time, 0.05f);
                    d->resting = false;
                } else if (const projv::Transform* t = world.try_get<projv::Transform>(p.entity)) {
                    runtime::setVelocity(world, p.entity, (eye - t->position) / std::max(PULL_SECONDS - p.time, 0.05f));
                }
                continue;
            }
            // Gone. Debris takes its component with it; a body piece keeps the island's link policy, so its
            // component is deleted here.
            const projv::ComponentHandle h = world.all_of<projv::VoxelComponent>(p.entity) ? world.get<projv::VoxelComponent>(p.entity).handle
                                                                                            : projv::INVALID_COMPONENT_HANDLE;
            world.destroy(p.entity);
            if (!p.debris && h != projv::INVALID_COMPONENT_HANDLE) projv::utils::deleteComponent(scene, h);
        }
        g.pulling.erase(std::remove_if(g.pulling.begin(), g.pulling.end(),
                                       [&](const Pull& p) { return p.time >= PULL_SECONDS || !world.valid(p.entity); }),
                        g.pulling.end());
    }

    inline void install(World& world) {
        world.ctx().emplace<Inventory>();
        world.ctx().emplace<Gather>();
    }

    // Each frame: what is in view, the prompt, E, the pulls. `allowed`: the cursor is the game's.
    inline void update(World& world, const projv::Input& input, float dt, vec3 eye, vec3 forward, bool allowed) {
        Gather& g = world.ctx().get<Gather>();
        const tools::Tools& t = world.ctx().get<tools::Tools>();
        g.prompt.clear();
        g.near.clear();
        g.offer.clear();
        if (allowed && !input.wantCaptureKeyboard && input.pressed(projv::Key::E)) g.collect = !g.collect;
        const auto hit = runtime::castRay(world, eye, forward, REACH, 0xffffffffu, t.walker);
        if (g.collect) {
            // On: what has landed near where you look, and near you, comes in by itself.
            const float youngest = t.time - SETTLE;
            if (hit) lookAround(world, g, hit->position, t.island, t.walker, GATHER_RADIUS, youngest);
            lookAround(world, g, eye - vec3(0.0f, 1.2f, 0.0f), t.island, t.walker, MAGNET, youngest);
            if (!g.near.empty()) take(world, g, world.ctx().get<Inventory>());
        } else if (hit) {
            // Off: say what is there, and how to take it.
            lookAround(world, g, hit->position, t.island, t.walker);
            if (!g.near.empty()) {
                std::vector<std::pair<double, std::string>> parts;
                for (const auto& [material, n] : g.offer) parts.push_back({n, material});
                std::sort(parts.rbegin(), parts.rend());
                g.prompt = "E: collect";
                for (size_t i = 0; i < parts.size() && i < 4; i++) g.prompt += (i ? ", " : " ") + blocksText(parts[i].first) + " " + parts[i].second;
                g.near.clear();
                g.offer.clear();
            }
        }
        pull(world, g, eye, dt);
    }

    // The inventory as a line of text (a placeholder for I5's hotbar).
    inline std::string inventoryText(const Inventory& inventory) {
        std::string out;
        for (const char* m : MATERIALS) {
            const double b = inventory.blocks(m);
            if (b <= 0.0) continue;
            out += (out.empty() ? "" : "   ") + std::string(m) + " " + blocksText(b * VOXELS_PER_BLOCK);
        }
        return out;
    }
}

#endif
