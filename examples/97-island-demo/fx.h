// The look of the tools: the one in hand, held at the right of the view and swung when used, and the little
// particles a strike throws off -- voxels smaller than the island's, in the colours of what was hit, that
// fly, fall and shrink away in about a third of a second. Looks only: no physics, nothing saved.
//
// Each is a small component of its own moved through its entity's Transform (as critters.h). The particles
// are a pool, each one voxel whose palette is the island's, so a burst names island palette slots
// (PendingVoxelOp::slot) and never grows a palette.
#ifndef ISLAND_DEMO_FX_H
#define ISLAND_DEMO_FX_H

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "core/application.h"
#include "runtime/scene_bridge.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

#include "build.h"
#include "critters.h"
#include "island.h"
#include "tools.h"

namespace fx {
    using projv::Entity;
    using projv::World;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;

    constexpr float TOOL_VOXEL = 0.019f;    // 16 of them: a tool about 30 cm, drawn smaller than life
    constexpr int   PARTICLES = 160;
    constexpr float PARTICLE_VOXEL = 0.04f;
    const vec3 HIDDEN(island::CENTRE, -40.0f, island::CENTRE);

    struct Particle {
        Entity entity = projv::NullEntity;
        projv::ComponentHandle component = projv::INVALID_COMPONENT_HANDLE;
        vec3   position{0.0f}, velocity{0.0f};
        float  age = 0.0f, life = 0.0f;   // life 0: free
        float  spin = 0.0f;
        uint8_t slot = 0xff;              // the colour it has now
    };
    constexpr float HELD_BLOCK_VOXEL = 0.035f;   // the block in hand: 4 of them a side, 14 cm

    struct Fx {
        Entity pickaxe = projv::NullEntity, chisel = projv::NullEntity, block = projv::NullEntity;
        projv::ComponentHandle blockComponent = projv::INVALID_COMPONENT_HANDLE;
        std::string blockMaterial;   // what the block in hand is shaded as now
        std::vector<Particle> particles;
        size_t next = 0;
        critters::Random random{12345u};
        float bob = 0.0f;
    };

    inline uint32_t rgb(int r, int g, int b) { return critters::rgb(r, g, b); }

    // The held tools are pixel art in voxels: a 16 x 16 picture in the y-z plane (row 0 the top), two voxels
    // thick, chunky the way a game's tool is drawn. '#' outline, 'o' head (light), 'm' head (mid), 'h'
    // handle, 'd' handle (dark), '.' empty. The grip, where the hand holds it, is at GRIP.
    inline const char* const PICKAXE_ART[16] = {
        "....######......",
        "..##oooooo##....",
        ".#oommmmmmoo#...",
        "#om##mhhm##mo#..",
        "#m#..#hd#..#mo#.",
        "##...#hd#...#m#.",
        ".....#hd#....##.",
        ".....#hd#.......",
        ".....#hh#.......",
        ".....#dh#.......",
        ".....#hd#.......",
        ".....#hh#.......",
        ".....#dh#.......",
        ".....#hd#.......",
        ".....#hh#.......",
        ".....###........",
    };
    inline const char* const CHISEL_ART[16] = {
        "......##........",
        ".....#oo#.......",
        ".....#om#.......",
        ".....#om#.......",
        ".....#mm#.......",
        ".....#mm#.......",
        "....##mm##......",
        "....#dhhd#......",
        "....#hhhh#......",
        "....#hdhh#......",
        "....#hhhh#......",
        "....#hhdh#......",
        "....#hhhh#......",
        "....#dhhd#......",
        ".....####.......",
        "................",
    };
    constexpr float TOOL_DEPTH = 2;                  // voxels thick
    const vec3 GRIP(1.0f, 3.0f, 6.5f);               // in voxels: low on the handle

    // The art's inks (the HUD's icons, icons.h, draw the same pictures).
    struct Ink { char key; int r, g, b; };
    constexpr Ink INKS[] = {{'#', 40, 34, 30}, {'o', 215, 220, 228}, {'m', 150, 156, 168}, {'h', 160, 110, 62}, {'d', 110, 72, 40}};
    inline const Ink* inkOf(char key) {
        for (const Ink& ink : INKS)
            if (ink.key == key) return &ink;
        return nullptr;
    }

    inline std::vector<projv::PendingVoxelOp> artVoxels(const char* const art[16]) {
        std::vector<projv::PendingVoxelOp> v;
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 16; col++) {
                const Ink* ink = inkOf(art[row][col]);
                if (!ink) continue;
                const uint32_t c = rgb(ink->r, ink->g, ink->b);
                for (int x = 0; x < int(TOOL_DEPTH); x++) v.push_back({true, ivec3(x, 15 - row, col), c});
            }
        return v;
    }
    inline std::vector<projv::PendingVoxelOp> pickaxeVoxels() { return artVoxels(PICKAXE_ART); }
    inline std::vector<projv::PendingVoxelOp> chiselVoxels() { return artVoxels(CHISEL_ART); }

    inline void heldTool(projv::Scene& scene, const std::string& name, const std::vector<projv::PendingVoxelOp>& voxels,
                         std::vector<std::pair<Entity*, projv::ComponentHandle>>& link, Entity& out) {
        const projv::ComponentHandle a = critters::asset(scene, name, projv::INVALID_COMPONENT_HANDLE);
        critters::chunk(scene, name + " voxels", a, TOOL_VOXEL, -GRIP * TOOL_VOXEL, voxels, 16);
        link.push_back({&out, a});
    }

    // Adds the held tools and the particle pool. Call with the critters (before the scene goes to the GPU).
    inline void install(World& world, projv::ComponentHandle islandComponent) {
        auto& scene = world.ctx().get<projv::Scene>();
        Fx& f = world.ctx().emplace<Fx>();
        std::vector<std::pair<Entity*, projv::ComponentHandle>> link;
        heldTool(scene, "pickaxe (held)", pickaxeVoxels(), link, f.pickaxe);
        heldTool(scene, "chisel (held)", chiselVoxels(), link, f.chisel);
        const std::vector<projv::Material> palette = scene.components[islandComponent].materialPalette;
        // The block in hand: 4 x 4 x 4 voxels on the island's palette, reshaded when the material changes.
        {
            const projv::ComponentHandle a = critters::asset(scene, "block (held)", projv::INVALID_COMPONENT_HANDLE);
            f.blockComponent = critters::chunk(scene, "block (held) voxels", a, HELD_BLOCK_VOXEL, -vec3(2.0f) * HELD_BLOCK_VOXEL, {}, 4);
            scene.components[f.blockComponent].materialPalette = palette;
            scene.components[f.blockComponent].paletteVersion++;
            std::vector<projv::PendingVoxelOp> ops;
            for (int z = 0; z < 4; z++)
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        projv::PendingVoxelOp op{true, ivec3(x, y, z), 0};
                        op.slot = uint8_t(island::Dirt0);
                        ops.push_back(op);
                    }
            projv::utils::queueVoxelAdd(scene, f.blockComponent, ops);
            link.push_back({&f.block, a});
        }
        // The particles: each a chunk with the island's palette, its one voxel set by slot when it is used.
        std::vector<projv::ComponentHandle> handles;
        for (int i = 0; i < PARTICLES; i++) {
            const projv::ComponentHandle h =
                critters::chunk(scene, "particle " + std::to_string(i), projv::INVALID_COMPONENT_HANDLE, PARTICLE_VOXEL, HIDDEN, {}, 4);
            scene.components[h].materialPalette = palette;
            scene.components[h].paletteVersion++;
            projv::PendingVoxelOp op{true, ivec3(0), 0};
            op.slot = uint8_t(island::Dirt0);
            projv::utils::queueVoxelAdd(scene, h, {op});
            handles.push_back(h);
        }
        projv::utils::updateScene(scene);
        for (auto& [entity, component] : link) *entity = projv::runtime::spawnComponent(world, component);
        for (projv::ComponentHandle h : handles) {
            Particle p;
            p.component = h;
            p.slot = uint8_t(island::Dirt0);
            p.entity = projv::runtime::spawnComponent(world, h);
            f.particles.push_back(p);
        }
    }

    inline void place(World& world, Entity e, vec3 position, quat rotation, float scale = 1.0f) {
        if (!world.valid(e) || !world.all_of<projv::Transform>(e)) return;
        world.patch<projv::Transform>(e, [&](projv::Transform& t) {
            t.position = position;
            t.rotation = rotation;
            t.scale = scale;
        });
    }

    // A shade of the same material as `slot`, now and then, so a burst is not all one colour.
    inline uint8_t shadeOf(uint8_t slot, critters::Random& random) {
        if (slot >= island::SLOTS || random.next() < 0.6f) return slot;
        std::vector<uint8_t> same;
        for (int i = 0; i < island::SLOTS; i++)
            if (std::string(island::PALETTE[i].name) == island::PALETTE[slot].name) same.push_back(uint8_t(i));
        return same[size_t(random.next() * float(same.size())) % same.size()];
    }

    // Each frame, after the camera: the held tool where the view puts it, swung by the tool's swing; the
    // bursts the strikes asked for; the particles flown.
    inline void update(World& world, float dt, vec3 eye, float yaw, float pitch, float walking) {
        Fx* found = world.ctx().find<Fx>();
        if (!found) return;
        Fx& f = *found;
        tools::Tools& t = world.ctx().get<tools::Tools>();
        auto& scene = world.ctx().get<projv::Scene>();
        dt = std::min(dt, 0.1f);

        // The view's frame: right, up, forward.
        const vec3 forward(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw));
        const vec3 right = glm::normalize(glm::cross(forward, vec3(0, 1, 0)));
        const vec3 up = glm::cross(right, forward);
        const quat view = glm::quat_cast(projv::core::mat3(right, up, -forward));
        f.bob += dt * walking * 9.0f;
        const vec3 sway = right * (0.012f * std::sin(f.bob)) + up * (0.01f * std::abs(std::cos(f.bob)));

        // The swing: back a little, down hard to the strike, and back up; the chisel jabs forward instead.
        const bool pick = t.holdingTool() && t.selected == tools::Tool::Pickaxe;
        float angle = 0.0f, jab = 0.0f;
        if (t.swing >= 0.0f) {
            const float impact = t.swingTool == tools::Tool::Pickaxe ? tools::PICKAXE_IMPACT : tools::CHISEL_IMPACT;
            const float total = t.swingTool == tools::Tool::Pickaxe ? tools::PICKAXE_EVERY : tools::CHISEL_EVERY;
            const float s = t.swing;
            const float wind = 0.35f, strike = -1.25f;
            if (s < impact * 0.35f) angle = wind * (s / (impact * 0.35f));
            else if (s < impact) { const float k = (s - impact * 0.35f) / (impact * 0.65f); angle = wind + (strike - wind) * k * k; }
            else { const float k = std::min(1.0f, (s - impact) / std::max(total - impact, 0.01f)); angle = strike * (1.0f - k * k * (3.0f - 2.0f * k)); }
            if (t.swingTool == tools::Tool::Chisel) { jab = -angle * 0.12f; angle *= 0.25f; }
        }
        const vec3 grip = eye + right * 0.3f - up * 0.34f + forward * 0.42f + forward * jab + sway;
        // At rest the tool leans forward and in, the head up and to the front.
        const quat rest = glm::angleAxis(-0.35f, vec3(1, 0, 0)) * glm::angleAxis(0.25f, vec3(0, 0, 1)) * glm::angleAxis(0.4f, vec3(0, 1, 0));
        const quat held = view * glm::angleAxis(angle, vec3(1, 0, 0)) * rest;
        const bool tool = t.holdingTool();
        place(world, f.pickaxe, tool && pick ? grip : HIDDEN, held);
        place(world, f.chisel, tool && !pick ? grip : HIDDEN, held);
        // A material in hand: a little block of it, reshaded when the material changes.
        if (const char* material = build::materialOf(t.slot); material && f.blockMaterial != material) {
            std::vector<projv::PendingVoxelOp> ops;
            for (int z = 0; z < 4; z++)
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        projv::PendingVoxelOp op{true, ivec3(x, y, z), 0};
                        op.slot = build::shadeAt(material, ivec3(x, y, z));
                        ops.push_back(op);
                    }
            projv::utils::queueVoxelAdd(scene, f.blockComponent, ops);
            projv::utils::updateScene(scene);
            f.blockMaterial = material;
        }
        const quat blockHeld = view * glm::angleAxis(angle * 0.5f, vec3(1, 0, 0)) * glm::angleAxis(0.6f, vec3(0, 1, 0)) * glm::angleAxis(0.2f, vec3(1, 0, 0));
        place(world, f.block, tool ? HIDDEN : grip + up * 0.08f, blockHeld);

        // Bursts: particles from the pool, coloured by slot (one voxel edit each, applied together).
        bool edited = false;
        for (const tools::Tools::Burst& b : t.bursts) {
            for (int i = 0; i < b.count; i++) {
                Particle& p = f.particles[f.next];
                f.next = (f.next + 1) % f.particles.size();
                const uint8_t slot = shadeOf(b.slot, f.random);
                if (slot != p.slot) {
                    projv::PendingVoxelOp op{true, ivec3(0), 0};
                    op.slot = slot;
                    projv::utils::queueVoxelAdd(scene, p.component, {op});
                    p.slot = slot;
                    edited = true;
                }
                const vec3 scatter = glm::normalize(vec3(f.random.range(-1, 1), f.random.range(-1, 1), f.random.range(-1, 1)) + vec3(1e-4f));
                p.position = b.at + b.normal * 0.05f + scatter * 0.06f;
                p.velocity = (b.normal * f.random.range(0.6f, 1.0f) + scatter * 0.7f + vec3(0.0f, 0.5f, 0.0f)) * b.speed * f.random.range(0.6f, 1.3f);
                p.age = 0.0f;
                p.life = f.random.range(0.3f, 0.4f);
                p.spin = f.random.range(-12.0f, 12.0f);
            }
        }
        t.bursts.clear();
        if (edited) projv::utils::updateScene(scene);

        for (Particle& p : f.particles) {
            if (p.life <= 0.0f) continue;
            p.age += dt;
            if (p.age >= p.life) {
                p.life = 0.0f;
                place(world, p.entity, HIDDEN, quat(1, 0, 0, 0), 1.0f);
                continue;
            }
            p.velocity.y -= 9.8f * dt;
            p.velocity *= std::exp(-1.5f * dt);
            p.position += p.velocity * dt;
            const float k = p.age / p.life;
            const float scale = 1.0f - k * k;   // shrinks away, slowly at first
            place(world, p.entity, p.position - vec3(0.5f * PARTICLE_VOXEL * scale), glm::angleAxis(p.spin * p.age, glm::normalize(vec3(1, 2, 3))),
                  std::max(scale, 0.05f));
        }
    }
}

#endif
