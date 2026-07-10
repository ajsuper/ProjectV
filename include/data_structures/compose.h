#ifndef PROJECTV_COMPOSE_H
#define PROJECTV_COMPOSE_H

#include <vector>
#include <string>
#include <stdint.h>

#include "core/math.h"
#include "data_structures/scene.h" // for projv::Mutability (runtime home)

// In-memory representations for the Compose scene-graph system.
// See docs/data_structures/compose_data_structure.md for the on-disk formats.
namespace projv {
    // Mutability lives in scene.h (runtime data home); ComposeComponent below references it.

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

    // One entry of a .data block table: a block's grid coordinate and where its arrays live in the file.
    // Read cheaply (without the blob region) via readDataFileHeader, so a streamer can decide what to
    // load and then readDataBlock exactly the blocks it wants. Byte offsets are absolute into the file.
    struct BlockEntry {
        int32_t  gridX = 0, gridY = 0, gridZ = 0; // grid coordinate of this block
        uint64_t geometryOffset = 0;              // byte offset of the tree64 uint32[]
        uint32_t geometryLength = 0;              // length in uint32 units
        uint64_t voxelTypeOffset = 0;             // byte offset of voxelTypeData uint32[] (0 if absent)
        uint32_t voxelTypeLength = 0;             // length in uint32 units (0 if absent)
    };

    // The header + block table of a .data container, WITHOUT the blob region. Lets a streamer learn a
    // file's shared params (resolution/voxelScale) and every block's grid coord + size/offset without
    // reading any geometry — the cheap index that drives per-block streaming.
    struct DataFileHeader {
        uint32_t version = 1;
        uint32_t resolution = 0;
        float    voxelScale = 0.0f;
        bool     hasVoxelTypeData = true;
        std::vector<BlockEntry> blocks;
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
