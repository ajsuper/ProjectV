// The three maps, each built in code voxel by voxel, each colour naming the physics material it is made of:
//
//   city     a five-by-five grid of blocks round a central tower, ground zero: towers, apartments and
//            shops of concrete, brick, plaster and glass, cars at the kerbs, street lamps
//   house    a suburban street: a brick house at ground zero with its garden -- fence, shed, car on the
//            drive, trees -- its neighbours either side and across the road. The explosion at half size
//   forest   a pine forest round a clearing, a cabin by a track: Tunguska. Near trees are shattered,
//            further ones knocked flat away from the blast (stripped bare nearer in), further still bent
//            hard over by the wind, and the furthest left standing
//
// What the blast does to each thing is main.cpp's (shockSweep): a map says only what is where, and what
// each thing is (a Target: a structure or a tree).
#pragma once

#include "explosion.h"

namespace explosion {
    // ============================================================================================
    // The city
    // ============================================================================================

    constexpr float CITY_VOXEL = 0.5f;
    constexpr int CITY_BLOCKS = 5;                        // a side
    constexpr float PITCH = 56.0f, BLOCK = 42.0f;         // block to block, and a block's own width (streets 14 m)
    constexpr float CITY_HALF = 0.5f * float(CITY_BLOCKS) * PITCH;
    constexpr int STOREY = 7;                             // voxels a storey: 3.5 m

    enum class Style : uint8_t { Tower, Apartments, Shops };

    // One building: a footprint of `size` voxels, its corner at `corner`, in a style. `seed` varies it.
    // Every building is one static grid of half-metre voxels -- a concrete frame and floor slabs, a facade
    // of glass curtain wall, brick or plaster, windows lit at random -- standing on its own y = 0.
    inline Target& buildBuilding(World& world, Demo& d, vec3 corner, ivec3 size, Style style, uint32_t seed) {
        Palette p;
        const float tone = 0.85f + 0.3f * float(seed % 7) / 6.0f;
        const uint32_t concrete[2] = {p.paint(vec3(0.46f, 0.45f, 0.43f) * tone, "city concrete"),
                                      p.paint(vec3(0.41f, 0.4f, 0.39f) * tone, "city concrete")};
        static const vec3 brickTints[4] = {{0.45f, 0.18f, 0.13f}, {0.52f, 0.3f, 0.2f}, {0.36f, 0.22f, 0.17f}, {0.55f, 0.45f, 0.35f}};
        const vec3 brickTint = brickTints[seed % 4];
        const uint32_t brick[2] = {p.paint(brickTint, "city brick"), p.paint(brickTint * 0.85f, "city brick")};
        static const vec3 plasterTints[4] = {{0.72f, 0.66f, 0.55f}, {0.6f, 0.68f, 0.7f}, {0.75f, 0.58f, 0.5f}, {0.66f, 0.7f, 0.58f}};
        const uint32_t plaster = p.paint(plasterTints[(seed / 4) % 4], "house plaster");
        const uint32_t darkGlass = p.paint({0.07f, 0.09f, 0.12f}, "city glass");
        const uint32_t warm = p.paint({1.0f, 0.8f, 0.5f}, "city glass", 2.0f);
        const uint32_t cool = p.paint({0.8f, 0.88f, 1.0f}, "city glass", 1.6f);
        static const vec3 shopTints[4] = {{1.0f, 0.55f, 0.35f}, {0.5f, 0.9f, 1.0f}, {1.0f, 0.9f, 0.5f}, {0.9f, 0.5f, 1.0f}};
        const uint32_t shop = p.paint(shopTints[(seed / 16) % 4], "city glass", 3.0f);
        const uint32_t roof = p.paint(vec3(0.24f, 0.24f, 0.25f) * tone, "city concrete");
        const float litShare = style == Style::Tower ? 0.3f : 0.45f;

        const int sx = size.x, sy = size.y, sz = size.z;
        const int top = (sy / STOREY) * STOREY;   // the roof slab
        auto colourAt = [&](int x, int y, int z) -> uint32_t {
            const uint32_t h = hashCell(x + int(seed), y, z);
            const bool wall = x == 0 || x == sx - 1 || z == 0 || z == sz - 1;
            // Rooftop: a parapet round the edge, and plant on top of towers.
            if (y > top) {
                const int above = y - top;
                if (wall && above <= 2) return concrete[0];
                if (style == Style::Tower && above <= 2 * STOREY && x > sx / 3 && x < 2 * sx / 3 && z > sz / 3 && z < 2 * sz / 3) {
                    const bool shell = x == sx / 3 + 1 || x == 2 * sx / 3 - 1 || z == sz / 3 + 1 || z == 2 * sz / 3 - 1 || above == 2 * STOREY;
                    return shell ? concrete[1] : 0u;
                }
                return 0u;
            }
            if (y % STOREY == 0) return y == top ? roof : concrete[(x + z) % 2];   // the slabs
            if (!wall) return 0u;
            // The facade.
            const int along = (x == 0 || x == sx - 1) ? z : x;
            const int face = x == 0 ? 0 : x == sx - 1 ? 1 : z == 0 ? 2 : 3;
            const int storey = y / STOREY, level = y % STOREY;
            const bool corner = (x == 0 || x == sx - 1) && (z == 0 || z == sz - 1);
            auto window = [&](int bay) {
                const uint32_t w = hashCell(bay, storey, face + int(seed) * 7);
                if (float(w % 1000) / 1000.0f > litShare) return darkGlass;
                return (w >> 10) % 4 == 0 ? cool : warm;
            };
            switch (style) {
                case Style::Tower: {
                    if (corner || along % 6 == 0) return concrete[1];        // columns
                    if (level == 1) return concrete[0];                      // the spandrel
                    if (storey == 0) return (h % 5) ? warm : darkGlass;     // the lobby, lit
                    return window(along / 6);
                }
                case Style::Apartments: {
                    const bool opening = (along % 4 == 1 || along % 4 == 2) && level >= 2 && level <= 5 && !corner;
                    if (opening) return window(along / 4);
                    return brick[(y + along / 2) % 2 ? (h & 1) : 0];
                }
                case Style::Shops: {
                    if (storey == 0 && !corner && level >= 1 && level <= 5) return along % 8 == 0 ? concrete[0] : shop;
                    const bool opening = along % 5 >= 1 && along % 5 <= 3 && level >= 2 && level <= 5 && !corner;
                    if (opening) return window(along / 5);
                    return plaster;
                }
            }
            return 0u;
        };
        auto& scene = world.ctx().get<projv::Scene>();
        const int height = top + (style == Style::Tower ? 2 * STOREY + 1 : 3);
        std::vector<ivec3> glass;
        auto noteGlass = [&](int x, int y, int z) {
            const uint32_t c = colourAt(x, y, z);
            if (c == darkGlass || c == warm || c == cool || c == shop) glass.push_back(ivec3(x, y, z));
            return c;
        };
        ComponentHandle h = voxels(scene, "building", ivec3(sx, height, sz), CITY_VOXEL, corner, noteGlass);
        p.apply(scene, h);
        Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        world.emplace<projv::StaticCollider>(e, breakable("city concrete"));
        runtime::assignNetId(world, e);
        own(d, e);
        // Struck from the slab nearest its middle: concrete, which breaks by "demolish", a storey at a time.
        const int slab = std::max(1, int(std::round(0.5f * float(top) / float(STOREY)))) * STOREY;
        return addStructure(d, e, corner, ivec3(sx, height, sz), CITY_VOXEL, float(top) * CITY_VOXEL, ivec3(sx / 2, slab, sz / 2), STOREY,
                            std::move(glass));
    }

    // A parked car, one body, its middle at `at`.
    inline Entity buildCar(World& world, Demo& d, vec3 at, float heading, vec3 paint) {
        projv::RigidBody car;
        car.mass = 1300.0f;
        car.friction = 0.7f;
        return own(d, big(world, "car", ivec3(32, 12, 15), 0.125f, at, glm::angleAxis(heading, vec3(0, 1, 0)), [paint](int x, int y, int z) -> uint32_t {
            const bool wheelX = (x >= 4 && x <= 8) || (x >= 23 && x <= 27);
            if (y < 4) return wheelX && (z <= 2 || z >= 12) ? rgb(0.05f, 0.05f, 0.05f) : (y >= 2 ? rgb(paint) : 0u);
            if (y < 7) return (x == 0 || x == 31) && y == 5 ? rgb(0.9f, 0.85f, 0.6f) : rgb(paint);
            if (x < 7 || x > 26) return 0u;
            const bool glass = y < 11 && (z == 0 || z == 14 || x == 7 || x == 26) && x != 16;
            return glass ? rgb(0.06f, 0.08f, 0.1f) : rgb(paint * 0.9f);
        }, car));
    }

    // A street lamp at `at`, its arm reaching along `arm` (a unit axis).
    inline void buildLamp(World& world, Demo& d, vec3 at, ivec3 arm) {
        const vec3 sodium(1.0f, 0.62f, 0.25f);
        const ivec3 size = arm.x != 0 ? ivec3(9, 34, 1) : ivec3(1, 34, 9);
        const vec3 corner = at - vec3(arm.x < 0 ? 8.0f : 0.0f, 0.0f, arm.z < 0 ? 8.0f : 0.0f) * 0.25f;
        Entity post = own(d, scenery(world, "street lamp", size, 0.25f, corner, [=](int x, int y, int z) -> uint32_t {
            const int along = arm.x != 0 ? (arm.x > 0 ? x : 8 - x) : (arm.z > 0 ? z : 8 - z);
            if (along == 0 && y < 33) return rgb(0.15f, 0.15f, 0.16f);
            if (y == 33 && along < 8) return rgb(0.15f, 0.15f, 0.16f);
            if (y == 32 && along >= 5 && along <= 7) return rgb(sodium);
            return 0u;
        }));
        auto& scene = world.ctx().get<projv::Scene>();
        glowColours(scene, runtime::voxelComponentOf(world, post), {{rgb(sodium), 30.0f}});
    }

    inline const vec3 CAR_PAINTS[6] = {{0.55f, 0.06f, 0.05f}, {0.1f, 0.2f, 0.45f}, {0.8f, 0.8f, 0.78f}, {0.08f, 0.08f, 0.09f},
                                       {0.15f, 0.35f, 0.18f}, {0.7f, 0.55f, 0.15f}};

    inline void buildCity(World& world, Demo& d) {
        std::mt19937 random(424242);
        auto chance = [&](float p) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(random) < p; };
        auto between = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(random); };
        uint32_t seed = 1;
        for (int bi = 0; bi < CITY_BLOCKS; bi++)
            for (int bj = 0; bj < CITY_BLOCKS; bj++) {
                const int ci = bi - CITY_BLOCKS / 2, cj = bj - CITY_BLOCKS / 2;
                const vec3 centre(float(ci) * PITCH, 0.0f, float(cj) * PITCH);
                const int ring = std::max(std::abs(ci), std::abs(cj));
                if (ring == 0) {
                    // Ground zero: one tower, 30 m square and sixty high. It breaks coarser than the rest: it
                    // stands in the fireball, which hides it and vaporises much of it (onBrokenBody), and at
                    // the finer size it alone was a thousand pieces in a step.
                    Target& tower = buildBuilding(world, d, centre - vec3(15.0f, 0.0f, 15.0f), ivec3(60, 18 * STOREY + 1, 60), Style::Tower, seed++);
                    tower.shard = 8.5f;
                    d.groundZero = tower.e;
                    continue;
                }
                // Four lots of 19 m, two sometimes joined.
                const float lot = 0.5f * (BLOCK - 4.0f);
                const bool joinX = chance(0.25f), joinZ = !joinX && chance(0.2f);
                for (int li = 0; li < 2; li++)
                    for (int lj = 0; lj < 2; lj++) {
                        if ((joinX && li == 1) || (joinZ && lj == 1)) continue;
                        const float w = joinX ? 2.0f * lot : lot, dd = joinZ ? 2.0f * lot : lot;
                        const vec3 lotCorner = centre - vec3(0.5f * BLOCK - 2.0f, 0.0f, 0.5f * BLOCK - 2.0f) + vec3(float(li) * lot, 0.0f, float(lj) * lot);
                        const float inset = between(0.5f, 2.0f);
                        const int sx = int((w - 2.0f * inset) / CITY_VOXEL), sz = int((dd - 2.0f * inset) / CITY_VOXEL);
                        Style style;
                        float storeys;
                        if (ring == 1) {
                            style = chance(0.6f) ? Style::Tower : Style::Apartments;
                            storeys = style == Style::Tower ? between(9.0f, 16.0f) : between(6.0f, 10.0f);
                        } else {
                            const float r = between(0.0f, 1.0f);
                            style = r < 0.2f ? Style::Tower : r < 0.65f ? Style::Apartments : Style::Shops;
                            storeys = style == Style::Tower ? between(6.0f, 10.0f) : style == Style::Apartments ? between(4.0f, 7.0f) : between(2.0f, 3.0f);
                        }
                        const int sy = int(storeys) * STOREY + 1;
                        buildBuilding(world, d, lotCorner + vec3(inset, 0.0f, inset), ivec3(sx, sy, sz), style, seed++);
                    }
            }
        // Cars parked along the kerbs, and street lamps down the two avenues that cross nearest the middle.
        for (int line = -2; line <= 2; line++) {
            if (line == 0) continue;
            const float street = (float(line) - (line > 0 ? 0.5f : -0.5f)) * PITCH;   // ±28, ±84
            for (int k = 0; k < 9; k++) {
                if (!chance(0.7f)) continue;
                const float along = -CITY_HALF + 16.0f + float(k) * 31.0f + between(-3.0f, 3.0f);
                const float side = chance(0.5f) ? 5.0f : -5.0f;
                const vec3 paint = CAR_PAINTS[random() % 6];
                if (chance(0.5f)) buildCar(world, d, vec3(street + side, 0.76f, along), PI * 0.5f, paint);
                else buildCar(world, d, vec3(along, 0.76f, street + side), 0.0f, paint);
            }
        }
        for (float street : {-28.0f, 28.0f})
            for (int k = 0; k < 14; k++) {
                const float along = -CITY_HALF + 10.0f + float(k) * 20.0f;
                buildLamp(world, d, vec3(street - 6.5f, 0.0f, along), ivec3(1, 0, 0));
                buildLamp(world, d, vec3(street + 6.5f, 0.0f, along + 10.0f), ivec3(-1, 0, 0));
                buildLamp(world, d, vec3(along, 0.0f, street - 6.5f), ivec3(0, 0, 1));
                buildLamp(world, d, vec3(along + 10.0f, 0.0f, street + 6.5f), ivec3(0, 0, -1));
            }
    }

    // Grass, three greens.
    inline uint32_t grass(uint32_t h) {
        const int n = int(h % 7);
        return n < 3 ? rgb(0.09f, 0.15f, 0.07f) : n < 5 ? rgb(0.08f, 0.13f, 0.06f) : rgb(0.1f, 0.17f, 0.075f);
    }

    // Streets and pavements in the city, grass beyond.
    inline uint32_t cityGround(float wx, float wz, uint32_t h) {
        if (std::abs(wx) >= CITY_HALF || std::abs(wz) >= CITY_HALF) return grass(h);
        const float bx = wx - std::round(wx / PITCH) * PITCH, bz = wz - std::round(wz / PITCH) * PITCH;   // from the block's middle
        const bool inBlock = std::abs(bx) < 0.5f * BLOCK && std::abs(bz) < 0.5f * BLOCK;
        if (inBlock) return h % 3 ? rgb(0.3f, 0.3f, 0.29f) : rgb(0.27f, 0.27f, 0.26f);   // pavement
        // A street: asphalt, a dashed line down the middle, zebra crossings at the corners.
        const float sx = std::abs(bx) - 0.5f * PITCH, sz = std::abs(bz) - 0.5f * PITCH;   // 0 on the street's centre line
        const bool alongX = std::abs(bz) >= 0.5f * BLOCK, alongZ = std::abs(bx) >= 0.5f * BLOCK;
        if (alongX && alongZ) {
            if ((std::abs(bx) > 0.5f * PITCH - 9.0f) != (std::abs(bz) > 0.5f * PITCH - 9.0f) && int(std::floor(wx + wz)) % 2 == 0)
                return rgb(0.6f, 0.6f, 0.58f);
        } else if (alongX && std::abs(sz) < 0.6f && int(std::floor(wx / 3.0f)) % 2 == 0) {
            return rgb(0.7f, 0.62f, 0.3f);
        } else if (alongZ && std::abs(sx) < 0.6f && int(std::floor(wz / 3.0f)) % 2 == 0) {
            return rgb(0.7f, 0.62f, 0.3f);
        }
        return h % 3 ? rgb(0.08f, 0.08f, 0.09f) : rgb(0.1f, 0.1f, 0.11f);
    }

    // ============================================================================================
    // Trees (the forest's, and the house's garden)
    // ============================================================================================

    constexpr float TREE_VOXEL = 0.5f;
    // Room round a grove's trees for the wind to bend them into (leanTick). Empty voxels cost nothing: a
    // component holds only the chunks that have something in them.
    constexpr int GROVE_PAD = 48;

    // The shapes: four pines (tiered cones of needles on a straight trunk, 12 to 17 m) and two broadleaf
    // trees (a round crown on a shorter trunk). Trunk wood splinters along the grain; needles and leaves
    // crumble. Their colours, and the materials they name, are d.treePalette.
    inline void makeTreeShapes(Demo& d) {
        d.trees.clear();
        Palette& p = d.treePalette;
        for (int k = 0; k < 6; k++) {
            const bool pine = k < 4;
            const int height = pine ? 24 + 3 * k : 18 + 3 * (k - 4);         // the trunk's top, voxels
            const int radius = pine ? 4 + k / 2 : 5 + (k - 4);                // the crown's, voxels
            const int thick = height >= 30 ? 2 : 1;                             // the trunk, voxels across
            const uint32_t bark[2] = {p.paint({0.27f, 0.18f, 0.11f}, "tree wood"), p.paint({0.22f, 0.15f, 0.1f}, "tree wood")};
            const vec3 leafTint = pine ? vec3(0.07f, 0.19f, 0.08f) : vec3(0.17f, 0.32f, 0.09f);
            const uint32_t leaf[3] = {p.paint(leafTint, "foliage"), p.paint(leafTint * 0.82f, "foliage"), p.paint(leafTint * 1.18f, "foliage")};
            TreeShape shape;
            shape.height = height + 3;
            shape.crownRadius = (float(radius) + 0.5f) * TREE_VOXEL;
            for (int y = 0; y <= height + 2; y++)
                for (int z = -radius - 1; z <= radius + 1; z++)
                    for (int x = -radius - 1; x <= radius + 1; x++) {
                        const uint32_t h = hashCell(x + 31 * k, y, z);
                        if (x >= 0 && x < thick && z >= 0 && z < thick && y <= height - 2) {
                            shape.voxels.push_back({ivec3(x, y, z), bark[h & 1], true});
                            continue;
                        }
                        // How far out from the trunk this voxel is, against how far the crown reaches here.
                        const float dx = float(x) - 0.5f * float(thick - 1), dz = float(z) - 0.5f * float(thick - 1);
                        float out, reach;
                        if (pine) {
                            // Tiers: each four voxels a cone, wider at its foot, the whole narrowing to the top.
                            const int foot = height * 3 / 10, top = height + 2;
                            if (y < foot) continue;
                            const float up = float(y - foot) / float(top - foot);
                            const float tier = 1.0f - 0.45f * float((y - foot) % 4) / 3.0f;
                            out = std::sqrt(dx * dx + dz * dz);
                            reach = (float(radius) * (1.0f - up) + 0.7f) * tier;
                        } else {
                            // A ball, a little squashed, round the top of the trunk.
                            const float dy = (float(y) - float(height) * 0.72f) / 0.8f;
                            out = std::sqrt(dx * dx + dz * dz + dy * dy);
                            reach = float(radius) + 0.4f;
                        }
                        if (out <= reach + 0.35f * (float(h % 100) / 100.0f - 0.5f)) shape.voxels.push_back({ivec3(x, y, z), leaf[h % 3], false});
                    }
            d.trees.push_back(std::move(shape));
        }
    }

    // A tree to plant: its shape, where its foot is, and how many quarter turns it is turned.
    struct Planted { int shape; vec3 at; int turn; };

    // A grove: trees grown into one voxel grid -- one component (and one body, if `collides`) for all of them,
    // so a forest is a few hundred rather than thousands -- each tree its own target (several targets, one
    // entity). No tree touches another: a crown voxel that would touch a neighbour's is left out, so a tree
    // shattered or tipped over leaves its neighbours whole. Only a grove that collides can be struck (Grove).
    inline void plantGrove(World& world, Demo& d, const std::vector<Planted>& trees, bool collides = true) {
        if (trees.empty()) return;
        const float v = TREE_VOXEL;
        vec3 lo(1e9f), hi(-1e9f);
        for (const Planted& t : trees) { lo = glm::min(lo, t.at); hi = glm::max(hi, t.at); }
        const vec3 origin(std::floor(lo.x / v) * v - float(GROVE_PAD) * v, 0.0f, std::floor(lo.z / v) * v - float(GROVE_PAD) * v);
        int tallest = 0;
        for (const TreeShape& shape : d.trees) tallest = std::max(tallest, shape.height);
        const ivec3 size(int((hi.x - origin.x) / v) + GROVE_PAD + 1, tallest, int((hi.z - origin.z) / v) + GROVE_PAD + 1);
        std::map<std::tuple<int, int, int>, int> owner;   // which tree has each voxel
        auto ownerOf = [&](ivec3 q) { auto it = owner.find({q.x, q.y, q.z}); return it == owner.end() ? -1 : it->second; };
        std::vector<projv::PendingVoxelOp> ops;
        const size_t first = d.targets.size();
        for (size_t i = 0; i < trees.size(); i++) {
            const Planted& planted = trees[i];
            const TreeShape& shape = d.trees[size_t(planted.shape)];
            const ivec3 foot(int(std::round((planted.at.x - origin.x) / v)), 0, int(std::round((planted.at.z - origin.z) / v)));
            Target t;
            t.role = Role::Tree;
            t.lo = planted.at - vec3(shape.crownRadius, 0.0f, shape.crownRadius);
            t.hi = planted.at + vec3(shape.crownRadius, float(shape.height) * v, shape.crownRadius);
            t.origin = origin;
            t.voxel = v;
            t.voxels = size;
            t.strikeAt = foot + ivec3(0, shape.height / 3, 0);
            t.foot = foot;
            t.reach = 0.75f * float(shape.height) * v + 1.0f;
            for (const TreeVoxel& sv : shape.voxels) {
                const ivec3 a = sv.at;
                const ivec3 turned = planted.turn == 0 ? a : planted.turn == 1 ? ivec3(-a.z, a.y, a.x) : planted.turn == 2 ? ivec3(-a.x, a.y, -a.z) : ivec3(a.z, a.y, -a.x);
                const ivec3 q = foot + turned;
                if (ownerOf(q) >= 0) continue;
                if (!sv.trunk) {
                    bool touches = false;
                    for (const ivec3 n : {ivec3(1, 0, 0), ivec3(-1, 0, 0), ivec3(0, 1, 0), ivec3(0, -1, 0), ivec3(0, 0, 1), ivec3(0, 0, -1)}) {
                        const int o = ownerOf(q + n);
                        touches = touches || (o >= 0 && o != int(i));
                    }
                    if (touches) continue;
                }
                owner[{q.x, q.y, q.z}] = int(i);
                ops.push_back({true, q, sv.colour});
                t.body.push_back({q, sv.colour, sv.trunk});
                t.now.push_back(q);
            }
            d.targets.push_back(std::move(t));
        }
        auto& scene = world.ctx().get<projv::Scene>();
        ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "grove", projv::INVALID_COMPONENT_HANDLE, 16, v);
        utils::setComponentTransform(scene, h, origin, IDENTITY, 1.0f);
        utils::queueVoxelAdd(scene, h, ops);
        utils::updateScene(scene);
        d.treePalette.apply(scene, h);
        Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        if (collides) world.emplace<projv::StaticCollider>(e, breakable("tree wood"));
        runtime::assignNetId(world, e);
        own(d, e);
        Grove& grove = d.groves[e];
        grove.component = h;
        grove.body = collides;
        grove.size = size;
        grove.bits.assign((size_t(size.x) * size_t(size.y) * size_t(size.z) + 63) / 64, 0u);
        for (size_t i = first; i < d.targets.size(); i++) {
            d.targets[i].e = e;
            for (const ivec3& v : d.targets[i].now) grove.fill(v, true);
        }
    }

    // ============================================================================================
    // The house: a suburban street
    // ============================================================================================

    constexpr float HOUSE_VOXEL = 0.25f;
    constexpr int HOUSE_STOREY = 11;   // voxels: 2.75 m

    // A two-storey house, `size` voxels across (its depth along z), its corner at `corner`: a concrete
    // ground slab and floors, brick below and plaster above, windows (some lit), a front door on z = 0, a
    // wall through the middle, and a pitched roof of tiles over it all with a chimney. `seed` varies it.
    inline Target& buildHouse(World& world, Demo& d, vec3 corner, int sx, int sz, uint32_t seed) {
        Palette p;
        const uint32_t slab = p.paint({0.5f, 0.5f, 0.48f}, "house slab");
        const uint32_t floor = p.paint({0.45f, 0.32f, 0.2f}, "house slab");
        static const vec3 brickTints[3] = {{0.55f, 0.22f, 0.15f}, {0.6f, 0.35f, 0.22f}, {0.48f, 0.3f, 0.24f}};
        const vec3 brickTint = brickTints[seed % 3];
        const uint32_t brick[2] = {p.paint(brickTint, "house brick"), p.paint(brickTint * 0.86f, "house brick")};
        static const vec3 plasterTints[3] = {{0.86f, 0.82f, 0.72f}, {0.75f, 0.8f, 0.82f}, {0.88f, 0.76f, 0.62f}};
        const uint32_t plaster = p.paint(plasterTints[(seed / 3) % 3], "house plaster");
        const uint32_t inner = p.paint({0.8f, 0.78f, 0.74f}, "house plaster");
        const uint32_t timber = p.paint({0.3f, 0.2f, 0.13f}, "house timber");
        const uint32_t glassDark = p.paint({0.1f, 0.13f, 0.17f}, "window glass");
        const uint32_t glassLit = p.paint({1.0f, 0.82f, 0.55f}, "window glass", 2.2f);
        const uint32_t tile[2] = {p.paint({0.5f, 0.2f, 0.13f}, "roof tile"), p.paint({0.42f, 0.17f, 0.12f}, "roof tile")};

        const int eaves = 2 * HOUSE_STOREY;          // the attic floor, where the roof starts
        const int half = sz / 2;                      // the roof rises over half the depth, front and back
        const int ridge = eaves + half;               // a 45-degree roof
        const int chimneyX = sx - 7, chimneyZ = half + 3;
        auto colourAt = [&](int x, int y, int z) -> uint32_t {
            const uint32_t h = hashCell(x + int(seed) * 13, y, z);
            const bool wall = x == 0 || x == sx - 1 || z == 0 || z == sz - 1;
            const int fromEdge = std::min(z, sz - 1 - z);   // across the roof's slope
            // The chimney, through the roof.
            if (x >= chimneyX && x < chimneyX + 3 && z >= chimneyZ && z < chimneyZ + 3 && y >= eaves && y <= ridge + 3)
                return brick[(y + x) % 2];
            if (y > eaves) {
                const int rise = y - eaves;
                if (fromEdge == rise - 1 || fromEdge == rise) return tile[(x + rise) % 2 ? 0 : (h & 1)];   // the roof, two thick
                if (fromEdge > rise && (x == 0 || x == sx - 1)) return plaster;                       // the gable ends
                return 0u;
            }
            if (y == 0) return slab;
            if (y == HOUSE_STOREY || y == eaves) return wall ? slab : floor;
            const int level = y % HOUSE_STOREY, storey = y / HOUSE_STOREY;
            if (!wall) return x == sx / 2 && !(z >= sz / 2 - 2 && z <= sz / 2 + 1 && level <= 8) ? inner : 0u;   // the middle wall, a doorway
            const int along = (x == 0 || x == sx - 1) ? z : x;
            const bool corner = (x == 0 || x == sx - 1) && (z == 0 || z == sz - 1);
            if (corner) return timber;
            if (z == 0 && storey == 0 && along >= sx / 2 + 3 && along <= sx / 2 + 6 && level <= 8) return timber;   // the front door
            if (along % 8 >= 2 && along % 8 <= 5 && level >= 3 && level <= 8)                                    // windows
                return hashCell(along / 8, storey, x == 0 ? 0 : x == sx - 1 ? 1 : z == 0 ? 2 : 3) % 3 == 0 ? glassLit : glassDark;
            if (storey == 0) return brick[(y + along / 2) % 2 ? (h & 1) : 0];
            return plaster;
        };
        auto& scene = world.ctx().get<projv::Scene>();
        std::vector<ivec3> glass;
        auto noteGlass = [&](int x, int y, int z) {
            const uint32_t c = colourAt(x, y, z);
            if (c == glassDark || c == glassLit) glass.push_back(ivec3(x, y, z));
            return c;
        };
        const ivec3 size(sx, ridge + 4, sz);
        ComponentHandle h = voxels(scene, "house", size, HOUSE_VOXEL, corner, noteGlass);
        p.apply(scene, h);
        Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        world.emplace<projv::StaticCollider>(e, breakable("house brick"));
        runtime::assignNetId(world, e);
        own(d, e);
        // Struck from the first floor, beside the middle wall: a slab, which breaks by "rubble".
        return addStructure(d, e, corner, size, HOUSE_VOXEL, float(eaves) * HOUSE_VOXEL, ivec3(sx / 2 + 3, HOUSE_STOREY, sz / 2),
                            HOUSE_STOREY, std::move(glass));
    }

    // A run of picket fence `length` metres long along x (or z, `alongZ`) from `start`: posts, two rails,
    // pickets.
    inline void buildFence(World& world, Demo& d, vec3 start, float length, bool alongZ) {
        const int n = int(length / HOUSE_VOXEL);
        const ivec3 size = alongZ ? ivec3(1, 5, n) : ivec3(n, 5, 1);
        const uint32_t wood = rgb(0.78f, 0.74f, 0.66f), post = rgb(0.62f, 0.58f, 0.5f);
        Entity e = own(d, scenery(world, "fence", size, HOUSE_VOXEL, start, [=](int x, int y, int z) -> uint32_t {
            const int along = alongZ ? z : x;
            if (along % 12 == 0) return post;
            if (y == 1 || y == 3) return wood;   // the rails
            return along % 2 == 0 ? wood : 0u;
        }, breakable("fence")));
        addStructure(d, e, start, size, HOUSE_VOXEL, 5.0f * HOUSE_VOXEL, alongZ ? ivec3(0, 1, n / 2) : ivec3(n / 2, 1, 0), 0);
    }

    // A timber shed or cabin, `size` voxels, its corner at `corner`: plank walls, a door on z = 0, a window
    // either side, a flat roof.
    inline void buildShed(World& world, Demo& d, vec3 corner, ivec3 size, vec3 tint) {
        Palette p;
        const uint32_t plank[2] = {p.paint(tint, "house timber"), p.paint(tint * 0.85f, "house timber")};
        const uint32_t floorboard = p.paint(tint * 0.7f, "floorboards");
        const uint32_t roof = p.paint({0.2f, 0.2f, 0.21f}, "roof tile");
        const uint32_t pane = p.paint({0.12f, 0.15f, 0.18f}, "window glass");
        const int sx = size.x, sy = size.y, sz = size.z;
        auto& scene = world.ctx().get<projv::Scene>();
        ComponentHandle h = voxels(scene, "shed", size, HOUSE_VOXEL, corner, [&](int x, int y, int z) -> uint32_t {
            if (y == 0) return floorboard;
            if (y == sy - 1) return roof;
            const bool wall = x == 0 || x == sx - 1 || z == 0 || z == sz - 1;
            if (!wall) return 0u;
            if (z == 0 && x >= sx / 2 - 1 && x <= sx / 2 + 1 && y <= 7) return 0u;   // the doorway
            if ((x == 0 || x == sx - 1) && z >= sz / 2 - 1 && z <= sz / 2 + 1 && y >= 4 && y <= 6) return pane;
            return plank[(x + z) % 2];
        });
        p.apply(scene, h);
        Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
        world.emplace<projv::StaticCollider>(e, breakable("floorboards"));
        runtime::assignNetId(world, e);
        own(d, e);
        addStructure(d, e, corner, size, HOUSE_VOXEL, float(sy) * HOUSE_VOXEL, ivec3(sx / 2, 0, sz / 2), 0);
    }

    // The street runs along x in front of the houses (z < 0); the plots are 24 m wide.
    constexpr float STREET_Z0 = -16.0f, STREET_Z1 = -8.0f;   // the road, kerb to kerb
    constexpr float PLOT = 24.0f;

    inline uint32_t houseGround(float wx, float wz, uint32_t h) {
        if (wz >= STREET_Z0 && wz < STREET_Z1) {   // the road: asphalt, a dashed centre line
            if (std::abs(wz - 0.5f * (STREET_Z0 + STREET_Z1)) < 0.2f && int(std::floor(wx / 3.0f)) % 2 == 0) return rgb(0.8f, 0.78f, 0.7f);
            return h % 3 ? rgb(0.09f, 0.09f, 0.1f) : rgb(0.11f, 0.11f, 0.12f);
        }
        if ((wz >= STREET_Z1 && wz < STREET_Z1 + 2.0f) || (wz >= STREET_Z0 - 2.0f && wz < STREET_Z0))   // the pavements
            return h % 2 ? rgb(0.42f, 0.42f, 0.4f) : rgb(0.38f, 0.38f, 0.36f);
        // Each plot's drive, from the road to beside its house.
        const float px = wx - std::round(wx / PLOT) * PLOT;
        const bool front = wz > STREET_Z1, across = wz < STREET_Z0;
        if ((front && px >= -10.0f && px < -6.5f && wz < 2.0f) || (across && px >= 6.5f && px < 10.0f && wz > -26.0f))
            return h % 2 ? rgb(0.35f, 0.33f, 0.3f) : rgb(0.32f, 0.3f, 0.28f);
        if (std::abs(px) > 11.6f && std::abs(wz) < 40.0f) return rgb(0.16f, 0.13f, 0.09f);   // the beds between plots
        return grass(h);
    }

    inline void buildHouseMap(World& world, Demo& d) {
        makeTreeShapes(d);
        std::vector<Planted> trees;
        // Ground zero's house, 10 m by 8 m, its middle on the origin, and its garden: a fence round the plot,
        // a shed at the back, a car on the drive, trees.
        Target& home = buildHouse(world, d, vec3(-5.0f, 0.0f, -4.0f), 40, 32, 0);
        d.groundZero = home.e;
        buildShed(world, d, vec3(3.5f, 0.0f, 9.0f), ivec3(14, 10, 11), vec3(0.42f, 0.3f, 0.18f));
        buildFence(world, d, vec3(-11.5f, 0.0f, 15.0f), 23.0f, false);
        buildFence(world, d, vec3(-11.5f, 0.0f, -6.0f), 21.0f, true);
        buildFence(world, d, vec3(11.25f, 0.0f, -6.0f), 21.0f, true);
        buildCar(world, d, vec3(-8.25f, 0.76f, -3.0f), PI * 0.5f, CAR_PAINTS[1]);
        trees.push_back({4, vec3(-7.0f, 0.0f, 10.0f), 0});
        trees.push_back({5, vec3(8.5f, 0.0f, 3.0f), 1});
        trees.push_back({0, vec3(9.0f, 0.0f, 12.5f), 2});
        // The neighbours: either side, and across the road.
        uint32_t seed = 1;
        for (float x : {-2.0f * PLOT, -PLOT, PLOT, 2.0f * PLOT}) {
            buildHouse(world, d, vec3(x - 5.0f, 0.0f, -4.0f), 40, 32, seed++);
            buildFence(world, d, vec3(x - 11.5f, 0.0f, 15.0f), 23.0f, false);
            trees.push_back({4 + int(seed % 2), vec3(x + 7.5f, 0.0f, 10.0f), int(seed % 4)});
            if (seed % 2) buildCar(world, d, vec3(x - 8.25f, 0.76f, -3.0f), PI * 0.5f, CAR_PAINTS[(seed * 3) % 6]);
        }
        for (float x : {-1.5f * PLOT, -0.5f * PLOT, 0.5f * PLOT, 1.5f * PLOT}) {
            buildHouse(world, d, vec3(x - 5.0f, 0.0f, -36.0f), 40, 32, seed++);
            trees.push_back({int(seed % 4), vec3(x - 7.0f, 0.0f, -40.0f), int(seed % 4)});
        }
        // Street lamps along the far pavement, and a few parked cars.
        for (int k = -3; k <= 3; k++) buildLamp(world, d, vec3(float(k) * 16.0f + 8.0f, 0.0f, STREET_Z0 - 1.0f), ivec3(0, 0, 1));
        buildCar(world, d, vec3(14.0f, 0.76f, STREET_Z0 + 1.6f), 0.0f, CAR_PAINTS[0]);
        buildCar(world, d, vec3(-30.0f, 0.76f, STREET_Z1 - 1.6f), PI, CAR_PAINTS[4]);
        // A line of trees along the back of the plots.
        std::mt19937 random(777);
        for (float x = -60.0f; x <= 60.0f; x += 7.0f)
            trees.push_back({int(random() % 6), vec3(x + float(random() % 30) / 10.0f, 0.0f, 22.0f + float(random() % 40) / 10.0f), int(random() % 4)});
        plantGrove(world, d, trees);
    }

    // ============================================================================================
    // The forest: Tunguska
    // ============================================================================================

    // The track runs north-south past the clearing, a little east of it.
    inline float trackOffset(float wz) { return 38.0f + 9.0f * std::sin(wz / 45.0f); }

    inline uint32_t forestGround(float wx, float wz, uint32_t h) {
        if (std::abs(wx - trackOffset(wz)) < 2.0f) return h % 3 ? rgb(0.3f, 0.24f, 0.16f) : rgb(0.26f, 0.2f, 0.13f);   // the track
        const float r = std::sqrt(wx * wx + wz * wz);
        if (r < 16.0f) return h % 4 ? rgb(0.15f, 0.2f, 0.08f) : rgb(0.18f, 0.22f, 0.1f);   // the clearing's grass
        // The forest floor: needles, moss, bare earth.
        const int n = int(h % 9);
        return n < 4 ? rgb(0.17f, 0.12f, 0.07f) : n < 7 ? rgb(0.09f, 0.13f, 0.05f) : rgb(0.21f, 0.16f, 0.1f);
    }

    inline void buildForest(World& world, Demo& d) {
        makeTreeShapes(d);
        // Trees on a jittered grid 9 m apart out to FOREST_REACH, mostly pines, none in the clearing or on the
        // track, grown in groves of GROVE_TILE metres square.
        std::mt19937 random(1908);
        auto between = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(random); };
        constexpr float SPACING = 9.0f, FOREST_REACH = 290.0f, GROVE_TILE = 24.0f;
        constexpr int TILES = int(2.0f * FOREST_REACH / GROVE_TILE) + 1;
        std::vector<std::vector<Planted>> groves(size_t(TILES * TILES));
        for (float gz = -FOREST_REACH; gz <= FOREST_REACH; gz += SPACING)
            for (float gx = -FOREST_REACH; gx <= FOREST_REACH; gx += SPACING) {
                const vec3 at(gx + between(-2.0f, 2.0f), 0.0f, gz + between(-2.0f, 2.0f));
                const float r = std::sqrt(at.x * at.x + at.z * at.z);
                if (r > FOREST_REACH || r < 14.0f || std::abs(at.x - trackOffset(at.z)) < 4.5f) continue;
                const int k = between(0.0f, 1.0f) < 0.82f ? int(random() % 4) : 4 + int(random() % 2);
                const int tx = std::clamp(int((at.x + FOREST_REACH) / GROVE_TILE), 0, TILES - 1);
                const int tz = std::clamp(int((at.z + FOREST_REACH) / GROVE_TILE), 0, TILES - 1);
                groves[size_t(tz * TILES + tx)].push_back({k, at, int(random() % 4)});
            }
        // Only the groves that reach in near enough to be shattered are bodies.
        for (const std::vector<Planted>& grove : groves) {
            float nearest = 1e9f;
            for (const Planted& t : grove) nearest = std::min(nearest, std::sqrt(t.at.x * t.at.x + t.at.z * t.at.z));
            plantGrove(world, d, grove, nearest < d.map->shatterRadius + 8.0f);
        }
        // A cabin by the track, and a truck outside it.
        buildShed(world, d, vec3(48.0f, 0.0f, -64.0f), ivec3(24, 13, 18), vec3(0.4f, 0.27f, 0.16f));
        buildCar(world, d, vec3(44.5f, 0.76f, -60.0f), PI * 0.5f, CAR_PAINTS[4]);
    }

    // ============================================================================================
    // The maps
    // ============================================================================================

    // name, title; scale; the ground (half its width) and the earth (half its width, depth, voxel); how far the
    // shock runs; structures flattened, wrecked, windows out; trees shattered, stripped, felled, bent; what
    // structures break to (against the city); the orbiting camera's distance, height and aim; the ground's
    // colours; the builder.
    inline const MapSpec MAPS[] = {
        {"city",   "a city, blown apart from its middle",          1.0f, 210.0f, 21.0f, 14.0f, 0.5f,  150.0f,
         95.0f, 140.0f, 185.0f,  40.0f, 100.0f, 165.0f, 0.0f,    1.0f,   230.0f, 22.0f, 15.0f, cityGround, buildCity},
        {"house",  "a house on a suburban street, at ground zero", 0.5f, 100.0f, 10.0f, 7.0f, 0.25f,  75.0f,
         9.0f, 30.0f, 60.0f,     12.0f, 22.0f, 40.0f, 70.0f,     0.45f,  75.0f, 12.0f, 7.0f, houseGround, buildHouseMap},
        {"forest", "a pine forest, flattened like Tunguska",       1.0f, 300.0f, 21.0f, 14.0f, 0.5f,  300.0f,
         60.0f, 110.0f, 160.0f,  45.0f, 120.0f, 190.0f, 290.0f,  0.35f,  320.0f, 60.0f, 15.0f, forestGround, buildForest},
    };

    inline const MapSpec* mapNamed(const std::string& name) {
        for (const MapSpec& m : MAPS)
            if (name == m.name) return &m;
        return nullptr;
    }
}
