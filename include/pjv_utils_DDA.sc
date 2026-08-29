#define MAX_STACK_SIZE 12
#define RENDER_MODE 3 //1-8
#define VOXEL_TYPEDATA_SLICES 3

// World-unit scale. Voxels are now 1 world unit (voxelScale = 1); the shaders were tuned for
// ~0.0727-unit voxels, so the scene is ~13.76x larger. Multiply world-space DISTANCE constants
// (self-hit ray-origin offsets, ray lengths) by this; leave voxel-relative / step-count values.
#ifndef WORLD_SCALE
#define WORLD_SCALE 13.76
#endif

/* File structure:
 * - Uniforms
 * - Textures
 * - Constants
 * - Structs
 * - Traversal arrays.
 * - LUT's
 * - box/ray intersections
 * - Traversal helper functions
 * - Traversal functions
 */

USAMPLER2D(tree64Data, 14);
USAMPLER2D(materialIDs, 9);
USAMPLER2D(materialPalette, 10);
USAMPLER2D(headerData, 15);
USAMPLER2D(gridInfo, 12);
USAMPLER2D(cellMap, 13);
USAMPLER2D(looseList, 11);

uniform vec4 tree64Dims;
uniform vec4 voxelTypeDims;
uniform vec4 paletteDims;

// =============================================================================
// The motion table
// =============================================================================
//
// Engine-owned, exactly as passTargetRes is: not declared in any resources.json, set before every
// pass, and available to a shader that declares it. Two vec4 per motion set, sixteen sets:
//
//   [2i+0] = (kind, amplitude in VOXELS, spatial frequency, speed)
//   [2i+1] = (direction.xyz, turbulence)
//
// A material's flags byte names a row (see MATERIAL_FLAG_ANIMATED in scene.h). The indirection is
// what makes this a mechanism rather than a menu: grass sway, leaf flutter, a rising flame and a
// water surface are one traversal and four rows, not four code paths. An all-zero table -- what
// every renderer that never configures one gets -- means nothing animates.
uniform vec4 pjvMotionSets[48];   // three per set, sixteen sets -- see PROJV_MOTION_SET_VEC4S
uniform vec4 pjvAnimTime;    // (seconds, 0, 0, 0)

#define PJV_MOTION_NONE   0u
#define PJV_MOTION_SWAY   1u
#define PJV_MOTION_ADVECT 2u

// The material flags byte, as scene.h lays it out. Bit 0 says this material moves; bits 1-4 name the
// motion set. Read straight off the palette word rather than through decodeMaterial, because the
// traversal asks this question far more often than it needs a whole material.
#define PJV_MAT_FLAG_ANIMATED   1u
#define PJV_MAT_MOTION_SHIFT    1u
#define PJV_MAT_MOTION_MASK     0xFu

// Does this material move, and by which mechanism? Named here so a renderer does not have to
// open-code the flags byte to answer either question -- both are read by the g-buffer to decide how
// a surface should be shaded and how a temporal filter may reuse it.
//
// The two are a genuine distinction rather than a convenience. A swayed voxel keeps its SOURCE cell
// (the animated march reports it on purpose), so a temporal filter can gate it on an exact voxel
// identity and give it a long mean. An advected parcel has no such anchor -- it moves THROUGH the
// lattice, so the voxel under it is a different one each frame -- and can only be matched
// positionally, with a looser radius and a shorter mean. Collapsing the two into one flag is why
// fire still flickered after grass had stopped: it was being offered grass's gate, which nothing
// about fire can pass.

struct MotionSet {
    uint  kind;
    float amplitude;    // voxels
    float frequency;    // cycles per voxel
    float speed;
    vec3  direction;    // already normalised on upload
    float turbulence;
    // ---- ADVECTION ONLY; a sway set leaves these zero and never reads them ----------------
    float dissolve;         // 0..1, how readily a risen parcel burns out
    float turbulenceGrowth; // how much the swirl grows with age
    float travel;           // how far a parcel may rise from its source, in voxels
};

MotionSet pjvMotionSet(uint index) {
    uint base = 3u * min(index, 15u);
    vec4 a = pjvMotionSets[base];
    vec4 b = pjvMotionSets[base + 1u];
    vec4 c = pjvMotionSets[base + 2u];
    MotionSet m;
    m.kind = uint(a.x + 0.5);
    m.amplitude = a.y;
    m.frequency = a.z;
    m.speed = a.w;
    m.direction = b.xyz;
    m.turbulence = b.w;
    // Advection's row. Zero for a sway set, which never reads it.
    m.dissolve = c.x;
    m.turbulenceGrowth = c.y;
    m.travel = c.z;
    return m;
}

float pjvAnimSeconds() { return pjvAnimTime.x; }


// ---- The field -------------------------------------------------------------------------------
//
// Value noise, advected along the motion set's direction. It replaced a travelling plane wave, and
// the reason is worth keeping: a plane wave's level sets are straight lines, and a quantised
// displacement DRAWS its level set -- so the grass moved in visible straight bands across the field.
//
// What it must be is LOW FREQUENCY, and that is a correctness requirement rather than a taste one.
// Neighbouring voxels of one blade get their displacement from their own positions, so if the field
// varies quickly in space those displacements diverge and the blade separates into floating
// segments. That does not read as motion blur; it reads as a bug. Hence a field smooth over tens of
// voxels, and hence NOT a per-voxel position hash -- a hash has no scale, so neighbours are
// independent, and independent neighbours under a quantiser is torn geometry by construction.
// The lattice hash. Deliberately transcendental-free: the sin()-based version this replaced ran four
// sines per corner set and the field is evaluated up to nine times per candidate cell, which measured
// as the dominant cost of the whole resolve -- not the tree descents, which is where the cost was
// assumed to be. Multiply-and-fract is a few ALU ops and the field is noise either way.
float pjvHash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float pjvValueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);   // smoothstep: C1, which is what keeps the gradient small
    float c000 = pjvHash13(i + vec3(0.0, 0.0, 0.0));
    float c100 = pjvHash13(i + vec3(1.0, 0.0, 0.0));
    float c010 = pjvHash13(i + vec3(0.0, 1.0, 0.0));
    float c110 = pjvHash13(i + vec3(1.0, 1.0, 0.0));
    float c001 = pjvHash13(i + vec3(0.0, 0.0, 1.0));
    float c101 = pjvHash13(i + vec3(1.0, 0.0, 1.0));
    float c011 = pjvHash13(i + vec3(0.0, 1.0, 1.0));
    float c111 = pjvHash13(i + vec3(1.0, 1.0, 1.0));
    float x00 = mix(c000, c100, f.x);
    float x10 = mix(c010, c110, f.x);
    float x01 = mix(c001, c101, f.x);
    float x11 = mix(c011, c111, f.x);
    return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z) * 2.0 - 1.0;   // -1..1
}

// TWO independent noise values from ONE lattice, and it costs what one costs.
//
// Advection needs a pair per step -- the two components of its swirl -- and calling pjvValueNoise
// twice pays for two sets of eight corner hashes and two trilinear blends. Nearly all of that is
// shared: the lattice cell, the smoothstep weights and the mix tree are identical, and only the hash
// differs. A hash that emits two decorrelated floats instead of one therefore halves the whole
// function, which is the single hottest thing in the advection trace.
vec2 pjvHash23(vec3 p) {
    vec3 q = fract(p * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yxz + 33.33);
    return fract(vec2((q.x + q.y) * q.z, (q.x + q.z) * q.y));
}

vec2 pjvValueNoise2(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    vec2 c000 = pjvHash23(i + vec3(0.0, 0.0, 0.0));
    vec2 c100 = pjvHash23(i + vec3(1.0, 0.0, 0.0));
    vec2 c010 = pjvHash23(i + vec3(0.0, 1.0, 0.0));
    vec2 c110 = pjvHash23(i + vec3(1.0, 1.0, 0.0));
    vec2 c001 = pjvHash23(i + vec3(0.0, 0.0, 1.0));
    vec2 c101 = pjvHash23(i + vec3(1.0, 0.0, 1.0));
    vec2 c011 = pjvHash23(i + vec3(0.0, 1.0, 1.0));
    vec2 c111 = pjvHash23(i + vec3(1.0, 1.0, 1.0));
    vec2 x00 = mix(c000, c100, f.x);
    vec2 x10 = mix(c010, c110, f.x);
    vec2 x01 = mix(c001, c101, f.x);
    vec2 x11 = mix(c011, c111, f.x);
    return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z) * 2.0 - 1.0;   // -1..1
}

// Displacement of a SWAY set at a world position, in voxels.
//
// A function of the POSITION and the time, and of nothing else -- not of the ray, and not of which
// voxel is asking. That is the property the whole resolve depends on: two rays crossing one cell
// must compute the same answer for it, or the voxel is drawn in two places at once and appears
// sliced in half along the silhouette where the two rays disagree.
vec3 pjvMotionDisplacement(MotionSet m, vec3 worldPos, float voxelSize, float t) {
    if (m.kind != PJV_MOTION_SWAY || m.amplitude <= 0.0) return vec3(0.0);
    // Sampled in VOXEL units so a set's frequency means the same thing whatever a chunk's scale is.
    vec3 p = worldPos / max(voxelSize, 1e-6);
    vec3 flow = m.direction * (m.speed * t);
    float signal = pjvValueNoise(p * m.frequency - flow * m.frequency);
    // Turbulence adds a second, finer octave, kept well below the base frequency on purpose: it stops
    // the field reading as one smooth swell rather than adding detail the blades could hold. Skipped
    // outright at zero -- it doubles the cost of the single hottest function in the resolve, and
    // paying that to add nothing is not a trade worth making by default.
    if (m.turbulence > 1e-3) {
        signal += m.turbulence * pjvValueNoise(p * (m.frequency * 2.7) - flow * m.frequency * 1.7);
    }
    signal = clamp(signal, -1.0, 1.0);
    // Perpendicular-to-vertical: wind pushes along its direction, and a horizontal set has no y.
    return m.direction * (signal * m.amplitude);
}

// ---- RayQuery: everything the caller wants out of a traversal, in one place -------------------
//
// NOTE ON DEFAULTS: the initializers below are NOT dependable, and this is the single most important
// thing to know about this struct. Every renderer builds a RayQuery by declaring one and assigning
// only the fields it cares about; the compiled result does not carry the values written here, so an
// unassigned field is garbage. `doTransparency` sat in here unset and unread for as long as it
// existed, which is why nobody noticed. Worse, when the peel's distance floor was first put here and
// left to default to 0, it arrived as a garbage floor in all ~20 existing shaders and rejected every
// hit in the scene -- a completely empty render, with nothing on screen to attribute it to.
//
// That history is why this struct now grew rather than staying frozen, and the two rules that make
// growing it safe:
//
//   1. A query is PRODUCED BY A CONSTRUCTOR (pjvPrimaryQuery / pjvShadowQuery / pjvBounceQuery),
//      never by a bare `RayQuery q;`. A constructor assigns every field, so nothing is left to a
//      default that does not survive.
//   2. `magic` is checked on entry to the scene query. A struct that did not come from a constructor
//      almost certainly does not carry PJV_QUERY_MAGIC, and the traversal then CLAMPS THE FIELDS
//      ADDED HERE to their safe values and reports PJV_STOP_BAD_QUERY in the result. The failure
//      becomes one readable stop reason instead of an empty frame.
//
// The clamp deliberately leaves the four pre-existing fields (startLOD, finishLOD,
// distanceToFinishLOD, maxRaySteps) alone: every call site written before the constructors existed
// assigns all four explicitly, so they are trustworthy even when `magic` is not. That is what lets
// the migration be incremental -- an un-migrated shader keeps rendering exactly what it always did.
//
// What still may NOT live here: anything the march needs before the scene query has had a chance to
// validate the struct. The peel's distance floor is the standing example and stays an explicit
// function parameter of raySceneIntersectFrom.
#define PJV_QUERY_MAGIC 0x51525931u   // 'QRY1'

// Traversal behaviour the caller opts into. All-bits-zero is the plain nearest-opaque-hit march that
// this file has always done, which is what a clamped/bad query falls back to.
#define PJV_Q_TRANSPARENCY  0x1u   // see through voxels whose material says transparency > 0
#define PJV_Q_REFRACTION    0x2u   // ...and bend the ray at an interface whose ior != 1
#define PJV_Q_ANIMATION     0x4u   // resolve animated geometry against its envelope
#define PJV_Q_OCCLUSION_ONLY 0x8u  // the caller wants transmittance, not a surface to shade
// Populate SceneHit::material. Off by default, and deliberately so: resolving a material costs two
// texel fetches, a caller that is going to shade the hit has always paid them itself through
// fetchVoxelMaterialFromHit, and turning this on unconditionally would make it pay them TWICE until
// it migrated. Asking for it here and reading SceneHit::material is strictly cheaper than the old
// route -- the traversal already has the leaf in registers and needs no second descent. Implied by
// PJV_Q_TRANSPARENCY, which cannot decide anything without the material.
#define PJV_Q_WANT_MATERIAL 0x10u
// Draw every envelope cell as SOLID instead of resolving it, so the baked envelope is directly
// visible. This is the diagnostic that splits the failure in half, and it is worth shipping rather
// than reaching for a debugger: if animated geometry is missing, blobs appearing here mean the
// envelope, its upload and its march are all fine and the fault is in the resolve, while nothing
// appearing means the geometry never had anywhere to be drawn in the first place. The two look
// identical on screen -- a hole with the background through it -- and are nowhere near each other.
#define PJV_Q_ANIM_DEBUG_SOLID 0x20u

// A starting LOD no ordinary ramp can mean; selects footprint mode in computeTargetLOD, which
// reinterprets finishLOD and distanceToFinishLOD. Defined here so pjvQueryLODFootprint can name it;
// the reasoning is at computeTargetLOD.
#define PJV_LOD_FOOTPRINT 255u

// ============================================================================================
// LOD IS NOT A RENDERING PERFORMANCE LEVER. Read this before tuning the fields below.
// ============================================================================================
//
// The LOD fields are a lever for **VRAM usage** (the CPU-side path, where a coarser storage LOD
// means fewer nodes resident) and for **anti-aliasing** (the shader-side path, where marching at a
// coarser level filters high-frequency voxel detail). They are NOT a frame-time dial.
//
// Changing startLOD / finishLOD / distanceToFinishLOD has very little effect on rendering
// performance. If a renderer here is slower than another, the difference is not in these fields,
// and tuning them will not recover it. Look instead at which GPU and driver were actually selected
// (integrated vs discrete, or a software Vulkan fallback such as lavapipe), at render target
// resolution and count, at the number of passes, and at whether the cost is CPU-side work --
// generation, upload, repacking -- rather than GPU-side. An order-of-magnitude gap is never one of
// these constants; it is the wrong device or a pathological stall.
//
// What these fields CAN legitimately explain is a **black screen**. A ray that exhausts maxRaySteps
// before reaching geometry reports a miss, and a renderer without a sky model draws that as black --
// while running fast, because nothing was traversed. That is a correctness question, not a
// performance one, and a suspiciously *low* frame time is evidence for it rather than against.
//
// This note exists because the mistake has been made repeatedly. Please leave it here.
// ============================================================================================

struct RayQuery {
    // Set by the constructors; validated by the scene query. See the note above.
    uint  magic;
    uint  flags;                  // PJV_Q_*

    // ---- pre-existing fields. Assigned explicitly by every call site, migrated or not. ----
    // See the LOD note above: these tune VRAM and anti-aliasing, not frame time.
    uint startLOD;
    uint finishLOD;
    uint distanceToFinishLOD;     // Measured in voxels. In footprint mode: the crossover distance.
    uint maxRaySteps;

    // ---- added, and therefore only trustworthy when `magic` checks out ----
    // How many transparent layers the ray may cross before it gives up. Runtime, not a per-shader
    // #define: the bound the loops unroll against is MAX_PEEL_ITERATIONS and is a ceiling, while this
    // is the budget, and the two were previously the same number in four different files.
    uint  maxTransparentLayers;
    // How many times a ray may be bent by refraction. 0 reproduces a pure filter exactly.
    uint  maxRefractionSegments;
    // World units. Nothing beyond this can affect the answer; 1e30 means "the whole ray".
    float maxDistance;
    // World units. Past this, animated geometry is drawn at its rest position rather than resolved.
    // A shadow ray wants a much smaller value than a camera ray -- sway reads as sway in the contact
    // region, and a metre up the ray the occluder is present either way.
    float animResolveDistance;
    // Advanced by the peel's stochastic interface decision. Pass a per-pixel, per-frame value.
    uint  seed;
    // Transmittance below which the peel abandons the ray as fully absorbed.
    //
    // Was a hardcoded 0.002 in pjvPeelConsume, which is right for a renderer: -27 dB, and nothing
    // behind that survives to be seen. It is wrong for anything measuring transmitted energy
    // rather than looking at it -- a link budget spans well over a hundred dB, and three concrete
    // walls are already past the old floor, so a genuinely weak result and a fully blocked one
    // became indistinguishable. Defaulted to 0.002 by every constructor, so nothing that predates
    // this field changes by a bit.
    float minTransmittance;
};

// The three constructors. One of these, or a copy of one, is the only sanctioned way to get a query.
//
// They differ only in what a caller of that shape always wants, and they are deliberately three
// rather than one-with-arguments: the name at the call site says what kind of ray this is, which is
// the thing a reader wants to know and the thing a default cannot express.
RayQuery pjvRayQueryRaw(uint maxSteps) {
    RayQuery q;
    q.magic = PJV_QUERY_MAGIC;
    q.flags = 0u;
    q.startLOD = 0u;
    q.finishLOD = 0u;
    q.distanceToFinishLOD = 0u;
    q.maxRaySteps = maxSteps;
    q.maxTransparentLayers = 0u;
    q.maxRefractionSegments = 0u;
    q.maxDistance = 1e30;
    q.animResolveDistance = 0.0;
    q.seed = 0u;
    q.minTransmittance = 0.002;
    return q;
}
// A camera or reflection ray: wants the nearest surface, over the whole ray.
RayQuery pjvPrimaryQuery(uint maxSteps) { return pjvRayQueryRaw(maxSteps); }
// A bounce ray. Same shape as a primary today; named apart because its budgets diverge in practice
// and a call site should say which it is.
RayQuery pjvBounceQuery(uint maxSteps)  { return pjvRayQueryRaw(maxSteps); }
// A shadow ray: bounded, and after transmittance rather than a surface.
RayQuery pjvShadowQuery(uint maxSteps, float maxDistance) {
    RayQuery q = pjvRayQueryRaw(maxSteps);
    q.flags |= PJV_Q_OCCLUSION_ONLY;
    q.maxDistance = maxDistance;
    return q;
}

// Opt-ins. Each one both sets its flag and supplies the budget that flag needs, so it is not possible
// to enable a feature and leave its bound at garbage -- which is the failure mode this whole API is
// shaped around.
void pjvQueryWantMaterial(inout RayQuery q) { q.flags |= PJV_Q_WANT_MATERIAL; }
void pjvQueryTransparency(inout RayQuery q, uint maxLayers, uint seed) {
    q.flags |= PJV_Q_TRANSPARENCY;
    q.maxTransparentLayers = maxLayers;
    q.seed = seed;
}
void pjvQueryRefraction(inout RayQuery q, uint maxSegments) {
    q.flags |= PJV_Q_REFRACTION;
    q.maxRefractionSegments = maxSegments;
}
void pjvQueryAnimation(inout RayQuery q, float resolveDistance) {
    q.flags |= PJV_Q_ANIMATION;
    q.animResolveDistance = resolveDistance;
}
void pjvQueryAnimationDebugSolid(inout RayQuery q) { q.flags |= PJV_Q_ANIM_DEBUG_SOLID; }
void pjvQueryLinearLOD(inout RayQuery q, uint finishLOD, uint rampDistanceVoxels) {
    q.startLOD = 0u;
    q.finishLOD = finishLOD;
    q.distanceToFinishLOD = rampDistanceVoxels;
}
void pjvQueryLODFootprint(inout RayQuery q, uint crossoverVoxels, uint coarsestLevel) {
    q.startLOD = PJV_LOD_FOOTPRINT;
    q.finishLOD = coarsestLevel;
    q.distanceToFinishLOD = crossoverVoxels;
}

// Whether this query came from a constructor. A false answer is not fatal -- see the clamp in
// pjvValidateQuery -- but it is always worth reporting.
bool pjvQueryIsValid(RayQuery q) { return q.magic == PJV_QUERY_MAGIC; }

// Force an unconstructed query into a shape the traversal can trust. Only the fields added alongside
// `magic` are touched; see the note above for why the older four are left as the caller set them.
RayQuery pjvValidateQuery(RayQuery q) {
    if (q.magic == PJV_QUERY_MAGIC) return q;
    q.magic = PJV_QUERY_MAGIC;
    q.flags = 0u;
    q.maxTransparentLayers = 0u;
    q.maxRefractionSegments = 0u;
    q.maxDistance = 1e30;
    q.animResolveDistance = 0.0;
    q.seed = 0u;
    q.minTransmittance = 0.002;
    return q;
}

// User defined return struct from render function.
struct returnStruct {
    vec4 globalIllumination;
    vec4 ambientOcclusion;
    vec4 directIllumination;
    vec4 normal;
    vec4 albedo;
    vec4 motionVector;
};

//Box structure with a position(min of x, y, z) and a size.
struct BoxAABB {
    vec3 position;
    float size;
};

struct Tree64NodeData {
    uint data1;
    uint data2;
    uint data3;
    uint childPtr;
};

// One entry of the descent stack. Both fields are live and both are needed: `dataIndex` is read at
// arbitrary depth (a pop reads nodeStack[n - 2 - boundariesCrossed]) and the z-order is read at the
// top of the stack.
//
// A third field, `cachedData` (a whole Tree64NodeData, 4 uints), used to sit here. Nothing in this
// file ever read or wrote it -- the march keeps the current node's data in a plain local, `data` --
// so it was 4 dead uints per entry, 20 across the stack. That is not free the way dead scalars are:
// nodeStack is a dynamically-indexed array, which is what stops the compiler from proving the field
// unused and eliminating it, so the array either lands in scratch memory or forces 30 dwords into
// indexable registers. Either one is paid in the DDA's inner loop.
struct CombinedNode64 {
    //BoxAABB boundingBox; // Every node has one.
    uint thisNodeZOrderInParent; // Root node won't have one since it has no parent.
    uint dataIndex; // Every node will have one except for the candidate node.
};

struct PushResults {
    bool continueMarching;
    bool successfullyPushed;
};

struct AdvanceResults {
    bool continueMarching;
    bool leaveChild;
};

//Ray structure with an origin, direction, and color.
struct Ray {
    vec3 origin;
    vec3 direction;
};

struct IntersectionResult {
    float distance;    // Distance from ray origin to intersection point
                       // -1.0f means no intersection
                       // 0.0f means ray starts inside box (for entry function)
    vec3 normal;       // Normal vector at intersection point
                       // {0, 0, 0} means no intersection or ray starts inside box
                       // For entry normals: points OUTWARD from box face
                       // For exit normals: points OUTWARD from box face
};

struct TraversalNode {
    BoxAABB boundingBox;
    uint ZOrderInParent; // (0, 0), (1, 0), (0, 1)... Will go up to 16 for branching factor of 4.
    //uint dataIndex;
};

struct chunkHeader {
    uint chunkID;
    float positionX;
    float positionY;
    float positionZ;
    float scale;
    uint resolution;
    uint geometryStartIndex;
    uint geometryEndIndex;
    uint materialIDStartIndex;
    uint materialIDEndIndex;
    uint dataRefID;
    uint paletteOffset;
    vec4 rotation;
    // Levels dropped below `resolution` this chunk instance's traversal should cap itself at, on
    // top of whatever the ray-distance-based cutoff (computeTargetLOD) already allows. See
    // marchRayThroughTree64_DDA's cutoff check and makeHeader (gpu_interface.cpp) for how this is
    // computed on the CPU side.
    uint traversalLOD;
    // ---- The animation envelope (see DataBlock in compose.h) --------------------------------
    // A SECOND tree64 for this chunk, at a quarter of `resolution`, marking where animated geometry
    // may be drawn. It lives in the same tree64Data texture through its own range, which is why it
    // needs no sampler stage of its own -- there were none left to spend.
    //
    // `envelopeNodeCount == 0` means this chunk has no envelope, and that is the field to test:
    // envelopeStartIndex 0 is a perfectly valid place for the first blob's range to sit.
    uint envelopeStartIndex;
    uint envelopeNodeCount;
    uint envelopeMotionStartIndex;   // BYTE offset into materialIDs, as materialIDStartIndex is
};

// Does this chunk carry an animation envelope at all? Almost never, and the traversal is written so
// that answering "no" costs one comparison.
bool chunkHasEnvelope(chunkHeader h) { return h.envelopeNodeCount > 0u; }

struct Voxel {
    uint index;
    vec3 color;
    vec3 normal;
};

struct SceneIntersectData {
    uint steps;
    uint headerIndex;
    BoxAABB foundBox;
    // Authoritative hit data straight from the DDA's own arithmetic. Callers must use
    // these instead of re-intersecting foundBox analytically: a fresh slab test on a
    // boundary-exact hit disagrees with the march by ULPs and misclassifies the hit.
    float rayT;  // Distance to the hit along the ray. -1.0 on a miss.
    vec3 normal; // Outward normal of the face the ray entered through. (0,0,0) when the
                 // ray started inside the volume and hit before its first DDA step.
    // The cell the march actually landed on, in the chunk's own voxel space, exactly as the
    // DDA held it. **This is what a material lookup must use.**
    //
    // foundBox.position carries the same cell, but converted to world space (rotated and
    // translated by the chunk's transform), and recovering an integer back out of it is a
    // float32 round trip that does not survive. Two things go wrong at once: the local offset
    // is added to the world translation P and subtracted back -- twice, since fetchVoxelColor
    // adds P and then immediately subtracts it again -- which loses low bits in proportion to
    // |P|; and the result is truncated rather than rounded, so an error of a single ULP below
    // an exact integer drops the coordinate a whole cell.
    //
    // What that looks like is a voxel shaded with its neighbour's colour: subtle, scattered
    // among correct voxels, stable for a given transform, and absent at the origin -- which is
    // why it only showed up once something had been moved or rotated, and why CPU picking
    // (which keeps its own integer) always disagreed. Measured over a chunk's voxels: 6% wrong
    // for a small translation, 10% for a rotation alone, 39% for both.
    //
    // The DDA has the exact integer the whole way through. Carry it rather than rebuild it.
    ivec3 voxelCoord;
    // Distance at which the ray LEAVES the cell it hit, in the same units and from the same origin as
    // rayT, so `exitT - rayT` is exactly how far the ray travelled inside that cell.
    //
    // Carried for the same reason voxelCoord is. Rebuilding it outside -- turning foundBox back into a
    // world AABB and slab-testing it -- looks equivalent and is not: on a near-axis-aligned ray the
    // near-zero direction components make 1/direction enormous, and a slab test takes the MINIMUM
    // across all three axes, so an axis the ray barely traverses contributes a spurious bound. Which
    // way it is spurious flips with the sign of that component, so the error shows up as a hard split
    // down the middle of the screen where the sign changes. The march knows the answer exactly.
    float exitT;
    // Flat index of this voxel's entry in the materialIDs texture, and the palette base to add to
    // what that entry holds. Together they are everything fetchVoxelMaterialFromHit needs, which is
    // why they are here: they turn a material lookup into two texelFetches.
    //
    // Carried for the same reason voxelCoord and exitT are, and with more to gain than either. The
    // alternative -- fetchVoxelMaterialAtCoord -- re-fetches the chunk header (5 texels) and then
    // re-descends the tree from the root, one texel and one popcount per level, to reach the leaf the
    // march was standing on when it returned. The march already had that leaf's occupancy mask and
    // material offset in hand, and the voxel's index within it on the top of the stack; the whole
    // descent exists to recover values that were live one function call earlier.
    //
    // That is paid on every hit of every ray in every renderer, and TWICE per transparent layer in
    // the peel (once to decide whether the layer is opaque, once for the surface that stops it), so
    // it is the redundant work that scales worst with what the peel and the bounce loop ask for.
    //
    // MATERIAL_INDEX_NEEDS_DESCENT means the march could not resolve it and the caller must fall back
    // to fetchVoxelMaterialAtCoord. That is the LOD-cutoff hit: a coarsened interior node returned as
    // wholly solid has no single material to name, and the fallback's behaviour (the material of the
    // node's minimum-corner voxel) is what those hits have always shaded as.
    uint materialListIndex;
    uint paletteOffset;
};

// See SceneIntersectData::materialListIndex. Not a valid index -- the material list is nowhere near
// this long, and a hit that carries it has no leaf to read.
#define MATERIAL_INDEX_NEEDS_DESCENT 0xFFFFFFFFu

// ...and the other thing a materialListIndex can be: a LOCAL PALETTE SLOT, named directly, rather
// than an entry in the materialIDs array to be read to find one.
//
// Needed by exactly one producer, and it is not an optimisation. The advection resolve picks a
// material by walking a chain from the source's palette entry (blue base -> yellow body -> orange
// tip, indexed by how far the parcel has travelled), and the slot it arrives at BELONGS TO NO VOXEL:
// nothing in the geometry references it, so there is no materialIDs entry whose byte holds it and no
// leaf-relative index that could name it. The alternative is a second material field on
// SceneIntersectData, which every one of the two dozen sites that build a miss record would then have
// to initialise -- and one that forgot would shade a hit from garbage.
//
// Tagged rather than ranged: real leaf offsets are dense from zero and a chunk's material array is
// nowhere near a billion entries, so the bit is free. Test it AFTER the descent sentinel, which has
// every bit set and would otherwise be read as a direct slot for palette entry 0xFFFF.
#define PJV_MATERIAL_DIRECT_SLOT 0x40000000u
#define PJV_MATERIAL_SLOT_MASK   0x0000FFFFu

static CombinedNode64 nodeStack[5];
static uint nodeStackQuantity = 0;

// LUT's
const uint MOVE_LUT[448] = {
    0u,     1u,     0u,     2u,     0u,     4u,     0u,
    1u,     8u,     0u,     3u,     1u,     5u,     1u,
    2u,     3u,     2u,     16u,     0u,     6u,     2u,
    3u,     10u,     2u,     17u,     1u,     7u,     3u,
    4u,     5u,     4u,     6u,     4u,     32u,     0u,
    5u,     12u,     4u,     7u,     5u,     33u,     1u,
    6u,     7u,     6u,     20u,     4u,     34u,     2u,
    7u,     14u,     6u,     21u,     5u,     35u,     3u,
    8u,     9u,     1u,     10u,     8u,     12u,     8u,
    9u,     9u,     8u,     11u,     9u,     13u,     9u,
    10u,     11u,     3u,     24u,     8u,     14u,     10u,
    11u,     11u,     10u,     25u,     9u,     15u,     11u,
    12u,     13u,     5u,     14u,     12u,     40u,     8u,
    13u,     13u,     12u,     15u,     13u,     41u,     9u,
    14u,     15u,     7u,     28u,     12u,     42u,     10u,
    15u,     15u,     14u,     29u,     13u,     43u,     11u,
    16u,     17u,     16u,     18u,     2u,     20u,     16u,
    17u,     24u,     16u,     19u,     3u,     21u,     17u,
    18u,     19u,     18u,     18u,     16u,     22u,     18u,
    19u,     26u,     18u,     19u,     17u,     23u,     19u,
    20u,     21u,     20u,     22u,     6u,     48u,     16u,
    21u,     28u,     20u,     23u,     7u,     49u,     17u,
    22u,     23u,     22u,     22u,     20u,     50u,     18u,
    23u,     30u,     22u,     23u,     21u,     51u,     19u,
    24u,     25u,     17u,     26u,     10u,     28u,     24u,
    25u,     25u,     24u,     27u,     11u,     29u,     25u,
    26u,     27u,     19u,     26u,     24u,     30u,     26u,
    27u,     27u,     26u,     27u,     25u,     31u,     27u,
    28u,     29u,     21u,     30u,     14u,     56u,     24u,
    29u,     29u,     28u,     31u,     15u,     57u,     25u,
    30u,     31u,     23u,     30u,     28u,     58u,     26u,
    31u,     31u,     30u,     31u,     29u,     59u,     27u,
    32u,     33u,     32u,     34u,     32u,     36u,     4u,
    33u,     40u,     32u,     35u,     33u,     37u,     5u,
    34u,     35u,     34u,     48u,     32u,     38u,     6u,
    35u,     42u,     34u,     49u,     33u,     39u,     7u,
    36u,     37u,     36u,     38u,     36u,     36u,     32u,
    37u,     44u,     36u,     39u,     37u,     37u,     33u,
    38u,     39u,     38u,     52u,     36u,     38u,     34u,
    39u,     46u,     38u,     53u,     37u,     39u,     35u,
    40u,     41u,     33u,     42u,     40u,     44u,     12u,
    41u,     41u,     40u,     43u,     41u,     45u,     13u,
    42u,     43u,     35u,     56u,     40u,     46u,     14u,
    43u,     43u,     42u,     57u,     41u,     47u,     15u,
    44u,     45u,     37u,     46u,     44u,     44u,     40u,
    45u,     45u,     44u,     47u,     45u,     45u,     41u,
    46u,     47u,     39u,     60u,     44u,     46u,     42u,
    47u,     47u,     46u,     61u,     45u,     47u,     43u,
    48u,     49u,     48u,     50u,     34u,     52u,     20u,
    49u,     56u,     48u,     51u,     35u,     53u,     21u,
    50u,     51u,     50u,     50u,     48u,     54u,     22u,
    51u,     58u,     50u,     51u,     49u,     55u,     23u,
    52u,     53u,     52u,     54u,     38u,     52u,     48u,
    53u,     60u,     52u,     55u,     39u,     53u,     49u,
    54u,     55u,     54u,     54u,     52u,     54u,     50u,
    55u,     62u,     54u,     55u,     53u,     55u,     51u,
    56u,     57u,     49u,     58u,     42u,     60u,     28u,
    57u,     57u,     56u,     59u,     43u,     61u,     29u,
    58u,     59u,     51u,     58u,     56u,     62u,     30u,
    59u,     59u,     58u,     59u,     57u,     63u,     31u,
    60u,     61u,     53u,     62u,     46u,     60u,     56u,
    61u,     61u,     60u,     63u,     47u,     61u,     57u,
    62u,     63u,     55u,     62u,     60u,     62u,     58u,
    63u,     63u,     62u,     63u,     61u,     63u,     59u
};


float getRayBoxEntryDistance(Ray ray, BoxAABB box) {
    IntersectionResult r;
    if (box.size <= 0) {
        return -1.0;
    }

    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);

    // Inside check (required to preserve behavior)
    if (all(greaterThanEqual(ray.origin, boxMin)) &&
        all(lessThanEqual(ray.origin, boxMax))) {
        return 0;
    }

    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    vec3 tNearVec = min(t0, t1);
    vec3 tFarVec  = max(t0, t1);

    float tNear = max(max(tNearVec.x, tNearVec.y), tNearVec.z);
    float tFar  = min(min(tFarVec.x,  tFarVec.y),  tFarVec.z);

    // Miss test (cannot be removed)
    if (tNear > tFar || tFar < 0.0) {
        return -1;
    }
    return tNear;
}

float getRayBoxEntryDistanceForSureHit(Ray ray, BoxAABB box) {
    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);

    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    vec3 tNearVec = min(t0, t1);

    // Entry distance = max component of tNearVec
    float tNear = max(max(tNearVec.x, tNearVec.y), tNearVec.z);

    // Clamp to zero to preserve behavior for rays starting inside
    return max(tNear, 0.0);
}

IntersectionResult getRayBoxEntry(Ray ray, BoxAABB box) {
    IntersectionResult r;
    if (box.size <= 0) {
        r.distance = -1.0;
        r.normal = vec3(0.0);
        return r;
    }

    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);

    // Inside check (required to preserve behavior)
    if (all(greaterThanEqual(ray.origin, boxMin)) &&
        all(lessThanEqual(ray.origin, boxMax))) {
        r.distance = 0.0;
        r.normal   = vec3(0.0);
        return r;
    }

    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    vec3 tNearVec = min(t0, t1);
    vec3 tFarVec  = max(t0, t1);

    float tNear = max(max(tNearVec.x, tNearVec.y), tNearVec.z);
    float tFar  = min(min(tFarVec.x,  tFarVec.y),  tFarVec.z);

    // Miss test (cannot be removed)
    if (tNear > tFar || tFar < 0.0) {
        r.distance = -1.0;
        r.normal   = vec3(0.0);
        return r;
    }

    // Normal selection (branch-minimal, exact)
    vec3 normal = vec3(0.0);

    
    if (tNear == tNearVec.x)
        normal = vec3(-sign(ray.direction.x), 0.0, 0.0);
    else if (tNear == tNearVec.y)
        normal = vec3(0.0, -sign(ray.direction.y), 0.0);
    else
        normal = vec3(0.0, 0.0, -sign(ray.direction.z));
    

    r.distance = tNear;
    r.normal   = normal;
    return r;
}

IntersectionResult getRayBoxExit(Ray ray, BoxAABB box) {
    IntersectionResult r;
    if (box.size < 0) {
        r.distance = -1.0;
        r.normal = vec3(0.0);
        return r;
    }

    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);

    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    vec3 tNearVec = min(t0, t1);
    vec3 tFarVec  = max(t0, t1);

    float tNear = max(max(tNearVec.x, tNearVec.y), tNearVec.z);
    float tFar  = min(min(tFarVec.x,  tFarVec.y),  tFarVec.z);

    if (tNear > tFar || tFar < 0.0) {
        r.distance = -1.0;
        r.normal   = vec3(0.0);
        return r;
    }

    vec3 normal = vec3(0.0);

    if (tFar == tFarVec.x)
        normal = vec3(sign(ray.direction.x), 0.0, 0.0);
    else if (tFar == tFarVec.y)
        normal = vec3(0.0, sign(ray.direction.y), 0.0);
    else
        normal = vec3(0.0, 0.0, sign(ray.direction.z));

    r.distance = tFar;
    r.normal   = normal;
    return r;
}

IntersectionResult getRayBoxExitForSureHit(Ray ray, BoxAABB box) {
    IntersectionResult r;

    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);

    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    vec3 tFarVec = max(t0, t1);

    float tFar = min(min(tFarVec.x, tFarVec.y), tFarVec.z);
    r.distance = tFar;

    // Normal selection (branch-minimal)
    if (tFar == tFarVec.x)
        r.normal = vec3(sign(ray.direction.x), 0.0, 0.0);
    else if (tFar == tFarVec.y)
        r.normal = vec3(0.0, sign(ray.direction.y), 0.0);
    else
        r.normal = vec3(0.0, 0.0, sign(ray.direction.z));

    return r;
}


float getRayBoxExitDistanceForSureHit(Ray ray, BoxAABB box) {
    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);
    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    // tFar = distance to exit the box
    vec3 tFarVec = max(t0, t1);
    return min(min(tFarVec.x, tFarVec.y), tFarVec.z);
}


uint tree64s(int index) {
    uint w = uint(tree64Dims.x);
    uint shift = uint(tree64Dims.y);
    uint pixelIndex = uint(index) >> 2u;
    int x = int(pixelIndex & (w - 1u));
    int y = int(pixelIndex >> shift);
    int colorIndex = int(uint(index) & 3u);
    uvec4 pixel = texelFetch(tree64Data, ivec2(x, y), 0);
    return pixel[colorIndex];
}

uint materialID(uint index) {
    uint w = uint(voxelTypeDims.x);
    uint shift = uint(voxelTypeDims.y);
    uint pixelIndex = uint(index) >> 2u;
    int x = int(pixelIndex & (w - 1u));
    int y = int(pixelIndex >> shift);
    int colorIndex = int(uint(index) & 3u);
    uvec4 pixel = texelFetch(materialIDs, ivec2(x, y), 0);
    return pixel[colorIndex];
}

// One palette entry is one RGBA32U texel: the four words of projv::Material. The palette used to
// pack four entries into a texel and select one component out of the fetch, so every material
// property added since then has been free -- the fetch was always returning four words and throwing
// three of them away. See the Material comment in data_structures/scene.h for the word layout and,
// more importantly, for why all-zero words 1-3 mean "shade this exactly as the colour-only pipeline
// did", which is what lets every compose.json already on disk render unchanged.
uvec4 materialPaletteTexel(uint index) {
    uint w = uint(paletteDims.x);
    uint shift = uint(paletteDims.y);
    int x = int(index & (w - 1u));
    int y = int(index >> shift);
    return texelFetch(materialPalette, ivec2(x, y), 0);
}

// Everything a shader can know about a voxel's surface. Decoded from that one texel; nothing in
// here costs a second fetch.
struct VoxelMaterial {
    vec3  albedo;
    vec3  emission;       // Already scaled by strength, so this is radiance rather than a colour.
    float glossiness;     // 0 = fully rough (Lambertian), 1 = mirror.
    float metallic;       // 0 = dielectric, 1 = conductor (the specular lobe takes the albedo tint).
    float transparency;   // 0 = opaque. Read by the traversal -- see PeelAccum / raySceneIntersect.
    float ior;            // 1.0 = no refraction.
    float transmission;
    uint  flags;
};

vec3 unpackPaletteRGB10(uint packed) {
    return vec3(float((packed >> 20u) & 0x3FFu),
                float((packed >> 10u) & 0x3FFu),
                float( packed         & 0x3FFu)) / 1023.0;
}

// NOTE ON TRANSPARENCY: acting on `transparency` is not a shading change -- it is a traversal change,
// because every march in this file stops at the first solid voxel. The traversal now makes it: a
// query with PJV_Q_TRANSPARENCY reads the material at a leaf and decides there whether the voxel stops
// the ray. The material fetch that costs is also the one the caller would have done anyway, so the
// work moved rather than doubling -- see SceneHit::material.
//
// `ior` and `transmission` are still decoded and carried without being acted on. That one IS a
// separate change: a refracted ray changes direction, so it is a new ray rather than a continuation
// of the same march. PJV_Q_REFRACTION is where it will hang.
// The material of nothing: black, non-emissive, fully rough, opaque. By the zero rule this is what a
// palette entry of all zeroes decodes to, so "empty" and "never had properties set" agree.
VoxelMaterial emptyVoxelMaterial() {
    VoxelMaterial m;
    m.albedo = vec3(0.0);
    m.emission = vec3(0.0);
    m.glossiness = 0.0;
    m.metallic = 0.0;
    m.transparency = 0.0;
    m.ior = 1.0;
    m.transmission = 0.0;
    m.flags = 0u;
    return m;
}
bool pjvMaterialIsAnimated(VoxelMaterial m) { return (m.flags & PJV_MAT_FLAG_ANIMATED) != 0u; }

// ...and specifically by ADVECTION. Answered from the motion table rather than from a flag bit,
// because "which kind of field moves this" is a property of the field and the material only names
// one. The prototype spent a palette flag bit on it and had to keep the two in step by hand.
bool pjvMaterialIsAdvected(VoxelMaterial m) {
    if ((m.flags & PJV_MAT_FLAG_ANIMATED) == 0u) return false;
    return pjvMotionSet((m.flags >> PJV_MAT_MOTION_SHIFT) & PJV_MAT_MOTION_MASK).kind == PJV_MOTION_ADVECT;
}

VoxelMaterial decodeMaterial(uvec4 texel) {
    VoxelMaterial m;
    m.albedo = unpackPaletteRGB10(texel.x);

    // Exponential, matching projv::unpackEmissiveStrength on the CPU: raw 0 is exactly zero, so a
    // legacy entry is not an emitter, and 1..255 spans roughly 0.004 to 245 with raw 128 sitting at
    // 1.0. A linear byte would put every usable dim emitter inside its first step.
    uint strengthRaw = (texel.w >> 24u) & 0xFFu;
    float strength = strengthRaw == 0u ? 0.0 : exp2(float(strengthRaw) / 16.0 - 8.0);
    // A zero emission word means "emit in the albedo's colour" -- emitting black is meaningless, so
    // zero is free to carry that. Keeps the strength control alive on a material that has never had
    // an emission colour picked, without giving up independent emission for the surfaces that need
    // it (a black voxel cannot glow if emission is albedo * strength). See
    // projv::materialEffectiveEmissionColor.
    vec3 emissionColour = texel.y == 0u ? m.albedo : unpackPaletteRGB10(texel.y);
    m.emission = emissionColour * strength;

    m.glossiness   = float((texel.z >> 24u) & 0xFFu) / 255.0;
    m.metallic     = float((texel.z >> 16u) & 0xFFu) / 255.0;
    m.transparency = float((texel.z >>  8u) & 0xFFu) / 255.0;
    m.ior          = 1.0 + float(texel.z & 0xFFu) / 128.0;
    m.transmission = float((texel.w >> 16u) & 0xFFu) / 255.0;
    m.flags        = (texel.w >> 8u) & 0xFFu;
    return m;
}

// =============================================================================
// SceneHit -- what a scene query answers with
// =============================================================================
//
// One record for every kind of ray. It supersedes the bare SceneIntersectData a caller used to get
// back from raySceneIntersect and the separate PeeledHit the transparency path returned, and it is a
// superset of both: on an opaque, unanimated scene queried without any flags, `transmittance` is
// exactly one, `emission` is exactly zero, and `hit` carries what it always carried.
//
// It also replaces the file-scope statics a traversal with side effects would otherwise need. Those
// are what the animation prototype grew (eleven of them, each with hand-written save/restore across
// the chunk loop, because whichever chunk marched LAST wrote them and that is not necessarily the
// chunk that won on distance). A returned struct cannot have that bug: the loser's copy is simply
// not the one assigned.
struct SceneHit {
    // Where the ray stopped. `hit.rayT < 0` or `hit.foundBox.size < 0` is a miss, unchanged.
    SceneIntersectData hit;
    // The material at `hit`. Populated only when the query asked -- PJV_Q_WANT_MATERIAL, or
    // PJV_Q_TRANSPARENCY, which implies it. A caller that asked should use this rather than calling
    // fetchVoxelMaterialFromHit: the traversal had the leaf in registers when it decided, so this
    // costs no second descent, and the work MOVES out of the caller rather than doubling.
    VoxelMaterial material;
    // False when the traversal could not name a single material for the hit -- a coarsened LOD node
    // spans many voxels and has no one material. The caller must descend on `hit.voxelCoord`, which
    // is what MATERIAL_INDEX_NEEDS_DESCENT has always meant. Carried as a bool as well because a
    // caller reading `material` should not have to know the sentinel.
    bool  materialValid;
    // Product of every transparent layer crossed on the way. Multiply the radiance arriving from
    // `hit` -- or from the sky, on a miss -- by this.
    vec3  transmittance;
    // Radiance emitted BY those layers, each already attenuated by the layers in front of it. Add it;
    // do not scale it by `transmittance` again.
    vec3  emission;
    uint  layers;      // transparent layers actually crossed
    uint  segments;    // refraction bends actually taken
    // ---- THE RAY THE HIT WAS ACTUALLY FOUND ON ----------------------------------------------
    //
    // Identical to the ray handed in unless refraction bent it, and `pathLength` is then identical to
    // hit.rayT. So a caller that never sets PJV_Q_REFRACTION can ignore both fields and nothing about
    // its arithmetic changes.
    //
    // A caller that DOES ask for refraction cannot ignore them, and the failure is silent: the
    // near-universal way to recover a hit position is `ray.origin + ray.direction * hit.rayT`, and
    // after a bend that names a point in empty space along the original direction. hit.rayT is
    // measured along the LAST segment, because that is the march that produced it and every other
    // field on the record (the normal, the exit, the box) belongs to that same march.
    // ---- DIAGNOSTIC: what the bend decision saw, the last time it was reached -----------------
    //
    // Refraction has exactly one gate, and when it declines there is nothing downstream to look at --
    // the ray simply carries on straight and the picture is wrong somewhere else entirely. These two
    // fields report the gate's own inputs so a renderer can colour by WHICH clause said no.
    //
    //   bit 0  the decision was reached at all (the layer was consumed as transparent rather than
    //          stopping the ray). Clear means the peel called this voxel OPAQUE, which is a very
    //          different bug from the bend declining it.
    //   bit 1  peel.crossedInterface -- the peel called it an interface rather than more of a body
    //   bit 2  the segment budget still had room
    //   bit 3  the material's IOR differed from 1 by more than the byte's own resolution
    // A bend happened iff all four are set.
    uint  diagBendFlags;
    float diagBendIor;
    Ray   finalRay;
    // Total distance travelled along the whole polyline, which is what depth, fog and any
    // distance-based LOD want. Equal to hit.rayT when nothing bent.
    float pathLength;
    uint  stopReason;  // PJV_STOP_*
    // How many cells the traversal declined to draw anything in after an animation resolve said
    // nothing was there. Diagnostic only: a ray that crossed forty target cells and resolved none of
    // them has a resolve problem, while a ray that reached the sky without meeting one never got near
    // the geometry -- and the two look identical on screen.
    uint  diagResolveMiss;
};

// Why a query stopped. Free to compute, and the only way to tell the failure modes apart from the
// outside: giving up with transmittance near 1 reads on screen as "the voxels are not there", giving
// up with it near 0 reads as "black", and the two are one line apart in the peel.
#define PJV_STOP_OPAQUE        0u  // reached an opaque surface -- the good case
#define PJV_STOP_NO_GEOMETRY   1u  // ran out of geometry; nothing behind
#define PJV_STOP_LAYER_BUDGET  2u  // spent maxTransparentLayers
#define PJV_STOP_TMIN_STALL    3u  // the resume floor failed to advance
#define PJV_STOP_ABSORBED      4u  // fully absorbed; nothing behind can contribute
#define PJV_STOP_ITERATIONS    5u  // spent MAX_PEEL_ITERATIONS
#define PJV_STOP_BAD_QUERY     6u  // the RayQuery did not come from a constructor -- see pjvValidateQuery
#define PJV_STOP_STEPS         7u  // a march spent maxRaySteps without reaching an answer
#define PJV_STOP_SEGMENTS      8u  // spent maxRefractionSegments

SceneHit pjvEmptySceneHit() {
    SceneHit r;
    r.hit.foundBox.position = vec3(0.0);
    r.hit.foundBox.size = -1.0;
    r.hit.rayT = -1.0;
    r.hit.exitT = -1.0;
    r.hit.normal = vec3(0.0);
    r.hit.voxelCoord = ivec3(0);
    r.hit.steps = 0u;
    r.hit.headerIndex = 0u;
    r.hit.materialListIndex = MATERIAL_INDEX_NEEDS_DESCENT;
    r.hit.paletteOffset = 0u;
    r.material = emptyVoxelMaterial();
    r.materialValid = false;
    r.transmittance = vec3(1.0);
    r.emission = vec3(0.0);
    r.layers = 0u;
    r.segments = 0u;
    r.stopReason = PJV_STOP_NO_GEOMETRY;
    r.diagResolveMiss = 0u;
    r.diagBendFlags = 0u;
    r.diagBendIor = 1.0;
    r.finalRay.origin = vec3(0.0);
    r.finalRay.direction = vec3(0.0, 0.0, 1.0);
    r.pathLength = -1.0;
    return r;
}

// ---- Transparency primitives -------------------------------------------------------------
// Moved above the traversal (they used to sit with raySceneIntersectPeeled at the end of the
// file) because the march itself now consults them: a transparent layer that lies inside an
// interval no other component overlaps is consumed IN PLACE rather than by restarting the whole
// scene query. See PeelAccum and the safe-interval note in raySceneIntersectFrom.
// Compile-time bound the peel loops unroll against on the HLSL/SPIR-V path.
// RayQuery::maxTransparentLayers is the runtime limit and must not exceed this.
// How many times ONE ray may be bent, as a compile-time ceiling. RayQuery::maxRefractionSegments is
// the runtime budget and must not exceed this; 0 there reproduces a non-refracting traversal exactly,
// which is what every caller written before refraction existed gets.
//
// Small on purpose. Each segment is a fresh scene query, and the visual return falls off fast: the
// first bend is what makes glass look like glass, the second is what makes a sphere show what is
// behind it, and past four nobody can tell.
#ifndef MAX_REFRACTION_SEGMENTS
// Eight, not four, and the reason is that a BEND IS PER INTERFACE, NOT PER OBJECT. A single pane of
// glass costs two -- one entering, one leaving -- so a budget of two is exhausted by one pane and a
// budget of four by two. That is not a quality ceiling, it is a correctness one: a ray that runs out
// mid-body behaves differently from its neighbour that did not, and the boundary between them is a
// hard edge through the image.
//
// Free to raise, now that the bend is a branch inside the peel loop rather than a loop of its own:
// this is a clamp, not a trip count, so nothing unrolls against it. See the note at raySceneIntersect
// for why the outer segment loop had to go.
#define MAX_REFRACTION_SEGMENTS 8
#endif

#ifndef MAX_PEEL_ITERATIONS
#define MAX_PEEL_ITERATIONS 16
#endif

// Absorption through `segmentLength` world units of a material's INTERIOR. Chromatic only.
//
// The albedo is normalised so its brightest channel passes unattenuated, which is the whole point:
// depth then changes the transmitted COLOUR without changing how much light gets through. Thick red
// glass becomes more saturated red rather than dark red, and a neutral material is perfectly clear at
// any thickness.
//
// This deliberately carries no notion of "how transparent" the material is. That belongs to the
// INTERFACE, not the medium -- see the entering/interior split in raySceneIntersectPeeled. Folding the
// two together is what made a partially transparent surface unusable: `transparency` was applied once
// per voxel, so N voxels of depth attenuated by transparency^N and anything below 1.0 went black
// within a few voxels, faster still at grazing angles where a ray crosses more of them.
//
// An overall (achromatic) absorption coefficient would be a genuine medium property and belongs here
// eventually; `transmission` in Material's word3 is the field reserved for it.
vec3 mediumAbsorption(VoxelMaterial m, float segmentLength, float voxelSize) {
    float peak = max(max(m.albedo.r, max(m.albedo.g, m.albedo.b)), 1e-4);
    vec3 tint = clamp(m.albedo / peak, vec3(1e-6), vec3(1.0));

    // ---- The tint is a HUE. `transmission` is the MAGNITUDE. --------------------------------
    //
    // The tint above is normalised by its own peak, so a pale green and a deep green absorb
    // identically and only the colour of what survives differs. That is the right shape for a
    // colour, and it leaves "how strongly does this absorb at all" unexpressed -- which is exactly
    // the quantity an author reaches for. A metre of pool water and a metre of ink are the same
    // colour problem and completely different absorption problems.
    //
    // Peak normalisation also has a consequence that is invisible in a render and fatal outside
    // one: the BRIGHTEST channel always passes unattenuated, because tint.max is exactly 1 and
    // pow(1, anything) is 1. For a renderer that is desirable -- thick red glass should saturate
    // rather than darken. For any use that needs an absolute figure per channel it means the
    // least-absorbed channel is transmitted for free, however thick the material.
    //
    // So `transmission` is an achromatic extinction coefficient in nepers per WORLD UNIT, applied
    // on top of the tint. Zero still means "as before" -- exp(0) is 1, so every palette written
    // before this behaves identically and nothing on disk needs migrating.
    //
    // Exponential, matching unpackEmissiveStrength, because the useful range spans four decades:
    // a light haze and a solid wall are both real materials and a linear byte would put every
    // usable haze inside its first step. raw 1 is 0.004/unit, raw 128 is 1.0, raw 255 is 245.
    float extinction = m.transmission <= 0.0 ? 0.0 : exp2(m.transmission * 255.0 / 16.0 - 8.0);

    float opticalDepth = max(segmentLength / max(voxelSize, 1e-9), 0.0);
    return pow(tint, vec3(opticalDepth)) * exp(-extinction * max(segmentLength, 0.0));
}

// One round of PCG, so the peel can make the stochastic interface decision below. Named apart from any
// renderer's own generator on purpose; a caller passes its own seed in and gets it advanced.
float peelRandom(inout uint seed) {
    seed = seed * 747796405u + 2891336453u;
    uint word = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    return float((word >> 22u) ^ word) * (1.0 / 4294967296.0);
}

// Whether two hits are the same material, continuing through one body rather than meeting a new
// surface. Compared on the decoded values because two hits on one palette entry decode identically.
bool sameMedium(VoxelMaterial a, VoxelMaterial b) {
    return a.transparency == b.transparency && all(equal(a.albedo, b.albedo));
}

// How much of the voxel just hit the ray actually crossed, for the Beer-Lambert exponent above.
//
// foundBox is axis-aligned in WORLD space, which is exact for an unrotated chunk and an approximation
// for a rotated one, where the voxel is an oriented box. Clamped to the longest a ray can cross a
// cube of this size (sqrt(3) * edge) so a rotated chunk cannot produce an absurd thickness.
float peelSegmentLength(Ray ray, SceneIntersectData hit) {
    // Straight from the march's own arithmetic. The previous version rebuilt the voxel as a world AABB
    // and slab-tested it, which collapsed the segment to zero for near-axis-aligned rays, and did so on
    // only one side of the sign flip -- see SceneIntersectData::exitT.
    return clamp(hit.exitT - hit.rayT, 0.0, 1.7320508 * max(hit.foundBox.size, 1e-9));
}

// Where the next pass should start looking: just inside the voxel just consumed, so that voxel's own
// entry (exactly hit.rayT) is excluded while the next voxel's entry is not.
//
// A small fraction of the voxel rather than its exit plane, for two reasons. The comparison is
// strict, so setting tMin to the exit plane would also exclude the next voxel, whose entry sits at
// the same t. And the exit plane is only approximate on a rotated chunk, where overshooting would
// skip a layer outright -- undershooting merely costs an iteration.
float peelNextTMin(SceneIntersectData hit, float segmentLength) {
    float voxelSize = max(hit.foundBox.size, 1e-9);
    // Prefer a step that stays INSIDE the voxel being left: capped at half of what the ray actually
    // crossed, so a grazing ray that clips a sliver off a corner cannot have its step land beyond the
    // next voxel's entry and skip it.
    float step = min(voxelSize * 0.01, max(segmentLength, 0.0) * 0.5);

    // ...but the step must also be REPRESENTABLE. tMin is `hit.rayT + step`, and a float32 mantissa at
    // a ray distance of a few hundred resolves about 1e-4; a sliver crossing can ask for a step orders
    // of magnitude below that, so the addition returns hit.rayT unchanged and the very same voxel is
    // returned on the next pass. That is not a near miss -- it attenuates one voxel for every
    // remaining layer (black) or, when the sliver makes the exponent ~0 so each pass multiplies by 1,
    // for none of them (the voxel vanishes). Both show up as direction-dependent, because which faces
    // a ray grazes depends on where it is pointing.
    //
    // So the step is floored at a few ULP for the magnitude of rayT in play. Where that floor beats
    // the cap above, the sliver is skipped -- a corner graze carrying essentially no material, which
    // is the right thing to lose.
    float resolvable = max(abs(hit.rayT), 1.0) * 1e-6;
    return hit.rayT + max(step, resolvable);
}

// ---- PeelAccum: consuming a transparent layer without restarting the scene query ---------------
//
// The transparency loop at the bottom of this file works by re-issuing the WHOLE scene query with an
// advancing distance floor, and the note there explains why it has to: which surface is actually in
// front is decided in raySceneIntersectFrom, which brute-forces the loose chunks tracking
// closestDistance and only then marches the grids pruned by it. A march that peeled one chunk to
// completion would hand back that chunk's nearest opaque hit while a transparent voxel belonging to a
// DIFFERENT component sat in front of it, and two panes of glass in different components would
// composite in the wrong order.
//
// That argument rules out peeling a chunk to completion. It does not rule out peeling at all. What
// actually forces the restart is one specific thing: ANOTHER COMPONENT'S BOX OVERLAPPING THE INTERVAL
// ABOUT TO BE CONSUMED. Components are placed objects and rarely interpenetrate, so over most of most
// rays there is nothing to interleave with and the ordering question does not arise.
//
// So the interval is made explicit. raySceneIntersectFrom computes how far the ray can be walked
// while only ONE component's bounding volume is live (see the safe-interval note there) and hands it
// down as `limitT`. Inside that interval the march consumes a transparent voxel in place and keeps
// stepping in the same DDA -- no restart, no re-descent from the root. Outside it, the voxel is
// returned as a hit and the outer loop restarts exactly as it always did.
//
// The win is largest on precisely the content that used to be worst: a flame, a body of water, a
// thick glass object. Those are deep media inside a single component, so the restart count was equal
// to their depth in voxels, and it is now one.
//
// This struct is threaded through the traversal as `inout` rather than kept in file-scope statics.
// That is deliberate and it fixes a bug class rather than a bug: with statics, whichever chunk
// marched LAST writes them, which is not necessarily the chunk that won on distance, so every scene
// query needs hand-written save/restore around its chunk loop and a missed one is silent.
struct PeelAccum {
    bool  active;        // the caller asked for transparency at all
    bool  analytic;      // multiply by `transparency` instead of choosing stochastically. A shadow
                         // ray wants the expected attenuation with none of the variance a random
                         // choice adds to every shadow in the image.
    uint  maxLayers;
    uint  layers;
    uint  seed;
    float minTransmittance;   // see RayQuery::minTransmittance
    vec3  transmittance;
    vec3  emission;
    // Interface-vs-interior tracking. Only an INTERFACE is charged the material's `transparency`;
    // interior voxels of one body are not separate panes of glass, and charging each of them an alpha
    // is what made depth read as darkness (transparency^N goes black within a few voxels).
    bool  inMedium;
    float previousExit;
    VoxelMaterial previous;
    // Was the layer just consumed an INTERFACE rather than another voxel of the body already being
    // crossed? Reported rather than recomputed because the test is not trivial (same material AND
    // beginning where the previous one ended) and the caller needs the same answer the peel used:
    // refraction bends at an interface and nowhere else. Bending at every interior voxel of one pane
    // would turn a flat sheet of glass into a lens per voxel.
    bool  crossedInterface;
    // The caller is able to REPLACE THE RAY at an interface -- i.e. it asked for refraction. Set on
    // the accumulator rather than passed down, because the only thing that needs it is the consume
    // rule below and it is a property of the whole query, not of one march.
    bool  refracting;
    // Set when the budget is spent or the ray is fully absorbed. The traversal must stop consuming
    // and the scene query must stop restarting.
    bool  stopped;
    uint  stopReason;

    // ---- the material the traversal resolved, handed back rather than refetched ----
    // The decision above had to read the material to make it, so handing it out is what keeps
    // transparency free on opaque geometry: the work MOVES out of the caller rather than doubling.
    // Valid only when `hitMaterialValid` -- a coarsened LOD node names no single material.
    VoxelMaterial hitMaterial;
    bool  hitMaterialValid;
    // Whether the caller intends to shade the hit. A pure visibility query (a shadow ray that only
    // wants to know if anything is there) leaves this false and the traversal skips two texel fetches
    // per hit, which is what it has always cost.
    bool  wantMaterial;
};

// What the march should do with the voxel it just landed on.
#define PJV_PEEL_STOP_HERE 0   // this voxel stops the ray: return it as the hit
#define PJV_PEEL_CONTINUE  1   // consumed as a transparent layer: keep stepping
#define PJV_PEEL_ABORT     2   // budget spent or fully absorbed: abandon the ray as a miss

PeelAccum pjvNoPeel() {
    PeelAccum a;
    a.active = false; a.analytic = false;
    a.maxLayers = 0u; a.layers = 0u; a.seed = 0u; a.minTransmittance = 0.002;
    a.transmittance = vec3(1.0); a.emission = vec3(0.0);
    a.inMedium = false; a.previousExit = 0.0; a.previous = emptyVoxelMaterial();
    a.stopped = false; a.stopReason = PJV_STOP_OPAQUE;
    a.hitMaterial = emptyVoxelMaterial(); a.hitMaterialValid = false; a.wantMaterial = false;
    a.crossedInterface = false; a.refracting = false;
    return a;
}

// Decide what the march should do with a voxel it has just landed on.
//
// Returns one of PJV_PEEL_*. All distances are in whatever units the caller is working in -- the
// march's local voxel units -- and only ratios of them are used, so nothing here needs to know the
// chunk's world scale.
int pjvPeelConsume(inout PeelAccum peel, VoxelMaterial m,
                   float rayT, float exitT, float cellSize, float limitT) {
    // Not peeling, or a genuinely opaque voxel: this is the surface the caller asked for.
    if (!peel.active || m.transparency <= 0.0) return PJV_PEEL_STOP_HERE;

    // Outside the interval this march is allowed to consume in place. Hand it back and let the scene
    // query restart, which is the ordering-safe path and what this file did for every layer before.
    // `limitT < 0` means the caller granted no interval at all.
    if (limitT < 0.0 || exitT > limitT) return PJV_PEEL_STOP_HERE;

    // Out of layer budget. Give up as a MISS carrying what was accumulated -- NEVER report this voxel
    // as the opaque hit. It is not opaque, and the face that would be shaded is INTERNAL to a
    // transparent volume: a face buried in glass, facing into more glass, which shades almost black.
    // Because the depth at which the budget runs out varies per ray, those dark faces land on
    // different voxel boundaries per pixel, which is what makes the inside of a thick transparent
    // object read as a lattice of dark squares.
    if (peel.layers >= peel.maxLayers) {
        peel.stopped = true;
        peel.stopReason = PJV_STOP_LAYER_BUDGET;
        return PJV_PEEL_ABORT;
    }

    // Interface or interior? A hit continues the previous body only if it is the same material AND
    // begins where the previous one ended.
    bool entering = !peel.inMedium || !sameMedium(peel.previous, m) ||
                    rayT > peel.previousExit + max(cellSize, 1e-9) * 0.5;
    peel.crossedInterface = entering;

    // ---- A REFRACTING INTERFACE IS NOT THIS FUNCTION'S TO CONSUME ---------------------------
    //
    // Bending the ray means REPLACING it, and only the scene-level loop can do that -- a march is
    // walking one chunk's tree along a fixed direction and has nowhere to put a new one. So an
    // interface that will bend is handed back unconsumed, exactly as one past `limitT` is, and the
    // outer loop consumes it there and starts the new segment.
    //
    // This replaces a blunter fix that disabled in-place peeling altogether whenever the query
    // wanted refraction. That was correct about the ordering and quietly wrong about the budget: a
    // scene-level restart per transparent voxel caps a body's depth at MAX_PEEL_ITERATIONS (16),
    // while the callers that want refraction are the same ones asking for 64 layers -- so a glass
    // body more than about fifteen voxels thick exhausted the loop and returned no hit at all,
    // which the renderer draws as background. Stopping only at the interfaces that actually bend
    // keeps the interior of a body peeling in place, where it costs one traversal.
    //
    // `peel.refracting` is cleared by the outer loop around its own call, or this would hand the
    // same voxel back forever.
    if (peel.refracting && entering && abs(m.ior - 1.0) > 1.0 / 256.0) return PJV_PEEL_STOP_HERE;

    if (entering) {
        if (peel.analytic) {
            // Expected attenuation, no variance.
            peel.transmittance *= m.transparency;
        } else {
            // Stochastic alpha. With probability (1 - transparency) the surface interacts and is
            // returned as an ordinary hit, so it is shaded by the full BSDF -- which is what gives a
            // water surface its specular reflection while still being see-through. Choosing the
            // interaction with exactly that probability makes both branches carry weight 1, so the
            // estimator needs no compensating factor and converges to the alpha-composited answer.
            if (peelRandom(peel.seed) >= m.transparency) return PJV_PEEL_STOP_HERE;
        }
    }

    float segmentLength = clamp(exitT - rayT, 0.0, 1.7320508 * max(cellSize, 1e-9));
    // Emission first: a layer's own glow is dimmed by what is in FRONT of it, not by itself.
    peel.emission += peel.transmittance * m.emission;
    peel.transmittance *= mediumAbsorption(m, segmentLength, cellSize);
    peel.layers++;
    peel.previous = m;
    peel.previousExit = exitT;
    peel.inMedium = true;

    // Fully absorbed: nothing behind this can contribute, so stop rather than spending the remaining
    // layers resolving a surface that will be multiplied by zero.
    if (max(peel.transmittance.r, max(peel.transmittance.g, peel.transmittance.b))
            < peel.minTransmittance) {
        peel.stopped = true;
        peel.stopReason = PJV_STOP_ABSORBED;
        return PJV_PEEL_ABORT;
    }
    return PJV_PEEL_CONTINUE;
}

chunkHeader headers(int headerIndex) {
    // One header is 5 consecutive texels (grown from 4 to fit the added traversalLOD + reserved
    // words -- a texel is atomic, so one new field forces a whole new texel). The texture is 2D
    // (it used to be a single row, which capped the scene at maxTextureSize/N chunks), and its
    // width is always a multiple of 5, so a header never straddles a row -- one divide resolves
    // the row, and the 5 fetches share it. The CPU builds the same layout in createHeaderTexture
    // (gpu_interface.cpp); the "width is a multiple of 5" invariant is what keeps the two in sync
    // without a uniform.
    int headersPerRow = textureSize(headerData, 0).x / 5;
    int row = headerIndex / headersPerRow;
    int index = (headerIndex % headersPerRow) * 5;

    uvec4 pixel0 = texelFetch(headerData, ivec2(index + 0, row), 0);
    uvec4 pixel1 = texelFetch(headerData, ivec2(index + 1, row), 0);
    uvec4 pixel2 = texelFetch(headerData, ivec2(index + 2, row), 0);
    uvec4 pixel3 = texelFetch(headerData, ivec2(index + 3, row), 0);
    uvec4 pixel4 = texelFetch(headerData, ivec2(index + 4, row), 0);

    chunkHeader header;
    header.chunkID = pixel0.r;
    header.positionX = uintBitsToFloat(pixel0.g);
    header.positionY = uintBitsToFloat(pixel0.b);
    header.positionZ = uintBitsToFloat(pixel0.a);
    header.scale = uintBitsToFloat(pixel1.r);
    header.resolution = pixel1.g;
    header.geometryStartIndex = pixel1.b;
    header.geometryEndIndex = pixel1.a;
    header.materialIDStartIndex = pixel2.r;
    header.materialIDEndIndex = pixel2.g;
    header.dataRefID = pixel2.b;
    header.paletteOffset = pixel2.a;
    header.rotation = vec4(uintBitsToFloat(pixel3.r), uintBitsToFloat(pixel3.g),
                           uintBitsToFloat(pixel3.b), uintBitsToFloat(pixel3.a));
    header.traversalLOD = pixel4.r;
    header.envelopeStartIndex = pixel4.g;
    header.envelopeNodeCount = pixel4.b;
    header.envelopeMotionStartIndex = pixel4.a;

    return header;
}

// Total addressable header slots. The texture is 2D, so this is headers-per-row * rows.
int headersLength() {
    ivec2 texSize = textureSize(headerData, 0);
    return int(texSize.x / 5) * int(texSize.y);
}

// Builds the rotation matrix R (R * v rotates v by q) from a quaternion [x, y, z, w].
// R is orthonormal, so its inverse is transpose(R). Used to bring rays into a chunk's
// local (axis-aligned) voxel frame and to push hit normals/positions back to world.
mat3 rotationFromQuat(vec4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;
    // GLSL mat3(vec3,vec3,vec3) takes the three vec3 as COLUMNS (column-major), so these
    // are columns 0,1,2 of R.
    return mat3(
        vec3(1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz),       2.0 * (xz - wy)),
        vec3(2.0 * (xy - wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx)),
        vec3(2.0 * (xz + wy),       2.0 * (yz - wx),       1.0 - 2.0 * (xx + yy))
    );
}

//Returns a random float.
float randomFloat0to1(vec2 uv, float timeX) {
    vec3 coords = vec3(uv * timeX, timeX);

    coords = fract(coords * vec3(443.8975, 441.4234, 437.195));
    coords += dot(coords, coords.yzx + 19.19);

    float n = dot(coords, vec3(12.9898, 78.233, 45.164));
    return fract(sin(n) * 43758.5453123);
}

// Given a ZOrder index and the grid resolution,
// return the original (x, y, z) that produced it.
uvec3 inverseZOrderIndex(uint ZOrder, uint gridResolution) {
    uint x = 0u;
    uint y = 0u;
    uint z = 0u;

    int levels = int(log2(float(gridResolution)));

    for (int i = 0; i < levels; i++)
    {
        uint bitX = (ZOrder >> (3 * i))     & 1u;
        uint bitY = (ZOrder >> (3 * i + 1)) & 1u;
        uint bitZ = (ZOrder >> (3 * i + 2)) & 1u;

        x |= (bitX << i);
        y |= (bitY << i);
        z |= (bitZ << i);
    }

    return uvec3(x, y, z);
}

//Given an integer x, y, z, and the resolution of the voxel grid, return the ZOrderIndex of that point.
uint calculateZOrderIndex(uint x, uint y, uint z, uint gridResolution){
    uint ZOrder = 0;
    int levels = int(log2(float(gridResolution)));
    for(int i = 0; i < levels; i++){
        uint bitX = (x >> i) & 1u;
        uint bitY = (y >> i) & 1u;
        uint bitZ = (z >> i) & 1u;

        ZOrder |= (bitX << (3 * i)) | (bitY << (3 * i + 1)) | (bitZ << (3 * i + 2));
    }
    return ZOrder;
}

// Hardcoded version of calculateZOrderIndex for gridResolution = 4 (log2(4) = 2 iterations).
uint calculateZOrderIndex4(uint x, uint y, uint z){
    uint ZOrder = 0;
    // i = 0
    ZOrder |= (x & 1u) | ((y & 1u) << 1) | ((z & 1u) << 2);
    // i = 1
    ZOrder |= (((x >> 1) & 1u) << 3) | (((y >> 1) & 1u) << 4) | (((z >> 1) & 1u) << 5);
    return ZOrder;
}

// Near Plane: 0.1
// Far Plane: 100.0

/**
 * Calculate the starting ray direction for the initial ray trace.
 *
 * @param uv - The normalized device coordinates (NDC) of the pixel, ranging from 0 to 1.
 * @param res - The resolution of the screen, where res.x is the width and res.y is the height.
 * @param cameraPosition - The position of the camera in world space.
 * @param cameraDirection - The direction the camera is facing in world space.
 * @param fov - The field of view of the camera in degrees.
 * @return vec3 - The normalized direction of the ray starting from the camera.
 */
//Calculate the starting ray direction for the inital ray trace
vec3 rayStartDirection(vec2 uv, vec2 res, vec3 cameraPosition, vec3 cameraDirection, float fov){
    // NDC in [-1, 1] with the usual top-left-origin UV flip.
    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float aspectRatio = res.x / res.y;
    float scale = tan(radians(fov * 0.5)); // vertical-FOV convention

    // Orthonormal camera basis. right/up are normalized so the effective FOV stays
    // correct for ANY look direction (an un-normalized basis shrinks with the pitch
    // angle and warps the FOV). Handedness and the UV flip match the previous version,
    // so the reprojection inverse (worldToUV) stays exact.
    vec3 forward = normalize(cameraDirection);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));

    return normalize(right * (ndc.x * scale * aspectRatio) + up * (ndc.y * scale) + forward);
}

uint calculateZOrderIntoBox(uint branchingFactor, vec3 intersectionOnBox, BoxAABB box) {
    // Assume box.position is the minimum corner of the box
    float xLocal = (intersectionOnBox.x - box.position.x) / box.size;
    float yLocal = (intersectionOnBox.y - box.position.y) / box.size;
    float zLocal = (intersectionOnBox.z - box.position.z) / box.size;

    // Clamp to avoid max-face and numerical precision issues
    xLocal = clamp(xLocal, 0.0f, 0.999999f);
    yLocal = clamp(yLocal, 0.0f, 0.999999f);
    zLocal = clamp(zLocal, 0.0f, 0.999999f);

    uint xZOrderPos = uint(xLocal * branchingFactor);
    uint yZOrderPos = uint(yLocal * branchingFactor);
    uint zZOrderPos = uint(zLocal * branchingFactor);

    uint zOrder = calculateZOrderIndex(xZOrderPos, yZOrderPos, zZOrderPos, 4);
    return zOrder;
}

uint calculateZOrderIntoBoxFast(vec3 pos, BoxAABB box) {
    uvec3 gridPos = uvec3((pos - box.position) / box.size * 4.0);
    
    // Clamp to valid range (0-3 for 4x4x4 grid)
    gridPos = min(gridPos, uvec3(3));
    
    // Interleave bits: XYZXYZ pattern
    uint zOrder = 0;
    zOrder |= (gridPos.x & 1u) << 0;    // X bit 0
    zOrder |= (gridPos.y & 1u) << 1;    // Y bit 0  
    zOrder |= (gridPos.z & 1u) << 2;    // Z bit 0
    zOrder |= ((gridPos.x >> 1) & 1u) << 3; // X bit 1
    zOrder |= ((gridPos.y >> 1) & 1u) << 4; // Y bit 1
    zOrder |= ((gridPos.z >> 1) & 1u) << 5; // Z bit 1
    
    return zOrder;
}

BoxAABB newBoxFromParent(uint branchingFactor, BoxAABB parentBox, uint zOrderInParent) {
    BoxAABB newBox;
    newBox.size = parentBox.size / branchingFactor;

    uvec3 childPositionInParent = inverseZOrderIndex(zOrderInParent, 4);
    newBox.position = parentBox.position + vec3(childPositionInParent) * newBox.size;

    return newBox;
}

BoxAABB parentBoxFromChildAndZOrder(uint branchingFactor, BoxAABB childBox, uint childZOrderInParent) {
    BoxAABB parentBox;

    parentBox.size = childBox.size * branchingFactor;

    uvec3 childPositionInParent =
        inverseZOrderIndex(childZOrderInParent, branchingFactor);

    parentBox.position =
        childBox.position - vec3(childPositionInParent) * childBox.size;

    return parentBox;
}

/*
void emplaceNodeDataIndexies(uint value) {
    nodeDataIndexies[nodeDataIndexiesSize] = value;
    nodeDataIndexiesSize += 1;
}

void emplaceTraversalBoxes(BoxAABB box) {
    traversalBoxes[traversalBoxesSize] = box;
    traversalBoxesSize += 1;
}

void emplaceTraversalZOrder(uint value) {
    traversalZOrder[traversalZOrderSize] = value;
    traversalZOrderSize += 1;
}
*/

Tree64NodeData tree64(uint indexOfNode) {
    //ivec2 texSize = textureSize(tree64Data, 0);
    //int pixelIndex = index / 4;
    //int x = pixelIndex % texSize.x;
    //int y = pixelIndex / texSize.x;
    //int colorIndex = index % 4;
    //uvec4 pixel = texelFetch(tree64Data, ivec2(x, y), 0);
    uint w = uint(tree64Dims.x);
    uint shift = uint(tree64Dims.y);
    int x = int(indexOfNode & (w - 1u));
    int y = int(indexOfNode >> shift);
    uvec4 pixel = texelFetch(tree64Data, ivec2(x, y), 0);
    Tree64NodeData nodeData;
    nodeData.data1 = pixel.x;
    nodeData.data2 = pixel.y;
    nodeData.data3 = pixel.z;
    nodeData.childPtr = pixel.w;
    return nodeData; 
}

// See if the valid bit for this z order is set.
bool checkZOrderInValidMasks(uint mask1, uint mask2, uint zOrder) {
    uint idx = zOrder & 31u;                 // 0..31
    uint bit = 0x80000000u >> idx;           // bit position (31-idx)

    uint sel = uint(zOrder >> 5);            // 0 or 1
    sel = ~sel + 1u;                         // 0 or 0xFFFFFFFF

    uint mask = (mask1 & ~sel) | (mask2 & sel);

    return (mask & bit) != 0u;
}

uint calculateSiblingsBeforeThisZOrder2(uint branchingFactor, uint mask1, uint mask2, uint zOrder) {
    uint siblings = 0;
    for (int i = 0; i < (branchingFactor * branchingFactor * branchingFactor); i++) {
        if (zOrder == i) return siblings;
        if (checkZOrderInValidMasks(mask1, mask2, i)) { // Increase counter if child is valid.
            siblings += 1;
        }
    }
    return siblings;
}

// How many of the 64 children before `zOrder` are set, across the node's two mask words -- i.e. the
// rank of this child among its present siblings, which is its offset from the node's first child.
//
// Serves two callers with the same arithmetic. For an INTERIOR node the children are nodes and the
// rank offsets into the child array; for a LEAF node the children are voxels and the rank offsets
// into the material list. fetchVoxelMaterialFromHit and fetchVoxelMaterialAtCoord both go through
// here so the two cannot drift.
//
// `countbits` is bgfx's portable popcount intrinsic (one instruction on every target this builds
// for). This used to call a hand-rolled Hamming popcount -- about a dozen ALU ops -- while the
// material path a few hundred lines down already used the intrinsic, which had it exactly backwards:
// the slow version was the one in the descent's inner loop.
//
// The zOrder == 0 early return is load-bearing, not a micro-optimisation: it is what keeps the shift
// below out of `0xFFFFFFFFu << 32u`, which is undefined in GLSL and masks to a shift of 0 in HLSL --
// yielding a full mask and a rank of popcount(mask1) where the answer is 0. Any caller open-coding
// this test needs the same guard.
uint calculateSiblingsBeforeThisZOrder(uint branchingFactor, uint mask1, uint mask2, uint zOrder) {
    uint siblings = 0u;

    if (zOrder < 32u) {

        // No bits before zOrder 0
        if (zOrder == 0u) {
            return 0u;
        }

        // Keep bits strictly before this zOrder
        uint beforeMask = 0xFFFFFFFFu << (32u - zOrder);
        siblings = countbits(mask1 & beforeMask);

    } else {

        // All bits in mask1 are before
        siblings = countbits(mask1);

        uint z2 = zOrder - 32u;

        if (z2 > 0u) {
            uint beforeMask2 = 0xFFFFFFFFu << (32u - z2);
            siblings += countbits(mask2 & beforeMask2);
        }
    }

    return siblings;
}

// The material list index of one voxel of a LEAF node, given the leaf's own node data and the
// voxel's z-order among the leaf's 64 children. Leaf-relative: the caller adds the chunk's
// materialIDStartIndex, which is the one piece of this that lives in the header rather than the tree.
//
// data3's layout is the leaf flag in bit 0, the uniform flag in bit 1, and the material byte offset
// in bits 2 and up (see voxel.h). A uniform leaf stores ONE material byte for all 64 of its voxels,
// so the rank is not needed and the popcount is skipped -- which is also why a homogeneous body of
// water costs no more per voxel to look up than a single painted voxel does.
uint leafMaterialListOffset(Tree64NodeData leaf, uint voxelZOrderInLeaf) {
    bool leafIsUniform = (leaf.data3 & 2u) != 0u;
    uint materialOffset = leaf.data3 >> 2u;
    if (leafIsUniform) {
        return materialOffset;
    }
    return materialOffset + calculateSiblingsBeforeThisZOrder(4, leaf.data1, leaf.data2, voxelZOrderInLeaf);
}

// ------------------------------------------------------------
// Z-order movement via LUT (bit-exact to original implementation)
// Layout: XYZXYZ (2 bits per axis)
// ------------------------------------------------------------

// Encode direction (-1,0,1)^3 → [0..26]
// Convert direction to index (only 7 values)
uint encodeDir(vec3 d) {
    return (d.x == 1) * 1u
         + (d.x == -1) * 2u
         + (d.y == 1) * 3u
         + (d.y == -1) * 4u
         + (d.z == 1) * 5u
         + (d.z == -1) * 6u;
}

// Fast move
uint moveZOrder(uint zOrder, vec3 direction)
{
    return MOVE_LUT[zOrder * 7u + encodeDir(direction)];
}

// findMSB / findLSB used to sit here -- 32-iteration scalar loops, "implementations from Nvidia",
// called by nothing in this file or in any shader that includes it. Removed rather than left for a
// compiler to strip, because a reader looking for the bit-scan the traversal uses would find these
// first: the one that is actually live is open-coded inside getBoundariesCrossed below.

// Will not work for high res position! Coordinate scale must ALWAYS be 1 node, not 1 voxel...
uint getBoundariesCrossed(ivec3 previousNodeCoordinate, ivec3 proposedNodeCoordinate, uint meaningles) {
      ivec3 diff = previousNodeCoordinate - proposedNodeCoordinate;
      int boundaryPos = 0;
      if      (diff.x != 0) boundaryPos = max(previousNodeCoordinate.x, proposedNodeCoordinate.x);
      else if (diff.y != 0) boundaryPos = max(previousNodeCoordinate.y, proposedNodeCoordinate.y);
      else if (diff.z != 0) boundaryPos = max(previousNodeCoordinate.z, proposedNodeCoordinate.z);
      else return 0u;

      if (boundaryPos == 0) return 0u;
      if ((uint(boundaryPos) & 3u) != 0u) return 0u;

      uint v = uint(boundaryPos);
      uint bits = 0u;
      if ((v & 0xFFFFu) == 0u) { bits += 16u; v >>= 16; }
      if ((v & 0x00FFu) == 0u) { bits += 8u;  v >>= 8;  }
      if ((v & 0x000Fu) == 0u) { bits += 4u;  v >>= 4;  }
      if ((v & 0x0003u) == 0u) { bits += 2u;  v >>= 2;  }
      if ((v & 0x0001u) == 0u) { bits += 1u;             }
      return bits / 2u;
}

// Level = 0: highest resolution of this tree, Level = 1, resolution/4...
uint getZOrderInParentFromThisNodesLevel(ivec3 highResPosition, uint level) {
    ivec3 positionAtLevelRes = highResPosition >> (2 * level);
    ivec3 positionInParent = positionAtLevelRes % 4;
    return calculateZOrderIndex4(positionInParent.x, positionInParent.y, positionInParent.z);
}

// Resolution is the resolution that we want it to find the intersect at. 
// BoxAABB boundingBox is assumed to be positionted correctly in traversal space. (bottom left corner is where it actually lies in voxel coordinates, size is how many voxels it takes up)
ivec3 determineTraversalCoordinatesFromRayAndBox(Ray ray, BoxAABB boundingBox) {
    float t = getRayBoxEntryDistanceForSureHit(ray, boundingBox);

    // Robust, scale-consistent entry: take the hit point and clamp it just inside the
    // box before flooring. Voxels are 1 unit, so the small inset off the max faces is
    // well-scaled and reliably lands in the last cell rather than one past it. This
    // avoids the old fixed-eps ray nudge (wrong at grazing angles) and the
    // equal(p, floor(p)) test (essentially never true in float).
    vec3 p = ray.origin + ray.direction * t;
    vec3 boxMinF = boundingBox.position;
    vec3 boxMaxF = boundingBox.position + vec3(boundingBox.size);
    p = clamp(p, boxMinF, boxMaxF - vec3(1e-4));

    // On an exact grid plane the cell is ambiguous: floor always picks the upper cell,
    // which is only correct when the ray travels up that axis. Pick the cell the ray is
    // actually entering, otherwise a solid upper cell the ray merely touches is
    // reported as a hit (boundary fireflies).
    vec3 fp = floor(p);
    fp -= vec3(equal(p, fp)) * vec3(lessThan(ray.direction, vec3(0.0)));

    ivec3 voxelCoordinate = clamp(ivec3(fp),
                                  ivec3(boxMinF), ivec3(boxMaxF) - ivec3(1));
    return voxelCoordinate;
}

// Resolution is the resolution that we want it to find the intersect at. 
// BoxAABB boundingBox is assumed to be positionted correctly in traversal space. (bottom left corner is where it actually lies in voxel coordinates, size is how many voxels it takes up)
ivec3 determineTraversalCoordinatesFromRayAndBoxAndRayDistance(Ray ray, BoxAABB boundingBox, float rayT) {
    // Same robust entry as determineTraversalCoordinatesFromRayAndBox, but with the ray
    // distance supplied by the caller: clamp the hit point just inside the box, then floor.
    vec3 p = ray.origin + ray.direction * rayT;
    vec3 boxMinF = boundingBox.position;
    vec3 boxMaxF = boundingBox.position + vec3(boundingBox.size);
    p = clamp(p, boxMinF, boxMaxF - vec3(1e-4));

    // Direction-aware floor on exact grid planes (see determineTraversalCoordinatesFromRayAndBox).
    // rayT here is usually a cell-boundary t, so p landing exactly on an interior plane
    // is the common case, not the rare one.
    vec3 fp = floor(p);
    fp -= vec3(equal(p, fp)) * vec3(lessThan(ray.direction, vec3(0.0)));

    ivec3 voxelCoordinate = clamp(ivec3(fp),
                                  ivec3(boxMinF), ivec3(boxMaxF) - ivec3(1));
    return voxelCoordinate;
}


// Rounds our voxel coordinates down.
// Takes in high res voxel coordinates, node size.
// Then rounds voxel coordinates down to the nearest multiple of node size.
ivec3 roundVoxelCoordinatesBasedOnNodeSize(ivec3 highResPosition, uint targetNodeSize) {
    ivec3 nodeSpaceCoordinate = highResPosition >> uint(log2(targetNodeSize));
    return nodeSpaceCoordinate << uint(log2(targetNodeSize)); // Could also be bit shift, targetNodeSize is always power of 4.
}

// LOD selection ---------------------------------------------------------------
// Chooses the coarsest tree level the traversal is allowed to descend to at a
// given distance along the ray. The returned value is in the same units as
// candidateNodeLevel: 0 = finest (1-voxel) nodes, 1 = 4-voxel nodes, 2 = 16-voxel
// nodes, and so on.
//
// Near the ray origin the cap is startLOD. As the ray travels the cap ramps
// linearly, reaching finishLOD once it has covered distanceToFinishLOD voxels,
// and stays at finishLOD for the rest of the ray. This lets distant geometry
// collapse into coarser, cheaper nodes while keeping full detail up close.
//
// The all-zero default (startLOD = finishLOD = distanceToFinishLOD = 0) yields a
// constant cap of 0, i.e. full-resolution traversal — behaviour identical to
// having no LOD at all.
// ---- FOOTPRINT MODE, opt-in ------------------------------------------------
// Selected by startLOD == PJV_LOD_FOOTPRINT, a value no ordinary ramp can mean (a
// starting LOD of 255 is far past any real tree depth). Every call site in the tree
// sets startLOD explicitly to 0, so nothing reaches this branch by accident and the
// linear path below is bit-for-bit unchanged for all of them. A sentinel in an
// existing field rather than a new RayQuery member on purpose: a new member would be
// uninitialised garbage in every call site that did not know to set it.
//
// In this mode the other two fields are reinterpreted:
//   distanceToFinishLOD = the CROSSOVER distance in voxels -- where one LOD-0 voxel
//                         projects to exactly one pixel. The caller knows it:
//                         height / (2*tan(fov/2)).
//   finishLOD           = the coarsest level allowed, as a cap.
//
// WHY A LOG RAMP RATHER THAN THE LINEAR ONE. A level-k node is 4^k voxels across, so
// it first shrinks to a pixel at 4^k * CROSSOVER: the ideal onsets are geometric --
// 1x, 4x, 16x, 64x. The linear ramp spaces levels EVENLY (level k at k*D/N), so it
// can place one level correctly and every later one arrives early and coarser than
// the pixel requires. With two levels the second lands 2x early and puts 8-pixel
// nodes on screen; a third lands 5.3x early at 21 pixels, which is the "LOD too
// coarse" look that makes distance LOD unusable for a primary ray.
//
// floor(log4(d / crossover)) puts every level exactly where its own nodes reach one
// pixel. What it drops is, at the moment it drops it, below what the display can
// resolve -- so the transition is invisible by construction at EVERY level, and the
// level count stops being a quality compromise.
// PJV_LOD_FOOTPRINT is defined next to RayQuery, so pjvQueryLODFootprint can name it.
uint computeTargetLOD(float distanceInVoxels, RayQuery rayQuery) {
    if (rayQuery.startLOD == PJV_LOD_FOOTPRINT) {
        float crossover = max(float(rayQuery.distanceToFinishLOD), 1.0);
        if (distanceInVoxels <= crossover) {
            return 0u;
        }
        // ROUNDED UP TO THE OCTAVE, not down, and this is the difference between geometry that
        // can be reconstructed and geometry that cannot.
        //
        // Rounding down picks the largest node that still fits INSIDE a pixel. That sounds
        // conservative and is the wrong way round: it is exact only at the instant a level
        // begins, and from there the node keeps shrinking relative to the growing footprint
        // until the next level catches it. Measured across a level the node runs 1.00 px down
        // to 0.59 px, so for most of every level the geometry is SMALLER THAN A PIXEL -- no
        // ray lands on much of it, nothing downstream has a sample to reconstruct from, and
        // the frame falls back to a filtered guess. That is what pools into a band at the far
        // end of each level.
        //
        // Rounding up picks the smallest node that still COVERS a pixel, so the node runs 1.00
        // to 1.67 px and is never undersampled. The cost is that geometry is at most 1.67x
        // coarser than the pixel strictly demands -- but the detail given up was never being
        // sampled, so what it buys back is a reconstruction that works rather than detail.
        //
        // ceil() names the octave (a size of 2^n voxels); halving it gives the tree level to
        // stop at, and an odd octave is served by the half-level check in the march, which is
        // why this floors afterwards rather than rounding.
        float octave = ceil(log2(distanceInVoxels / crossover));
        return uint(clamp(floor(octave * 0.5), 0.0, float(rayQuery.finishLOD)));
    }

    // A zero ramp distance means there is nothing to interpolate across: the cap
    // is finishLOD everywhere (an instantaneous jump at the origin).
    if (rayQuery.distanceToFinishLOD == 0u) {
        return rayQuery.finishLOD;
    }

    if (distanceInVoxels >= float(rayQuery.distanceToFinishLOD)) {
        return rayQuery.finishLOD;
    }

    float t = distanceInVoxels / float(rayQuery.distanceToFinishLOD);
    // mix() handles both the usual case (startLOD < finishLOD, detail falling off
    // with distance) and a reversed ramp. uint() floors toward the finer level so
    // we never cap coarser than requested.
    return uint(mix(float(rayQuery.startLOD), float(rayQuery.finishLOD), t));
}


// Exit distance of the cell at `cellMin` with edge `cellSize`, for a ray already inside it. The same
// expression the DDA stepper builds its own tMax from, so the two cannot disagree.
float cellExitDistance(Ray ray, vec3 invRayDir, ivec3 cellMin, uint cellSize) {
    vec3 farPlanes = mix(vec3(cellMin), vec3(cellMin) + vec3(float(cellSize)), step(vec3(0.0), ray.direction));
    vec3 t = (farPlanes - ray.origin) * invRayDir;
    return min(min(t.x, t.y), t.z);
}

// `materialIDStart` / `paletteOffsetIn` are the two header values that turn this march's leaf-local
// material offset into an absolute palette lookup. They are passed IN rather than applied by the
// caller afterwards because the march now has to read a material to decide whether a voxel stops the
// ray -- see PeelAccum. The returned materialListIndex stays leaf-relative, exactly as it was, and
// castRayThroughTree64 still adds the base to it; only the march's own private lookup is absolute.
//
// `limitT` is how far along the ray the traversal may consume transparent voxels in place instead of
// handing them back for the scene query to restart on -- the interval over which no other component's
// bounding volume is live. Negative disables it, which is the behaviour every caller had before this
// existed.
//
// `localToWorld` converts this march's own t values into the caller's. THE PEEL MUST SEE WORLD UNITS.
// Its interface-vs-interior test compares a distance against `previousExit`, and that value is also
// written by the scene-level loop from a world-space hit -- so feeding it local units from in here
// would make the comparison meaningless the moment a ray crossed between the two paths. Which it
// does constantly: a layer inside the solo interval is consumed here, the next one outside it is
// consumed there, and they belong to the same body of glass. The symptom would be an interface alpha
// charged twice (a body darkening with depth) or skipped (a surface losing its reflection), varying
// with the chunk's scale, which is about as hard to attribute as this file gets.
SceneIntersectData marchRayThroughTree64_DDA(Ray ray, RayQuery rayQuery, float tMin, BoxAABB boundingBox, uint tree64StartIndex, uint tree64EndIndex, uint tree64Resolution, uint chunkTraversalLOD, uint materialIDStart, uint paletteOffsetIn, float limitT, float localToWorld, bool skipAnimated, inout PeelAccum peel) {
    SceneIntersectData returnData;
    returnData.rayT = -1.0;
    returnData.exitT = -1.0;
    returnData.normal = vec3(0.0);
    vec3 invRayDir = 1.0/ray.direction;
    // Whether a leaf hit has to resolve its material at all. The peel needs it to decide; a caller
    // that means to shade the hit wants it handed back rather than refetched. A pure visibility query
    // needs neither, and skips two texel fetches per hit -- which is what this march has always cost.
    bool needMaterial = peel.active || peel.wantMaterial || skipAnimated;
    // Normal of the face the ray most recently crossed. Starts as the volume entry face
    // (zero if the ray origin is inside the volume) and is updated on every committed
    // DDA step, so any hit — found while descending or while stepping — reports the
    // face the ray actually came through, with no analytic re-derivation.
    vec3 hitNormal = getRayBoxEntry(ray, boundingBox).normal;
    // If res = 4, treeLevels = 1. In this case just the root node exists.
    // If res = 16, treeLevels = 2. In this case just the root node exists.
    uint treeLevels = log2(tree64Resolution)/2.0; // Levels in the tree. 1 for 4x4x4 volume, 2 for 16x16x16 etc...
    // If res = 4, candidateNodeLevel = 0. In this case, candidate node IS highest resolution node (hence the - 1 to make it 0).
    uint candidateNodeLevel = treeLevels - 1; // Starting one level down into the tree.
    uint shift = 2 * candidateNodeLevel;
    uint stepSize = 1u << (shift);

    ivec3 traversalPosition = ivec3(0); // Always at 1 voxel resolution, even if level/step size isn't.
    traversalPosition = determineTraversalCoordinatesFromRayAndBox(ray, boundingBox);
    // Above is same logic used in push!

    nodeStack[0].thisNodeZOrderInParent = 0;
    nodeStack[0].dataIndex = tree64StartIndex;
    nodeStack[1].thisNodeZOrderInParent = getZOrderInParentFromThisNodesLevel(traversalPosition, candidateNodeLevel);
    nodeStack[1].dataIndex = 0;
    nodeStackQuantity = 2;

    //traversalPosition = roundVoxelCoordinatesBasedOnNodeSize(traversalPosition, tree64Resolution/4);
    traversalPosition = (traversalPosition >> shift) << shift;
    ivec3 stepI = ivec3(sign(ray.direction));
    float rayT = getRayBoxEntryDistanceForSureHit(ray, boundingBox);

    uint stepCount = 0;
    bool previouslyPopped = false;
    Tree64NodeData data = tree64(tree64StartIndex);
    for (; stepCount < rayQuery.maxRaySteps; stepCount++) {
        //stepCount += 1;
        while (candidateNodeLevel >= 0) {
            //stepCount -= 1;
            if (previouslyPopped) break;
            if (checkZOrderInValidMasks(data.data1, data.data2, nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent)) {
                if((data.data3 & 0b1) == 1u) {
                    // Leaf found! Handle accordingly -- unless it sits at or before tMin, which is
                    // the case a peeling caller has already consumed. This is the branch that
                    // matters for a ray whose origin is INSIDE a solid voxel: the descent reaches
                    // that voxel immediately at rayT == 0, and returning it would hand back the
                    // surface the caller is trying to get past. Breaking out of the descent instead
                    // drops into the DDA stepping section below, which walks out of the voxel one
                    // step at a time and reports the next one properly, with a real normal.
                    if (tMin <= 0.0 || rayT >= tMin) {
                        // `data` IS the leaf node here -- the leaf flag is carried by the node whose
                        // 64 children are voxels, and the candidate we just validated against its
                        // masks is one of those voxels. So the material offset is available from the
                        // values already in registers, with no fetch and no second descent. See
                        // SceneIntersectData::materialListIndex.
                        uint leafOffset = leafMaterialListOffset(
                            data, nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent);
                        float cellExitT = cellExitDistance(ray, invRayDir, traversalPosition, stepSize);
                        int verdict = PJV_PEEL_STOP_HERE;
                        // Held LOCALLY until the verdict is known. Publishing it to `peel` before
                        // deciding is what made a skipped voxel's material outlive the skip: the
                        // march walks past every animated voxel publishing each one, then stops on
                        // the first ordinary voxel and publishes that -- and if the animated march
                        // then wins the merge, the hit is the animated voxel while the material is
                        // the one behind it. Position, normal and shadows all come out right and only
                        // the ALBEDO is another voxel's, which on screen reads as seeing through the
                        // geometry rather than as a material bug.
                        VoxelMaterial leafMaterial = emptyVoxelMaterial();
                        if (needMaterial) {
                            leafMaterial = decodeMaterial(materialPaletteTexel(
                                materialID(leafOffset + materialIDStart) + paletteOffsetIn));
                            // This voxel's drawn position belongs to the envelope, not to its own
                            // cell. Step through it: pjvAnimatedMarch will report it wherever it
                            // actually is. Without this it is drawn TWICE -- once at rest here and
                            // once displaced there -- which reads as a doubled, smeared field.
                            //
                            // ONLY WITHIN THE RESOLVE DISTANCE. Past it the animated march does not
                            // run, so skipping here would hand these voxels to nobody and they would
                            // vanish at range. Beyond the cutoff an animated voxel is ordinary
                            // geometry drawn at its rest position -- it stops moving, it does not
                            // stop existing.
                            if (skipAnimated && rayT * localToWorld <= rayQuery.animResolveDistance &&
                                (leafMaterial.flags & PJV_MAT_FLAG_ANIMATED) != 0u) {
                                verdict = PJV_PEEL_CONTINUE;
                            } else {
                                verdict = pjvPeelConsume(peel, leafMaterial,
                                                         rayT * localToWorld, cellExitT * localToWorld,
                                                         float(stepSize) * localToWorld, limitT);
                            }
                        }
                        if (verdict == PJV_PEEL_ABORT) {
                            returnData.foundBox.position = vec3(0);
                            returnData.foundBox.size = -1;
                            returnData.voxelCoord = ivec3(0);
                            returnData.steps = stepCount;
                            returnData.materialListIndex = MATERIAL_INDEX_NEEDS_DESCENT;
                            peel.hitMaterialValid = false;
                            return returnData;
                        }
                        if (verdict == PJV_PEEL_STOP_HERE) {
                            peel.hitMaterial = leafMaterial;
                            peel.hitMaterialValid = needMaterial;
                            returnData.foundBox.position = traversalPosition;
                            returnData.voxelCoord = traversalPosition;
                            returnData.foundBox.size = stepSize;
                            returnData.steps = stepCount;
                            returnData.rayT = rayT;
                            returnData.exitT = cellExitT;
                            returnData.normal = hitNormal;
                            returnData.materialListIndex = leafOffset;
                            return returnData;
                        }
                        // Consumed as a transparent layer. Break out of the descent, which drops into
                        // the stepping section below and walks out of this voxel one step at a time --
                        // the same route the tMin gate takes for a layer the caller already consumed.
                    }
                    break;
                }
                // LOD cutoff. This candidate is occupied (valid in its parent's
                // mask) but is an interior node we would normally descend into.
                // If it already sits at (or below) the coarsest level allowed for
                // its distance along the ray, stop here and treat the whole node
                // as solid instead of resolving finer geometry inside it. rayT is
                // the distance to this candidate in voxel units, and this is the
                // only place the traversal descends, so capping here is sufficient
                // to guarantee we never resolve finer than the requested LOD.
                //
                // chunkTraversalLOD is a second, independent cap: this specific chunk instance's
                // own requested detail level, which can be coarser than what's actually uploaded
                // (e.g. one of several instances sharing a blob that another, closer instance
                // forced finer -- see makeHeader in gpu_interface.cpp). It never makes the ray see
                // MORE detail than computeTargetLOD already allows, only less -- hence max(), not
                // a replacement.
                if (candidateNodeLevel <= max(computeTargetLOD(rayT, rayQuery), chunkTraversalLOD)) {
                    // Same tMin gate as the leaf above. A coarsened node is treated as wholly solid,
                    // so there is no finer geometry inside it for a peel to find -- stepping past it
                    // is the right way to get beyond a layer the caller already consumed.
                    if (tMin <= 0.0 || rayT >= tMin) {
                        // A coarsened node is treated as wholly SOLID, and therefore as opaque: it
                        // spans 4, 16 or 64 voxels with no single transparency to read, so there is
                        // nothing for the peel to decide with. Guessing either way would be wrong for
                        // most of the node -- and at the distance a node is coarsened to, the
                        // difference is below what the frame can resolve anyway.
                        peel.hitMaterialValid = false;
                        returnData.foundBox.position = traversalPosition;
                        returnData.voxelCoord = traversalPosition;
                        returnData.foundBox.size = stepSize;
                        returnData.steps = stepCount;
                        returnData.rayT = rayT;
                        returnData.exitT = cellExitDistance(ray, invRayDir, traversalPosition, stepSize);
                        returnData.normal = hitNormal;
                        // The one hit that cannot name a material. This candidate is an INTERIOR node
                        // returned whole, so there is no leaf to read a material byte from and no one
                        // voxel it would belong to -- the node spans 4, 16 or 64 of them. Hand the
                        // caller the sentinel and let it descend on voxelCoord, which reproduces
                        // exactly what a coarsened hit has always shaded as (the material of the
                        // node's minimum-corner voxel, or empty if that corner is not filled).
                        returnData.materialListIndex = MATERIAL_INDEX_NEEDS_DESCENT;
                        return returnData;
                    }
                    break;
                }
                BoxAABB candidateBox;
                candidateBox.position = vec3(traversalPosition);
                candidateBox.size = float(stepSize);
                ivec3 highResPosition = determineTraversalCoordinatesFromRayAndBoxAndRayDistance(ray, candidateBox, rayT);
                uint parentDataIndex = nodeStack[nodeStackQuantity - 2u].dataIndex;
                uint childrenBeforeThisNode = calculateSiblingsBeforeThisZOrder(4, data.data1, data.data2, nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent);
                uint childDataIndex = parentDataIndex + data.childPtr + childrenBeforeThisNode;
                nodeStack[nodeStackQuantity - 1u].dataIndex = childDataIndex;
                nodeStack[nodeStackQuantity].thisNodeZOrderInParent = getZOrderInParentFromThisNodesLevel(highResPosition, candidateNodeLevel - 1);
                nodeStack[nodeStackQuantity].dataIndex = 0;
                nodeStackQuantity += 1;
                candidateNodeLevel -= 1;
                // equates to : pow(4, candidateNodeLevel);
                shift = 2u * candidateNodeLevel;
                stepSize = 1u << (shift);
                traversalPosition = (highResPosition >> shift) << shift;

                data = tree64(childDataIndex);
            } else {
                break;
            }
        }

        vec3 nextPlanes = mix(vec3(traversalPosition), vec3(traversalPosition) + vec3(stepSize), step(vec3(0.0), ray.direction));

        vec3 tMax = (nextPlanes - ray.origin) * invRayDir;
        vec3 tDelta = stepSize * abs(invRayDir);
        previouslyPopped = false;
        //Tree64NodeData parentData = tree64(nodeStack[nodeStackQuantity - 2u].dataIndex);
        for (int i = 0; i < 12; i++) {
            // Actually do DDA

            // Step exactly ONE axis per iteration: the axis whose next cell boundary is
            // nearest. Ties (the ray crossing an edge or corner where two/three tMax
            // components are equal) are broken deterministically x > y > z, so the ray
            // advances one axis at a time and enters the face-adjacent voxel first,
            // reaching the corner voxel on the following iteration. Stepping multiple
            // axes at once (as step(tMax, min) would on a tie) moves diagonally and
            // skips the voxel that only touches that edge/corner, which is what caused
            // the speckle/clip-through artifacts. Single-axis stepping also keeps the
            // node-coordinate diff single-axis so getBoundariesCrossed stays well-defined.
            float sx = float(tMax.x <= tMax.y && tMax.x <= tMax.z);
            float sy = float(tMax.y <= tMax.x && tMax.y <= tMax.z) * (1.0 - sx);
            float sz = 1.0 - sx - sy;
            vec3 axisMask = vec3(sx, sy, sz);   // exactly one component is 1.0

            rayT = dot(tMax, axisMask);         // t of the stepped axis (== the minimum)

            tMax += axisMask * tDelta;

            ivec3 directionSteppedIn = ivec3(axisMask) * stepI;

            ivec3 proposedTraversalPosition = traversalPosition + directionSteppedIn * stepSize;
            if (proposedTraversalPosition.x < 0 || proposedTraversalPosition.y < 0 || proposedTraversalPosition.z < 0 ||
              proposedTraversalPosition.x >= tree64Resolution || proposedTraversalPosition.y >= tree64Resolution || proposedTraversalPosition.z >= tree64Resolution) {
                returnData.foundBox.position = vec3(0);
                returnData.foundBox.size = -1;
                returnData.voxelCoord = ivec3(0);
                returnData.steps = stepCount;
                returnData.materialListIndex = MATERIAL_INDEX_NEEDS_DESCENT;
                return returnData;
            }
            // Get our coordinates in node space. Shift right is the same as traversalPosition / 4^stepSize
            ivec3 previousNodeCoordinate = traversalPosition >> (shift); // Used to be uint(log2(stepSize));
            ivec3 proposedNodeCoordinate = proposedTraversalPosition >> (shift);
            uint boundariesCrossed = getBoundariesCrossed(previousNodeCoordinate, proposedNodeCoordinate, candidateNodeLevel);

            if (boundariesCrossed != 0) { // If we cross boundaires, pop.
                uint newParentIdx = nodeStack[nodeStackQuantity - 2u - boundariesCrossed].dataIndex;
                nodeStackQuantity -= boundariesCrossed;
                candidateNodeLevel += boundariesCrossed;
                shift = 2 * candidateNodeLevel;
                stepSize = 1u << (shift);
                traversalPosition = (traversalPosition >> shift) << shift;
                previouslyPopped = true;
                data = tree64(newParentIdx);
                break;
            }

            nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent = moveZOrder(nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent, directionSteppedIn);
            traversalPosition = proposedTraversalPosition;
            hitNormal = vec3(-directionSteppedIn);

            // Zero-measure guard: on an exact edge/corner crossing two tMax components
            // tie, so the cell we just stepped into is exited at the SAME t on the other
            // tied axis — the ray only touches it at a point. Treating it as occupied is
            // what produced the boundary fireflies (a solid cell the ray never actually
            // travels through gets returned as a hit, and descending into it hands the
            // push a corner-exact rayT). Skip its validity check entirely; the next
            // iteration steps the tied axis at the same rayT and lands in the diagonal
            // cell, which is then tested with real measure.
            float cellExitT = min(min(tMax.x, tMax.y), tMax.z);

            // Only check for if its valid after we advance.
            // This is because if we are advancing it could mean that we just popped out of a valid candidate,
            // checking if its valid before we advance would just put us right back into the one we popped out of.
            // Used to be 17 -
            if (cellExitT > rayT &&
                checkZOrderInValidMasks(data.data1, data.data2, nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent)) { // New valid z order found!!
                if ((data.data3 & 0b1) == 1) {
                    // Leaf found, handle accordingly.
                    if (tMin <= 0.0 || rayT >= tMin) {
                        // Same as the descent's leaf hit: `data` is the leaf, and moveZOrder has just
                        // put this voxel's index among its 64 children on the top of the stack. The
                        // z-order stays within the node by construction (MOVE_LUT is 6 bits wide), so
                        // it is the leaf-local index the material list is ranked by.
                        uint leafOffset2 = leafMaterialListOffset(
                            data, nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent);
                        int verdict2 = PJV_PEEL_STOP_HERE;
                        VoxelMaterial leafMaterial2 = emptyVoxelMaterial();   // see the descent site
                        if (needMaterial) {
                            leafMaterial2 = decodeMaterial(materialPaletteTexel(
                                materialID(leafOffset2 + materialIDStart) + paletteOffsetIn));
                            if (skipAnimated && rayT * localToWorld <= rayQuery.animResolveDistance &&
                                (leafMaterial2.flags & PJV_MAT_FLAG_ANIMATED) != 0u) {
                                verdict2 = PJV_PEEL_CONTINUE;   // see the descent site
                            } else {
                                verdict2 = pjvPeelConsume(peel, leafMaterial2,
                                                          rayT * localToWorld, cellExitT * localToWorld,
                                                          float(stepSize) * localToWorld, limitT);
                            }
                        }
                        if (verdict2 == PJV_PEEL_ABORT) {
                            returnData.foundBox.position = vec3(0);
                            returnData.foundBox.size = -1;
                            returnData.voxelCoord = ivec3(0);
                            returnData.steps = stepCount;
                            returnData.materialListIndex = MATERIAL_INDEX_NEEDS_DESCENT;
                            peel.hitMaterialValid = false;
                            return returnData;
                        }
                        if (verdict2 == PJV_PEEL_STOP_HERE) {
                            peel.hitMaterial = leafMaterial2;
                            peel.hitMaterialValid = needMaterial;
                            returnData.foundBox.position = traversalPosition;
                            returnData.voxelCoord = traversalPosition;
                            returnData.foundBox.size = stepSize;
                            returnData.steps = stepCount;
                            returnData.rayT = rayT;
                            returnData.exitT = cellExitT;
                            returnData.normal = hitNormal;
                            returnData.materialListIndex = leafOffset2;
                            return returnData;
                        }
                        // Consumed as a transparent layer: fall through and keep stepping.
                    }
                    // At or before tMin, or consumed in place: a surface this ray is done with. Keep
                    // stepping through this leaf rather than returning it. Deliberately does NOT
                    // break -- breaking would leave the stepping loop and re-descend onto the same
                    // voxel.
                } else {
                    break;   // Interior node: leave the stepping loop and descend into it.
                }
            }
        }
    }
    returnData.foundBox.position = vec3(0);
    returnData.foundBox.size = -1;
    returnData.voxelCoord = ivec3(0);
    returnData.steps = stepCount;
    returnData.materialListIndex = MATERIAL_INDEX_NEEDS_DESCENT;
    return returnData;
}

// =============================================================================
// Animation: resolving what is drawn inside an envelope cell
// =============================================================================
//
// The geometry tree stores the REST POSE. The envelope -- a second, quarter-resolution tree in the
// same texture -- marks every cell an animated voxel could be drawn in. Neither says where anything
// IS; that is decided here, per frame, from the motion table.
//
// ---- Why this is a separate march rather than a hook in the geometry DDA ----
//
// Because an envelope cell can be marked where the geometry is EMPTY, and that is the entire point:
// a blade that has swayed sideways is drawn in a cell its rest pose never occupied. The geometry
// march steps straight past empty cells -- correctly -- so it can never visit them. Unioning the two
// trees' masks during one descent would work and would mean carrying two node stacks in lockstep
// through the hottest loop in the engine, with the envelope one level shallower than the geometry.
// Marching the envelope separately costs one extra traversal of a small sparse tree, only for chunks
// that have one, and leaves the geometry DDA untouched.
//
// The other half of not drawing anything twice lives in the geometry march: when animation is on, a
// leaf voxel whose material is animated is SKIPPED there, because its drawn position is this
// function's answer to give. With animation off, nothing skips and nothing resolves -- the geometry
// tree renders the rest pose, which is exactly what an animation-blind renderer sees.

// Is a voxel set in an arbitrary tree64 range, and if so where is its material byte?
//
// Distinct from fetchVoxelMaterialAtCoord in the one way that matters here: it CHECKS THE VALID MASK
// before descending. That function is written for coordinates already known to be occupied, and
// following childPtr past an unset child reads a node belonging to some other branch. This probes
// arbitrary coordinates -- most of them empty, by construction -- so the check is not optional.
bool pjvProbeTree(uint startIndex, uint resolution, ivec3 coord, out uint outLeafOffset) {
    outLeafOffset = 0u;
    if (any(lessThan(coord, ivec3(0))) || any(greaterThanEqual(coord, ivec3(int(resolution))))) return false;

    uint zOrder = calculateZOrderIndex(uint(coord.x), uint(coord.y), uint(coord.z), resolution);
    uint nodeIdx = startIndex;
    uint treeLevels = uint(log2(float(resolution)) / 2.0);
    int level = int(treeLevels) - 1;
    uint stepSize = 1u << (6u * uint(level));

    for (; level >= 0; --level) {
        Tree64NodeData node = tree64(nodeIdx);
        uint childZOrder = (zOrder / stepSize) & 63u;
        if ((node.data3 & 1u) != 0u) {
            if (!checkZOrderInValidMasks(node.data1, node.data2, childZOrder)) return false;
            outLeafOffset = leafMaterialListOffset(node, childZOrder);
            return true;
        }
        if (!checkZOrderInValidMasks(node.data1, node.data2, childZOrder)) return false;
        uint siblingsBefore = calculateSiblingsBeforeThisZOrder(4, node.data1, node.data2, childZOrder);
        nodeIdx = nodeIdx + node.childPtr + siblingsBefore;
        stepSize >>= 6u;
    }
    return false;
}

// Descend to the LEAF NODE covering a 4x4x4 block, and stop there.
//
// This is the optimisation the whole resolve turns on, and it exists because of an alignment that is
// not a coincidence: a tree64 leaf's 64 children ARE a 4x4x4 block of voxels, and an envelope cell is
// a 4x4x4 block of voxels. They are the same box. So one descent per envelope cell puts the leaf's
// 64-bit occupancy mask in registers, and every candidate inside that block becomes a MASK TEST
// rather than a root-to-leaf descent.
//
// Without it the resolve pays a descent per candidate -- up to eight per cell, a dozen cells per
// block -- and descents are what a resolve costs. The prototype measured this as the second largest
// cost it had.
bool pjvProbeLeafNode(uint startIndex, uint resolution, ivec3 blockMin, out Tree64NodeData outLeaf) {
    outLeaf = tree64(startIndex);
    if (any(lessThan(blockMin, ivec3(0))) || any(greaterThanEqual(blockMin, ivec3(int(resolution))))) return false;

    uint zOrder = calculateZOrderIndex(uint(blockMin.x), uint(blockMin.y), uint(blockMin.z), resolution);
    uint nodeIdx = startIndex;
    uint treeLevels = uint(log2(float(resolution)) / 2.0);
    int level = int(treeLevels) - 1;
    uint stepSize = 1u << (6u * uint(level));

    for (; level >= 0; --level) {
        Tree64NodeData node = tree64(nodeIdx);
        if ((node.data3 & 1u) != 0u) { outLeaf = node; return true; }
        uint childZOrder = (zOrder / stepSize) & 63u;
        if (!checkZOrderInValidMasks(node.data1, node.data2, childZOrder)) return false;
        uint siblingsBefore = calculateSiblingsBeforeThisZOrder(4, node.data1, node.data2, childZOrder);
        nodeIdx = nodeIdx + node.childPtr + siblingsBefore;
        stepSize >>= 6u;
    }
    return false;
}

// Is an animated source stored at this voxel, and under WHICH motion set?
//
// It reports the set rather than filtering on one, and that distinction is the whole correctness of
// the resolve. Requiring a candidate to match the envelope cell's motion byte looks reasonable and is
// wrong, because THE CELL CARRIES ONE BYTE AND ENVELOPES OVERLAP: the bake writes the first set that
// reaches a cell, so anywhere two fields' envelopes meet -- grass under a tree, which is most of a
// canopy floor -- every source of the losing set is rejected here. Skipped by the geometry march
// because it is animated, refused here because its set does not match, it is then drawn NOWHERE and
// reads on screen as a voxel you can see straight through.
//
// The symptom is worth recording because it does not look like this: it looks like an occlusion bug.
// A lone animated voxel renders correctly, and only fails once another animated voxel shares its
// envelope -- so what you see is the far one apparently drawn in front of the near one, which is
// really the near one having become invisible.
//
// The cell's motion byte survives only as a HINT for bracketing the candidate search below. It never
// decides whether a source may be drawn.
// `blockLeaf` is the leaf covering [blockMin, blockMin+4), already in registers. A candidate inside
// that block is answered from its mask with no memory traffic beyond the two texel fetches the
// material needs; one outside costs a descent, and those are budgeted by the caller because the
// neighbourhood straddles a block boundary for exactly the cells at its edge.
bool pjvAnimatedSourceAt(chunkHeader h, ivec3 voxel,
                         Tree64NodeData blockLeaf, bool haveBlockLeaf, ivec3 blockMin,
                         inout int descentBudget, inout uvec2 slotCache,
                         out uint outListIndex, out uint outMotionSet) {
    outListIndex = MATERIAL_INDEX_NEEDS_DESCENT;
    outMotionSet = 0u;
    uint leafOffset;

    ivec3 rel = voxel - blockMin;
    if (haveBlockLeaf && all(greaterThanEqual(rel, ivec3(0))) && all(lessThan(rel, ivec3(4)))) {
        uint z = calculateZOrderIndex4(uint(rel.x), uint(rel.y), uint(rel.z));
        if (!checkZOrderInValidMasks(blockLeaf.data1, blockLeaf.data2, z)) return false;
        leafOffset = leafMaterialListOffset(blockLeaf, z);
    } else {
        if (descentBudget <= 0) return false;
        descentBudget--;
        // The SAME descent the in-block path is served by, aimed at the candidate's own block rather
        // than at this one. Deliberately not a general per-voxel probe: spirv-opt inlines every
        // function into main, so a second descent written out separately is a second copy of it in
        // every call site of castRayThroughTree64 -- and gbuffer.frag sits close enough to the
        // SPIR-V id ceiling that one extra copy is the difference between a shader and a
        // zero-length .bin. One descent shape, used everywhere, is what keeps it under.
        ivec3 outBlock = voxel & ivec3(~3);
        Tree64NodeData outLeaf;
        if (!pjvProbeLeafNode(h.geometryStartIndex, h.resolution, outBlock, outLeaf)) return false;
        uint zOut = calculateZOrderIndex4(uint(voxel.x - outBlock.x), uint(voxel.y - outBlock.y),
                                          uint(voxel.z - outBlock.z));
        if (!checkZOrderInValidMasks(outLeaf.data1, outLeaf.data2, zOut)) return false;
        leafOffset = leafMaterialListOffset(outLeaf, zOut);
    }
    // ---- TWO DEPENDENT FETCHES, AND WHY THE SECOND ONE IS USUALLY FREE ----------------------
    //
    // The slot byte is per-voxel and has to be read. The palette entry it names does not: this is
    // the hottest fetch in the whole animation path -- nine candidates per cell, up to a dozen cells
    // per envelope cell, for every ray -- and it is asked the same question over and over, because
    // ADJACENT VOXELS OF ONE BLADE SHARE A MATERIAL SLOT. A single-entry cache keyed on the slot
    // therefore hits nearly every time in foliage, which is the only place this code runs at all.
    //
    // Measured on a canopy interior: the resolve was 14.2 ms of a 21.0 ms frame against a 3.6 ms
    // static floor, and this fetch was the largest part of it.
    //
    // Cached across the WHOLE block walk rather than per cell, since the palette is a property of
    // the chunk and pjvAnimatedMarch runs inside one. `slotCache.x` is the slot, `.y` its flags;
    // 0xFFFFFFFF is the empty key, which materialID -- returning one byte -- cannot produce.
    uint slot = materialID(leafOffset + h.materialIDStartIndex);
    uint flags;
    if (slot == slotCache.x) {
        flags = slotCache.y;
    } else {
        flags = (materialPaletteTexel(slot + h.paletteOffset).w >> 8u) & 0xFFu;
        slotCache = uvec2(slot, flags);
    }
    if ((flags & PJV_MAT_FLAG_ANIMATED) == 0u) return false;
    outMotionSet = (flags >> PJV_MAT_MOTION_SHIFT) & PJV_MAT_MOTION_MASK;
    // LEAF-RELATIVE, deliberately: castRayThroughTree64 adds materialIDStartIndex to whatever comes
    // back from a march, and handing it an already-absolute index adds the base twice -- which reads
    // a byte from some other chunk's material array and shades the voxel an unrelated colour.
    outListIndex = leafOffset;
    return true;
}

// One moving voxel, intersected as a box at its drawn position. The shared primitive: an ember, a
// splash droplet or a falling leaf is the same intersection with a different candidate source, which
// is what stops a particle system being a second copy of this.
struct MovingVoxelHit {
    float rayT;
    float exitT;
    vec3  normal;
    vec3  drawnMin;
};

bool pjvMovingVoxelHit(Ray ray, vec3 invDir, vec3 restMin, vec3 displacement, out MovingVoxelHit hit) {
    vec3 lo = restMin + displacement;
    vec3 t0 = (lo - ray.origin) * invDir;
    vec3 t1 = (lo + vec3(1.0) - ray.origin) * invDir;
    vec3 tsmall = min(t0, t1);
    vec3 tbig = max(t0, t1);
    float tEnter = max(max(tsmall.x, tsmall.y), tsmall.z);
    float tExit = min(min(tbig.x, tbig.y), tbig.z);
    if (tEnter > tExit || tExit < 0.0) return false;
    hit.rayT = max(tEnter, 0.0);
    hit.exitT = tExit;
    hit.drawnMin = lo;
    // Analytic face normal: whichever slab supplied the entry.
    vec3 sel = step(tsmall.yzx, tsmall) * step(tsmall.zxy, tsmall);
    hit.normal = -sign(ray.direction) * sel;
    return true;
}

// ---- Advection: the second motion kind, and the one that tests whether this generalises ------
//
// Sway moves by DISPLACEMENT: one bounded offset, resolved by asking which source lands in this cell.
// Advection moves by FLOW: a parcel rises and swirls, and what is drawn here is found by walking
// BACKWARDS along the flow until the trace reaches something that was actually modelled. That is the
// semi-Lagrangian step every fluid solver takes, run at render time against a static source volume
// instead of against last frame's grid.
//
// Two properties are what make it worth a second kind rather than a bigger amplitude on the first:
//
//   * ITERATING A SMALL FIELD COMPOSES INTO LARGE DISPLACEMENT. A parcel eight steps up the flow is
//     eight voxels from its source, out of a field that never exceeds a voxel per step. Sway cannot
//     go that far -- its envelope is a dilation of the source and would have to grow with the
//     amplitude -- while a flame's envelope is the shape the flame occupies, authored once.
//   * THE STEP COUNT IS THE AGE, and it is free: the loop counter already holds it. It is also
//     exactly what a flame needs to colour itself, which is what the material chain below spends it
//     on.
//
// FOLDING, WHICH IS THE ONE REAL WEAKNESS OF A FIELD WARP, COSTS NOTHING HERE. A flow can fold, and
// where it folds a backward trace finds two sources or none. For a solid surface that is a tear. For
// a medium it is invisible: two sources means the parcel is a little brighter, none means a gap in
// something already wispy. Fire is the case where the weakness does not bite, which is why it is the
// right second kind to build.

// The flow at a point, in VOXELS PER STEP, in WORLD orientation (the caller rotates it into the
// chunk's frame). Rise along the set's direction, plus a swirl that grows with age.
//
// The pattern SCROLLS ALONG THE RISE rather than merely varying with time, and that distinction is
// what makes a parcel appear to travel: the backward trace follows a path that itself moves, so one
// cell resolves to a different part of the flame from frame to frame. A field that only oscillated
// in place would shimmer without rising.
vec3 pjvAdvectFlow(MotionSet m, vec3 worldPos, float voxelSize, float t, float ageFrac) {
    // Voxel units, so a set's frequency means the same thing whatever a chunk's scale is -- the same
    // convention pjvMotionDisplacement uses.
    vec3 p = worldPos / max(voxelSize, 1e-6);
    vec3 q = p * m.frequency - m.direction * (m.speed * t * m.frequency);

    vec2 ab = pjvValueNoise2(q);
    float a = ab.x;
    float b = ab.y;

    // TURBULENCE GROWS WITH AGE, which is the difference between a glow and a flame. A real fire is
    // laminar where it leaves the fuel and ragged by the time it is a few voxels up; uniform
    // turbulence gives a body that wanders as a whole instead of one that breaks into tongues.
    float turb = m.turbulence * (1.0 + m.turbulenceGrowth * ageFrac);

    // The swirl is built in the plane PERPENDICULAR to the rise, with only a quarter of it along the
    // rise itself. Displacing mostly across the flow is what spreads a plume; displacing along it
    // only speeds parcels up and down, which reads as flicker rather than as motion.
    vec3 up = m.direction;
    vec3 seed = abs(up.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 e1 = normalize(cross(up, seed));
    vec3 e2 = cross(up, e1);
    vec3 swirl = (e1 * a + e2 * b + up * ((a + b) * 0.25)) * turb;

    return up + swirl;
}

// Follow the material chain `steps` times, along the `nextMaterial` link stored in the reserved byte
// of each palette entry's extra word. Blue at the base, yellow in the body, orange at the tip, with
// the hop count supplied by the parcel's age.
//
// LOCAL slot in, LOCAL slot out. The stored link is a component-local index, and so is what the hit
// carries: castRayThroughTree64 leaves a direct slot alone and fetchVoxelMaterialFromHit adds the
// palette base once, at the end. Re-adding it here as well would walk into another component's
// palette on the second hop.
//
// Bounded at three hops rather than looped until the link is zero. The chain is authored and short,
// and an unbounded walk over malformed data is an infinite loop in a fragment shader.
uint pjvChainWalk(uint localSlot, uint paletteOffsetIn, int steps) {
    uint slot = localSlot;
    for (int i = 0; i < 3; i++) {
        if (i >= steps) break;
        uint next = materialPaletteTexel(slot + paletteOffsetIn).w & 0xFFu;
        if (next == 0u) break;    // no link: the end of the chain
        slot = next;
    }
    return slot;
}

// What, if anything, is drawn in one geometry voxel by an advecting field.
//
// `cell` is a geometry voxel of this chunk; the drawn box IS that voxel, which is the other way round
// from sway (where the source holds still and the box moves). Returns a LOCAL palette slot.
//
// STEP ZERO TESTS THE CELL ITSELF, before any flow is applied, and that is not an off-by-one to be
// tidied away. The modelled fuel voxels carry the animated flag, so the geometry march skips them and
// hands responsibility for drawing them here; a trace that stepped before its first test would never
// look at the cell it started in, and the fuel would be drawn nowhere at all -- the same
// invisible-geometry failure this system has had twice by other routes.
bool pjvResolveAdvect(chunkHeader h, MotionSet ms, uint setIndex, ivec3 cell,
                      mat3 R, mat3 Rinv, vec3 chunkOriginWorld, float voxelWorldSize,
                      out uint outSlot) {
    outSlot = 0u;
    // `travel` is the rise in voxels and the step is one voxel, so it is also the step count. Capped
    // because this loop runs per sub-cell crossed and the whole cost of advection is in it.
    int maxSteps = int(clamp(ms.travel, 1.0, 32.0));
    float invSteps = 1.0 / float(maxSteps);
    float now = pjvAnimSeconds();

    // ---- THE LEAF CACHE, AND WHY THE TRACE CANNOT AFFORD TO GO WITHOUT ONE -------------------
    //
    // A step is one voxel, so consecutive probes land in the same 4x4x4 leaf four times out of five,
    // and a leaf's 64-bit occupancy mask answers all of them. Without it every step is a root-to-leaf
    // descent -- four levels, a texel and a popcount each -- and this loop runs per sub-cell crossed
    // of every marked cell along the ray. Measured on a scene with fire scattered through it, the
    // uncached form cost 69 ms against a 16 ms floor and the descents were nearly all of it.
    //
    // Same shape as the sway path's block leaf, for the same reason (see pjvProbeLeafNode): a tree64
    // leaf and a 4x4x4 block are the same box. `cacheMin` is the block the cached leaf covers;
    // ivec3(-1) is the empty key, which a real block corner cannot be.
    Tree64NodeData cacheLeaf;
    ivec3 cacheMin = ivec3(-1);
    bool haveCache = false;
    // ...and the slot cache on top of it, since a flame's fuel is a contiguous body of one material.
    uvec2 slotCache = uvec2(0xFFFFFFFFu, 0u);

    vec3 pos = vec3(cell) + vec3(0.5);
    ivec3 last = cell;
    vec3 flow = vec3(0.0);
    for (int i = 0; i <= maxSteps; i++) {
        float ageFrac = float(i) * invSteps;
        ivec3 c = ivec3(floor(pos));
        // A step that did not leave the cell has nothing new to test. `i == 0` forces the first one
        // through regardless, because `last` is seeded to the cell itself.
        if (i == 0 || any(notEqual(c, last))) {
            last = c;
            bool inBounds = all(greaterThanEqual(c, ivec3(0))) &&
                            all(lessThan(c, ivec3(int(h.resolution))));
            ivec3 blockMin = c & ivec3(~3);
            if (inBounds && (!haveCache || any(notEqual(blockMin, cacheMin)))) {
                haveCache = pjvProbeLeafNode(h.geometryStartIndex, h.resolution, blockMin, cacheLeaf);
                cacheMin = blockMin;
            }
            uint leafOffset = 0u;
            bool occupied = false;
            if (inBounds && haveCache) {
                uint z = calculateZOrderIndex4(uint(c.x - blockMin.x), uint(c.y - blockMin.y),
                                               uint(c.z - blockMin.z));
                if (checkZOrderInValidMasks(cacheLeaf.data1, cacheLeaf.data2, z)) {
                    leafOffset = leafMaterialListOffset(cacheLeaf, z);
                    occupied = true;
                }
            }
            if (occupied) {
                uint slot = materialID(leafOffset + h.materialIDStartIndex);
                uint flags;
                if (slot == slotCache.x) {
                    flags = slotCache.y;
                } else {
                    flags = (materialPaletteTexel(slot + h.paletteOffset).w >> 8u) & 0xFFu;
                    slotCache = uvec2(slot, flags);
                }
                // Only a voxel belonging to THIS field is a source. Another set's geometry standing
                // in the flame's path is scaffolding as far as this trace is concerned, and letting
                // it end the trace would draw a copy of it hanging in the air above itself.
                if ((flags & PJV_MAT_FLAG_ANIMATED) != 0u &&
                    ((flags >> PJV_MAT_MOTION_SHIFT) & PJV_MAT_MOTION_MASK) == setIndex) {
                    // ---- DISSOLVE --------------------------------------------------------------
                    // A parcel that has risen far enough burns out, and the GAPS that leaves are what
                    // make fire read as fire rather than as a lit volume.
                    //
                    // Sampled at the SOURCE rather than at the cell being drawn, so every cell
                    // tracing back to one source dies together: whole tongues detach and drift,
                    // instead of individual voxels speckling out. Scrolled on the same clock as the
                    // flow, so a gap travels upward with the flame rather than flickering in place.
                    if (ms.dissolve > 0.0) {
                        vec3 sWorld = R * ((vec3(c) + vec3(0.5)) * voxelWorldSize) + chunkOriginWorld;
                        vec3 sp = sWorld / max(voxelWorldSize, 1e-6) * (ms.frequency * 2.0)
                                - ms.direction * (ms.speed * now * ms.frequency * 2.0);
                        // pjvValueNoise is -1..1; the dissolve threshold is a 0..1 fraction.
                        if (pjvValueNoise(sp) * 0.5 + 0.5 < ageFrac * ms.dissolve) return false;
                    }
                    // Age picks the hop count. Three is the chain's bound, so a parcel at the tip
                    // walks it out and one at the base does not move off its own entry.
                    outSlot = pjvChainWalk(slot, h.paletteOffset, int(ageFrac * 3.0 + 0.5));
                    return true;
                }
            }
        }
        // BACKWARD: where did what is here come from? The flow is a world-space direction; the trace
        // walks the chunk's voxel lattice, so it is rotated in.
        //
        // ---- THE FIELD IS EVALUATED EVERY OTHER STEP, AND REUSED IN BETWEEN --------------------
        //
        // The step has to stay ONE VOXEL: it is what the probe above walks, and a two-voxel step
        // would jump straight over a one-voxel-thick piece of fuel and find nothing where there is
        // something. But the FIELD does not have to be resampled that often -- it is low frequency by
        // construction, varying over the tens of voxels the whole design rests on (see the note on
        // pjvValueNoise), so two consecutive steps a voxel apart get answers that differ by far less
        // than the swirl they are being scaled into.
        //
        // Halving the evaluations halves the trace's arithmetic, which is where its cost is: the
        // probes are nearly all cache hits off the block leaf above, while every step otherwise pays
        // a full trilinear noise.
        if ((i & 1) == 0) {
            vec3 pWorld = R * (pos * voxelWorldSize) + chunkOriginWorld;
            flow = Rinv * pjvAdvectFlow(ms, pWorld, voxelWorldSize, now, ageFrac);
        }
        pos -= flow;
    }
    return false;
}

// ---- The resolve -----------------------------------------------------------------------------
//
// Which source voxel, if any, is drawn in `cell` right now.
//
// ENUMERATE, DO NOT INVERT, and this is the single most important line in the file.
//
// The obvious implementation inverts the field once -- q = cell - round(d(cell)) -- and accepts if
// that q maps back. It does not work, and it fails in a way that looks like a rendering bug rather
// than a maths one. Rounding a coherent field makes q + round(d(q)) a STEP function, so wherever the
// rounding steps from k to k+1 a seam opens: cells on the seam have no source mapping to them, and
// the voxels that want to be there are drawn nowhere at all. The seam sweeps across the field as the
// wave moves, which reads as everything fading in and out of existence.
//
// So do not invert. The sources that could be drawn here all lie within a voxel of cell - d(cell),
// because d varies slowly. Test those, ask each where IT is drawn, and keep the ones whose box
// actually covers this cell. No inversion, no seam, and no cell can come up empty while a voxel
// wants to be in it.
void pjvResolveCell(Ray ray, vec3 invDir, chunkHeader h,
                    ivec3 cell, ivec3 K, ivec3 spanVec, ivec3 prevStep, ivec3 envCell,
                    mat3 Rinv, vec3 chunkOriginWorld, mat3 R, float voxelWorldSize,
                    Tree64NodeData blockLeaf, bool haveBlockLeaf, ivec3 blockMin,
                    inout int descentBudget, inout uvec2 slotCache, float tMinDrawn,
                    inout float bestT, inout ivec3 outSource, inout uint outList,
                    inout MovingVoxelHit outHit, inout bool found) {
    // The candidate window is a PURE TRANSLATE of the cell: base = cell + K, with K held constant
    // across the whole envelope cell by the caller. That is exact rather than approximate --
    // floor(cell - d0) == cell + floor(-d0) for integer cell -- and it is what makes the dedupe
    // below expressible at all, because two adjacent cells' windows then differ by exactly the step
    // between them.
    ivec3 base = cell + K;

    for (int iz = 0; iz < spanVec.z; iz++)
    for (int iy = 0; iy < spanVec.y; iy++)
    for (int ix = 0; ix < spanVec.x; ix++) {
        ivec3 o = ivec3(ix, iy, iz);

        // ---- DON'T PROBE THE SAME SOURCE TWICE IN ONE ENVELOPE CELL -------------------------
        //
        // Adjacent sub-cells' windows overlap heavily -- a unit step along one axis shifts a 3x1x3
        // window by one, so six of its nine candidates were probed a moment ago. Across the five or
        // six sub-cells a ray typically crosses, that is better than half the work in the hottest
        // loop of the animation path, spent re-deriving answers already had.
        //
        // Skipping is only sound because the accept test below is CELL-INDEPENDENT: a candidate
        // tested anywhere in this envelope cell is fully accounted for, since what it is tested
        // against is the envelope cell and the ray, neither of which changes as the sub-cell walk
        // advances. Under the old per-sub-cell coverage test this would have been a bug -- the same
        // source had a different answer for each sub-cell, so an earlier test said nothing about a
        // later one.
        //
        // Deduped against the immediately previous sub-cell only. A walk that turns leaves a few
        // duplicates in, which costs a probe and cannot lose one -- the safe direction.
        // any(notEqual(...)), not `!=`. bgfx cross-compiles this to HLSL, where a vector `!=` is
        // COMPONENT-WISE and yields a bool3 -- which an `if` then rejects as not a scalar. It is a
        // compile error rather than a silent wrong answer, but only in the HLSL backend, so it does
        // not show up until every other target has already built.
        if (any(notEqual(prevStep, ivec3(0)))) {
            ivec3 prevOffset = o + prevStep;
            if (all(greaterThanEqual(prevOffset, ivec3(0))) && all(lessThan(prevOffset, spanVec))) continue;
        }

        ivec3 q = base + o;

        // ---- PROBE FIRST, THEN EVALUATE THE FIELD ------------------------------------------
        // The order is reversed from the obvious one, and deliberately. Coverage cannot be tested
        // before the source is known, because the displacement depends on the source's OWN motion
        // set -- which is the fix for the overlap bug described at pjvAnimatedSourceAt. Probing first
        // is affordable only because of the in-leaf fast path: a candidate inside this block is a
        // 64-bit mask test with no descent, and the block is where nearly all of them are.
        uint list; uint qSet;
        if (!pjvAnimatedSourceAt(h, q, blockLeaf, haveBlockLeaf, blockMin,
                                 descentBudget, slotCache, list, qSet)) continue;

        MotionSet qms = pjvMotionSet(qSet);
        if (qms.kind != PJV_MOTION_SWAY) continue;

        vec3 qWorld = R * ((vec3(q) + vec3(0.5)) * voxelWorldSize) + chunkOriginWorld;
        vec3 dq = Rinv * pjvMotionDisplacement(qms, qWorld, voxelWorldSize, pjvAnimSeconds());
        // Snapped to the lattice. Quantisation is load-bearing rather than decorative: it makes the
        // drawn position a function of the VOXEL alone, so every ray in the image agrees about where
        // a given voxel is and a voxel is either wholly present or wholly absent. Without it two rays
        // crossing one voxel compute different offsets and it is drawn in two places at once, which
        // shows as single voxels sliced in half along silhouettes.
        vec3 dqSnapped = floor(dq + vec3(0.5));
        vec3 drawnMin = vec3(q) + dqSnapped;

        // ---- IS IT DRAWN IN THIS ENVELOPE CELL? --------------------------------------------
        //
        // Exact, and exactly equivalent to the per-sub-cell coverage test it replaces, because a
        // drawn box always lies WHOLLY INSIDE ONE ENVELOPE CELL: drawnMin is integral (an integer
        // source plus a snapped displacement), the box is one voxel, and envelope cells are aligned
        // to multiples of four. So it cannot straddle a boundary and there is no ambiguity to
        // resolve. Which sub-cell it lands in stops mattering -- the ray intersection below decides
        // whether this ray sees it, and the ray can only reach a box inside a sub-cell it crosses.
        //
        // The old test asked whether the box covered THIS sub-cell, which is the same question asked
        // once per sub-cell instead of once per candidate. Answering it against the envelope cell is
        // what the dedupe above needs, and it drops the sub-cell out of the accept path entirely.
        if (any(notEqual(ivec3(floor(drawnMin * 0.25)), envCell))) continue;

        MovingVoxelHit mv;
        if (!pjvMovingVoxelHit(ray, invDir, vec3(q), dqSnapped, mv)) continue;

        // NEAREST wins, and now across the WHOLE envelope cell rather than within one sub-cell. The
        // walk is spatial rather than along the ray, and with two motion sets overlapping two sources
        // really can both be drawn in one cell, so "first" would be an arbitrary order. Accumulating
        // globally also makes the ordering exact instead of merely sub-cell-ordered.
        if (mv.rayT >= bestT) continue;
        // At or before the peel's resume point: a layer the caller has already consumed. Rejected
        // HERE as well as by the cell seeding above, because a source drawn near a cell boundary can
        // be hit before the cell the walk started in.
        if (mv.rayT < tMinDrawn) continue;
        bestT = mv.rayT;
        outSource = q;
        outList = list;
        outHit = mv;
        found = true;
    }
}

// The entry face of an axis-aligned box, computed analytically.
//
// Needed because a march's carried normal is NOT always usable. marchRayThroughTree64_DDA seeds
// hitNormal from getRayBoxEntry against the whole volume, and that function returns (0,0,0) when the
// ray ORIGIN IS INSIDE the volume -- which is the normal case for a camera flying through a scene.
// The seed is replaced on the first committed DDA step, so an ordinary hit is fine; a hit found
// during the INITIAL DESCENT, before any step, keeps the zero.
//
// That is not a cosmetic problem. A renderer's miss test is typically `dot(n, n) < 0.5` alongside the
// size and distance checks -- AdvancedRenderer's gbuffer does exactly this -- so a hit carrying a zero
// normal is read as SKY. The geometry is found, returned, and then discarded as background: it reads
// on screen as a voxel you can see straight through, with no clue that anything was hit at all.
vec3 pjvBoxEntryNormal(Ray ray, vec3 boxMin, float size) {
    vec3 invD = 1.0 / ray.direction;
    vec3 t0 = (boxMin - ray.origin) * invD;
    vec3 t1 = (boxMin + vec3(size) - ray.origin) * invD;
    vec3 tsmall = min(t0, t1);
    // The entry is the LATEST of the three slab entries; that axis supplies the face.
    vec3 sel = step(tsmall.yzx, tsmall) * step(tsmall.zxy, tsmall);
    return -sign(ray.direction) * sel;
}

// Walks this chunk's envelope and returns the nearest animated voxel actually drawn along the ray.
//
// `ray` is in the chunk's GEOMETRY voxel frame, the same one marchRayThroughTree64_DDA uses.
// `maxT` bounds the search: nothing drawn beyond it can win, because the geometry march already
// found something nearer. Without it the envelope is walked along the WHOLE ray even when the answer
// was settled in the first cell -- which on a canopy is most of the cost, since a ray that hits a
// trunk immediately would still walk every envelope cell behind it.
// `tMin` is the peel's resume point, in this march's own geometry-voxel units, and it is a PARAMETER
// rather than a query field for the reason the whole promotion turned on: the prototype's forked
// traversal dropped tMin, and without it the animated path cannot participate in the transparency
// peel at all. A peel restart raises tMin past the layer it just consumed and asks again; a march
// that ignores that either returns the same voxel forever or -- as this one did -- is skipped on
// every pass after the first, which makes animated geometry behind any transparent layer vanish.
SceneIntersectData pjvAnimatedMarch(Ray ray, RayQuery rayQuery, chunkHeader h,
                                    mat3 R, mat3 Rinv, vec3 chunkOriginWorld, float voxelWorldSize,
                                    float maxT, float tMin) {
    SceneIntersectData miss;
    miss.foundBox.size = -1.0; miss.rayT = -1.0; miss.exitT = -1.0; miss.normal = vec3(0.0);
    miss.voxelCoord = ivec3(0); miss.steps = 0u;
    miss.materialListIndex = MATERIAL_INDEX_NEEDS_DESCENT; miss.paletteOffset = 0u;

    uint envRes = max(h.resolution / 4u, 4u);

    // The envelope march runs in ENVELOPE cell units -- one cell is one unit -- so the geometry-frame
    // ray is scaled down by four. Only the origin scales; the direction is left alone, which is what
    // keeps t comparable between the two frames after the reciprocal scaling below.
    Ray envRay;
    envRay.origin = ray.origin * 0.25;
    envRay.direction = ray.direction;

    BoxAABB envBox;
    envBox.position = vec3(0.0);
    envBox.size = float(envRes);

    vec3 invDir = 1.0 / ray.direction;

    // ---- WHY THE RESTART ADVANCES THE ORIGIN INSTEAD OF RAISING tMin -------------------------
    //
    // marchRayThroughTree64_DDA always begins at the volume entry -- `rayT =
    // getRayBoxEntryDistanceForSureHit(ray, boundingBox)` -- and enforces tMin by DISCARDING leaves
    // that fall before it. It does not skip ahead. So a restart at tMin re-walks every cell already
    // consumed, and crossing N cells costs N(N+1)/2 DDA steps rather than N.
    //
    // That is invisible at a budget of 8 and dominant at a budget of tens: on a canopy interior it
    // measured 49 ms against 7.5 ms for the same view with animation off, and the quadratic was
    // nearly all of the difference.
    //
    // Advancing the ORIGIN makes the skip real. The march's own entry computation then lands on the
    // restart point, so each restart costs one root descent plus the steps to the next set cell, and
    // the total is linear in cells crossed. Both entry helpers already handle an origin inside the
    // box -- getRayBoxEntryDistanceForSureHit clamps its result to zero, and
    // determineTraversalCoordinatesFromRayAndBox clamps the entry point into the box before flooring
    // -- which is the same path a camera standing inside a chunk takes every frame.
    //
    // The offset is applied to the ORIGINAL origin each time rather than accumulated onto the
    // previous one, so repeated restarts do not compound rounding into the ray.
    vec3 envOriginBase = envRay.origin;
    // Envelope-frame t already consumed. Seeded from the caller's tMin rather than from zero, which
    // is what makes a peel restart skip the cells it has already been through instead of re-walking
    // them. The envelope frame scales the ORIGIN by a quarter and leaves the direction alone, so a
    // geometry-frame t maps to a quarter of itself here -- the same factor the *4.0 conversions below
    // undo.
    float tAdvance = max(tMin, 0.0) * 0.25;

    // The far side of the envelope volume. Once the advanced origin is past it there is nothing left
    // to find, and marching from an origin beyond the box is not merely wasted: the entry helpers
    // clamp an outside point ONTO a face, so the march would walk a cell the ray never crosses.
    vec3 envT0 = (envBox.position - envRay.origin) * invDir;
    vec3 envT1 = (envBox.position + vec3(envBox.size) - envRay.origin) * invDir;
    vec3 envTFar = max(envT0, envT1);
    float envExitT = min(min(envTFar.x, envTFar.y), envTFar.z);

    PeelAccum noPeel = pjvNoPeel();

    // The palette-flag cache, held for the whole march. See pjvAnimatedSourceAt for what it saves.
    // The key is a material slot; 0xFFFFFFFF is "empty", which a one-byte slot cannot collide with.
    uvec2 slotCache = uvec2(0xFFFFFFFFu, 0u);

    // The envelope is marched at FULL resolution, never coarsened, and this is not a quality choice.
    // An envelope cell is an occupancy oracle -- "something may be drawn in this 4x4x4 box" -- and a
    // coarsened node merging sixty-four of them answers a question nobody asked. Worse, it comes back
    // with MATERIAL_INDEX_NEEDS_DESCENT, so the motion byte cannot be read and the cell resolves to
    // nothing; the ray then spends its whole restart budget on coarse nodes and reports a miss, and
    // the animated geometry inside them is drawn nowhere at all.
    //
    // The caller's query carries a distance-LOD ramp meant for geometry (AdvancedRenderer's primary
    // ray sets PJV_LOD_FOOTPRINT), so it cannot be reused here. A fresh one, with the step budget
    // capped: this tree is three levels deep and a ray that escapes it terminates on its own.
    RayQuery envQuery = pjvPrimaryQuery(min(rayQuery.maxRaySteps, 128u));

    // ---- HOW MANY CELLS A RAY MAY CROSS, AND WHY THIS NUMBER IS NOT A QUALITY KNOB -------------
    //
    // Each iteration consumes ONE envelope cell that turned out to have nothing drawn in it, by
    // restarting the march past it. That was originally 8, on the reasoning that a restart is cheap
    // here -- one descent of a three-level tree rather than a whole scene query.
    //
    // The reasoning was right about the cost and wrong about the COUNT. A ray entering a canopy at a
    // grazing angle crosses dozens of envelope cells before it reaches the blade it should draw,
    // because the envelope is a dilation of everything that moves and foliage is dense. Exhausting
    // the budget returns a MISS, so the geometry beyond is drawn nowhere -- and it fails worst
    // exactly where foliage is thickest, which is where it is least likely to be read as a budget
    // and most likely to be read as "the traversal is broken".
    //
    // Raised to a bound that a ray crossing an entire chunk cannot reach: an envelope is
    // resolution/4 cells across, so 3*envRes/4 covers the body diagonal with room to spare. It stays
    // a bound rather than becoming unbounded, because a non-advancing tMin would otherwise hang the
    // shader, and peelNextTMin's ULP floor makes non-advance unlikely rather than impossible.
    //
    // This is still the wrong SHAPE -- consuming a cell in place, the way the transparency peel does
    // within a safe interval, would avoid the restart entirely. That is a real piece of work and this
    // is not: correctness first, and the restart is measurably affordable.
    int maxCells = int(min(3u * envRes / 4u, 4096u));
    for (int attempt = 0; attempt < maxCells; attempt++) {
        if (tAdvance >= envExitT) return miss;
        // The march starts where the last one left off. Its rayT is measured from THIS origin, so
        // every use of it below is lifted back to the whole ray by tAdvance.
        Ray stepRay;
        stepRay.origin = envOriginBase + ray.direction * tAdvance;
        stepRay.direction = ray.direction;
        SceneIntersectData cellHit = marchRayThroughTree64_DDA(
            stepRay, envQuery, 0.0, envBox, h.envelopeStartIndex, 0u, envRes, 0u,
            h.envelopeMotionStartIndex, h.paletteOffset, -1.0, 1.0, false, noPeel);
        if (cellHit.foundBox.size < 0.0 || cellHit.rayT < 0.0) return miss;
        float cellEnterT = cellHit.rayT + tAdvance;   // envelope-frame, from the ray's own origin
        float cellExitT  = cellHit.exitT + tAdvance;
        // The envelope march runs in cell units; the caller's bound is in geometry-voxel units.
        if (cellEnterT * 4.0 >= maxT) return miss;

        // Which motion set marked this cell.
        uint setIndex = 0u;
        if (cellHit.materialListIndex != MATERIAL_INDEX_NEEDS_DESCENT) {
            setIndex = uint(materialID(cellHit.materialListIndex + h.envelopeMotionStartIndex)) & PJV_MAT_MOTION_MASK;
        }
        // The cell's set is a bracketing HINT and nothing more -- see pjvResolveCell. It must not
        // gate whether the block is walked at all: a cell marked by one field routinely contains
        // sources belonging to another, and gating here would make those invisible for exactly the
        // reason the set must not gate the candidates either.
        MotionSet ms = pjvMotionSet(setIndex);

        if ((rayQuery.flags & PJV_Q_ANIM_DEBUG_SOLID) != 0u) {
            // The whole envelope cell, drawn where it is. No resolve, no candidates, no field.
            SceneIntersectData dbg;
            dbg.voxelCoord = ivec3(cellHit.voxelCoord) * 4;
            dbg.foundBox.position = vec3(dbg.voxelCoord);
            dbg.foundBox.size = 4.0;
            dbg.rayT = cellEnterT * 4.0;
            dbg.exitT = cellExitT * 4.0;
            // The envelope frame differs from the geometry frame only by a scale on the origin, so a
            // face normal transfers across unchanged -- when there is one. See pjvBoxEntryNormal for
            // why there might not be.
            dbg.normal = dot(cellHit.normal, cellHit.normal) > 0.5
                       ? cellHit.normal
                       : pjvBoxEntryNormal(ray, vec3(dbg.voxelCoord), 4.0);
            dbg.steps = cellHit.steps;
            dbg.headerIndex = 0u;
            dbg.paletteOffset = 0u;
            // The MOTION BYTE's own leaf offset, not the descent sentinel.
            //
            // The sentinel is what this returned first, and it made the debug view useless in the one
            // situation it exists for: the caller descends on voxelCoord in the GEOMETRY tree, which
            // at an envelope cell's corner is usually empty, so every cell shaded as empty space and
            // the view looked identical to the bug it was meant to expose.
            //
            // Slot 0 of this component's palette, named directly.
            //
            // Not the envelope's own motion byte, which would be the "honest" choice and is not
            // available: castRayThroughTree64 lifts every returned index by the GEOMETRY's material
            // base, and an index into the ENVELOPE's array lifted by the geometry's base points at
            // neither. Cancelling that add from in here would work by arithmetic and would be a trap
            // for whoever next changes either base.
            //
            // Slot 0 survives the geometry's base correctly, always exists, and is a real material.
            // Every envelope cell therefore shades the same flat colour -- which is exactly what this
            // view wants: it answers WHERE cells are, and it is not trying to say anything about what
            // is in them.
            dbg.materialListIndex = 0u;
            return dbg;
        }

        {
            // Walk the 4x4x4 geometry cells of this envelope cell in ray order. At most ten of the
            // sixty-four are crossed, which is why this is a DDA rather than a loop over the block.
            ivec3 envCell = ivec3(cellHit.voxelCoord);
            ivec3 blockMin = envCell * 4;
            // One descent for the whole envelope cell. See pjvProbeLeafNode: the geometry leaf
            // covering this block and the envelope cell are the same box.
            Tree64NodeData blockLeaf;
            bool haveBlockLeaf = pjvProbeLeafNode(h.geometryStartIndex, h.resolution, blockMin, blockLeaf);

            // ---- THE BRACKET HINT, ONCE PER ENVELOPE CELL RATHER THAN ONCE PER SUB-CELL --------
            //
            // d0 only has to get the candidate NEIGHBOURHOOD roughly right; which sources are
            // actually eligible is decided per candidate, against that candidate's OWN motion set.
            // So sampling it at the block centre instead of each sub-cell centre is within what it
            // already promises: the field varies over tens of voxels by construction (see the note on
            // MotionKind::Sway), the block is four across, and the span carries a spare voxel either
            // side precisely to absorb this kind of slack.
            //
            // Hoisting it is not about the six noise evaluations saved -- those measured as free. It
            // is that a per-sub-cell d0 makes each window an independent rounding of a slightly
            // different number, and windows that are not exact translates of one another cannot be
            // deduped. See pjvResolveCell.
            vec3 blockWorld = R * ((vec3(blockMin) + vec3(2.0)) * voxelWorldSize) + chunkOriginWorld;
            vec3 d0 = Rinv * pjvMotionDisplacement(ms, blockWorld, voxelWorldSize, pjvAnimSeconds());

            // BRACKET WITH THE FREE FIELD, NOT THE SNAPPED ONE.
            //
            // The candidates must straddle the true source, so K is chosen from the CONTINUOUS
            // displacement. With the snapped value it does not work: snapped, cell - d0 is already an
            // integer, floor() is the identity, and the span degenerates -- silently dropping the
            // source whose own displacement rounded one step FURTHER than this cell's did. That is
            // precisely the cells straddling a rounding boundary, so it is a one-voxel line of
            // geometry drawn nowhere, running along the boundary and sweeping with the wave.
            //
            // Widened by one either side of the two-candidate span, because different motion sets
            // have different amplitudes and the hint describes only one of them. That extra ring is
            // what makes two overlapping fields resolve, which is most of a canopy floor.
            bool advecting = (ms.kind == PJV_MOTION_ADVECT) && ms.travel > 0.0;

            bool horizontal = abs(ms.direction.y) < 1e-4;
            ivec3 spanVec = ivec3(3, horizontal ? 1 : 3, 3);
            ivec3 K = ivec3(floor(-d0)) - ivec3(1, horizontal ? 0 : 1, 1);
            // A horizontal field draws a source only in its own row, so the window must sit exactly
            // on the cell's row. Forced rather than relied upon: d0.y is zero for such a field and
            // floor(-0.0) is 0, but a direction a hair off horizontal would otherwise shift the row.
            if (horizontal) K.y = 0;

            // Entry into the block, in geometry-frame t.
            float tBlock = cellEnterT * 4.0;
            // RELATIVE nudge, not an absolute one. A fixed 1e-4 is below what a float32 mantissa can
            // represent once the ray is a few hundred voxels out, so the addition returns tBlock
            // unchanged and the start cell is decided by whichever side of the boundary the rounding
            // fell -- which varies across the screen and shows as patches starting a cell late.
            float nudge = max(abs(tBlock), 1.0) * 1e-5;
            vec3 p = ray.origin + ray.direction * (tBlock + nudge);
            ivec3 cell = clamp(ivec3(floor(p)), blockMin, blockMin + ivec3(3));
            ivec3 stepDir = ivec3(sign(ray.direction));
            vec3 nextB = vec3(cell) + step(vec3(0.0), ray.direction);
            vec3 tMax = (nextB - ray.origin) * invDir;
            vec3 tDelta = abs(invDir);

            // Accumulated across the whole block rather than returned from the first sub-cell that
            // resolves, because the dedupe means a candidate probed at one sub-cell may turn out to
            // be drawn at a later one. Ordering is preserved by taking the nearest rayT, which is
            // stricter than the sub-cell order it replaces.
            float bestT = 1e30;
            bool found = false;
            ivec3 bestSrc = cell; uint bestList = MATERIAL_INDEX_NEEDS_DESCENT;
            MovingVoxelHit bestHit;
            // Zero means "no previous window", which is how the first sub-cell probes all of its
            // candidates instead of deduping every one of them away.
            ivec3 prevStep = ivec3(0);
            float subEnterT = tBlock;

            for (int i = 0; i < 12; i++) {
                if (any(lessThan(cell, blockMin)) || any(greaterThan(cell, blockMin + ivec3(3)))) break;
                // Nothing drawn from here on can beat what we already have. Sub-cells are visited in
                // increasing t, and a source drawn in one has its box inside it, so its hit cannot
                // precede that sub-cell's entry. Safe alongside the dedupe for the same reason: a
                // source drawn in sub-cell S is always in S's own window, so skipping the rest of the
                // walk cannot drop a source that would have won.
                if (found && subEnterT >= bestT) break;
                if (subEnterT >= maxT) break;

                // PER SUB-CELL, not per block. The candidate window straddles the block boundary for
                // every cell on the block's edge -- which at a 4-voxel block is most of them -- so a
                // budget shared across the whole walk is spent by the first cell or two and every
                // later cell then fails to find sources that are really there. Those voxels are drawn
                // nowhere, which is the same invisible-geometry failure by a different route.
                int descentBudget = 4;

                pjvResolveCell(ray, invDir, h, cell, K, spanVec, prevStep, envCell,
                               Rinv, chunkOriginWorld, R, voxelWorldSize,
                               blockLeaf, haveBlockLeaf, blockMin,
                               descentBudget, slotCache, tMin,
                               bestT, bestSrc, bestList, bestHit, found);

                // ---- ...and the advecting field, if this cell was marked by one ----------------
                //
                // BOTH resolves run, rather than one or the other on the cell's kind. The sway pass
                // above is set-agnostic by construction -- it asks each candidate which field it
                // belongs to -- so gating it on the cell's byte would resurrect exactly the bug that
                // byte is documented not to gate: grass at the foot of a fire would be skipped by the
                // geometry march as animated, refused here as the wrong kind, and drawn nowhere.
                //
                // Advection is the other way round and cannot be set-agnostic. There is no candidate
                // to ask: what is drawn here is found by tracing a field backwards, so the field has
                // to be named before the trace can start, and the cell's byte is the only thing that
                // names it. That asymmetry is why the bake gives an advecting set PRIORITY over a
                // swaying one when both reach a cell -- a sway source that loses the byte is still
                // found, an advect field that loses it has no other route in.
                if (advecting) {
                    uint advSlot;
                    if (pjvResolveAdvect(h, ms, setIndex, cell, R, Rinv, chunkOriginWorld,
                                         voxelWorldSize, advSlot)) {
                        // The drawn box is the sub-cell itself: advection moves the CONTENTS of a
                        // fixed lattice, where sway moves a fixed voxel to a new place.
                        MovingVoxelHit amv;
                        if (pjvMovingVoxelHit(ray, invDir, vec3(cell), vec3(0.0), amv) &&
                            amv.rayT < bestT && amv.rayT >= tMin) {
                            bestT = amv.rayT;
                            // Identity is the DRAWN cell here, and that is the same rule sway
                            // follows, not a different one: voxelCoord is what a temporal filter
                            // gates its history on, so it has to be whichever of the two holds
                            // still. For sway that is the source; for advection it is the cell.
                            bestSrc = cell;
                            // No voxel owns this palette entry -- the chain walk arrived at it by
                            // age. See PJV_MATERIAL_DIRECT_SLOT.
                            bestList = PJV_MATERIAL_DIRECT_SLOT | (advSlot & PJV_MATERIAL_SLOT_MASK);
                            bestHit = amv;
                            found = true;
                        }
                    }
                }

                if (tMax.x <= tMax.y && tMax.x <= tMax.z) { subEnterT = tMax.x; prevStep = ivec3(stepDir.x, 0, 0); cell.x += stepDir.x; tMax.x += tDelta.x; }
                else if (tMax.y <= tMax.z)                { subEnterT = tMax.y; prevStep = ivec3(0, stepDir.y, 0); cell.y += stepDir.y; tMax.y += tDelta.y; }
                else                                      { subEnterT = tMax.z; prevStep = ivec3(0, 0, stepDir.z); cell.z += stepDir.z; tMax.z += tDelta.z; }
            }

            if (found) {
                SceneIntersectData out_;
                // Identity is the SOURCE cell, not the drawn one, and this matters downstream:
                // voxelCoord is what a temporal filter gates its history on, and a drawn position
                // moves every frame by construction. The source holds still.
                out_.voxelCoord = bestSrc;
                out_.materialListIndex = bestList;
                out_.foundBox.position = bestHit.drawnMin;
                out_.foundBox.size = 1.0;
                out_.rayT = bestHit.rayT;
                out_.exitT = bestHit.exitT;
                // Belt and braces: pjvMovingVoxelHit computes this analytically and should never
                // hand back a zero, but a hit discarded as sky is invisible and unattributable, so
                // it is not a failure worth leaving to trust.
                out_.normal = dot(bestHit.normal, bestHit.normal) > 0.5
                            ? bestHit.normal
                            : pjvBoxEntryNormal(ray, bestHit.drawnMin, 1.0);
                out_.steps = cellHit.steps;
                out_.headerIndex = 0u;
                out_.paletteOffset = 0u;
                return out_;
            }
        }

        // Nothing drawn in this envelope cell. Advance past it and ask again.
        //
        // PAST ITS EXIT, and this is the opposite of what the tMin form needed -- the two are not
        // interchangeable and getting it wrong costs fifty times the frame.
        //
        // With a tMin the march re-walked from the volume entry and rejected cells on `rayT >= tMin`,
        // so the floor had to land just INSIDE the cell being left: at its exit plane the gate would
        // have excluded the next cell too, whose entry sits at exactly the same t. (peelNextTMin is
        // that floor, and it says so.)
        //
        // Advancing the ORIGIN inverts the requirement, because there is no longer a gate -- where
        // the origin sits is what decides which cell the march descends into. An origin placed just
        // inside the cell just consumed lands the march back on that same cell at rayT == 0, forever,
        // until the budget runs out. So clear the exit instead, by a RELATIVE epsilon: a fixed one is
        // below what a float32 mantissa can represent a few hundred units out, and the addition then
        // returns the exit unchanged.
        //
        // Nothing is skipped by this. The new origin sits just inside the NEXT cell, and
        // determineTraversalCoordinatesFromRayAndBox floors it into that cell directly -- it is
        // already direction-aware on exact grid planes, which is the case this lands on.
        tAdvance = cellExitT + max(abs(cellExitT), 1.0) * 1e-5;
    }
    return miss;
}

//Cast the ray through the tree64 bounding box.
// `nearestSoFar` is the closest hit the SCENE has produced up to this call, in world units, or 1e30
// when nothing has been found yet. It bounds the animated march only. The geometry march does not
// need it -- the broadphase already declines to enter a chunk whose whole box starts beyond it --
// but a chunk the ray enters BEFORE that distance can easily hold all of its animated geometry
// after it, and without this the envelope is then walked in full for an answer that cannot win.
SceneIntersectData castRayThroughTree64(Ray ray, RayQuery rayQuery, float tMin, uint headerIndex, float limitT, float nearestSoFar, inout PeelAccum peel) {
    chunkHeader header = headers(headerIndex);

    uint tree64StartIndex = header.geometryStartIndex;
    uint tree64EndIndex = header.geometryEndIndex;

    BoxAABB tree64BoundingBox;
    //tree64BoundingBox.position = vec3(header.positionX, header.positionY, header.positionZ);
    //tree64BoundingBox.size = header.scale;
    tree64BoundingBox.position = vec3(0.0, 0.0, 0.0);
    tree64BoundingBox.size = header.resolution; // Allows each voxel to be of scale 1 for ray marching.

    // Bring the ray into the chunk's local (axis-aligned) voxel frame. The chunk is placed
    // in the world by translation P, rotation R and uniform scale (scale/resolution), so the
    // inverse is: subtract P, rotate by R^-1 = transpose(R), then scale to voxel units.
    // R^-1 is orthonormal so ray length is preserved and the rayT scaling below is unchanged.
    mat3 R = rotationFromQuat(header.rotation);
    mat3 Rinv = transpose(R);
    vec3 P = vec3(header.positionX, header.positionY, header.positionZ);
    Ray transformedRay;
    transformedRay.origin = (Rinv * (ray.origin - P)) * (tree64BoundingBox.size / header.scale);
    transformedRay.direction = Rinv * ray.direction;

    // Nudge near-zero components away from zero so 1/direction stays finite. SIGN-PRESERVING: the
    // test is on the magnitude, so assigning a bare +epsilon silently reversed a small NEGATIVE
    // component (-3e-7 became +1e-6), which reverses the DDA's step direction and the entry/exit slab
    // ordering on that axis -- a direction-dependent wrong walk for any near-axis-aligned ray.
    //
    // Barely visible while only the FIRST hit mattered, because a flipped near-zero axis rarely
    // changes which voxel is met first. It matters once a ray has to be walked correctly through a
    // sequence of voxels, as the transparency peel does: the march then follows a slightly different
    // ray than the caller's, so the voxels it visits and the segment lengths measured against the
    // caller's ray stop agreeing.
    float epsilon = 1e-6;
    if (abs(transformedRay.direction.x) < epsilon)
        transformedRay.direction.x = transformedRay.direction.x < 0.0 ? -epsilon : epsilon;
    if (abs(transformedRay.direction.y) < epsilon)
        transformedRay.direction.y = transformedRay.direction.y < 0.0 ? -epsilon : epsilon;
    if (abs(transformedRay.direction.z) < epsilon)
        transformedRay.direction.z = transformedRay.direction.z < 0.0 ? -epsilon : epsilon;

    // tMin arrives in world units and the march runs in voxel units. The origin above was scaled by
    // resolution/scale while the direction was left unscaled, so a world distance t maps to
    // t * (resolution/scale) locally -- exactly the factor the returned rayT is divided by further
    // down. Converting here rather than at the three comparison sites keeps the march working in one
    // set of units throughout.
    float localTMin = tMin * (tree64BoundingBox.size / header.scale);
    // The peel's interval stays in WORLD units and the march is told how to convert into them. See
    // the note on marchRayThroughTree64_DDA: the peel's state is shared with the scene-level loop, so
    // it has to be in one set of units and world is the one both paths can name.
    float localToWorld = header.scale / tree64BoundingBox.size;

    IntersectionResult rootIntersect = getRayBoxEntry(transformedRay, tree64BoundingBox);

    SceneIntersectData tree64Intersect;
    tree64Intersect.foundBox.size = -1;
    tree64Intersect.voxelCoord = ivec3(0);
    tree64Intersect.rayT = -1.0;
    tree64Intersect.exitT = -1.0;
    tree64Intersect.normal = vec3(0.0);
    tree64Intersect.materialListIndex = MATERIAL_INDEX_NEEDS_DESCENT;
    tree64Intersect.paletteOffset = 0u;

    // ---- Is animated geometry in play for this chunk? -----------------------------------------
    //
    // Both conditions are required, and the ENVELOPE one is a safety property rather than an
    // optimisation. Skipping animated voxels in the geometry march hands responsibility for drawing
    // them to pjvAnimatedMarch; if this chunk has no envelope, there is nothing for that march to
    // find and the voxels are drawn NOWHERE -- not stale, not misplaced, absent. So a chunk whose
    // bake did not produce an envelope renders its animated materials at rest, statically, which is
    // a degradation with a visible cause rather than a hole with none.
    bool animate = (rayQuery.flags & PJV_Q_ANIMATION) != 0u && chunkHasEnvelope(header);

    if(rootIntersect.distance >= 0){
        // The bounds that apply to the animated march however it is ordered below.
        //
        // The resolve distance has to be the SAME bound the geometry march uses to decide what to
        // skip, or there is a band of the ray where each thinks the other owns those voxels and
        // neither draws them.
        float animBound = rayQuery.animResolveDistance / max(localToWorld, 1e-9);
        // ...and what the rest of the scene has already found. Chunks are marched in list order, not
        // front to back, so a nearer chunk's hit is routinely already in hand while this one is
        // walked. Consistent with the geometry march's skip because closestDistance only ever
        // decreases, so nothing skipped here can become visible later.
        if (nearestSoFar < 1e29) animBound = min(animBound, nearestSoFar / max(localToWorld, 1e-9));

        // ---- IN-PLACE PEELING IS SWITCHED OFF WHILE ANIMATING ---------------------------------
        //
        // The geometry march can consume transparent voxels in place, anywhere inside the interval
        // the caller granted -- including PAST where the animated hit turns out to be, because the
        // animated hit is not known yet. The merge below then hands back the animated voxel carrying
        // a transmittance that includes panes standing BEHIND it, so an animated voxel is tinted by
        // glass it is in front of. Nothing about that looks wrong enough on screen to attribute.
        //
        // Giving the interval up is what makes the two compose, and it costs only the rays that
        // asked for both. Correctness is then the outer peel loop's, where it already was: each pass
        // returns the nearest hit of EITHER kind at or beyond tMin, the loop consumes it if it is
        // transparent and raises tMin past it, and both marches honour tMin. Ordering is exact
        // because nothing is ever consumed out of turn.
        //
        // The alternative -- resolving the animation first so the interval could be capped at it --
        // needs a second pjvAnimatedMarch call site, and a second inlined copy of that function
        // overflows the SPIR-V id space in this shader. That is a real constraint, not a preference.
        // Refraction does NOT give the interval up. It used to, on the reasoning that the bend is
        // applied in the outer loop and a march that consumes in place crosses interfaces the outer
        // loop never hears about. The reasoning was right and the remedy was too broad: see
        // pjvPeelConsume, which now hands back the interfaces that actually bend and keeps peeling
        // the ones that do not. A restart per transparent VOXEL caps a body at MAX_PEEL_ITERATIONS;
        // a restart per refracting INTERFACE costs one per surface, which is what it has to cost.
        float geometryLimitT = (animate && peel.active) ? -1.0 : limitT;

        tree64Intersect = marchRayThroughTree64_DDA(transformedRay, rayQuery, localTMin, tree64BoundingBox, tree64StartIndex, tree64EndIndex, header.resolution, header.traversalLOD, header.materialIDStartIndex, header.paletteOffset, geometryLimitT, localToWorld, animate, peel);

        SceneIntersectData animHit;
        animHit.foundBox.size = -1.0;
        animHit.rayT = -1.0;
        if (animate) {
            // Bounded by whatever the geometry already found. An animated voxel behind an opaque
            // surface cannot be seen, and searching for it is the single largest avoidable cost here.
            //
            // Still correct when that nearest static voxel is TRANSPARENT rather than opaque: the
            // bound hides an animated voxel behind it for this pass only, and the peel's next pass --
            // with tMin past the consumed layer -- searches the range that was excluded.
            float animLimit = (tree64Intersect.foundBox.size > 0.0 && tree64Intersect.rayT >= 0.0)
                            ? tree64Intersect.rayT : 1e30;
            animLimit = min(animLimit, animBound);
            animHit = pjvAnimatedMarch(transformedRay, rayQuery, header, R, Rinv, P,
                                       header.scale / tree64BoundingBox.size, animLimit, localTMin);
        }

        // Merged in the SAME frame and units as the geometry hit so the nearer of the two wins on a
        // straight comparison. Merging after the world lift-back below would compare two numbers that
        // had been through different transforms.
        if (animHit.foundBox.size > 0.0 && animHit.rayT >= 0.0 &&
            (tree64Intersect.foundBox.size < 0.0 || animHit.rayT < tree64Intersect.rayT)) {
            tree64Intersect = animHit;
            // The hit just changed to a voxel the GEOMETRY march never returned, so anything it
            // published about its own answer is now about a different voxel. Invalidate rather than
            // fetch: the animated hit carries its own materialListIndex, and by the time the caller
            // reads it the palette base and offset have been folded in, so fetchVoxelMaterialFromHit
            // resolves it correctly and cheaply. Leaving this true is what shaded an animated voxel
            // with the albedo of whatever stood behind it.
            //
            // It is also what lets the PEEL see an animated voxel's transparency: the peel loop
            // fetches the material itself when this is false, so an animated voxel that is
            // transparent is crossed and accumulated exactly like a static one, with no special case.
            peel.hitMaterialValid = false;
        }

        // ---- A HIT FOUND WITHOUT A DDA STEP CARRIES A ZERO NORMAL, AND READS AS SKY -------------
        //
        // marchRayThroughTree64_DDA seeds hitNormal from getRayBoxEntry against the WHOLE volume, and
        // that function returns (0,0,0) when the ray origin is already inside the volume. The seed is
        // replaced on the first committed DDA step, so an ordinary hit is fine -- but a hit found
        // during the INITIAL DESCENT, before any step, keeps the zero. Every renderer's miss test is
        // some form of `dot(n, n) < 0.5`, so such a hit is found, returned, and then discarded as
        // background.
        //
        // This was documented as latent (see pjvBoxEntryNormal) on the grounds that it needs the ray
        // to start inside the geometry, which only a camera flying through a solid voxel could do.
        // REFRACTION DOES IT ROUTINELY: a bent ray is restarted at the interface voxel's exit plus a
        // quarter voxel, which is right up against whatever is behind the glass. Any surface close
        // enough to be reached without a step comes back with a zero normal.
        //
        // On screen that is not a hole, which is what makes it hard to place: the pixels whose bent
        // ray happens to need a step DO shade, and the ones that reach the voxel directly do not, so
        // what survives is the EDGES of the voxels behind the glass. It reads as seeing voxel
        // boundaries instead of voxels -- a wireframe of the geometry -- and it looks far more like
        // an LOD or descent fault than like a normal that was never written.
        //
        // Repaired HERE, in local space, because that is the last point at which the ray, the box and
        // the normal are all in the same frame: the world lift-back below rotates the normal, and the
        // box position and size are still the march's own integers until then. Costs one comparison
        // per hit and fires on almost none of them.
        if (tree64Intersect.foundBox.size > 0.0 && tree64Intersect.rayT >= 0.0 &&
            dot(tree64Intersect.normal, tree64Intersect.normal) < 0.5) {
            tree64Intersect.normal = pjvBoxEntryNormal(transformedRay,
                                                       tree64Intersect.foundBox.position,
                                                       tree64Intersect.foundBox.size);
        }

        // ---- A REAL HIT IS NEVER AT DISTANCE ZERO, BECAUSE ZERO MEANS "MISS" DOWNSTREAM --------
        //
        // Every renderer in the tree tests `hit.rayT <= 0.0` as part of its miss test -- fifteen-odd
        // shaders across six examples, and they are right to, because until now a rayT of zero only
        // ever arrived on a hit that was ALSO degenerate: the ray origin was inside the volume, the
        // entry helper clamped its distance to zero, and the normal was the (0,0,0) seed. One test
        // for one condition.
        //
        // The repair above splits those apart. The normal is now always usable, so a rayT of zero no
        // longer means "degenerate" -- it means the surface is exactly AT the ray origin, which is a
        // perfectly good answer that every one of those renderers still throws away as background.
        //
        // REFRACTION MAKES IT THE COMMON CASE. A bent ray restarts a quarter-voxel past the interface,
        // and the geometry behind glass is usually right there -- a frame behind a pane, a riverbed
        // under water -- so the new origin lands inside it and the hit comes back at zero. Confirmed
        // by instrumenting the miss test to colour its three rejection reasons separately: every
        // artifact pixel was the `rayT <= 0` branch.
        //
        // Fixed HERE rather than in the fifteen call sites, and not only to avoid editing them all: a
        // renderer written next week would inherit the same bug, and the convention it depends on --
        // "a hit that is really there has a positive distance" -- is the traversal's to guarantee.
        //
        // The cost of the guarantee is that a surface at the origin is reported a thousandth of a
        // voxel in front of itself. Nothing downstream can resolve that, and it buys back a whole
        // class of geometry that was being silently discarded.
        if (tree64Intersect.foundBox.size > 0.0 && tree64Intersect.rayT >= 0.0 &&
            tree64Intersect.rayT < tree64Intersect.foundBox.size * 1e-3) {
            tree64Intersect.rayT = tree64Intersect.foundBox.size * 1e-3;
        }
    }

    // The march works in tree space and returns a LEAF-RELATIVE material offset; the two values that
    // turn it into an absolute lookup live in the header, which is fetched here and not down there.
    // Same division of labour as the position and normal below, which the march also returns in local
    // terms for this function to lift into world space.
    //
    // The sentinel is preserved rather than offset -- it is not an index and adding a base to it would
    // turn "descend for this" into a wild read.
    tree64Intersect.paletteOffset = header.paletteOffset;
    // Neither the sentinel nor a direct palette slot is an index into the materialIDs array, so
    // neither takes the base. Offsetting the sentinel would turn "descend for this" into a wild read;
    // offsetting a direct slot would point it at an unrelated palette entry.
    if (tree64Intersect.materialListIndex != MATERIAL_INDEX_NEEDS_DESCENT &&
        (tree64Intersect.materialListIndex & PJV_MATERIAL_DIRECT_SLOT) == 0u) {
        tree64Intersect.materialListIndex += header.materialIDStartIndex;
    }
    // size should be 1 for full res, 2 for half res, 4 for quarter res etc. So we multiply the size by what the voxel scale actually is.
    tree64Intersect.foundBox.size *= header.scale/tree64BoundingBox.size;
    vec3 normalizedBoxPosition = tree64Intersect.foundBox.position/tree64BoundingBox.size;
    // Back to world: apply uniform scale, rotate by R, then translate by P (inverse of the
    // world->local transform applied to the ray above).
    tree64Intersect.foundBox.position = R * (normalizedBoxPosition * header.scale) + P;
    // The DDA's hit normal is in the chunk's local voxel frame; rotate it back to world.
    tree64Intersect.normal = R * tree64Intersect.normal;
    // The march ran in voxel units (origin scaled by resolution/scale, direction kept
    // length-preserving), so t values convert back to world with the inverse uniform scale.
    // A miss's -1 stays negative. Rotation preserves distance, so it does not affect rayT.
    tree64Intersect.rayT *= header.scale/tree64BoundingBox.size;
    tree64Intersect.exitT *= header.scale/tree64BoundingBox.size;
    return tree64Intersect;
}

// Geometry-only form: nearest solid voxel in one chunk, with no transparency and no material
// resolved. This is what a caller reaching past raySceneIntersect into a single chunk almost always
// wants, and it is what this function meant before the peel could run inside the march.
SceneIntersectData castRayThroughTree64(Ray ray, RayQuery rayQuery, float tMin, uint headerIndex) {
    PeelAccum peel = pjvNoPeel();
    return castRayThroughTree64(ray, rayQuery, tMin, headerIndex, -1.0, 1e30, peel);
}
// The colour of one voxel, given the cell the march landed on **in the chunk's own voxel space**.
//
// This is the exact path, and the one a renderer should call. Its counterpart below takes the hit's
// world-space bounding box and reconstructs this coordinate from it -- rotate by R^-1, subtract the
// chunk's world position, divide by its scale, multiply by the resolution, truncate -- and that
// round trip does not survive float32. The forward direction had already added the world translation
// P, so undoing it costs low bits in proportion to |P|; and the truncation turns any error below an
// exact integer into a whole cell of drift. Every affected voxel is then shaded with a *neighbour's*
// colour: scattered among correct ones, stable for a given transform, absent at the origin, and
// invisible in the stored data because the stored data was right all along. Measured over a chunk's
// voxels: 6% wrong for a small translation, 10% for a rotation alone, 39% for both.
//
// The DDA holds the exact integer throughout, so the fix is to carry it rather than rebuild it --
// see SceneIntersectData::voxelCoord.
//
// The coordinate is clamped rather than trusted: a caller handing over a miss's zeroed struct should
// read a defined voxel rather than index the tree out of bounds.
//
// This returns the WHOLE material. fetchVoxelColorAtCoord below is the albedo-only wrapper, and it
// stays because a renderer that only wants a colour should not have to know what a VoxelMaterial is
// -- and because every existing call site across the examples asks for exactly that. Splitting the
// two costs nothing: the descent, the fetch and the decode are identical either way, and the unused
// fields fold away in the compiler.
VoxelMaterial fetchVoxelMaterialAtCoord(ivec3 hitVoxelCoord, uint headerIndex) {
    chunkHeader h = headers(int(headerIndex));
    uint res = h.resolution;

    ivec3 voxelPos = clamp(hitVoxelCoord, ivec3(0), ivec3(int(res) - 1));

    uint zOrder = calculateZOrderIndex(uint(voxelPos.x), uint(voxelPos.y), uint(voxelPos.z), res);

    uint nodeIdx = h.geometryStartIndex;
    uint treeLevels = uint(log2(float(res)) / 2.0);
    int level = int(treeLevels) - 1;
    uint stepSize = 1u << (6u * uint(level));

    for (; level >= 0; --level) {
        Tree64NodeData node = tree64(nodeIdx);
        if ((node.data3 & 1u) != 0u) {
            // Leaf: bit 1 = uniform flag, bits 2.. = material byte offset. See voxel.h for the
            // layout, and leafMaterialListOffset for the decode -- shared with the march's own hit
            // sites so the fast path and this fallback cannot disagree about which byte a voxel owns.
            //
            // The rank used to be open-coded here, and it was missing the `childPos == 0` guard that
            // calculateSiblingsBeforeThisZOrder carries: `0xFFFFFFFFu << 32u` is undefined in GLSL and
            // masks to a shift of zero in HLSL, which returns the whole mask's popcount where the
            // answer is zero. That mis-shaded the first-numbered voxel of every non-uniform leaf.
            // NOTE: `uniform` is a reserved storage qualifier in HLSL/GLSL — do not name it that.
            uint matID = materialID(h.materialIDStartIndex +
                                    leafMaterialListOffset(node, (zOrder / stepSize) & 63u)) +
                         h.paletteOffset;
            return decodeMaterial(materialPaletteTexel(matID));
        }
        uint childZOrder = (zOrder / stepSize) & 63u;
        uint siblingsBefore = calculateSiblingsBeforeThisZOrder(4, node.data1, node.data2, childZOrder);
        uint childIdx = nodeIdx + node.childPtr + siblingsBefore;
        nodeIdx = childIdx;
        stepSize >>= 6u;
    }

    // Fell out of the descent without finding a leaf: an empty voxel.
    return emptyVoxelMaterial();
}

// Albedo only, for the renderers that shade from a colour and nothing else.
vec3 fetchVoxelColorAtCoord(ivec3 hitVoxelCoord, uint headerIndex) {
    return fetchVoxelMaterialAtCoord(hitVoxelCoord, headerIndex).albedo;
}

// The material of a hit the march just returned. **This is what a renderer holding a
// SceneIntersectData should call.**
//
// Two texelFetches: the material byte, then the palette entry. Nothing else -- no header fetch, no
// tree descent, no popcount. The march resolved the leaf-relative offset from values it already had in
// registers at the moment it decided to stop, and castRayThroughTree64 added the chunk's base while it
// still had the header open. See SceneIntersectData::materialListIndex.
//
// What this replaces, per call, is 5 header texels plus one texel and one popcount per tree level
// (4 levels at resolution 256) to rediscover the leaf the march was standing in. The saving is largest
// exactly where it hurt most: the peel pays it per transparent layer, so a body of water goes from a
// full root descent per voxel crossed to two fetches per voxel crossed.
//
// The fallback is a genuine fallback, not a slow path taken by accident. Only a LOD-cutoff hit -- a
// coarsened interior node returned whole -- carries the sentinel, and only because such a hit has no
// single voxel whose material it could name. Every full-resolution hit takes the fast path, which is
// every hit in the scene editor's viewport and in its path tracer.
VoxelMaterial fetchVoxelMaterialFromHit(SceneIntersectData hit) {
    if (hit.materialListIndex == MATERIAL_INDEX_NEEDS_DESCENT) {
        return fetchVoxelMaterialAtCoord(hit.voxelCoord, hit.headerIndex);
    }
    // A direct slot names its palette entry outright and has no materialIDs byte to read. See
    // PJV_MATERIAL_DIRECT_SLOT; the palette base still applies, because the slot is component-local.
    uint matID = ((hit.materialListIndex & PJV_MATERIAL_DIRECT_SLOT) != 0u)
               ? (hit.materialListIndex & PJV_MATERIAL_SLOT_MASK)
               : materialID(hit.materialListIndex);
    return decodeMaterial(materialPaletteTexel(matID + hit.paletteOffset));
}

// Albedo only, matching fetchVoxelColorAtCoord's relationship to the function above it.
vec3 fetchVoxelColorFromHit(SceneIntersectData hit) {
    return fetchVoxelMaterialFromHit(hit).albedo;
}

// The lossy path, kept only because a dozen shaders across the other examples still call it.
//
// **Do not use it in new code, and prefer fetchVoxelColorAtCoord when porting one.** It recovers the
// voxel coordinate from the hit's world-space box, which is a float32 round trip through the chunk's
// translation and rotation: the recovered cell is off by one for a share of voxels that grows with
// how far the chunk sits from the origin and whether it is rotated, and each of those is shaded with
// a neighbour's colour. See fetchVoxelColorAtCoord above for the full account and the measurements.
//
// A caller with a SceneIntersectData already has the exact answer in its `voxelCoord` field; the
// port is to pass that instead of `foundBox`.
vec3 fetchVoxelColor(BoxAABB voxelBoundingBox, uint headerIndex) {
    chunkHeader h = headers(int(headerIndex));
    uint res = h.resolution;
    vec3 P = vec3(h.positionX, h.positionY, h.positionZ);
    mat3 Rinv = transpose(rotationFromQuat(h.rotation));

    vec3 zeroed = Rinv * (voxelBoundingBox.position - P);
    vec3 unitPos = clamp(zeroed / h.scale, 0.0, 1.0 - 1e-6);
    ivec3 voxelPos = ivec3(unitPos * float(res));

    return fetchVoxelColorAtCoord(voxelPos, headerIndex);
}

// ------------------------------------------------------------
// Top-level uniform-grid acceleration (see gpu_interface.cpp / compose_io.cpp).
// gridInfo texel0 = {gridCount, looseChunkCount, headerCount, 0}; each grid then occupies
// 3 texels: {origin.xyz, cellSize}, {dims.xyz, cellMapOffset}, {rotation quat xyzw}.
// ------------------------------------------------------------
struct Grid {
    vec3 origin;
    float cellSize;
    ivec3 dims;
    uint cellMapOffset;
    vec4 rotation;
};

uint sceneGridCount()  { return texelFetch(gridInfo, ivec2(0, 0), 0).r; }
uint sceneLooseCount() { return texelFetch(gridInfo, ivec2(0, 0), 0).g; }

Grid getGrid(int i) {
    int base = 1 + i * 3;
    uvec4 p0 = texelFetch(gridInfo, ivec2(base + 0, 0), 0);
    uvec4 p1 = texelFetch(gridInfo, ivec2(base + 1, 0), 0);
    uvec4 p2 = texelFetch(gridInfo, ivec2(base + 2, 0), 0);
    Grid g;
    g.origin = vec3(uintBitsToFloat(p0.r), uintBitsToFloat(p0.g), uintBitsToFloat(p0.b));
    g.cellSize = uintBitsToFloat(p0.a);
    g.dims = ivec3(int(p1.r), int(p1.g), int(p1.b));
    g.cellMapOffset = p1.a;
    g.rotation = vec4(uintBitsToFloat(p2.r), uintBitsToFloat(p2.g), uintBitsToFloat(p2.b), uintBitsToFloat(p2.a));
    return g;
}

// cellMap is flattened exactly like voxelTypeData (4 uints per RGBA32U texel).
uint cellMapValue(uint index) {
    ivec2 texSize = textureSize(cellMap, 0);
    int pixelIndex = int(index) / 4;
    int x = pixelIndex % texSize.x;
    int y = pixelIndex / texSize.x;
    int colorIndex = int(index) % 4;
    uvec4 pixel = texelFetch(cellMap, ivec2(x, y), 0);
    return pixel[colorIndex];
}

// looseList holds the header rows (chunk handles) of the loose chunks, flattened like cellMap.
// The shader iterates [0, looseCount) of these instead of a positional header prefix, so a loose
// chunk can be added/removed without any ordering constraint on the header texture.
uint looseListValue(uint index) {
    ivec2 texSize = textureSize(looseList, 0);
    int pixelIndex = int(index) / 4;
    int x = pixelIndex % texSize.x;
    int y = pixelIndex / texSize.x;
    int colorIndex = int(index) % 4;
    uvec4 pixel = texelFetch(looseList, ivec2(x, y), 0);
    return pixel[colorIndex];
}

// Walks one grid volume with a uniform-grid DDA, front-to-back, and returns the nearest hit
// closer than maxDistance (or a miss). The DDA runs in grid-local space (cells axis-aligned),
// but each occupied cell is marched with the original WORLD ray via castRayThroughTree64 —
// the chunk header already carries world placement + rotation. R^-1 is orthonormal, so the
// grid-local ray parameter t matches world rayT and is directly comparable to maxDistance.
SceneIntersectData marchGrid(Ray ray, RayQuery rayQuery, int gridIndex, float maxDistance, float tMin, float limitT, inout PeelAccum peel) {
    SceneIntersectData miss;
    miss.foundBox.size = -1;
    miss.voxelCoord = ivec3(0);
    miss.rayT = -1.0;
    miss.exitT = -1.0;
    miss.normal = vec3(0.0);
    miss.steps = 0;
    miss.materialListIndex = MATERIAL_INDEX_NEEDS_DESCENT;
    miss.paletteOffset = 0u;

    Grid g = getGrid(gridIndex);

    mat3 Rinv = transpose(rotationFromQuat(g.rotation));
    vec3 lo = Rinv * (ray.origin - g.origin);
    vec3 ld = Rinv * ray.direction;
    // Sign-preserving, for the same reason as castRayThroughTree64's clamp above: a bare +eps flips a
    // small negative component and walks the cell DDA the wrong way along that axis.
    float eps = 1e-6;
    if (abs(ld.x) < eps) ld.x = ld.x < 0.0 ? -eps : eps;
    if (abs(ld.y) < eps) ld.y = ld.y < 0.0 ? -eps : eps;
    if (abs(ld.z) < eps) ld.z = ld.z < 0.0 ? -eps : eps;
    vec3 invD = 1.0 / ld;

    // Slab test against the (possibly non-cubic) grid box [0, dims*cellSize].
    vec3 gridMax = vec3(g.dims) * g.cellSize;
    vec3 t0 = (vec3(0.0) - lo) * invD;
    vec3 t1 = (gridMax - lo) * invD;
    vec3 tsmall = min(t0, t1);
    vec3 tbig = max(t0, t1);
    float tEnter = max(max(tsmall.x, tsmall.y), tsmall.z);
    float tExit = min(min(tbig.x, tbig.y), tbig.z);
    if (tEnter > tExit || tExit < 0.0) return miss;
    // Start the cell walk at tMin when the caller has already consumed everything nearer, so a peel
    // does not re-walk the same cells on every pass. The grid's frame is rotation-only (no scale),
    // so its t and the caller's are the same units.
    float tStart = max(max(tEnter, 0.0), tMin);
    if (tStart > tExit) return miss;

    // Entry cell + DDA setup.
    vec3 p = lo + ld * tStart;
    vec3 cellF = clamp(floor(p / g.cellSize), vec3(0.0), vec3(g.dims) - 1.0);
    ivec3 cell = ivec3(cellF);
    ivec3 stepDir = ivec3(sign(ld));
    vec3 tDelta = abs(g.cellSize * invD);
    vec3 nextBoundary = (vec3(cell) + step(vec3(0.0), ld)) * g.cellSize;
    vec3 tMax = (nextBoundary - lo) * invD;

    float tCurrent = tStart;
    int maxSteps = g.dims.x + g.dims.y + g.dims.z + 3;
    for (int i = 0; i < maxSteps; i++) {
        // No cell entered beyond the closest hit so far can improve on it.
        if (tCurrent >= maxDistance) break;
        // The peel gave up (budget spent, or fully absorbed). Nothing further along this ray can
        // contribute, and marching the remaining cells would be a full descent each for an answer
        // already known.
        if (peel.stopped) break;

        if (cell.x >= 0 && cell.y >= 0 && cell.z >= 0 &&
            cell.x < g.dims.x && cell.y < g.dims.y && cell.z < g.dims.z) {
            uint lin = uint(cell.x + g.dims.x * (cell.y + g.dims.y * cell.z));
            uint chunkIdx = cellMapValue(g.cellMapOffset + lin);
            if (chunkIdx != 0xFFFFFFFFu) {
                SceneIntersectData hit = castRayThroughTree64(ray, rayQuery, tMin, int(chunkIdx), limitT, maxDistance, peel);
                if (hit.foundBox.size > 0 && hit.rayT >= 0.0 && hit.rayT < maxDistance) {
                    hit.headerIndex = chunkIdx;
                    // Front-to-back: the first occupied cell with a hit holds this grid's nearest hit.
                    return hit;
                }
            }
        }

        // Advance to the next cell along the single nearest axis boundary.
        if (tMax.x <= tMax.y && tMax.x <= tMax.z) { tCurrent = tMax.x; cell.x += stepDir.x; tMax.x += tDelta.x; }
        else if (tMax.y <= tMax.z)                { tCurrent = tMax.y; cell.y += stepDir.y; tMax.y += tDelta.y; }
        else                                      { tCurrent = tMax.z; cell.z += stepDir.z; tMax.z += tDelta.z; }

        if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
            cell.x >= g.dims.x || cell.y >= g.dims.y || cell.z >= g.dims.z) break;
    }
    return miss;
}

// Geometry-only form. See the note on castRayThroughTree64's.
SceneIntersectData marchGrid(Ray ray, RayQuery rayQuery, int gridIndex, float maxDistance, float tMin) {
    PeelAccum peel = pjvNoPeel();
    return marchGrid(ray, rayQuery, gridIndex, maxDistance, tMin, -1.0, peel);
}

// ---- The safe interval -------------------------------------------------------------------------
//
// How far the ray can be walked while only ONE component's bounding volume is live. Inside such an
// interval there is nothing for a nearer surface to arrive from, so a march may consume transparent
// voxels IN PLACE instead of handing each one back for a full scene restart. See PeelAccum.
//
// The two helpers below answer "where does this volume begin, along the ray" for the two kinds of
// component. Both work in a rotation-only frame, and rotation preserves distance, so both return
// WORLD units and are directly comparable -- which is the whole reason this is cheap enough to do on
// every query. A negative return means the ray misses the volume entirely.
//
// Entries are clamped to zero by the caller: a ray whose origin is already inside two volumes has no
// solo interval at all, and clamping is what makes that fall out as limit == 0 rather than as a
// negative interval that would look inviting.
float pjvLooseEntryT(Ray ray, uint headerIndex) {
    chunkHeader h = headers(int(headerIndex));
    if (h.scale <= 0.0) return -1.0;
    vec3 P = vec3(h.positionX, h.positionY, h.positionZ);
    mat3 Rinv = transpose(rotationFromQuat(h.rotation));
    Ray localRay;
    localRay.origin = Rinv * (ray.origin - P);
    localRay.direction = Rinv * ray.direction;
    BoxAABB box;
    box.position = vec3(0.0);
    box.size = h.scale;
    return getRayBoxEntryDistance(localRay, box);
}

float pjvGridEntryT(Ray ray, int gridIndex) {
    Grid g = getGrid(gridIndex);
    mat3 Rinv = transpose(rotationFromQuat(g.rotation));
    vec3 lo = Rinv * (ray.origin - g.origin);
    vec3 ld = Rinv * ray.direction;
    float eps = 1e-6;
    if (abs(ld.x) < eps) ld.x = ld.x < 0.0 ? -eps : eps;
    if (abs(ld.y) < eps) ld.y = ld.y < 0.0 ? -eps : eps;
    if (abs(ld.z) < eps) ld.z = ld.z < 0.0 ? -eps : eps;
    vec3 invD = 1.0 / ld;
    vec3 gridMax = vec3(g.dims) * g.cellSize;
    vec3 t0 = (vec3(0.0) - lo) * invD;
    vec3 t1 = (gridMax - lo) * invD;
    float tEnter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
    float tExit  = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    if (tEnter > tExit || tExit < 0.0) return -1.0;
    return tEnter;
}

SceneIntersectData raySceneIntersectFrom(Ray ray, RayQuery rayQuery, float tMin, inout PeelAccum peel) {
    float closestDistance = 100000000;
    SceneIntersectData sceneIntersect;
    sceneIntersect.foundBox.size = -1;
    sceneIntersect.voxelCoord = ivec3(0);
    sceneIntersect.steps = 0;
    sceneIntersect.rayT = -1.0;
    sceneIntersect.exitT = -1.0;
    sceneIntersect.normal = vec3(0.0);
    sceneIntersect.materialListIndex = MATERIAL_INDEX_NEEDS_DESCENT;
    sceneIntersect.paletteOffset = 0u;

    int looseCount = int(sceneLooseCount());
    int gridCount = int(sceneGridCount());

    // ---- Pass 0: find the solo interval ------------------------------------------------------
    // The component whose bounding volume the ray enters FIRST is the only one live between its own
    // entry and the entry of the next component, whichever that is. So the nearest entry identifies
    // the one component allowed to peel in place, and the second-nearest is how far it may go.
    //
    // Only that component gets an interval. Every other one is handed -1 and behaves exactly as this
    // file always did, restarting the query per layer -- which is the correct answer whenever
    // ordering is genuinely in question, and is still what happens for interpenetrating geometry.
    //
    // Cost is one slab test per component, against a whole march saved per transparent layer. It is
    // skipped outright when the caller is not peeling.
    float soloLimit = -1.0;
    int   soloKind = -1;    // 0 = loose, 1 = grid
    int   soloIndex = -1;
    if (peel.active) {
        float bestEntry = 1e30;
        float secondEntry = 1e30;
        for (int i = 0; i < looseCount; i++) {
            float e = pjvLooseEntryT(ray, looseListValue(uint(i)));
            if (e < 0.0) continue;
            e = max(e, 0.0);
            if (e < bestEntry) { secondEntry = bestEntry; bestEntry = e; soloKind = 0; soloIndex = i; }
            else if (e < secondEntry) { secondEntry = e; }
        }
        for (int gi = 0; gi < gridCount; gi++) {
            float e = pjvGridEntryT(ray, gi);
            if (e < 0.0) continue;
            e = max(e, 0.0);
            if (e < bestEntry) { secondEntry = bestEntry; bestEntry = e; soloKind = 1; soloIndex = gi; }
            else if (e < secondEntry) { secondEntry = e; }
        }
        // A lone component owns the whole ray; two sharing an entry own none of it.
        soloLimit = min(secondEntry, rayQuery.maxDistance);
        if (soloLimit <= bestEntry) { soloKind = -1; soloIndex = -1; soloLimit = -1.0; }
    }

    // The winner's material, saved as it wins. Every march writes peel.hitMaterial, and the march
    // that wrote it LAST is not necessarily the one that won on distance -- a far chunk marched after
    // a near one would otherwise hand the near one its own material. This is the whole of the
    // save/restore that a file-scope static would need eleven copies of.
    VoxelMaterial winnerMaterial = emptyVoxelMaterial();
    bool winnerMaterialValid = false;

    // Loose (transform-placed) chunks: brute-force over the explicit loose-handle list (not a
    // positional prefix), so add/remove needs no header reordering. Broadphase in each chunk's
    // local frame so rotated boxes are tested correctly.
    for(int i = 0; i < looseCount; i++){
        if (peel.stopped) break;
        uint headerIndex = looseListValue(uint(i));
        chunkHeader h = headers(int(headerIndex));
        // A freed/dead slot carries a degenerate header (scale <= 0); skip it.
        if(h.scale <= 0.0) continue;
        vec3 P = vec3(h.positionX, h.positionY, h.positionZ);
        mat3 Rinv = transpose(rotationFromQuat(h.rotation));
        Ray localRay;
        localRay.origin = Rinv * (ray.origin - P);
        localRay.direction = Rinv * ray.direction;
        BoxAABB tree64BoundingBox;
        tree64BoundingBox.position = vec3(0.0);
        tree64BoundingBox.size = h.scale;
        float tBB = getRayBoxEntryDistance(localRay, tree64BoundingBox);
        if(tBB < 0 || tBB >= closestDistance) continue;

        float limitT = (soloKind == 0 && soloIndex == i) ? soloLimit : -1.0;
        SceneIntersectData tree64Intersect = castRayThroughTree64(ray, rayQuery, tMin, int(headerIndex), limitT, closestDistance, peel);
        // Rank by the march's own rayT (re-intersecting foundBox analytically flips
        // boundary-exact hits into misses by ULPs). rayT == 0 is a real hit.
        if(tree64Intersect.foundBox.size > 0 && tree64Intersect.rayT >= 0 && tree64Intersect.rayT < closestDistance){
            sceneIntersect = tree64Intersect;
            sceneIntersect.headerIndex = headerIndex;
            closestDistance = tree64Intersect.rayT;
            winnerMaterial = peel.hitMaterial;
            winnerMaterialValid = peel.hitMaterialValid;
        }
    }

    // Grid volumes: top-level uniform-grid DDA, pruned by the closest loose hit so far.
    for(int gi = 0; gi < gridCount; gi++){
        if (peel.stopped) break;
        float limitT = (soloKind == 1 && soloIndex == gi) ? soloLimit : -1.0;
        SceneIntersectData gh = marchGrid(ray, rayQuery, gi, closestDistance, tMin, limitT, peel);
        if(gh.foundBox.size > 0 && gh.rayT >= 0 && gh.rayT < closestDistance){
            sceneIntersect = gh;
            closestDistance = gh.rayT;
            winnerMaterial = peel.hitMaterial;
            winnerMaterialValid = peel.hitMaterialValid;
        }
    }

    peel.hitMaterial = winnerMaterial;
    peel.hitMaterialValid = winnerMaterialValid;
    return sceneIntersect;
}

// Geometry-only form: the nearest solid voxel beyond `tMin`, with no transparency and no material
// resolved. raySceneIntersect below is what a renderer should call; this stays public because a
// caller that genuinely wants one unfiltered nearest-hit -- a picking ray, a distance probe -- should
// not have to construct a peel to ask for it.
SceneIntersectData raySceneIntersectFrom(Ray ray, RayQuery rayQuery, float tMin) {
    PeelAccum peel = pjvNoPeel();
    return raySceneIntersectFrom(ray, rayQuery, tMin, peel);
}
// =============================================================================
// raySceneIntersect -- the one scene query
// =============================================================================
//
// Everything a caller wants out of a traversal is described by the RayQuery it passes in, and
// everything the traversal found comes back in the SceneHit. There is no second entry point for
// transparency and no third for animation: those are flags, because they are properties of the
// question rather than different questions.
//
// ---- What the loop below is for, and why it is now the exception rather than the rule ----
//
// Seeing through a voxel is a TRAVERSAL change, not a shading one: every march in this file stops at
// the first solid voxel it meets. The march can now keep going by itself, but only within an interval
// where no other component's bounding volume is live (see PeelAccum and the pass-0 note in
// raySceneIntersectFrom). Outside such an interval, which surface is actually in front is decided
// HERE and nowhere below -- raySceneIntersectFrom brute-forces every loose chunk tracking
// closestDistance and only then marches the grids pruned by it -- so the only correct way to get past
// a transparent voxel is to re-ask "what is nearest now" from just beyond it.
//
// So this loop still exists and is still correct for interleaved geometry. What changed is how often
// it runs: a deep medium inside one component (a flame, a body of water, a thick pane) is consumed by
// the march in a single traversal, where it used to cost one full scene re-traversal per voxel of
// depth.
//
// ---- Resuming without moving the ray ----
//
// Each pass asks for the nearest hit BEYOND a distance (`tMin`) rather than restarting the ray past
// the last one. That distinction is not stylistic; moving the origin is broken here.
//
// An origin nudged past a hit lands either inside the next voxel or exactly on a boundary, and this
// marcher is degenerate at both. Inside a voxel, the descent reaches it immediately and returns a real
// hit carrying rayT == 0 and a zero normal (SceneIntersectData::normal says so outright), which every
// renderer's miss test reads as a miss -- so a transparent surface showed the SKY behind it, not the
// geometry. Exactly on a boundary, the zero-measure guard in the DDA step skips the cell's validity
// check by design. Scaling the nudge by the voxel does not help: both failures are about where the
// origin lands, not how far it moved.
//
// Keeping the origin fixed sidesteps all of it. Every entry computation stays in the regime it was
// written for, rayT and the normal stay meaningful, and there is no epsilon to tune.
//
// ---- Refraction: the segment loop around the peel ----
//
// A refracted ray CHANGES DIRECTION, so it cannot be a continuation of the same march -- it is a new
// ray. That is the whole reason it lives out here as a loop AROUND the peel rather than inside it,
// and the reason it costs nothing when it is not asked for: with maxRefractionSegments at 0 the outer
// loop runs exactly once and the arithmetic below is bit-identical to what it was.
//
// Three things it has to get right, each of which is a way this fails silently:
//
//   * BEND AT AN INTERFACE, NOT AT EVERY VOXEL. The peel already distinguishes the two (a hit
//     continues the previous body only if it is the same material AND begins where the previous one
//     ended) and now reports which it saw. Bending at every interior voxel of a pane would make a
//     flat sheet of glass a stack of lenses.
//   * THE NEW ORIGIN MUST NOT LAND INSIDE THE VOXEL IT JUST LEFT. This marcher is degenerate at an
//     origin inside a voxel -- the descent reaches it immediately and returns rayT == 0 with a zero
//     normal, which every renderer's miss test reads as sky. The peel avoids this by never moving the
//     origin at all, and refraction cannot: the direction changed, so a tMin along the old direction
//     means nothing. So the new ray starts at the interface voxel's EXIT and is pushed a further
//     fraction of a voxel along its new direction, which clears the cell in every case a tMin could
//     have handled.
//   * TOTAL INTERNAL REFLECTION IS A REFLECTION, NOT A MISS. GLSL's refract() returns the zero vector
//     there, and continuing with it would march a ray with no direction -- every t is NaN and the
//     result is whatever the first comparison against NaN happens to do. Reflected instead, which is
//     both correct and what makes water read as water at grazing angles.
SceneHit raySceneIntersect(Ray ray, RayQuery rayQueryIn) {
    // A query that did not come from a constructor carries garbage in the fields added alongside
    // `magic`. Clamp them and say so, rather than acting on them -- acting on them is how a bad
    // distance floor once rejected every hit in the scene with nothing on screen to attribute it to.
    bool queryOK = pjvQueryIsValid(rayQueryIn);
    RayQuery q = pjvValidateQuery(rayQueryIn);

    SceneHit result = pjvEmptySceneHit();

    PeelAccum peel = pjvNoPeel();
    peel.active   = (q.flags & PJV_Q_TRANSPARENCY) != 0u;
    // A shadow ray wants the EXPECTED attenuation. Multiplying by `transparency` once per interface is
    // exactly that, with none of the variance a random choice would add to every shadow in the image.
    peel.analytic = (q.flags & PJV_Q_OCCLUSION_ONLY) != 0u;
    peel.maxLayers = q.maxTransparentLayers;
    peel.seed = q.seed;
    peel.minTransmittance = q.minTransmittance;
    // Only when asked. See PJV_Q_WANT_MATERIAL for why this is opt-in rather than implied.
    peel.wantMaterial = (q.flags & PJV_Q_WANT_MATERIAL) != 0u;
    // Tells the in-march peel to hand a refracting interface back rather than consume it, because
    // this loop is the only place the ray can be replaced. See pjvPeelConsume.
    peel.refracting = (q.flags & PJV_Q_REFRACTION) != 0u && q.maxRefractionSegments > 0u;

    float tMin = 0.0;
    // Identity of the voxel the previous pass consumed, so a pass that fails to move on is caught
    // exactly rather than being absorbed into the result. See peelNextTMin.
    uint  lastHeader = 0xFFFFFFFFu;
    ivec3 lastVoxel = ivec3(0x7FFFFFFF);
    uint  stop = PJV_STOP_ITERATIONS;

    // The ray the CURRENT segment marches. Advances only when something bends it.
    Ray  cur = ray;
    // Distance already travelled along the segments behind this one, so pathLength can be reported.
    float travelled = 0.0;
    uint  segments = 0u;
    bool  wantRefraction = (q.flags & PJV_Q_REFRACTION) != 0u && q.maxRefractionSegments > 0u;

    // ---- A BEND IS ONE PASS OF THE LOOP BELOW, NOT A LOOP AROUND IT --------------------------
    //
    // The obvious shape is a segment loop wrapping the peel: march until something bends the ray,
    // then start over with the new one. It works and it was written that way first, and it cost
    // 30% OF THE FRAME IN A SCENE WITH NOTHING REFRACTIVE IN IT -- 6.70 ms against 4.69 ms at the
    // default framing. Two ways: unrolled to the ceiling it is four inlined copies of the entire
    // peel, all but one of them dead; bounded at runtime to take the unroll away it is still a loop
    // carried by every ray, and that alone measured ~0.4 ms per allowed segment.
    //
    // A bend fits inside the peel loop instead, because it is the same kind of event the peel already
    // handles: consume something, move the resume point, ask again. The only difference is that the
    // resume point is a new origin and direction rather than a distance along the old ones. So there
    // is no second loop, and a caller that never sets PJV_Q_REFRACTION reaches a branch that is never
    // taken rather than a structure it has to carry.
    //
    // The cost is that a bend spends one of the peel's iterations. That is the right budget to spend
    // it from: both are "how many times may this ray be asked again", and a ray deep enough in glass
    // to exhaust sixteen of them between them has run out either way.

    // ---- THE CEILING STAYS A COMPILE-TIME CONSTANT, AND THAT IS DELIBERATE --------------------
    //
    // Bounding this loop by `q.maxTransparentLayers + 1` instead looks strictly better -- a caller
    // asking for four layers has no use for sixteen passes -- and it is NOT, twice over.
    //
    // It buys nothing, because the budget is already enforced inside: pjvPeelConsume returns ABORT
    // with PJV_STOP_LAYER_BUDGET the moment `maxLayers` is spent, and the break below takes it. The
    // runtime bound would only stop the loop one pass after the peel already had.
    //
    // And it costs 3.3 ms of a 15.7 ms frame on a canopy view, measured, because a constant trip
    // count is one the compiler will unroll and a runtime one is not. That is a 17% frame for a
    // redundant guard.
    //
    // It was tried for a different reason -- gbuffer.frag had just failed with "ID overflow. Try
    // running compact-ids", and taking the unroll away was the obvious lever. It did not help,
    // because spirv-opt inlines every function into main and the id cost is per CALL SITE, not per
    // unrolled iteration. What actually overflowed the space was the four forked prototype
    // traversals that example still included, each a full copy of the march. Worth recording so the
    // next id-space problem is diagnosed by counting call sites rather than by fighting unrolling.
    for (int iteration = 0; iteration < MAX_PEEL_ITERATIONS; iteration++) {
        SceneIntersectData hit = raySceneIntersectFrom(cur, q, tMin, peel);

        // The march gave up in place -- budget spent, or fully absorbed. Its own reason is the honest
        // one and it is already recorded.
        if (peel.stopped) { stop = peel.stopReason; break; }

        // The same voxel again: tMin failed to advance past it. Stop rather than attenuate it a second
        // time. Belt and braces behind peelNextTMin's ULP floor -- cheap, and it turns any residual
        // non-advance into one lost layer instead of a black or invisible voxel.
        if (hit.headerIndex == lastHeader && all(equal(hit.voxelCoord, lastVoxel))) {
            stop = PJV_STOP_TMIN_STALL; break;
        }

        if (hit.foundBox.size < 0.0 || hit.rayT < 0.0) {
            // Nothing solid left along the ray. Whatever the caller does on a miss -- sky, usually --
            // is still filtered by the layers already crossed.
            stop = PJV_STOP_NO_GEOMETRY; break;
        }
        if (hit.rayT >= q.maxDistance) {
            // Everything found is past what the caller cares about. Same as a miss.
            stop = PJV_STOP_NO_GEOMETRY; break;
        }

        // The traversal fetched this while deciding, unless it had no reason to. A query that is
        // neither peeling nor shading needs no material at all -- pjvPeelConsume returns STOP_HERE
        // without reading it -- so fetching one here would be pure cost on every shadow ray.
        VoxelMaterial m = emptyVoxelMaterial();
        if (peel.hitMaterialValid) {
            m = peel.hitMaterial;
        } else if (peel.active || peel.wantMaterial) {
            m = fetchVoxelMaterialFromHit(hit);
        }

        // One rule for every voxel. Opaque stops the ray; transparent is crossed. Nothing consults a
        // flag to decide whether a particular KIND of voxel occludes -- a fire parcel, a pane of
        // glass and a leaf all answer with the transparency their palette entry actually carries.
        // Cleared around this one call. `peel.refracting` means "hand a refracting interface back to
        // the loop that can bend" -- and this IS that loop, so here the layer is consumed normally.
        // Left set, pjvPeelConsume would hand the same voxel back to the caller that just received it.
        bool callerBends = peel.refracting;
        peel.refracting = false;
        int verdict = pjvPeelConsume(peel, m, hit.rayT, hit.exitT, hit.foundBox.size, 1e30);
        peel.refracting = callerBends;

        if (verdict == PJV_PEEL_STOP_HERE) {
            result.hit = hit;
            result.material = m;
            result.materialValid = peel.hitMaterialValid;
            stop = PJV_STOP_OPAQUE;
            break;
        }
        if (verdict == PJV_PEEL_ABORT) { stop = peel.stopReason; break; }

        // ---- The layer just crossed bends light ----------------------------------------------
        //
        // Only at an interface, only when the caller asked, and only while segments remain. An IOR of
        // exactly 1 is "no refraction", which is what raw 0 in the palette byte decodes to -- so a
        // palette written before this existed cannot bend anything.
        // Recorded whether or not the bend fires -- the declining case is the one worth seeing.
        result.diagBendIor = m.ior;
        result.diagBendFlags = 1u
                             | (peel.crossedInterface ? 2u : 0u)
                             | (segments < q.maxRefractionSegments ? 4u : 0u)
                             | (abs(m.ior - 1.0) > 1.0 / 256.0 ? 8u : 0u);

        // ---- RUNNING OUT OF SEGMENTS MUST NOT PUNCH A HOLE THROUGH THE GLASS -------------------
        //
        // A refracting interface the budget cannot pay for used to be crossed STRAIGHT: the ray
        // carried on undeviated as though the surface were not there. That is the worst available
        // failure, because the pixel next to it did bend -- so the two land on completely different
        // geometry and the boundary between them is a hard edge. It is what the diagnostic showed as
        // yellow along the edges of a body, which is exactly where a ray meets the most surfaces and
        // is first to run out.
        //
        // Stopping AT the interface instead is a much softer failure. The surface is returned as the
        // hit, so what you see is the glass itself -- shaded, tinted by its own alpha -- rather than
        // whatever happened to be behind it along an unbent ray. Neighbouring pixels that still had
        // budget show what is behind; these show the surface. That is a difference in shading rather
        // than in geometry, and it degrades instead of tearing.
        //
        // PJV_STOP_SEGMENTS is what this reason was defined for, and a caller can now see it.
        bool bendableInterface = wantRefraction && peel.crossedInterface &&
                                 abs(m.ior - 1.0) > 1.0 / 256.0;
        if (bendableInterface && segments >= q.maxRefractionSegments) {
            result.hit = hit;
            result.material = m;
            result.materialValid = peel.hitMaterialValid;
            stop = PJV_STOP_SEGMENTS;
            break;
        }

        if (bendableInterface) {
            // Which side are we on? The march's normal points out of the face the ray ENTERED, so a
            // ray going into a body has it opposing the direction. Air outside, the material inside:
            // this does not carry a medium stack, so a body inside a body refracts as if the outer
            // one were air. That is a real limit and a cheap one to live with -- nesting two
            // different IORs is rare, and the alternative is per-ray state proportional to depth.
            vec3 nrm = hit.normal;
            float cosi = dot(cur.direction, nrm);
            float eta;
            if (cosi < 0.0) {
                eta = 1.0 / max(m.ior, 1e-3);       // entering
            } else {
                eta = max(m.ior, 1e-3);             // leaving
                nrm = -nrm;
            }
            vec3 bent = refract(cur.direction, nrm, eta);
            // TIR. refract() hands back the zero vector, which would march a directionless ray.
            if (dot(bent, bent) < 1e-8) bent = reflect(cur.direction, nrm);
            bent = normalize(bent);

            // ---- THE BEND HAPPENS AT THE ENTRY FACE, WHICH IS WHERE THE NORMAL CAME FROM ----
            //
            // `hit.normal` is the face the ray ENTERED this voxel through, and `eta` was chosen from
            // its sign. Applying that bend at the voxel's EXIT -- which is what this did -- pairs a
            // normal taken at one surface with a position taken at another, and the ray then travels
            // the WHOLE CHORD of the interface voxel undeviated before turning.
            //
            // That error is not constant, and that is what made it visible. The chord depends on
            // where the ray crosses the voxel: near a corner it is a sliver, through the middle it is
            // up to sqrt(3) of a voxel. So the refracted image is displaced by chord x bend-angle,
            // and the displacement CHANGES FROM VOXEL TO VOXEL -- geometry seen through the surface
            // lands slightly differently either side of every cell boundary. Small, structured, and
            // aligned to the lattice, which reads as node boundaries rather than as a maths error.
            //
            // Entering at the entry point makes the normal and the position describe the same
            // surface, and the ray then travels through the medium along the direction it actually
            // refracted to.
            //
            // The nudge along `bent` keeps the origin off the face itself. It lands INSIDE the
            // interface voxel, which is correct and costs nothing: the march returns that voxel
            // again, `sameMedium` makes it interior rather than a second interface (see the note on
            // previousExit below), so it contributes its absorption and not another alpha -- which is
            // exactly what passing through the body of the glass should do.
            // The nudge is a PRECISION epsilon, not a geometric offset, so it is kept as small as
            // float32 allows rather than as large as the box. A quarter of the hit's box -- which is
            // what this was -- starts the refracted ray a quarter of a voxel into the medium, and
            // `foundBox.size` is 4x or 16x larger on a coarsened node, so the offset JUMPED at LOD
            // boundaries. Same family of defect as the exit-point bend above: a quantity that should
            // be continuous taking its value from whatever node the traversal happened to stop on.
            //
            // Two terms because the two failure modes pull opposite ways: a fraction of the box keeps
            // it well inside one voxel at any scene scale, and the distance-relative floor keeps it
            // above a float32 ULP once the ray is thousands of units from the origin -- where a fixed
            // small epsilon rounds away to nothing and the origin lands exactly on the face.
            float push = max(hit.foundBox.size * 0.01, max(abs(hit.rayT), 1.0) * 1e-5);
            vec3 entryPoint = cur.origin + cur.direction * hit.rayT;
            travelled += hit.rayT;
            cur.origin = entryPoint + bent * push;
            cur.direction = bent;
            // A new ray means a new march from its own origin, so the resume floor and the
            // stall guard both start over. The peel's accumulated transmittance does NOT --
            // everything crossed so far is still in front of whatever this segment finds.
            tMin = 0.0;
            lastHeader = 0xFFFFFFFFu;
            lastVoxel = ivec3(0x7FFFFFFF);
            // The interface anchor has to be re-expressed on the NEW ray. `previousExit` is a
            // distance, and the peel's interface test is `rayT > previousExit + half a cell` -- so
            // leaving it holding a t measured along the ray we just stopped using compares two
            // different rays' distances. A large stale value swallows the next interface (charged as
            // interior, so a pane crossed after a bend costs no alpha); a small one splits a body in
            // half and charges its interior twice.
            //
            // ---- THE CONTIGUITY TEST DOES NOT SURVIVE A BEND, SO IT IS SWITCHED OFF ------------
            //
            // `previousExit` exists so the peel can tell "another voxel of the body I am already in"
            // from "a new surface of the same material further along", and it does it by DISTANCE:
            // a hit more than half a cell past where the last one ended is a new interface. That is
            // sound on a straight ray and meaningless across a bend, because the bend replaced the
            // ray the distance was measured on.
            //
            // Setting it to zero -- "the body ended where this ray begins" -- is exactly true and
            // still wrong, and the way it fails is the interesting part. After a bend the origin sits
            // a quarter voxel past the interface, so the NEXT voxel of the same body comes back at a
            // rayT somewhere around half a cell -- right on the threshold. Whether it lands above or
            // below varies with where in the voxel the ray crossed, so ADJACENT PIXELS DISAGREE ABOUT
            // WHETHER THEY ARE STILL INSIDE THE GLASS. The ones that say no bend a second time, and
            // two populations of rays with different bend counts leave a hard edge between them --
            // aligned to voxel boundaries, because that is what decides which side of the threshold
            // a ray falls on.
            //
            // After a bend we are unambiguously INSIDE the medium we just entered; there is no
            // distance test that needs to establish it. So contiguity is decided by material alone
            // until the ray meets something different, which is what a large value encodes.
            //
            // What that gets wrong, stated plainly: a ray that leaves this body into air and later
            // meets the SAME material again reads as still-inside and is not charged that surface's
            // alpha. One uncharged interface, only after a bend, against a hard edge through every
            // refracted surface -- and `sameMedium` still separates different materials, which is
            // the case that actually carries the image.
            peel.previousExit = 1e30;
            segments++;
            continue;
        }

        lastHeader = hit.headerIndex;
        lastVoxel = hit.voxelCoord;
        tMin = peelNextTMin(hit, peelSegmentLength(cur, hit));
    }

    // Spent every segment the caller granted and still met refractive interfaces afterwards: the ray
    // carried on STRAIGHT through them rather than stopping, which is the graceful degradation, and
    // the reason says so. Distinguished from PJV_STOP_ITERATIONS because they look identical on
    // screen -- glass with the wrong thing behind it -- and only one of them is fixed by more layers.
    if (segments >= q.maxRefractionSegments && segments > 0u && stop == PJV_STOP_ITERATIONS) {
        stop = PJV_STOP_SEGMENTS;
    }

    result.transmittance = peel.transmittance;
    result.emission = peel.emission;
    result.layers = peel.layers;
    result.segments = segments;
    result.finalRay = cur;
    result.pathLength = (result.hit.rayT >= 0.0) ? travelled + result.hit.rayT : -1.0;
    result.stopReason = queryOK ? stop : PJV_STOP_BAD_QUERY;
    return result;
}

// How much light survives the trip from `ray.origin` to `rayQuery.maxDistance` along `ray`.
//
// The shadow-ray form, and the reason transparency is visible at all rather than merely stored: a
// visibility test can only answer "lit" or "not", so a glass pane casts a fully black shadow no matter
// what its material says. This returns the tint instead, which is what puts coloured light on the
// floor under stained glass.
//
// Running out of budget returns the transmittance accumulated so far, NOT black. Black is the
// conservative answer when a ray might still be blocked, and it was the original choice here on the
// grounds that an over-dark shadow is less noticeable than light leaking through solid geometry. That
// reasoning does not survive a transparent VOLUME: every shading point inside one needs more layers to
// reach the light than the budget allows, so black turned the whole interior into a lattice of unlit
// faces. Between the two errors, slightly-too-much light through deep glass is far less visible than a
// black interior, and it degrades smoothly instead of per-voxel.
vec3 raySceneTransmittance(Ray ray, RayQuery rayQuery) {
    SceneHit r = raySceneIntersect(ray, rayQuery);
    // Genuinely blocked: an opaque surface between here and the light.
    if (r.stopReason == PJV_STOP_OPAQUE) return vec3(0.0);
    return r.transmittance;
}
