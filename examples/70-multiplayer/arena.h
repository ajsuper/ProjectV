// What the server and the client share: the arena they both load, and how to make it.
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "data_structures/scene.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

namespace arena {
    constexpr uint16_t PORT = 27015;
    constexpr const char* FOLDER = "arena";    // beside each binary: the same files on both
    constexpr const char* AVATAR = "avatar";   // a prefab inside it

    inline std::vector<projv::PendingVoxelOp> box(projv::core::ivec3 low, projv::core::ivec3 high, uint32_t colour) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) ops.push_back({true, projv::core::ivec3(x, y, z), colour});
        return ops;
    }

    // A 32 m floor of two greys in a checker, its top at y = 0, a static collider; a few pillars;
    // and the avatar, a 0.5 x 1.75 m figure with a lighter head. Written with --write-arena.
    inline bool write(const std::string& folder) {
        using namespace projv;
        {
            Scene scene;
            ComponentHandle floor = utils::addComponent(scene, ComponentKind::Chunk, "floor", INVALID_COMPONENT_HANDLE, 16, 0.5f);
            std::vector<PendingVoxelOp> ops;
            for (int z = 0; z < 64; z++)
                for (int x = 0; x < 64; x++)
                    for (int y = 0; y < 2; y++)
                        ops.push_back({true, core::ivec3(x, y, z), ((x / 8 + z / 8) % 2) ? packRGB10(0.42f, 0.44f, 0.47f) : packRGB10(0.3f, 0.32f, 0.35f)});
            for (core::ivec3 at : {core::ivec3(12, 2, 12), core::ivec3(48, 2, 12), core::ivec3(12, 2, 48), core::ivec3(48, 2, 48)}) {
                auto pillar = box(at, at + core::ivec3(3, 9, 3), packRGB10(0.75f, 0.55f, 0.3f));
                ops.insert(ops.end(), pillar.begin(), pillar.end());
            }
            utils::queueVoxelAdd(scene, floor, ops);
            utils::updateScene(scene);
            utils::setComponentTransform(scene, floor, core::vec3(-16, -1, -16), core::quat(1, 0, 0, 0), 1.0f);
            if (!utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, folder)) return false;
        }
        uint32_t floorId = utils::loadComposeFromDisk(folder).components[0].localId;
        std::ofstream(std::filesystem::path(folder) / "entities.json")
            << "{ \"version\": 1, \"entities\": [ { \"name\": \"floor\", \"link\": " << floorId
            << ", \"components\": { \"physics.static\": { \"v\": 1 } } } ] }\n";

        Scene avatar;
        ComponentHandle body = utils::addComponent(avatar, ComponentKind::Chunk, "body", INVALID_COMPONENT_HANDLE, 16, 0.25f);
        auto ops = box({0, 0, 0}, {1, 5, 1}, packRGB10(0.25f, 0.45f, 0.9f));
        auto head = box({0, 6, 0}, {1, 6, 1}, packRGB10(0.95f, 0.85f, 0.7f));
        ops.insert(ops.end(), head.begin(), head.end());
        utils::queueVoxelAdd(avatar, body, ops);
        utils::updateScene(avatar);
        // Centred on its feet, as a character's position is.
        utils::setComponentTransform(avatar, body, core::vec3(-0.25f, 0.0f, -0.25f), core::quat(1, 0, 0, 0), 1.0f);
        if (!utils::saveComposeToDisk(avatar, INVALID_COMPONENT_HANDLE, (std::filesystem::path(folder) / AVATAR).string())) return false;
        core::info("Wrote the arena to {}", folder);
        return true;
    }
}
