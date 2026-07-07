#ifndef PROJECTV_COMPOSE_H
#define PROJECTV_COMPOSE_H

#include <vector>
#include <string>
#include <stdint.h>

#include "core/math.h"

// In-memory representations for the Compose scene-graph system.
// See docs/data_structures/compose_data_structure.md for the on-disk formats.
namespace projv {
    // Governs what happens when a `data` component's voxel data is modified and persisted.
    enum class Mutability {
        Locked, // Never written back; loader may alias one shared buffer across instances.
        Direct, // Edits are written in place to the source .data.
        Copy    // Edits are written to a new .data (copy-on-write); original untouched.
    };

    // --- .data (PVDT) container, in-memory form ---

    // One tree64 + its voxelTypeData. A simple asset has a single block at (0,0,0);
    // a grid volume has one block per occupied grid cell.
    struct DataBlock {
        int32_t gridX = 0, gridY = 0, gridZ = 0; // grid coordinate of this block
        std::vector<uint32_t> geometry;          // tree64 array
        std::vector<uint32_t> voxelTypeData;     // voxelTypeData array (may be empty)
    };

    // The intrinsic voxel data for one component: one tree64, or a grid of equally-sized,
    // grid-aligned tree64s. Placement is NOT stored here (that lives in compose.json).
    struct DataFile {
        uint32_t version = 1;
        uint32_t resolution = 0;      // edge resolution, shared by all blocks
        float    voxelScale = 0.0f;   // world size of one voxel at `resolution`
        bool     hasVoxelTypeData = true;
        std::vector<DataBlock> blocks;
    };

    // --- compose.json, in-memory form ---

    enum class ComponentType {
        Data,  // `source` is a .data file (a leaf).
        Asset  // `source` is a folder containing its own compose.json (recurses).
    };

    // One entry in a compose.json `components` list. Carries a transform applied
    // relative to its parent.
    struct ComposeComponent {
        ComponentType type = ComponentType::Data;
        std::string   source;                        // file (data) or folder (asset), unresolved
        core::vec3    position = core::vec3(0.0f);
        core::quat    rotation = core::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity (w,x,y,z)
        core::vec3    scale = core::vec3(1.0f);
        Mutability    mutability = Mutability::Locked; // only meaningful for `data`
    };

    // A parsed compose.json.
    struct ComposeDoc {
        uint32_t version = 1;
        std::string name;
        std::vector<ComposeComponent> components;
    };
}

#endif
