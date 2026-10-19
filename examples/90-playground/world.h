// The playground's field and everything on it: a castle, brick chimneys, a timber house, a glass
// greenhouse, a stone bridge and trees round a plaza, every one of them breakable; and the things
// players make -- cannonballs, TNT, crates, balls, blocks. Every model is made voxel by voxel here,
// each colour naming the physics material it is made of; the scene folder the server and every
// client load is written from this (`playground_server --write-assets <dir>`).
#pragma once

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "data_structures/scene.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/scene_query.h"

namespace playground {
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;

    inline uint32_t rgb(float r, float g, float b) { return projv::packRGB10(r, g, b); }

    // Small changes of shade by position, so a surface reads as made of something.
    inline float shade(int x, int y, int z, float amount) {
        // Unsigned arithmetic: a signed multiply that overflows is undefined.
        uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u;
        return 1.0f - amount + amount * float(h % 4) / 3.0f;   // four levels: a palette holds 255
    }

    constexpr int MAX_PLAYERS = 4;
    constexpr vec3 PLAYER_COLOURS[MAX_PLAYERS] = {vec3(0.9f, 0.22f, 0.18f), vec3(0.2f, 0.42f, 0.95f), vec3(0.25f, 0.8f, 0.3f),
                                                  vec3(0.98f, 0.82f, 0.15f)};

    // ---- Where things are -----------------------------------------------------------------------------
    //
    // The plaza, where everyone starts, is the middle; the castle is north (+z), the chimneys east,
    // the house west, the greenhouse and the bridge south. The crates and the TNT stand between.
    constexpr float GROUND_HALF = 70.0f;
    inline vec3 castleCentre() { return vec3(0.0f, 0.0f, 38.0f); }
    constexpr float CASTLE_HALF = 10.0f, WALL_HEIGHT = 5.0f;
    inline vec3 crateStack() { return vec3(12.0f, 0.0f, 8.0f); }
    inline vec3 tntStack() { return vec3(-4.0f, 0.0f, 24.0f); }

    // ---- Models ----------------------------------------------------------------------------------------

    // A model being built: voxels, each colour naming the physics material it is made of.
    struct Model {
        std::string name;
        vec3        at{0.0f};            // where its voxel (0, 0, 0) corner stands
        float       voxel = 0.25f;
        std::vector<projv::PendingVoxelOp> ops;
        std::map<uint32_t, std::string> materials;   // colour -> physics material

        void put(int x, int y, int z, uint32_t colour, const std::string& material) {
            ops.push_back({true, ivec3(x, y, z), colour});
            if (!material.empty()) materials[colour] = material;
        }
    };

    struct Voxel {
        uint32_t    colour = 0;   // 0: empty
        const char* material = "";
    };
    using Fill = std::function<Voxel(int x, int y, int z)>;

    // A box `size` metres from its corner `at`, filled voxel by voxel by `fill`.
    inline Model box(const std::string& name, vec3 at, vec3 size, float voxel, const Fill& fill) {
        const ivec3 n(int(std::lround(size.x / voxel)), int(std::lround(size.y / voxel)), int(std::lround(size.z / voxel)));
        Model m{name, at, voxel};
        for (int y = 0; y < n.y; y++)
            for (int z = 0; z < n.z; z++)
                for (int x = 0; x < n.x; x++)
                    if (const Voxel v = fill(x, y, z); v.colour) m.put(x, y, z, v.colour, v.material);
        return m;
    }

    inline Voxel stone(int x, int y, int z, float darker = 1.0f) {
        const float s = shade(x / 2, y / 2, z / 2, 0.24f) * darker;
        return {y % 8 == 0 ? rgb(0.26f * s, 0.25f * s, 0.23f * s) : rgb(0.34f * s, 0.32f * s, 0.29f * s), "stone"};
    }

    // Bricks in courses, in mortar: for 1/8 m voxels, three to a course, six to a brick.
    inline Voxel brick(int along, int y) {
        const int course = y / 3, shifted = along + (course % 2) * 3;
        if (y % 3 == 2 || shifted % 6 == 5) return {rgb(0.72f, 0.7f, 0.64f), "mortar"};
        static const float reds[3][3] = {{0.56f, 0.23f, 0.15f}, {0.64f, 0.29f, 0.18f}, {0.48f, 0.2f, 0.14f}};
        const int which = (course * 7 + shifted / 6 * 3) % 3;
        return {rgb(reds[which][0], reds[which][1], reds[which][2]), "brick"};
    }

    // ---- The castle -------------------------------------------------------------------------------------
    //
    // Four curtain walls round a courtyard, a gate in the south one (toward the plaza), a tower at each
    // corner, and a hollow keep in the middle. Each is a component of its own, so each breaks alone.

    // A curtain wall `thick` metres deep, along x or along z, crenellated on top; `gate`, a doorway.
    inline Model curtain(const std::string& name, vec3 at, float length, bool alongX, bool gate) {
        constexpr float voxel = 0.25f, thick = 1.5f;
        const vec3 size = alongX ? vec3(length, WALL_HEIGHT + 1.0f, thick) : vec3(thick, WALL_HEIGHT + 1.0f, length);
        const int walk = int(WALL_HEIGHT / voxel), n = int(length / voxel);
        return box(name, at, size, voxel, [=](int x, int y, int z) -> Voxel {
            const int along = alongX ? x : z, across = alongX ? z : x;
            if (gate && std::abs(along - n / 2) < 7 && y < 14) return {};
            if (y >= walk) {
                if (across >= 2 && across < 4) return {};          // the walk
                if (y >= walk + 2 && (along / 4) % 2) return {};   // crenels
            }
            return stone(x, y, z);
        });
    }

    inline Model tower(const std::string& name, vec3 at) {
        return box(name, at, vec3(3.5f, 9.0f, 3.5f), 0.25f, [](int x, int y, int z) -> Voxel {
            if (y >= 32) {
                const bool rim = x < 2 || z < 2 || x >= 12 || z >= 12;
                if (!rim || ((x + z) / 3) % 2) return {};
            }
            return stone(x, y, z, 0.92f);
        });
    }

    // The keep, 5 m square and 12 m to its roof: hollow, a door facing the gate, slit windows.
    inline Model keep(vec3 at) {
        return box("keep", at, vec3(5.0f, 13.0f, 5.0f), 0.25f, [](int x, int y, int z) -> Voxel {
            const int side = 20, roof = 48;
            const bool shell = x < 3 || z < 3 || x >= side - 3 || z >= side - 3;
            if (y >= roof + 4) return {};
            if (y >= roof) {
                const bool rim = x < 2 || z < 2 || x >= side - 2 || z >= side - 2;
                if (!rim || ((x + z) / 3) % 2) return {};
                return stone(x, y, z, 0.85f);
            }
            if (y >= roof - 3) return stone(x, y, z, 0.85f);
            if (!shell) return {};
            if (z < 3 && x >= 7 && x < 13 && y < 11) return {};                                         // the door
            if ((y % 16) >= 8 && (y % 16) < 13 && ((z >= 9 && z < 11) || (x >= 9 && x < 11))) return {};   // slits
            return stone(x, y, z, 0.85f);
        });
    }

    inline std::vector<Model> castle() {
        const vec3 c = castleCentre();
        const float h = CASTLE_HALF;
        std::vector<Model> m;
        m.push_back(curtain("castle south wall", c + vec3(-h, 0.0f, -h), 2.0f * h, true, true));
        m.push_back(curtain("castle north wall", c + vec3(-h, 0.0f, h - 1.5f), 2.0f * h, true, false));
        m.push_back(curtain("castle west wall", c + vec3(-h, 0.0f, -h + 1.5f), 2.0f * h - 3.0f, false, false));
        m.push_back(curtain("castle east wall", c + vec3(h - 1.5f, 0.0f, -h + 1.5f), 2.0f * h - 3.0f, false, false));
        for (float sx : {-1.0f, 1.0f})
            for (float sz : {-1.0f, 1.0f})
                m.push_back(tower(std::string("castle tower ") + (sz < 0 ? "south " : "north ") + (sx < 0 ? "west" : "east"),
                                  c + vec3(sx * h - 1.75f, 0.0f, sz * h - 1.75f)));
        m.push_back(keep(c + vec3(-2.5f, 0.0f, 0.5f)));
        return m;
    }

    // ---- The east: brick chimneys and a wall -----------------------------------------------------------

    // A hollow square chimney of brick, `height` metres, 1/8 m voxels.
    inline Model chimney(const std::string& name, vec3 at, float height) {
        const int side = 20, wall = 2;
        return box(name, at, vec3(2.5f, height, 2.5f), 0.125f, [=](int x, int y, int z) -> Voxel {
            if (x >= wall && z >= wall && x < side - wall && z < side - wall) return {};
            if (x < wall || x >= side - wall) return brick(z + x, y);
            return brick(x + z, y);
        });
    }

    inline Model brickWall(const std::string& name, vec3 at, float length) {
        return box(name, at, vec3(0.5f, 2.5f, length), 0.125f, [](int, int y, int z) { return brick(z, y); });
    }

    // ---- The west: a timber house -----------------------------------------------------------------------

    // Plank walls on a timber frame, a door and windows, a gable roof of shingles.
    inline Model house(vec3 at) {
        constexpr int nx = 32, nz = 24, walls = 16;   // 8 by 6 m, 4 m to the eaves
        return box("house", at, vec3(8.0f, 7.0f, 6.0f), 0.25f, [](int x, int y, int z) -> Voxel {
            const bool corner = (x < 1 || x >= nx - 1) && (z < 1 || z >= nz - 1);
            const bool shell = x < 1 || z < 1 || x >= nx - 1 || z >= nz - 1;
            const uint32_t post = rgb(0.33f, 0.22f, 0.13f);
            if (y < walls) {
                if (corner || (shell && (y == 0 || y == walls - 1))) return {post, "timber"};
                if (!shell) return {};
                if (z < 1 && x >= 13 && x < 19 && y < 10) return {};                         // the door, east of middle
                if ((y >= 6 && y < 11) && ((x % 10) >= 4 && (x % 10) < 7) && (z < 1 || z >= nz - 1)) return {};   // windows
                if ((y >= 6 && y < 11) && ((z % 10) >= 4 && (z % 10) < 7) && (x < 1 || x >= nx - 1)) return {};
                const float s = shade(x, y / 2, z, 0.18f);
                return {(y % 2) ? rgb(0.72f * s, 0.55f * s, 0.34f * s) : rgb(0.64f * s, 0.48f * s, 0.29f * s), "planks"};
            }
            // The roof: a ridge along x, falling a voxel for each voxel out toward z = 0 and z = nz.
            const int rise = y - walls;
            const int fromEdge = std::min(z, nz - 1 - z);
            if (rise == fromEdge || rise == fromEdge + 1) {
                const float s = shade(x, rise, z, 0.2f);
                return {(x / 2 + rise) % 2 ? rgb(0.45f * s, 0.18f * s, 0.12f * s) : rgb(0.38f * s, 0.15f * s, 0.1f * s), "planks"};
            }
            // The gables: the end walls carried up under the roof.
            if ((x < 1 || x >= nx - 1) && rise < fromEdge) return {post, "planks"};
            return {};
        });
    }

    // ---- The south: a greenhouse and a bridge ------------------------------------------------------------

    // Glass panes on a timber frame, 10 by 6 m, 4 m high: what breaks most easily on the field.
    inline Model greenhouse(vec3 at) {
        constexpr int nx = 40, ny = 16, nz = 24;
        return box("greenhouse", at, vec3(10.0f, 4.0f, 6.0f), 0.25f, [](int x, int y, int z) -> Voxel {
            const bool side = x < 1 || z < 1 || x >= nx - 1 || z >= nz - 1;
            const bool top = y == ny - 1;
            if (!side && !top) return {};
            if (z < 1 && x >= 18 && x < 22 && y < 9) return {};   // the door
            const bool frame = (x % 8 == 0) || (z % 8 == 0) || y == 0 || (y % 5 == 0) || x == nx - 1 || z == nz - 1;
            if (frame) return {rgb(0.9f, 0.9f, 0.86f), "timber"};
            return {(x + y + z) % 5 ? rgb(0.62f, 0.82f, 0.86f) : rgb(0.72f, 0.9f, 0.92f), "glass"};
        });
    }

    // A stone arch 16 m across and 6 m high at its crown, 3 m wide, its deck walkable from ramps of
    // earth at either end -- set on the ground, so it stands on its feet.
    inline Model bridge(vec3 at) {
        constexpr float voxel = 0.25f;
        constexpr int span = 64, wide = 12, deck = 24;   // 16 m, 3 m, 6 m
        return box("bridge", at, vec3(16.0f, 7.0f, 3.0f), voxel, [](int x, int y, int z) -> Voxel {
            // Under the deck, the arch's opening: a half ellipse 12 m wide and 4.5 m high.
            const float dx = (float(x) + 0.5f - float(span) * 0.5f) / 24.0f, dy = (float(y) + 0.5f) / 18.0f;
            if (dx * dx + dy * dy < 1.0f) return {};
            if (y >= deck + 1) {
                // A parapet on each side of the deck.
                if (z > 0 && z < wide - 1) return {};
                if (y >= deck + 4 || ((x / 4) % 2 && y >= deck + 2)) return {};
            }
            return stone(x, y, z, z == 0 || z == wide - 1 ? 0.9f : 1.0f);
        });
    }
    // Ramps up to the bridge's deck: earth, which does not break.
    inline Model ramp(const std::string& name, vec3 at, bool up) {
        return box(name, at, vec3(8.0f, 6.25f, 3.0f), 0.25f, [up](int x, int y, int z) -> Voxel {
            const int height = (up ? x + 1 : 32 - x) * 25 / 32;
            if (y >= height) return {};
            const float s = shade(x, y, z, 0.2f);
            return {y == height - 1 ? rgb(0.36f * s, 0.5f * s, 0.24f * s) : rgb(0.42f * s, 0.32f * s, 0.22f * s), ""};
        });
    }

    inline Model tree(const std::string& name, vec3 at, int seed) {
        Model m{name, at, 0.25f};
        const int c = 10;
        for (int y = 0; y < 16; y++)
            for (int z = c - 1; z <= c; z++)
                for (int x = c - 1; x <= c; x++) m.put(x, y, z, rgb(0.33f, 0.22f, 0.13f), "timber");
        for (int y = 12; y < 26; y++)
            for (int z = 0; z < 2 * c; z++)
                for (int x = 0; x < 2 * c; x++) {
                    const float dx = float(x - c) + 0.5f, dy = float(y - 19) * 1.2f, dz = float(z - c) + 0.5f;
                    const float r = 8.5f + 1.5f * std::sin(float(x * 3 + z * 5 + seed));
                    if (dx * dx + dy * dy + dz * dz > r * r) continue;
                    const float s = shade(x, y, z + seed, 0.3f);
                    m.put(x, y, z, rgb(0.2f * s, 0.46f * s, 0.16f * s), "leaves");
                }
        return m;
    }

    // Everything that breaks.
    inline std::vector<Model> structures() {
        std::vector<Model> m = castle();
        m.push_back(chimney("chimney north", vec3(30.0f, 0.0f, 8.0f), 16.0f));
        m.push_back(chimney("chimney middle", vec3(34.0f, 0.0f, 0.0f), 20.0f));
        m.push_back(chimney("chimney south", vec3(30.0f, 0.0f, -8.0f), 13.0f));
        m.push_back(brickWall("brick wall", vec3(24.0f, 0.0f, -8.0f), 18.0f));
        m.push_back(house(vec3(-34.0f, 0.0f, -3.0f)));
        m.push_back(greenhouse(vec3(-22.0f, 0.0f, -32.0f)));
        m.push_back(bridge(vec3(8.0f, 0.0f, -34.0f)));
        const vec3 trees[] = {vec3(-18, 0, 12), vec3(20, 0, 22), vec3(-24, 0, 30), vec3(-12, 0, -18), vec3(26, 0, -24), vec3(-40, 0, 18)};
        for (int i = 0; i < 6; i++) m.push_back(tree("tree " + std::to_string(i), trees[i] - vec3(2.5f, 0, 2.5f), i * 7));
        return m;
    }

    // The ground: 140 m of grass, a stone plaza in the middle and paths out of it, half a metre thick.
    // It does not break.
    inline Model ground() {
        const int n = int(GROUND_HALF * 2.0f / 0.5f);
        Model m{"ground", vec3(-GROUND_HALF, -0.5f, -GROUND_HALF), 0.5f};
        for (int z = 0; z < n; z++)
            for (int x = 0; x < n; x++) {
                const float px = float(x) * 0.5f - GROUND_HALF, pz = float(z) * 0.5f - GROUND_HALF;
                const float s = shade(x, 0, z, 0.2f);
                uint32_t colour = rgb(0.26f * s, 0.46f * s, 0.18f * s);
                const bool plaza = px * px + pz * pz < 64.0f;
                const bool path = (std::abs(px) < 1.5f && pz > 0.0f && pz < 30.0f) || (std::abs(pz) < 1.5f && std::abs(px) < 28.0f);
                if (path) colour = rgb(0.48f * s, 0.38f * s, 0.25f * s);
                if (plaza) colour = ((x / 2 + z / 2) % 2) ? rgb(0.62f * s, 0.6f * s, 0.56f * s) : rgb(0.54f * s, 0.52f * s, 0.49f * s);
                m.put(x, 0, z, colour, "");
            }
        return m;
    }
    inline std::vector<Model> scenery() {
        return {ground(), ramp("ramp west", vec3(0.0f, 0.0f, -34.0f), true), ramp("ramp east", vec3(24.0f, 0.0f, -34.0f), false)};
    }

    // ---- Small models, about a pivot -------------------------------------------------------------------

    // A player: a round little figure in its own colour, its feet on its pivot, facing +z.
    inline Model player(int index) {
        Model m{"player " + std::to_string(index), vec3(0.0f), 0.125f};
        const vec3 c = PLAYER_COLOURS[index];
        for (int y = 0; y < 14; y++)
            for (int z = 0; z < 5; z++)
                for (int x = 0; x < 5; x++) {
                    if ((x == 0 || x == 4) && (z == 0 || z == 4)) continue;
                    uint32_t colour = rgb(c.x, c.y, c.z);
                    if (y < 3) colour = rgb(0.18f, 0.18f, 0.2f);                                      // boots
                    else if (y >= 9 && y < 13) colour = rgb(0.95f, 0.82f, 0.68f);                     // face
                    else if (y >= 13) colour = rgb(c.x * 0.7f, c.y * 0.7f, c.z * 0.7f);               // a cap
                    if (y == 11 && z == 4 && (x == 1 || x == 3)) colour = rgb(0.08f, 0.08f, 0.08f);   // eyes
                    m.put(x, y, z, colour, "");
                }
        return m;
    }

    // A cannonball: iron, 0.6 m across, its pivot the middle.
    inline Model cannonball() {
        Model m{"cannonball", vec3(0.0f), 0.1f};
        for (int z = 0; z < 6; z++)
            for (int y = 0; y < 6; y++)
                for (int x = 0; x < 6; x++) {
                    if (glm::length(vec3(float(x), float(y), float(z)) + 0.5f - 3.0f) > 3.0f) continue;
                    m.put(x, y, z, (x + y + z) % 2 ? rgb(0.15f, 0.15f, 0.17f) : rgb(0.22f, 0.22f, 0.24f), "iron");
                }
        return m;
    }

    // A barrel of TNT: red, a white band, 0.75 m high. Its pivot the middle.
    inline Model tnt() {
        Model m{"tnt", vec3(0.0f), 0.125f};
        for (int y = 0; y < 6; y++)
            for (int z = 0; z < 5; z++)
                for (int x = 0; x < 5; x++) {
                    if ((x == 0 || x == 4) && (z == 0 || z == 4)) continue;
                    const uint32_t colour = (y == 2 || y == 3) ? rgb(0.95f, 0.93f, 0.88f) : rgb(0.85f, 0.12f, 0.08f);
                    m.put(x, y, z, colour, "tnt");
                }
        return m;
    }

    // A crate: a metre of planks in a frame. Its pivot the middle.
    inline Model crate() {
        Model m{"crate", vec3(0.0f), 0.125f};
        for (int y = 0; y < 8; y++)
            for (int z = 0; z < 8; z++)
                for (int x = 0; x < 8; x++) {
                    const int edges = (x == 0 || x == 7) + (y == 0 || y == 7) + (z == 0 || z == 7);
                    if (edges == 0) continue;   // hollow
                    m.put(x, y, z, edges >= 2 ? rgb(0.4f, 0.26f, 0.15f) : ((x + y + z) % 2 ? rgb(0.78f, 0.6f, 0.36f) : rgb(0.7f, 0.53f, 0.31f)),
                          "planks");
                }
        return m;
    }

    // A rubber ball, 1.2 m across, in stripes. Its pivot the middle.
    inline Model ball() {
        Model m{"ball", vec3(0.0f), 0.1f};
        for (int z = 0; z < 12; z++)
            for (int y = 0; y < 12; y++)
                for (int x = 0; x < 12; x++) {
                    const vec3 d = vec3(float(x), float(y), float(z)) + 0.5f - 6.0f;
                    if (glm::length(d) > 6.0f) continue;
                    static const vec3 stripes[3] = {vec3(0.95f, 0.3f, 0.6f), vec3(0.98f, 0.85f, 0.2f), vec3(0.2f, 0.75f, 0.95f)};
                    const vec3 c = stripes[(y / 3 + (x + z) / 6) % 3];
                    m.put(x, y, z, rgb(c.x, c.y, c.z), "rubber");
                }
        return m;
    }

    // A block someone builds: a metre of stone. Its pivot the middle of its underside.
    inline Model block() {
        Model m{"block", vec3(0.0f), 0.25f};
        for (int y = 0; y < 4; y++)
            for (int z = 0; z < 4; z++)
                for (int x = 0; x < 4; x++) {
                    const int edges = (x == 0 || x == 3) + (y == 0 || y == 3) + (z == 0 || z == 3);
                    const float s = shade(x, y, z, 0.15f);
                    m.put(x, y, z, edges >= 2 ? rgb(0.42f * s, 0.44f * s, 0.5f * s) : rgb(0.6f * s, 0.62f * s, 0.68f * s), "stone");
                }
        return m;
    }

    // ---- Writing the scene folder ---------------------------------------------------------------------

    constexpr const char* FOLDER = "playground";
    constexpr const char* MATERIALS = "physics_materials.json";
    inline std::string playerPrefab(int index) { return "player_" + std::to_string(index); }
    constexpr const char* CANNONBALL = "cannonball";
    constexpr const char* TNT = "tnt";
    constexpr const char* CRATE = "crate";
    constexpr const char* BALL = "ball";
    constexpr const char* BLOCK = "block";

    constexpr const char* MATERIAL_LIBRARY = R"({
  "v": 1,
  "materials": [
    { "name": "stone",  "density": 2200, "friction": 0.8, "restitution": 0.0,
      "strength": 30000, "fracture": "shatter",  "shardSize": 0.5 },
    { "name": "brick",  "density": 1900, "friction": 0.8, "restitution": 0.0,
      "strength": 25000, "fracture": "seams",    "shardSize": 0.4 },
    { "name": "mortar", "density": 1700, "friction": 0.8, "restitution": 0.0,
      "strength": 6000,  "fracture": "crumble",  "shardSize": 0.2 },
    { "name": "timber", "density": 650,  "friction": 0.6, "restitution": 0.1,
      "strength": 20000, "fracture": "splinter", "shardSize": 0.6, "grain": [0, 1, 0] },
    { "name": "planks", "density": 500,  "friction": 0.6, "restitution": 0.1,
      "strength": 6000,  "fracture": "splinter", "shardSize": 0.4, "grain": [1, 0, 0] },
    { "name": "leaves", "density": 200,  "friction": 0.9, "restitution": 0.0,
      "strength": 800,   "fracture": "crumble",  "shardSize": 0.6 },
    { "name": "glass",  "density": 2500, "friction": 0.4, "restitution": 0.1,
      "strength": 1500,  "fracture": "shatter",  "shardSize": 0.25 },
    { "name": "iron",   "density": 7800, "friction": 0.5, "restitution": 0.2 },
    { "name": "rubber", "density": 300,  "friction": 0.9, "restitution": 0.85 },
    { "name": "tnt",    "density": 700,  "friction": 0.7, "restitution": 0.0 }
  ]
})";

    // A model as a top-level component of `scene`, standing where its `at` says, each palette entry
    // naming its physics material.
    inline projv::ComponentHandle place(projv::Scene& scene, const Model& m) {
        using namespace projv;
        ComponentHandle h = utils::addComponent(scene, ComponentKind::Chunk, m.name, INVALID_COMPONENT_HANDLE, 16, m.voxel);
        utils::queueVoxelAdd(scene, h, m.ops);
        utils::updateScene(scene);
        utils::setComponentTransform(scene, h, m.at, quat(1, 0, 0, 0), 1.0f);
        const auto& palette = scene.components[h].materialPalette;
        for (size_t slot = 0; slot < palette.size(); slot++)
            if (auto it = m.materials.find(palette[slot].packedColor); it != m.materials.end())
                utils::setMaterialPhysics(scene, h, uint8_t(slot), it->second);
        return h;
    }

    // A prefab: the model alone in a folder, offset so `pivot` (in voxels) is the prefab's origin.
    inline bool writePrefab(const std::filesystem::path& folder, Model m, vec3 pivot) {
        projv::Scene scene;
        m.at = -pivot * m.voxel;
        place(scene, m);
        return projv::utils::saveComposeToDisk(scene, projv::INVALID_COMPONENT_HANDLE, folder.string());
    }

    // The field as one scene; and its entities.json: the ground and the ramps solid, everything else
    // solid, breakable, falling when cut loose, and named by a NetId so its changes reach every client.
    inline bool writeField(const std::filesystem::path& folder) {
        struct Entry { Model model; bool breaks; };
        std::vector<Entry> entries;
        for (Model& m : scenery()) entries.push_back({std::move(m), false});
        for (Model& m : structures()) entries.push_back({std::move(m), true});

        projv::Scene scene;
        for (const Entry& e : entries) place(scene, e.model);
        if (!projv::utils::saveComposeToDisk(scene, projv::INVALID_COMPONENT_HANDLE, folder.string())) return false;
        const projv::Scene saved = projv::utils::loadComposeFromDisk(folder.string());
        std::ofstream out(folder / "entities.json");
        out << "{ \"version\": 1, \"entities\": [\n";
        for (size_t i = 0; i < entries.size(); i++) {
            const std::string components = entries[i].breaks
                ? R"({ "physics.static": { "v": 1, "splits": true, "anchorBase": true, "breaksOnImpact": true }, "projv.netid": { "v": 1, "id": )" +
                      std::to_string(10 + i) + " } }"
                : std::string(R"({ "physics.static": { "v": 1 } })");
            out << "  { \"name\": \"" << entries[i].model.name << "\", \"link\": " << saved.components[i].localId
                << ", \"components\": " << components << " }" << (i + 1 < entries.size() ? ",\n" : "\n");
        }
        out << "] }\n";
        return bool(out);
    }

    inline bool write(const std::string& where) {
        const std::filesystem::path folder(where);
        std::filesystem::create_directories(folder);
        if (!writeField(folder)) return false;
        std::ofstream(folder / MATERIALS) << MATERIAL_LIBRARY;
        bool ok = true;
        for (int i = 0; i < MAX_PLAYERS; i++) ok = ok && writePrefab(folder / playerPrefab(i), player(i), vec3(2.5f, 0.0f, 2.5f));
        ok = ok && writePrefab(folder / CANNONBALL, cannonball(), vec3(3.0f));
        ok = ok && writePrefab(folder / TNT, tnt(), vec3(2.5f, 3.0f, 2.5f));
        ok = ok && writePrefab(folder / CRATE, crate(), vec3(4.0f));
        ok = ok && writePrefab(folder / BALL, ball(), vec3(6.0f));
        ok = ok && writePrefab(folder / BLOCK, block(), vec3(2.0f, 0.0f, 2.0f));
        return ok;
    }
}
