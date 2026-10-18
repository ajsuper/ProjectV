// What the server and the client share: the arena they both load, and how to make it.
#pragma once

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "runtime/entities.h"
#include "data_structures/scene.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/scene_query.h"

namespace arena {
    constexpr uint16_t PORT = 27015;
    constexpr const char* FOLDER = "arena";        // beside each binary: the same files on both
    constexpr const char* AVATAR = "avatar";       // prefabs inside it
    constexpr const char* BALL = "ball";
    constexpr const char* MATERIALS = "physics_materials.json";

    using projv::core::ivec3;
    using projv::core::vec3;

    inline uint32_t rgb(float r, float g, float b) { return projv::packRGB10(r, g, b); }

    // On a thrown ball: who threw it, so the thrower's client can swap its own stand-in for it.
    struct Thrower {
        uint16_t client = 0;
    };

    // A model being built: voxels, each colour naming the physics material it is made of.
    struct Model {
        std::string name;
        vec3        at{0.0f};            // where its voxel (0, 0, 0) corner stands
        float       voxel = 0.25f;
        std::string components;          // its entity's, as entities.json writes them
        std::vector<projv::PendingVoxelOp> ops;
        std::map<uint32_t, std::string> materials;   // colour -> physics material

        void put(int x, int y, int z, uint32_t colour, const std::string& material) {
            ops.push_back({true, ivec3(x, y, z), colour});
            if (!material.empty()) materials[colour] = material;
        }
    };

    // Small changes of shade by position, so a surface reads as made of something.
    inline float shade(int x, int y, int z, float amount) {
        // Unsigned arithmetic: a signed multiply that overflows is undefined, and the optimiser
        // takes that as licence to never leave the loop calling this.
        uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u;
        return 1.0f - amount + amount * float(h % 4) / 3.0f;   // four levels: a palette holds 255
    }

    // ---- The models ------------------------------------------------------------------------------

    // A stone watchtower: hollow, 6 m square, 10 m high with battlements, a doorway and windows.
    // Stone shatters.
    inline Model tower(vec3 at) {
        Model m{"tower", at, 0.25f, R"({ "physics.static": { "v": 1, "splits": true, "anchorBase": true, "breaksOnImpact": true } })"};
        const int side = 24, height = 40, wall = 3;
        for (int y = 0; y < height + 4; y++)
            for (int z = 0; z < side; z++)
                for (int x = 0; x < side; x++) {
                    const bool edge = x < wall || z < wall || x >= side - wall || z >= side - wall;
                    if (!edge) continue;
                    if (y >= height) {   // battlements: every other block of the parapet
                        const int along = (x < wall || x >= side - wall) ? z : x;
                        if ((along / 4) % 2) continue;
                    }
                    if (z < wall && x >= 10 && x < 14 && y < 10) continue;                            // doorway
                    if ((x < wall || x >= side - wall) && z >= 10 && z < 14 && y >= 22 && y < 28) continue;   // windows
                    const float s = shade(x / 2, y, z / 2, 0.18f);
                    const bool band = y % 8 == 0;
                    m.put(x, y, z, band ? rgb(0.52f * s, 0.5f * s, 0.47f * s) : rgb(0.62f * s, 0.6f * s, 0.56f * s), "stone");
                }
        return m;
    }

    // A brick wall, 14 m long and 3 m high, bricks laid in mortar: the bricks come out whole.
    inline Model wall(vec3 at) {
        Model m{"wall", at, 0.125f, R"({ "physics.static": { "v": 1, "splits": true, "anchorBase": true, "breaksOnImpact": true } })"};
        for (int y = 0; y < 24; y++)
            for (int z = 0; z < 4; z++)
                for (int x = 0; x < 112; x++) {
                    const int course = y / 3, shifted = x + (course % 2) * 3;
                    if (y % 3 == 2 || shifted % 6 == 5) {
                        m.put(x, y, z, rgb(0.72f, 0.7f, 0.64f), "mortar");
                        continue;
                    }
                    const int which = (course * 7 + shifted / 6 * 3) % 3;
                    static const float reds[3][3] = {{0.56f, 0.23f, 0.15f}, {0.64f, 0.29f, 0.18f}, {0.48f, 0.2f, 0.14f}};
                    m.put(x, y, z, rgb(reds[which][0], reds[which][1], reds[which][2]), "brick");
                }
        return m;
    }

    // A timber house, 8 x 6 m: plank walls between timber posts, a door, windows, and a tiled gable
    // roof. Planks splinter, the roof crumbles.
    inline Model house(vec3 at) {
        Model m{"house", at, 0.25f, R"({ "physics.static": { "v": 1, "splits": true, "anchorBase": true, "breaksOnImpact": true } })"};
        const int w = 32, d = 24, h = 12;
        for (int y = 0; y < h; y++)
            for (int z = 0; z < d; z++)
                for (int x = 0; x < w; x++) {
                    const bool edge = x == 0 || z == 0 || x == w - 1 || z == d - 1;
                    if (!edge) continue;
                    const bool post = (x == 0 || x == w - 1) && (z == 0 || z == d - 1 || z == d / 2);
                    const bool beam = y == h - 1;
                    if (z == 0 && x >= 14 && x < 18 && y < 9) continue;                                   // door
                    if (!post && !beam && y >= 4 && y < 8 && ((x % 10 >= 3 && x % 10 < 7 && (z == 0 || z == d - 1)) ||
                                                               (z % 12 >= 4 && z % 12 < 8 && (x == 0 || x == w - 1))))
                        continue;                                                                          // windows
                    if (post || beam) m.put(x, y, z, rgb(0.36f, 0.24f, 0.14f), "timber");
                    else {
                        const float s = shade(x, y / 2, z, 0.12f);
                        m.put(x, y, z, (y / 2) % 2 ? rgb(0.66f * s, 0.5f * s, 0.32f * s) : rgb(0.6f * s, 0.45f * s, 0.29f * s), "planks");
                    }
                }
        // The roof: a gable along x, a row of tiles overhanging each side.
        for (int step = 0; step <= d / 2 + 1; step++)
            for (int x = -2; x < w + 2; x++)
                for (int side = 0; side < 2; side++) {
                    const int z = side ? d - step : step - 1;
                    const int y = h + step;
                    const bool ridge = step >= d / 2;
                    m.put(x, y, z, ridge ? rgb(0.5f, 0.18f, 0.12f) : ((x + step) % 2 ? rgb(0.62f, 0.25f, 0.16f) : rgb(0.68f, 0.3f, 0.19f)), "tile");
                }
        // Gable ends.
        for (int step = 0; step < d / 2; step++)
            for (int z = step; z < d - step; z++)
                for (int x : {0, w - 1}) m.put(x, h + step, z, rgb(0.6f, 0.45f, 0.29f), "planks");
        return m;
    }

    // A tree: a timber trunk and a ragged crown of leaves.
    inline Model tree(const std::string& name, vec3 at, int seed) {
        Model m{name, at, 0.25f, R"({ "physics.static": { "v": 1, "splits": true, "anchorBase": true, "breaksOnImpact": true } })"};
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

    // A 1 m wooden crate, boards and a frame: it moves, and breaks when hit hard.
    inline Model crate(const std::string& name, vec3 at, int netId) {
        Model m{name, at, 0.125f,
                R"({ "physics.rigidbody": { "v": 1, "material": "planks", "breaksOnImpact": true }, "projv.netid": { "v": 1, "id": )" +
                    std::to_string(netId) + " } }"};
        for (int y = 0; y < 8; y++)
            for (int z = 0; z < 8; z++)
                for (int x = 0; x < 8; x++) {
                    const int edges = (x == 0 || x == 7) + (y == 0 || y == 7) + (z == 0 || z == 7);
                    if (edges >= 2) m.put(x, y, z, rgb(0.42f, 0.28f, 0.16f), "planks");
                    else m.put(x, y, z, (y % 2) ? rgb(0.74f, 0.58f, 0.36f) : rgb(0.68f, 0.52f, 0.32f), "planks");
                }
        return m;
    }

    // The ground: 64 m of grass with a dirt path, half a metre thick. It does not break.
    inline Model ground() {
        Model m{"ground", vec3(-32.0f, -0.5f, -32.0f), 0.5f, R"({ "physics.static": { "v": 1 } })"};
        for (int z = 0; z < 128; z++)
            for (int x = 0; x < 128; x++) {
                const float px = float(x) - 64.0f, pz = float(z) - 64.0f;
                const bool path = std::abs(px - 6.0f * std::sin(pz * 0.08f)) < 3.0f;
                const float s = shade(x, 0, z, 0.2f);
                m.put(x, 0, z, path ? rgb(0.48f * s, 0.38f * s, 0.25f * s) : rgb(0.26f * s, 0.46f * s, 0.18f * s), "");
            }
        return m;
    }

    // ---- Writing it ------------------------------------------------------------------------------

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
    { "name": "tile",   "density": 1800, "friction": 0.7, "restitution": 0.0,
      "strength": 5000,  "fracture": "crumble",  "shardSize": 0.35 },
    { "name": "leaves", "density": 200,  "friction": 0.9, "restitution": 0.0,
      "strength": 800,   "fracture": "crumble",  "shardSize": 0.6 },
    { "name": "iron",   "density": 4000, "friction": 0.5, "restitution": 0.2 }
  ]
})";

    // Builds a model as a component of `scene`, each palette entry naming its physics material.
    inline projv::ComponentHandle place(projv::Scene& scene, const Model& m) {
        using namespace projv;
        ComponentHandle h = utils::addComponent(scene, ComponentKind::Chunk, m.name, INVALID_COMPONENT_HANDLE, 16, m.voxel);
        utils::queueVoxelAdd(scene, h, m.ops);
        utils::updateScene(scene);
        utils::setComponentTransform(scene, h, m.at, core::quat(1, 0, 0, 0), 1.0f);
        const auto& palette = scene.components[h].materialPalette;
        for (size_t slot = 0; slot < palette.size(); slot++)
            if (auto it = m.materials.find(palette[slot].packedColor); it != m.materials.end())
                utils::setMaterialPhysics(scene, h, uint8_t(slot), it->second);
        return h;
    }

    // Writes a folder of models, and its entities.json: each model's entity linked to it, with its
    // components and, for those that break or move, a NetId.
    inline bool writeFolder(const std::string& folder, const std::vector<Model>& models, int firstNetId) {
        projv::Scene scene;
        for (const Model& m : models) place(scene, m);
        if (!projv::utils::saveComposeToDisk(scene, projv::INVALID_COMPONENT_HANDLE, folder)) return false;
        const projv::Scene saved = projv::utils::loadComposeFromDisk(folder);
        std::ofstream out(std::filesystem::path(folder) / "entities.json");
        out << "{ \"version\": 1, \"entities\": [\n";
        int netId = firstNetId;
        for (size_t i = 0; i < models.size(); i++) {
            std::string components = models[i].components;
            // Anything that breaks needs a NetId, so its breaks can be named on every machine.
            if (components.find("projv.netid") == std::string::npos && components.find("splits") != std::string::npos)
                components.insert(components.size() - 1, ", \"projv.netid\": { \"v\": 1, \"id\": " + std::to_string(netId++) + " } ");
            out << "  { \"name\": \"" << models[i].name << "\", \"link\": " << saved.components[i].localId
                << ", \"components\": " << components << " }" << (i + 1 < models.size() ? ",\n" : "\n");
        }
        out << "] }\n";
        return bool(out);
    }

    inline bool write(const std::string& folder) {
        std::vector<Model> models{ground(), tower(vec3(12.0f, 0.0f, 10.0f)), wall(vec3(-17.0f, 0.0f, 9.0f)),
                                  house(vec3(-20.0f, 0.0f, -20.0f))};
        const vec3 trees[] = {vec3(20, 0, -18), vec3(4, 0, -24), vec3(-26, 0, 20), vec3(24, 0, 24), vec3(-4, 0, 22)};
        for (int i = 0; i < 5; i++) models.push_back(tree("tree" + std::to_string(i), trees[i] - vec3(2.5f, 0, 2.5f), i * 7));
        // Two stacks of crates: a pyramid of six and a tower of four.
        int crateId = 100;
        for (int row = 0, n = 0; row < 3; row++)
            for (int i = 0; i < 3 - row; i++, n++)
                models.push_back(crate("crate" + std::to_string(n), vec3(4.0f + float(i) * 1.02f + float(row) * 0.51f, float(row), -10.0f), crateId++));
        for (int k = 0; k < 4; k++) models.push_back(crate("stack" + std::to_string(k), vec3(-6.0f, float(k), -6.0f), crateId++));
        if (!writeFolder(folder, models, 10)) return false;
        std::ofstream(std::filesystem::path(folder) / MATERIALS) << MATERIAL_LIBRARY;

        // The avatar: a figure in a blue coat, centred on its feet.
        {
            projv::Scene avatar;
            Model a{"body", vec3(-0.25f, 0.0f, -0.25f), 0.125f, ""};
            for (int y = 0; y < 14; y++)
                for (int z = 0; z < 4; z++)
                    for (int x = 0; x < 4; x++) {
                        uint32_t colour = y < 6 ? rgb(0.2f, 0.22f, 0.3f) : y < 11 ? rgb(0.22f, 0.4f, 0.85f) : rgb(0.95f, 0.8f, 0.65f);
                        if (y >= 11 && z == 3 && (x == 1 || x == 2) && y == 12) colour = rgb(0.1f, 0.1f, 0.1f);   // eyes
                        a.put(x, y, z, colour, "");
                    }
            place(avatar, a);
            if (!projv::utils::saveComposeToDisk(avatar, projv::INVALID_COMPONENT_HANDLE, (std::filesystem::path(folder) / AVATAR).string()))
                return false;
        }
        // The ball the players throw: iron, 0.6 m across, centred.
        {
            projv::Scene ball;
            Model b{"ball", vec3(-0.3f), 0.1f, ""};
            for (int z = 0; z < 6; z++)
                for (int y = 0; y < 6; y++)
                    for (int x = 0; x < 6; x++) {
                        const vec3 d = vec3(float(x), float(y), float(z)) + 0.5f - 3.0f;
                        if (glm::length(d) > 3.0f) continue;
                        b.put(x, y, z, (x + y + z) % 2 ? rgb(0.3f, 0.3f, 0.32f) : rgb(0.38f, 0.38f, 0.4f), "iron");
                    }
            place(ball, b);
            if (!projv::utils::saveComposeToDisk(ball, projv::INVALID_COMPONENT_HANDLE, (std::filesystem::path(folder) / BALL).string()))
                return false;
        }
        projv::core::info("Wrote the arena to {}", folder);
        return true;
    }
}

template<> struct projv::runtime::ComponentTraits<arena::Thrower> {
    static constexpr const char* key = "mp.thrower";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const arena::Thrower& t) { return nlohmann::json{{"client", t.client}}; }
    static std::optional<arena::Thrower> load(const nlohmann::json& j, uint32_t) { return arena::Thrower{j.value("client", uint16_t(0))}; }
};
