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

        // ---- The animation envelope: adjacent, and never required (v3) --------------------------
        //
        // A SECOND tree64, at `resolution / 4`, marking where animated geometry may be drawn. Empty
        // means this block has no animation, which is what every .data written before v3 means and
        // what the overwhelming majority of blocks will always mean.
        //
        // It is deliberately a separate tree rather than extra voxels in `geometry`, and that is the
        // whole point of the structure. Dilating the geometry itself -- which is what the prototype
        // did -- makes plain traversal WRONG: a renderer that knows nothing about animation marches
        // the swept volume as solid and draws grass as dilated blobs. Kept apart, the geometry keeps
        // meaning exactly what it always meant (the rest pose), so an animation-blind renderer,
        // picking, the fold and the voxelizer all keep working with no knowledge of any of this.
        //
        // ---- Why a quarter resolution ----
        //
        // One envelope cell spans 4x4x4 voxels, which comfortably contains the one-to-two voxels of
        // displacement that per-voxel detail animation wants. Against a per-voxel dilation that is
        // ~1/64 the cells and one tree level shallower, so envelope level k lines up with geometry
        // level k+1 and the two descend in lockstep at a fixed offset.
        //
        // What it costs is FALSE POSITIVES: a marked cell where nothing turns out to be drawn makes
        // the traversal resolve a 4^3 block that comes back empty. In dense canopy that is free --
        // nearly every cell is a target anyway -- and on sparse grass it is the thing to measure.
        std::vector<uint32_t> envelope;
        // One byte per set envelope cell, addressed exactly as materialIDs is: through the envelope
        // tree's own leaf material offsets, honouring the same uniform-leaf flag. It names a MOTION
        // SET -- which field moves this geometry and with what parameters -- and 0 means none.
        //
        // Note what is NOT here: no source tag, and no offset. Envelopes overlap (one position is
        // reachable by several blades at different moments of the cycle), so a single source tag
        // makes every other blade flicker out when it wants that spot, and a bitmask of valid offsets
        // escapes that for a 1-D sweep and does not fit for a ball. The cell says only "something can
        // be drawn here, under this motion"; which voxel is resolved at render time.
        std::vector<uint8_t>  envelopeMotion;
    };

    // The intrinsic voxel data for one component: one tree64, or a grid of equally-sized,
    // grid-aligned tree64s. Neither placement nor the palette is stored here -- both live in
    // compose.json, so this file is the geometry and nothing else.
    struct DataFile {
        uint32_t version = 3;
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
        // v3. Zero length means absent, exactly as materialLength does -- which is what makes a v2
        // file and a v3 file with no animation the same thing to every reader.
        uint64_t envelopeOffset = 0;              // byte offset of the envelope tree64 uint32[]
        uint32_t envelopeLength = 0;              // length in uint32 units (0 if absent)
        uint64_t envelopeMotionOffset = 0;        // byte offset of the envelopeMotion uint8[]
        uint32_t envelopeMotionLength = 0;        // length in BYTES (0 if absent)
    };

    // The header + block table of a .data container, WITHOUT the blob region. Lets a streamer learn a
    // file's shared params (resolution/voxelScale) and every block's grid coord + size/offset without
    // reading any geometry — the cheap index that drives per-block streaming.
    struct DataFileHeader {
        uint32_t version = 3;
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
