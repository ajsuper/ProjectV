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

    // One tree64 + the material byte per solid voxel the shader indexes alongside it. A simple asset
    // has a single block at (0,0,0); a grid volume has one block per occupied grid cell.
    //
    // materialIDs are slots into the *component's* palette, and that palette lives in compose.json
    // rather than here -- see ComposeComponent::palette. The split is what lets one .data be instanced
    // by several components that colour it differently: the geometry and the slot indices are shared,
    // and only the short list of colours those slots point at is per-instance.
    //
    // The array is indexed by the leaf nodes of `geometry`: each leaf's data3 carries a byte offset
    // into it, and a leaf whose voxels all share a material stores one byte rather than one per voxel
    // (the uniform-leaf flag). Produced by bakeMaterialsFromBrickMap, consumed as-is by the GPU -- so
    // nothing about the material system is rebuilt at load time.
    struct DataBlock {
        int32_t gridX = 0, gridY = 0, gridZ = 0; // grid coordinate of this block
        std::vector<uint32_t> geometry;          // tree64 array
        std::vector<uint8_t>  materialIDs;       // one byte per solid voxel, uniform leaves collapsed
    };

    // The intrinsic voxel data for one component: one tree64, or a grid of equally-sized,
    // grid-aligned tree64s. Neither placement nor the palette is stored here -- both live in
    // compose.json, so this file is the geometry and nothing else.
    struct DataFile {
        uint32_t version = 2;
        uint32_t resolution = 0;      // edge resolution, shared by all blocks
        float    voxelScale = 0.0f;   // world size of one voxel at `resolution`
        std::vector<DataBlock> blocks;
    };

    // One entry of a .data block table: a block's grid coordinate and where its arrays live in the file.
    // Read cheaply (without the blob region) via readDataFileHeader, so a streamer can decide what to
    // load and then readDataBlock exactly the blocks it wants. Byte offsets are absolute into the file.
    struct BlockEntry {
        int32_t  gridX = 0, gridY = 0, gridZ = 0; // grid coordinate of this block
        uint64_t geometryOffset = 0;              // byte offset of the tree64 uint32[]
        uint32_t geometryLength = 0;              // length in uint32 units
        uint64_t materialOffset = 0;              // byte offset of the materialIDs uint8[] (0 if absent)
        uint32_t materialLength = 0;              // length in BYTES, not words (0 if absent)
    };

    // The header + block table of a .data container, WITHOUT the blob region. Lets a streamer learn a
    // file's shared params (resolution/voxelScale) and every block's grid coord + size/offset without
    // reading any geometry — the cheap index that drives per-block streaming.
    struct DataFileHeader {
        uint32_t version = 2;
        uint32_t resolution = 0;
        float    voxelScale = 0.0f;
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
        std::string   name;                          // from "name" in JSON, or auto-generated
        core::vec3    position = core::vec3(0.0f);
        core::quat    rotation = core::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity (w,x,y,z)
        core::vec3    scale = core::vec3(1.0f);
        Mutability    mutability = Mutability::Locked; // only meaningful for `data`
        // How this entry combines with the ones above it in the list -- see projv::BooleanOp. `none`
        // is the default and the only thing a compose.json written before this field existed can
        // mean, so adding it reinterprets nothing already on disk. Because `asset` entries recurse,
        // this one field gives nested CSG for free: subtracting a whole sub-assembly is an `asset`
        // entry with `op: subtract`, and loadComposeFromDisk already walks it.
        BooleanOp     op = BooleanOp::None;
        // The colours this component's material slots name, in slot order -- the palette that the
        // .data's materialIDs index into, and the same list that becomes
        // ComponentRecord::materialPalette at load. It lives here rather than in the .data so that two
        // components can instance one geometry file and still be coloured independently, and so that
        // a recolour is a small JSON edit instead of a geometry rewrite. Empty for `asset` entries,
        // which own no voxels.
        std::vector<Material> palette;
    };

    // A parsed compose.json.
    struct ComposeDoc {
        uint32_t version = 1;
        std::string name;
        std::vector<ComposeComponent> components;
    };
}

#endif
