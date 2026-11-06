// Life on the island: butterflies and bees over the flower fields, leaves falling from the trees, rabbits
// hopping in the meadows, gulls circling, clouds and dandelion fluff on the wind. Looks only:
// no physics, nothing saved. Each is a few voxels in a component of its own, moved every frame through
// its entity's Transform (the engine has no particles; a small moving component is how it draws one).
//
// A butterfly is an asset with a body and two wings, each wing an asset of its own turned about the
// body's axis to flap. It wanders round a home in a flower field, a new spot every second or two, at a
// height over the ground. A leaf drops from inside a crown near the player, sways and tumbles down, lies
// a moment on the ground, and goes back up into another tree.
#ifndef ISLAND_DEMO_CRITTERS_H
#define ISLAND_DEMO_CRITTERS_H

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "core/application.h"
#include "core/log.h"
#include "runtime/scene_bridge.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

#include "island.h"

namespace critters {
    using projv::Entity;
    using projv::World;
    using projv::core::quat;
    using projv::core::vec3;

    constexpr int   BUTTERFLIES = 36;
    constexpr int   LEAVES = 90;
    constexpr int   CLOUDS = 12;
    constexpr int   GULLS = 9;
    constexpr int   BEES = 24;
    constexpr int   RABBITS = 14;
    constexpr int   FLUFF = 50;
    constexpr float CLOUD_VOXEL = 0.5f;
    constexpr float GULL_VOXEL = 0.06f;
    constexpr float BEE_VOXEL = 0.035f;
    constexpr float RABBIT_VOXEL = 0.045f;
    constexpr float FLUFF_VOXEL = 0.03f;
    constexpr float WIND_X = 0.9f, WIND_Z = 0.25f;   // m/s: what the clouds and the fluff drift on
    constexpr float WING_VOXEL = 0.035f;   // a wing is 6 x 8 of these: a 40 cm span, whimsically big
    constexpr float LEAF_VOXEL = 0.06f;
    const vec3 HIDDEN(island::CENTRE, -40.0f, island::CENTRE);   // under the sea floor, out of sight

    // A small random number source, so nothing here touches the generator's hashes.
    struct Random {
        uint32_t state;
        float next() { state = island::hash(state + 0x9e3779b9u); return island::unit(state); }
        float range(float a, float b) { return a + (b - a) * next(); }
    };

    struct Butterfly {
        Entity body = projv::NullEntity, left = projv::NullEntity, right = projv::NullEntity;
        vec3   home{0.0f}, position{0.0f}, velocity{0.0f}, target{0.0f};
        float  retarget = 0.0f;   // seconds to the next spot
        float  phase = 0.0f, rate = 14.0f;
        float  yaw = 0.0f;
    };
    struct Leaf {
        Entity entity = projv::NullEntity;
        vec3   position{0.0f}, drift{0.0f};
        float  ground = 0.0f, wait = 0.0f, fall = 0.7f, phase = 0.0f, sway = 0.6f, spin = 1.0f, time = 0.0f;
        bool   falling = false;
    };
    struct Cloud { Entity entity = projv::NullEntity; vec3 position{0.0f}; float speed = 1.0f; };
    struct Gull {
        Entity body = projv::NullEntity, left = projv::NullEntity, right = projv::NullEntity;
        vec3   centre{0.0f};
        float  radius = 20.0f, angle = 0.0f, speed = 6.0f, phase = 0.0f, height = 30.0f;
    };
    struct Bee {
        Entity entity = projv::NullEntity;
        vec3   home{0.0f}, position{0.0f}, velocity{0.0f}, target{0.0f};
        float  retarget = 0.0f, phase = 0.0f, yaw = 0.0f;
    };
    struct Rabbit {
        Entity entity = projv::NullEntity;
        vec3   home{0.0f}, from{0.0f}, to{0.0f};
        float  yaw = 0.0f, wait = 0.0f, hop = -1.0f;   // hop: 0..1 through a hop, < 0 sitting
    };
    struct Fluff { Entity entity = projv::NullEntity; vec3 position{0.0f}; float life = 0.0f, phase = 0.0f; };

    struct Critters {
        std::vector<Butterfly> butterflies;
        std::vector<Leaf>      leaves;
        std::vector<Cloud>     clouds;
        std::vector<Gull>      gulls;
        std::vector<Bee>       bees;
        std::vector<Rabbit>    rabbits;
        std::vector<Fluff>     fluff;
        std::vector<island::Tree> trees;
        uint32_t seed = 0;
        Random   random{1};
        float    time = 0.0f;
    };

    // ---- Making them --------------------------------------------------------------------------------

    inline uint32_t rgb(int r, int g, int b) { return projv::packRGB10(float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f); }

    struct WingColours { uint32_t main, edge, spot; };
    inline const WingColours WINGS[] = {
        {rgb(240, 130, 30), rgb(30, 20, 20), rgb(250, 250, 240)},     // orange, black edge
        {rgb(70, 140, 250), rgb(20, 30, 80), rgb(200, 240, 255)},     // blue
        {rgb(250, 225, 60), rgb(90, 70, 20), rgb(255, 160, 40)},      // yellow
        {rgb(250, 250, 245), rgb(150, 150, 160), rgb(40, 40, 40)},    // white
        {rgb(250, 120, 190), rgb(120, 30, 90), rgb(255, 230, 120)},   // pink
        {rgb(150, 80, 230), rgb(40, 20, 70), rgb(120, 230, 230)},     // purple
        {rgb(110, 220, 120), rgb(20, 80, 40), rgb(250, 250, 200)},    // green
    };

    // One wing's voxels, mirrored for the left: a big forewing and a smaller hindwing, an edge, a spot.
    inline std::vector<projv::PendingVoxelOp> wingVoxels(const WingColours& c, bool left) {
        std::vector<projv::PendingVoxelOp> out;
        for (int z = 0; z < 8; z++)
            for (int x = 0; x < 6; x++) {
                const float px = float(x) + 0.5f, pz = float(z) + 0.5f;
                auto lobe = [&](float cx, float cz, float rx, float rz) {
                    return ((px - cx) * (px - cx)) / (rx * rx) + ((pz - cz) * (pz - cz)) / (rz * rz);
                };
                const float fore = lobe(2.4f, 5.4f, 3.2f, 2.7f), hind = lobe(1.6f, 2.0f, 2.2f, 2.1f);
                const float e = std::min(fore, hind);
                if (e > 1.0f) continue;
                uint32_t colour = e > 0.55f ? c.edge : c.main;
                if (lobe(3.2f, 5.6f, 0.9f, 0.9f) < 1.0f) colour = c.spot;
                out.push_back({true, projv::core::ivec3(left ? 5 - x : x, 0, z), colour});
            }
        return out;
    }

    inline projv::ComponentHandle chunk(projv::Scene& scene, const std::string& name, projv::ComponentHandle parent, float voxel,
                                        vec3 offset, const std::vector<projv::PendingVoxelOp>& voxels, uint32_t resolution = 16) {
        const projv::ComponentHandle h = projv::utils::addComponent(scene, projv::ComponentKind::Chunk, name, parent, resolution, voxel);
        projv::utils::setComponentTransform(scene, h, offset, quat(1, 0, 0, 0), 1.0f);
        projv::utils::queueVoxelAdd(scene, h, voxels);
        return h;
    }
    inline projv::ComponentHandle asset(projv::Scene& scene, const std::string& name, projv::ComponentHandle parent) {
        const projv::ComponentHandle h = projv::utils::addComponent(scene, projv::ComponentKind::Asset, name, parent, 4, 1.0f);
        projv::utils::setComponentTransform(scene, h, vec3(0.0f), quat(1, 0, 0, 0), 1.0f);
        return h;
    }

    // A cloud: a few overlapping blobs, flat underneath, white on top and greyer below.
    inline std::vector<projv::PendingVoxelOp> cloudVoxels(Random& random) {
        std::vector<projv::PendingVoxelOp> out;
        struct Blob { vec3 c; vec3 r; };
        std::vector<Blob> blobs;
        const int count = 4 + int(random.next() * 4.0f);
        const float length = random.range(14.0f, 26.0f);
        for (int i = 0; i < count; i++) {
            const float t = (float(i) + 0.5f) / float(count);
            const float r = random.range(3.0f, 6.0f) * (1.0f - 0.5f * std::abs(t - 0.5f));
            blobs.push_back({vec3(4.0f + t * length, 3.0f + random.range(0.0f, 2.0f), 14.0f + random.range(-4.0f, 4.0f)), vec3(r * 1.2f, r * 0.75f, r)});
        }
        const uint32_t top = rgb(250, 250, 252), mid = rgb(232, 236, 244), under = rgb(205, 212, 226);
        for (int z = 0; z < 64; z++)
            for (int y = 0; y < 24; y++)
                for (int x = 0; x < 64; x++) {
                    const vec3 p = (vec3(float(x), float(y), float(z)) + vec3(0.5f)) * CLOUD_VOXEL;
                    if (p.y < 2.5f) continue;   // flat underneath
                    bool inside = false;
                    for (const Blob& b : blobs) {
                        const vec3 d = (p - b.c) / b.r;
                        if (glm::dot(d, d) < 1.0f) { inside = true; break; }
                    }
                    if (!inside) continue;
                    out.push_back({true, projv::core::ivec3(x, y, z), p.y < 3.3f ? under : p.y < 5.0f ? mid : top});
                }
        return out;
    }

    // A gull's wing: long and narrow, grey, the tip black.
    inline std::vector<projv::PendingVoxelOp> gullWing(bool left) {
        std::vector<projv::PendingVoxelOp> out;
        for (int x = 0; x < 11; x++)
            for (int z = 0; z < 4; z++) {
                const float chord = 3.6f - float(x) * 0.22f;   // narrower toward the tip
                if (float(z) > chord) continue;
                const uint32_t c = x > 8 ? rgb(30, 30, 35) : z == 0 ? rgb(200, 205, 212) : rgb(170, 176, 186);
                out.push_back({true, projv::core::ivec3(left ? 10 - x : x, 0, z), c});
            }
        return out;
    }

    // Homes in the flower fields: half near the start, so some are seen at once, half anywhere.
    inline std::vector<vec3> homes(uint32_t seed, vec3 start, int count, Random& random) {
        std::vector<vec3> out;
        for (int tries = 0; tries < 6000 && int(out.size()) < count; tries++) {
            const bool closeIn = int(out.size()) < count / 2;
            const float a = random.range(0.0f, 6.2831853f), r = closeIn ? random.range(3.0f, 28.0f) : random.range(0.0f, 70.0f);
            const vec3 c = closeIn ? start : vec3(island::CENTRE, 0.0f, island::CENTRE);
            const float x = c.x + std::cos(a) * r, z = c.z + std::sin(a) * r;
            if (island::flowerField(x, z, seed) < (tries < 4000 ? 0.35f : 0.0f)) continue;
            const float ground = island::surfaceAt(x, z, seed);
            if (ground < island::SEA + 1.5f) continue;
            out.push_back(vec3(x, ground, z));
        }
        return out;
    }

    // Adds the critters' components to the scene and links them. Call after the scene bridge is in and
    // before the scene goes to the GPU; it applies the voxels (updateScene).
    inline void install(World& world, uint32_t seed, vec3 start) {
        auto& scene = world.ctx().get<projv::Scene>();
        Critters& c = world.ctx().emplace<Critters>();
        c.seed = seed;
        c.random = Random{seed * 7919u + 13u};
        c.trees = island::treesNear(vec3(0.0f), vec3(island::SIZE, island::HEIGHT, island::SIZE), seed);

        const std::vector<projv::PendingVoxelOp> bodyVoxels = [] {
            std::vector<projv::PendingVoxelOp> v;
            for (int z = 0; z < 6; z++) v.push_back({true, projv::core::ivec3(0, 0, z), rgb(35, 25, 25)});
            v.push_back({true, projv::core::ivec3(0, 1, 5), rgb(35, 25, 25)});   // a head, up a little
            return v;
        }();
        struct Made { projv::ComponentHandle body, left, right; };
        std::vector<Made> made;
        const std::vector<vec3> where = homes(seed, start, BUTTERFLIES, c.random);
        for (size_t i = 0; i < where.size(); i++) {
            const std::string name = "butterfly " + std::to_string(i);
            const WingColours& colours = WINGS[size_t(c.random.next() * float(std::size(WINGS))) % std::size(WINGS)];
            Made m;
            m.body = asset(scene, name, projv::INVALID_COMPONENT_HANDLE);
            chunk(scene, name + " body", m.body, WING_VOXEL, vec3(-0.5f, -0.5f, -3.0f) * WING_VOXEL, bodyVoxels);
            m.right = asset(scene, name + " right", m.body);
            chunk(scene, name + " right wing", m.right, WING_VOXEL, vec3(0.5f, 0.0f, -4.0f) * WING_VOXEL, wingVoxels(colours, false));
            m.left = asset(scene, name + " left", m.body);
            chunk(scene, name + " left wing", m.left, WING_VOXEL, vec3(-6.5f, 0.0f, -4.0f) * WING_VOXEL, wingVoxels(colours, true));
            made.push_back(m);

            Butterfly b;
            b.home = where[i];
            b.position = b.target = where[i] + vec3(0.0f, c.random.range(0.6f, 1.6f), 0.0f);
            b.phase = c.random.range(0.0f, 6.28f);
            b.rate = c.random.range(11.0f, 17.0f);
            b.retarget = c.random.range(0.0f, 1.5f);
            c.butterflies.push_back(b);
        }

        const uint32_t leafColours[] = {rgb(60, 130, 45), rgb(80, 150, 50), rgb(170, 150, 50), rgb(190, 110, 40)};
        std::vector<projv::ComponentHandle> leafChunks;
        for (int i = 0; i < LEAVES; i++) {
            const uint32_t colour = leafColours[i % 4];
            std::vector<projv::PendingVoxelOp> v;
            for (int z = 0; z < 3; z++)
                for (int x = 0; x < 2; x++)
                    if (!(z == 2 && x == 1)) v.push_back({true, projv::core::ivec3(x, 0, z), colour});
            leafChunks.push_back(chunk(scene, "leaf " + std::to_string(i), projv::INVALID_COMPONENT_HANDLE, LEAF_VOXEL, HIDDEN, v));
            Leaf leaf;
            leaf.wait = c.random.range(0.0f, 8.0f);   // staggered, so they do not all start together
            leaf.position = HIDDEN;
            c.leaves.push_back(leaf);
        }

        // Clouds, high over the island, spread across it and upwind.
        std::vector<projv::ComponentHandle> cloudChunks;
        for (int i = 0; i < CLOUDS; i++) {
            Cloud cloud;
            cloud.position = vec3(c.random.range(-80.0f, island::SIZE + 40.0f), c.random.range(60.0f, 74.0f), c.random.range(-40.0f, island::SIZE));
            cloud.speed = c.random.range(0.7f, 1.3f);
            cloudChunks.push_back(chunk(scene, "cloud " + std::to_string(i), projv::INVALID_COMPONENT_HANDLE, CLOUD_VOXEL, cloud.position,
                                        cloudVoxels(c.random), 64));
            c.clouds.push_back(cloud);
        }

        // Gulls, circling over the island and its shore.
        struct Bird { projv::ComponentHandle body, left, right; };
        std::vector<Bird> birds;
        for (int i = 0; i < GULLS; i++) {
            const std::string name = "gull " + std::to_string(i);
            std::vector<projv::PendingVoxelOp> body;
            for (int z = 0; z < 7; z++)
                for (int y = 0; y < 2; y++)
                    for (int x = 0; x < 2; x++) body.push_back({true, projv::core::ivec3(x, y, z), z < 2 ? rgb(150, 155, 165) : rgb(245, 245, 245)});
            body.push_back({true, projv::core::ivec3(0, 1, 7), rgb(245, 245, 245)});
            body.push_back({true, projv::core::ivec3(1, 1, 7), rgb(245, 245, 245)});
            body.push_back({true, projv::core::ivec3(0, 1, 8), rgb(240, 170, 40)});   // the beak
            Bird bird;
            bird.body = asset(scene, name, projv::INVALID_COMPONENT_HANDLE);
            chunk(scene, name + " body", bird.body, GULL_VOXEL, vec3(-1.0f, -1.0f, -4.0f) * GULL_VOXEL, body);
            bird.right = asset(scene, name + " right", bird.body);
            chunk(scene, name + " right wing", bird.right, GULL_VOXEL, vec3(1.0f, 0.0f, -1.0f) * GULL_VOXEL, gullWing(false));
            bird.left = asset(scene, name + " left", bird.body);
            chunk(scene, name + " left wing", bird.left, GULL_VOXEL, vec3(-12.0f, 0.0f, -1.0f) * GULL_VOXEL, gullWing(true));
            birds.push_back(bird);
            Gull g;
            const float a = c.random.range(0.0f, 6.2831853f), r = c.random.range(20.0f, 70.0f);
            g.centre = vec3(island::CENTRE + std::cos(a) * r, 0.0f, island::CENTRE + std::sin(a) * r);
            g.radius = c.random.range(10.0f, 26.0f);
            g.speed = c.random.range(5.0f, 8.0f) * (c.random.next() < 0.5f ? -1.0f : 1.0f);
            g.angle = c.random.range(0.0f, 6.2831853f);
            g.phase = c.random.range(0.0f, 6.28f);
            g.height = c.random.range(26.0f, 44.0f);
            c.gulls.push_back(g);
        }

        // Bees, about the flowers.
        std::vector<projv::ComponentHandle> beeChunks;
        const std::vector<vec3> beeHomes = homes(seed, start, BEES, c.random);
        for (size_t i = 0; i < beeHomes.size(); i++) {
            std::vector<projv::PendingVoxelOp> v;
            for (int z = 0; z < 4; z++)
                for (int y = 0; y < 2; y++)
                    for (int x = 0; x < 2; x++) v.push_back({true, projv::core::ivec3(x, y, z), z % 2 ? rgb(30, 25, 20) : rgb(250, 200, 30)});
            v.push_back({true, projv::core::ivec3(0, 2, 2), rgb(235, 245, 255)});   // wings: a little white on top
            v.push_back({true, projv::core::ivec3(1, 2, 2), rgb(235, 245, 255)});
            beeChunks.push_back(chunk(scene, "bee " + std::to_string(i), projv::INVALID_COMPONENT_HANDLE, BEE_VOXEL, HIDDEN, v));
            Bee b;
            b.home = beeHomes[i];
            b.position = b.target = beeHomes[i] + vec3(0.0f, 0.5f, 0.0f);
            b.phase = c.random.range(0.0f, 6.28f);
            c.bees.push_back(b);
        }

        // Rabbits, in the meadows.
        std::vector<projv::ComponentHandle> rabbitAssets;
        const std::vector<vec3> rabbitHomes = homes(seed, start, RABBITS, c.random);
        const uint32_t furs[][2] = {{rgb(240, 238, 232), rgb(250, 190, 190)}, {rgb(150, 105, 70), rgb(230, 200, 170)},
                                    {rgb(130, 128, 125), rgb(220, 215, 205)}, {rgb(200, 160, 110), rgb(245, 230, 210)}};
        for (size_t i = 0; i < rabbitHomes.size(); i++) {
            const uint32_t* fur = furs[i % 4];
            std::vector<projv::PendingVoxelOp> v;
            auto ball = [&](vec3 centre, vec3 radius, uint32_t colour) {
                for (int z = 0; z < 16; z++)
                    for (int y = 0; y < 16; y++)
                        for (int x = 0; x < 16; x++) {
                            const vec3 d = (vec3(float(x), float(y), float(z)) + vec3(0.5f) - centre) / radius;
                            if (glm::dot(d, d) < 1.0f) v.push_back({true, projv::core::ivec3(x, y, z), colour});
                        }
            };
            ball(vec3(4.0f, 3.0f, 5.0f), vec3(3.0f, 2.8f, 4.0f), fur[0]);     // body
            ball(vec3(4.0f, 5.5f, 9.0f), vec3(2.2f, 2.2f, 2.2f), fur[0]);     // head
            ball(vec3(4.0f, 3.5f, 1.0f), vec3(1.3f, 1.3f, 1.3f), rgb(250, 250, 250));   // tail
            for (int y = 7; y < 11; y++) {                                    // ears, pink inside
                v.push_back({true, projv::core::ivec3(3, y, 9), y > 7 ? fur[1] : fur[0]});
                v.push_back({true, projv::core::ivec3(5, y, 9), y > 7 ? fur[1] : fur[0]});
            }
            v.push_back({true, projv::core::ivec3(2, 6, 10), rgb(20, 20, 20)});   // eyes
            v.push_back({true, projv::core::ivec3(6, 6, 10), rgb(20, 20, 20)});
            const std::string name = "rabbit " + std::to_string(i);
            const projv::ComponentHandle a = asset(scene, name, projv::INVALID_COMPONENT_HANDLE);
            chunk(scene, name + " body", a, RABBIT_VOXEL, vec3(-4.0f, 0.0f, -5.0f) * RABBIT_VOXEL, v);
            rabbitAssets.push_back(a);
            Rabbit r;
            r.home = r.from = r.to = rabbitHomes[i];
            r.yaw = c.random.range(0.0f, 6.28f);
            r.wait = c.random.range(0.0f, 2.0f);
            c.rabbits.push_back(r);
        }

        // Dandelion fluff: a white tuft on a stalk, drifting on the wind near the player.
        std::vector<projv::ComponentHandle> fluffChunks;
        for (int i = 0; i < FLUFF; i++) {
            std::vector<projv::PendingVoxelOp> v;
            const uint32_t white = rgb(250, 250, 245), pale = rgb(245, 240, 200);
            for (int k = 0; k < 3; k++) v.push_back({true, projv::core::ivec3(1, k, 1), k == 0 ? pale : white});
            v.push_back({true, projv::core::ivec3(0, 2, 1), white});
            v.push_back({true, projv::core::ivec3(2, 2, 1), white});
            v.push_back({true, projv::core::ivec3(1, 2, 0), white});
            v.push_back({true, projv::core::ivec3(1, 2, 2), white});
            fluffChunks.push_back(chunk(scene, "fluff " + std::to_string(i), projv::INVALID_COMPONENT_HANDLE, FLUFF_VOXEL, HIDDEN, v));
            Fluff f;
            f.life = -c.random.range(0.0f, 6.0f);   // staggered starts
            f.phase = c.random.range(0.0f, 6.28f);
            f.position = HIDDEN;
            c.fluff.push_back(f);
        }

        projv::utils::updateScene(scene);
        for (size_t i = 0; i < cloudChunks.size(); i++) c.clouds[i].entity = projv::runtime::spawnComponent(world, cloudChunks[i]);
        for (size_t i = 0; i < birds.size(); i++) {
            Gull& g = c.gulls[i];
            g.body = projv::runtime::spawnComponent(world, birds[i].body);
            g.left = projv::runtime::spawnComponent(world, birds[i].left, projv::LinkMode::Part);
            g.right = projv::runtime::spawnComponent(world, birds[i].right, projv::LinkMode::Part);
        }
        for (size_t i = 0; i < beeChunks.size(); i++) c.bees[i].entity = projv::runtime::spawnComponent(world, beeChunks[i]);
        for (size_t i = 0; i < rabbitAssets.size(); i++) c.rabbits[i].entity = projv::runtime::spawnComponent(world, rabbitAssets[i]);
        for (size_t i = 0; i < fluffChunks.size(); i++) c.fluff[i].entity = projv::runtime::spawnComponent(world, fluffChunks[i]);
        for (size_t i = 0; i < made.size(); i++) {
            Butterfly& b = c.butterflies[i];
            b.body = projv::runtime::spawnComponent(world, made[i].body);
            b.left = projv::runtime::spawnComponent(world, made[i].left, projv::LinkMode::Part);
            b.right = projv::runtime::spawnComponent(world, made[i].right, projv::LinkMode::Part);
        }
        for (size_t i = 0; i < leafChunks.size(); i++) c.leaves[i].entity = projv::runtime::spawnComponent(world, leafChunks[i]);
        if (!c.butterflies.empty())
            projv::core::info("island: {} butterflies (the first at {:.1f}, {:.1f}, {:.1f}), {} leaves", c.butterflies.size(),
                              c.butterflies[0].home.x, c.butterflies[0].home.y, c.butterflies[0].home.z, c.leaves.size());
    }

    // ---- Moving them ---------------------------------------------------------------------------------

    inline void place(World& world, Entity e, vec3 position, quat rotation) {
        if (!world.valid(e) || !world.all_of<projv::Transform>(e)) return;
        world.patch<projv::Transform>(e, [&](projv::Transform& t) {
            t.position = position;
            t.rotation = rotation;
        });
    }

    // A leaf starts again inside a crown near `around` (any tree if none is near).
    inline void respawn(Critters& c, Leaf& leaf, vec3 around) {
        const island::Tree* pick = nullptr;
        for (int tries = 0; tries < 24 && !pick; tries++) {
            const island::Tree& t = c.trees[size_t(c.random.next() * float(c.trees.size())) % c.trees.size()];
            const float dx = t.base.x - around.x, dz = t.base.z - around.z;
            if (dx * dx + dz * dz < 35.0f * 35.0f || tries == 23) pick = &t;
        }
        const island::Tree& t = *pick;
        const float a = c.random.range(0.0f, 6.2831853f), r = c.random.range(0.2f, 0.85f) * t.crown;
        leaf.position = t.crownCentre + vec3(std::cos(a) * r, c.random.range(-0.5f, 0.2f) * t.crown, std::sin(a) * r);
        leaf.ground = island::surfaceAt(leaf.position.x, leaf.position.z, c.seed);
        leaf.fall = c.random.range(0.45f, 0.85f);
        leaf.sway = c.random.range(0.3f, 0.8f);
        leaf.spin = c.random.range(1.2f, 2.4f);
        leaf.phase = c.random.range(0.0f, 6.28f);
        leaf.drift = vec3(c.random.range(0.1f, 0.4f), 0.0f, c.random.range(-0.15f, 0.2f));   // a light breeze
        leaf.time = 0.0f;
        leaf.falling = true;
    }

    // Each frame: `eye` is where the player looks from (leaves fall near it).
    inline void update(World& world, float dt, vec3 eye) {
        Critters* found = world.ctx().find<Critters>();
        if (!found) return;
        Critters& c = *found;
        dt = std::min(dt, 0.1f);
        c.time += dt;

        for (Butterfly& b : c.butterflies) {
            b.retarget -= dt;
            if (b.retarget <= 0.0f) {
                const float a = c.random.range(0.0f, 6.2831853f), r = c.random.range(0.5f, 4.5f);
                const float x = b.home.x + std::cos(a) * r, z = b.home.z + std::sin(a) * r;
                b.target = vec3(x, island::surfaceAt(x, z, c.seed) + c.random.range(0.35f, 2.0f), z);
                b.retarget = c.random.range(0.8f, 2.5f);
            }
            // Steer for the spot, with a flutter: a bob up and down and a wobble side to side.
            vec3 want = b.target - b.position;
            const float distance = glm::length(want);
            want = distance > 0.05f ? want / distance * std::min(1.8f, distance * 1.5f) : vec3(0.0f);
            want += vec3(0.4f * std::sin(c.time * 2.3f + b.phase), 0.7f * std::sin(c.time * 4.1f + b.phase * 2.0f), 0.4f * std::cos(c.time * 1.9f + b.phase));
            b.velocity += (want - b.velocity) * (1.0f - std::exp(-3.0f * dt));
            b.position += b.velocity * dt;
            const float flat = std::sqrt(b.velocity.x * b.velocity.x + b.velocity.z * b.velocity.z);
            if (flat > 0.05f) {
                const float yaw = std::atan2(b.velocity.x, b.velocity.z);
                float delta = yaw - b.yaw;
                while (delta > 3.14159265f) delta -= 6.2831853f;
                while (delta < -3.14159265f) delta += 6.2831853f;
                b.yaw += delta * (1.0f - std::exp(-6.0f * dt));
            }
            const quat facing = glm::angleAxis(b.yaw, vec3(0, 1, 0)) * glm::angleAxis(-0.25f, vec3(1, 0, 0));
            // Flap: fast beats, now and then a short glide with the wings held up.
            const float glide = std::sin(c.time * 0.7f + b.phase * 3.0f) > 0.8f ? 1.0f : 0.0f;
            const float flap = glide > 0.0f ? 0.5f : 0.35f + 0.95f * std::sin(c.time * b.rate + b.phase);
            place(world, b.body, b.position, facing);
            place(world, b.right, vec3(0.0f), glm::angleAxis(flap, vec3(0, 0, 1)));
            place(world, b.left, vec3(0.0f), glm::angleAxis(-flap, vec3(0, 0, 1)));
        }

        // Clouds drift downwind and come round again from upwind.
        for (Cloud& cloud : c.clouds) {
            cloud.position += vec3(WIND_X, 0.0f, WIND_Z) * cloud.speed * dt;
            if (cloud.position.x > island::SIZE + 60.0f) cloud.position.x = -100.0f;
            if (cloud.position.z > island::SIZE + 40.0f) cloud.position.z = -60.0f;
            place(world, cloud.entity, cloud.position, quat(1, 0, 0, 0));
        }

        // Gulls circle, banked into the turn, flapping a while and then gliding.
        for (Gull& g : c.gulls) {
            g.angle += g.speed / g.radius * dt;
            const float wobble = 3.0f * std::sin(c.time * 0.3f + g.phase);
            const vec3 p = g.centre + vec3(std::cos(g.angle) * (g.radius + wobble), g.height + 2.0f * std::sin(c.time * 0.5f + g.phase),
                                           std::sin(g.angle) * (g.radius + wobble));
            const float dir = g.speed > 0.0f ? 1.0f : -1.0f;
            const vec3 tangent(-std::sin(g.angle) * dir, 0.0f, std::cos(g.angle) * dir);
            const float yaw = std::atan2(tangent.x, tangent.z);
            const quat facing = glm::angleAxis(yaw, vec3(0, 1, 0)) * glm::angleAxis(-0.35f * dir, vec3(0, 0, 1));
            const bool flapping = std::sin(c.time * 0.45f + g.phase) > 0.2f;
            const float flap = flapping ? 0.1f + 0.55f * std::sin(c.time * 5.5f + g.phase) : 0.12f;
            place(world, g.body, p, facing);
            place(world, g.right, vec3(0.0f), glm::angleAxis(flap, vec3(0, 0, 1)));
            place(world, g.left, vec3(0.0f), glm::angleAxis(-flap, vec3(0, 0, 1)));
        }

        // Bees dart from flower to flower, hovering with a fast jitter.
        for (Bee& b : c.bees) {
            b.retarget -= dt;
            if (b.retarget <= 0.0f) {
                const float a = c.random.range(0.0f, 6.2831853f), r = c.random.range(0.3f, 3.0f);
                const float x = b.home.x + std::cos(a) * r, z = b.home.z + std::sin(a) * r;
                b.target = vec3(x, island::surfaceAt(x, z, c.seed) + c.random.range(0.35f, 0.9f), z);
                b.retarget = c.random.range(0.5f, 1.8f);
            }
            vec3 want = b.target - b.position;
            const float distance = glm::length(want);
            want = distance > 0.03f ? want / distance * std::min(2.6f, distance * 3.0f) : vec3(0.0f);
            want += 0.35f * vec3(std::sin(c.time * 23.0f + b.phase), std::sin(c.time * 31.0f + b.phase * 1.7f), std::cos(c.time * 27.0f + b.phase));
            b.velocity += (want - b.velocity) * (1.0f - std::exp(-6.0f * dt));
            b.position += b.velocity * dt;
            const float flat = std::sqrt(b.velocity.x * b.velocity.x + b.velocity.z * b.velocity.z);
            if (flat > 0.2f) b.yaw = std::atan2(b.velocity.x, b.velocity.z);
            place(world, b.entity, b.position - vec3(0.0f, 0.0f, 0.0f), glm::angleAxis(b.yaw, vec3(0, 1, 0)));
        }

        // Rabbits sit, look about, then hop: a short arc to a spot nearby, a few in a row.
        for (Rabbit& r : c.rabbits) {
            if (r.hop < 0.0f) {
                r.wait -= dt;
                if (r.wait <= 0.0f) {
                    // Mostly onward, sometimes a turn; drawn back home when it strays.
                    float heading = r.yaw + c.random.range(-0.9f, 0.9f);
                    const vec3 away = r.from - r.home;
                    if (glm::length(away) > 7.0f) heading = std::atan2(-away.x, -away.z) + c.random.range(-0.4f, 0.4f);
                    const float step = c.random.range(0.5f, 1.2f);
                    const float x = r.from.x + std::sin(heading) * step, z = r.from.z + std::cos(heading) * step;
                    const float ground = island::surfaceAt(x, z, c.seed);
                    if (std::abs(ground - r.from.y) < 0.8f) {
                        r.to = vec3(x, ground, z);
                        r.yaw = heading;
                        r.hop = 0.0f;
                    } else {
                        r.yaw += 2.0f;   // a wall or a drop: turn away
                        r.wait = 0.3f;
                    }
                }
            }
            vec3 p = r.from;
            float pitch = 0.0f;
            if (r.hop >= 0.0f) {
                r.hop += dt / 0.32f;
                const float t = std::min(r.hop, 1.0f);
                p = r.from + (r.to - r.from) * t + vec3(0.0f, 0.28f * std::sin(t * 3.14159265f), 0.0f);
                pitch = -0.35f * std::cos(t * 3.14159265f);
                if (r.hop >= 1.0f) {
                    r.from = r.to;
                    r.hop = -1.0f;
                    r.wait = c.random.next() < 0.6f ? c.random.range(0.05f, 0.25f) : c.random.range(1.0f, 4.0f);   // a run of hops, or a sit
                    p = r.from;
                    pitch = 0.0f;
                }
            }
            place(world, r.entity, p, glm::angleAxis(r.yaw, vec3(0, 1, 0)) * glm::angleAxis(pitch, vec3(1, 0, 0)));
        }

        // Fluff drifts on the wind near the player, rising and falling a little, then is gone.
        for (Fluff& f : c.fluff) {
            f.life += dt;
            if (f.life < 0.0f) continue;
            if (f.life > 12.0f || f.position == HIDDEN) {
                const float a = c.random.range(0.0f, 6.2831853f), r = c.random.range(2.0f, 14.0f);
                const float x = eye.x + std::cos(a) * r - WIND_X * 5.0f, z = eye.z + std::sin(a) * r - WIND_Z * 5.0f;
                f.position = vec3(x, island::surfaceAt(x, z, c.seed) + c.random.range(0.4f, 2.4f), z);
                f.life = c.random.range(0.0f, 4.0f);
            }
            f.position += vec3(WIND_X * 0.6f + 0.25f * std::sin(c.time * 0.9f + f.phase), 0.12f * std::sin(c.time * 0.7f + f.phase * 2.0f),
                               WIND_Z * 0.6f + 0.25f * std::cos(c.time * 0.8f + f.phase)) * dt;
            place(world, f.entity, f.position, glm::angleAxis(c.time * 0.8f + f.phase, vec3(0, 1, 0)));
        }

        for (Leaf& leaf : c.leaves) {
            if (!leaf.falling) {
                leaf.wait -= dt;
                if (leaf.wait <= 0.0f) respawn(c, leaf, eye);
                else continue;
            }
            leaf.time += dt;
            if (leaf.position.y > leaf.ground + 0.03f) {
                // Down slowly, swinging side to side like a pendulum, tumbling a little, drifting downwind.
                const float swing = std::sin(leaf.time * leaf.spin + leaf.phase);
                leaf.position += (leaf.drift + vec3(swing * leaf.sway, -leaf.fall * (0.75f + 0.25f * std::abs(swing)), 0.4f * std::cos(leaf.time * leaf.spin * 0.7f + leaf.phase) * leaf.sway)) * dt;
                const quat tumble = glm::angleAxis(leaf.time * 0.6f + leaf.phase, vec3(0, 1, 0)) * glm::angleAxis(swing * 0.7f, vec3(0, 0, 1)) *
                                    glm::angleAxis(0.3f * std::sin(leaf.time * 1.3f), vec3(1, 0, 0));
                place(world, leaf.entity, leaf.position, tumble);
            } else if (leaf.time > 0.0f) {
                // Landed: lie a moment, then go.
                leaf.position.y = leaf.ground + 0.01f;
                place(world, leaf.entity, leaf.position, glm::angleAxis(leaf.phase, vec3(0, 1, 0)));
                leaf.time = -c.random.range(2.0f, 5.0f);
            }
            if (leaf.time < 0.0f && leaf.time + dt >= 0.0f) {
                place(world, leaf.entity, HIDDEN, quat(1, 0, 0, 0));
                leaf.falling = false;
                leaf.wait = c.random.range(0.0f, 3.0f);
            }
        }
    }
}

#endif
