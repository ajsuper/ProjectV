// What the explosion example's parts share: the voxel-building helpers, the state (Demo), what the blast
// acts on (Target), and what a map is (MapSpec). maps.h builds the three maps from these; main.cpp blows
// them up.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "core/application.h"
#include "runtime/physics.h"
#include "runtime/scene_bridge.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/scene_query.h"

#include "mushroom.h"
#include "night_renderer.h"

namespace explosion {
    using projv::ComponentHandle;
    using projv::Entity;
    using projv::NullEntity;
    using projv::World;
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;
    using projv::core::vec4;
    namespace runtime = projv::runtime;
    namespace utils = projv::utils;

    constexpr float PI = 3.14159265f;
    constexpr int SHARD_FAMILY_STEPS = 15;   // fixed steps a break's shards pass through each other
    const quat IDENTITY(1, 0, 0, 0);

    using Clock = std::chrono::steady_clock;
    inline double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

    inline uint32_t rgb(float r, float g, float b) { return projv::packRGB10(r, g, b); }
    inline uint32_t rgb(vec3 c) { return projv::packRGB10(c.r, c.g, c.b); }

    inline uint32_t hashCell(int x, int y, int z) {
        uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        return h ^ (h >> 15);
    }

    // ---- Building voxel things (the helpers Voxel Mayhem and the physics demos use) --------------------

    // A component of `size` voxels of `voxel` metres, coloured by `colourAt` (0 = empty), its corner at
    // `position`.
    template <typename ColourAt>
    ComponentHandle voxels(projv::Scene& scene, const char* name, ivec3 size, float voxel, vec3 position, ColourAt colourAt,
                           ComponentHandle parent = projv::INVALID_COMPONENT_HANDLE) {
        ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, name, parent, 16, voxel);
        utils::setComponentTransform(scene, h, position, IDENTITY, 1.0f);
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = 0; z < size.z; z++)
            for (int y = 0; y < size.y; y++)
                for (int x = 0; x < size.x; x++)
                    if (uint32_t c = colourAt(x, y, z)) ops.push_back({true, ivec3(x, y, z), c});
        utils::queueVoxelAdd(scene, h, ops);
        utils::updateScene(scene);
        return h;
    }

    // Palette entries of these colours glow, each by its strength.
    inline void glowColours(projv::Scene& scene, ComponentHandle h, const std::map<uint32_t, float>& strengths) {
        const auto& palette = scene.components[h].materialPalette;
        for (size_t slot = 0; slot < palette.size(); slot++)
            if (auto found = strengths.find(palette[slot].packedColor); found != strengths.end())
                utils::setMaterialProperties(scene, h, uint8_t(slot), palette[slot].packedColor,
                                             projv::packSurfaceWord(0.0f, 0.0f, 0.0f, 1.0f),
                                             projv::packExtraWord(found->second, 0.0f, 0u));
    }

    // Every palette entry glows (0: none does).
    inline void glow(projv::Scene& scene, ComponentHandle h, float strength) {
        std::map<uint32_t, float> all;
        for (const auto& entry : scene.components[h].materialPalette) all[entry.packedColor] = strength;
        glowColours(scene, h, all);
    }

    // Palette entries of these colours are made of these physics materials.
    inline void materials(projv::Scene& scene, ComponentHandle h, const std::map<uint32_t, const char*>& of) {
        const auto& palette = scene.components[h].materialPalette;
        for (size_t slot = 0; slot < palette.size(); slot++)
            if (auto found = of.find(palette[slot].packedColor); found != of.end())
                utils::setMaterialPhysics(scene, h, uint8_t(slot), found->second);
    }

    // A palette being made: each colour names its physics material, and may glow.
    struct Palette {
        std::map<uint32_t, const char*> material;
        std::map<uint32_t, float> glow;
        uint32_t paint(vec3 c, const char* m, float g = 0.0f) {
            const uint32_t packed = rgb(c);
            material[packed] = m;
            if (g > 0.0f) glow[packed] = g;
            return packed;
        }
        void apply(projv::Scene& scene, ComponentHandle h) const {
            materials(scene, h, material);
            glowColours(scene, h, glow);
        }
    };

    // A moving thing built in place: an asset node at `centre` with its voxels centred inside.
    template <typename ColourAt>
    Entity big(World& world, const char* name, ivec3 size, float voxel, vec3 centre, quat rotation, ColourAt colourAt,
               const projv::RigidBody& rigid) {
        auto& scene = world.ctx().get<projv::Scene>();
        ComponentHandle node = utils::addComponent(scene, projv::ComponentKind::Asset, name, projv::INVALID_COMPONENT_HANDLE, 4, 1.0f);
        voxels(scene, name, size, voxel, -0.5f * vec3(size) * voxel, colourAt, node);
        utils::setComponentTransform(scene, node, centre, rotation, 1.0f);
        Entity e = runtime::spawnComponent(world, node, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        world.emplace<projv::RigidBody>(e, rigid);
        runtime::assignNetId(world, e);
        return e;
    }

    // Static scenery, its corner at `corner`.
    template <typename ColourAt>
    Entity scenery(World& world, const char* name, ivec3 size, float voxel, vec3 corner, ColourAt colourAt,
                   projv::StaticCollider collider = {}) {
        auto& scene = world.ctx().get<projv::Scene>();
        ComponentHandle h = voxels(scene, name, size, voxel, corner, colourAt);
        Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        world.emplace<projv::StaticCollider>(e, collider);
        runtime::assignNetId(world, e);
        return e;
    }

    // What breaks, as a static collider: standing on its own y = 0 (anchorBase), so what loses its support
    // comes down, and splitting when it does. Not on impact: the front deals with what is near, and every
    // strike on a body is an island pass over all of it -- the crater's clods alone struck the city's tower
    // dozens of times a step.
    inline projv::StaticCollider breakable(const char* material) {
        projv::StaticCollider c;
        c.material = material;
        c.splits = true;
        c.anchorBase = true;
        c.breaksOnImpact = false;
        return c;
    }

    // ---- The cloud ---------------------------------------------------------------------------------

    // What an ember is part of: that decides how it moves and how it looks.
    //   Fire     the fireball, the stem's fire, the smouldering ruins: carried by the mushroom field, glowing
    //            by its temperature and cooling to charcoal
    //   Dust     raised off the ground and drawn in to the stem's foot
    //   Shock    the shockwave's dust ring, racing out along the ground and slowing
    //   Collar   the condensation ring that bursts out round the stem under the cap, and evaporates
    enum class Kind : uint8_t { Fire, Dust, Shock, Collar };

    // An ember of the cloud: a particle carried by the field (cloudTick), drawn as a little cluster of voxel
    // cubes where it is (gbuffer.frag traces them), glowing and cooling. It carries smoke into the grid.
    // Not a physics body: as bodies, thousands of them were most of the physics step.
    struct Ember {
        // Where it is and was at the last two fixed steps (drawn between them), and how fast it goes.
        vec3 position{0.0f}, previous{0.0f}, velocity{0.0f};
        bool dead = false;
        Kind kind = Kind::Fire;
        // Its look: three to eight voxel lumps about it, each (offset, half edge), and each lump's seed
        // (which of its voxels are missing).
        std::array<vec4, 8> cubes{};
        std::array<float, 8> seeds{};
        int cubeCount = 0;
        vec3 axis{0.0f, 1.0f, 0.0f};
        float spin = 1.0f;
        float age = 0.0f, temperature = 0.0f, cooling = 0.3f;
        float smoke = 1.0f;         // how much smoke it carries
        float chunkLife = 0.0f;     // all but fire: how long its chunk is drawn
        float lifetime = 50.0f;
        float scale = 1.0f;         // its cubes' size this frame (0: not drawn)
        // Shock and Collar: which way out from the middle it rides its ring, how far ahead of or behind the
        // ring's line, how high, and when after the detonation it was made.
        vec3 out{1.0f, 0.0f, 0.0f};
        float offset = 0.0f, height = 0.0f, born = 0.0f;
        float smokeSince = -1.0f;   // pure voxels: its age when it became a smoke puff
    };

    struct Kick { vec3 at; float radius, speed, lift; int steps; };

    // ---- What the blast acts on ------------------------------------------------------------------

    // A tree's voxels: where (in its grove), its colour, and whether it is trunk (or crown).
    struct TreeVoxel {
        ivec3 at{0};
        uint32_t colour = 0;
        bool trunk = false;
    };

    // A tree's shape, stamped into groves (maps.h, plantGrove) as many times as there are trees made from
    // it: its voxels about the foot of its trunk (x and z from the trunk's middle, y from the ground).
    struct TreeShape {
        std::vector<TreeVoxel> voxels;
        int height = 0;             // voxels
        float crownRadius = 0.0f;   // metres
    };

    // Which places of a grove (one static grid of many trees) are filled: so a tree the wind tips over goes
    // only where nothing is, and takes away only what it put down -- trees lying across each other never cut
    // holes in each other (the scraps a hole leaves would come away as bodies of their own).
    //
    // A grove is a static body only where something may strike it (near ground zero); beyond, it is scenery
    // alone, edited in the scene directly: no island pass, no shape rebuilt, at every gust of every tree.
    struct Grove {
        ComponentHandle component = projv::INVALID_COMPONENT_HANDLE;
        bool body = true;
        ivec3 size{0};
        std::vector<uint64_t> bits;
        size_t index(ivec3 p) const { return (size_t(p.y) * size_t(size.z) + size_t(p.z)) * size_t(size.x) + size_t(p.x); }
        bool inside(ivec3 p) const { return p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < size.x && p.y < size.y && p.z < size.z; }
        bool filled(ivec3 p) const { return inside(p) && (bits[index(p) / 64] >> (index(p) % 64) & 1u); }
        void fill(ivec3 p, bool on) {
            if (!inside(p)) return;
            if (on) bits[index(p) / 64] |= uint64_t(1) << (index(p) % 64);
            else bits[index(p) / 64] &= ~(uint64_t(1) << (index(p) % 64));
        }
    };

    // How the blast takes something:
    //   Structure   a building, a fence, a shed: flattened near, wrecked further, its windows blown out
    //               further still (Target::glass)
    //   Tree        shattered near, knocked flat further -- away from the blast, stripped bare nearer in --
    //               bent hard over further still, and left standing beyond
    enum class Role : uint8_t { Structure, Tree };

    // One thing the blast acts on: its entity, its box in the world, how its voxels sit in the world, and
    // what of it the blast needs to know. The shock front acts on it once, when it gets there (`struck`).
    // Several trees share one entity, their grove: each is a target of its own, with its own voxels.
    struct Target {
        Entity e = NullEntity;
        Role role = Role::Structure;
        vec3 lo{0.0f}, hi{0.0f};    // the world box: the footprint, and up to the roof (not the parapet)
        vec3 origin{0.0f};          // where its voxel (0, 0, 0)'s corner is
        quat turn = IDENTITY;       // its rotation (trees turn about y)
        float voxel = 0.5f;
        ivec3 voxels{0};            // its size in voxels
        ivec3 strikeAt{0};          // where a strike that takes all of it is made: a floor's middle, in voxels
        int storey = 0;             // voxels a storey (0: one storey, its whole height)
        float shard = 0.0f;         // what it breaks to across, metres (0: by its distance, flattenShard)
        float reach = 0.0f;         // a strike that takes all of it reaches this far, metres (0: its far corner)
        std::vector<ivec3> glass;   // structures: window glass, in its voxels
        // Trees: the foot of its trunk, all of its voxels as it was grown, and where its voxels are now (tipped
        // over by the wind, leanTick).
        ivec3 foot{0};
        std::vector<TreeVoxel> body;
        std::vector<ivec3> now;
        bool struck = false;

        vec3 worldOf(vec3 v) const { return origin + turn * ((v + 0.5f) * voxel); }
    };

    // ---- Maps --------------------------------------------------------------------------------------

    struct Demo;

    // A map: what it is called, how big its explosion is, how far out the blast does what, and how it is
    // built and seen. `scale` sizes the explosion against the city's (1): lengths by it, speeds by its square
    // root (as mushroom::Field scales), times not at all. The radii are the map's own, in metres.
    struct MapSpec {
        const char* name;           // --map
        const char* title;
        float scale;
        float floorHalf;            // the ground: 2 floorHalf square, 1 m voxels
        float earthHalf, earthDepth, earthVoxel;   // the block of earth under ground zero (the crater)
        float shockReach;           // how far the shock front runs before it stops, metres
        float flattenRadius, wreckRadius, glassRadius;   // structures (see shockSweep)
        float shatterRadius, stripRadius, fellRadius, leanRadius;   // trees
        float shardScale;           // flattenShard, by this
        float orbitDistance, orbitHeight, targetHeight;  // the orbiting camera, before the cloud rises
        uint32_t (*ground)(float x, float z, uint32_t h);   // the ground's colour at a point
        void (*build)(World& world, Demo& d);
    };

    // ---- State -------------------------------------------------------------------------------------

    struct Demo {
        const MapSpec* map = nullptr;
        float S = 1.0f;                // map->scale
        vec3 blast{0.0f, 1.6f, 0.0f}; // ground zero
        std::vector<night_renderer::Cube> cubes;   // this frame's ember cubes
        Entity floor = NullEntity, groundZero = NullEntity, earth = NullEntity;
        std::vector<Target> targets;
        std::vector<std::pair<size_t, float>> breaking;   // targets the front has reached, to break (and how far out)
        std::vector<Entity> owned;     // what the build made, swept up on a rebuild
        std::vector<TreeShape> trees;  // the shapes this map's trees are grown from
        Palette treePalette;           // their colours, and the materials they name
        std::map<Entity, Grove> groves;
        bool sceneEdited = false;      // a scenery grove was edited this step (leanTick)
        // Trees the wind is tipping over: which, its next gust and the step it is due, how far over it ends
        // (radians), which way (an axis and its sign), whether it goes down flat (and so falls in its gusts
        // rather than swaying), and whether its crown is stripped off.
        struct Lean { size_t target; int stage, due; float angle; int axis, sign; bool falls, strip; };
        std::vector<Lean> leaning;
        std::vector<Ember> cloud;
        std::vector<Kick> kicks;
        std::vector<std::pair<Entity, int>> newborn;   // shards still in their break's family, and for how many steps

        bool detonated = false;
        int boomStep = 0;              // fixed steps since the detonation
        float stepSeconds = 1.0f / 30.0f;   // Time::fixedDelta, set at startup
        float timeScale = 1.0f;             // EXPLOSION_TIMESCALE
        float emberScale = 1.5f;            // EXPLOSION_EMBERS: every ember count and rate, times this
        float clock = 0.0f, detonateAt = 4.0f;
        mushroom::Field field;
        mushroom::Grid<night_renderer::GRID> grid;
        std::vector<mushroom::Grid<night_renderer::GRID>::Splat> splats;
        std::vector<float> atlas;
        bool chunks = true;            // E: draw the ember cubes
        float baseExposure = 1.5f;
        // EXPLOSION_RECORD: frames a second of simulated time (0: not recording), from when, and frames saved.
        float recordFps = 0.0f, recordFrom = 0.0f;
        int recorded = 0;
        int pieces = 0;
        std::mt19937 random{20261018};

        // Unattended runs.
        bool unattended = false, fixedCamera = false;
        std::vector<float> shots;
        std::string capturePrefix = "explosion";
        float exitAt = -1.0f;
        bool stats = false;
        float statsTimer = 1.0f, titleTimer = 0.0f;
        int framesThisSecond = 0;
        uint32_t pieceBudget = 2500;   // the most pieces at once: past it the smallest go (cullPieces)
        bool settle = true;            // settleTick (EXPLOSION_SETTLE=0: off)
        double cloudMs = 0.0, presentMs = 0.0, fixedMs = 0.0, flushMs = 0.0, renderMs = 0.0, frameMs = 0.0;
        // The whole physics system (the step, fractures, splits, shape rebuilds), summed over a second, and
        // how many steps that was.
        Clock::time_point stepStart;
        double physicsSecondMs = 0.0, physicsWorstMs = 0.0, physicsLastMs = 0.0;
        int stepsThisSecond = 0;
    };

    inline float uniform(Demo& d, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(d.random); }
    inline vec3 inBall(Demo& d, float radius) {
        for (;;) {
            vec3 p(uniform(d, -1, 1), uniform(d, -1, 1), uniform(d, -1, 1));
            if (glm::dot(p, p) <= 1.0f) return p * radius;
        }
    }
    inline vec3 unitVector(Demo& d) {
        vec3 p = inBall(d, 1.0f);
        return glm::length(p) > 1e-3f ? glm::normalize(p) : vec3(0, 1, 0);
    }

    inline Entity own(Demo& d, Entity e) {
        d.owned.push_back(e);
        return e;
    }

    // A structure the blast acts on, its corner at `corner`, unturned: its target, with its windows.
    inline Target& addStructure(Demo& d, Entity e, vec3 corner, ivec3 size, float voxel, float roof, ivec3 strikeAt, int storey,
                                std::vector<ivec3> glass = {}) {
        Target t;
        t.e = e;
        t.role = Role::Structure;
        t.lo = corner;
        t.hi = corner + vec3(float(size.x) * voxel, roof, float(size.z) * voxel);
        t.origin = corner;
        t.voxel = voxel;
        t.voxels = size;
        t.strikeAt = strikeAt;
        t.storey = storey;
        t.glass = std::move(glass);
        d.targets.push_back(std::move(t));
        return d.targets.back();
    }
}
