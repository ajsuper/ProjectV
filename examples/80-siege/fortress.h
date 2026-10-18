// The siege field and everything built on it: two fortresses, their cannons and flags, the players,
// and what they throw. Every model is made voxel by voxel here, each colour naming the physics
// material it is made of -- nothing is read from disk, so a round's fortresses are built afresh
// from these each time it starts.
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

namespace siege {
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

    enum Team : uint8_t { Blue = 0, Red = 1 };
    inline Team other(Team t) { return t == Blue ? Red : Blue; }
    inline const char* teamName(Team t) { return t == Blue ? "BLUE" : "RED"; }

    // ---- Where things are ----------------------------------------------------------------------------
    //
    // The field runs along x. Each fortress has a frame of its own: u toward the enemy, v across, in
    // metres from the middle of its courtyard on the ground. Blue's stands at x = -40 facing +x, Red's
    // at +40 facing -x; Red's is Blue's mirrored, so both are built from the same description.
    //
    //   u = -11 .. -9    back wall          u = 9 .. 11     front wall, a gate in its middle
    //   u =  -9 .. -4    the keep, 13 m, the flag on its roof
    //   u = 7.5 .. 9     two stairs up to the front wall's walk, rising from beside the gate
    //   u = 10, v = ±5   two cannons on the walk, each in a gap in the parapet
    constexpr float FORTRESS_X = 40.0f;
    constexpr float WALL_HEIGHT = 5.0f;     // the walk on top of the walls
    constexpr float KEEP_HEIGHT = 13.0f;    // the keep's roof, where the flag stands
    constexpr float CANNON_U = 10.0f, CANNON_V = 5.0f, CANNON_PIVOT = WALL_HEIGHT + 0.7f;

    struct Frame {
        Team  team = Blue;
        float centreX = -FORTRESS_X;
        float facing = 1.0f;   // +1: toward +x

        static Frame of(Team t) { return t == Blue ? Frame{Blue, -FORTRESS_X, 1.0f} : Frame{Red, FORTRESS_X, -1.0f}; }
        vec3 at(float u, float y, float v) const { return {centreX + facing * u, y, v}; }
        // The yaw (direction (cos yaw, 0, sin yaw)) that looks toward the enemy.
        float yaw() const { return facing > 0.0f ? 0.0f : 3.14159265f; }
        vec3 forward() const { return {facing, 0.0f, 0.0f}; }

        vec3 spawn() const { return at(-2.5f, 0.05f, 0.0f); }            // in front of the keep's door
        vec3 flagHome() const { return at(-6.5f, KEEP_HEIGHT + 0.02f, 0.0f); }
        vec3 cannonPivot(int side) const { return at(CANNON_U, CANNON_PIVOT, side ? CANNON_V : -CANNON_V); }
        vec3 cannonSeat(int side) const { return at(CANNON_U - 0.9f, WALL_HEIGHT + 0.05f, side ? CANNON_V : -CANNON_V); }
        vec3 gateInside() const { return at(6.5f, 0.0f, 0.0f); }
        vec3 gateOutside() const { return at(13.5f, 0.0f, 0.0f); }
    };

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
    using Fill = std::function<Voxel(int u, int y, int v)>;

    // A box of the fortress, u0..u1 by y0..y1 by v0..v1 metres in its frame, filled voxel by voxel by
    // `fill` (indices from the box's own corner, u from u0). Mirrored for a fortress facing -x, so
    // `fill` always sees u running toward the enemy.
    inline Model box(const Frame& f, const std::string& name, float u0, float u1, float y0, float y1, float v0, float v1,
                     float voxel, const Fill& fill) {
        const int nu = int(std::lround((u1 - u0) / voxel)), ny = int(std::lround((y1 - y0) / voxel)),
                  nv = int(std::lround((v1 - v0) / voxel));
        Model m{name + (f.team == Blue ? " (blue)" : " (red)"), vec3(0.0f), voxel};
        m.at = vec3(f.facing > 0.0f ? f.centreX + u0 : f.centreX - u1, y0, v0);
        for (int y = 0; y < ny; y++)
            for (int v = 0; v < nv; v++)
                for (int u = 0; u < nu; u++) {
                    const Voxel voxelHere = fill(u, y, v);
                    if (!voxelHere.colour) continue;
                    m.put(f.facing > 0.0f ? u : nu - 1 - u, y, v, voxelHere.colour, voxelHere.material);
                }
        return m;
    }

    inline Voxel stone(int x, int y, int z, float darker = 1.0f) {
        const float s = shade(x / 2, y / 2, z / 2, 0.24f) * darker;
        return {y % 8 == 0 ? rgb(0.26f * s, 0.25f * s, 0.23f * s) : rgb(0.34f * s, 0.32f * s, 0.29f * s), "stone"};
    }

    // A curtain wall, `thick` metres deep with the walk on top, and a parapet along its outer face
    // `outerAtHigh`: crenellated, with a gap where each of `embrasures` (v, metres) has a cannon.
    // `gate`: a doorway through its middle.
    inline Model curtain(const Frame& f, const std::string& name, float u0, float u1, float v0, float v1, bool alongV,
                         bool outerAtHigh, bool gate, std::vector<float> embrasures) {
        constexpr float voxel = 0.25f;
        const int walk = int(WALL_HEIGHT / voxel), parapet = walk + 4;
        return box(f, name, u0, u1, 0.0f, WALL_HEIGHT + 1.0f, v0, v1, voxel, [=](int u, int y, int v) -> Voxel {
            // Across the wall and along it, whichever way it runs.
            const int across = alongV ? u : v, along = alongV ? v : u;
            const int depth = int(std::lround(((alongV ? u1 - u0 : v1 - v0)) / voxel));
            const float alongMetres = (alongV ? v0 : u0) + (float(along) + 0.5f) * voxel;
            if (gate && std::abs(alongMetres) < 1.5f && y < 14) return {};
            if (y >= walk) {
                const bool outer = outerAtHigh ? across >= depth - 2 : across < 2;
                if (!outer) return {};
                for (float e : embrasures)
                    if (std::abs(alongMetres - e) < 0.75f) return {};
                if (y >= walk + 2 && (along / 4) % 2) return {};   // crenels
                if (y >= parapet) return {};
            }
            return stone(u, y, v);
        });
    }

    // A corner tower, 3.5 m square and 8 m high with battlements: solid stone.
    inline Model tower(const Frame& f, const std::string& name, float u0, float v0) {
        return box(f, name, u0, u0 + 3.5f, 0.0f, 9.0f, v0, v0 + 3.5f, 0.25f, [](int u, int y, int v) -> Voxel {
            if (y >= 32) {
                const bool rim = u < 2 || v < 2 || u >= 12 || v >= 12;
                if (!rim || ((u + v) / 3) % 2) return {};
            }
            return stone(u, y, v, 0.92f);
        });
    }

    // The keep, 5 m square and 13 m to its roof: hollow, a door facing the courtyard, slit windows,
    // a roof the flag stands on and battlements round it. Bring it down and the flag comes with it.
    inline Model keep(const Frame& f) {
        return box(f, "keep", -9.0f, -4.0f, 0.0f, KEEP_HEIGHT + 1.0f, -2.5f, 2.5f, 0.25f, [](int u, int y, int v) -> Voxel {
            const int side = 20, roof = 52;   // the roof's top: 13 m
            const bool shell = u < 3 || v < 3 || u >= side - 3 || v >= side - 3;
            if (y >= roof + 4) return {};
            if (y >= roof) {   // battlements on the roof's edge
                const bool rim = u < 2 || v < 2 || u >= side - 2 || v >= side - 2;
                if (!rim || ((u + v) / 3) % 2) return {};
                return stone(u, y, v, 0.85f);
            }
            if (y >= roof - 3) return stone(u, y, v, 0.85f);   // the roof
            if (!shell) return {};
            if (u >= side - 3 && v >= 7 && v < 13 && y < 11) return {};                                 // the door
            if ((y % 16) >= 8 && (y % 16) < 13 && ((v >= 9 && v < 11) || (u >= 9 && u < 11))) return {};   // slits
            return stone(u, y, v, 0.85f);
        });
    }

    // Stairs up to the front wall's walk, against its inside face: a step of a quarter metre every
    // quarter metre, from beside the gate (v = ±2), open to the courtyard, up to its corner (v = ±8.5).
    inline Model stairs(const Frame& f, int side) {
        const float v0 = side ? 2.0f : -8.5f, v1 = side ? 8.5f : -2.0f;
        return box(f, side ? "stairs right" : "stairs left", 7.5f, 9.0f, 0.0f, WALL_HEIGHT, v0, v1, 0.25f,
                   [side](int u, int y, int v) -> Voxel {
                       const int run = 26;
                       const int fromBottom = side ? v : run - 1 - v;   // the low end is by the gate
                       const int height = (fromBottom + 1) * 20 / run;
                       if (y >= height) return {};
                       return stone(u, y, v, y == height - 1 ? 1.08f : 0.95f);
                   });
    }

    inline std::vector<Model> fortress(Team team) {
        const Frame f = Frame::of(team);
        std::vector<Model> models;
        models.push_back(curtain(f, "front wall", 9.0f, 11.0f, -8.5f, 8.5f, true, true, true, {-CANNON_V, CANNON_V}));
        models.push_back(curtain(f, "back wall", -11.0f, -9.0f, -8.5f, 8.5f, true, false, false, {}));
        models.push_back(curtain(f, "left wall", -8.5f, 8.5f, -11.0f, -9.0f, false, false, false, {}));
        models.push_back(curtain(f, "right wall", -8.5f, 8.5f, 9.0f, 11.0f, false, true, false, {}));
        models.push_back(tower(f, "tower front left", 8.5f, -12.0f));
        models.push_back(tower(f, "tower front right", 8.5f, 8.5f));
        models.push_back(tower(f, "tower back left", -12.0f, -12.0f));
        models.push_back(tower(f, "tower back right", -12.0f, 8.5f));
        models.push_back(keep(f));
        models.push_back(stairs(f, 0));
        models.push_back(stairs(f, 1));
        return models;
    }

    // Cover in the field between the fortresses: low brick walls and a few trees.
    inline Model brickWall(const std::string& name, vec3 at, int length, bool alongZ) {
        Model m{name, at, 0.125f};
        for (int y = 0; y < 12; y++)
            for (int w = 0; w < 4; w++)
                for (int l = 0; l < length; l++) {
                    const int course = y / 3, shifted = l + (course % 2) * 3;
                    uint32_t colour;
                    const char* material;
                    if (y % 3 == 2 || shifted % 6 == 5) {
                        colour = rgb(0.72f, 0.7f, 0.64f);
                        material = "mortar";
                    } else {
                        static const float reds[3][3] = {{0.56f, 0.23f, 0.15f}, {0.64f, 0.29f, 0.18f}, {0.48f, 0.2f, 0.14f}};
                        const int which = (course * 7 + shifted / 6 * 3) % 3;
                        colour = rgb(reds[which][0], reds[which][1], reds[which][2]);
                        material = "brick";
                    }
                    if (alongZ) m.put(w, y, l, colour, material);
                    else m.put(l, y, w, colour, material);
                }
        return m;
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

    inline std::vector<Model> field() {
        std::vector<Model> models;
        models.push_back(brickWall("cover centre", vec3(-0.25f, 0.0f, -4.0f), 64, true));
        models.push_back(brickWall("cover north", vec3(-14.0f, 0.0f, 14.0f), 56, false));
        models.push_back(brickWall("cover south", vec3(7.0f, 0.0f, -18.0f), 56, false));
        const vec3 trees[] = {vec3(-20, 0, -24), vec3(18, 0, 24), vec3(-4, 0, 26), vec3(6, 0, -28), vec3(-26, 0, 22),
                              vec3(26, 0, -22)};
        for (int i = 0; i < 6; i++) models.push_back(tree("tree" + std::to_string(i), trees[i] - vec3(2.5f, 0, 2.5f), i * 7));
        return models;
    }

    // The ground: 150 by 90 m of grass, a dirt road between the gates, half a metre thick. It does
    // not break.
    inline Model ground() {
        Model m{"ground", vec3(-75.0f, -0.5f, -45.0f), 0.5f};
        for (int z = 0; z < 180; z++)
            for (int x = 0; x < 300; x++) {
                const float px = float(x) * 0.5f - 75.0f, pz = float(z) * 0.5f - 45.0f;
                const bool road = std::abs(pz - 3.0f * std::sin(px * 0.07f) * (1.0f - std::min(std::abs(px) / 40.0f, 1.0f))) < 2.0f &&
                                  std::abs(px) < 52.0f;
                const bool yard = std::abs(std::abs(px) - FORTRESS_X) < 11.0f && std::abs(pz) < 11.0f;
                const float s = shade(x, 0, z, 0.2f);
                uint32_t colour = rgb(0.26f * s, 0.46f * s, 0.18f * s);
                if (road) colour = rgb(0.48f * s, 0.38f * s, 0.25f * s);
                if (yard) colour = rgb(0.3f * s, 0.28f * s, 0.24f * s);
                m.put(x, 0, z, colour, "");
            }
        return m;
    }

    // ---- Small models, turned about a pivot ------------------------------------------------------------

    // A soldier in a coat of the team's colour, its feet on its pivot, facing +z.
    inline Model avatar(Team team) {
        Model m{team == Blue ? "blue soldier" : "red soldier", vec3(0.0f), 0.125f};
        const uint32_t coat = team == Blue ? rgb(0.2f, 0.36f, 0.9f) : rgb(0.85f, 0.2f, 0.16f);
        const uint32_t trim = team == Blue ? rgb(0.12f, 0.2f, 0.55f) : rgb(0.5f, 0.1f, 0.08f);
        for (int y = 0; y < 15; y++)
            for (int z = 0; z < 4; z++)
                for (int x = 0; x < 4; x++) {
                    uint32_t colour = y < 6 ? rgb(0.2f, 0.2f, 0.24f) : y < 11 ? (y == 7 ? trim : coat) : rgb(0.95f, 0.8f, 0.65f);
                    if (y >= 14) colour = trim;                                               // a helmet
                    if (y == 12 && z == 3 && (x == 1 || x == 2)) colour = rgb(0.1f, 0.1f, 0.1f);   // eyes
                    m.put(x, y, z, colour, "");
                }
        return m;
    }

    // A flag: an iron foot, a timber pole 3.75 m high, and a cloth in the team's colour. Its pivot is
    // the middle of its foot; the cloth hangs across x, so it faces the enemy.
    inline Model flag(Team team) {
        Model m{team == Blue ? "blue flag" : "red flag", vec3(0.0f), 0.125f};
        const uint32_t cloth = team == Blue ? rgb(0.15f, 0.3f, 0.95f) : rgb(0.95f, 0.15f, 0.12f);
        const uint32_t stripe = rgb(0.95f, 0.9f, 0.75f);
        for (int z = 0; z < 6; z++)
            for (int x = 0; x < 6; x++) m.put(x, 0, z, rgb(0.25f, 0.25f, 0.27f), "iron");
        for (int y = 1; y < 31; y++)
            for (int z = 2; z < 4; z++)
                for (int x = 2; x < 4; x++) m.put(x, y, z, rgb(0.4f, 0.27f, 0.15f), "timber");
        for (int y = 20; y < 30; y++)
            for (int z = 4; z < 18; z++) m.put(2, y, z, (y == 24 || y == 25) ? stripe : cloth, "cloth");
        return m;
    }

    // A cannon's carriage, its pivot the middle of its top, the barrel's end at +x.
    inline Model carriage() {
        Model m{"carriage", vec3(0.0f), 0.125f};
        for (int y = 0; y < 4; y++)
            for (int z = 1; z < 9; z++)
                for (int x = 0; x < 10; x++) {
                    const bool cheek = z < 3 || z >= 7;
                    if (!cheek && y > 1) continue;
                    m.put(x, y, z, (x + y) % 3 ? rgb(0.45f, 0.3f, 0.17f) : rgb(0.38f, 0.25f, 0.14f), "planks");
                }
        // Wheels, either side.
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 10; x++)
                for (int z : {0, 9}) {
                    const float dx = float(x) - 6.5f, dy = float(y) - 1.5f;
                    if (dx * dx + dy * dy < 7.0f) m.put(x, y, z, rgb(0.22f, 0.15f, 0.09f), "planks");
                }
        return m;
    }

    // A cannon's barrel along +x: iron, 1.75 m, its pivot (the trunnions) a third of the way along.
    inline Model barrel() {
        Model m{"barrel", vec3(0.0f), 0.125f};
        for (int x = 0; x < 14; x++)
            for (int y = 0; y < 5; y++)
                for (int z = 0; z < 5; z++) {
                    const float dy = float(y) - 2.0f, dz = float(z) - 2.0f;
                    const float r = x < 2 ? 2.6f : (x >= 12 ? 2.4f : 2.2f - float(x) * 0.03f);
                    if (dy * dy + dz * dz > r * r) continue;
                    if (x >= 11 && dy * dy + dz * dz < 1.0f) continue;   // the bore
                    m.put(x, y, z, (x == 1 || x == 12) ? rgb(0.18f, 0.18f, 0.2f) : rgb(0.28f, 0.28f, 0.31f), "iron");
                }
        return m;
    }

    // A cannonball: iron, 0.6 m across, its pivot the middle.
    inline Model cannonball() {
        Model m{"cannonball", vec3(0.0f), 0.1f};
        for (int z = 0; z < 6; z++)
            for (int y = 0; y < 6; y++)
                for (int x = 0; x < 6; x++) {
                    const vec3 d = vec3(float(x), float(y), float(z)) + 0.5f - 3.0f;
                    if (glm::length(d) > 3.0f) continue;
                    m.put(x, y, z, (x + y + z) % 2 ? rgb(0.15f, 0.15f, 0.17f) : rgb(0.22f, 0.22f, 0.24f), "iron");
                }
        return m;
    }

    // A placed block: a metre of planks in a frame.
    inline Model block() {
        Model m{"block", vec3(0.0f), 0.25f};
        for (int y = 0; y < 4; y++)
            for (int z = 0; z < 4; z++)
                for (int x = 0; x < 4; x++) {
                    const int edges = (x == 0 || x == 3) + (y == 0 || y == 3) + (z == 0 || z == 3);
                    m.put(x, y, z, edges >= 2 ? rgb(0.4f, 0.26f, 0.15f) : ((x + z) % 2 ? rgb(0.74f, 0.58f, 0.36f) : rgb(0.68f, 0.52f, 0.32f)),
                          "planks");
                }
        return m;
    }

    // A holding pen under the middle of the field, where the dead wait for their respawn: a stone
    // floor and walls, 10 m across, 12 m down, out of everyone's sight. It does not break.
    inline Model pen() {
        Model m{"pen", vec3(-5.0f, -14.0f, -5.0f), 0.5f};
        for (int y = 0; y < 6; y++)
            for (int z = 0; z < 20; z++)
                for (int x = 0; x < 20; x++) {
                    const bool wall = x == 0 || z == 0 || x == 19 || z == 19;
                    if (y == 0 || wall) m.put(x, y, z, rgb(0.3f, 0.3f, 0.32f), "");
                }
        return m;
    }
    constexpr float PEN_FLOOR = -13.5f;

    // ---- Writing the scene folder ---------------------------------------------------------------------
    //
    // `siege_server --write-assets <dir>` writes it: the server and every client load the same files
    // (the handshake checks), so this is run once and the folder shipped -- examples/80-siege/siege.

    constexpr const char* FOLDER = "siege";
    constexpr const char* MATERIALS = "physics_materials.json";
    // The prefabs inside it: what the server makes during a game, sent to every client by name.
    constexpr const char* SOLDIER[2] = {"blue_soldier", "red_soldier"};
    constexpr const char* FLAG[2] = {"blue_flag", "red_flag"};
    constexpr const char* CARRIAGE = "carriage";
    constexpr const char* BARREL = "barrel";
    constexpr const char* CANNONBALL = "cannonball";
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
    { "name": "iron",   "density": 4000, "friction": 0.5, "restitution": 0.2 },
    { "name": "cloth",  "density": 80,   "friction": 0.8, "restitution": 0.0 }
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

    // A prefab: the model alone in a folder, offset so `pivot` (in voxels) is the prefab's origin --
    // a soldier's feet, a barrel's trunnions. Turning the instance turns it about its pivot.
    inline bool writePrefab(const std::filesystem::path& folder, Model m, vec3 pivot) {
        projv::Scene scene;
        m.at = -pivot * m.voxel;
        place(scene, m);
        return projv::utils::saveComposeToDisk(scene, projv::INVALID_COMPONENT_HANDLE, folder.string());
    }

    // The field, both fortresses, the cover between them and the pen, as one scene; and its
    // entities.json: the ground and the pen solid, everything else solid, breakable, falling when
    // cut loose, and named by a NetId so its breaks reach every client.
    inline bool writeField(const std::filesystem::path& folder) {
        struct Entry { Model model; bool breaks; };
        std::vector<Entry> entries{{ground(), false}, {pen(), false}};
        for (Team team : {Blue, Red})
            for (Model& m : fortress(team)) entries.push_back({std::move(m), true});
        for (Model& m : field()) entries.push_back({std::move(m), true});

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
        for (Team team : {Blue, Red}) {
            ok = ok && writePrefab(folder / SOLDIER[team], avatar(team), vec3(2.0f, 0.0f, 2.0f));
            ok = ok && writePrefab(folder / FLAG[team], flag(team), vec3(3.0f, 0.0f, 3.0f));
        }
        ok = ok && writePrefab(folder / CARRIAGE, carriage(), vec3(5.0f, 4.0f, 5.0f));
        ok = ok && writePrefab(folder / BARREL, barrel(), vec3(4.5f, 2.5f, 2.5f));
        ok = ok && writePrefab(folder / CANNONBALL, cannonball(), vec3(3.0f));
        ok = ok && writePrefab(folder / BLOCK, block(), vec3(2.0f, 0.0f, 2.0f));
        return ok;
    }
}
