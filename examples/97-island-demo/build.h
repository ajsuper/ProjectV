// Building (docs/plans/island-demo.md, D8, D17-D19; I3, I7): with a material in hand (hotbar slots 3-9),
// blocks of 0.5 m -- 4 x 4 x 4 voxels at 0.125 m -- on a 0.5 m grid, against the face you aim at. Scrap
// Mechanic's drag: press on a face, and that cell is one corner of a box; while the button is held, the cell
// you aim at now (or, aiming at nothing, the point at the first corner's distance along your view) is the
// other; release, and the whole box goes in. A click is one block. The right button cancels a drag.
//
// A box goes into the island itself (runtime::editVoxels, the material's own palette slots, one edit), so
// what is built is ground like any other. It costs the voxels it fills: only empty voxels are filled, and
// cells you stand in are left out. Plants in it are cleared. Before it is placed it is shown -- brackets at
// its corners, gold if you have enough, red if not -- and, while dragging, its size and price on the HUD.
#ifndef ISLAND_DEMO_BUILD_H
#define ISLAND_DEMO_BUILD_H

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "core/application.h"
#include "graphics/input.h"
#include "runtime/physics.h"
#include "utils/editing.h"

#include "critters.h"
#include "gather.h"
#include "island.h"
#include "tools.h"

namespace build {
    using projv::Entity;
    using projv::World;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;
    namespace runtime = projv::runtime;

    constexpr int   BLOCK = 4;                       // voxels on a side
    constexpr float BLOCK_SIZE = BLOCK * island::VOXEL;
    constexpr float REACH = 6.0f;                    // to start a box, or to aim its far corner at a face
    constexpr int   MAX_SIDE = 16;                   // blocks a box may span on a side (8 m)

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

    // A box of cells, as island voxel coordinates of its lowest cell's and its highest cell's lowest corners.
    struct Box {
        ivec3 lo{0}, hi{0};
        ivec3 cells() const { return (hi - lo) / BLOCK + ivec3(1); }
        bool operator==(const Box& o) const { return lo == o.lo && hi == o.hi; }
    };
    // The box from corner `a` to corner `b`, no more than MAX_SIDE cells from `a` along any axis.
    inline Box boxOf(ivec3 a, ivec3 b) {
        const ivec3 reach(BLOCK * (MAX_SIDE - 1));
        b = glm::clamp(b, a - reach, a + reach);
        return {glm::min(a, b), glm::max(a, b)};
    }

    // What placing a box would do: the voxels it fills, the plants it clears, and why not if it cannot.
    struct Fill {
        std::vector<projv::PendingVoxelOp> ops, clear;
        int cells = 0;               // cells it puts anything into
        std::string refused;
    };

    struct Build {
        bool  dragging = false;
        ivec3 anchor{0};             // the corner pressed on
        float anchorDistance = 0.0f; // from the eye, for aiming at nothing
        bool  wasDown = false;
        // What is shown: the box under the crosshair or being dragged, and whether it can be placed.
        bool  showing = false;
        Box   box;
        bool  affordable = false;
        std::string status;          // the HUD's, while dragging: "4 x 3 x 1   12.0 of 31.4 stone"
        // The last fill worked out, for the box and material it was worked out for.
        Box   filledBox;
        std::string filledMaterial;
        double filledHave = -1.0;
        Fill  fill;
        int   placed = 0;            // boxes, this session
        int   lastFilled = 0;        // voxels the last box filled (the self-test's)
        std::string refused;         // why the last try placed nothing
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

    // What placing `box` of `material` would fill.
    inline Fill fillFor(World& world, const std::string& material, const Box& box) {
        Fill f;
        tools::Tools& t = world.ctx().get<tools::Tools>();
        auto& scene = world.ctx().get<projv::Scene>();
        const projv::SceneGrid* ground = gridOf(world, world.get<projv::VoxelComponent>(t.island).handle);
        const projv::SceneGrid* plants = t.plants != projv::INVALID_COMPONENT_HANDLE ? gridOf(world, t.plants) : nullptr;
        if (!ground) { f.refused = "no island"; return f; }
        // You: the walker's capsule, as a box. Cells it is in are left out.
        vec3 plo(1e9f), phi(-1e9f);
        if (const projv::Transform* w = world.try_get<projv::Transform>(t.walker)) {
            plo = w->position - vec3(0.32f, 0.0f, 0.32f);
            phi = w->position + vec3(0.32f, 1.85f, 0.32f);
        }
        bool inTheWay = false;
        for (int cz = box.lo.z; cz <= box.hi.z; cz += BLOCK)
            for (int cy = box.lo.y; cy <= box.hi.y; cy += BLOCK)
                for (int cx = box.lo.x; cx <= box.hi.x; cx += BLOCK) {
                    const ivec3 corner(cx, cy, cz);
                    const vec3 lo = vec3(corner) * island::VOXEL, hi = lo + vec3(BLOCK_SIZE);
                    if (glm::all(glm::lessThan(lo, phi)) && glm::all(glm::lessThan(plo, hi))) { inTheWay = true; continue; }
                    const size_t before = f.ops.size();
                    for (int z = 0; z < BLOCK; z++)
                        for (int y = 0; y < BLOCK; y++)
                            for (int x = 0; x < BLOCK; x++) {
                                const ivec3 v = corner + ivec3(x, y, z);
                                if (v.y < 0 || v.y >= island::COLUMN) continue;
                                if (plants && tools::plantAt(scene, *plants, v)) f.clear.push_back({false, v, 0});
                                if (tools::plantAt(scene, *ground, v)) continue;   // ground already: left as it is
                                projv::PendingVoxelOp op{true, v, 0};
                                op.slot = shadeAt(material, v);
                                f.ops.push_back(op);
                            }
                    if (f.ops.size() > before) f.cells++;
                }
        if (f.ops.empty()) f.refused = inTheWay ? "you are in the way" : "something is already there";
        return f;
    }

    // The fill for `box`, worked out again only when the box, the material or what you have changed.
    inline const Fill& fillOf(World& world, Build& b, const std::string& material, const Box& box) {
        const double have = world.ctx().get<gather::Inventory>().voxels[material];
        if (!(b.filledBox == box) || b.filledMaterial != material || b.filledHave != have) {
            b.fill = fillFor(world, material, box);
            b.filledBox = box;
            b.filledMaterial = material;
            b.filledHave = have;
        }
        return b.fill;
    }

    // Places `box` of `material`: true if it did (the edit is issued; it lands at the next physics step).
    inline bool placeBox(World& world, Build& b, const std::string& material, const Box& box) {
        tools::Tools& t = world.ctx().get<tools::Tools>();
        gather::Inventory& inventory = world.ctx().get<gather::Inventory>();
        Fill f = fillFor(world, material, box);
        if (!f.refused.empty()) { b.refused = f.refused; return false; }
        if (inventory.voxels[material] < double(f.ops.size()) - 1e-6) { b.refused = "not enough " + material; return false; }
        auto& scene = world.ctx().get<projv::Scene>();
        runtime::editVoxels(world, t.island, f.ops);
        if (!f.clear.empty()) {
            projv::utils::queueVoxelRemove(scene, t.plants, f.clear);
            projv::utils::updateScene(scene);
        }
        inventory.voxels[material] -= double(f.ops.size());   // the voxels it filled
        b.lastFilled = int(f.ops.size());
        b.placed++;
        b.refused.clear();
        b.filledHave = -1.0;   // what you have changed
        // A puff as it lands: at a few of its cells.
        const ivec3 n = box.cells();
        const int every = std::max(1, (n.x * n.y * n.z) / 6);
        int i = 0;
        for (int cz = box.lo.z; cz <= box.hi.z; cz += BLOCK)
            for (int cy = box.lo.y; cy <= box.hi.y; cy += BLOCK)
                for (int cx = box.lo.x; cx <= box.hi.x; cx += BLOCK)
                    if (i++ % every == 0)
                        t.bursts.push_back({(vec3(cx, cy, cz) + vec3(BLOCK * 0.5f)) * island::VOXEL, vec3(0, 1, 0),
                                            shadeAt(material, ivec3(cx, cy, cz)), 8, 1.2f});
        return true;
    }

    // One block in the cell at `corner` (the self-test's, and a click's).
    inline bool place(World& world, Build& b, const std::string& material, ivec3 corner) {
        return placeBox(world, b, material, {corner, corner});
    }

    // ---- The preview: brackets at a box's eight corners ---------------------------------------------------

    constexpr float BRACKET_VOXEL = 0.03f;   // 3 cm
    constexpr int   BRACKET_LENGTH = 5;      // voxels an arm: 15 cm
    struct Preview {
        std::array<Entity, 24> gold{}, red{};   // eight corners, three arms each; one set shown
        bool shown = false;
        Box  box;
        bool affordable = false;
    };

    // The turn that lays an arm's local +x along `axis` (0 x, 1 y, 2 z), towards + or -.
    inline quat armTurn(int axis, bool positive) {
        const float h = 0.70710678f;
        if (axis == 0) return positive ? quat(1.0f, 0.0f, 0.0f, 0.0f) : quat(0.0f, 0.0f, 1.0f, 0.0f);   // -x: half a turn about y
        if (axis == 1) return positive ? quat(h, 0.0f, 0.0f, h) : quat(h, 0.0f, 0.0f, -h);             // about z
        return positive ? quat(h, 0.0f, -h, 0.0f) : quat(h, 0.0f, h, 0.0f);                            // about y
    }

    inline void install(World& world) {
        world.ctx().emplace<Build>();
        auto& scene = world.ctx().get<projv::Scene>();
        Preview& p = world.ctx().emplace<Preview>();
        // A palette of two glowing colours, so the brackets read in shade as in sun.
        auto glow = [](float r, float g, float b) {
            projv::Material m;
            m.packedColor = projv::packRGB10(r, g, b);
            m.packedEmission = projv::packRGB10(r, g, b);
            m.packedExtra = projv::packExtraWord(1.5f, 0.0f, 0u);
            m.name = "bracket";
            return m;
        };
        const std::vector<projv::Material> palette = {glow(1.0f, 0.82f, 0.25f), glow(1.0f, 0.3f, 0.25f)};
        std::vector<std::pair<Entity*, projv::ComponentHandle>> link;
        for (int set = 0; set < 2; set++)
            for (int i = 0; i < 24; i++) {
                std::vector<projv::PendingVoxelOp> arm;
                for (int x = 0; x < BRACKET_LENGTH; x++) {
                    projv::PendingVoxelOp op{true, ivec3(x, 0, 0), 0};
                    op.slot = uint8_t(set);
                    arm.push_back(op);
                }
                const projv::ComponentHandle h = critters::chunk(scene, set ? "bracket (red)" : "bracket (gold)", projv::INVALID_COMPONENT_HANDLE,
                                                                 BRACKET_VOXEL, vec3(0.0f), {}, 16);
                scene.components[h].materialPalette = palette;
                scene.components[h].paletteVersion++;
                projv::utils::queueVoxelAdd(scene, h, arm);
                link.push_back({set ? &p.red[size_t(i)] : &p.gold[size_t(i)], h});
            }
        projv::utils::updateScene(scene);
        for (auto& [entity, component] : link) {
            *entity = runtime::spawnComponent(world, component);
            if (projv::Transform* t = world.try_get<projv::Transform>(*entity)) t->position = vec3(island::CENTRE, -40.0f, island::CENTRE);
        }
    }

    // Each frame: the brackets round what Build shows, gold or red, or out of sight.
    inline void showPreview(World& world) {
        const Build& b = world.ctx().get<Build>();
        Preview& p = world.ctx().get<Preview>();
        if (p.shown == b.showing && (!b.showing || (p.box == b.box && p.affordable == b.affordable))) return;
        p.shown = b.showing;
        p.box = b.box;
        p.affordable = b.affordable;
        const vec3 hidden(island::CENTRE, -40.0f, island::CENTRE);
        const vec3 lo = vec3(b.box.lo) * island::VOXEL - vec3(0.02f), hi = vec3(b.box.hi + ivec3(BLOCK)) * island::VOXEL + vec3(0.02f);
        const vec3 axes[3] = {vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1)};
        for (int set = 0; set < 2; set++) {
            const bool on = b.showing && (set == 0) == b.affordable;
            auto& arms = set ? p.red : p.gold;
            for (int corner = 0; corner < 8; corner++)
                for (int a = 0; a < 3; a++) {
                    const Entity e = arms[size_t(corner * 3 + a)];
                    projv::Transform* t = world.try_get<projv::Transform>(e);
                    if (!t) continue;
                    if (!on) {
                        world.patch<projv::Transform>(e, [&](projv::Transform& x) { x.position = hidden; });
                        continue;
                    }
                    const ivec3 high((corner & 1) != 0, (corner & 2) != 0, (corner & 4) != 0);
                    const vec3 at(high.x ? hi.x : lo.x, high.y ? hi.y : lo.y, high.z ? hi.z : lo.z);
                    const vec3 inward = axes[a] * (high[a] ? -1.0f : 1.0f);   // along the edge, into the box
                    // The arm is along its local +x, a voxel thick: centre its cross-section on the edge.
                    const quat turn = armTurn(a, inward[a] > 0.0f);
                    const vec3 position = at - turn * vec3(0.0f, 0.5f, 0.5f) * BRACKET_VOXEL;
                    world.patch<projv::Transform>(e, [&](projv::Transform& x) {
                        x.position = position;
                        x.rotation = turn;
                        x.scale = 1.0f;
                    });
                }
        }
    }

    // ---- Each frame ------------------------------------------------------------------------------------

    // With a material in hand: the box under the crosshair shown; `down` (the left button held) starts, grows
    // and, let go, places a box; `cancel` (the right button) drops one being dragged.
    inline void update(World& world, vec3 eye, vec3 forward, bool down, bool cancel) {
        Build& b = world.ctx().get<Build>();
        tools::Tools& t = world.ctx().get<tools::Tools>();
        const char* material = materialOf(t.slot);
        const bool pressed = down && !b.wasDown, released = !down && b.wasDown;
        b.wasDown = down;
        b.showing = false;
        b.status.clear();
        if (!material) {
            b.dragging = false;
            showPreview(world);
            return;
        }
        if (b.dragging && cancel) b.dragging = false;
        // The cell aimed at: just outside the face under the crosshair (only the island: a box must touch
        // the ground it joins, never a falling piece).
        const auto hit = runtime::castRay(world, eye, forward, b.dragging ? REACH * 2.0f : REACH, 0xffffffffu, t.walker);
        const bool onIsland = hit && hit->entity == t.island;
        const std::optional<ivec3> aimed = onIsland ? std::optional<ivec3>(cellFor(hit->position, hit->normal)) : std::nullopt;

        if (!b.dragging && pressed) {
            if (!aimed) {
                b.refused = "nothing to build on";
            } else {
                b.dragging = true;
                b.anchor = *aimed;
                b.anchorDistance = glm::length((vec3(b.anchor) + vec3(BLOCK * 0.5f)) * island::VOXEL - eye);
            }
        }
        if (b.dragging) {
            // The far corner: the cell aimed at, or the point at the anchor's distance along the view.
            ivec3 other = b.anchor;
            if (aimed) other = *aimed;
            else other = ivec3(glm::floor((eye + forward * b.anchorDistance) / BLOCK_SIZE)) * BLOCK;
            b.box = boxOf(b.anchor, other);
            b.showing = true;
            const Fill& f = fillOf(world, b, material, b.box);
            const double have = world.ctx().get<gather::Inventory>().voxels[material];
            b.affordable = f.refused.empty() && have >= double(f.ops.size()) - 1e-6;
            const ivec3 n = b.box.cells();
            char text[160];
            std::snprintf(text, sizeof text, "%d x %d x %d   %.1f of %.1f %s", n.x, n.y, n.z,
                          double(f.ops.size()) / gather::VOXELS_PER_BLOCK, have / gather::VOXELS_PER_BLOCK, material);
            b.status = text;
            if (!f.refused.empty()) b.status += "  -  " + f.refused;
            if (released) {
                b.dragging = false;
                if (placeBox(world, b, material, b.box)) {
                    t.swing = 0.0f;   // the hand's jab (fx.h)
                    t.swingTool = tools::Tool::Chisel;
                    t.landed = true;
                }
            }
        } else if (aimed) {
            // Not dragging: the one cell a click would fill.
            b.box = {*aimed, *aimed};
            b.showing = true;
            const Fill& f = fillOf(world, b, material, b.box);
            b.affordable = f.refused.empty() && world.ctx().get<gather::Inventory>().voxels[material] >= double(f.ops.size()) - 1e-6;
        }
        showPreview(world);
    }
}

#endif
