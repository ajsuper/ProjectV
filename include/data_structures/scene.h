#ifndef SCENE_H
#define SCENE_H

#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <stdint.h>

#include "core/math.h"
#include "data_structures/voxel.h"

namespace projv{

    constexpr uint32_t MAX_MATERIALS_PER_COMPONENT = 256u;
    constexpr uint8_t  INVALID_MATERIAL = 255u;

    // One palette entry: four words of properties plus an editor-facing name.
    //
    // The four words are exactly one RGBA32U texel, which is why the entry is this size and not some
    // other size. The palette texture used to pack four entries per texel and the shader fetched a
    // texel and selected one of its four components; at one entry per texel the SAME single
    // texelFetch returns all four words, so three extra properties cost no bandwidth and no extra
    // fetch. Going wider would cost a second fetch, which is the real budget here -- not memory. A
    // full 254-entry palette is 4 KB, against data textures measured in gigabytes.
    //
    // ---- The zero rule ----
    //
    // Words 1-3 are defined so that ALL ZEROES IS TODAY'S BEHAVIOUR: an opaque, non-emissive,
    // fully-rough dielectric shaded exactly as the colour-only pipeline shaded it. That is what
    // makes this change additive rather than a migration -- every compose.json already on disk
    // parses into palettes whose new words are zero, and renders identically.
    //
    // It is also why two of the fields are stored inverted from how you would name them out loud.
    // `glossiness` rather than roughness, because 0 has to mean "fully rough"; `transparency`
    // rather than opacity, because 0 has to mean "opaque". Storing roughness/opacity would make a
    // zero-filled legacy palette read as a scene of mirrors seen through glass.
    struct Material {
        // word0: albedo, R10G10B10 in bits 20-29 / 10-19 / 0-9. Bit-identical to the field this
        // struct has always had, so nothing that reads or writes a colour changes.
        uint32_t packedColor = 0;
        // word1: emission colour, same R10G10B10 layout. The intensity is separate (see below)
        // because a 10-bit channel cannot hold an emitter brighter than the sky.
        uint32_t packedEmission = 0;
        // word2: glossiness | metallic | transparency | ior, one byte each, high byte first.
        uint32_t packedSurface = 0;
        // word3: emissiveStrength | transmission | flags | reserved, one byte each, high byte first.
        uint32_t packedExtra = 0;
        std::string name;
    };

    // ---- Field accessors -------------------------------------------------------------------
    //
    // Free functions rather than members so Material stays a trivially-copyable POD-plus-string:
    // it is snapshotted wholesale by the editor's undo history and memcpy'd into the palette
    // texture, and both want it to stay dumb.

    inline uint32_t packRGB10(float r, float g, float b) {
        uint32_t red   = uint32_t(std::lround(std::clamp(r, 0.0f, 1.0f) * 1023.0f));
        uint32_t green = uint32_t(std::lround(std::clamp(g, 0.0f, 1.0f) * 1023.0f));
        uint32_t blue  = uint32_t(std::lround(std::clamp(b, 0.0f, 1.0f) * 1023.0f));
        return (red << 20) | (green << 10) | blue;
    }
    inline void unpackRGB10(uint32_t packed, float& r, float& g, float& b) {
        r = float((packed >> 20) & 0x3FFu) / 1023.0f;
        g = float((packed >> 10) & 0x3FFu) / 1023.0f;
        b = float((packed >>  0) & 0x3FFu) / 1023.0f;
    }

    inline uint32_t packByteField(float unitValue) {
        return uint32_t(std::lround(std::clamp(unitValue, 0.0f, 1.0f) * 255.0f));
    }

    // IOR occupies one byte as `1 + raw/128`, so raw 0 is 1.0 -- no refraction, which is what a
    // legacy palette must mean -- and the byte spans 1.0 to 2.99 in steps of 1/128. That covers
    // water (1.33), glass (1.5) and diamond (2.42) with room to spare, at a precision far finer
    // than anyone can see.
    inline uint32_t packIOR(float ior)   { return uint32_t(std::clamp(std::lround((ior - 1.0f) * 128.0f), 0L, 255L)); }
    inline float    unpackIOR(uint32_t raw) { return 1.0f + float(raw) / 128.0f; }

    // Emissive strength is exponential, not linear: an emitter is useful anywhere from "barely
    // glowing" to "brighter than the sun", and 256 linear steps across that range would put every
    // usable dim value in the first step. raw 0 is exactly zero (not emissive -- the zero rule
    // again), and 1..255 covers 2^-7.9 to 2^7.9, roughly 0.004 to 245, with raw 128 sitting at 1.0.
    inline float unpackEmissiveStrength(uint32_t raw) {
        return raw == 0u ? 0.0f : std::exp2(float(raw) / 16.0f - 8.0f);
    }
    inline uint32_t packEmissiveStrength(float strength) {
        if (strength <= 0.0f) return 0u;
        long raw = std::lround((std::log2(strength) + 8.0f) * 16.0f);
        return uint32_t(std::clamp(raw, 1L, 255L));
    }

    // word2 / word3 are byte lanes; these name them so no call site open-codes a shift.
    inline uint32_t packSurfaceWord(float glossiness, float metallic, float transparency, float ior) {
        return (packByteField(glossiness)  << 24) | (packByteField(metallic) << 16) |
               (packByteField(transparency) << 8) |  packIOR(ior);
    }
    inline uint32_t packExtraWord(float emissiveStrength, float transmission, uint32_t flags) {
        return (packEmissiveStrength(emissiveStrength) << 24) |
               (packByteField(transmission) << 16) | ((flags & 0xFFu) << 8);
    }
    inline float materialGlossiness(const Material& m)   { return float((m.packedSurface >> 24) & 0xFFu) / 255.0f; }
    inline float materialMetallic(const Material& m)     { return float((m.packedSurface >> 16) & 0xFFu) / 255.0f; }
    inline float materialTransparency(const Material& m) { return float((m.packedSurface >>  8) & 0xFFu) / 255.0f; }
    inline float materialIOR(const Material& m)          { return unpackIOR(m.packedSurface & 0xFFu); }
    inline float materialEmissiveStrength(const Material& m) { return unpackEmissiveStrength((m.packedExtra >> 24) & 0xFFu); }

    // The colour an emitter actually emits.
    //
    // Emission has to be storable INDEPENDENTLY of albedo -- the whole point is surfaces whose lit
    // appearance is unrelated to their unlit one, and `albedo * strength` cannot express the one that
    // matters most: a black voxel that glows. Multiplying by a zero albedo gives zero, so lava on
    // dark rock, an LED panel or a screen would have to be painted white to emit at all.
    //
    // But independent storage makes black the default emission colour, and black times any strength
    // is still nothing -- so raising Strength on a fresh material would do visibly nothing until a
    // colour was picked separately. Emitting black is meaningless (strength 0 is how you mean that),
    // which makes zero a free sentinel: it means "emit in the albedo's colour", which is what a
    // glowing version of a surface almost always wants. No flag bit and no extra state -- an entry
    // that has never had an emission colour set behaves the intuitive way, and one that has is
    // independent.
    inline uint32_t materialEffectiveEmissionColor(const Material& m) {
        return m.packedEmission != 0u ? m.packedEmission : m.packedColor;
    }
    inline float materialTransmission(const Material& m) { return float((m.packedExtra >> 16) & 0xFFu) / 255.0f; }
    inline uint32_t materialFlags(const Material& m)     { return (m.packedExtra >> 8) & 0xFFu; }

    // Whether an entry carries anything beyond its colour. Used by the serializer to keep
    // colour-only palettes writing exactly the JSON they used to.
    inline bool materialHasExtendedProperties(const Material& m) {
        return m.packedEmission != 0u || m.packedSurface != 0u || m.packedExtra != 0u;
    }
    // Stable slot handle for a chunk. A chunk keeps its handle for its whole life, and the handle IS
    // its row in the GPU header texture and its index into Scene.chunks. Grids and the loose list
    // reference chunks by handle so add/remove never has to rebase anything. See Scene below.
    using ChunkHandle = uint32_t;

    // Stable handle for a compose component (one `data` leaf instance from a compose.json -- see
    // ComposeComponent in compose.h). A component becomes exactly one of: a single loose Chunk, or a
    // whole SceneGrid (when its .data file has more than one block).
    using ComponentHandle = uint32_t;
    static constexpr ComponentHandle INVALID_COMPONENT_HANDLE = 0xFFFFFFFFu;

    enum class ComponentKind { Chunk, Grid, Asset };

    // The boolean role a component plays inside its parent. `None` is the historical behaviour and
    // the only value any compose.json written before this field existed can mean: the component is
    // *placed* -- parented, transformed, and rendered as its own geometry, with no relationship to
    // its siblings. The other three make it *resolved*: its cells are folded into its parent's
    // voxels, left to right down the child list, and it stops being a thing you look at on its own.
    //
    // Defaulting to None is what makes the field backward compatible in both directions. A default
    // of Union would silently reinterpret every scene already on disk as a boolean resolve, and a
    // loader that does not know the field reads a composed asset as its placed parts -- a degraded
    // picture, but a coherent one.
    //
    // Only meaningful on a child of an Asset node, and it means the same thing for all three kinds.
    // A Grid used to be excluded -- treated as None whatever the file said, on the grounds that
    // folding many blocks on many cells into a single-lattice .data is a rebuild rather than a bake.
    // That exclusion was invisible where it mattered: a Chunk becomes a Grid on its own the first
    // time a sculpt writes outside its resolution (convertChunkToGrid), so an ordinary .data could
    // quietly stop composing, and a stack of two of them resolved to nothing with no stated cause.
    // The cost argument was real but it is a budget question, and the fold already has budgets.
    enum class BooleanOp {
        None,       // Placed. Its own object, its own lattice, rendered as itself.
        Union,      // Add its cells to the accumulator. Its own colours win.
        Subtract,   // Remove its cells from the accumulator. Contributes no colour.
        Intersect   // Keep only cells in both. The accumulator's colours survive.
    };

    inline const char* booleanOpName(BooleanOp op) {
        switch (op) {
            case BooleanOp::None:      return "none";
            case BooleanOp::Union:     return "union";
            case BooleanOp::Subtract:  return "subtract";
            case BooleanOp::Intersect: return "intersect";
        }
        return "none";
    }

    // One queued edit. Continuous component-space coords (not Z-order). P1: append via
    // queueVoxelAdd/queueVoxelRemove, drained by updateScene.
    struct PendingVoxelOp {
        bool         isAdd;
        core::ivec3  position;
        uint32_t     packedColor;
    };

    // Per-component edit queue. P1: drained by updateScene; cleared after drain.
    struct ComponentEditQueue {
        std::vector<PendingVoxelOp> ops;
    };

    // Reference to the .data file backing a component. One entry per unique
    // (path, resolution, voxelScale). P1: allocated lazily by updateScene on first edit.
    struct DataReference {
        std::string sourceDataPath;
        uint32_t    resolution;
        float       voxelScale;
    };

    // One entry per loaded component, in Scene.components, indexed by ComponentHandle. Populated by
    // loadComposeFromDisk at the same two sites that create a loose Chunk or push a SceneGrid.
    struct ComponentRecord {
        ComponentKind kind;
        ChunkHandle   chunkHandle = 0;  // valid when kind == ComponentKind::Chunk
        int32_t       gridIndex = -1;   // valid when kind == ComponentKind::Grid
        std::string   sourcePath;       // provenance/debugging, mirrors GeometryBlob::sourceDataPath

        // True when this Asset node is a *reference* to the folder at `sourcePath` rather than
        // geometry this document owns. It is expanded in memory either way -- the renderer has to
        // see it -- so the flag matters at exactly one moment: saveComposeToDisk writes a linked node
        // as a bare `type: asset` entry pointing at `sourcePath`, and does not descend into it.
        //
        // Without it a link survives until the first save and then quietly becomes a copy, which is
        // the worst of the three behaviours: the user asked for propagation, still believes they have
        // it, and no longer do.
        //
        // Only meaningful for kind == Asset. A Chunk's equivalent question is Chunk::mutability.
        bool          externalSource = false;

        // P1 additions: per-component edit queue and lazily-assigned data reference.
        ComponentEditQueue editQueue;
        int32_t            dataRefID = -1;   // index into Scene.dataReferences; -1 = unassigned

        // P6: Identity
        std::string name;                    // local name (from compose.json or auto-generated)

        // P6: Hierarchy (Chunk, Grid, Asset -- all three participate)
        ComponentHandle parent = INVALID_COMPONENT_HANDLE;
        std::vector<ComponentHandle> children;   // populated for Asset; empty for Chunk/Grid

        // How this component combines with its siblings inside its parent. See BooleanOp: None means
        // "placed", which is what every component loaded from a compose.json without an `op` field
        // is, and the three boolean values make the parent Asset node an *assembly* whose voxels are
        // the fold of its children. Travels through compose.json, so an assembly re-opens as the
        // editable stack that produced it rather than as a finished mesh.
        BooleanOp op = BooleanOp::None;

        // P6: Local transform (relative to parent; all three use this)
        core::vec3 localPosition = core::vec3(0.0f);
        core::quat localRotation = core::quat(1.0f, 0.0f, 0.0f, 0.0f);
        float      localScale = 1.0f;            // uniform only (v0.0)

        // Per-component material palette: each component owns its own palette of
        // up to MAX_MATERIALS_PER_COMPONENT entries. Chunks/blobs of this component
        // reference local slots through their materialIDs texture; the GPU header's
        // paletteOffset + the local slot = global palette index.
        std::vector<Material> materialPalette;
        uint32_t paletteVersion = 0;
    };

    // Governs what happens when a `data` component's voxel data is modified and persisted.
    // (Runtime home; ComposeComponent in compose.h references it.) See compose_data_structure.md.
    enum class Mutability {
        Locked, // Never written back; instances of the same source share one buffer.
        Direct, // Edits are written in place to the source .data; same-policy instances still share.
        Copy    // Edits are written to a new .data (copy-on-write); original untouched.
    };

    struct ChunkHeader { // Designed to be user interfacable on CPU. Stored in disk. Only the necessary information for loading the chunk.
        uint32_t chunkID;
        core::vec3 position;
        float scale;
        float voxelScale;
        uint32_t resolution;
        core::quat rotation = core::quat(1.0f, 0.0f, 0.0f, 0.0f); // World rotation (w,x,y,z), identity by default.
    };

    #pragma pack(push, 1)
    struct GPUChunkHeader {
        uint32_t chunkID;
        float positionX;
        float positionY;
        float positionZ;
        float scale;
        uint32_t resolution;
        uint32_t geometryStartIndex;
        uint32_t geometryEndIndex;
        uint32_t materialIDStartIndex;      // index into materialID texture (in bytes)
        uint32_t materialIDEndIndex;        // end index (exclusive, in bytes)
        uint32_t dataRefID;
        uint32_t paletteOffset; // replaced padding[1]: per-blob offset into global palette
        float rotationX;
        float rotationY;
        float rotationZ;
        float rotationW;
        // Levels dropped below `resolution` this specific chunk instance's ray traversal should cap
        // itself at, on top of whatever the ray-distance-based cutoff already allows (see
        // computeTargetLOD/marchRayThroughTree64_DDA in pjv_utils_DDA.sc). Free/per-frame-safe: no
        // GPU data movement, just a shader-side "stop descending here" clamp. Computed in makeHeader
        // from Chunk::requestedLOD vs. the blob's actual uploadedLOD -- see the comment there.
        uint32_t traversalLOD;
        // Forced into existence by texel granularity (a texel is 4 uint32s and this field alone
        // pushed the header into a 5th texel; the struct must total a multiple of 4 words, so all
        // 3 remaining slots in that texel are claimed now rather than left implicitly wasted).
        uint32_t reserved0;
        uint32_t reserved1;
        uint32_t reserved2;
    };
    #pragma pack(pop)
    
    struct Chunk { // Only exists during runtime. Contains all of the header data and our geometry and color data, along with any extra runtime data not used in rendering.
        ChunkHeader header;
        std::vector<uint32_t> geometryData;
        // Levels dropped below this chunk's own native resolution (header.resolution), last
        // requested via requestChunkLOD. Drives this instance's GPU traversal cap (see makeHeader's
        // traversalLOD computation) independent of what's actually uploaded for a shared blob.
        uint32_t requestedLOD = 0;
        // Instancing: when >= 0, this chunk's geometry lives once in Scene.geometryPool[idx]
        // (shared across every instance of the same .data block) and geometryData above is left
        // empty. -1 = unpooled (interned into the pool by internChunkGeometry before any GPU work;
        // see internChunkGeometry in scene.h).
        int32_t geometryPoolIndex = -1;
        // Persistence policy for this instance (from its compose.json component). Drives whether an
        // edit is written back (Direct), copied off (Copy), or in-memory only (Locked).
        Mutability mutability = Mutability::Locked;
        // Slot liveness. Dead slots get a degenerate GPU header row (scale <= 0) that the shader
        // skips. Loaders create chunks alive.
        bool alive = true;
        // P6: true when the chunk's header (position/rotation) changed since the last GPU
        // sync. Set by rebakeSubtree, cleared by flushSceneUpdates → updateDirtyHeaders.
        bool headerDirty = false;
        // Residency key: which grid + cell this chunk fills, or -1/-1 when it is a loose
        // (transform-placed) leaf. Gives O(1) chunk->cell so eviction can clear the right
        // SceneGrid.cellToChunk slot without scanning. gridIndex indexes Scene.grids.
        int32_t gridIndex = -1;
        int32_t cellIndex = -1;
        // Which compose component this chunk came from. Only meaningful when loose (gridIndex == -1)
        // -- a grid-resident chunk's component is SceneGrid::componentHandle instead (one component
        // owns every cell of its grid).
        ComponentHandle componentHandle = INVALID_COMPONENT_HANDLE;
        // World-space edge length this chunk would have if its owning component's whole ancestor
        // chain had localScale == 1.0 everywhere (i.e. voxelScale * resolution, transform-independent).
        // Set once at load and never touched again; scene_query's rebakeSubtree multiplies it by the
        // scale extracted from the current world transform to get header.scale on every edit, rather
        // than trying to recover a scale factor from header.scale after the fact (there is nothing to
        // recover it relative to once a transform edit has already overwritten it).
        float nativeScale = 0.0f;
    };

    // One unique geometry blob shared across chunk instances. See Scene.geometryPool.
struct GeometryBlob {
        std::vector<uint32_t> geometry;
        // One byte per solid voxel, addressed through each leaf's material offset in `geometry` and
        // naming a slot in the owning component's palette. This is what the .data stores and what the
        // GPU reads: there is no second per-voxel colour representation to keep in step with it.
        std::vector<uint8_t>  materialIDs;
        // The editable form. Built lazily from geometry + materialIDs on the first edit
        // (ensureBrickMapExists -> brickMapFromTree64) and rebaked back into that pair after.
        std::unique_ptr<VoxelBrickMap> brickMap;

        GeometryBlob() = default;
        GeometryBlob(GeometryBlob&&) = default;
        GeometryBlob& operator=(GeometryBlob&&) = default;
        GeometryBlob(const GeometryBlob& rhs);
        GeometryBlob& operator=(const GeometryBlob& rhs);
        // Provenance for persistence: the .data file and block this geometry came from. Deduped with
        // the blob (few per scene), so it stays off the per-chunk struct. A copy-on-write fork inherits
        // these, then repoints sourceDataPath at its new file once persisted.
        std::string sourceDataPath;
        core::ivec3 sourceBlockCoord = core::ivec3(0);
        // False on a fresh copy-on-write fork: sourceDataPath still points at the inherited *original*
        // source (read-only for us). The first Copy persist writes a new file, repoints sourceDataPath,
        // and sets this true; subsequent Copy persists then write in place to that owned file.
        bool ownsSourceFile = true;
        // Number of live chunks referencing this blob. Incremented when a chunk is bound to the blob,
        // decremented on chunk removal / COW fork-away. At 0 the blob's GPU range is freed and its pool
        // slot is recycled via Scene.blobFreeList. Bounds pool growth over a long dynamic session.
        uint32_t refCount = 0;
        // P5: true when the blob's GPU content is stale (newly forked, interned, or content changed).
        // Cleared by flushSceneUpdates after the incremental upload.
        bool dirty = false;
        // Storage LOD: how many tree64 levels to drop when uploading this blob to the GPU. The CPU
        // arrays above always stay full resolution -- only the GPU copy is coarsened, at upload
        // time, by downsampleTree64. Each step is 4x coarser per axis (see clampRenderLOD in
        // voxel.h). This is the VRAM knob: distant geometry can be resident at a fraction of the
        // cost without any of it leaving CPU memory, so editing, persistence and the brick map are
        // all untouched by it. Set via setBlobRenderLOD below.
        uint32_t renderLOD = 0;
    };
    
    // A uniform grid of equal, grid-aligned chunks (one .data grid volume). Enables a
    // top-level DDA over cells instead of brute-forcing every chunk. See compose_data_structure.md.
    struct SceneGrid {
        core::vec3 origin;                  // World position of the grid's local (0,0,0) corner.
        float cellSize;                     // World size of one cell (one block).
        core::ivec3 dims;                   // Per-axis cell count.
        core::quat rotation;                // World rotation of the grid.
        std::vector<int32_t> cellToChunk;   // dims.x*dims.y*dims.z entries; index into Scene.chunks, or -1 if empty.
        // The compose component that owns this grid (every cell shares one component identity).
        ComponentHandle componentHandle = INVALID_COMPONENT_HANDLE;
        // Phase 2: cell coordinate (in the original .data block space) that sits at grid.origin.
        // After negative expansion this shifts away from (0,0,0). Used to linearize floorDiv'd
        // cell coordinates into the cellToChunk array.
        core::ivec3 originCellCoord{0};
        // Transform-independent cell size (see Chunk::nativeScale -- same idea, one level up).
        // rebakeSubtree multiplies this by the transform's extracted scale to get cellSize.
        float nativeCellSize = 0.0f;
        // Whether the renderer draws this grid's cells. The grid equivalent of Scene::looseChunks
        // membership, which is how an editor hides a loose chunk without disturbing it: a component
        // that has been folded into a resolved result must stop being drawn while staying fully
        // readable, so clearing Chunk::alive or blanking cellToChunk is not an option -- the fold
        // itself reads through both. Consumed by syncSceneTables (which emits every cell as empty
        // while this is false) and by picking's visibility mask, and by nothing else.
        bool rendered = true;
    };

    struct Scene {
        // Slot-indexed with holes: a chunk's index is its stable ChunkHandle and its GPU header row.
        std::vector<Chunk> chunks;
        std::vector<SceneGrid> grids;
        // Explicit set of loose (transform-placed) chunk handles. The shader iterates this list
        // instead of a positional [0, looseChunkCount) prefix, so a loose chunk can be added or
        // removed without any ordering constraint on Scene.chunks. Grid blocks are reached only via
        // SceneGrid.cellToChunk and are NOT listed here.
        std::vector<ChunkHandle> looseChunks;
        // Derived count of loose chunks, kept only so the legacy no-grid GPU fallback still works;
        // ordering of Scene.chunks is no longer load-bearing. Prefer looseChunks.size().
        uint32_t looseChunkCount = 0;
        // Shared geometry for .data blocks; chunks reference entries via Chunk.geometryPoolIndex.
        // Refcounted (GeometryBlob.refCount); empty slots recycled via blobFreeList.
        std::vector<GeometryBlob> geometryPool;
        std::vector<uint32_t> blobFreeList; // recycled empty geometryPool slots (LIFO).
        // One entry per loaded compose component, indexed by ComponentHandle. Populated by
        // loadComposeFromDisk; never compacted or reordered (handles are stable for the Scene's life).
        std::vector<ComponentRecord> components;
        // P1 addition: lazily populated by editing::updateScene on first edit per source.
        std::vector<DataReference>   dataReferences;
        // Guards every ComponentRecord::materialPalette (+ paletteVersion) against concurrent
        // interning. One mutex for the whole Scene rather than one per ComponentRecord: std::mutex
        // is neither movable nor copyable, so a per-component mutex would break growth of the
        // `components` vector. Locked by internMaterial (material.cpp) and by
        // rebuildGlobalPaletteTexture (gpu_interface.cpp), which live-iterates materialPalette once
        // a frame and would otherwise race with a concurrent intern from a worker thread.
        mutable std::mutex materialPaletteMutex;

        // std::mutex is neither movable nor copyable, which would otherwise make Scene itself
        // neither -- and that breaks two real things: loadComposeFromDisk (compose_io.cpp) returns
        // a Scene by value (needs move), and the ECS's createGlobalResource<T> (core/ecs.h) stores
        // Scene inside a std::any, whose in-place constructor is SFINAE-constrained on
        // is_copy_constructible_v<T> as a *static* requirement -- even though it only ever
        // default-constructs Scene in place and nothing in the engine actually copies a live Scene
        // value. Both special members below move/copy every field except the mutex, which is left
        // freshly default-constructed on the destination (a mutex has no state worth moving or
        // copying; each Scene instance just needs its own).
        Scene() = default;
        Scene(Scene&& other) noexcept
            : chunks(std::move(other.chunks))
            , grids(std::move(other.grids))
            , looseChunks(std::move(other.looseChunks))
            , looseChunkCount(other.looseChunkCount)
            , geometryPool(std::move(other.geometryPool))
            , blobFreeList(std::move(other.blobFreeList))
            , components(std::move(other.components))
            , dataReferences(std::move(other.dataReferences))
        {}
        Scene& operator=(Scene&& other) noexcept {
            if (this == &other) return *this;
            chunks = std::move(other.chunks);
            grids = std::move(other.grids);
            looseChunks = std::move(other.looseChunks);
            looseChunkCount = other.looseChunkCount;
            geometryPool = std::move(other.geometryPool);
            blobFreeList = std::move(other.blobFreeList);
            components = std::move(other.components);
            dataReferences = std::move(other.dataReferences);
            return *this;
        }
        Scene(const Scene& other)
            : chunks(other.chunks)
            , grids(other.grids)
            , looseChunks(other.looseChunks)
            , looseChunkCount(other.looseChunkCount)
            , geometryPool(other.geometryPool)
            , blobFreeList(other.blobFreeList)
            , components(other.components)
            , dataReferences(other.dataReferences)
        {}
        Scene& operator=(const Scene& other) {
            if (this == &other) return *this;
            chunks = other.chunks;
            grids = other.grids;
            looseChunks = other.looseChunks;
            looseChunkCount = other.looseChunkCount;
            geometryPool = other.geometryPool;
            blobFreeList = other.blobFreeList;
            components = other.components;
            dataReferences = other.dataReferences;
            return *this;
        }
    };

    // Inserts a blob into the pool, reusing a recycled slot when one is free so live pool indices
    // stay stable. Pure data-structure op (no IO), so both loaders and the GPU builder can call it.
    inline int32_t poolInsertBlob(Scene& scene, GeometryBlob&& blob) {
        if (!scene.blobFreeList.empty()) {
            int32_t idx = static_cast<int32_t>(scene.blobFreeList.back());
            scene.blobFreeList.pop_back();
            scene.geometryPool[idx] = std::move(blob);
            return idx;
        }
        int32_t idx = static_cast<int32_t>(scene.geometryPool.size());
        scene.geometryPool.push_back(std::move(blob));
        return idx;
    }

    // Releases one chunk's reference to a pool blob. Decrements refCount; if it hits 0, recycles
    // the slot via blobFreeList so poolInsertBlob can reuse it. GPU-side teardown (freeing the
    // allocator range) is handled generically by uploadDirtyBlobs on the next flush, which frees
    // the range as soon as it sees refCount==0 on an uploaded blob -- this is the CPU-side half.
    inline void releaseBlob(Scene& scene, int32_t poolIdx) {
        if (poolIdx < 0 || poolIdx >= static_cast<int32_t>(scene.geometryPool.size())) return;
        GeometryBlob& blob = scene.geometryPool[poolIdx];
        if (blob.refCount == 0) return; // already released; avoid double-freeing the slot
        if (--blob.refCount == 0) scene.blobFreeList.push_back(static_cast<uint32_t>(poolIdx));
    }

    // Moves an unpooled chunk's owned geometry (geometryPoolIndex < 0) into a fresh refcount-1 pool blob
    // and points the chunk at it, so every chunk flows through the single pooled path. No-op (returns
    // the existing index) if the chunk is already pooled. Used by the voxelizer and defensively by
    // the GPU builder to guarantee no chunk reaches the GPU with geometryPoolIndex < 0.
    inline int32_t internChunkGeometry(Scene& scene, Chunk& chunk,
                                       std::unique_ptr<VoxelBrickMap> brickMap = nullptr) {
        if (chunk.geometryPoolIndex >= 0) return chunk.geometryPoolIndex;
        GeometryBlob blob;
        blob.geometry = std::move(chunk.geometryData);
        if (brickMap) blob.brickMap = std::move(brickMap);
        blob.refCount = 1;
        blob.dirty = true;
        chunk.geometryData.clear();
        chunk.geometryPoolIndex = poolInsertBlob(scene, std::move(blob));
        return chunk.geometryPoolIndex;
    }

    // Copy-on-write fork of a pool blob (P1 editing primitive). Creates a new GeometryBlob that is a
    // deep copy of geometryPool[srcIndex], with refCount = 1 (the editing chunk owns it). Does NOT
    // decrement the original's refCount -- other chunks may still share it. The caller is responsible
    // for pointing the editing chunk at the returned index. Returns srcIndex unchanged if it is
    // out of range (caller should intern the chunk first via internChunkGeometry).
    //
    // P6 optimization: if the blob's refCount is exactly 1 (sole owner), skip the copy and edit
    // in-place. The blob is marked dirty for the next GPU flush, and the same index is returned.
    // This avoids allocating a new pool entry and GPU range on every edit, which is the dominant
    // source of allocator pressure in sustained editing sessions.
    inline int32_t forkBlob(Scene& scene, int32_t srcIndex) {
        if (srcIndex < 0 || srcIndex >= static_cast<int32_t>(scene.geometryPool.size())) {
            return srcIndex;
        }
        // Sole owner — no copy needed, edit in place.
        if (scene.geometryPool[srcIndex].refCount == 1) {
            scene.geometryPool[srcIndex].dirty = true;
            return srcIndex;
        }
        GeometryBlob fork = scene.geometryPool[srcIndex]; // deep copy via copy ctor
        fork.refCount = 1;
        fork.dirty = true;
        return poolInsertBlob(scene, std::move(fork));
    }

    // Sets the storage LOD a blob's GPU copy is built at, and marks it dirty. Marking dirty is all
    // that is needed to make the change take effect: uploadDirtyBlobs re-uploads the blob at the
    // new detail and reports it in `uploadedPools`, which makes updateDirtyHeaders rewrite the
    // header row of every chunk referencing it -- picking up the reduced resolution. Returns true
    // if the LOD actually changed, so callers can rate-limit transitions.
    inline bool setBlobRenderLOD(Scene& scene, int32_t poolIdx, uint32_t lod) {
        if (poolIdx < 0 || poolIdx >= static_cast<int32_t>(scene.geometryPool.size())) return false;
        GeometryBlob& blob = scene.geometryPool[poolIdx];
        if (blob.renderLOD == lod) return false;
        blob.renderLOD = lod;
        blob.dirty = true;
        return true;
    }

    enum class ChunkLODResult : uint8_t { Unchanged, Coarsened, NeedsRegeneration };

    // Requests that `handle`'s GPU-visible geometry represent `requestedResolution`. If the chunk's
    // stored (native) resolution -- header.resolution, set once at generation time -- is >= the
    // request, the engine derives the coarser view via the existing GPU-upload truncation path
    // (setBlobRenderLOD/downsampleTree64) and returns Coarsened (or Unchanged if already there); no
    // CPU regeneration is needed. If the request is finer than what's stored, nothing is changed --
    // NeedsRegeneration tells the caller it must generate finer geometry itself and install it via
    // replaceChunkGeometry, then call requestChunkLOD again. requestedResolution must be reachable
    // from the chunk's native resolution by exact 4x steps (same constraint tree64 depth already
    // has -- see voxel.h), matching whatever ring/step scheme the caller uses.
    inline ChunkLODResult requestChunkLOD(Scene& scene, ChunkHandle handle, uint32_t requestedResolution) {
        if (handle >= scene.chunks.size()) return ChunkLODResult::NeedsRegeneration;
        Chunk& chunk = scene.chunks[handle];
        uint32_t native = chunk.header.resolution;
        if (requestedResolution > native) return ChunkLODResult::NeedsRegeneration;

        uint32_t levels = 0;
        uint32_t res = native;
        while (res > requestedResolution && res > 1) { res /= 4; levels++; }

        if (levels == chunk.requestedLOD) return ChunkLODResult::Unchanged;
        chunk.requestedLOD = levels;

        int32_t poolIdx = chunk.geometryPoolIndex;
        if (poolIdx < 0 || poolIdx >= static_cast<int32_t>(scene.geometryPool.size()))
            return ChunkLODResult::Coarsened;

        GeometryBlob& blob = scene.geometryPool[poolIdx];
        uint32_t reconciled = levels;
        if (blob.refCount > 1) {
            // Shared blob: its one GPU upload must satisfy the pickiest (finest) sharer's request.
            // No reverse index from blob->chunks exists, so this scans scene.chunks -- only paid
            // when sharing is actually happening (the common refCount==1 case skips straight past).
            for (const Chunk& c : scene.chunks)
                if (c.alive && c.geometryPoolIndex == poolIdx)
                    reconciled = std::min(reconciled, c.requestedLOD);
        }

        if (!setBlobRenderLOD(scene, poolIdx, reconciled)) {
            // The blob's actual upload didn't move (a pickier sharer already wins), but this
            // chunk's own requested level changed -- its header row still needs rebuilding so the
            // shader picks up the new traversalLOD (see makeHeader in gpu_interface.cpp).
            chunk.headerDirty = true;
        }
        return ChunkLODResult::Coarsened;
    }

    // Installs newly generated geometry into an existing chunk in place, keeping its ChunkHandle,
    // grid-cell binding, and identity untouched -- no eviction needed. If the chunk currently owns
    // its blob solely (refCount == 1), overwrites it directly; if shared (refCount > 1), this chunk
    // gets a fresh blob of its own and detaches from the old one via releaseBlob (mirrors forkBlob's
    // sole-owner-vs-fork split). Raises header.resolution to newResolution and resets requestedLOD
    // (it was relative to the OLD native resolution) and renderLOD (stale coarsening no longer
    // applies to new content). This is what lets a chunk be "refined" (regenerated at higher detail
    // as the camera approaches) with zero pop-out: the chunk stays visible at its old resolution
    // right up until the new data lands, instead of being evicted and re-streamed from scratch.
    inline void replaceChunkGeometry(Scene& scene, ChunkHandle handle,
                                      std::vector<uint32_t> newGeometry,
                                      std::vector<uint8_t> newMaterialIDs,
                                      uint32_t newResolution,
                                      std::unique_ptr<VoxelBrickMap> newBrickMap = nullptr) {
        if (handle >= scene.chunks.size()) return;
        Chunk& chunk = scene.chunks[handle];
        int32_t oldIdx = chunk.geometryPoolIndex;

        int32_t targetIdx = -1;
        if (oldIdx >= 0 && oldIdx < static_cast<int32_t>(scene.geometryPool.size())) {
            if (scene.geometryPool[oldIdx].refCount == 1) {
                targetIdx = oldIdx; // sole owner: overwrite in place
            } else {
                GeometryBlob fresh;
                fresh.refCount = 1;
                targetIdx = poolInsertBlob(scene, std::move(fresh));
                releaseBlob(scene, oldIdx); // detach from the old shared blob
            }
        }
        if (targetIdx < 0) {
            GeometryBlob fresh;
            fresh.refCount = 1;
            targetIdx = poolInsertBlob(scene, std::move(fresh));
        }

        GeometryBlob& blob = scene.geometryPool[targetIdx];
        blob.geometry = std::move(newGeometry);
        blob.materialIDs = std::move(newMaterialIDs);
        if (newBrickMap) blob.brickMap = std::move(newBrickMap);
        blob.renderLOD = 0;
        blob.dirty = true;

        chunk.geometryPoolIndex = targetIdx;
        chunk.header.resolution = newResolution;
        chunk.requestedLOD = 0;
        chunk.headerDirty = true;
    }

    }

#endif
