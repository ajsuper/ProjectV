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

// NOTE ON DEFAULTS: these initializers are NOT dependable. Every renderer builds a RayQuery by
// declaring one and assigning only the fields it cares about, and the compiled result does not carry
// the values written here -- an unassigned field is garbage. `doTransparency` has been in this struct
// unset and unread by anything for as long as it has existed, which is why that was never noticed.
//
// So nothing that GATES A HIT may live in here. The distance floor the peeling traversals need is an
// explicit function parameter (see raySceneIntersectFrom) for exactly that reason: putting it here
// first, and letting it default to 0, produced a garbage floor in all ~20 existing shaders and
// rejected every hit in the scene -- a completely empty render.
struct RayQuery {
    bool doTransparency = true;   // Vestigial: nothing sets it and nothing reads it.
    uint startLOD = 0;
    uint finishLOD = 0;
    uint distanceToFinishLOD = 0; // Measured in voxels.
    uint maxRaySteps = 100;
};

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
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

struct GPUChunkHeader {
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
    uint padding[1];
};

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
    float transparency;   // Stored and carried. Nothing reads it yet -- see the note below.
    float ior;            // 1.0 = no refraction.
    float transmission;
    uint  flags;
};

vec3 unpackPaletteRGB10(uint packed) {
    return vec3(float((packed >> 20u) & 0x3FFu),
                float((packed >> 10u) & 0x3FFu),
                float( packed         & 0x3FFu)) / 1023.0;
}

// NOTE ON TRANSPARENCY: `transparency`, `ior` and `transmission` are decoded and carried here, but
// no renderer acts on them, because acting on them is not a shading change -- it is a traversal
// change. Every march in this file stops at the first solid voxel, so a see-through voxel would
// need the marcher to read the material mid-traversal and keep going, which is a material fetch per
// step instead of per hit. The storage is free; that is not. Kept decoded so the shading side is
// ready when the traversal side is.
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
    header.reserved0 = pixel4.g;
    header.reserved1 = pixel4.b;
    header.reserved2 = pixel4.a;

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
uint computeTargetLOD(float distanceInVoxels, RayQuery rayQuery) {
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

SceneIntersectData marchRayThroughTree64_DDA(Ray ray, RayQuery rayQuery, float tMin, BoxAABB boundingBox, uint tree64StartIndex, uint tree64EndIndex, uint tree64Resolution, uint chunkTraversalLOD) {
    SceneIntersectData returnData;
    returnData.rayT = -1.0;
    returnData.exitT = -1.0;
    returnData.normal = vec3(0.0);
    vec3 invRayDir = 1.0/ray.direction;
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
                        returnData.foundBox.position = traversalPosition;
                        returnData.voxelCoord = traversalPosition;
                        returnData.foundBox.size = stepSize;
                        returnData.steps = stepCount;
                        returnData.rayT = rayT;
                        returnData.exitT = cellExitDistance(ray, invRayDir, traversalPosition, stepSize);
                        returnData.normal = hitNormal;
                        // `data` IS the leaf node here -- the leaf flag is carried by the node whose
                        // 64 children are voxels, and the candidate we just validated against its
                        // masks is one of those voxels. So the material offset is available from the
                        // values already in registers, with no fetch and no second descent. See
                        // SceneIntersectData::materialListIndex.
                        returnData.materialListIndex = leafMaterialListOffset(
                            data, nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent);
                        return returnData;
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
                        returnData.foundBox.position = traversalPosition;
                        returnData.voxelCoord = traversalPosition;
                        returnData.foundBox.size = stepSize;
                        returnData.steps = stepCount;
                        returnData.rayT = rayT;
                        returnData.exitT = cellExitT;
                        returnData.normal = hitNormal;
                        // Same as the descent's leaf hit: `data` is the leaf, and moveZOrder has just
                        // put this voxel's index among its 64 children on the top of the stack. The
                        // z-order stays within the node by construction (MOVE_LUT is 6 bits wide), so
                        // it is the leaf-local index the material list is ranked by.
                        returnData.materialListIndex = leafMaterialListOffset(
                            data, nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent);
                        return returnData;
                    }
                    // At or before tMin: a surface the caller has already consumed. Keep stepping
                    // through this leaf rather than returning it. Deliberately does NOT break --
                    // breaking would leave the stepping loop and re-descend onto the same voxel.
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

//Cast the ray through the tree64 bounding box.
SceneIntersectData castRayThroughTree64(Ray ray, RayQuery rayQuery, float tMin, uint headerIndex) {
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

    IntersectionResult rootIntersect = getRayBoxEntry(transformedRay, tree64BoundingBox);

    SceneIntersectData tree64Intersect;
    tree64Intersect.foundBox.size = -1;
    tree64Intersect.voxelCoord = ivec3(0);
    tree64Intersect.rayT = -1.0;
    tree64Intersect.exitT = -1.0;
    tree64Intersect.normal = vec3(0.0);
    tree64Intersect.materialListIndex = MATERIAL_INDEX_NEEDS_DESCENT;
    tree64Intersect.paletteOffset = 0u;
    if(rootIntersect.distance >= 0){
        tree64Intersect = marchRayThroughTree64_DDA(transformedRay, rayQuery, localTMin, tree64BoundingBox, tree64StartIndex, tree64EndIndex, header.resolution, header.traversalLOD);
    }

    // The march works in tree space and returns a LEAF-RELATIVE material offset; the two values that
    // turn it into an absolute lookup live in the header, which is fetched here and not down there.
    // Same division of labour as the position and normal below, which the march also returns in local
    // terms for this function to lift into world space.
    //
    // The sentinel is preserved rather than offset -- it is not an index and adding a base to it would
    // turn "descend for this" into a wild read.
    tree64Intersect.paletteOffset = header.paletteOffset;
    if (tree64Intersect.materialListIndex != MATERIAL_INDEX_NEEDS_DESCENT) {
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
    uint matID = materialID(hit.materialListIndex) + hit.paletteOffset;
    return decodeMaterial(materialPaletteTexel(matID));
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
SceneIntersectData marchGrid(Ray ray, RayQuery rayQuery, int gridIndex, float maxDistance, float tMin) {
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

        if (cell.x >= 0 && cell.y >= 0 && cell.z >= 0 &&
            cell.x < g.dims.x && cell.y < g.dims.y && cell.z < g.dims.z) {
            uint lin = uint(cell.x + g.dims.x * (cell.y + g.dims.y * cell.z));
            uint chunkIdx = cellMapValue(g.cellMapOffset + lin);
            if (chunkIdx != 0xFFFFFFFFu) {
                SceneIntersectData hit = castRayThroughTree64(ray, rayQuery, tMin, int(chunkIdx));
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

SceneIntersectData raySceneIntersectFrom(Ray ray, RayQuery rayQuery, float tMin) {
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

    // Loose (transform-placed) chunks: brute-force over the explicit loose-handle list (not a
    // positional prefix), so add/remove needs no header reordering. Broadphase in each chunk's
    // local frame so rotated boxes are tested correctly.
    int looseCount = int(sceneLooseCount());
    for(int i = 0; i < looseCount; i++){
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

        SceneIntersectData tree64Intersect = castRayThroughTree64(ray, rayQuery, tMin, int(headerIndex));
        // Rank by the march's own rayT (re-intersecting foundBox analytically flips
        // boundary-exact hits into misses by ULPs). rayT == 0 is a real hit.
        if(tree64Intersect.foundBox.size > 0 && tree64Intersect.rayT >= 0 && tree64Intersect.rayT < closestDistance){
            sceneIntersect = tree64Intersect;
            sceneIntersect.headerIndex = headerIndex;
            closestDistance = tree64Intersect.rayT;
        }
    }

    // Grid volumes: top-level uniform-grid DDA, pruned by the closest loose hit so far.
    int gridCount = int(sceneGridCount());
    for(int gi = 0; gi < gridCount; gi++){
        SceneIntersectData gh = marchGrid(ray, rayQuery, gi, closestDistance, tMin);
        if(gh.foundBox.size > 0 && gh.rayT >= 0 && gh.rayT < closestDistance){
            sceneIntersect = gh;
            closestDistance = gh.rayT;
        }
    }
    return sceneIntersect;
}

// The nearest hit along the whole ray -- what every renderer calls. A floor of 0 is the value the
// march's comparisons are written to pass unconditionally, so this is bit for bit the traversal it
// has always been, and no caller has to know the floor exists.
SceneIntersectData raySceneIntersect(Ray ray, RayQuery rayQuery) {
    return raySceneIntersectFrom(ray, rayQuery, 0.0);
}

// =============================================================================
// Transparency
// =============================================================================
//
// Seeing through a voxel is a TRAVERSAL change, not a shading one: every march above stops at the
// first solid voxel it meets. These two functions add the "and keep going" case by repeatedly calling
// raySceneIntersect with the ray restarted just past each non-opaque voxel.
//
// ---- Why the loop is out here and not in the DDA ----
//
// The tempting version is to teach marchRayThroughTree64_DDA to skip transparent leaves and carry on
// stepping. That is cheaper and it is WRONG, because of where nearest-hit is decided. raySceneIntersect
// brute-forces every loose chunk tracking `closestDistance` and only then marches the grids pruned by
// it, so which surface is actually in front is known at THIS level and nowhere below it. A march that
// skipped transparent leaves inside one chunk would return that chunk's nearest opaque hit while a
// transparent voxel belonging to a different loose chunk sat in front of it -- two overlapping glass
// panes in different components would composite in the wrong order. Restarting the whole query is
// correct by construction: each pass re-answers "what is nearest now".
//
// The price is a full scene re-traversal per transparent layer -- loose broadphase over every loose
// chunk, grid slab tests, grid DDA, tree descent from the root. Thin geometry (a pane, a window) is
// cheap. A deep volume of glass or water is not, which is what RayQuery::maxTransparentLayers is for.
// If a scene ever needs many layers cheaply, THAT is the point to move the loop inward and accept the
// ordering work, not before.
//
// ---- Resuming without moving the ray ----
//
// Each pass asks for the nearest hit BEYOND a distance (RayQuery::tMin) rather than restarting the
// ray past the last one. That distinction is not stylistic; moving the origin is broken here.
//
// An origin nudged past a hit lands either inside the next voxel or exactly on a boundary, and this
// marcher is degenerate at both. Inside a voxel, the descent reaches it immediately and returns a
// real hit carrying rayT == 0 and a zero normal (SceneIntersectData::normal says so outright), which
// every renderer's miss test reads as a miss -- so a transparent surface showed the SKY behind it,
// not the geometry. Exactly on a boundary, the zero-measure guard in the DDA step skips the cell's
// validity check by design. Scaling the nudge by the voxel does not help: both failures are about
// where the origin lands, not how far it moved.
//
// Keeping the origin fixed sidesteps all of it. Every entry computation stays in the regime it was
// written for, rayT and the normal stay meaningful, and there is no epsilon to tune.
//
// ---- What is NOT here ----
//
// Refraction. `ior` and `transmission` are decoded but unused: a refracted ray CHANGES DIRECTION, so
// it cannot be a continuation of the same march -- it is a new ray, which belongs in a renderer's
// bounce loop rather than in a traversal. The peel reports the entry normal a refraction pass would
// need, so adding it later is additive. A transparent voxel here is a pure filter: it tints and
// attenuates what is behind it, and it does not reflect.

// Compile-time bound the peel loops unroll against on the HLSL/SPIR-V path.
// RayQuery::maxTransparentLayers is the runtime limit and must not exceed this.
#ifndef MAX_PEEL_ITERATIONS
#define MAX_PEEL_ITERATIONS 16
#endif

struct PeeledHit {
    // The nearest OPAQUE hit, with rayT measured from the caller's own ray origin (not from whatever
    // advanced origin found it). A miss carries rayT < 0 exactly as raySceneIntersect's does.
    SceneIntersectData hit;
    // The material at `hit`, already fetched. Callers must use this rather than calling
    // fetchVoxelMaterialFromHit again -- the peel had to fetch it to decide whether to stop, so
    // reusing it is what keeps this free on opaque geometry: the work moves rather than doubling.
    VoxelMaterial material;
    // Product of the transparent layers crossed on the way. Multiply the radiance arriving from
    // `hit` (or from the sky, on a miss) by this.
    vec3 transmittance;
    // Radiance emitted BY those transparent layers, each already attenuated by the layers in front of
    // it. Add it; do not scale it by `transmittance` again.
    vec3 emission;
    // Layers actually crossed.
    uint layers;
    // Why the peel stopped. Free to compute, and the only way to tell the failure modes apart from
    // the outside: giving up with transmittance near 1 reads on screen as "the voxels are not there",
    // giving up with it near 0 reads as "black", and the two are one line apart in here. Rendered as
    // false colour by PEEL_DEBUG in the scene editor's path tracer.
    //
    //   0 reached an opaque surface (the good case)   3 tMin failed to advance
    //   1 ran out of geometry (nothing behind)        4 fully absorbed
    //   2 layer budget spent                         5 iteration budget spent
    uint stopReason;
};

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
    return pow(tint, vec3(max(segmentLength / max(voxelSize, 1e-9), 0.0)));
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

// Nearest opaque hit, seeing through everything transparent in front of it.
PeeledHit raySceneIntersectPeeled(Ray ray, RayQuery rayQuery, uint maxLayers, inout uint seed) {
    PeeledHit result;
    result.transmittance = vec3(1.0);
    result.emission = vec3(0.0);
    result.layers = 0u;
    result.stopReason = 5u;
    result.material = emptyVoxelMaterial();

    // The ray never moves; only the floor on acceptable distances does. So every rayT below is
    // already in the caller's own parametrisation, with no bookkeeping to undo.
    float tMin = 0.0;
    // Identity of the voxel the previous pass consumed, so a pass that fails to move on is caught
    // exactly rather than being absorbed into the result. See the guard below.
    uint  lastHeader = 0xFFFFFFFFu;
    ivec3 lastVoxel = ivec3(0x7FFFFFFF);
    // Tracking for the interface-vs-interior split: a hit continues the previous body only if it is
    // the same material AND begins where the previous one ended.
    VoxelMaterial previous = emptyVoxelMaterial();
    bool  inMedium = false;
    float previousExit = 0.0;

    for (int iteration = 0; iteration < MAX_PEEL_ITERATIONS; iteration++) {
        SceneIntersectData hit = raySceneIntersectFrom(ray, rayQuery, tMin);

        // The same voxel again: tMin failed to advance past it (see peelNextTMin). Stop rather than
        // attenuate it a second time. Belt and braces behind the ULP floor -- cheap, and it turns any
        // residual non-advance into one lost layer instead of a black or invisible voxel.
        if (hit.headerIndex == lastHeader && all(equal(hit.voxelCoord, lastVoxel))) { result.stopReason = 3u; break; }

        if (hit.foundBox.size < 0.0 || hit.rayT < 0.0) {
            // Nothing solid left along the ray. Whatever the caller does on a miss (sky, usually) is
            // still filtered by the layers already crossed.
            result.stopReason = 1u;
            result.hit = hit;
            result.hit.rayT = -1.0;
            return result;
        }

        VoxelMaterial material = fetchVoxelMaterialFromHit(hit);

        // Genuinely opaque: this is the surface the caller asked for.
        if (material.transparency <= 0.0) {
            result.stopReason = 0u;
            result.hit = hit;
            result.material = material;
            return result;
        }

        // Out of layer budget. Give up as a MISS carrying what was accumulated -- never report this
        // voxel as the opaque hit. It is not opaque, and the face the caller would shade is INTERNAL
        // to a transparent volume: a face buried in glass, facing into more glass, which shades
        // almost black. Because the depth at which the budget runs out varies per ray, those dark
        // faces land on different voxel boundaries per pixel, which is what makes the inside of a
        // thick transparent object read as a lattice of dark squares.
        if (result.layers >= maxLayers) { result.stopReason = 2u; break; }

        // Interface or interior? Only an INTERFACE gets the transparency decision. Interior voxels of
        // one body are not separate panes of glass, and charging them each an alpha is what made depth
        // read as darkness.
        bool entering = !inMedium || !sameMedium(previous, material) ||
                        hit.rayT > previousExit + max(hit.foundBox.size, 1e-9) * 0.5;
        if (entering) {
            // Stochastic alpha. With probability (1 - transparency) the surface interacts and is
            // returned as an ordinary hit, so it is shaded by the full BSDF -- which is what gives a
            // water surface its specular reflection while still being see-through. Choosing the
            // interaction with exactly that probability makes both branches carry weight 1, so the
            // estimator needs no compensating factor and converges to the alpha-composited answer.
            if (peelRandom(seed) >= material.transparency) {
                result.stopReason = 0u;
                result.hit = hit;
                result.material = material;
                return result;
            }
        }

        // How much of this voxel the ray actually crossed. Drives both the Beer-Lambert exponent
        // and how far the next pass steps, so the two cannot disagree about the voxel's extent.
        float segmentLength = peelSegmentLength(ray, hit);

        // Emission first: a layer's own glow is dimmed by what is in FRONT of it, not by itself.
        result.emission += result.transmittance * material.emission;
        result.transmittance *= mediumAbsorption(material, segmentLength, hit.foundBox.size);
        result.layers++;
        lastHeader = hit.headerIndex;
        lastVoxel = hit.voxelCoord;
        previous = material;
        previousExit = hit.exitT;
        inMedium = true;

        // Fully absorbed: nothing behind this can contribute, so stop rather than spending the
        // remaining layers resolving a surface that will be multiplied by zero. A miss again, not
        // this voxel as opaque -- the near-zero transmittance already makes the result black without
        // shading an internal face to get there.
        if (max(result.transmittance.r, max(result.transmittance.g, result.transmittance.b)) < 0.002) break;

        tMin = peelNextTMin(hit, segmentLength);
    }

    // Ran out of iterations with transparent voxels still ahead. Reported as a miss carrying the
    // transmittance accumulated so far, which is the honest answer: we do not know what is behind.
    result.hit.foundBox.size = -1.0;
    result.hit.rayT = -1.0;
    result.hit.normal = vec3(0.0);
    result.hit.voxelCoord = ivec3(0);
    return result;
}

// How much light survives the trip from `ray.origin` to `maxDistance` along `ray`.
//
// The shadow-ray form, and the reason transparency is visible at all rather than merely stored: a
// visibility test can only answer "lit" or "not", so a glass pane casts a fully black shadow no
// matter what its material says. This returns the tint instead, which is what puts coloured light on
// the floor under stained glass.
//
// Cheaper than the peel above per layer -- it never resolves the occluder's material beyond its
// transparency, needs no ordering (multiplication commutes), and quits as soon as it is dark.
//
// Running out of budget returns the transmittance accumulated so far, NOT black. Black is the
// conservative answer when a ray might still be blocked, and it was the original choice here on the
// grounds that an over-dark shadow is less noticeable than light leaking through solid geometry.
// That reasoning does not survive a transparent VOLUME: every shading point inside one needs more
// layers to reach the light than the budget allows, so black turned the whole interior into a
// lattice of unlit faces. Between the two errors, slightly-too-much light through deep glass is far
// less visible than a black interior, and it degrades smoothly instead of per-voxel.
vec3 raySceneTransmittance(Ray ray, RayQuery rayQuery, float maxDistance, uint maxLayers) {
    vec3 transmittance = vec3(1.0);
    float tMin = 0.0;
    uint layers = 0u;
    uint  lastHeader = 0xFFFFFFFFu;
    ivec3 lastVoxel = ivec3(0x7FFFFFFF);
    VoxelMaterial previous = emptyVoxelMaterial();
    bool  inMedium = false;
    float previousExit = 0.0;

    for (int iteration = 0; iteration < MAX_PEEL_ITERATIONS; iteration++) {
        SceneIntersectData hit = raySceneIntersectFrom(ray, rayQuery, tMin);
        // Same non-advance guard as the peel above; without it a sliver crossing darkens one voxel
        // once per remaining layer and the shadow goes black.
        if (hit.headerIndex == lastHeader && all(equal(hit.voxelCoord, lastVoxel))) return transmittance;
        if (hit.foundBox.size < 0.0 || hit.rayT < 0.0) return transmittance;  // reached the light
        if (hit.rayT >= maxDistance) return transmittance;                    // occluder is past it

        VoxelMaterial material = fetchVoxelMaterialFromHit(hit);
        if (material.transparency <= 0.0) return vec3(0.0);   // genuinely opaque: blocked
        // Out of budget. Return what survived so far rather than black. Black is the conservative
        // answer for a ray that might still be blocked, and it is badly wrong here: a shading point
        // inside or behind a transparent volume needs more layers than the budget allows, so every
        // one of them would lose its direct sunlight and the volume's interior goes dark.
        if (layers >= maxLayers) return transmittance;

        float segmentLength = peelSegmentLength(ray, hit);
        // Interface alpha applied ANALYTICALLY here rather than stochastically: a shadow ray wants the
        // expected attenuation, and multiplying by `transparency` once per interface is exactly that,
        // with none of the variance a random choice would add to every shadow in the image.
        bool entering = !inMedium || !sameMedium(previous, material) ||
                        hit.rayT > previousExit + max(hit.foundBox.size, 1e-9) * 0.5;
        if (entering) transmittance *= material.transparency;
        transmittance *= mediumAbsorption(material, segmentLength, hit.foundBox.size);
        if (max(transmittance.r, max(transmittance.g, transmittance.b)) < 0.002) return vec3(0.0);
        layers++;
        lastHeader = hit.headerIndex;
        lastVoxel = hit.voxelCoord;
        previous = material;
        previousExit = hit.exitT;
        inMedium = true;

        tMin = peelNextTMin(hit, segmentLength);
    }

    // Iterations exhausted with transparent voxels still ahead: same call as the layer budget above.
    return transmittance;
}

